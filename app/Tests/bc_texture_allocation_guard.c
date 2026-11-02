#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static size_t allocationCalls;
static void *countedMalloc(size_t size) { ++allocationCalls; return malloc(size); }

#define malloc countedMalloc
#include "../Source/BCTextureDecoder.c"
#include "../Source/BC1ToASTC.c"
#undef malloc

int main(void) {
	uint8_t block[16] = {0}; size_t outputBytes = 99; uint32_t outputBpr = 99, outputRows = 99;
	// Both outputs are large enough to allocate, but the tiny compressed source
	// is invalid. Sizing and allocating wrappers must reject before malloc.
	assert(!PGDecodeBCOutputSize(1, 4096, 4096, 1, 8192, 1024, block, sizeof block, &outputBytes));
	assert(outputBytes == 0);
	assert(!PGDecodeBC(1, 4096, 4096, 1, 8192, 1024, block, sizeof block, &outputBytes));
	assert(outputBytes == 0 && allocationCalls == 0);
	assert(!PGTranscodeBCToASTCOutputSize(1, 16384, 16384, 1, 32768, 4096, block, sizeof block,
	                                     &outputBytes, &outputBpr, &outputRows));
	assert(outputBytes == 0 && outputBpr == 0 && outputRows == 0);
	assert(!PGTranscodeBCToASTC(1, 16384, 16384, 1, 32768, 4096, block, sizeof block,
	                           &outputBytes, &outputBpr, &outputRows));
	assert(outputBytes == 0 && outputBpr == 0 && outputRows == 0 && allocationCalls == 0);
	// Keep the allocation ceiling at 256 MiB for both expanded decodes and
	// ASTC transcodes, even when the advertised compressed span is large.
	assert(!PGDecodeBCOutputSize(1, 8196, 8196, 1, 16392, 2049, block, SIZE_MAX, &outputBytes));
	assert(outputBytes == 0);
	assert(!PGDecodeBC(1, 8196, 8196, 1, 16392, 2049, block, SIZE_MAX, &outputBytes));
	assert(outputBytes == 0 && allocationCalls == 0);
	assert(!PGTranscodeBCToASTCOutputSize(1, 16388, 16388, 1, 32776, 4097, block, SIZE_MAX,
	                                     &outputBytes, &outputBpr, &outputRows));
	assert(outputBytes == 0 && outputBpr == 0 && outputRows == 0);
	assert(!PGTranscodeBCToASTC(1, 16388, 16388, 1, 32776, 4097, block, SIZE_MAX,
	                           &outputBytes, &outputBpr, &outputRows));
	assert(outputBytes == 0 && outputBpr == 0 && outputRows == 0 && allocationCalls == 0);
	puts("PASS: invalid large BC regions fail sizing and allocating wrappers before any destination allocation.");
	return 0;
}
