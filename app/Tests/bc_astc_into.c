#define _POSIX_C_SOURCE 200809L
#include "../Source/BC1ToASTC.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng = 0x31ca57a9u;
static uint8_t randomByte(void) {
	rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
	return (uint8_t)rng;
}
static double now(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static void verify(int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t pad, uint32_t extraRows) {
	const size_t blockBytes = kind == 1 ? 8 : 16;
	uint32_t bw = w / 4 + (w % 4 != 0), bh = h / 4 + (h % 4 != 0);
	uint32_t bpr = (uint32_t)(bw * blockBytes + pad), rpi = bh + extraRows;
	size_t sourceSize = (size_t)bpr * rpi * d;
	uint8_t *source = malloc(sourceSize); assert(source);
	for (size_t i = 0; i < sourceSize; ++i) source[i] = randomByte();
	size_t outputImage = (size_t)bw * 16 * bh, outputSize = outputImage * d;
	size_t layoutSize = 99; uint32_t layoutBpr = 99, layoutRows = 99;
	assert(PGTranscodeBCToASTCOutputSize(kind, w, h, d, bpr, rpi, source, sourceSize, &layoutSize, &layoutBpr, &layoutRows));
	assert(layoutSize == outputSize && layoutBpr == bw * 16 && layoutRows == bh);
	size_t minimumSource = (size_t)(d - 1) * bpr * rpi + (size_t)bpr * bh;
	layoutSize = 99; layoutBpr = layoutRows = 99;
	assert(!PGTranscodeBCToASTCOutputSize(kind, w, h, d, bpr, rpi, source, minimumSource - 1, &layoutSize, &layoutBpr, &layoutRows));
	assert(layoutSize == 0 && layoutBpr == 0 && layoutRows == 0);
	uint8_t *expected = malloc(outputSize), *actual = malloc(outputSize + 16); assert(expected && actual);
	size_t legacySize = 0; uint32_t legacyBpr = 0, legacyRpi = 0;
	uint8_t *legacy = PGTranscodeBCToASTC(kind, w, h, d, bpr, rpi, source, sourceSize, &legacySize, &legacyBpr, &legacyRpi);
	assert(legacy && legacySize == outputSize && legacyBpr == bw * 16 && legacyRpi == bh);
	for (uint32_t z = 0; z < d; ++z) for (uint32_t y = 0; y < bh; ++y) for (uint32_t x = 0; x < bw; ++x) {
		const uint8_t *block = source + (size_t)z * bpr * rpi + (size_t)y * bpr + (size_t)x * blockBytes;
		uint8_t *to = expected + (size_t)z * outputImage + (size_t)y * bw * 16 + (size_t)x * 16;
		if (kind == 1) PGBC1BlockToASTC(block, to); else PGBC3BlockToASTC(block, to);
	}
	assert(!memcmp(legacy, expected, outputSize)); free(legacy);
	memset(actual, 0x6d, outputSize + 16);
	size_t written = 99; uint32_t outBpr = 99, outRpi = 99;
	assert(PGTranscodeBCToASTCInto(kind, w, h, d, bpr, rpi, source, sourceSize, actual, outputSize, &written, &outBpr, &outRpi));
	assert(written == outputSize && outBpr == legacyBpr && outRpi == legacyRpi && !memcmp(actual, expected, outputSize));
	for (size_t i = outputSize; i < outputSize + 16; ++i) assert(actual[i] == 0x6d);
	memset(actual, 0x6d, outputSize + 16); written = 99; outBpr = outRpi = 99;
	assert(!PGTranscodeBCToASTCInto(kind, w, h, d, bpr, rpi, source, sourceSize, actual, outputSize - 1, &written, &outBpr, &outRpi));
	assert(written == 0 && outBpr == 0 && outRpi == 0);
	for (size_t i = 0; i < outputSize + 16; ++i) assert(actual[i] == 0x6d);
	written = 99; outBpr = outRpi = 99;
	assert(!PGTranscodeBCToASTCInto(kind, w, h, d, bpr, rpi, source, minimumSource - 1, actual, outputSize + 16, &written, &outBpr, &outRpi));
	assert(written == 0 && outBpr == 0 && outRpi == 0);
	for (size_t i = 0; i < outputSize + 16; ++i) assert(actual[i] == 0x6d);
	if (bh > 1) {
		assert(!PGTranscodeBCToASTCInto(kind, w, h, d, bpr, bh - 1, source, sourceSize, actual, outputSize + 16, &written, &outBpr, &outRpi));
		assert(written == 0 && outBpr == 0 && outRpi == 0);
	}
	free(source); free(expected); free(actual);
}

static double benchmark(int kind, int reuse) {
	const uint32_t w = 1024, h = 1024, bw = w / 4, bh = h / 4;
	size_t inputBytes = (size_t)bw * bh * (kind == 1 ? 8 : 16);
	size_t outputBytes = (size_t)bw * 16 * bh;
	uint8_t *source = malloc(inputBytes), *destination = malloc(outputBytes);
	assert(source && destination);
	for (size_t i = 0; i < inputBytes; ++i) source[i] = randomByte();
	volatile uint8_t sample = 0;
	double start = now();
	for (int i = 0; i < 40; ++i) {
		size_t written = 0; uint32_t bpr = 0, rows = 0; uint8_t *temporary = NULL;
		if (reuse) assert(PGTranscodeBCToASTCInto(kind, w, h, 1, bw * (kind == 1 ? 8 : 16), bh, source, inputBytes,
		                                            destination, outputBytes, &written, &bpr, &rows));
		else {
			temporary = PGTranscodeBCToASTC(kind, w, h, 1, bw * (kind == 1 ? 8 : 16), bh, source, inputBytes, &written, &bpr, &rows);
			assert(temporary); memcpy(destination, temporary, outputBytes);
		}
		assert(written == outputBytes && bpr == bw * 16 && rows == bh);
		sample ^= destination[outputBytes / 2];
		free(temporary);
	}
	double elapsed = (now() - start) * 1000 / 40 + sample * 0.0;
	free(source); free(destination); return elapsed;
}

int main(void) {
	for (int kind = 1; kind <= 3; kind += 2) {
		for (uint32_t mip = 1; mip <= 1024; mip *= 2)
			for (uint32_t depth = 1; depth <= 3; depth += 2)
				for (uint32_t pad = 0; pad <= 20; pad += 20)
					verify(kind, mip, mip > 3 ? mip - 3 : mip, depth, pad, pad ? 2 : 0);
	}
	puts("PASS: BC1/BC3 direct ASTC output matches the allocating API across mip dimensions, odd edges, padded rows and depth; undersized destinations and bad row counts are rejected without writes.");
	if (getenv("PG_ASTC_INTO_MICROBENCH")) {
		for (int kind = 1; kind <= 3; kind += 2) {
			double allocating = benchmark(kind, 0), reused = benchmark(kind, 1);
			printf("BC%d 1024x1024 ASTC staging: allocating transcode + ring copy %.3f ms/upload, direct into ring %.3f ms/upload, %.2fx\n", kind, allocating, reused, allocating / reused);
		}
	}
	return 0;
}
