// BC1 blocks rewritten as ASTC 4x4 blocks, for GPUs without BC support (iPhones).
//
// A BC1 block is two RGB565 endpoints and a 2-bit index per texel; an ASTC 4x4 block can hold the
// same two endpoints and a weight per texel, so each block converts on its own, with no image search:
//  - four-colour blocks (c0 > c1): RGB endpoints at 8 bits, 2-bit weights 0, 1/3, 2/3, 1
//    (ASTC's 21/64 and 43/64 for BC1's thirds: within one step of 255);
//  - three-colour blocks without transparency: 3-level weights 0, 1/2, 1 (exact midpoint);
//  - three-colour blocks with transparent texels: two weight planes, colour and alpha, with
//    7-bit RGBA endpoints (every remaining bit is used).
// ASTC is 8 bits per texel: half of the 16-bit A1BGR5 copy and a quarter of RGBA8.
#include "BC1ToASTC.h"
#include <stdlib.h>
#include <string.h>

// The ASTC integer sequence for trits: five values in 8 bits. Built once by running the spec's decoder
// over every 8-bit pattern and keeping the first pattern for each combination.
static uint8_t tritPack[243];
static void buildTrits(void) {
	static int built;
	if (built) return;
	uint8_t seen[243] = {0};
	for (unsigned T = 0; T < 256; T++) {
		unsigned C, t[5];
		if (((T >> 2) & 7) == 7) { C = ((T >> 5) & 7) << 2 | (T & 3); t[4] = 2; t[3] = 2; }
		else { C = T & 31; if (((T >> 5) & 3) == 3) { t[4] = 2; t[3] = T >> 7 & 1; } else { t[4] = T >> 7 & 1; t[3] = T >> 5 & 3; } }
		if ((C & 3) == 3) { t[2] = 2; t[1] = C >> 4 & 1; t[0] = (C >> 3 & 1) << 1 | ((C >> 2 & 1) & ~(C >> 3) & 1); }
		else if (((C >> 2) & 3) == 3) { t[2] = 2; t[1] = 2; t[0] = C & 3; }
		else { t[2] = C >> 4 & 1; t[1] = C >> 2 & 3; t[0] = (C >> 1 & 1) << 1 | ((C & 1) & ~(C >> 1) & 1); }
		if (t[0] > 2 || t[1] > 2 || t[2] > 2 || t[3] > 2 || t[4] > 2) continue;
		unsigned key = t[0] + 3 * (t[1] + 3 * (t[2] + 3 * (t[3] + 3 * t[4])));
		if (!seen[key]) { seen[key] = 1; tritPack[key] = (uint8_t)T; }
	}
	built = 1;
}

// Weights (values 0..2, 3-level range) as an integer sequence, written from bit 127 downwards.
static unsigned __int128 tritWeights(const uint8_t *w, unsigned count) {
	static const unsigned bitsFor[6] = {0, 2, 4, 5, 7, 8};
	uint64_t stream = 0;
	unsigned bit = 0;
	for (unsigned g = 0; g < count; g += 5) {
		unsigned n = count - g < 5 ? count - g : 5, key = 0, mul = 1;
		for (unsigned i = 0; i < n; i++, mul *= 3) key += w[g + i] * mul;
		stream |= (uint64_t)(tritPack[key] & ((1u << bitsFor[n]) - 1)) << bit;
		bit += bitsFor[n];
	}
	return (unsigned __int128)__builtin_bitreverse64(stream) << 64;		// stream bit i lands on block bit 127 - i
}

static uint8_t quant7(unsigned v8) {		// best 7-bit endpoint: decodes as v << 1 | v >> 6
	int best = 0, bestError = 1000;
	for (int v = (int)(v8 >> 1) - 1; v <= (int)(v8 >> 1) + 1; v++) {
		if (v < 0 || v > 127) continue;
		int e = abs((v << 1 | v >> 6) - (int)v8);
		if (e < bestError) { bestError = e; best = v; }
	}
	return (uint8_t)best;
}

