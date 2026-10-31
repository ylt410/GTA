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
};

struct SelectedKey {
	uint64_t key;
	uint32_t countThisFrame;
	bool used;
};

std::mutex gMutex;
std::atomic<int> gActive{0};
std::atomic<uint64_t> gSession{0};
std::atomic<uint32_t> gEpoch{0};
PGTDReport gReport{};
PacketEntry gPackets[PGTD_PACKET_SLOTS]{};
TokenEntry gTokens[PGTD_TOKEN_CAPACITY]{};
uint32_t gNextToken = 1;
uint64_t gNextThread = 1;
uint64_t gNextCookie = 1;
uint64_t gTagSequence = 0;
uint32_t gFrameClaims = 0;
SelectedKey gSelectedKeys[kEntityClassCount][2]{};
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
		return 0;
	}
	TokenEntry &entry = gTokens[firstReusable];
	entry = {raw, domain, gNextToken++, true, false};
	return entry.token;
}

void clearSidebandLocked() {
	std::memset(gPackets, 0, sizeof(gPackets));
}

void clearReportLocked(uint64_t session) {
	std::memset(&gReport, 0, sizeof(gReport));
	gReport.session = session;
	std::memset(gTokens, 0, sizeof(gTokens));
	gNextToken = 1;
	gNextThread = 1;
	gNextCookie = 1;
	gTagSequence = 0;
	gFrameClaims = 0;
	std::memset(gSelectedKeys, 0, sizeof(gSelectedKeys));
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
		tag.entityKey = tokenLocked(6u, thread.context.rawCandidateKey);
		if (tag.entityKey == 0) tag.flags &= ~PGTD_TAG_FLAG_KEY_VALID;
	}
	tag.entityToken = tokenLocked(1u, thread.context.entityIdentity);
	tag.auxiliaryToken = tokenLocked(2u, thread.context.auxiliaryIdentity);
	tag.rootMatrixToken = tokenLocked(4u, thread.context.rootMatrixIdentity);
	tag.paletteToken = tokenLocked(3u, thread.context.paletteIdentity);
	tag.scopeDepth = thread.depth;
	if (camera != nullptr) {
		tag.camera = *camera;
		tag.viewportToken = tokenLocked(5u, camera->rawViewport);
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
	if (!regular && !stageChanged && (!entityEligible ||
		!selectedKeyExtraLocked(thread.context, true))) return 0;
	if (gReport.stats.claimed >= PGTD_TAG_BUDGET) {
		++gReport.stats.overflow;
		return 0;
	}
	++gReport.stats.claimed;
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
	const PGTDTag tag = makeTagLocked(thread, camera);
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
	return tokenLocked(domain, raw);
}

void PGTDForgetToken(uint32_t domain, uint64_t raw) {
	if (raw == 0 || !gActive.load(std::memory_order_acquire)) return;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return;
	const uint32_t start = tokenSlot(domain, raw);
	for (uint32_t n = 0; n < PGTD_TOKEN_CAPACITY; ++n) {
		TokenEntry &entry = gTokens[(start + n) % PGTD_TOKEN_CAPACITY];
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
	if (gReport.recordCount >= PGTD_MAX_RECORDS) {
		++gReport.stats.recordOverflow;
		++gReport.stats.truncatedRecords;
		return;
	}
	PGTDMetalSample copy = *sample;
	copy.frame = gReport.frameCount == UINT32_MAX ? UINT32_MAX : gReport.frameCount + 1;
	gReport.samples[gReport.recordCount++] = copy;
}

uint32_t PGTDNextFrame(void) {
	if (!gActive.load(std::memory_order_acquire)) return 0;
	std::lock_guard<std::mutex> lock(gMutex);
	if (!gActive.load(std::memory_order_relaxed)) return 0;
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
