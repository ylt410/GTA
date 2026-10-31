#include "NativeTemporalCameraProbe.h"
#include "NativeTemporalProbeKernel.h"
#include "NativeTemporalReprojection.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
namespace tr = temporal_reprojection;
constexpr unsigned kGridWidth = 64, kGridHeight = 36, kSamples = kGridWidth * kGridHeight;
constexpr unsigned kMaximumDispatches = 12, kSlots = 3;
std::mutex mutex;
std::condition_variable completion;
std::atomic<bool> requested{false};

struct Frame {
    bool found = false, ambiguous = false, verified = false, boundary = false;
    uint32_t depthToken = 0, sceneTarget = 0, sceneWritten = 0, uiTarget = 0;
    uint32_t width = 0, height = 0;
    double residual = 1.0;
    PGTDCamera camera{};
    id<MTLTexture> depth = nil;
    int result = -1;
};
struct History {
    bool valid = false;
    uint32_t frame = 0;
    PGTDCamera camera{};
    id<MTLTexture> depth = nil;
};
struct Result {
    uint32_t frame = 0, width = 0, height = 0, depthToken = 0, depthFormat = 0;
    uint32_t sceneColorToken = 0, uiColorToken = 0;
    uint32_t validDepth = 0, background = 0, invalidDepth = 0, validMotion = 0;
    uint32_t compared = 0, mismatches = 0;
    double residual = 1.0, maxError = 0, meanMotion = 0, maxMotion = 0;
    bool hud = false;
    const char *status = "pending";
};
struct Slot { id<MTLBuffer> buffer = nil; bool busy = false; };
struct Session {
    uint64_t id = 0;
    bool enabled = false, active = true, pipelineFailed = false;
    uint32_t sourceFrames = 0, candidates = 0, cameraVerifiedFrames = 0, boundaryFrames = 0;
    uint32_t scheduled = 0, completed = 0, gpuFailures = 0, busySkips = 0;
    uint32_t contractRejects = 0, historyResets = 0, pending = 0;
    std::map<std::string, uint32_t> rejections;
    Frame frame;
    History history;
    id<MTLDevice> device = nil;
    id<MTLComputePipelineState> pipeline = nil;
    std::array<Slot, kSlots> slots{};
    std::vector<Result> results;
};
std::shared_ptr<Session> current;