void PGBC1BlockToASTC(const uint8_t bc1[8], uint8_t astc[16]) {
	buildTrits();
	unsigned c0 = bc1[0] | bc1[1] << 8, c1 = bc1[2] | bc1[3] << 8;
	uint32_t indices = bc1[4] | bc1[5] << 8 | bc1[6] << 16 | (uint32_t)bc1[7] << 24;
	unsigned e[2][3] = {
		{(((c0 >> 11) & 31) * 527 + 23) >> 6, (((c0 >> 5) & 63) * 259 + 33) >> 6, ((c0 & 31) * 527 + 23) >> 6},
		{(((c1 >> 11) & 31) * 527 + 23) >> 6, (((c1 >> 5) & 63) * 259 + 33) >> 6, ((c1 & 31) * 527 + 23) >> 6}};
	bool fourColour = c0 > c1, transparent = false;
	if (!fourColour) for (unsigned i = 0; i < 16; i++) if ((indices >> 2 * i & 3) == 3) transparent = true;
	if (fourColour) {
		// Block mode 0x042: 4x4 weights, range 0..3, one plane. CEM 8 (RGB direct), endpoints at 8 bits.
		// Built as one 128-bit value: this is most blocks, and it runs on the render thread for every upload.
		bool swap = e[1][0] + e[1][1] + e[1][2] < e[0][0] + e[0][1] + e[0][2];	// keep ASTC's blue contraction off
		unsigned __int128 v = 0x042 | 8u << 13;
		for (unsigned c = 0; c < 3; c++) v |= (unsigned __int128)(e[swap][c] | e[!swap][c] << 8) << (17 + 16 * c);
		// BC1 index to weight (0, 3, 1, 2) per 2-bit field: low bit = i0 ^ i1, high bit = i0. Weight bit k of
		// texel i sits at bit 127 - (2i + k), which is the 32-bit index word reversed into the top 32 bits.
		uint32_t weights = ((indices ^ indices >> 1) & 0x55555555u) | (indices << 1 & 0xAAAAAAAAu);
		if (swap) weights = ~weights;
		v |= (unsigned __int128)__builtin_bitreverse32(weights) << 96;
		memcpy(astc, &v, 16);		// little-endian: bit n of v is bit n of the block
		return;
	}
	static const uint8_t weightOf[4] = {0, 2, 1, 0};		// c0, c1, midpoint; the transparent texel's colour doesn't matter
	bool swap = e[1][0] + e[1][1] + e[1][2] < e[0][0] + e[0][1] + e[0][2];
	if (!transparent) {
		// Block mode 0x051: 4x4 weights, range 0..2, one plane. CEM 8, endpoints at 8 bits.
		unsigned __int128 v = 0x051 | 8u << 13;
		for (unsigned c = 0; c < 3; c++) v |= (unsigned __int128)(e[swap][c] | e[!swap][c] << 8) << (17 + 16 * c);
		uint8_t w[16];
		for (unsigned i = 0; i < 16; i++) { w[i] = weightOf[indices >> 2 * i & 3]; if (swap) w[i] = 2 - w[i]; }
		v |= tritWeights(w, 16);
		memcpy(astc, &v, 16);
		return;
	}
	// Block mode 0x451: two planes (colour, then alpha: CCS 3), range 0..2. CEM 12 (RGBA direct), 7-bit endpoints.
	uint8_t q[2][4];
	for (unsigned k = 0; k < 2; k++) for (unsigned c = 0; c < 3; c++) q[k][c] = quant7(e[k][c]);
	q[0][3] = 127; q[1][3] = 0;		// alpha: endpoint 0 opaque, endpoint 1 transparent
	unsigned s0 = 0, s1 = 0;
	for (unsigned c = 0; c < 3; c++) { s0 += q[0][c] << 1 | q[0][c] >> 6; s1 += q[1][c] << 1 | q[1][c] >> 6; }
	swap = s1 < s0;
	unsigned __int128 v = 0x451 | 12u << 13;
	for (unsigned c = 0; c < 4; c++) v |= (unsigned __int128)(q[swap][c] | q[!swap][c] << 7) << (17 + 14 * c);
	uint8_t w[32];
	for (unsigned i = 0; i < 16; i++) {
		unsigned index = indices >> 2 * i & 3, colour = weightOf[index], alpha = index == 3 ? 2 : 0;
		if (swap) { colour = 2 - colour; alpha = 2 - alpha; }
		w[2 * i] = (uint8_t)colour; w[2 * i + 1] = (uint8_t)alpha;
	}
	v |= tritWeights(w, 32);
	v |= (unsigned __int128)3 << (128 - 52 - 2);		// colour component selector, right below the 52 weight bits
	memcpy(astc, &v, 16);
}

