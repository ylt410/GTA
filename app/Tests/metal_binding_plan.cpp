#include "../Source/MetalRenderer/MRBindingPlan.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

struct Binding { uint8_t kind; uint16_t binding; uint8_t index; };

static void assertSame(const mr::BindingPlan &plan, const std::vector<Binding> &bindings,
                       const std::vector<std::pair<uint32_t, uint32_t>> &lengths,
                       uint8_t samplerKind) {
	std::vector<uint32_t> resources, samplers;
	for (uint32_t i = 0; i < bindings.size(); ++i)
		(static_cast<uint8_t>(bindings[i].kind) == samplerKind ? samplers : resources).push_back(i);
	assert(plan.resources == resources);
	assert(plan.samplers == samplers);
	std::array<std::vector<uint8_t>, 32> expected{};
	uint32_t immediateCount = 0;
	for (const auto &entry : lengths) {
		if (entry.second < 32) {
			if (entry.first < expected.size()) expected[entry.first].push_back((uint8_t)entry.second);
			immediateCount = std::max(immediateCount, entry.second + 1);
		}
	}
	assert(plan.sizeIndicesByBuffer == expected);
	assert(plan.immediateArrayCount == immediateCount);
}

int main() {
	constexpr uint8_t sampler = 3;
	std::vector<Binding> bindings{
		{0, 0, 0}, {sampler, 16, 4}, {2, 34, 7}, {sampler, 17, 5},
		{1, 176, 9}, {0, 15, 15},
	};
	std::vector<std::pair<uint32_t, uint32_t>> lengths{
		{7, 0}, {7, 4}, {9, 2}, {40, 6}, {5, 32},
	};
	auto plan = mr::BindingPlan::build(bindings, lengths, sampler);
	assertSame(plan, bindings, lengths, sampler);
	assert((plan.resources == std::vector<uint32_t>{0, 2, 4, 5}));
	assert((plan.samplers == std::vector<uint32_t>{1, 3}));
	assert((plan.sizeIndicesByBuffer[7] == std::vector<uint8_t>{0, 4}));
	assert(plan.immediateArrayCount == 7); // index 6 contributes to block length even without a valid buffer slot.

	// Empty reflection and absent array metadata remain valid: runtime binding
	// still supplies dummy resources, while the immediate block is its 16-byte header.
	std::vector<Binding> emptyBindings;
	std::vector<std::pair<uint32_t, uint32_t>> noLengths;
	auto empty = mr::BindingPlan::build(emptyBindings, noLengths, sampler);
	assertSame(empty, emptyBindings, noLengths, sampler);
	assert(empty.immediateArrayCount == 0);
	std::puts("PASS: stable resource/sampler order, repeated size destinations, invalid metadata bounds, and empty reflection");
}
