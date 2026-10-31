#include "../Source/NativeTemporalDiagnostics.h"

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <thread>

static PGTDCamera cameraFor(float marker, uint64_t rawViewport = 0x1234) {
	PGTDCamera camera{};
	camera.rawViewport = rawViewport;
	camera.valid = 1;
	camera.width = 1600;
	camera.height = 900;
	for (int i = 0; i < 16; ++i) {
		camera.view[i] = marker + static_cast<float>(i);
		camera.projection[i] = marker + 100.0f + static_cast<float>(i);
	}
	camera.nearPlane = 0.1f;
	camera.farPlane = 1000.0f;
	return camera;
}

static PGTDTag publishConsume(uint64_t address, uint32_t op = 32, uint32_t words = 12,
		const PGTDCamera *camera = nullptr) {
	PGTDProducerPacket(address, op, words, camera);
	PGTDTag tag{};
	assert(PGTDConsumePacket(address, op, words, &tag));
	return tag;
}

static uint64_t testMix64(uint64_t x) {
	x ^= x >> 30;
	x *= UINT64_C(0xbf58476d1ce4e5b9);
	x ^= x >> 27;
	x *= UINT64_C(0x94d049bb133111eb);
	x ^= x >> 31;
	return x;
}

static uint32_t testTokenSlot(uint32_t domain, uint64_t raw) {
	return static_cast<uint32_t>(testMix64(raw ^
		(static_cast<uint64_t>(domain) * UINT64_C(0x9e3779b97f4a7c15))) % PGTD_TOKEN_CAPACITY);
}

static void testDisabledAndSessionIsolation() {
	assert(!PGTDEnabled());
	assert(PGTDBegin() != 0);
	assert(PGTDEnabled());
	assert(PGTDToken(9, 0xabcdef) != 0);
	const uint64_t oldCookie = PGTDScopeEnter(100, 0, 0, 0, 0);
	assert(oldCookie != 0);
	const auto cam = cameraFor(1);
	PGTDProducerPacket(0xfeed, 32, 4, &cam);
	PGTDEnd();
	assert(!PGTDEnabled());
	assert(PGTDToken(9, 0xabcdef) == 0);
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.session != 0 && report.stats.published == 1);
	const uint64_t newSession = PGTDBegin();
	assert(newSession != report.session);
	PGTDScopeLeave(oldCookie); // Epoch mismatch must not pop or poison new state.
	assert(publishConsume(0xfeed, 32, 4, &cam).stage == 0);
	PGTDEnd();
	assert(PGTDCopyReport(&report));
	assert(report.stats.published == 1 && report.stats.matched == 1);
}

