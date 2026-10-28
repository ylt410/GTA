#include "../Source/MetalRenderer/MRBindingPlan.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr uint8_t kSampler = 3, kStorageRO = 1, kStorageRW = 2, kTexture = 4;
struct Binding { uint8_t kind; uint16_t binding; uint8_t index; int8_t pairedSampler; bool compare; };
struct Metadata {
	std::vector<Binding> resources;
	std::vector<std::pair<uint32_t, uint32_t>> arrayLengths;
};
struct Dynamic {
	uint32_t sizes[32]{};
	uint32_t resourceIds[32]{};
	uint32_t samplerIds[16]{};
	bool samplerCompare[16]{};
};
struct PlanCache { std::unordered_map<const Metadata *, mr::BindingPlan> plans; };
volatile uint64_t gSink = 0;

uint64_t mix(uint64_t hash, uint64_t value) {
	hash ^= value;
	return hash * 1099511628211ull;
}

uint64_t baselineDraw(const Metadata &meta, const Dynamic &dynamic) {
	uint32_t sizes[32] = {};
	uint32_t floatNoFilter = 0;
	uint64_t hash = 1469598103934665603ull;
	for (const Binding &b : meta.resources) {
		if (b.kind == kSampler) continue;
		const uint32_t value = (b.kind == kStorageRO || b.kind == kStorageRW)
			? dynamic.sizes[b.index] : dynamic.resourceIds[b.index & 31];
		hash = mix(hash, ((uint64_t)b.kind << 32) | b.binding);
		hash = mix(hash, value);
		if ((b.kind == kStorageRO || b.kind == kStorageRW)) {
			for (const auto &entry : meta.arrayLengths)
				if (entry.first == b.index && entry.second < 32) sizes[entry.second] = value;
		} else if (b.kind == kTexture && b.pairedSampler >= 0) {
			floatNoFilter |= 1u << ((uint32_t)b.pairedSampler & 31);
		}
	}
	for (const Binding &b : meta.resources) {
		if (b.kind != kSampler) continue;
		const uint32_t slot = (b.binding - 16) & 15;
		uint32_t selected = 0xf001u;
		if (dynamic.samplerCompare[slot] == b.compare) {
			selected = dynamic.samplerIds[slot];
			if ((floatNoFilter >> slot) & 1) selected += 0x10000;
		}
		hash = mix(hash, selected);
	}
	uint32_t count = 0;
	for (const auto &entry : meta.arrayLengths) if (entry.second < 32) count = std::max(count, entry.second + 1);
	for (uint32_t i = 0; i < count; ++i) hash = mix(hash, sizes[i]);
	return hash;
}

uint64_t plannedDraw(const Metadata &meta, const Dynamic &dynamic, PlanCache &cache) {
	// This lookup intentionally models bindStage's unordered_map lookup per draw.
	const mr::BindingPlan &plan = cache.plans.find(&meta)->second;
	uint32_t sizes[32] = {};
	uint32_t floatNoFilter = 0;
	uint64_t hash = 1469598103934665603ull;
	for (uint32_t index : plan.resources) {
		const Binding &b = meta.resources[index];
		const uint32_t value = (b.kind == kStorageRO || b.kind == kStorageRW)
			? dynamic.sizes[b.index] : dynamic.resourceIds[b.index & 31];
		hash = mix(hash, ((uint64_t)b.kind << 32) | b.binding);
		hash = mix(hash, value);
		if ((b.kind == kStorageRO || b.kind == kStorageRW)) {
			for (uint8_t sizeIndex : plan.sizeIndicesByBuffer[b.index]) sizes[sizeIndex] = value;
		} else if (b.kind == kTexture && b.pairedSampler >= 0) {
			floatNoFilter |= 1u << ((uint32_t)b.pairedSampler & 31);
		}
	}
	for (uint32_t index : plan.samplers) {
		const Binding &b = meta.resources[index];
		const uint32_t slot = (b.binding - 16) & 15;
		uint32_t selected = 0xf001u;
		if (dynamic.samplerCompare[slot] == b.compare) {
			selected = dynamic.samplerIds[slot];
			if ((floatNoFilter >> slot) & 1) selected += 0x10000;
		}
		hash = mix(hash, selected);
	}
	for (uint32_t i = 0; i < plan.immediateArrayCount; ++i) hash = mix(hash, sizes[i]);
	return hash;
}

Dynamic dynamicFor(uint32_t draw) {
	Dynamic d;
	for (uint32_t i = 0; i < 32; ++i) {
		d.sizes[i] = 256 + ((draw * 17 + i * 131) & 0xffff);
		d.resourceIds[i] = (draw * 29) ^ (i * 65537u);
	}
	for (uint32_t i = 0; i < 16; ++i) {
		d.samplerIds[i] = (draw * 7) ^ (i * 313u);
		d.samplerCompare[i] = ((draw + i) & 1) != 0;
	}
	return d;
}

