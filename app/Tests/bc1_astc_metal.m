// Checks Source/BC1ToASTC.c against the GPU: random BC1 blocks of every kind are rewritten as ASTC,
// decoded by Metal (Apple silicon Mac), and compared texel by texel with bcdec's BC1 decode.
// Run: Tests/check_bc1_astc.sh
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "BC1ToASTC.h"

static const char *kKernel =
	"#include <metal_stdlib>\nusing namespace metal;\n"
	"kernel void dump(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
	"  if (p.x >= t.get_width() || p.y >= t.get_height()) return;\n"
	"  float4 c = t.read(p); out[p.y * t.get_width() + p.x] = c;\n}\n";

static uint8_t toSRGB(float linear) {
	float s = linear <= 0.0031308f ? linear * 12.92f : 1.055f * powf(linear, 1 / 2.4f) - 0.055f;
	return (uint8_t)lroundf(fminf(fmaxf(s, 0), 1) * 255);
}

int main(void) {
	@autoreleasepool {
		const int W = 1024, H = 1024, BW = W / 4, BH = H / 4;
		uint8_t *bc1 = malloc((size_t)BW * BH * 8);
		srand(12345);
		int kinds[3] = {0};
		for (int i = 0; i < BW * BH; i++) {
			uint8_t *b = bc1 + 8 * i;
			for (int k = 0; k < 8; k++) b[k] = (uint8_t)rand();
			int kind = i % 3;		// 0: four colours, 1: three colours, 2: three colours + transparent
			unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
			if ((kind == 0) != (c0 > c1)) { b[0] = (uint8_t)c1; b[1] = (uint8_t)(c1 >> 8); b[2] = (uint8_t)c0; b[3] = (uint8_t)(c0 >> 8); }
			c0 = b[0] | b[1] << 8; c1 = b[2] | b[3] << 8;
			if (kind == 0 && c0 == c1) { b[0] ^= 1; if ((b[0] | b[1] << 8) < c1) b[1] |= 0x80; }
			uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
			if (kind == 1) for (int t = 0; t < 16; t++) if ((idx >> 2 * t & 3) == 3) idx &= ~(1u << 2 * t);	// 3 -> 2
			if (kind == 2 && (idx & 3) != 3) idx |= 3;
			memcpy(b + 4, &idx, 4);
			c0 = b[0] | b[1] << 8; c1 = b[2] | b[3] << 8;
			bool tr = false; for (int t = 0; t < 16; t++) if ((idx >> 2 * t & 3) == 3) tr = true;
			kinds[c0 > c1 ? 0 : tr ? 2 : 1]++;
		}
		size_t astcSize; uint32_t astcBpr, astcRpi;
		uint8_t *astc = PGTranscodeBC1ToASTC(W, H, 1, BW * 8, BH, bc1, (size_t)BW * BH * 8, &astcSize, &astcBpr, &astcRpi);
		if (!astc) { puts("FAIL: transcode refused the region"); return 1; }
		uint8_t *expect = malloc((size_t)W * H * 4);
		for (int by = 0; by < BH; by++) for (int bx = 0; bx < BW; bx++)
			bcdec_bc1(bc1 + 8 * (by * BW + bx), expect + ((size_t)by * 4 * W + bx * 4) * 4, W * 4);

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		NSError *error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:@(kKernel) options:nil error:&error];
		id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"dump"] error:&error];
		if (!pipeline) { printf("FAIL: %s\n", error.localizedDescription.UTF8String); return 1; }
		int failures = 0;
		for (int srgb = 0; srgb < 2; srgb++) {
			MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:srgb ? MTLPixelFormatASTC_4x4_sRGB : MTLPixelFormatASTC_4x4_LDR width:W height:H mipmapped:NO];
			d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
			id<MTLTexture> tex = [device newTextureWithDescriptor:d];
			if (!tex) { puts("FAIL: no ASTC texture on this GPU"); return 1; }
			[tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:astc bytesPerRow:astcBpr];
			id<MTLBuffer> out = [device newBufferWithLength:(size_t)W * H * 16 options:MTLResourceStorageModeShared];
			id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
			id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
			[e setComputePipelineState:pipeline]; [e setTexture:tex atIndex:0]; [e setBuffer:out offset:0 atIndex:0];
			[e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
			[e endEncoding]; [cb commit]; [cb waitUntilCompleted];
			const float *got = out.contents;
			int worst[3] = {0}, worstAlpha = 0, bad = 0;
			for (int i = 0; i < W * H; i++) {
				int bx = (i % W) / 4, by = (i / W) / 4; const uint8_t *b = bc1 + 8 * (by * BW + bx);
				unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
				int kind = c0 > c1 ? 0 : 1;
				const uint8_t *x = expect + 4 * i; const float *g = got + 4 * i;
				int alphaDiff = abs(x[3] - (int)lroundf(g[3] * 255));
				if (alphaDiff > worstAlpha) worstAlpha = alphaDiff;
				if (alphaDiff > 1) bad++;
				if (x[3] == 0) continue;		// transparent: colour undefined
				for (int c = 0; c < 3; c++) {
					int v = srgb ? toSRGB(g[c]) : (int)lroundf(g[c] * 255);
					int diff = abs(v - x[c]), limit = 2;
					if (diff > worst[kind]) worst[kind] = diff;
					if (diff > limit) bad++;
				}
			}
			printf("%s: worst colour difference four-colour %d, three-colour %d; worst alpha %d; texels over the limit %d\n",
			       srgb ? "ASTC sRGB" : "ASTC linear", worst[0], worst[1], worstAlpha, bad);
			failures += bad;
		}
		printf("blocks: %d four-colour, %d three-colour, %d with transparency\n", kinds[0], kinds[1], kinds[2]);
		puts(failures ? "FAIL" : "PASS");
		return failures ? 1 : 0;
	}
}
