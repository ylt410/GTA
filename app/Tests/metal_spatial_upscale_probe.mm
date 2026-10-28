#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <mach/mach_time.h>
#import <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double millisecondsSince(uint64_t start, uint64_t end) {
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return (double)(end - start) * (double)info.numer / (double)info.denom / 1.0e6;
}

static void writeReport(NSString *path, NSString *report) {
    NSError *error = nil;
    NSData *data = [report dataUsingEncoding:NSUTF8StringEncoding];
    if (![data writeToFile:path options:NSDataWritingAtomic error:&error]) {
        fprintf(stderr, "FAIL: cannot write report to %s: %s\n", path.fileSystemRepresentation,
                error.localizedDescription.UTF8String);
        exit(2);
    }
    fputs(report.UTF8String, stdout);
}

static int skip(NSString *path, NSString *reason) {
    writeReport(path, [NSString stringWithFormat:@"SKIPPED: %@\n", reason]);
    return 0;
}
static int fail(NSString *path, NSString *reason) {
    writeReport(path, [NSString stringWithFormat:@"FAIL: %@\n", reason]);
    return 1;
}

static id<MTLTexture> makeTexture(id<MTLDevice> device, NSUInteger width, NSUInteger height,
                                 MTLTextureUsage usage, MTLStorageMode storage) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:width height:height mipmapped:NO];
    descriptor.usage = usage;
    descriptor.storageMode = storage;
    return [device newTextureWithDescriptor:descriptor];
}

static BOOL validateReadback(const uint8_t *pixels, NSUInteger width, NSUInteger height,
                             NSUInteger bytesPerRow, NSString **failure) {
    if (!pixels || width != 1280 || height != 720 || bytesPerRow < width * 4) {
        *failure = @"readback dimensions or row stride are invalid";
        return NO;
    }
    const NSUInteger points[4][2] = {{width / 4, height / 4},
                                     {3 * width / 4, height / 4},
                                     {width / 4, 3 * height / 4},
                                     {3 * width / 4, 3 * height / 4}};
    NSUInteger nonBlack = 0;
    uint8_t samples[4][4];
    for (NSUInteger p = 0; p < 4; ++p) {
        const uint8_t *pixel = pixels + points[p][1] * bytesPerRow + points[p][0] * 4;
        memcpy(samples[p], pixel, 4);
        NSUInteger sum = (NSUInteger)pixel[0] + pixel[1] + pixel[2];
        if (sum > 90 && pixel[3] > 200) ++nonBlack;
    }
    // Input quadrants are red, green, blue, and white. BGRA readback is checked
    // coarsely so scaler filtering and quantization do not require pixel equality.
    if (nonBlack != 4 || samples[0][2] < 150 || samples[0][1] > 110 || samples[0][0] > 110 ||
        samples[1][1] < 150 || samples[1][2] > 110 || samples[1][0] > 110 ||
        samples[2][0] < 150 || samples[2][2] > 110 || samples[2][1] > 110 ||
        samples[3][0] < 135 || samples[3][1] < 135 || samples[3][2] < 135) {
        *failure = @"coarse color-pattern readback check failed";
        return NO;
    }
    return YES;
}