// BC3 (DXT5): a BC1 colour block that always uses its four colours, plus an alpha block (two 8-bit endpoints and a
// 3-bit index per texel: 8 levels). ASTC 4x4 holds it in 128 bits as follows (GTA-118):
//  - one alpha in the whole block (most BC3 blocks: opaque or flat): one weight plane, RGBA endpoints at 8 bits, the
//    colour weights exactly as BC1's. Lossless apart from ASTC's 21/64 and 43/64 for BC1's thirds.
//  - alpha that varies: two weight planes (colour, alpha), 2-bit weights, eight endpoints at range 48 (a trit and 4
//    bits each: the 45 bits left). The colour keeps BC1's weights; its endpoints round to the nearest of 48 levels.
//    The alpha drops from 8 levels to 4 between endpoints fitted to the block's own alphas.
// Measured on the Mac's GPU against bcdec on 1M blocks the game uploaded (56% flat alpha): colour 47-48 dB (largest
// error 4), alpha 34 dB overall and 30.5 dB in varying blocks (a full 0-255 gradient can be off by up to ~43), about
// 50 ns a block.
static uint8_t unq48[48], q48ForValue[256];		// range-48 endpoint value -> 8 bits; 8 bits -> nearest value
static void build48(void) {
	static int built;
	if (built) return;
	for (unsigned v = 0; v < 48; v++) {
		unsigned D = v >> 4, bits = v & 15, a = bits & 1, x = bits >> 1 & 7;
		unsigned A = a ? 0x1FF : 0, B = x << 6 | x, T = D * 22 + B;
		T ^= A;
		unq48[v] = (uint8_t)((A & 0x80) | (T >> 2));
	}
	for (unsigned t = 0; t < 256; t++) {
		unsigned best = 0, bestError = 1000;
		for (unsigned v = 0; v < 48; v++) { unsigned e = (unsigned)abs((int)unq48[v] - (int)t); if (e < bestError) { bestError = e; best = v; } }
		q48ForValue[t] = (uint8_t)best;
	}
	built = 1;
}

// Eight range-48 values as an integer sequence from bit 17 upwards: groups of five values with their trits packed
// into 8 bits (spread between the 4-bit parts), then three values.
static unsigned __int128 pack48(const uint8_t v[8]) {
	static const unsigned tBits[5] = {2, 2, 1, 2, 1};
	unsigned __int128 out = 0;
	unsigned bit = 17;
	for (unsigned g = 0; g < 8; g += 5) {
		unsigned n = 8 - g < 5 ? 8 - g : 5, key = 0, mul = 1;
		for (unsigned i = 0; i < n; i++, mul *= 3) key += (v[g + i] >> 4) * mul;
		unsigned T = tritPack[key], used = 0;
		for (unsigned i = 0; i < n; i++) {
			out |= (unsigned __int128)(v[g + i] & 15) << bit; bit += 4;
			unsigned take = tBits[i];
			if (n == 3 && i == 2) take = 1;		// three values: 5 trit bits (2, 2, 1)
			out |= (unsigned __int128)((T >> used) & ((1u << take) - 1)) << bit; bit += take; used += take;
		}
	}
	return out;
}

