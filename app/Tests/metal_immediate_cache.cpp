#include "../Source/MetalRenderer/MRImmediateCache.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

// A stage/encoder model drives the actual production content cache. Its
// observable driver state must match an encoder that binds on every draw.
struct Stage {
	mr::ImmediateBytesCache cache;
	std::array<uint8_t, mr::ImmediateBytesCache::Capacity> driver{};
	std::size_t driverSize = 0;
	unsigned calls = 0;
	bool normalBuffer = false;
	void beginEncoder() { cache.invalidate(); driverSize = 0; normalBuffer = false; }
	void bufferAtSharedSlot() { cache.invalidate(); normalBuffer = true; }
	void bytes(const void *value, std::size_t length) {
		if (!cache.matches(value, length)) {
			std::memcpy(driver.data(), value, length);
			driverSize = length;
			normalBuffer = false;
			cache.remember(value, length);
			calls++;
		}
		assert(!normalBuffer);
		assert(driverSize == length);
		assert(std::memcmp(driver.data(), value, length) == 0);
	}
};

int main() {
	std::array<uint32_t, 36> words{};
	Stage stages[3];
	// The first draw must bind, including when its bytes are all zeros.
	for (auto &stage : stages) { stage.bytes(words.data(), 16); assert(stage.calls == 1); }
	// Vertex, fragment and compute have independent binding state.
	for (unsigned i = 0; i < 10000; i++) {
		for (auto &stage : stages) stage.bytes(words.data(), 16);
	}
	for (const auto &stage : stages) assert(stage.calls == 1);
	// Changing only the length cannot leave stale trailing constants bound.
	stages[0].bytes(words.data(), 32); assert(stages[0].calls == 2);
	stages[0].bytes(words.data(), 16); assert(stages[0].calls == 3);
	// A changed storage-buffer length must reach the encoder.
	words[4] = 4096;
	stages[1].bytes(words.data(), 32); assert(stages[1].calls == 2);
	words[4] = 8192;
	stages[1].bytes(words.data(), 32); assert(stages[1].calls == 3);
	stages[1].bytes(words.data(), 32); assert(stages[1].calls == 3);
	// Full capacity, including the final word, participates in comparisons.
	words[35] = 123;
	stages[2].bytes(words.data(), sizeof words); assert(stages[2].calls == 2);
	words[35]++;
	stages[2].bytes(words.data(), sizeof words); assert(stages[2].calls == 3);
	// A vertex zero stream/ordinary buffer aliases immediate slot 15. Rebind
	// the same bytes afterward even though their content never changed.
	stages[0].bufferAtSharedSlot();
	stages[0].bytes(words.data(), 16); assert(stages[0].calls == 4);
	stages[0].bufferAtSharedSlot();
	stages[0].bufferAtSharedSlot();
	stages[0].bytes(words.data(), 16); assert(stages[0].calls == 5);
	// New encoder state never inherits a content-cache hit from the old one.
	for (auto &stage : stages) {
		unsigned before = stage.calls;
		stage.beginEncoder(); stage.bytes(words.data(), 16);
		assert(stage.calls == before + 1);
	}
	// Invalid oversized input may never be remembered or compared out of range.
	mr::ImmediateBytesCache invalid;
	invalid.remember(words.data(), sizeof words + 1);
	assert(!invalid.matches(words.data(), sizeof words + 1));
	assert(!invalid.matches(words.data(), 16));
	std::puts("PASS: unchanged, changed, length, independent stages, shared-slot alias, encoder reset, and bounded content");
	std::puts("Synthetic stable sequence: 30,003 requested bindings -> 3 driver bindings (30,000 avoided), not an FPS measurement.");
}