tr::Mat4 matrix(const float *data) {
    tr::Mat4 result{};
    for (unsigned i = 0; i < 16; ++i) result.m[i] = data[i];
    return result;
}
bool sameCamera(const PGTDCamera &a, const PGTDCamera &b) {
    return a.width == b.width && a.height == b.height &&
           a.nearPlane == b.nearPlane && a.farPlane == b.farPlane &&
           std::memcmp(a.view, b.view, sizeof a.view) == 0 &&
           std::memcmp(a.projection, b.projection, sizeof a.projection) == 0;
}
void reject(Session &s, const char *reason) { ++s.contractRejects; ++s.rejections[reason]; }
bool validTexture(id<MTLTexture> t, uint32_t width, uint32_t height) {
    return t && t.textureType == MTLTextureType2D && t.sampleCount == 1 &&
        t.width == width && t.height == height && t.arrayLength == 1 &&
        (t.pixelFormat == MTLPixelFormatDepth32Float || t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) &&
        (t.usage & MTLTextureUsageShaderRead) && t.storageMode != MTLStorageModeMemoryless;
}
bool cameraCut(const PGTDCamera &a, const PGTDCamera &b) {
    tr::Mat4 ia{}, ib{};
    if (!tr::inverse(matrix(a.view), ia) || !tr::inverse(matrix(b.view), ib)) return true;
    double distanceSquared = 0.0, rotationTrace = 0.0;
    for (unsigned i = 12; i < 15; ++i) distanceSquared += (ia.m[i] - ib.m[i]) * (ia.m[i] - ib.m[i]);
    for (unsigned r = 0; r < 3; ++r) for (unsigned c = 0; c < 3; ++c)
        rotationTrace += ia.m[r * 4 + c] * ib.m[r * 4 + c];
    // Conservative experiment reset, not a gameplay-derived cut classifier.
    return distanceSquared > 25.0 * 25.0 || rotationTrace < 2.0;
}
void preparePipeline(const std::shared_ptr<Session> &s) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        MTLCompileOptions *options = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *)) {
            options.mathMode = MTLMathModeSafe;
            options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        } else {
            // Required compatibility path for the app's iOS 16 deployment target.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kPGTDProbeKernel]
                                                    options:options error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"pgtd_camera_depth_probe"];
        id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
        std::array<Slot, kSlots> slots{};
        bool valid = pipeline != nil;
        for (auto &slot : slots) {
            if (valid) slot.buffer = [device newBufferWithLength:kSamples * sizeof(PGTDProbeGPUSample)
                                                       options:MTLResourceStorageModeShared];
            valid = valid && slot.buffer != nil;
        }
        std::lock_guard<std::mutex> lock(mutex);
        s->device = device;
        s->pipelineFailed = !valid;
        if (valid) { s->pipeline = pipeline; s->slots = slots; }
        completion.notify_all();
    }
}
NSDictionary *resultJSON(const Result &r) {
    return @{@"frame": @(r.frame), @"width": @(r.width), @"height": @(r.height),
        @"depthToken": @(r.depthToken), @"depthFormat": @(r.depthFormat),
        @"sceneColorToken": @(r.sceneColorToken), @"uiColorToken": @(r.uiColorToken),
        @"cameraVPResidual": @(r.residual), @"status": [NSString stringWithUTF8String:r.status],
        @"validDepthSamples": @(r.validDepth), @"backgroundSamples": @(r.background),
        @"invalidDepthSamples": @(r.invalidDepth), @"validMotionSamples": @(r.validMotion),
        @"cpuGpuComparedSamples": @(r.compared), @"cpuGpuMismatchSamples": @(r.mismatches),
        @"maxErrorPixels": @(r.maxError), @"meanMotionPixels": @(r.meanMotion),
        @"maxMotionPixels": @(r.maxMotion), @"hudAfterBoundary": @(r.hud)};
}
} // namespace

void PGTDProbeBegin(uint64_t session, bool enabled) {
    auto s = std::make_shared<Session>();
    s->id = session; s->enabled = enabled; s->results.reserve(kMaximumDispatches);
    {
        std::lock_guard<std::mutex> lock(mutex);
        current = s;
        requested.store(enabled, std::memory_order_release);
    }
    // Compilation/allocation never occurs on the renderer thread. Early frames
    // may report pipeline-pending; no synchronous wait or game-thread stall.
    if (enabled) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ preparePipeline(s); });
}
void PGTDProbeEnd(uint64_t session) {
    std::lock_guard<std::mutex> lock(mutex);
    if (current && current->id == session) {
        current->active = false;
        requested.store(false, std::memory_order_release);
    }
}
bool PGTDProbeRequested(void) { return requested.load(std::memory_order_acquire); }

void PGTDProbeInvalidateDepth(id<MTLTexture> depth) {
    if (!depth || !PGTDProbeRequested()) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (current && current->active && current->frame.found && current->frame.depth == depth)
        current->frame.ambiguous = true;
}

