#pragma once
#include <cstdint>

// Shared by the live bounded probe and the actual-Metal regression test.
// Scalar arrays intentionally avoid host SIMD alignment/column-major ambiguity.
struct PGTDProbeGPUParams {
    float currentToPrevious[16];
    uint32_t width, height, gridWidth, gridHeight;
    float epsilon, maximumMotion;
};
struct PGTDProbeGPUSample {
    float depth, motionX, motionY, previousU, previousV;
    uint32_t flags, pixelX, pixelY;
};
static_assert(sizeof(PGTDProbeGPUParams) == 88, "MSL parameter layout");
static_assert(sizeof(PGTDProbeGPUSample) == 32, "MSL result layout");

static constexpr const char *kPGTDProbeKernel = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Params {
    float m[16]; uint width, height, gridWidth, gridHeight;
    float epsilon, maximumMotion;
};
struct Sample {
    float depth, dx, dy, previousU, previousV;
    uint flags, pixelX, pixelY;
};
kernel void pgtd_camera_depth_probe(depth2d<float> depth [[texture(0)]],
                                   constant Params &p [[buffer(0)]],
                                   device Sample *out [[buffer(1)]],
                                   uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= p.gridWidth || gid.y >= p.gridHeight) return;
    Sample s = {};
    s.pixelX = min(uint((float(gid.x) + 0.5f) * float(p.width) / float(p.gridWidth)), p.width - 1);
    s.pixelY = min(uint((float(gid.y) + 0.5f) * float(p.height) / float(p.gridHeight)), p.height - 1);
    s.depth = depth.read(uint2(s.pixelX, s.pixelY));
    if (isfinite(s.depth) && s.depth > 0.0f && s.depth <= 1.0f) {
        s.flags = 1;
        float2 uv = (float2(s.pixelX, s.pixelY) + 0.5f) / float2(p.width, p.height);
        float4 clip(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y, s.depth, 1.0f);
        float4 prev;
        prev.x = dot(clip, float4(p.m[0], p.m[4], p.m[8], p.m[12]));
        prev.y = dot(clip, float4(p.m[1], p.m[5], p.m[9], p.m[13]));
        prev.z = dot(clip, float4(p.m[2], p.m[6], p.m[10], p.m[14]));
        prev.w = dot(clip, float4(p.m[3], p.m[7], p.m[11], p.m[15]));
        if (all(isfinite(prev)) && prev.w > p.epsilon) {
            float3 ndc = prev.xyz / prev.w;
            if (all(isfinite(ndc)) && all(ndc.xy >= -1.0f) && all(ndc.xy <= 1.0f) && ndc.z >= 0.0f && ndc.z <= 1.0f) {
                float2 previousUV((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f);
                float2 motion = (previousUV - uv) * float2(p.width, p.height);
                if (all(isfinite(motion)) && all(abs(motion) <= p.maximumMotion)) {
                    s.flags |= 2; s.dx = motion.x; s.dy = motion.y;
                    s.previousU = previousUV.x; s.previousV = previousUV.y;
                }
            }
        }
    }
    out[gid.y * p.gridWidth + gid.x] = s;
}
)MSL";
