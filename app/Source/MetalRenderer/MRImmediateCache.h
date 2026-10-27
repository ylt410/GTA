// Content cache for the small Tint immediate block. Independent of Metal so
// encoder lifetime and shared-slot invalidation can be regression tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mr {

class ImmediateBytesCache {
public:
	// Four reserved words followed by at most 32 storage-buffer lengths.
	static constexpr std::size_t Capacity = (4 + 32) * sizeof(uint32_t);

	void invalidate() { valid = false; }
	bool matches(const void *data, std::size_t length) const {
		return valid && length <= Capacity && size == length && std::memcmp(bytes, data, length) == 0;
	}
	void remember(const void *data, std::size_t length) {
		if (length > Capacity) { invalidate(); return; }
		std::memcpy(bytes, data, length);
		size = length;
		valid = true;
	}

private:
	uint8_t bytes[Capacity] = {};
	std::size_t size = 0;
	bool valid = false;
};

}  // namespace mr
