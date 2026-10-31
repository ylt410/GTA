#include "NativeTemporalDiagnostics.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace {

constexpr uint32_t kDrawOp = 32;
constexpr uint32_t kPresentOp = 7;
constexpr uint32_t kSetRenderTargetsOp = 26;
constexpr uint32_t kStageKindFirst = 100;
constexpr uint32_t kStageKindLast = 109;
constexpr uint32_t kEntityKindFirst = 1;
constexpr uint32_t kEntityKindLast = 8;
constexpr uint32_t kPaletteKind = 200;
constexpr uint32_t kEntityClassCount = 16;
constexpr uint64_t kHashSalt = UINT64_C(0x9e3779b97f4a7c15);
constexpr uint32_t kTransientWays = 4;
constexpr uint32_t kTransientSets = PGTD_TRANSIENT_TOKEN_CAPACITY / kTransientWays;
constexpr uint32_t kStageCount = kStageKindLast - kStageKindFirst + 1;
constexpr uint32_t kSelectedRecordKeys = 4;
constexpr uint32_t kSelectedRecordsPerKey = 2;
constexpr uint32_t kGeneralRecordsPerFrame = PGTD_RECORDS_PER_FRAME;
static_assert(PGTD_RECORDS_PER_FRAME >= 1u + 2u * kStageCount +
	kSelectedRecordKeys * kSelectedRecordsPerKey,
	"per-frame record budget must preserve PRESENT, stage markers, and selected draws");
static_assert(PGTD_RECORDS_PER_FRAME <= PGTD_MAX_RECORDS / 60u,
	"sixty complete frames must fit in the bounded report");

struct TokenEntry {
	uint64_t raw;
	uint32_t domain;
	uint32_t token;
	bool used;
	bool tombstone;
};

struct PacketEntry {
	uint64_t session;
	uint64_t address;
	uint32_t op;
	uint32_t words;
	PGTDTag tag;
	bool used;
};

struct ScopeContext {
	uint32_t stage;
	uint32_t entityClass;
	uint64_t rawCandidateKey;
	uint64_t entityIdentity;
	uint64_t auxiliaryIdentity;
	uint64_t rootMatrixIdentity;
	uint64_t paletteIdentity;
	uint32_t flags;
};

struct ScopeFrame {
	uint64_t cookie;
	ScopeContext previous;
};

struct ThreadState {
	uint64_t session;
	uint32_t id;
	uint32_t depth;
	uint32_t stageVersion;
	uint32_t lastClaimedStageVersion;
	uint32_t lastClaimedStage;
	uint32_t lastRTStageVersion;
	uint64_t drawOrdinal;
	ScopeContext context;
	ScopeFrame stack[PGTD_SCOPE_MAX_DEPTH];
	bool poisoned;
	uint32_t pendingTagFlags;
};

struct SelectedKey {
	uint64_t key;
	uint32_t countThisFrame;
	bool used;
};

struct FrameRecordStore {
	PGTDMetalSample present;
	PGTDMetalSample stageTargets[kStageCount];
	PGTDMetalSample stageDraws[kStageCount];
	PGTDMetalSample selected[kSelectedRecordKeys][kSelectedRecordsPerKey];
	PGTDMetalSample general[kGeneralRecordsPerFrame];
	uint32_t selectedCounts[kSelectedRecordKeys];
	uint32_t generalCount;
	uint64_t generalSeen;
	bool hasPresent;
	bool downsampled;
	bool hasStageTargets[kStageCount];
	bool hasStageDraws[kStageCount];
};

struct SelectedRecordKey {
	uint32_t token;
	bool used;
};

std::mutex gMutex;
std::atomic<int> gActive{0};
std::atomic<uint64_t> gSession{0};
std::atomic<uint32_t> gEpoch{0};
PGTDReport gReport{};
PacketEntry gPackets[PGTD_PACKET_SLOTS]{};
TokenEntry gTokens[PGTD_TOKEN_CAPACITY]{};
TokenEntry gCandidateTokens[PGTD_CANDIDATE_TOKEN_CAPACITY]{};
TokenEntry gTransientTokens[PGTD_TRANSIENT_TOKEN_CAPACITY]{};
uint32_t gTransientVictim[kTransientSets]{};
uint32_t gNextToken = 1;
uint64_t gNextThread = 1;
uint64_t gNextCookie = 1;
uint64_t gTagSequence = 0;
uint32_t gFrameClaims = 0;
SelectedKey gSelectedKeys[kEntityClassCount][2]{};
SelectedRecordKey gSelectedRecordKeys[kSelectedRecordKeys]{};
FrameRecordStore gFrameRecords{};
thread_local ThreadState tState{};