void PGTDProbeObserveSample(const PGTDMetalSample &sample, const PGTDProbeDrawState &state) {
    if (!PGTDProbeRequested()) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto s = current;
    if (!s || !s->active || s->id != sample.tag.session) return;
    Frame &f = s->frame;
    if (sample.opcode == 26 && sample.tag.stage == 103) f.sceneTarget = sample.colorToken[0];
    if (sample.opcode == 26 && sample.tag.stage == 105 && f.boundary && !f.uiTarget) {
        f.uiTarget = sample.colorToken[0];
        if (f.result >= 0) s->results[f.result].uiColorToken = f.uiTarget;
    }
    if (sample.opcode != 32 || (sample.flags & 4u)) return; // only actually encoded draws
    if (f.sceneTarget && sample.colorToken[0] == f.sceneTarget && !f.boundary)
        f.sceneWritten = f.sceneTarget;
    if (f.boundary) {
        if (sample.tag.stage == 100 && f.uiTarget && sample.colorToken[0] == f.uiTarget && f.result >= 0)
            s->results[f.result].hud = true;
        return;
    }
    const PGTDCamera &camera = sample.tag.camera;
    if (!camera.valid || !state.fullViewport || (sample.flags & (4u | 8u | 16u | 64u)) ||
        !sample.colorToken[3] || !sample.depthToken || sample.depthMip || sample.depthSlice ||
        !state.depthWrite || (state.depthCompare != MTLCompareFunctionGreater && state.depthCompare != MTLCompareFunctionGreaterEqual) ||
        !validTexture(state.depth, camera.width, camera.height)) return;
    for (unsigned i = 0; i < 4; ++i)
        if (!sample.colorToken[i] || sample.colorWidth[i] != camera.width ||
            sample.colorHeight[i] != camera.height || sample.colorSamples[i] != 1) return;
    ++s->candidates;
    if (f.found && (f.depth != state.depth || !sameCamera(f.camera, camera))) {
        f.ambiguous = true;
        return;
    }
    f.found = true; f.camera = camera; f.depth = state.depth;
    f.depthToken = sample.depthToken; f.width = camera.width; f.height = camera.height;
    double residual = 1.0;
    if (!state.worldKnown || !(sample.flags & 2u)) return;
    if (!tr::verifyWorldViewProjection(matrix(state.worldRows), matrix(camera.view), matrix(camera.projection),
            matrix(sample.wvp), tr::MatrixConvention::RowMajorRowVector, residual, 5e-5,
            camera.nearPlane, camera.farPlane)) {
        f.ambiguous = true; reject(*s, "world-wvp-mismatch"); return;
    }
    f.residual = f.verified ? std::max(f.residual, residual) : residual;
    f.verified = true;
}

bool PGTDProbeNeedsBoundary(const PGTDTag &tag, uint32_t opcode) {
    if (!PGTDProbeRequested() || opcode != 26 || tag.stage != 105) return false;
    std::lock_guard<std::mutex> lock(mutex);
    return current && current->active && current->id == tag.session && !current->frame.boundary;
}

