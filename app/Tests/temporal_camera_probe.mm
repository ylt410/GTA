#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>

#include "../Source/NativeTemporalCameraProbe.h"
#include "../Source/NativeTemporalProbeKernel.h"
#include "../Source/NativeTemporalReprojection.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

using namespace temporal_reprojection;

static bool check(bool value, const char *message) {
    if (!value) std::fprintf(stderr, "temporal camera probe test failed: %s\n", message);
    return value;
}

static Mat4 identity() {
    Mat4 m{};
    m.m = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    return m;
}

static Mat4 reverseZProjection() {
    constexpr double nearPlane = 0.05, farPlane = 10000.0;
    Mat4 p{};
    p.m = {1,0,0,0, 0,1,0,0,
           0,0,nearPlane/(farPlane-nearPlane),-1,
           0,0,nearPlane*farPlane/(farPlane-nearPlane),0};
    return p;
}

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format,
                              uint32_t width, uint32_t height,
                              MTLStorageMode storage, MTLTextureUsage usage) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
        width:width height:height mipmapped:NO];
    d.storageMode = storage;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

static id<MTLTexture> depthTexture(id<MTLDevice> device, uint32_t width, uint32_t height) {
    return texture(device, MTLPixelFormatDepth32Float, width, height, MTLStorageModePrivate,
                   MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
}

static bool clearDepth(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> depth, double value) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = value;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return false;
    [encoder endEncoding];
    return true;
}

static NSDictionary *copySummaryOnUtilityQueue(uint64_t session) {
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    __block NSDictionary *summary = nil;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            summary = PGTDProbeCopySummary(session);
            dispatch_semaphore_signal(semaphore);
        }
    });
    if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0)
        return nil;
    return summary;
}

static uint64_t integer(NSDictionary *dictionary, NSString *key) {
    return [dictionary[key] respondsToSelector:@selector(unsignedLongLongValue)]
        ? [dictionary[key] unsignedLongLongValue] : 0;
}

static bool directProductionKernelTest(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    constexpr uint32_t width = 64, height = 36, count = width * height;
    MTLCompileOptions *options = [MTLCompileOptions new];
    if (@available(macOS 15.0, iOS 18.0, *)) {
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kPGTDProbeKernel]
        options:options error:&error];
    if (!check(library != nil, error.localizedDescription.UTF8String ?: "production Metal kernel compilation")) return false;
    id<MTLFunction> function = [library newFunctionWithName:@"pgtd_camera_depth_probe"];
    id<MTLComputePipelineState> pipeline = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!check(pipeline != nil, error.localizedDescription.UTF8String ?: "production Metal pipeline creation")) return false;

    id<MTLTexture> input = depthTexture(device, width, height);
    id<MTLBuffer> output = [device newBufferWithLength:count * sizeof(PGTDProbeGPUSample)
                                               options:MTLResourceStorageModeShared];
    if (!check(input != nil && output != nil, "private sampleable real depth texture/result buffer")) return false;

    const auto run = [&](const Mat4 &matrix, double clearValue, bool background,
                         double expectedMotion) -> bool {
        PGTDProbeGPUParams params{};
        for (unsigned i = 0; i < 16; ++i) params.currentToPrevious[i] = (float)matrix.m[i];
        params.width = width; params.height = height; params.gridWidth = width; params.gridHeight = height;
        params.epsilon = 1e-7f; params.maximumMotion = 1e6f;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (!clearDepth(cb, input, clearValue)) return check(false, "Depth32Float clear pass creation");
        id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
        if (!encoder) return check(false, "compute encoder creation");
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:input atIndex:0];
        [encoder setBytes:&params length:sizeof params atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (!check(cb.status == MTLCommandBufferStatusCompleted, "production kernel command completed")) return false;
        const auto *samples = static_cast<const PGTDProbeGPUSample *>(output.contents);
        const auto &valid = samples[count / 2];
        if (background) {
            for (unsigned i = 0; i < count; ++i)
                if (!check(samples[i].depth == 0.0f && samples[i].flags == 0,
                           "cleared real depth is classified as background")) return false;
            return true;
        }
        if (!check(valid.depth == clearValue && (valid.flags & 3u) == 3u,
                   "real clear depth produced valid GPU motion")) return false;
        return check(std::fabs(valid.motionX - expectedMotion) < 1e-4 && std::fabs(valid.motionY) < 1e-4,
                     "identity/translation GPU motion matches expected pixel shift");
    };
    Mat4 same = identity();
    if (!run(same, 0.0, true, 0.0)) return false;
    if (!run(same, 0.5, false, 0.0)) return false;
    if (!run(same, 1.0, false, 0.0)) return false;
    Mat4 translated = identity(); translated.m[12] = 0.2;
    return run(translated, 0.5, false, 0.2 * width * 0.5);
}

static PGTDCamera cameraAt(double cameraX) {
    PGTDCamera camera{};
    camera.valid = 1; camera.width = 64; camera.height = 36;
    camera.nearPlane = 0.05f; camera.farPlane = 10000.0f;
    Mat4 view = identity(); view.m[12] = -cameraX;
    const Mat4 projection = reverseZProjection();
    for (unsigned i = 0; i < 16; ++i) {
        camera.view[i] = (float)view.m[i];
        camera.projection[i] = (float)projection.m[i];
    }
    return camera;
}