uint64_t mix64(uint64_t x) {
	x ^= x >> 30;
	x *= UINT64_C(0xbf58476d1ce4e5b9);
	x ^= x >> 27;
	x *= UINT64_C(0x94d049bb133111eb);
	x ^= x >> 31;
	return x;
}

uint32_t packetSlot(uint64_t session, uint64_t address) {
	return static_cast<uint32_t>(mix64(address ^ mix64(session + kHashSalt)) % PGTD_PACKET_SLOTS);
}

uint32_t tokenSlot(uint32_t domain, uint64_t raw) {
	return static_cast<uint32_t>(mix64(raw ^ (static_cast<uint64_t>(domain) * kHashSalt)) % PGTD_TOKEN_CAPACITY);
}

uint32_t tokenLocked(uint32_t domain, uint64_t raw) {
	if (raw == 0) return 0;
	const uint32_t start = tokenSlot(domain, raw);
	uint32_t firstReusable = PGTD_TOKEN_CAPACITY;
	for (uint32_t n = 0; n < PGTD_TOKEN_CAPACITY; ++n) {
		const uint32_t index = (start + n) % PGTD_TOKEN_CAPACITY;
		TokenEntry &entry = gTokens[index];
		if (entry.used) {
			if (entry.domain == domain && entry.raw == raw) return entry.token;
			continue;
		}
		if (firstReusable == PGTD_TOKEN_CAPACITY) firstReusable = index;
		if (!entry.tombstone) break;
	}
	if (firstReusable == PGTD_TOKEN_CAPACITY || gNextToken == 0) {
		++gReport.stats.tokenOverflow;
		++gReport.stats.resourceTokenOverflow;
		return 0;
	}
	TokenEntry &entry = gTokens[firstReusable];
	entry = {raw, domain, gNextToken++, true, false};
	return entry.token;
}

uint32_t candidateTokenLocked(uint32_t domain, uint64_t raw) {
	if (raw == 0) return 0;
	const uint32_t start = static_cast<uint32_t>(
		mix64(raw ^ (static_cast<uint64_t>(domain) * kHashSalt)) % PGTD_CANDIDATE_TOKEN_CAPACITY);
	uint32_t firstReusable = PGTD_CANDIDATE_TOKEN_CAPACITY;
	for (uint32_t n = 0; n < PGTD_CANDIDATE_TOKEN_CAPACITY; ++n) {
		const uint32_t index = (start + n) % PGTD_CANDIDATE_TOKEN_CAPACITY;
		TokenEntry &entry = gCandidateTokens[index];
		if (entry.used) {
			if (entry.domain == domain && entry.raw == raw) return entry.token;
			continue;
		}
		if (firstReusable == PGTD_CANDIDATE_TOKEN_CAPACITY) firstReusable = index;
		if (!entry.tombstone) break;
	}
	if (firstReusable == PGTD_CANDIDATE_TOKEN_CAPACITY || gNextToken == 0) {
		++gReport.stats.tokenOverflow;
		++gReport.stats.candidateTokenOverflow;
		return 0;
	}
	TokenEntry &entry = gCandidateTokens[firstReusable];
	entry = {raw, domain, gNextToken++, true, false};
	return entry.token;
}

uint32_t transientTokenLocked(uint32_t domain, uint64_t raw) {
	if (raw == 0) return 0;
	const uint32_t set = static_cast<uint32_t>(
		mix64(raw ^ (static_cast<uint64_t>(domain) * kHashSalt)) % kTransientSets);
	const uint32_t start = set * kTransientWays;
	uint32_t empty = PGTD_TRANSIENT_TOKEN_CAPACITY;
	for (uint32_t way = 0; way < kTransientWays; ++way) {
		const uint32_t index = start + way;
		TokenEntry &entry = gTransientTokens[index];
		if (entry.used && entry.domain == domain && entry.raw == raw) return entry.token;
		if (!entry.used && empty == PGTD_TRANSIENT_TOKEN_CAPACITY) empty = index;
	}
	if (gNextToken == 0) {
		++gReport.stats.tokenOverflow;
		++gReport.stats.transientTokenOverflow;
		return 0;
	}
	uint32_t index = empty;
	if (index == PGTD_TRANSIENT_TOKEN_CAPACITY) {
		index = start + gTransientVictim[set];
		gTransientVictim[set] = (gTransientVictim[set] + 1) % kTransientWays;
		++gReport.stats.transientTokenEvictions;
	}
	gTransientTokens[index] = {raw, domain, gNextToken++, true, false};
	return gTransientTokens[index].token;
}

