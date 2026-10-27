// BC3 to ASTC quality check (GTA-118): encodes BC3 blocks with Source/BC1ToASTC.c, lets this Mac's GPU decode the
// ASTC, and compares every texel with bcdec's BC3 decode (PSNR per channel, flat- and varying-alpha blocks apart).
// Real blocks: run the simulator with PG_FORCE_BC_DECODE=1 PG_DUMP_BC=3 and copy Documents/bc3-blocks.bin.
//
//   clang -O2 -fobjc-arc -framework Metal -framework Foundation -I Source -I Tools/bcdec \
//     Scripts/astc_check.m Source/BC1ToASTC.c -o /tmp/astc_check
//   /tmp/astc_check [bc3-blocks.bin]      (raw BC3 blocks, 16 bytes each; random blocks without a file)
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "BC1ToASTC.h"
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

static const char *kSrc =
"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void dec(texture2d<float> t [[texture(0)]], device uchar4 *out [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
"  constexpr sampler s(filter::nearest, coord::pixel);\n"
"  if (p.x >= t.get_width() || p.y >= t.get_height()) return;\n"
"  float4 c = t.sample(s, float2(p) + 0.5);\n"
"  out[p.y * t.get_width() + p.x] = uchar4(round(saturate(c) * 255.0));\n"
"}\n";

int main(int argc, char **argv) {
  @autoreleasepool {
    size_t n = 64 * 64 * 16;		// blocks
    uint8_t *bc3 = NULL;
    if (argc > 1) {
      FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
      n = (size_t)sz / 16; n -= n % 4096; if (n > 4096 * 256) n = 4096 * 256;
      bc3 = malloc(n * 16); fread(bc3, 16, n, f); fclose(f);
    } else {
      bc3 = malloc(n * 16); srand(7);
      for (size_t b = 0; b < n; b++) {
        uint8_t *k = bc3 + 16 * b;
        for (int i = 0; i < 16; i++) k[i] = rand() & 255;
        if (b % 3 == 0) { k[0] = k[1] = 255; }		// opaque blocks, like most real ones
        if (b % 3 == 1) { k[0] = 255; k[1] = 0; }	// full alpha range
      }
    }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kSrc] options:nil error:&err];
    if (!lib) { printf("shader: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"dec"] error:&err];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const uint32_t W = 256, H = 256, per = (W / 4) * (H / 4);
    double se[4] = {0}, seFlat[4] = {0}, seVar[4] = {0}; size_t cnt = 0, cntFlat = 0, cntVar = 0; int maxErr[4] = {0}, maxFlat[4] = {0};
    long hist[256] = {0};
    double encodeSeconds = 0;
    for (size_t base = 0; base < n; base += per) {
      uint8_t *astc = malloc(per * 16);
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      for (uint32_t i = 0; i < per; i++) PGBC3BlockToASTC(bc3 + 16 * (base + i), astc + 16 * i);
      clock_gettime(CLOCK_MONOTONIC, &t1); encodeSeconds += (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR width:W height:H mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
      id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
      [tex replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:astc bytesPerRow:(W / 4) * 16];
      id<MTLBuffer> out = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
      [ce setComputePipelineState:ps]; [ce setTexture:tex atIndex:0]; [ce setBuffer:out offset:0 atIndex:0];
      [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)]; [ce endEncoding];
      [cb commit]; [cb waitUntilCompleted];
      const uint8_t *gpu = out.contents;
      for (uint32_t i = 0; i < per; i++) {
        uint8_t ref[64]; bcdec_bc3(bc3 + 16 * (base + i), ref, 16);
        bool flat = true; for (int p = 1; p < 16; p++) if (ref[4 * p + 3] != ref[3]) flat = false;
        uint32_t bx = i % (W / 4), by = i / (W / 4);
        for (int p = 0; p < 16; p++) {
          const uint8_t *g = gpu + ((by * 4 + p / 4) * W + bx * 4 + p % 4) * 4;
          for (int c = 0; c < 4; c++) {
            int d = (int)g[c] - (int)ref[4 * p + c]; double d2 = d * d;
            se[c] += d2; if (abs(d) > maxErr[c]) maxErr[c] = abs(d);
            if (flat) { seFlat[c] += d2; if (abs(d) > maxFlat[c]) maxFlat[c] = abs(d); } else seVar[c] += d2;
            if (c == 3 && !flat) hist[abs(d)]++;
          }
        }
        cnt += 16; if (flat) cntFlat += 16; else cntVar += 16;
      }
      free(astc);
    }
    const char *ch = "RGBA";
    printf("%zu blocks (%zu flat-alpha, %zu varying), encode %.1f ns/block\n", n, cntFlat / 16, cntVar / 16, encodeSeconds / n * 1e9);
    for (int c = 0; c < 4; c++) {
      double mse = se[c] / cnt, mf = cntFlat ? seFlat[c] / cntFlat : 0, mv = cntVar ? seVar[c] / cntVar : 0;
      printf("%c: PSNR all %.2f dB (max err %d) | flat blocks %.2f dB (max %d) | varying %.2f dB\n", ch[c],
             mse ? 10 * log10(65025 / mse) : 99, maxErr[c], mf ? 10 * log10(65025 / mf) : 99, maxFlat[c], mv ? 10 * log10(65025 / mv) : 99);
    }
    printf("varying-alpha error histogram: "); for (int e = 0; e < 40; e += 4) printf("%d-%d:%ld ", e, e + 3, hist[e] + hist[e+1] + hist[e+2] + hist[e+3]); printf("\n");
  }
  return 0;
}
