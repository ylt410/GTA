#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "../Source/MetalRenderer/MRCopyOnWriteBytes.h"

using mr::CopyOnWriteBytes;

static void same(const CopyOnWriteBytes &actual, const std::vector<uint8_t> &expected) {
	assert(actual.size() == expected.size());
	assert(actual.empty() == expected.empty());
	if (!expected.empty()) assert(std::equal(expected.begin(), expected.end(), actual.data()));
}

int main() {
	CopyOnWriteBytes src;
	std::vector<uint8_t> expected(4096);
	src.resize(expected.size());
	for (size_t i = 0; i < expected.size(); ++i) expected[i] = uint8_t((i * 37u + 9u) & 255u);
	std::copy(expected.begin(), expected.end(), src.mutableData());

	// CopyResource-like snapshot: cheap shared copy, then independent partial writes.
	CopyOnWriteBytes dst = src;
	std::vector<uint8_t> expectedDst = expected;
	assert(src.data() == dst.data());
	const auto beforeSameSizeResize = CopyOnWriteBytes::statistics();
	dst.resize(dst.size());
	const auto afterSameSizeResize = CopyOnWriteBytes::statistics();
	assert(src.data() == dst.data());
	assert(afterSameSizeResize.cloneBytes == beforeSameSizeResize.cloneBytes);
	for (size_t i = 700; i < 901; ++i) { dst.mutableData()[i] ^= 0x5a; expectedDst[i] ^= 0x5a; }
	same(src, expected); same(dst, expectedDst);
	assert(src.data() != dst.data());

	// Destroying the source must not invalidate a copied destination.
	{
		CopyOnWriteBytes temporary = dst;
		dst = CopyOnWriteBytes();
		same(temporary, expectedDst);
	}

	// std::vector-compatible resize: preserve prefix, zero-fill growth, release on zero.
	CopyOnWriteBytes resized = src;
	std::vector<uint8_t> expectedResize = expected;
	resized.resize(5000); expectedResize.resize(5000);
	same(resized, expectedResize); same(src, expected);
	resized.resize(17); expectedResize.resize(17); same(resized, expectedResize);
	resized.resize(0); expectedResize.resize(0); same(resized, expectedResize);
	assert(resized.data() == nullptr);

	// Distinct wrappers may detach and write concurrently from one shared payload.
	CopyOnWriteBytes base = src;
	CopyOnWriteBytes left = base, right = base;
	std::thread a([&] { left.mutableData()[0] = 11; });
	std::thread b([&] { right.mutableData()[0] = 22; });
	a.join(); b.join();
	assert(left.data()[0] == 11 && right.data()[0] == 22 && base.data()[0] == expected[0]);
	same(base, expected);

	puts("copy_on_write_bytes: ok");
	return 0;
}