void clearSidebandLocked() {
	std::memset(gPackets, 0, sizeof(gPackets));
}

void countRecordDropLocked() {
	++gReport.stats.quotaDroppedRecords;
	gFrameRecords.downsampled = true;
}

void appendReportRecordLocked(const PGTDMetalSample &sample) {
	if (gReport.recordCount >= PGTD_MAX_RECORDS) {
		++gReport.stats.recordOverflow;
		++gReport.stats.truncatedRecords;
		return;
	}
	gReport.samples[gReport.recordCount++] = sample;
}

void flushFrameRecordsLocked() {
	uint32_t fixedCount = gFrameRecords.hasPresent ? 1u : 0u;
	for (uint32_t i = 0; i < kStageCount; ++i) {
		fixedCount += gFrameRecords.hasStageTargets[i] ? 1u : 0u;
		fixedCount += gFrameRecords.hasStageDraws[i] ? 1u : 0u;
	}
	for (uint32_t i = 0; i < kSelectedRecordKeys; ++i)
		fixedCount += gFrameRecords.selectedCounts[i];
	const uint32_t generalLimit = fixedCount < PGTD_RECORDS_PER_FRAME
		? PGTD_RECORDS_PER_FRAME - fixedCount : 0u;
	const uint32_t generalToWrite = gFrameRecords.generalCount < generalLimit
		? gFrameRecords.generalCount : generalLimit;
	if (generalToWrite < gFrameRecords.generalCount) {
		gReport.stats.quotaDroppedRecords += gFrameRecords.generalCount - generalToWrite;
		gFrameRecords.downsampled = true;
	}
	// Selection priority must not change the producer-to-consumer chronology.
	// Gather a bounded set of pointers, sort it by sequence, then copy to report.
	const PGTDMetalSample *ordered[PGTD_RECORDS_PER_FRAME]{};
	uint32_t orderedCount = 0;
	if (gFrameRecords.hasPresent) ordered[orderedCount++] = &gFrameRecords.present;
	for (uint32_t i = 0; i < kStageCount; ++i) {
		if (gFrameRecords.hasStageTargets[i]) ordered[orderedCount++] = &gFrameRecords.stageTargets[i];
	}
	for (uint32_t i = 0; i < kStageCount; ++i) {
		if (gFrameRecords.hasStageDraws[i]) ordered[orderedCount++] = &gFrameRecords.stageDraws[i];
	}
	for (uint32_t i = 0; i < kSelectedRecordKeys; ++i) {
		for (uint32_t j = 0; j < gFrameRecords.selectedCounts[i]; ++j) {
			ordered[orderedCount++] = &gFrameRecords.selected[i][j];
		}
	}
	for (uint32_t i = 0; i < generalToWrite; ++i) {
		ordered[orderedCount++] = &gFrameRecords.general[i];
	}
	for (uint32_t i = 1; i < orderedCount; ++i) {
		const PGTDMetalSample *sample = ordered[i];
		uint32_t j = i;
		while (j > 0 && ordered[j - 1]->tag.sequence > sample->tag.sequence) {
			ordered[j] = ordered[j - 1];
			--j;
		}
		ordered[j] = sample;
	}
	for (uint32_t i = 0; i < orderedCount; ++i) appendReportRecordLocked(*ordered[i]);
	if (gFrameRecords.downsampled) ++gReport.stats.downsampledFrames;
	std::memset(&gFrameRecords, 0, sizeof(gFrameRecords));
}

int32_t selectedRecordKeyLocked(uint32_t token) {
	if (token == 0) return -1;
	for (uint32_t i = 0; i < kSelectedRecordKeys; ++i) {
		if (gSelectedRecordKeys[i].used && gSelectedRecordKeys[i].token == token)
			return static_cast<int32_t>(i);
	}
	for (uint32_t i = 0; i < kSelectedRecordKeys; ++i) {
		if (!gSelectedRecordKeys[i].used) {
			gSelectedRecordKeys[i] = {token, true};
			return static_cast<int32_t>(i);
		}
	}
	return -1;
}