static inline unsigned lerp64(unsigned e0, unsigned e1, unsigned w) { return (e0 * (64 - w) + e1 * w + 32) >> 6; }

void PGBC3BlockToASTC(const uint8_t bc3[16], uint8_t astc[16]) {
	buildTrits(); build48();
	// Alpha: the eight levels as bcdec computes them, then each texel's.
	unsigned a[8], alpha[16];
	uint64_t ab = 0; for (int i = 7; i >= 0; i--) ab = ab << 8 | bc3[i];
	a[0] = bc3[0]; a[1] = bc3[1];
	if (a[0] > a[1]) for (unsigned k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
	else { for (unsigned k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5; a[6] = 0; a[7] = 255; }
	unsigned aMin = 255, aMax = 0;
	for (unsigned i = 0; i < 16; i++) { alpha[i] = a[ab >> (16 + 3 * i) & 7]; if (alpha[i] < aMin) aMin = alpha[i]; if (alpha[i] > aMax) aMax = alpha[i]; }
	// Colour: BC1's endpoints and four-colour weights.
	const uint8_t *c = bc3 + 8;
	unsigned c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
	uint32_t indices = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
	unsigned e[2][3] = {
		{(((c0 >> 11) & 31) * 527 + 23) >> 6, (((c0 >> 5) & 63) * 259 + 33) >> 6, ((c0 & 31) * 527 + 23) >> 6},
		{(((c1 >> 11) & 31) * 527 + 23) >> 6, (((c1 >> 5) & 63) * 259 + 33) >> 6, ((c1 & 31) * 527 + 23) >> 6}};
	uint32_t weights = ((indices ^ indices >> 1) & 0x55555555u) | (indices << 1 & 0xAAAAAAAAu);	// BC1 index -> 0, 3, 1, 2
	if (aMin == aMax) {
		// Block mode 0x042 (4x4 weights, range 0..3, one plane), CEM 12 (RGBA direct), endpoints at 8 bits.
		bool swap = e[1][0] + e[1][1] + e[1][2] < e[0][0] + e[0][1] + e[0][2];
		unsigned __int128 v = 0x042 | 12u << 13;
		for (unsigned k = 0; k < 3; k++) v |= (unsigned __int128)(e[swap][k] | e[!swap][k] << 8) << (17 + 16 * k);
		v |= (unsigned __int128)(aMin | aMin << 8) << (17 + 48);
		if (swap) weights = ~weights;
		v |= (unsigned __int128)__builtin_bitreverse32(weights) << 96;
		memcpy(astc, &v, 16);
		return;
	}
	static const unsigned wv[4] = {0, 21, 43, 64};		// ASTC's 2-bit weights
	uint8_t w[16];
	for (unsigned i = 0; i < 16; i++) w[i] = weights >> 2 * i & 3;
	uint8_t q[8];		// r0 r1 g0 g1 b0 b1 a0 a1 as range-48 values
	// Colour endpoints: the nearest of the 48 levels. A search against the block's colours gained 0.35 dB on 1M real
	// blocks (PG_DUMP_BC) at 7x the time, and doubled the largest error.
	for (unsigned k = 0; k < 3; k++) { q[2 * k] = q48ForValue[e[0][k]]; q[2 * k + 1] = q48ForValue[e[1][k]]; }
	// Alpha: endpoints around the block's lowest and highest alpha, each texel on the nearest of four levels.
	uint8_t aw[16];
	{
		unsigned bestError = ~0u; uint8_t best0 = q48ForValue[aMin], best1 = q48ForValue[aMax];
		for (int d0 = -2; d0 <= 2; d0++) for (int d1 = -2; d1 <= 2; d1++) {
			int t0 = (int)aMin + d0 * 6, t1 = (int)aMax + d1 * 6;
			if (t0 < 0 || t0 > 255 || t1 < 0 || t1 > 255) continue;
			unsigned x0 = q48ForValue[t0], x1 = q48ForValue[t1], error = 0;
			for (unsigned i = 0; i < 16; i++) {
				unsigned be = ~0u;
				for (unsigned k = 0; k < 4; k++) { int dd = (int)lerp64(unq48[x0], unq48[x1], wv[k]) - (int)alpha[i]; if ((unsigned)(dd * dd) < be) be = (unsigned)(dd * dd); }
				error += be;
			}
			if (error < bestError) { bestError = error; best0 = (uint8_t)x0; best1 = (uint8_t)x1; }
		}
		q[6] = best0; q[7] = best1;
		for (unsigned i = 0; i < 16; i++) {
			unsigned be = ~0u;
			for (unsigned k = 0; k < 4; k++) { int dd = (int)lerp64(unq48[q[6]], unq48[q[7]], wv[k]) - (int)alpha[i]; if ((unsigned)(dd * dd) < be) { be = (unsigned)(dd * dd); aw[i] = (uint8_t)k; } }
		}
	}
	// ASTC swaps the endpoints (and contracts blue) when endpoint 1's RGB sum is below endpoint 0's: swap first.
	unsigned s0 = unq48[q[0]] + unq48[q[2]] + unq48[q[4]], s1 = unq48[q[1]] + unq48[q[3]] + unq48[q[5]];
	bool swap = s1 < s0;
	if (swap) {
		for (unsigned k = 0; k < 4; k++) { uint8_t t = q[2 * k]; q[2 * k] = q[2 * k + 1]; q[2 * k + 1] = t; }
		for (unsigned i = 0; i < 16; i++) { w[i] = 3 - w[i]; aw[i] = 3 - aw[i]; }
	}
	// Block mode 0x442: 4x4 weights, range 0..3, two planes; CEM 12; colour component selector 3 (alpha).
	unsigned __int128 v = 0x442 | 12u << 13;
	v |= pack48(q);
	uint64_t stream = 0;
	for (unsigned i = 0; i < 16; i++) stream |= (uint64_t)w[i] << 4 * i | (uint64_t)aw[i] << (4 * i + 2);
	v |= (unsigned __int128)__builtin_bitreverse64(stream) << 64;
	v |= (unsigned __int128)3 << 62;
	memcpy(astc, &v, 16);
}

void *PGTranscodeBC1ToASTC(uint32_t w, uint32_t h, uint32_t d, uint32_t bpr, uint32_t rpi, const void *data, size_t size,
                           size_t *outSize, uint32_t *outBpr, uint32_t *outRpi) {
	return PGTranscodeBCToASTC(1, w, h, d, bpr, rpi, data, size, outSize, outBpr, outRpi);
}

void *PGTranscodeBCToASTC(int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t bpr, uint32_t rpi, const void *data, size_t size,
                          size_t *outSize, uint32_t *outBpr, uint32_t *outRpi) {
	if (kind != 1 && kind != 3) return NULL;
	const size_t blockBytes = kind == 1 ? 8 : 16;
	uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, imageRows = rpi ? rpi : bh;
	size_t pitch = (size_t)bw * 16, image = pitch * bh;
	if ((size_t)bw * blockBytes > bpr || (size_t)bpr * imageRows * (d - 1) + (size_t)bpr * bh > size) return NULL;
	uint8_t *out = malloc(image * d);
	if (!out) return NULL;
	for (uint32_t z = 0; z < d; z++) for (uint32_t y = 0; y < bh; y++) {
		const uint8_t *row = (const uint8_t *)data + (size_t)z * bpr * imageRows + (size_t)y * bpr;
		uint8_t *to = out + z * image + y * pitch;
		if (kind == 1) for (uint32_t x = 0; x < bw; x++) PGBC1BlockToASTC(row + 8 * x, to + 16 * x);
		else for (uint32_t x = 0; x < bw; x++) PGBC3BlockToASTC(row + 16 * x, to + 16 * x);
	}
	*outSize = image * d; *outBpr = (uint32_t)pitch; *outRpi = bh;
	return out;
}