void PGTDProbeEncodeBoundary(id<MTLCommandBuffer> commandBuffer, const PGTDTag &tag) {
    if (!PGTDProbeRequested()) return;
    std::unique_lock<std::mutex> lock(mutex);
    auto s = current;
    if (!s || !s->active || s->id != tag.session || s->frame.boundary) return;
    Frame &f = s->frame;
    f.boundary = true; ++s->boundaryFrames;
    const uint32_t frame = s->sourceFrames + 1;
    if (!f.found) { reject(*s, "no-main-draw"); return; }
    if (f.ambiguous) { reject(*s, "ambiguous-camera"); return; }
    if (!f.verified) { reject(*s, "unverified-camera"); return; }
    if (!validTexture(f.depth, f.width, f.height)) { reject(*s, "unsupported-depth"); return; }
    if (!s->history.valid || s->history.frame + 1 != frame) { reject(*s, "no-history"); return; }
    if (s->history.depth != f.depth) { reject(*s, "resource-change"); ++s->historyResets; return; }
    if (s->history.camera.width != f.camera.width || s->history.camera.height != f.camera.height ||
        s->history.camera.nearPlane != f.camera.nearPlane || s->history.camera.farPlane != f.camera.farPlane) {
        reject(*s, "camera-range-change"); ++s->historyResets; return;
    }
    if (cameraCut(f.camera, s->history.camera)) { reject(*s, "camera-cut"); ++s->historyResets; return; }
    tr::Transform transform{};
    if (!tr::buildGameCameraTransform(matrix(f.camera.view), matrix(f.camera.projection),
        matrix(s->history.camera.view), matrix(s->history.camera.projection),
        tr::MatrixConvention::RowMajorRowVector, {(int)f.width, (int)f.height}, transform,
        f.camera.nearPlane, f.camera.farPlane)) {
        reject(*s, "projection-change"); ++s->historyResets; return;
    }
    // One sparse probe per five source frames; no full-resolution readback.
    if (frame % 5 != 2 || s->scheduled >= kMaximumDispatches) return;
    if (!s->pipeline || !commandBuffer) {
        reject(*s, s->pipelineFailed ? "pipeline-failed" : "pipeline-pending"); return;
    }
    if (commandBuffer.commandQueue.device != s->device) { reject(*s, "device-mismatch"); return; }
    unsigned slot = 0;
    while (slot < kSlots && s->slots[slot].busy) ++slot;
    if (slot == kSlots) { ++s->busySkips; reject(*s, "busy"); return; }
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    if (!encoder) { reject(*s, "encoder-unavailable"); return; }
    transform.epsilon = 1e-7;
    transform.maxMotionPixels = 1e6;
    PGTDProbeGPUParams params{};
    std::copy(transform.gpuCurrentClipToPreviousClip.begin(), transform.gpuCurrentClipToPreviousClip.end(), params.currentToPrevious);
    params.width = f.width; params.height = f.height; params.gridWidth = kGridWidth; params.gridHeight = kGridHeight;
    params.epsilon = (float)transform.epsilon; params.maximumMotion = (float)transform.maxMotionPixels;
    id<MTLBuffer> buffer = s->slots[slot].buffer;
    [encoder setComputePipelineState:s->pipeline];
    [encoder setTexture:f.depth atIndex:0];
    [encoder setBytes:&params length:sizeof params atIndex:0];
    [encoder setBuffer:buffer offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(kGridWidth, kGridHeight, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
    Result result{};
    result.frame = frame; result.width = f.width; result.height = f.height;
    result.depthToken = f.depthToken; result.depthFormat = (uint32_t)f.depth.pixelFormat;
    result.sceneColorToken = f.sceneWritten; result.residual = f.residual;
    const size_t index = s->results.size();
    f.result = (int)index; s->results.push_back(result);
    s->slots[slot].busy = true; ++s->scheduled; ++s->pending;
    [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        @autoreleasepool {
            Result numbers{};
            const bool ok = done.status == MTLCommandBufferStatusCompleted;
            if (ok) {
                const auto *samples = static_cast<const PGTDProbeGPUSample *>(buffer.contents);
                double totalMotion = 0.0;
                for (unsigned i = 0; i < kSamples; ++i) {
                    const auto &p = samples[i];
                    if (!std::isfinite(p.depth) || p.depth < 0 || p.depth > 1) { ++numbers.invalidDepth; continue; }
                    if (p.depth == 0) { ++numbers.background; continue; }
                    ++numbers.validDepth;
                    const double u = (p.pixelX + .5) / transform.viewport.width;
                    const double v = (p.pixelY + .5) / transform.viewport.height;
                    const auto reference = tr::reproject(transform, u, v, p.depth, true);
                    const bool gpuValid = (p.flags & 2u) != 0;
                    bool mismatch = false;
                    if (gpuValid) {
                        ++numbers.validMotion;
                        const double motion = std::hypot(p.motionX, p.motionY);
                        if (std::isfinite(motion)) { totalMotion += motion; numbers.maxMotion = std::max(numbers.maxMotion, motion); }
                        else mismatch = true;
                    }
                    if (reference.valid != gpuValid) { ++numbers.mismatches; continue; }
                    if (!gpuValid) { if (mismatch) ++numbers.mismatches; continue; }
                    ++numbers.compared;
                    const double error = std::max(std::fabs(reference.xPixels - p.motionX), std::fabs(reference.yPixels - p.motionY));
                    if (std::isfinite(error)) numbers.maxError = std::max(numbers.maxError, error);
                    if (!std::isfinite(error) || error > .05 + 1e-4 * std::max(std::fabs(reference.xPixels), std::fabs(reference.yPixels)))
                        mismatch = true;
                    if (mismatch) ++numbers.mismatches;
                }
                if (numbers.validMotion) numbers.meanMotion = totalMotion / numbers.validMotion;
            }
            std::lock_guard<std::mutex> guard(mutex);
            Result &r = s->results[index];
            r.status = ok ? "ok" : "gpu-failed";
            r.validDepth = numbers.validDepth; r.background = numbers.background; r.invalidDepth = numbers.invalidDepth;
            r.validMotion = numbers.validMotion; r.compared = numbers.compared; r.mismatches = numbers.mismatches;
            r.maxError = numbers.maxError; r.meanMotion = numbers.meanMotion; r.maxMotion = numbers.maxMotion;
            if (ok) ++s->completed; else ++s->gpuFailures;
            --s->pending; s->slots[slot].busy = false;
            completion.notify_all();
        }
    }];
}

void PGTDProbeSourcePresent(void) {
    if (!PGTDProbeRequested()) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto s = current;
    if (!s || !s->active) return;
    const Frame &f = s->frame;
    if (!f.boundary) reject(*s, "missing-boundary");
    const bool valid = f.found && f.verified && !f.ambiguous && f.boundary;
    if (valid) ++s->cameraVerifiedFrames;
    if (!valid && s->history.valid) ++s->historyResets;
    s->history.valid = valid;
    if (valid) {
        s->history.frame = s->sourceFrames + 1;
        s->history.camera = f.camera; s->history.depth = f.depth;
    } else s->history.depth = nil;
    ++s->sourceFrames;
    s->frame = Frame{};
}

NSDictionary *PGTDProbeCopySummary(uint64_t session) {
    std::unique_lock<std::mutex> lock(mutex);
    auto s = current;
    if (!s || s->id != session) { s = std::make_shared<Session>(); s->id = session; s->active = false; }
    completion.wait_for(lock, std::chrono::seconds(2), [&] { return s->pending == 0; });
    NSMutableArray *frames = [NSMutableArray arrayWithCapacity:s->results.size()];
    for (const auto &r : s->results) {
        Result copy = r;
        if (std::strcmp(copy.status, "pending") == 0) copy.status = "timed-out";
        [frames addObject:resultJSON(copy)];
    }
    NSMutableDictionary *rejections = [NSMutableDictionary dictionary];
    for (const auto &entry : s->rejections) rejections[[NSString stringWithUTF8String:entry.first.c_str()]] = @(entry.second);
    return @{@"requested": @(s->enabled), @"mode": s->enabled ? @"camera-only-depth-probe" : @"off",
        @"session": [NSString stringWithFormat:@"%016llx", (unsigned long long)s->id],
        @"grid": @[@(kGridWidth), @(kGridHeight)], @"maximumDispatches": @(kMaximumDispatches),
        @"sourceFrames": @(s->sourceFrames), @"candidates": @(s->candidates),
        @"cameraVerifiedFrames": @(s->cameraVerifiedFrames), @"boundaryFrames": @(s->boundaryFrames),
        @"scheduled": @(s->scheduled), @"completed": @(s->completed), @"gpuFailures": @(s->gpuFailures),
        @"busySkips": @(s->busySkips), @"contractRejects": @(s->contractRejects),
        @"historyResets": @(s->historyResets), @"pending": @(s->pending), @"timedOut": @(s->pending != 0),
        @"partialFrame": @(s->frame.found || s->frame.boundary),
        @"readyForTemporal": @NO,
        @"limitations": @"Actual sparse scene-depth reads and camera-only motion. Dynamic actors, deformation, disocclusion, jitter and final clean scene color are not solved. CPU/GPU agreement is arithmetic evidence, not image-quality or full Temporal validation. Scene color tokens identify observed stage-103 writes, not certified final color. No images or depth grids are exported.",
        @"rejections": rejections, @"frames": frames};
}
