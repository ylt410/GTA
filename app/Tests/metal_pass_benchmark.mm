// Host Metal microbenchmark for repeated identical SET_RENDER_TARGETS.
// This is deliberately an isolated render-pass workload, NOT GTA V or an
// iPhone FPS measurement. Both paths execute the same draws and verify every
// output byte. A missing Metal device is reported as SKIPPED, not a saving.
// Build on macOS:
// xcrun -sdk macosx clang++ -std=c++17 -fobjc-arc metal_pass_benchmark.mm \
//     -framework Foundation -framework Metal -o metal_pass_benchmark
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Source/MetalRenderer/MRRenderTargetCache.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>
#include <time.h>
#include <vector>

namespace {
constexpr NSUInteger Width = 1280, Height = 720, Draws = 256;
constexpr unsigned Warmups = 4, Trials = 9;
constexpr NSUInteger RowBytes = Width * 4;
struct Color { uint32_t tex, mip, slice, fmt; };
struct Depth { uint32_t tex, mip, slice, fmt, flags; };
struct Sample {
	double cpuMs = 0, gpuMs = std::numeric_limits<double>::quiet_NaN();
	uint64_t allocatedBefore = 0, allocatedEncoded = 0, allocatedCompleted = 0;
	unsigned passes = 0;
	std::vector<uint8_t> output;
};

static double threadMilliseconds() {
	struct timespec value{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0)
		throw std::runtime_error("The CPU encoding thread clock is unavailable.");
	return value.tv_sec * 1000.0 + value.tv_nsec / 1e6;
}

static void complete(id<MTLCommandBuffer> command) {
	if (!command) throw std::runtime_error("Metal did not create a command buffer.");
	[command commit]; [command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted)
		throw std::runtime_error(command.error.localizedDescription.UTF8String ?: "Metal command buffer failed.");
}

static id<MTLTexture> makeTarget(id<MTLDevice> device) {
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		width:Width height:Height mipmapped:NO];
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
	if (!texture) throw std::runtime_error("Metal could not allocate the benchmark target.");
	return texture;
}

static Sample run(id<MTLDevice> device, id<MTLCommandQueue> queue,
		  id<MTLRenderPipelineState> pipeline, id<MTLTexture> target,
		  id<MTLBuffer> readback, bool cached) {
	Sample result;
	result.allocatedBefore = device.currentAllocatedSize;
	id<MTLCommandBuffer> command = [queue commandBuffer];
	if (!command) throw std::runtime_error("Metal did not create an encoding command buffer.");
	Color colors[8] = {};
	Depth depth{};
	std::array<uint32_t, 37> packet{};
	packet[0] = 1; packet[3] = MTLPixelFormatRGBA8Unorm;
	bool targetsDirty = false, pipelineDirty = false;
	id<MTLRenderCommandEncoder> encoder = nil;
	double start = threadMilliseconds();
	for (NSUInteger draw = 0; draw < Draws; draw++) {
		if (cached) {
			mr::SetRenderTargetBindings(colors, depth, packet.data(), targetsDirty, pipelineDirty);
		} else {
			// The previous renderer assigned every field and dirtied the pass
			// even when all incoming bindings were exactly the same.
			for (unsigned i = 0; i < 8; i++) {
				const uint32_t *p = packet.data() + i * 4;
				colors[i] = {p[0], p[1], p[2], p[3]};
			}
			const uint32_t *p = packet.data() + 32;
			depth = {p[0], p[1], p[2], p[3], p[4]};
			targetsDirty = pipelineDirty = true;
		}
		if (targetsDirty || !encoder) {
			[encoder endEncoding];
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = target;
			pass.colorAttachments[0].loadAction = result.passes ? MTLLoadActionLoad : MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
			encoder = [command renderCommandEncoderWithDescriptor:pass];
			if (!encoder) throw std::runtime_error("Metal did not create a render encoder.");
			[encoder setRenderPipelineState:pipeline];
			targetsDirty = pipelineDirty = false;
			result.passes++;
		}
		// Adjacent strips cover the entire image. Every draw contributes a
		// distinct strip, so omitting a draw changes the validated output.
		NSUInteger left = draw * Width / Draws, right = (draw + 1) * Width / Draws;
		[encoder setScissorRect:(MTLScissorRect){left, 0, right - left, Height}];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	}
	[encoder endEncoding];
	result.cpuMs = threadMilliseconds() - start; // excludes commit, waits and readback
	result.allocatedEncoded = device.currentAllocatedSize;
	complete(command);
	result.allocatedCompleted = device.currentAllocatedSize;
	double gpuStart = command.GPUStartTime, gpuEnd = command.GPUEndTime;
	if (std::isfinite(gpuStart) && std::isfinite(gpuEnd) && gpuStart > 0 && gpuEnd > gpuStart)
		result.gpuMs = (gpuEnd - gpuStart) * 1000.0;
	// Read back in a separate command buffer so validation is outside both
	// the CPU encoding time and this sample's measured GPU interval.
	id<MTLCommandBuffer> copy = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
	if (!blit) throw std::runtime_error("Metal did not create the readback encoder.");
	[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(Width, Height, 1) toBuffer:readback destinationOffset:0
		destinationBytesPerRow:RowBytes destinationBytesPerImage:RowBytes * Height];
	[blit endEncoding]; complete(copy);
	result.output.resize(RowBytes * Height);
	std::memcpy(result.output.data(), readback.contents, result.output.size());
	for (NSUInteger pixel = 0; pixel < Width * Height; pixel++) {
		const uint8_t *value = result.output.data() + pixel * 4;
		if (!value[0] || !value[1] || !value[2] || value[3] != 255)
			throw std::runtime_error("Benchmark output is incomplete or invalid.");
	}
	if (result.passes != (cached ? 1 : Draws))
		throw std::runtime_error("Unexpected render-pass count.");
	return result;
}

