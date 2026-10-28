// Portable indexing plan for immutable reflected shader bindings.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mr {

struct BindingPlan {
	std::vector<uint32_t> resources;
	std::vector<uint32_t> samplers;
	std::array<std::vector<uint8_t>, 32> sizeIndicesByBuffer;
	uint32_t immediateArrayCount = 0;

	template <class Bindings, class ArrayLengths>
	static BindingPlan build(const Bindings &bindings, const ArrayLengths &arrayLengths,
	                         uint8_t samplerKind) {
		BindingPlan plan;
		plan.resources.reserve(bindings.size());
		plan.samplers.reserve(bindings.size());
		for (uint32_t i = 0; i < bindings.size(); ++i) {
			if (static_cast<uint8_t>(bindings[i].kind) == samplerKind) plan.samplers.push_back(i);
			else plan.resources.push_back(i);
		}
		for (const auto &entry : arrayLengths) {
			if (entry.second < 32) {
				if (entry.first < plan.sizeIndicesByBuffer.size())
					plan.sizeIndicesByBuffer[entry.first].push_back((uint8_t)entry.second);
				plan.immediateArrayCount = std::max(plan.immediateArrayCount, entry.second + 1);
			}
		}
		return plan;
	}
};

}  // namespace mr
