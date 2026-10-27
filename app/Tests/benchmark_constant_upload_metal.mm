// Optional native macOS Metal microbenchmark, not a game/device benchmark.
// Build with clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation
// -framework Metal Tests/benchmark_constant_upload_metal.mm -o /tmp/cb-metal
// A missing Metal device or unavailable GPU timestamps is explicitly reported.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../Source/MetalRenderer/MRConstantUpload.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <vector>

namespace {
constexpr unsigned kBytes=4096,kBuffers=128,kRepeats=32,kFrames=12,kSlots=3;
constexpr NSUInteger kChunk=8<<20;
constexpr uint64_t kInvalid=UINT64_MAX;
struct Buffer { std::array<uint8_t,kBytes> shadow{};uint64_t frame=kInvalid;NSUInteger offset=0;unsigned chunk=0; };
struct Result { double cpuMs=0,gpuMs=0;bool gpuTime=true;uint64_t checksum=0,stagingBytes=0,allocatedBytes=0,bindings=0; };
double cpuSeconds() { timespec value{};if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&value)!=0) [NSException raise:@"BenchmarkClock" format:@"Process CPU clock unavailable"];return value.tv_sec+value.tv_nsec*1e-9; }
uint64_t hash(const void *memory,std::size_t bytes) { auto *p=static_cast<const uint8_t*>(memory);uint64_t h=1469598103934665603ull;for(std::size_t i=0;i<bytes;i++) {h^=p[i];h*=1099511628211ull;}return h; }
void validatePixels(const std::vector<uint8_t> &pixels,unsigned frame) {
    for (unsigned y=0;y<128;y++) for (unsigned x=0;x<64;x++) {
        const unsigned material=(y/8)*8+x/8;
        const int expected[3]={static_cast<int>(std::lround(float(material%8)/7*255)),
                               static_cast<int>(std::lround(float(material/8)/15*255)),
                               static_cast<int>(std::lround(float(frame%5)/4*255))};
        const uint8_t *pixel=pixels.data()+(y*64+x)*4;
        for (unsigned channel=0;channel<3;channel++) {
            if (std::abs(static_cast<int>(pixel[channel])-expected[channel])>1)
                [NSException raise:@"BenchmarkPixels" format:@"Frame %u pixel %u,%u channel %u: %u, expected %d +/- 1",frame,x,y,channel,pixel[channel],expected[channel]];
        }
        if (pixel[3]!=255) [NSException raise:@"BenchmarkPixels" format:@"Frame %u pixel %u,%u alpha: %u, expected 255",frame,x,y,pixel[3]];
    }
}
NSArray *rawTimes(const std::vector<double> &values) {
    NSMutableArray *result=[NSMutableArray arrayWithCapacity:values.size()];
    for (double value:values) [result addObject:std::isfinite(value) ? @(value) : (id)NSNull.null];
    return result;
}

