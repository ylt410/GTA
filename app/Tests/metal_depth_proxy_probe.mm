// Optional host Metal probe for demanded-range depth-proxy copies. This is a
// renderer-subsystem workload only; it does not measure GTA V or iPhone FPS.
// Build: xcrun --sdk macosx clang++ -std=c++17 -fobjc-arc \
//   -framework Foundation -framework Metal metal_depth_proxy_probe.mm -o probe
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Source/MetalRenderer/MRDepthProxyRange.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>

namespace {
constexpr NSUInteger Width = 256, Height = 256;
constexpr uint32_t Mips = 4, Layers = 12;
constexpr unsigned Warmups = 1, Trials = 3;
constexpr uint32_t FractionBits = 24;

static uint32_t mipWidth(uint32_t mip) { return std::max(1u, (uint32_t)Width >> mip); }
static uint32_t mipHeight(uint32_t mip) { return std::max(1u, (uint32_t)Height >> mip); }
static double threadMs() {
	struct timespec ts{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
		throw std::runtime_error("Thread CPU clock is unavailable.");
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static void complete(id<MTLCommandBuffer> command) {
	if (!command) throw std::runtime_error("Metal did not create a command buffer.");
	[command commit]; [command waitUntilCompleted];
	if (command.status != MTLCommandBufferStatusCompleted)
		throw std::runtime_error(command.error.localizedDescription.UTF8String ?: "Metal command buffer failed.");
}
static id<MTLTexture> makeTexture(id<MTLDevice> device, MTLPixelFormat format,
									 MTLTextureType type, NSUInteger width, NSUInteger height,
									 uint32_t mips, uint32_t layers, MTLTextureUsage usage,
									 MTLStorageMode storage) {
		MTLTextureDescriptor *d = [MTLTextureDescriptor new];
	d.pixelFormat = format; d.textureType = type; d.width = width; d.height = height;
	d.mipmapLevelCount = mips; d.arrayLength = layers; d.usage = usage; d.storageMode = storage;
	id<MTLTexture> t = [device newTextureWithDescriptor:d];
	if (!t) throw std::runtime_error("Metal could not allocate a probe texture.");
	return t;
}
static id<MTLTexture> subresourceView(id<MTLTexture> texture, MTLPixelFormat format,
										 uint32_t mip, uint32_t layer) {
	id<MTLTexture> view = [texture newTextureViewWithPixelFormat:format textureType:MTLTextureType2D
		levels:NSMakeRange(mip, 1) slices:NSMakeRange(layer, 1)];
	if (!view) throw std::runtime_error("Metal could not create a 2D subresource view.");
	return view;
}

struct Resources {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLTexture> source, proxy, readTarget;
	id<MTLRenderPipelineState> copyPipeline, readPipeline;
	id<MTLDepthStencilState> depthState;
	std::vector<id<MTLTexture>> sourceViews, proxyViews;
	std::vector<std::vector<float>> sourcePixels;
	std::vector<NSUInteger> readOffsets;
	NSUInteger readbackBytes = 0;
	id<MTLBuffer> readback;
	uint64_t sourceTexels = 0;
};

static Resources makeResources(id<MTLDevice> device) {
	Resources r{}; r.device = device; r.queue = [device newCommandQueue];
	if (!r.queue) throw std::runtime_error("Metal could not create a command queue.");
	r.source = makeTexture(device, MTLPixelFormatR32Float, MTLTextureType2DArray,
	                       Width, Height, Mips, Layers, MTLTextureUsageShaderRead, MTLStorageModeShared);
	r.proxy = makeTexture(device, MTLPixelFormatDepth32Float, MTLTextureType2DArray,
	                      Width, Height, Mips, Layers, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget,
	                      MTLStorageModePrivate);
	r.readTarget = makeTexture(device, MTLPixelFormatRGBA32Float, MTLTextureType2D,
	                           Width, Height, Mips, 1, MTLTextureUsageRenderTarget, MTLStorageModePrivate);
	r.sourceViews.resize((size_t)Mips * Layers);
	r.proxyViews.resize((size_t)Mips * Layers);
	for (uint32_t mip = 0; mip < Mips; ++mip) for (uint32_t layer = 0; layer < Layers; ++layer) {
		const size_t index = (size_t)mip * Layers + layer;
		r.sourceViews[index] = subresourceView(r.source, MTLPixelFormatR32Float, mip, layer);
		r.proxyViews[index] = subresourceView(r.proxy, MTLPixelFormatDepth32Float, mip, layer);
	}

	r.sourcePixels.resize((size_t)Mips * Layers);
	uint32_t ordinal = 1;
	for (uint32_t mip = 0; mip < Mips; ++mip) {
		const uint32_t w = mipWidth(mip), h = mipHeight(mip);
		for (uint32_t layer = 0; layer < Layers; ++layer) {
			auto &pixels = r.sourcePixels[(size_t)mip * Layers + layer];
			pixels.resize((size_t)w * h);
			for (float &pixel : pixels) pixel = (float)ordinal++ * (1.0f / (1u << FractionBits));
			[r.source replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:mip slice:layer
				withBytes:pixels.data() bytesPerRow:(NSUInteger)w * sizeof(float) bytesPerImage:0];
			r.sourceTexels += pixels.size();
		}
	}
	if (ordinal >= (1u << FractionBits)) throw std::runtime_error("Probe source pattern exceeded exact binary-fraction range.");

	NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
	"struct V { float4 p [[position]]; };\n"
	"vertex V vertex_main(uint i [[vertex_id]]) { float2 p=i==0?float2(-1,-1):(i==1?float2(3,-1):float2(-1,3)); V o; o.p=float4(p,0,1); return o; }\n"
	"struct D { float z [[depth(any)]]; };\n"
	"fragment D copy_depth(V in [[stage_in]], texture2d<float> src [[texture(0)]]) { D o; o.z=src.read(uint2(in.p.xy)).x; return o; }\n"
	"fragment float4 read_depth(V in [[stage_in]], depth2d<float> src [[texture(0)]]) { float z=src.read(uint2(in.p.xy)); return float4(z,z,z,1); }\n";
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:msl options:nil error:&error];
	if (!library) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Metal shader compilation failed.");
	MTLRenderPipelineDescriptor *copy = [MTLRenderPipelineDescriptor new];
	copy.vertexFunction = [library newFunctionWithName:@"vertex_main"];
	copy.fragmentFunction = [library newFunctionWithName:@"copy_depth"];
	copy.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	r.copyPipeline = [device newRenderPipelineStateWithDescriptor:copy error:&error];
	if (!r.copyPipeline) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth-copy pipeline creation failed.");
	MTLRenderPipelineDescriptor *read = [MTLRenderPipelineDescriptor new];
	read.vertexFunction = [library newFunctionWithName:@"vertex_main"];
	read.fragmentFunction = [library newFunctionWithName:@"read_depth"];
	read.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
	r.readPipeline = [device newRenderPipelineStateWithDescriptor:read error:&error];
	if (!r.readPipeline) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth-read pipeline creation failed.");
	MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
	ds.depthCompareFunction = MTLCompareFunctionAlways; ds.depthWriteEnabled = YES;
	r.depthState = [device newDepthStencilStateWithDescriptor:ds];
	if (!r.depthState) throw std::runtime_error("Metal could not create a depth-write state.");

	r.readOffsets.resize((size_t)Mips * Layers);
	NSUInteger offset = 0;
	for (uint32_t mip = 0; mip < Mips; ++mip) {
		NSUInteger row = ((NSUInteger)mipWidth(mip) * 16 + 255) & ~255u;
		NSUInteger image = row * mipHeight(mip);
		for (uint32_t layer = 0; layer < Layers; ++layer) {
			r.readOffsets[(size_t)mip * Layers + layer] = offset;
			offset += image;
		}
	}
	r.readbackBytes = offset;
	r.readback = [device newBufferWithLength:offset options:MTLResourceStorageModeShared];
	if (!r.readback) throw std::runtime_error("Metal could not allocate the bounded readback buffer.");
	return r;
}

struct Timing { double cpuMs = NAN, gpuMs = NAN; uint32_t subresources = 0; };
static Timing encodeCopies(Resources &r, const mr::DepthProxyRange &demand, bool whole) {
	id<MTLCommandBuffer> cb = [r.queue commandBuffer];
	if (!cb) throw std::runtime_error("Metal did not create a timed command buffer.");
	Timing result;
	const double start = threadMs();
	for (uint32_t mip = 0; mip < Mips; ++mip) for (uint32_t layer = 0; layer < Layers; ++layer) {
		if (!whole && !(mip >= demand.baseMip && mip < demand.baseMip + demand.mipCount &&
		                layer >= demand.firstLayer && layer < demand.firstLayer + demand.layerCount)) continue;
		id<MTLTexture> src = r.sourceViews[(size_t)mip * Layers + layer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.depthAttachment.texture = r.proxy; pass.depthAttachment.level = mip; pass.depthAttachment.slice = layer;
		pass.depthAttachment.loadAction = MTLLoadActionDontCare; pass.depthAttachment.storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
		if (!encoder) throw std::runtime_error("Metal did not create a depth-copy encoder.");
		[encoder setRenderPipelineState:r.copyPipeline]; [encoder setDepthStencilState:r.depthState];
		[encoder setFragmentTexture:src atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];
		result.subresources++;
	}
	const double cpuMs = threadMs() - start;
	complete(cb);
	result.cpuMs = cpuMs;
	if (std::isfinite(cb.GPUStartTime) && std::isfinite(cb.GPUEndTime) && cb.GPUStartTime > 0 && cb.GPUEndTime > cb.GPUStartTime)
		result.gpuMs = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
	return result;
}

static std::vector<float> validateRange(Resources &r, const mr::DepthProxyRange &range) {
	id<MTLCommandBuffer> cb = [r.queue commandBuffer];
	if (!cb) throw std::runtime_error("Metal did not create a validation command buffer.");
	for (uint32_t mip = range.baseMip; mip < range.baseMip + range.mipCount; ++mip) {
		for (uint32_t layer = range.firstLayer; layer < range.firstLayer + range.layerCount; ++layer) {
			auto depth = r.proxyViews[(size_t)mip * Layers + layer];
			MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
			pass.colorAttachments[0].texture = r.readTarget; pass.colorAttachments[0].level = mip;
			pass.colorAttachments[0].loadAction = MTLLoadActionDontCare; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
			if (!render) throw std::runtime_error("Metal did not create a validation render encoder.");
			[render setRenderPipelineState:r.readPipeline]; [render setFragmentTexture:depth atIndex:0];
			[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [render endEncoding];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			if (!blit) throw std::runtime_error("Metal did not create a validation readback encoder.");
			const NSUInteger row = ((NSUInteger)mipWidth(mip) * 16 + 255) & ~255u;
			[blit copyFromTexture:r.readTarget sourceSlice:0 sourceLevel:mip sourceOrigin:MTLOriginMake(0, 0, 0)
				sourceSize:MTLSizeMake(mipWidth(mip), mipHeight(mip), 1) toBuffer:r.readback
				destinationOffset:r.readOffsets[(size_t)mip * Layers + layer]
				destinationBytesPerRow:row destinationBytesPerImage:row * mipHeight(mip)];
			[blit endEncoding];
		}
	}
	complete(cb);
	std::vector<float> rgba;
	for (uint32_t mip = range.baseMip; mip < range.baseMip + range.mipCount; ++mip) {
		const uint32_t w = mipWidth(mip), h = mipHeight(mip);
		const NSUInteger row = ((NSUInteger)w * 16 + 255) & ~255u;
		for (uint32_t layer = range.firstLayer; layer < range.firstLayer + range.layerCount; ++layer) {
			const uint8_t *base = (const uint8_t *)r.readback.contents + r.readOffsets[(size_t)mip * Layers + layer];
			for (uint32_t y = 0; y < h; ++y) {
				const float *pixels = (const float *)(base + (NSUInteger)y * row);
				rgba.insert(rgba.end(), pixels, pixels + (size_t)w * 4);
			}
		}
	}
	return rgba;
}

static void validatePixels(const Resources &r, const mr::DepthProxyRange &range,
							  const std::vector<float> &pixels) {
	size_t at = 0;
	for (uint32_t mip = range.baseMip; mip < range.baseMip + range.mipCount; ++mip) {
		const uint32_t w = mipWidth(mip), h = mipHeight(mip);
		for (uint32_t layer = range.firstLayer; layer < range.firstLayer + range.layerCount; ++layer) {
			const auto &source = r.sourcePixels[(size_t)mip * Layers + layer];
			for (uint32_t i = 0; i < w * h; ++i) {
				if (pixels[at] != source[i] || pixels[at + 1] != source[i] ||
				    pixels[at + 2] != source[i] || pixels[at + 3] != 1.0f)
					throw std::runtime_error("Readback pixels differ from the deterministic source fixture.");
				at += 4;
			}
		}
	}
	if (at != pixels.size()) throw std::runtime_error("Validation pixel count is inconsistent.");
}

static double median(std::vector<double> values) {
	if (values.empty()) return NAN;
	std::sort(values.begin(), values.end());
	return values[values.size() / 2];
}
static id number(double value) { return std::isfinite(value) ? @(value) : NSNull.null; }
static void poisonProxy(Resources &r) {
	// Validation must not inherit a correct image from the whole-copy run.
	// Every expected fixture pixel is nonzero, so a missed candidate plane
	// remains zero and fails the independent source comparison.
	id<MTLCommandBuffer> cb = [r.queue commandBuffer];
	for (uint32_t mip = 0; mip < Mips; ++mip) for (uint32_t layer = 0; layer < Layers; ++layer) {
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.depthAttachment.texture = r.proxy; pass.depthAttachment.level = mip; pass.depthAttachment.slice = layer;
		pass.depthAttachment.loadAction = MTLLoadActionClear; pass.depthAttachment.clearDepth = 0;
		pass.depthAttachment.storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
		if (!encoder) throw std::runtime_error("Could not poison proxy before candidate validation.");
		[encoder endEncoding];
	}
	complete(cb);
}
static NSDictionary *scenario(Resources &r, NSString *name, const mr::DepthProxyRange &demand) {
	std::vector<double> oldCpu, oldGpu, newCpu, newGpu;
	uint64_t oldSubresources = 0, demandedSubresources = 0;
	for (unsigned trial = 0; trial < Warmups + Trials; ++trial) {
		Timing old{}, optimized{};
		if (trial & 1) { optimized = encodeCopies(r, demand, false); old = encodeCopies(r, demand, true); }
		else { old = encodeCopies(r, demand, true); optimized = encodeCopies(r, demand, false); }
		if (trial >= Warmups) {
			oldCpu.push_back(old.cpuMs); newCpu.push_back(optimized.cpuMs);
			oldSubresources += old.subresources; demandedSubresources += optimized.subresources;
			if (std::isfinite(old.gpuMs)) oldGpu.push_back(old.gpuMs);
			if (std::isfinite(optimized.gpuMs)) newGpu.push_back(optimized.gpuMs);
		}
	}
	encodeCopies(r, demand, true);
	const std::vector<float> baseline = validateRange(r, demand);
	poisonProxy(r);
	encodeCopies(r, demand, false);
	const std::vector<float> demanded = validateRange(r, demand);
	validatePixels(r, demand, baseline); validatePixels(r, demand, demanded);
	if (baseline != demanded) throw std::runtime_error("Whole-copy and demanded-range proxy outputs differ.");
	const double oldTexels = (double)r.sourceTexels;
	double demandedTexels = 0;
	for (uint32_t mip = demand.baseMip; mip < demand.baseMip + demand.mipCount; ++mip)
		demandedTexels += (double)mipWidth(mip) * mipHeight(mip) * demand.layerCount;
	const double oldCpuMs = median(oldCpu), newCpuMs = median(newCpu);
	const double oldGpuMs = median(oldGpu), newGpuMs = median(newGpu);
	return @{@"name": name, @"status": @"PASS", @"range_valid": @(demand.valid),
		@"requested_mips": @(demand.mipCount), @"requested_layers": @(demand.layerCount),
		@"source_fixture_pixels_checked_per_path": @(baseline.size() / 4),
		@"baseline_copy_subresources_per_trial": @((double)oldSubresources / Trials),
		@"demanded_copy_subresources_per_trial": @((double)demandedSubresources / Trials),
		@"baseline_texels_per_copy_set": @(oldTexels), @"demanded_texels_per_copy_set": @(demandedTexels),
		@"copy_work_reduction_percent": @(oldTexels > 0 ? (oldTexels - demandedTexels) * 100.0 / oldTexels : 0),
		@"warmup_pairs": @(Warmups), @"measured_pairs": @(Trials), @"alternating_order": @YES,
		@"baseline_cpu_encoding_thread_ms_median": number(oldCpuMs),
		@"demanded_cpu_encoding_thread_ms_median": number(newCpuMs),
		@"baseline_gpu_command_buffer_ms_median": number(oldGpuMs),
		@"demanded_gpu_command_buffer_ms_median": number(newGpuMs),
		@"gpu_timing_samples": @(std::min(oldGpu.size(), newGpu.size())),
		@"pixel_equal_to_source_fixture": @YES, @"whole_and_demanded_pixels_equal": @YES};
}

static NSDictionary *runProbe() {
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device) return @{@"status": @"SKIPPED", @"reason": @"MTLCreateSystemDefaultDevice returned nil; no GPU result is available.",
		@"gpu_gain_claimed": @NO};
	Resources resources = makeResources(device);
	const auto all = mr::depthProxyRange(Mips, Layers, 0, UINT32_MAX, 0, 0, mr::DepthProxyViewKind::Array);
	const auto cube = mr::depthProxyRange(Mips, Layers, 0, UINT32_MAX, 0, 1, mr::DepthProxyViewKind::Cube);
	if (!all.valid || !cube.valid) throw std::runtime_error("Shared depth-proxy range helper rejected the probe ranges.");
	NSDictionary *full = scenario(resources, @"full_array_view", all);
	NSDictionary *partial = scenario(resources, @"one_cube_of_two_cube_array", cube);
	return @{@"status": @"PASS", @"benchmark": @"metal_depth_proxy_demanded_range_probe",
		@"scope": @"Host Metal depth-proxy copy workload only; not the game, iPhone, or FPS.",
		@"device": device.name ?: @"unknown", @"texture_width": @(Width), @"texture_height": @(Height),
		@"mip_levels": @(Mips), @"array_layers": @(Layers), @"format_source": @"R32Float",
		@"format_proxy": @"Depth32Float", @"gpu_gain_claimed": @NO,
		@"cpu_scope": @"Current-thread CPU time spent encoding copy render passes only; excludes submit, waits, setup, and validation.",
		@"gpu_scope": @"Completed copy command-buffer GPUStartTime to GPUEndTime; excludes validation/readback.",
		@"source_pattern": @"Unique exact binary fractions by mip/layer/pixel; fixture remains below 2^24 samples.",
		@"full_view": full, @"partial_cube_view": partial};
}
} // namespace

int main(int argc, char **argv) {
	@autoreleasepool {
		NSString *output = nil;
		if (argc == 3 && std::strcmp(argv[1], "--output") == 0) output = [NSString stringWithUTF8String:argv[2]];
		else if (argc != 1) { std::fprintf(stderr, "Usage: metal_depth_proxy_probe [--output report.json]\n"); return 2; }
		NSDictionary *report = nil; int result = 0;
		try { report = runProbe(); }
		catch (const std::exception &error) { report = @{@"status": @"FAIL", @"reason": [NSString stringWithUTF8String:error.what()]}; result = 1; }
		NSError *error = nil;
		NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:&error];
		if (!json) { std::fprintf(stderr, "Could not encode probe report.\n"); return 1; }
		if (output && ![json writeToFile:output options:NSDataWritingAtomic error:&error]) {
			std::fprintf(stderr, "Could not write probe report.\n"); return 1;
		}
		std::fwrite(json.bytes, 1, json.length, stdout); std::putchar('\n');
		return result;
	}
}