void stageRecordLocked(const PGTDMetalSample &sample) {
	FrameRecordStore &frame = gFrameRecords;
	if (sample.opcode == kPresentOp) {
		if (frame.hasPresent) countRecordDropLocked();
		else {
			frame.present = sample;
			frame.hasPresent = true;
		}
		return;
	}
	if (sample.tag.stage >= kStageKindFirst && sample.tag.stage <= kStageKindLast) {
		const uint32_t stage = sample.tag.stage - kStageKindFirst;
		if (sample.opcode == kSetRenderTargetsOp) {
			if (frame.hasStageTargets[stage]) countRecordDropLocked();
			else {
				frame.stageTargets[stage] = sample;
				frame.hasStageTargets[stage] = true;
			}
			return;
		}
	}
	if (sample.opcode == kDrawOp &&
		(sample.tag.flags & PGTD_TAG_FLAG_SELECTED_SAMPLE) != 0) {
		const int32_t keyIndex = selectedRecordKeyLocked(sample.tag.entityKey);
		if (keyIndex >= 0 && frame.selectedCounts[keyIndex] < kSelectedRecordsPerKey) {
			frame.selected[keyIndex][frame.selectedCounts[keyIndex]++] = sample;
			return;
		}
		countRecordDropLocked();
		return;
	}
	if (sample.opcode == kDrawOp &&
		sample.tag.stage >= kStageKindFirst && sample.tag.stage <= kStageKindLast) {
		const uint32_t stage = sample.tag.stage - kStageKindFirst;
		if (frame.hasStageDraws[stage]) countRecordDropLocked();
		else {
			frame.stageDraws[stage] = sample;
			frame.hasStageDraws[stage] = true;
		}
		return;
	}
	if (frame.generalSeen != UINT64_MAX) ++frame.generalSeen;
	if (frame.generalCount < kGeneralRecordsPerFrame) {
		frame.general[frame.generalCount++] = sample;
		return;
	}
	const uint64_t pick = mix64(sample.tag.sequence ^ gReport.session ^
		(static_cast<uint64_t>(sample.frame) * kHashSalt)) % frame.generalSeen;
	if (pick < kGeneralRecordsPerFrame) {
		frame.general[static_cast<uint32_t>(pick)] = sample;
	}
	countRecordDropLocked();
}

void clearReportLocked(uint64_t session) {
	std::memset(&gReport, 0, sizeof(gReport));
	gReport.session = session;
	std::memset(gTokens, 0, sizeof(gTokens));
	std::memset(gCandidateTokens, 0, sizeof(gCandidateTokens));
	std::memset(gTransientTokens, 0, sizeof(gTransientTokens));
	std::memset(gTransientVictim, 0, sizeof(gTransientVictim));
	gNextThread = 1;
	gNextCookie = 1;
	gTagSequence = 0;
	gFrameClaims = 0;
	std::memset(gSelectedKeys, 0, sizeof(gSelectedKeys));
	std::memset(gSelectedRecordKeys, 0, sizeof(gSelectedRecordKeys));
	std::memset(&gFrameRecords, 0, sizeof(gFrameRecords));
	clearSidebandLocked();
}

void resetThreadLocked() {
	std::memset(&tState, 0, sizeof(tState));
	tState.session = gReport.session;
	if (gNextThread > UINT32_MAX) {
		++gReport.stats.threadOverflow;
		tState.id = 0;
	} else {
		tState.id = static_cast<uint32_t>(gNextThread++);
	}
}

ThreadState &threadStateLocked() {
	const uint64_t session = gReport.session;
	if (tState.session != session) resetThreadLocked();
	return tState;
}

bool finiteCamera(const PGTDCamera &camera) {
	if (!std::isfinite(camera.nearPlane) || !std::isfinite(camera.farPlane)) return false;
	for (float v : camera.view) if (!std::isfinite(v)) return false;
	for (float v : camera.projection) if (!std::isfinite(v)) return false;
	return true;
}

uint32_t stageValue(uint32_t kind) {
	return kind >= kStageKindFirst && kind <= kStageKindLast ? kind : 0;
}