static NSString *runTrial(id<MTLDevice> device, id<MTLCommandQueue> queue,
                          id<MTLRenderPipelineState> pipeline, NSUInteger inputWidth,
                          NSUInteger inputHeight, NSString *label) {
    const NSUInteger outputWidth = 1280, outputHeight = 720;
    MTLFXSpatialScalerDescriptor *descriptor = [MTLFXSpatialScalerDescriptor new];
    descriptor.inputWidth = inputWidth;
    descriptor.inputHeight = inputHeight;
    descriptor.outputWidth = outputWidth;
    descriptor.outputHeight = outputHeight;
    descriptor.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
    id<MTLFXSpatialScaler> scaler = [descriptor newSpatialScalerWithDevice:device];
    if (!scaler) return [NSString stringWithFormat:@"FAIL %@: supported runtime could not create a spatial scaler\n", label];

    id<MTLTexture> input = makeTexture(device, inputWidth, inputHeight,
        MTLTextureUsageRenderTarget | scaler.colorTextureUsage, MTLStorageModeShared);
    id<MTLTexture> output = makeTexture(device, outputWidth, outputHeight,
        scaler.outputTextureUsage, MTLStorageModePrivate);
    id<MTLTexture> readback = makeTexture(device, outputWidth, outputHeight,
        MTLTextureUsageShaderRead, MTLStorageModeShared);
    if (!input || !output || !readback)
        return [NSString stringWithFormat:@"FAIL %@: texture allocation failed\n", label];

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = input;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> command = [queue commandBuffer];
    if (!command) return [NSString stringWithFormat:@"FAIL %@: command buffer allocation failed\n", label];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return [NSString stringWithFormat:@"FAIL %@: source render encoder creation failed\n", label];
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    scaler.colorTexture = input;
    scaler.outputTexture = output;
    scaler.inputContentWidth = inputWidth;
    scaler.inputContentHeight = inputHeight;
    uint64_t encodeStart = mach_absolute_time();
    [scaler encodeToCommandBuffer:command];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(outputWidth, outputHeight, 1)
                 toTexture:readback destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    uint64_t encodeEnd = mach_absolute_time();
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted)
        return [NSString stringWithFormat:@"FAIL %@: GPU command buffer status %lu (%s)\n", label,
                (unsigned long)command.status, command.error.localizedDescription.UTF8String ?: "no error detail"];

    const NSUInteger bytesPerRow = outputWidth * 4;
    NSMutableData *bytes = [NSMutableData dataWithLength:bytesPerRow * outputHeight];
    [readback getBytes:bytes.mutableBytes bytesPerRow:bytesPerRow
             fromRegion:MTLRegionMake2D(0, 0, outputWidth, outputHeight) mipmapLevel:0];
    NSString *validationFailure = nil;
    if (!validateReadback((const uint8_t *)bytes.bytes, outputWidth, outputHeight,
                          bytesPerRow, &validationFailure))
        return [NSString stringWithFormat:@"FAIL %@: %@\n", label, validationFailure];

    double gpuMs = NAN;
    if (command.GPUStartTime > 0 && command.GPUEndTime > command.GPUStartTime)
        gpuMs = (command.GPUEndTime - command.GPUStartTime) * 1000.0;
    double ratioX = (double)inputWidth / outputWidth;
    double ratioY = (double)inputHeight / outputHeight;
    return [NSString stringWithFormat:
        @"PASS %@ input=%lux%lu output=%lux%lu ratio=%.3fx%.3f colorMode=perceptual outputBytes=%lu hostEncodeMs=%.3f gpuCommandBufferMs=%@ scope=MetalFX-spatial-scaler-plus-source-draw-and-readback-copy\n",
        label, (unsigned long)inputWidth, (unsigned long)inputHeight,
        (unsigned long)outputWidth, (unsigned long)outputHeight, ratioX, ratioY,
        (unsigned long)bytes.length, millisecondsSince(encodeStart, encodeEnd),
        isfinite(gpuMs) ? [NSString stringWithFormat:@"%.3f", gpuMs] : @"unavailable"];
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSString *outputPath = nil;
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) outputPath = [NSString stringWithUTF8String:argv[++i]];
            else {
                fprintf(stderr, "Usage: %s --output PATH\n", argv[0]);
                return 2;
            }
        }
        if (!outputPath) {
            fprintf(stderr, "Usage: %s --output PATH\n", argv[0]);
            return 2;
        }
        if (@available(macOS 13.0, *)) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) return skip(outputPath, @"Metal device unavailable");
            if (![MTLFXSpatialScalerDescriptor supportsDevice:device])
                return skip(outputPath, [NSString stringWithFormat:@"MetalFX spatial scaling unsupported by %@", device.name]);
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!queue) return fail(outputPath, @"Metal command queue unavailable");

            NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "struct V { float4 position [[position]]; float2 uv; };\n"
                "vertex V vmain(uint id [[vertex_id]]) { constexpr float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; V o; o.position=float4(p[id],0,1); o.uv=float2((p[id].x+1)*0.5,1-(p[id].y+1)*0.5); return o; }\n"
                "fragment float4 fmain(V in [[stage_in]]) { float2 uv=in.uv; bool right=uv.x>=0.5; bool bottom=uv.y>=0.5; float3 c = bottom ? (right ? float3(1,1,1) : float3(0,0,1)) : (right ? float3(0,1,0) : float3(1,0,0)); float checker=fmod(floor(uv.x*40)+floor(uv.y*24),2.0); c *= mix(0.82,1.0,checker); c += float3(uv.x,uv.y,0)*0.05; return float4(c,1); }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (!library) return fail(outputPath, [NSString stringWithFormat:@"Metal source-pattern compile failed: %@", error.localizedDescription ?: @"unknown error"]);
            MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
            pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"vmain"];
            pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"fmain"];
            pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
            if (!pipeline) return fail(outputPath, [NSString stringWithFormat:@"Metal source-pattern pipeline failed: %@", error.localizedDescription ?: @"unknown error"]);

            NSArray<NSString *> *trials = @[
                runTrial(device, queue, pipeline, 986, 554, @"quality"),
                runTrial(device, queue, pipeline, 858, 482, @"balanced"),
                runTrial(device, queue, pipeline, 640, 360, @"performance")];
            NSMutableString *report = [NSMutableString stringWithString:
                @"MetalFX spatial upscale probe; fixed output 1280x720; synthetic procedural source; three bounded trials.\n"];
            [report appendString:@"Host timing covers scaler encode plus readback-copy encode; GPU timing covers the command buffer, including source draw, scaler, and copy. This measures the host scaler path only, not the game, FPS, or power.\n"];
            for (NSString *trial in trials) [report appendString:trial];
            BOOL failed = NO;
            for (NSString *trial in trials) if ([trial hasPrefix:@"FAIL "]) failed = YES;
            writeReport(outputPath, report);
            return failed ? 1 : 0;
        }
        return skip(outputPath, @"MetalFX spatial scaling requires macOS 13 or newer");
    }
}