static void testNestedScopesAndFailClosed() {
	PGTDBegin();
	constexpr uint64_t rawCandidate = UINT64_C(0x112233440000002a);
	const uint32_t rootMatrixToken = PGTDToken(4, 0x3333);
	const uint32_t paletteCandidateToken = PGTDToken(3, 0x4444);
	const uint64_t stage = PGTDScopeEnter(100, 0, 0, 0, 0);
	const uint64_t entity = PGTDScopeEnter(1, 0x1111, 0x2222, rawCandidate, PGTD_SCOPE_FLAG_KEY_VALID);
	const uint64_t palette = PGTDScopeEnter(200, 0x3333, 0x4444, 0, 0);
	auto tag = publishConsume(101, 32, 5);
	assert(tag.stage == 100 && tag.entityClass == 1 && tag.entityKey != 0);
	assert(tag.entityKey != static_cast<uint32_t>(rawCandidate));
	const uint32_t outerCandidateToken = tag.entityKey;
	assert((tag.flags & PGTD_TAG_FLAG_KEY_VALID) != 0);
	assert(tag.entityToken != 0 && tag.auxiliaryToken != 0 && tag.paletteToken != 0);
	assert(tag.rootMatrixToken == rootMatrixToken && tag.paletteToken == paletteCandidateToken);
	assert(tag.rootMatrixToken != tag.paletteToken);
	assert(tag.rootMatrixToken != 0x3333 && tag.paletteToken != 0x4444);
	assert(tag.auxiliaryToken == PGTDToken(2, 0x2222)); // Palette scope leaves entity context intact.
	const uint32_t outerRootToken = tag.rootMatrixToken;
	const uint32_t outerPaletteToken = tag.paletteToken;

	const uint64_t nestedStage = PGTDScopeEnter(101, 0, 0, 0, 0);
	const uint64_t unknown = PGTDScopeEnter(7, 0x7777, 0, 999, PGTD_SCOPE_FLAG_KEY_VALID);
	tag = publishConsume(102, 32, 5);
	assert(tag.stage == 101 && tag.entityClass == 7 && tag.entityKey == 0);
	assert(tag.rootMatrixToken == 0 && tag.paletteToken == 0); // Nested batch must not inherit outer matrices.
	assert((tag.flags & PGTD_TAG_FLAG_ENTITY_UNKNOWN) != 0);
	assert((tag.flags & PGTD_TAG_FLAG_KEY_VALID) == 0);
	PGTDScopeLeave(unknown);
	tag = publishConsume(103, 32, 5);
	assert(tag.stage == 101 && tag.entityClass == 1 && tag.entityKey == outerCandidateToken);
	assert(tag.rootMatrixToken == outerRootToken && tag.paletteToken == outerPaletteToken);
	PGTDScopeLeave(nestedStage);
	tag = publishConsume(104, 32, 5);
	assert(tag.stage == 100 && tag.paletteToken != 0);
	PGTDScopeLeave(palette);
	tag = publishConsume(105, 32, 5);
	assert(tag.stage == 100 && tag.rootMatrixToken == 0 && tag.paletteToken == 0);
	PGTDScopeLeave(entity);
	tag = publishConsume(106, 32, 5);
	assert(tag.stage == 100 && tag.entityClass == 0 && tag.entityKey == 0);
	PGTDScopeLeave(stage);
	assert(publishConsume(107).scopeDepth == 0);

	uint64_t scopes[PGTD_SCOPE_MAX_DEPTH]{};
	for (uint32_t i = 0; i < PGTD_SCOPE_MAX_DEPTH; ++i) {
		scopes[i] = PGTDScopeEnter(1, i + 1, 0, 700 + i, PGTD_SCOPE_FLAG_KEY_VALID);
		assert(scopes[i] != 0);
	}
	assert(PGTDScopeEnter(1, 0x999, 0, 999, PGTD_SCOPE_FLAG_KEY_VALID) == 0);
	tag = publishConsume(108);
	assert(tag.entityKey == 0 && tag.entityClass == 0 && tag.scopeDepth == 0);
	PGTDScopeLeave(scopes[PGTD_SCOPE_MAX_DEPTH - 1]);
	tag = publishConsume(109);
	assert(tag.entityKey == 0 && tag.entityClass == 0);
	PGTDEnd();
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.stats.scopeErrors >= 1);
}

static void testWideCandidateIdentityAndSampling() {
	constexpr uint64_t candidateA = UINT64_C(0x112233440000002a);
	constexpr uint64_t candidateB = UINT64_C(0x556677880000002a);
	PGTDBegin();
	uint64_t scope = PGTDScopeEnter(1, 0xabc, 0, candidateA, PGTD_SCOPE_FLAG_KEY_VALID);
	auto tagA = publishConsume(0x201, 32, 6);
	const uint32_t tokenA = tagA.entityKey;
	PGTDMetalSample sample{};
	sample.tag = tagA;
	PGTDRecordMetal(&sample);
	PGTDScopeLeave(scope);
	scope = PGTDScopeEnter(1, 0xabc, 0, candidateB, PGTD_SCOPE_FLAG_KEY_VALID);
	auto tagB = publishConsume(0x202, 32, 6);
	const uint32_t tokenB = tagB.entityKey;
	sample.tag = tagB;
	PGTDRecordMetal(&sample);
	PGTDScopeLeave(scope);
	assert(tokenA != 0 && tokenB != 0 && tokenA != tokenB);
	assert(tokenA != static_cast<uint32_t>(candidateA));
	assert(tokenB != static_cast<uint32_t>(candidateB));
	PGTDEnd();
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.recordCount == 2);
	assert(report.samples[0].tag.entityKey == tokenA && report.samples[1].tag.entityKey == tokenB);
	assert(report.samples[0].tag.entityKey != static_cast<uint32_t>(candidateA));
	assert(report.samples[1].tag.entityKey != static_cast<uint32_t>(candidateB));

	PGTDBegin();
	uint32_t claims = 0;
	for (uint64_t candidate : {candidateA, candidateB}) {
		scope = PGTDScopeEnter(1, 0xdef, 0, candidate, PGTD_SCOPE_FLAG_KEY_VALID);
		for (uint32_t i = 0; i < 512; ++i) claims += PGTDClaimPacket(32) != 0;
		PGTDScopeLeave(scope);
	}
	assert(claims == 8 + 4); // Full-width keys each retain two extra samples.
	PGTDEnd();
}