PGTDTag makeTagLocked(ThreadState &thread, const PGTDCamera *camera) {
	PGTDTag tag{};
	tag.session = gReport.session;
	tag.sequence = ++gTagSequence;
	tag.producerThread = thread.id;
	tag.stage = thread.context.stage;
	tag.entityClass = thread.context.entityClass;
	tag.flags = thread.context.flags;
	tag.entityKey = 0;
	if ((thread.context.flags & PGTD_TAG_FLAG_KEY_VALID) != 0) {
		tag.entityKey = candidateTokenLocked(6u, thread.context.rawCandidateKey);
		if (tag.entityKey == 0) tag.flags &= ~PGTD_TAG_FLAG_KEY_VALID;
	}
	tag.entityToken = transientTokenLocked(1u, thread.context.entityIdentity);
	tag.auxiliaryToken = transientTokenLocked(2u, thread.context.auxiliaryIdentity);
	tag.rootMatrixToken = transientTokenLocked(4u, thread.context.rootMatrixIdentity);
	tag.paletteToken = transientTokenLocked(3u, thread.context.paletteIdentity);
	tag.scopeDepth = thread.depth;
	if (camera != nullptr) {
		tag.camera = *camera;
		tag.viewportToken = transientTokenLocked(5u, camera->rawViewport);
		tag.camera.rawViewport = 0;
		if (!camera->valid || !finiteCamera(*camera)) {
			tag.flags |= PGTD_TAG_FLAG_CAMERA_INVALID;
			++gReport.stats.invalidCamera;
			tag.camera.valid = 0;
			if (!finiteCamera(*camera)) {
				std::memset(tag.camera.view, 0, sizeof(tag.camera.view));
				std::memset(tag.camera.projection, 0, sizeof(tag.camera.projection));
				tag.camera.nearPlane = 0;
				tag.camera.farPlane = 0;
			}
		}
	} else {
		tag.flags |= PGTD_TAG_FLAG_CAMERA_INVALID;
		++gReport.stats.invalidCamera;
	}
	return tag;
}

bool selectedKeyExtraLocked(const ScopeContext &context, bool consume) {
	if ((context.flags & PGTD_TAG_FLAG_KEY_VALID) == 0 || context.entityClass == 0 ||
		context.entityClass >= kEntityClassCount) return false;
	SelectedKey *keys = gSelectedKeys[context.entityClass];
	SelectedKey *chosen = nullptr;
	for (uint32_t i = 0; i < 2; ++i) {
		if (keys[i].used && keys[i].key == context.rawCandidateKey) chosen = &keys[i];
	}
	if (chosen == nullptr) {
		for (uint32_t i = 0; i < 2; ++i) {
			if (!keys[i].used) {
				keys[i] = {context.rawCandidateKey, 0, true};
				chosen = &keys[i];
				break;
			}
		}
	}
	if (chosen == nullptr || chosen->countThisFrame >= 2 || gFrameClaims >= 24) return false;
	if (consume) {
		++chosen->countThisFrame;
		++gFrameClaims;
	}
	return true;
}

uint64_t makeCookieLocked() {
	uint64_t low = gNextCookie++;
	if (low == 0 || low > UINT32_MAX) {
		gNextCookie = 2;
		low = 1;
	}
	const uint64_t epoch = static_cast<uint64_t>(gEpoch.load(std::memory_order_relaxed));
	return (epoch << 32) | low;
}

} // namespace