Metadata fixture() {
	Metadata m;
	// A resource-heavy graphics shader fixture: 10 live non-sampler resources,
	// 5 sampler declarations, 4 storage-array length destinations and one
	// resource array with multiple sizes.
	m.resources = {
		{0, 0, 0, -1, false}, {1, 160, 3, -1, false}, {4, 33, 6, 1, false},
		{kSampler, 17, 1, -1, false}, {2, 161, 4, -1, false}, {4, 36, 8, 4, false},
		{kSampler, 20, 4, -1, true}, {0, 15, 15, -1, false}, {1, 162, 5, -1, false},
		{kSampler, 18, 2, -1, false}, {4, 39, 10, 6, false}, {2, 163, 7, -1, false},
		{kSampler, 22, 6, -1, true}, {0, 3, 2, -1, false}, {kSampler, 23, 7, -1, false},
	};
	m.arrayLengths = {{3, 0}, {3, 1}, {3, 4}, {4, 2}, {5, 5}, {7, 6}, {7, 8}, {9, 3}};
	return m;
}

template <class Function>
double timeSample(Function function, const Metadata &meta, PlanCache &cache,
                  uint32_t frames, uint32_t drawsPerFrame, uint64_t &checksum) {
	auto start = std::chrono::steady_clock::now();
	uint64_t sum = 0;
	for (uint32_t frame = 0; frame < frames; ++frame) {
		for (uint32_t draw = 0; draw < drawsPerFrame; ++draw) {
			const Dynamic d = dynamicFor(frame * drawsPerFrame + draw);
			sum = mix(sum, function(meta, d, cache));
		}
	}
	gSink = sum;
	checksum = gSink;
	auto finish = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::nano>(finish - start).count();
}

uint64_t plannedAdapter(const Metadata &m, const Dynamic &d, PlanCache &cache) {
	return plannedDraw(m, d, cache);
}
uint64_t baselineAdapter(const Metadata &m, const Dynamic &d, PlanCache &) {
	return baselineDraw(m, d);
}
} // namespace

int main() {
	const Metadata meta = fixture();
	PlanCache cache;
	cache.plans.emplace(&meta, mr::BindingPlan::build(meta.resources, meta.arrayLengths, kSampler));
	constexpr uint32_t drawsPerFrame = 5000, framesPerSample = 20, pairs = 3;
	const uint32_t draws = drawsPerFrame * framesPerSample;
	std::array<double, pairs> baseNs{}, planNs{};
	uint64_t baselineChecksum = 0, plannedChecksum = 0;
	// Warm both implementations before timing.
	timeSample(baselineAdapter, meta, cache, 1, drawsPerFrame, baselineChecksum);
	timeSample(plannedAdapter, meta, cache, 1, drawsPerFrame, plannedChecksum);
	if (baselineChecksum != plannedChecksum) return 2;
	for (uint32_t pair = 0; pair < pairs; ++pair) {
		uint64_t checksums[4]{};
		double times[4]{};
		// ABBA order controls first-order thermal/frequency drift.
		times[0] = timeSample(baselineAdapter, meta, cache, framesPerSample, drawsPerFrame, checksums[0]);
		times[1] = timeSample(plannedAdapter, meta, cache, framesPerSample, drawsPerFrame, checksums[1]);
		times[2] = timeSample(plannedAdapter, meta, cache, framesPerSample, drawsPerFrame, checksums[2]);
		times[3] = timeSample(baselineAdapter, meta, cache, framesPerSample, drawsPerFrame, checksums[3]);
		if (checksums[0] != checksums[1] || checksums[0] != checksums[2] || checksums[0] != checksums[3]) return 3;
		baseNs[pair] = (times[0] + times[3]) / (2.0 * draws);
		planNs[pair] = (times[1] + times[2]) / (2.0 * draws);
		baselineChecksum = checksums[0]; plannedChecksum = checksums[2];
	}
	auto median = [](std::array<double, pairs> v) {
		std::sort(v.begin(), v.end()); return v[1];
	};
	const double base = median(baseNs), planned = median(planNs);
	std::printf("{\"workload\":\"renderer_binding_metadata_host_microbenchmark\",\"scope\":\"host metadata iteration and plan lookup only; excludes Metal calls, resource resolution, GPU work, and FPS\",\"draws_per_frame\":%u,\"frames_per_sample\":%u,\"samples\":%u,\"order\":\"ABBA\",\"resource_declarations\":%zu,\"non_sampler_resource_declarations\":10,\"sampler_declarations\":5,\"array_length_entries\":%zu,\"checksums_match\":true,\"checksum\":%llu,\"baseline_median_ns_per_draw\":%.3f,\"planned_with_unordered_map_lookup_median_ns_per_draw\":%.3f,\"ratio_planned_over_baseline\":%.4f}\n",
		drawsPerFrame, framesPerSample, pairs, meta.resources.size(), meta.arrayLengths.size(),
		(unsigned long long)baselineChecksum, base, planned, planned / base);
}