static void testThreadInvalidation() {
	PGTDBegin();
	const uint64_t stage = PGTDScopeEnter(100, 0, 0, 0, 0);
	const uint64_t entity = PGTDScopeEnter(1, 0x777, 0x888, 0x1234567800000042ull,
		PGTD_SCOPE_FLAG_KEY_VALID);
	const auto camera = cameraFor(50);
	PGTDProducerPacket(0x301, 32, 7, &camera);
	PGTDTag tag{};
	assert(PGTDConsumePacket(0x301, 32, 7, &tag));
	assert(tag.stage == 100 && tag.entityClass == 1 && tag.entityKey != 0);

	PGTDInvalidateThread(); // Models a longjmp/trap bypassing both scope leaves.
	PGTDScopeLeave(entity); // A stale leave cannot restore a saved scope.
	PGTDProducerPacket(0x302, 32, 7, &camera);
	assert(PGTDConsumePacket(0x302, 32, 7, &tag));
	assert(tag.stage == 0 && tag.entityClass == 0 && tag.entityKey == 0 && tag.scopeDepth == 0);
	assert(PGTDScopeEnter(1, 0x999, 0, 7, PGTD_SCOPE_FLAG_KEY_VALID) == 0);
	PGTDScopeLeave(stage);
	PGTDProducerPacket(0x303, 32, 7, &camera);
	assert(PGTDConsumePacket(0x303, 32, 7, &tag));
	assert(tag.stage == 0 && tag.entityClass == 0 && tag.entityKey == 0);
	PGTDEnd();
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.stats.scopeErrors >= 1);

	PGTDBegin();
	const uint64_t freshStage = PGTDScopeEnter(100, 0, 0, 0, 0);
	const uint64_t freshEntity = PGTDScopeEnter(1, 0xaaa, 0, 0x1234567800000042ull,
		PGTD_SCOPE_FLAG_KEY_VALID);
	PGTDProducerPacket(0x304, 32, 7, &camera);
	assert(PGTDConsumePacket(0x304, 32, 7, &tag));
	assert(tag.stage == 100 && tag.entityClass == 1 && tag.entityKey != 0);
	PGTDScopeLeave(freshEntity);
	PGTDScopeLeave(freshStage);
	PGTDEnd();
}

static void testWrongHeaderCollisionAndAnonymization() {
	PGTDBegin();
	const auto cam = cameraFor(10);
	PGTDProducerPacket(0xabc, 32, 8, &cam);
	PGTDTag tag{};
	assert(!PGTDConsumePacket(0xabc, 32, 9, &tag));
	assert(tag.session == 0);
	assert(!PGTDConsumePacket(0xabc, 32, 8, &tag)); // Mismatch removed stale sideband.

	for (uint64_t i = 1; i <= PGTD_PACKET_SLOTS; ++i) {
		auto item = cameraFor(static_cast<float>(i), 0x777);
		PGTDProducerPacket(i, 32, 16, &item);
	}
	auto last = cameraFor(static_cast<float>(PGTD_PACKET_SLOTS + 1), 0x777);
	PGTDProducerPacket(PGTD_PACKET_SLOTS + 1, 32, 16, &last);
	assert(PGTDConsumePacket(PGTD_PACKET_SLOTS + 1, 32, 16, &tag));
	assert(tag.camera.view[0] == static_cast<float>(PGTD_PACKET_SLOTS + 1));
	assert(tag.camera.rawViewport == 0 && tag.viewportToken != 0);
	uint32_t successful = 0;
	for (uint64_t i = 1; i <= PGTD_PACKET_SLOTS; ++i) {
		PGTDTag candidate{};
		if (PGTDConsumePacket(i, 32, 16, &candidate)) {
			++successful;
			assert(candidate.camera.view[0] == static_cast<float>(i));
			assert(candidate.camera.rawViewport == 0);
		}
	}
	assert(successful < PGTD_PACKET_SLOTS); // At least one direct-hash overwrite.
	const uint32_t first = PGTDToken(44, 0x123456789ull);
	const uint32_t same = PGTDToken(44, 0x123456789ull);
	const uint32_t otherDomain = PGTDToken(45, 0x123456789ull);
	assert(first != 0 && first == same && first != otherDomain);
	PGTDForgetToken(44, 0x123456789ull);
	const uint32_t nextLifetime = PGTDToken(44, 0x123456789ull);
	assert(nextLifetime != 0 && nextLifetime != first);
	PGTDEnd();
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.stats.collisions >= 1 && report.stats.misses >= 2);
}