static PGTDMetalSample makeSample(uint64_t session, uint32_t stage, uint32_t opcode,
                                 const PGTDCamera &camera, double cameraX, uint32_t colorToken) {
    PGTDMetalSample s{};
    s.tag.session = session; s.tag.stage = stage; s.tag.camera = camera;
    s.opcode = opcode;
    s.depthToken = 900; s.depthFormat = MTLPixelFormatDepth32Float;
    s.depthWidth = camera.width; s.depthHeight = camera.height; s.depthSamples = 1;
    for (unsigned i = 0; i < 4; ++i) {
        s.colorToken[i] = colorToken + i; s.colorWidth[i] = camera.width;
        s.colorHeight[i] = camera.height; s.colorSamples[i] = 1;
    }
    if (opcode == 32) {
        s.flags = 2u; // renderer's verified WVP-present bit
        Mat4 view = identity(); view.m[12] = -cameraX;
        const Mat4 wvp = multiply(multiply(identity(), view), reverseZProjection());
        for (unsigned i = 0; i < 16; ++i) s.wvp[i] = (float)wvp.m[i];
    }
    return s;
}

static bool renderDiagnosticFrame(id<MTLCommandQueue> queue, uint64_t session,
                                  uint32_t frame, double cameraX, id<MTLTexture> depth,
                                  bool endBeforeCommit = false, bool clearActualDepth = true) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (clearActualDepth && !clearDepth(cb, depth, 0.5)) return check(false, "real depth clear encoder");
    const PGTDCamera camera = cameraAt(cameraX);
    PGTDProbeDrawState state{};
    state.depth = depth; state.depthCompare = MTLCompareFunctionGreater;
    state.depthWrite = true; state.fullViewport = true; state.worldKnown = true;
    const Mat4 world = identity();
    for (unsigned i = 0; i < 16; ++i) state.worldRows[i] = (float)world.m[i];

    auto sceneTargets = makeSample(session, 103, 26, camera, cameraX, 1000);
    PGTDProbeObserveSample(sceneTargets, state);
    auto sceneDraw = makeSample(session, 103, 32, camera, cameraX, 1000);
    PGTDProbeObserveSample(sceneDraw, state);
    PGTDTag boundary = sceneDraw.tag; boundary.stage = 105;
    if (!check(PGTDProbeNeedsBoundary(boundary, 26), "stage-105 boundary recognized")) return false;
    PGTDProbeEncodeBoundary(cb, boundary);
    auto uiTargets = makeSample(session, 105, 26, camera, cameraX, 2000);
    PGTDProbeObserveSample(uiTargets, state);
    auto hudDraw = makeSample(session, 100, 32, camera, cameraX, 2000);
    PGTDProbeObserveSample(hudDraw, state);

    if (endBeforeCommit) PGTDProbeEnd(session);
    [cb commit];
    PGTDProbeSourcePresent();
    [cb waitUntilCompleted];
    if (!check(cb.status == MTLCommandBufferStatusCompleted, "synthetic depth/probe command buffer completed")) return false;
    (void)frame;
    return true;
}