extern "C" {

int PGTDEnabled(void) {
	return gActive.load(std::memory_order_acquire);
}

uint64_t PGTDBegin(void) {
	std::lock_guard<std::mutex> lock(gMutex);
	uint64_t session = gSession.load(std::memory_order_relaxed) + 1;
	if (session == 0) session = 1;
	gSession.store(session, std::memory_order_release);
	uint32_t epoch = gEpoch.load(std::memory_order_relaxed) + 1;
	if (epoch == 0) epoch = 1;
	gEpoch.store(epoch, std::memory_order_release);
	clearReportLocked(session);
	gActive.store(1, std::memory_order_release);
	return session;
}

void PGTDEnd(void) {
	std::lock_guard<std::mutex> lock(gMutex);
	flushFrameRecordsLocked();
	gActive.store(0, std::memory_order_release);
	clearSidebandLocked();
}

uint64_t PGTDScopeEnter(uint32_t kind, uint64_t identity, uint64_t auxiliary,
		uint64_t candidateKey, uint32_t flags) {
	if (!gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
	ThreadState &thread = threadStateLocked();
	if (thread.poisoned || thread.depth >= PGTD_SCOPE_MAX_DEPTH) {
		thread.poisoned = true;
		thread.depth = 0;
		std::memset(&thread.context, 0, sizeof(thread.context));
		++gReport.stats.scopeErrors;
		return 0;
	}
	ScopeFrame &frame = thread.stack[thread.depth];
	frame.cookie = makeCookieLocked();
	frame.previous = thread.context;
	++thread.depth;
	const uint32_t previousStage = thread.context.stage;
	if (kind >= kStageKindFirst && kind <= kStageKindLast) {
		thread.context.stage = stageValue(kind);
	} else if (kind >= kEntityKindFirst && kind <= kEntityKindLast) {
		thread.context.entityClass = kind;
		thread.context.entityIdentity = identity;
		thread.context.auxiliaryIdentity = auxiliary;
		thread.context.rawCandidateKey = candidateKey;
		thread.context.rootMatrixIdentity = 0;
		thread.context.paletteIdentity = 0;
		thread.context.flags &= ~(PGTD_TAG_FLAG_KEY_VALID | PGTD_TAG_FLAG_ENTITY_UNKNOWN);
		if (kind == 7 || kind == 8) {
			thread.context.rawCandidateKey = 0;
			thread.context.flags |= PGTD_TAG_FLAG_ENTITY_UNKNOWN;
		} else if ((flags & PGTD_SCOPE_FLAG_KEY_VALID) != 0) {
			thread.context.flags |= PGTD_TAG_FLAG_KEY_VALID;
		}
	} else if (kind == kPaletteKind) {
		thread.context.rootMatrixIdentity = identity;
		thread.context.paletteIdentity = auxiliary;
	}
	if (thread.context.stage != previousStage) ++thread.stageVersion;
	return frame.cookie;
}

void PGTDScopeLeave(uint64_t cookie) {
	if (!gActive.load(std::memory_order_acquire) || cookie == 0) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	ThreadState &thread = threadStateLocked();
	const uint32_t cookieEpoch = static_cast<uint32_t>(cookie >> 32);
	if (cookieEpoch != gEpoch.load(std::memory_order_relaxed)) return;
	if (thread.poisoned || thread.depth == 0 || thread.stack[thread.depth - 1].cookie != cookie) {
		thread.poisoned = true;
		thread.depth = 0;
		std::memset(&thread.context, 0, sizeof(thread.context));
		++gReport.stats.scopeErrors;
		return;
	}
	const uint32_t oldStage = thread.context.stage;
	thread.context = thread.stack[thread.depth - 1].previous;
	thread.stack[thread.depth - 1] = {};
	--thread.depth;
	if (thread.context.stage != oldStage) ++thread.stageVersion;
}

void PGTDInvalidateThread(void) {
	if (!gActive.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	ThreadState &thread = threadStateLocked();
	thread.poisoned = true;
	thread.depth = 0;
	std::memset(&thread.context, 0, sizeof(thread.context));
	std::memset(thread.stack, 0, sizeof(thread.stack));
	++thread.stageVersion;
	++gReport.stats.scopeErrors;
}

int PGTDClaimPacket(uint32_t op) {
	if (!gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
	ThreadState &thread = threadStateLocked();
	thread.pendingTagFlags = 0;
	if (op == kPresentOp) {
		for (auto &klass : gSelectedKeys) for (auto &key : klass) key.countThisFrame = 0;
		gFrameClaims = 0;
		++gReport.stats.claimed;
		if (gReport.stats.claimed > PGTD_TAG_BUDGET) ++gReport.stats.overflow;
		return 1;
	}
	if (op == kSetRenderTargetsOp) {
		if (thread.context.stage < kStageKindFirst ||
			thread.context.stage > kStageKindLast ||
			thread.lastRTStageVersion == thread.stageVersion) return 0;
		thread.lastRTStageVersion = thread.stageVersion;
		if (gReport.stats.claimed >= PGTD_TAG_BUDGET) {
			++gReport.stats.overflow;
			return 0;
		}
		++gReport.stats.claimed;
		return 1;
	}
	if (op != kDrawOp) return 0;
	++thread.drawOrdinal;
	const bool regular = ((thread.drawOrdinal - 1) % 128) == 0;
	const bool stageChanged = thread.lastClaimedStageVersion != thread.stageVersion ||
		thread.lastClaimedStage != thread.context.stage;
	const bool entityEligible = selectedKeyExtraLocked(thread.context, false);
	bool selectedExtra = false;
	if (!regular && !stageChanged) {
		if (!entityEligible || !selectedKeyExtraLocked(thread.context, true)) return 0;
		selectedExtra = true;
	}
	if (gReport.stats.claimed >= PGTD_TAG_BUDGET) {
		++gReport.stats.overflow;
		return 0;
	}
	++gReport.stats.claimed;
	if (selectedExtra) thread.pendingTagFlags |= PGTD_TAG_FLAG_SELECTED_SAMPLE;
	thread.lastClaimedStageVersion = thread.stageVersion;
	thread.lastClaimedStage = thread.context.stage;
	return 1;
}

void PGTDProducerPacket(uint64_t packetAddress, uint32_t op, uint32_t words,
		const PGTDCamera *camera) {
	if (!gActive.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	if (op != kDrawOp && op != kPresentOp && op != kSetRenderTargetsOp) return;
	ThreadState &thread = threadStateLocked();
	const uint32_t slotIndex = packetSlot(gReport.session, packetAddress);
	PacketEntry &slot = gPackets[slotIndex];
	if (slot.used) ++gReport.stats.collisions;
	PGTDTag tag = makeTagLocked(thread, camera);
	if (op == kDrawOp) tag.flags |= thread.pendingTagFlags;
	thread.pendingTagFlags = 0;
	slot.session = gReport.session;
	slot.address = packetAddress;
	slot.op = op;
	slot.words = words;
	slot.tag = tag;
	slot.used = true;
	++gReport.stats.published;
}

int PGTDConsumePacket(uint64_t packetAddress, uint32_t op, uint32_t words, PGTDTag *out) {
	if (out != nullptr) std::memset(out, 0, sizeof(*out));
	if (out == nullptr || !gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
	const uint32_t slotIndex = packetSlot(gReport.session, packetAddress);
	PacketEntry &slot = gPackets[slotIndex];
	if (!slot.used || slot.session != gReport.session || slot.address != packetAddress) {
		++gReport.stats.misses;
		return 0;
	}
	if (slot.op != op || slot.words != words || slot.tag.session != gReport.session) {
		std::memset(&slot, 0, sizeof(slot));
		++gReport.stats.misses;
		return 0;
	}
	*out = slot.tag;
	std::memset(&slot, 0, sizeof(slot));
	++gReport.stats.matched;
	return 1;
}

uint32_t PGTDToken(uint32_t domain, uint64_t raw) {
	if (raw == 0 || !gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
	return domain == 6u ? candidateTokenLocked(domain, raw) : tokenLocked(domain, raw);
}

void PGTDForgetToken(uint32_t domain, uint64_t raw) {
	if (raw == 0 || !gActive.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	const bool candidate = domain == 6u;
	const uint32_t capacity = candidate ? PGTD_CANDIDATE_TOKEN_CAPACITY : PGTD_TOKEN_CAPACITY;
	const uint32_t start = candidate
		? static_cast<uint32_t>(mix64(raw ^ (static_cast<uint64_t>(domain) * kHashSalt)) % capacity)
		: tokenSlot(domain, raw);
	TokenEntry *table = candidate ? gCandidateTokens : gTokens;
	for (uint32_t n = 0; n < capacity; ++n) {
		TokenEntry &entry = table[(start + n) % capacity];
		if (entry.used && entry.domain == domain && entry.raw == raw) {
			entry.used = false;
			entry.tombstone = true;
			return;
		}
		if (!entry.used && !entry.tombstone) return;
	}
}

void PGTDRecordMetal(const PGTDMetalSample *sample) {
	if (sample == nullptr || !gActive.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	if (sample->tag.session != gReport.session) {
		++gReport.stats.invalidRecord;
		return;
	}
	PGTDMetalSample copy = *sample;
	copy.frame = gReport.frameCount == UINT32_MAX ? UINT32_MAX : gReport.frameCount + 1;
	stageRecordLocked(copy);
}

uint32_t PGTDNextFrame(void) {
	if (!gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
	flushFrameRecordsLocked();
	if (gReport.frameCount != UINT32_MAX) ++gReport.frameCount;
	return gReport.frameCount;
}

int PGTDCopyReport(PGTDReport *out) {
	if (out == nullptr) return 0;
	if (gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (gActive.load(std::memory_order_relaxed) || gReport.session == 0) return 0;
	*out = gReport;
	return 1;
}

} // extern "C"