static void testTokenTombstoneProbeChain() {
	constexpr uint32_t domain = 77;
	uint64_t raw[3]{};
	uint32_t commonSlot = UINT32_MAX;
	uint32_t found = 0;
	for (uint64_t candidate = 1; found < 3; ++candidate) {
		const uint32_t slot = testTokenSlot(domain, candidate);
		if (commonSlot == UINT32_MAX) commonSlot = slot;
		if (slot == commonSlot) raw[found++] = candidate;
	}
	PGTDBegin();
	const uint32_t first = PGTDToken(domain, raw[0]);
	const uint32_t middle = PGTDToken(domain, raw[1]);
	const uint32_t last = PGTDToken(domain, raw[2]);
	assert(first != 0 && middle != 0 && last != 0);
	PGTDForgetToken(domain, raw[1]);
	assert(PGTDToken(domain, raw[2]) == last); // Lookup crosses tombstone.
	assert(PGTDToken(domain, raw[0]) == first);
	PGTDForgetToken(domain, raw[0]);
	assert(PGTDToken(domain, raw[2]) == last);
	const uint32_t newMiddle = PGTDToken(domain, raw[1]);
	assert(newMiddle != 0 && newMiddle != middle); // Deleted token IDs are never reused.
	assert(PGTDToken(domain, raw[2]) == last);
	PGTDEnd();
}

static void testSamplingAndRecordBounds() {
	static PGTDReport report;
	PGTDBegin();
	uint32_t claims = 0;
	for (uint32_t i = 0; i < 512; ++i) claims += PGTDClaimPacket(32) != 0;
	assert(claims == 4); // One per 128 DRAW packets.
	assert(!PGTDClaimPacket(23));
	assert(PGTDClaimPacket(7));
	const uint64_t entity = PGTDScopeEnter(1, 0x1212, 0, 11, PGTD_SCOPE_FLAG_KEY_VALID);
	for (uint32_t i = 0; i < 512; ++i) claims += PGTDClaimPacket(32) != 0;
	assert(claims <= 4 + 4 + 2); // Baseline plus no more than two entity samples.
	const uint64_t stage = PGTDScopeEnter(102, 0, 0, 0, 0);
	assert(PGTDClaimPacket(32)); // Stage change gets the next DRAW.
	PGTDScopeLeave(stage);
	PGTDScopeLeave(entity);
	PGTDBegin();
	const uint64_t renderStage = PGTDScopeEnter(100, 0, 0, 0, 0);
	assert(PGTDClaimPacket(26));
	assert(!PGTDClaimPacket(26)); // Only once for this stage version.
	const auto renderCamera = cameraFor(26);
	PGTDProducerPacket(0x2600, 26, 6, &renderCamera);
	PGTDTag renderTag{};
	assert(PGTDConsumePacket(0x2600, 26, 6, &renderTag));
	PGTDMetalSample renderSample{};
	renderSample.tag = renderTag;
	renderSample.opcode = 26;
	PGTDRecordMetal(&renderSample);
	assert(PGTDClaimPacket(32)); // op 26 does not consume DRAW's stage-change claim.
	PGTDScopeLeave(renderStage);
	const uint64_t nextRenderStage = PGTDScopeEnter(101, 0, 0, 0, 0);
	assert(PGTDClaimPacket(26));
	assert(!PGTDClaimPacket(26));
	PGTDScopeLeave(nextRenderStage);
	PGTDEnd();
	assert(PGTDCopyReport(&report));
	assert(report.recordCount == 1 && report.samples[0].opcode == 26);
	assert(report.samples[0].tag.stage == 100 && report.stats.matched == 1);

	PGTDBegin();
	uint32_t broadClaims = 0;
	for (uint32_t klass = 1; klass <= 8; ++klass) {
		for (uint32_t key = 1; key <= 2; ++key) {
			const uint64_t scope = PGTDScopeEnter(klass, 0x8000 + klass, 0,
				key, PGTD_SCOPE_FLAG_KEY_VALID);
			for (uint32_t i = 0; i < 512; ++i) broadClaims += PGTDClaimPacket(32) != 0;
			PGTDScopeLeave(scope);
		}
	}
	assert(broadClaims == 64 + 24); // 64 regular plus the global per-frame extra cap.

	PGTDMetalSample sample{};
	sample.tag.session = PGTDBegin(); // New epoch, old packet state must be discarded.
	sample.drawKind = 1;
	for (uint32_t i = 0; i < PGTD_MAX_RECORDS + 12; ++i) {
		sample.tag.session = PGTDEnabled() ? sample.tag.session : 0;
		// Capture the active session using a matched packet, without exposing an API
		// that returns it outside the diagnostic tag.
		PGTDProducerPacket(0x500000 + i, 32, 1, nullptr);
		PGTDTag tag{};
		assert(PGTDConsumePacket(0x500000 + i, 32, 1, &tag));
		sample.tag = tag;
		PGTDRecordMetal(&sample);
	}
	assert(PGTDNextFrame() == 1);
	PGTDEnd();
	assert(PGTDCopyReport(&report));
	assert(report.recordCount == PGTD_MAX_RECORDS);
	assert(report.frameCount == 1);
	assert(report.samples[0].frame == 1);
	assert(report.stats.recordOverflow == 12 && report.stats.truncatedRecords == 12);

	PGTDBegin();
	for (uint32_t i = 0; i < PGTD_TAG_BUDGET - 1; ++i) {
		const uint64_t scope = PGTDScopeEnter(100 + (i & 1u), 0, 0, 0, 0);
		assert(scope != 0 && PGTDClaimPacket(32));
		PGTDScopeLeave(scope);
	}
	const uint64_t overBudgetScope = PGTDScopeEnter(101, 0, 0, 0, 0);
	assert(overBudgetScope != 0 && PGTDClaimPacket(26)); // Shares the DRAW budget.
	assert(overBudgetScope != 0 && !PGTDClaimPacket(32));
	assert(PGTDClaimPacket(7)); // PRESENT remains unconditional past the budget.
	PGTDScopeLeave(overBudgetScope);
	PGTDEnd();
	assert(PGTDCopyReport(&report));
	assert(report.stats.claimed == PGTD_TAG_BUDGET + 1);
	assert(report.stats.overflow == 2);
}