static double median(std::vector<double> values) {
	if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}
static id value(double number) { return std::isfinite(number) ? @(number) : NSNull.null; }
static double change(double before, double after) {
	return before > 0 && std::isfinite(before) && std::isfinite(after)
		? (before - after) * 100.0 / before : std::numeric_limits<double>::quiet_NaN();
}
static NSDictionary *summary(const std::vector<Sample> &samples) {
	std::vector<double> cpu, gpu, before, encoded, completed;
	NSMutableArray *individual = [NSMutableArray array];
	for (const Sample &sample : samples) {
		cpu.push_back(sample.cpuMs);
		if (std::isfinite(sample.gpuMs)) gpu.push_back(sample.gpuMs);
		before.push_back(sample.allocatedBefore); encoded.push_back(sample.allocatedEncoded);
		completed.push_back(sample.allocatedCompleted);
		[individual addObject:@{@"cpu_encoding_thread_ms": @(sample.cpuMs),
			@"gpu_command_buffer_ms": value(sample.gpuMs), @"render_passes": @(sample.passes),
			@"metal_allocated_before_bytes": @(sample.allocatedBefore),
			@"metal_allocated_after_encoding_bytes": @(sample.allocatedEncoded),
			@"metal_allocated_after_completion_bytes": @(sample.allocatedCompleted)}];
	}
	return @{@"cpu_encoding_thread_ms_median": value(median(cpu)),
		@"gpu_command_buffer_ms_median": value(median(gpu)), @"gpu_timing_sample_count": @(gpu.size()),
		@"metal_allocated_before_bytes_median": value(median(before)),
		@"metal_allocated_after_encoding_bytes_median": value(median(encoded)),
		@"metal_allocated_after_completion_bytes_median": value(median(completed)), @"samples": individual};
}
static uint64_t checksum(const std::vector<uint8_t> &bytes) {
	uint64_t hash = 14695981039346656037ull;
	for (uint8_t byte : bytes) hash = (hash ^ byte) * 1099511628211ull;
	return hash;
}
static NSDictionary *benchmark() {
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	struct utsname platform{}; uname(&platform);
	NSDictionary *environment = @{@"os": NSProcessInfo.processInfo.operatingSystemVersionString,
		@"architecture": [NSString stringWithUTF8String:platform.machine],
		@"logical_cpu_count": @(NSProcessInfo.processInfo.activeProcessorCount),
		@"host_physical_memory_bytes": @(NSProcessInfo.processInfo.physicalMemory),
		@"metal_device": device.name ?: @"none"};
	if (!device) return @{@"status": @"SKIPPED", @"reason": @"No Metal device is available on this host.",
		@"environment": environment, @"gpu_timing_status": @"UNAVAILABLE",
		@"cpu_encoding_thread_reduction_percent": NSNull.null,
		@"gpu_command_buffer_reduction_percent": NSNull.null,
		@"metal_allocated_after_encoding_reduction_percent": NSNull.null};
	NSError *error = nil;
	NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
		"vertex float4 vertex_main(uint i [[vertex_id]]) { float2 p = i == 0 ? float2(-1,-1) : (i == 1 ? float2(3,-1) : float2(-1,3)); return float4(p,0,1); }\n"
		"fragment float4 fragment_main() { return float4(.25,.5,.75,1); }\n";
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	if (!library) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Metal shader compilation failed.");
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
	if (!pipeline) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Metal pipeline creation failed.");
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLTexture> baselineTarget = makeTarget(device), optimizedTarget = makeTarget(device);
	id<MTLBuffer> readback = [device newBufferWithLength:RowBytes * Height options:MTLResourceStorageModeShared];
	if (!queue || !readback) throw std::runtime_error("Metal benchmark resources are unavailable.");
	std::vector<Sample> baseline, optimized;
	uint64_t outputHash = 0;
	for (unsigned trial = 0; trial < Warmups + Trials; trial++) {
		Sample oldSample, newSample;
		// Alternating order reduces a consistent warm-cache/thermal advantage.
		if (trial & 1) {
			@autoreleasepool { newSample = run(device, queue, pipeline, optimizedTarget, readback, true); }
			@autoreleasepool { oldSample = run(device, queue, pipeline, baselineTarget, readback, false); }
		} else {
			@autoreleasepool { oldSample = run(device, queue, pipeline, baselineTarget, readback, false); }
			@autoreleasepool { newSample = run(device, queue, pipeline, optimizedTarget, readback, true); }
		}
		if (oldSample.output != newSample.output)
			throw std::runtime_error("Baseline and optimized Metal readbacks differ.");
		uint64_t hash = checksum(oldSample.output);
		if (trial && hash != outputHash) throw std::runtime_error("Metal output changed between trials.");
		outputHash = hash;
		oldSample.output.clear(); oldSample.output.shrink_to_fit();
		newSample.output.clear(); newSample.output.shrink_to_fit();
		if (trial >= Warmups) { baseline.push_back(std::move(oldSample)); optimized.push_back(std::move(newSample)); }
	}
	NSDictionary *oldResult = summary(baseline), *newResult = summary(optimized);
	id oldGPU = oldResult[@"gpu_command_buffer_ms_median"], newGPU = newResult[@"gpu_command_buffer_ms_median"];
	bool allTimestamps = [oldResult[@"gpu_timing_sample_count"] unsignedIntValue] == Trials &&
		[newResult[@"gpu_timing_sample_count"] unsignedIntValue] == Trials;
	double gpuChange = !allTimestamps || oldGPU == NSNull.null || newGPU == NSNull.null ? std::numeric_limits<double>::quiet_NaN()
		: change([oldGPU doubleValue], [newGPU doubleValue]);
	return @{@"status": @"PASS", @"benchmark": @"identical_render_target_rebind_microbenchmark",
		@"scope": @"Host Metal only: repeated identical target bindings; not full-game or iPhone performance.",
		@"environment": environment, @"width": @(Width), @"height": @(Height), @"draws_per_trial": @(Draws),
		@"warmup_pairs": @(Warmups), @"measured_pairs": @(Trials), @"alternating_order": @YES,
		@"readback_equal_every_pair": @YES, @"readback_fnv1a64": [NSString stringWithFormat:@"%016llx", (unsigned long long)outputHash],
		@"baseline": oldResult, @"optimized": newResult,
		@"gpu_timing_status": allTimestamps ? @"AVAILABLE" : @"UNAVAILABLE",
		@"cpu_encoding_thread_reduction_percent": value(change([oldResult[@"cpu_encoding_thread_ms_median"] doubleValue], [newResult[@"cpu_encoding_thread_ms_median"] doubleValue])),
		@"gpu_command_buffer_reduction_percent": value(gpuChange),
		@"metal_allocated_after_encoding_reduction_percent": value(change([oldResult[@"metal_allocated_after_encoding_bytes_median"] doubleValue], [newResult[@"metal_allocated_after_encoding_bytes_median"] doubleValue])),
		@"memory_measurement": @"MTLDevice.currentAllocatedSize snapshots include shared benchmark resources; these are not process memory or peak allocation measurements.",
		@"cpu_measurement": @"CLOCK_THREAD_CPUTIME_ID around encoding only; excludes submission, GPU waits and validation.",
		@"gpu_measurement": @"Completed command-buffer GPUEndTime minus GPUStartTime; separate readback command buffers are excluded. Null means timestamps unavailable."};
}
} // namespace

int main(int argc, char **argv) {
	@autoreleasepool {
		NSString *output = nil;
		if (argc == 3 && std::strcmp(argv[1], "--output") == 0) output = [NSString stringWithUTF8String:argv[2]];
		else if (argc != 1) { std::fprintf(stderr, "Usage: metal_pass_benchmark [--output report.json]\n"); return 2; }
		NSDictionary *report;
		int result = 0;
		try { report = benchmark(); }
		catch (const std::exception &error) {
			report = @{@"status": @"FAIL", @"reason": [NSString stringWithUTF8String:error.what()]}; result = 1;
		}
		NSError *error = nil;
		NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
		if (!json) { std::fprintf(stderr, "Could not encode benchmark report.\n"); return 1; }
		if (output && ![json writeToFile:output options:NSDataWritingAtomic error:&error]) {
			std::fprintf(stderr, "Could not write benchmark report.\n"); return 1;
		}
		std::fwrite(json.bytes, 1, json.length, stdout); std::putchar('\n');
		return result;
	}
}