template<bool Deduplicate>
Result run(id<MTLDevice> device,id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline) {
    Result result;
    std::array<Buffer,kBuffers> buffers;
    constexpr NSUInteger maximum=static_cast<NSUInteger>(kBytes)*kBuffers*(Deduplicate?1:kRepeats);
    constexpr unsigned chunks=static_cast<unsigned>((maximum+kChunk-1)/kChunk);
    std::array<std::array<id<MTLBuffer>,chunks>,kSlots> ring;
    for(auto &slot:ring) for(auto &chunk:slot) {
        chunk=[device newBufferWithLength:kChunk options:MTLResourceStorageModeShared];
        if(!chunk || !chunk.contents) [NSException raise:@"BenchmarkAllocation" format:@"Could not allocate CPU-addressable staging buffer"];
        result.allocatedBytes+=chunk.allocatedSize;
    }
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:128 mipmapped:NO];
    td.usage=MTLTextureUsageRenderTarget;td.storageMode=MTLStorageModePrivate;
    id<MTLTexture> target=[device newTextureWithDescriptor:td];
    if(!target) [NSException raise:@"BenchmarkAllocation" format:@"Could not allocate render target"];
    id<MTLBuffer> readback=[device newBufferWithLength:64*128*4 options:MTLResourceStorageModeShared];
    if(!readback || !readback.contents) [NSException raise:@"BenchmarkAllocation" format:@"Could not allocate CPU-addressable readback buffer"];
    std::array<uint8_t,kBytes> packet{};
    std::vector<uint8_t> pixels(64*128*4);
    for(unsigned frame=0;frame<kFrames;frame++) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb=[queue commandBuffer];
            if(!cb) [NSException raise:@"BenchmarkEncoder" format:@"Could not create command buffer"];
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=target;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
            rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
            const double start=cpuSeconds();
            id<MTLRenderCommandEncoder> encoder=[cb renderCommandEncoderWithDescriptor:rp];
            if(!encoder) [NSException raise:@"BenchmarkEncoder" format:@"Could not create render encoder"];
            [encoder setRenderPipelineState:pipeline];
            NSUInteger cursor=0,previousOffset=NSUIntegerMax;id<MTLBuffer> previous=nil;
            for(unsigned material=0;material<kBuffers;material++) {
                float color[4]={float(material%8)/7,float(material/8)/15,float(frame%5)/4,1};
                packet.fill(static_cast<uint8_t>(material+frame));std::memcpy(packet.data(),color,sizeof color);
                [encoder setViewport:(MTLViewport){double(material%8)*8,double(material/8)*8,8,8,0,1}];
                [encoder setScissorRect:(MTLScissorRect){(material%8)*8,(material/8)*8,8,8}];
                for(unsigned repetition=0;repetition<kRepeats;repetition++) {
                    auto &b=buffers[material];
                    if constexpr(Deduplicate) {
                        if(mr::updateConstantShadow(b.shadow.data(),kBytes,0,packet.data(),kBytes)) b.frame=kInvalid;
                    } else { std::memcpy(b.shadow.data(),packet.data(),kBytes);b.frame=kInvalid; }
                    if(b.frame!=frame) {
                        b.chunk=static_cast<unsigned>(cursor/kChunk);b.offset=cursor%kChunk;b.frame=frame;
                        id<MTLBuffer> storage=ring[frame%kSlots][b.chunk];
                        std::memcpy(static_cast<uint8_t*>(storage.contents)+b.offset,b.shadow.data(),kBytes);
                        cursor+=kBytes;result.stagingBytes+=kBytes;
                    }
                    id<MTLBuffer> storage=ring[frame%kSlots][b.chunk];
                    // Same stage buffer/offset cache as the production path.
                    if(storage!=previous) { [encoder setFragmentBuffer:storage offset:b.offset atIndex:0];result.bindings++; }
                    else if(b.offset!=previousOffset) { [encoder setFragmentBufferOffset:b.offset atIndex:0];result.bindings++; }
                    previous=storage;previousOffset=b.offset;
                    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                }
            }
            [encoder endEncoding];result.cpuMs+=(cpuSeconds()-start)*1000;
            id<MTLBlitCommandEncoder> copy=[cb blitCommandEncoder];
            if(!copy) [NSException raise:@"BenchmarkEncoder" format:@"Could not create readback blit encoder"];
            [copy copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(64,128,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:64*4 destinationBytesPerImage:64*128*4];
            [copy endEncoding];
            [cb commit];[cb waitUntilCompleted];
            if(cb.status!=MTLCommandBufferStatusCompleted) [NSException raise:@"BenchmarkGPU" format:@"%@",cb.error.localizedDescription];
            const double elapsed=cb.GPUEndTime-cb.GPUStartTime;
            if(elapsed>0 && cb.GPUStartTime>0) result.gpuMs+=elapsed*1000;else result.gpuTime=false;
            std::memcpy(pixels.data(),readback.contents,pixels.size());
            validatePixels(pixels,frame);
            result.checksum=(result.checksum*131)^hash(pixels.data(),pixels.size());
        }
    }
    return result;
}
double median(std::vector<double> values) { std::sort(values.begin(),values.end());return values[values.size()/2]; }
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device) { std::puts("{\"benchmark\":\"constant-upload-metal\",\"status\":\"SKIPPED\",\"reason\":\"No native Metal device\"}");return 0; }
        NSError *error=nil;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 v(uint i [[vertex_id]]) { return float4(i==0?float2(-1,-1):i==1?float2(3,-1):float2(-1,3),0,1); }\nfragment float4 f(constant float4 &color [[buffer(0)]]) { return color; }\n";
        id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
        if(!library) { std::fprintf(stderr,"Metal test shader failed: %s\n",error.localizedDescription.UTF8String);return 1; }
        MTLRenderPipelineDescriptor *descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[library newFunctionWithName:@"v"];descriptor.fragmentFunction=[library newFunctionWithName:@"f"];descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        id<MTLCommandQueue> queue=[device newCommandQueue];
        if(!pipeline || !queue) { std::fprintf(stderr,"Metal test setup failed: %s\n",error.localizedDescription.UTF8String);return 1; }
        (void)run<false>(device,queue,pipeline);(void)run<true>(device,queue,pipeline);
        std::vector<double> beforeCPU,afterCPU,beforeGPU,afterGPU;Result before,after;bool gpuTimes=true;
        for(unsigned trial=0;trial<5;trial++) {
            @autoreleasepool {
                if(trial%2==0) { before=run<false>(device,queue,pipeline);after=run<true>(device,queue,pipeline); }
                else { after=run<true>(device,queue,pipeline);before=run<false>(device,queue,pipeline); }
                if(before.checksum!=after.checksum) { std::fputs("ERROR: rendered-pixel hash mismatch\n",stderr);return 1; }
                beforeCPU.push_back(before.cpuMs);afterCPU.push_back(after.cpuMs);
                beforeGPU.push_back(before.gpuTime ? before.gpuMs : std::numeric_limits<double>::quiet_NaN());
                afterGPU.push_back(after.gpuTime ? after.gpuMs : std::numeric_limits<double>::quiet_NaN());
                gpuTimes=gpuTimes && before.gpuTime && after.gpuTime;
            }
        }
        NSDictionary *result=@{
            @"benchmark":@"constant-upload-metal",@"status":@"PASS",@"scope":@"synthetic constant-upload draw sequence, not full game or iPhone",
            @"device":device.name,@"trials":@5,@"frames_per_trial":@(kFrames),@"draws_per_frame":@(kBuffers*kRepeats),@"constant_bytes":@(kBytes),
            @"trial_order":@"before-after on even trial indices, after-before on odd indices",
            @"cpu_clock":@"CLOCK_PROCESS_CPUTIME_ID: all process threads during each encoding section",
            @"cpu_scope":@"render encoder setup, shadow/staging updates and draw encoding; excludes explicit command commit/wait and readback encoding",
            @"gpu_scope":@"complete command buffer: render draws plus 32 KiB texture-to-buffer readback copy",
            @"before_cpu_encoding_ms_median":@(median(beforeCPU)),@"after_cpu_encoding_ms_median":@(median(afterCPU)),
            @"before_cpu_encoding_ms_trials":rawTimes(beforeCPU),@"after_cpu_encoding_ms_trials":rawTimes(afterCPU),
            @"cpu_encoding_reduction_percent":@(100*(median(beforeCPU)-median(afterCPU))/median(beforeCPU)),
            @"gpu_time_measured":@(gpuTimes),@"before_gpu_ms_median":gpuTimes?@(median(beforeGPU)):(id)NSNull.null,@"after_gpu_ms_median":gpuTimes?@(median(afterGPU)):(id)NSNull.null,
            @"before_gpu_ms_trials":rawTimes(beforeGPU),@"after_gpu_ms_trials":rawTimes(afterGPU),
            @"gpu_time_reduction_percent":gpuTimes?@(100*(median(beforeGPU)-median(afterGPU))/median(beforeGPU)):(id)NSNull.null,
            @"before_staging_allocated_bytes":@(before.allocatedBytes),@"after_staging_allocated_bytes":@(after.allocatedBytes),
            @"before_staging_bytes_written":@(before.stagingBytes),@"after_staging_bytes_written":@(after.stagingBytes),
            @"before_buffer_bindings":@(before.bindings),@"after_buffer_bindings":@(after.bindings),
            @"rendered_pixel_hash_match":@YES,@"every_pixel_matches_expected_tile_rgba":@YES,@"pixel_validation":@"all 8192 pixels per frame: expected tile RGB +/- 1 UNORM rounding, alpha exactly 255",@"rendered_pixel_hash":[NSString stringWithFormat:@"%016llx",(unsigned long long)after.checksum],@"app_rss_measured":@NO
        };
        NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingSortedKeys error:nil];
        std::puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
        return 0;
    }
}