static void testCameraValidationAndConcurrency() {
	PGTDBegin();
	auto bad = cameraFor(0);
	bad.projection[7] = std::numeric_limits<float>::quiet_NaN();
	PGTDProducerPacket(0x9000, 32, 1, &bad);
	PGTDTag tag{};
	assert(PGTDConsumePacket(0x9000, 32, 1, &tag));
	assert(tag.session != 0 && tag.camera.valid == 0);
	assert((tag.flags & PGTD_TAG_FLAG_CAMERA_INVALID) != 0 && tag.camera.rawViewport == 0);
	for (float value : tag.camera.view) assert(value == 0);
	for (float value : tag.camera.projection) assert(value == 0);
	assert(tag.camera.nearPlane == 0 && tag.camera.farPlane == 0);
	PGTDMetalSample invalidCameraSample{};
	invalidCameraSample.tag = tag;
	invalidCameraSample.vsToken = PGTDToken(90, 0x90000001);
	invalidCameraSample.psToken = PGTDToken(91, 0x90000002);
	PGTDRecordMetal(&invalidCameraSample);

	constexpr uint32_t count = 600;
	std::atomic<bool> done{false};
	std::atomic<uint32_t> acknowledged{0};
	std::atomic<uint32_t> consumed{0};
	const auto good = cameraFor(77);
	std::thread producer([&] {
		for (uint32_t i = 0; i < count; ++i) {
			while (acknowledged.load(std::memory_order_acquire) != i) {}
			PGTDProducerPacket(0x10000000ull + i, 32, i, &good);
		}
		done.store(true, std::memory_order_release);
	});
	std::thread consumer([&] {
		uint32_t cursor = 0;
		while (!done.load(std::memory_order_acquire) || cursor < count) {
			if (cursor >= count) continue;
			PGTDTag current{};
			if (PGTDConsumePacket(0x10000000ull + cursor, 32, cursor, &current)) {
				assert(current.session != 0 && current.camera.view[0] == 77);
				assert(current.camera.rawViewport == 0);
				consumed.fetch_add(1, std::memory_order_relaxed);
				++cursor;
				acknowledged.store(cursor, std::memory_order_release);
			}
		}
	});
	producer.join();
	consumer.join();
	PGTDEnd();
	static PGTDReport report;
	assert(PGTDCopyReport(&report));
	assert(report.stats.invalidCamera == 1);
	assert(report.recordCount == 1);
	assert(report.samples[0].tag.camera.valid == 0);
	assert(report.samples[0].vsToken == invalidCameraSample.vsToken);
	assert(report.samples[0].psToken == invalidCameraSample.psToken);
	assert(consumed.load() > 0 && report.stats.matched == consumed.load() + 1);
}

int main() {
	testDisabledAndSessionIsolation();
	testNestedScopesAndFailClosed();
	testWideCandidateIdentityAndSampling();
	testThreadInvalidation();
	testWrongHeaderCollisionAndAnonymization();
	testTokenTombstoneProbeChain();
	testSamplingAndRecordBounds();
	testCameraValidationAndConcurrency();
	std::puts("PASS: temporal diagnostic epochs, scopes, packet matching, anonymization, sampling, bounds, camera validation and concurrency.");
}
