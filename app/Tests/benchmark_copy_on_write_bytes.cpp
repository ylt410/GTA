#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include "../Source/MetalRenderer/MRCopyOnWriteBytes.h"

using mr::CopyOnWriteBytes;
using Clock = std::chrono::steady_clock;

static size_t bcMipBytes(unsigned side) {
	size_t total = 0;
	for (unsigned w = side, h = side;; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
		total += size_t((w + 3) / 4) * ((h + 3) / 4) * 8; // BC1 blocks
		if (w == 1 && h == 1) break;
	}
	return total;
}

static uint64_t ns(Clock::time_point a, Clock::time_point b) {
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}

static void run(unsigned copyCount, size_t bytes, unsigned iterations, bool mixedWrites) {
	std::vector<uint8_t> source(bytes);
	for (size_t i = 0; i < bytes; ++i) source[i] = uint8_t(i * 31u + 7u);
	const uint64_t groupStartLive = CopyOnWriteBytes::statistics().livePayloadCapacityBytes;
	CopyOnWriteBytes root;
	root.resize(bytes);
	std::copy(source.begin(), source.end(), root.mutableData());

	auto c0 = CopyOnWriteBytes::statistics();
	uint64_t cowNs = 0;
	std::vector<CopyOnWriteBytes> finalCow;
	for (unsigned it = 0; it < iterations; ++it) {
		auto start = Clock::now();
		std::vector<CopyOnWriteBytes> copies(copyCount, root);
		for (unsigned i = 0; i < copyCount; ++i) {
			if (mixedWrites && (i & 15u) == 0) copies[i].mutableData()[(it + i) % bytes] ^= 0x3d;
		}
		auto stop = Clock::now();
		cowNs += ns(start, stop);
		if (it + 1 == iterations) finalCow = std::move(copies);
	}
	auto c1 = CopyOnWriteBytes::statistics();
	const uint64_t cowLiveDelta = c1.livePayloadCapacityBytes - c0.livePayloadCapacityBytes;
	const uint64_t cowCloneDelta = c1.cloneBytes - c0.cloneBytes;
	const uint64_t cowAllocDelta = c1.payloadAllocations - c0.payloadAllocations;
	const uint64_t cowAllocatedDelta = c1.cumulativePayloadCapacityAllocatedBytes - c0.cumulativePayloadCapacityAllocatedBytes;
	const uint64_t cowLivePayload = c1.livePayloadCapacityBytes - groupStartLive;

	uint64_t vectorNs = 0, vectorPayloadBytes = 0;
	std::vector<std::vector<uint8_t>> finalVector;
	for (unsigned it = 0; it < iterations; ++it) {
		auto start = Clock::now();
		std::vector<std::vector<uint8_t>> copies(copyCount, source);
		for (unsigned i = 0; i < copyCount; ++i) {
			if (mixedWrites && (i & 15u) == 0) copies[i][(it + i) % bytes] ^= 0x3d;
		}
		auto stop = Clock::now();
		vectorNs += ns(start, stop);
		for (const auto &copy : copies) vectorPayloadBytes += copy.capacity();
		if (it + 1 == iterations) finalVector = std::move(copies);
	}
	uint64_t vectorLivePayload = source.capacity();
	for (const auto &copy : finalVector) vectorLivePayload += copy.capacity();
	// Verify every byte outside timed sections. Mutations always start from root.
	for (unsigned i = 0; i < copyCount; ++i) for (size_t at = 0; at < bytes; ++at) {
		uint8_t expected = source[at];
		if (mixedWrites && (i & 15u) == 0 && at == ((iterations - 1 + i) % bytes)) expected ^= 0x3d;
		if (finalCow[i].data()[at] != expected || finalVector[i][at] != expected) {
			std::fprintf(stderr, "payload mismatch: mode=%s copies=%u size=%zu copy=%u byte=%zu\n",
				mixedWrites ? "mixed16:1" : "read-only", copyCount, bytes, i, at);
			std::abort();
		}
	}
	std::printf("{\"mode\":\"%s\",\"copies\":%u,\"payload_bytes\":%zu,\"iterations\":%u,"
		"\"cow_ns\":%llu,\"vector_ns\":%llu,\"cow_live_delta_bytes\":%llu,"
		"\"cow_live_payload_bytes\":%llu,\"vector_live_payload_bytes\":%llu,\"cow_payload_allocated_bytes\":%llu,\"cow_clone_bytes\":%llu,\"cow_allocations\":%llu,\"vector_copied_payload_bytes\":%llu}\n",
		mixedWrites ? "mixed16:1" : "read-only", copyCount, bytes, iterations, (unsigned long long)cowNs, (unsigned long long)vectorNs,
		(unsigned long long)cowLiveDelta, (unsigned long long)cowLivePayload, (unsigned long long)vectorLivePayload, (unsigned long long)cowAllocatedDelta, (unsigned long long)cowCloneDelta,
		(unsigned long long)cowAllocDelta, (unsigned long long)vectorPayloadBytes);
}

int main() {
	for (bool mixedWrites : {false, true}) for (unsigned clones : {1u, 4u, 16u, 64u}) {
		for (unsigned side : {16u, 32u, 64u, 128u}) run(clones, bcMipBytes(side), 100, mixedWrites);
	}
	return 0;
}