static bool runProductionModuleTests(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    id<MTLTexture> depthA = depthTexture(device, 64, 36);
    id<MTLTexture> depthB = depthTexture(device, 64, 36);
    if (!check(depthA != nil && depthB != nil, "private sampleable Depth32Float textures")) return false;

    constexpr uint64_t session = 0x1234;
    PGTDProbeBegin(session, true);
    if (!check(PGTDProbeRequested(), "explicit probe session enabled")) return false;
    uint32_t lastSourceFrame = 0, firstDispatchFrame = 0;
    NSDictionary *summary = nil;
    const auto pipelineDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Pipeline compilation is intentionally asynchronous; keep feeding bounded,
    // valid source frames until the real module schedules its first dispatch.
    // This is test-only waiting; production never waits for pipeline readiness.
    for (unsigned attempt = 0; attempt < 1200 && firstDispatchFrame == 0 &&
         std::chrono::steady_clock::now() < pipelineDeadline; ++attempt) {
        const uint32_t sourceFrame = lastSourceFrame + 1;
        if (!renderDiagnosticFrame(queue, session, sourceFrame, 0.005 * (sourceFrame - 1), depthA)) return false;
        lastSourceFrame = sourceFrame;
        summary = copySummaryOnUtilityQueue(session);
        if (!check(summary != nil, "probe summary returned on utility queue")) return false;
        if (integer(summary, @"scheduled") != 0) {
            NSArray *results = summary[@"frames"];
            if (results.count) firstDispatchFrame = [results[0][@"frame"] unsignedIntValue];
        }
        if (!firstDispatchFrame) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!check(firstDispatchFrame != 0, "real module scheduled a Metal probe within the bounded 10-second pipeline wait")) return false;

    // A resource change at the next scheduled sampling boundary must reset
    // history and reject the cross-resource reprojection.
    const uint32_t resourceChangeFrame = firstDispatchFrame + 5;
    while (lastSourceFrame + 1 < resourceChangeFrame) {
        ++lastSourceFrame;
        if (!renderDiagnosticFrame(queue, session, lastSourceFrame,
                                   0.005 * (lastSourceFrame - 1), depthA)) return false;
    }
    if (!renderDiagnosticFrame(queue, session, resourceChangeFrame,
                               0.005 * (resourceChangeFrame - 1), depthB)) return false;
    lastSourceFrame = resourceChangeFrame;
    summary = copySummaryOnUtilityQueue(session);
    NSDictionary *rejections = summary[@"rejections"];
    if (!check(integer(rejections, @"resource-change") >= 1 && integer(summary, @"historyResets") >= 1,
               "depth resource replacement resets history")) return false;

    // End while the next scheduled dispatch is still in flight. The callback
    // owns the session/buffer and summary drains it asynchronously (test-only).
    const uint32_t asyncEndFrame = firstDispatchFrame + 10;
    while (lastSourceFrame + 1 < asyncEndFrame) {
        ++lastSourceFrame;
        if (!renderDiagnosticFrame(queue, session, lastSourceFrame,
                                   0.005 * (lastSourceFrame - 1), depthB)) return false;
    }
    if (!renderDiagnosticFrame(queue, session, asyncEndFrame, 0.005 * (asyncEndFrame - 1), depthB, true)) return false;
    summary = copySummaryOnUtilityQueue(session);
    if (!check(summary != nil && !PGTDProbeRequested(), "explicit end stops future probe requests")) return false;
    if (!check([summary[@"partialFrame"] boolValue] && ![summary[@"readyForTemporal"] boolValue],
               "end-before-present is marked partial and never ready for Temporal")) return false;
    if (!check(integer(summary, @"scheduled") >= 2 && integer(summary, @"pending") == 0 &&
               integer(summary, @"completed") == integer(summary, @"scheduled"),
               "async end drains retained GPU work without pending slots")) return false;
    if (!check(integer(summary, @"cameraVerifiedFrames") >= 2, "known world/view/projection WVP verified across frames")) return false;
    if (!check(integer(summary, @"historyResets") >= 1, "history reset recorded")) return false;
    bool compared = false, moved = false, hud = false;
    for (NSDictionary *result in summary[@"frames"]) {
        if ([result[@"status"] isEqual:@"ok"]) {
            compared |= integer(result, @"cpuGpuComparedSamples") > 0;
            moved |= [result[@"maxMotionPixels"] doubleValue] > 1e-4;
            hud |= [result[@"hudAfterBoundary"] boolValue];
            if (!check(integer(result, @"cpuGpuMismatchSamples") == 0,
                       "production GPU kernel agrees with production CPU reference")) return false;
        }
    }
    if (!check(compared && moved && hud, "CPU/GPU agreement, camera translation, HUD-after-boundary observed")) return false;

    // Unsupported depth formats must not be sampled by the production module.
    constexpr uint64_t unsupportedSession = 0x1235;
    id<MTLTexture> colorInsteadOfDepth = texture(device, MTLPixelFormatR32Float, 64, 36,
        MTLStorageModeShared, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    if (!check(colorInsteadOfDepth != nil, "unsupported color-as-depth fixture")) return false;
    PGTDProbeBegin(unsupportedSession, true);
    if (!renderDiagnosticFrame(queue, unsupportedSession, 1, 0, colorInsteadOfDepth, false, false)) return false;
    PGTDProbeEnd(unsupportedSession);
    summary = copySummaryOnUtilityQueue(unsupportedSession);
    if (!check(integer(summary, @"scheduled") == 0 &&
               integer(summary[@"rejections"], @"no-main-draw") >= 1,
               "unsupported depth contract rejected without dispatch")) return false;

    // Disabled, stale-tag, and new-session isolation checks.
    PGTDProbeBegin(unsupportedSession + 1, false);
    if (!check(!PGTDProbeRequested(), "metadata-only mode leaves GPU probe disabled")) return false;
    PGTDTag stale{}; stale.session = unsupportedSession + 1; stale.stage = 105;
    PGTDProbeEnd(unsupportedSession + 1);
    PGTDProbeBegin(unsupportedSession + 2, true);
    if (!check(!PGTDProbeNeedsBoundary(stale, 26), "stale prior-session boundary ignored")) return false;
    PGTDProbeEnd(unsupportedSession + 2);
    NSDictionary *off = copySummaryOnUtilityQueue(unsupportedSession + 1);
    if (!check(off != nil && ![off[@"requested"] boolValue] && [off[@"mode"] isEqual:@"off"],
               "metadata-only summary reports probe off")) return false;
    return true;
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::puts("SKIP: no Metal device available for production camera probe regression.");
            return 77;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!check(queue != nil, "Metal command queue creation")) return 1;
        if (!directProductionKernelTest(device, queue)) return 1;
        if (!runProductionModuleTests(device, queue)) return 1;
        std::puts("temporal camera probe Metal regression passed");
        return 0;
    }
}
