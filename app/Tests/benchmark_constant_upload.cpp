// Subsystem microbenchmark. The production helper and the previous memcpy +
// unconditional snapshot invalidation process identical deterministic draws.
// This is not a game FPS, total-app CPU, GPU-time or RSS measurement.
#include "../Source/MetalRenderer/MRConstantUpload.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numeric>
#include <string>
#include <vector>

namespace {
constexpr unsigned kBytes=1024, kBuffers=128, kRepeats=16, kFrames=120;
constexpr uint64_t kInvalid=UINT64_MAX;
struct Buffer { std::array<uint8_t,kBytes> bytes{}; uint64_t frame=kInvalid; std::size_t at=0; };
struct Result { double cpuMs=0; uint64_t snapshots=0, stagingBytes=0, updates=0, checksum=0, peakFrameStaging=0; };
double processSeconds() { timespec t{}; if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)!=0) std::abort();return t.tv_sec+t.tv_nsec*1e-9; }
uint64_t checksum(const uint8_t *p, std::size_t n) { uint64_t sum=0;for(std::size_t i=0;i<n;i++) sum=sum*131+p[i];return sum; }
__attribute__((noinline)) void snapshot(uint8_t *destination,const uint8_t *source) {
    std::memcpy(destination,source,kBytes);
    // The renderer passes every staged byte to an external Metal driver.
    // Preserve those writes in this framework-free workload as well.
    asm volatile("" : : "r"(destination) : "memory");
}

template<bool Deduplicate> Result run(unsigned changeEvery) {
    std::array<Buffer,kBuffers> buffers;
    // Preallocate the same worst-case arena for BOTH variants. Allocation time
    // and speculative RSS reductions are deliberately excluded from timing.
    std::vector<uint8_t> arena(kBuffers*kRepeats*kBytes);
    std::array<uint8_t,kBytes> packet{};
    Result result;
    const double start=processSeconds();
    for(unsigned frame=0;frame<kFrames;frame++) {
        std::size_t at=0;
        for(unsigned material=0;material<kBuffers;material++) {
            for(unsigned repetition=0;repetition<kRepeats;repetition++) {
                auto &b=buffers[material];
                // A full 1-KiB shader-constant update; the selected mode
                // changes a world/material word at a known repeat frequency.
                if(repetition==0) {
                    packet.fill(static_cast<uint8_t>(material*13+frame));
                    std::memcpy(packet.data(),&material,sizeof material);
                    std::memcpy(packet.data()+4,&frame,sizeof frame);
                }
                if(changeEvery && repetition%changeEvery==0) {
                    uint32_t value=(frame<<16)|(material<<8)|repetition;
                    std::memcpy(packet.data()+8,&value,sizeof value);
                }
                if constexpr(Deduplicate) {
                    if(mr::updateConstantShadow(b.bytes.data(),kBytes,0,packet.data(),kBytes)) b.frame=kInvalid;
                } else {
                    std::memcpy(b.bytes.data(),packet.data(),kBytes);b.frame=kInvalid;
                }
                if(b.frame!=frame) {
                    snapshot(arena.data()+at,b.bytes.data());b.at=at;at+=kBytes;b.frame=frame;
                    result.snapshots++;result.stagingBytes+=kBytes;
                }
                // Observe the snapshot at each draw. This dependency prevents
                // optimization from deleting overwritten intermediate writes.
                uint32_t word;std::memcpy(&word,arena.data()+b.at+8,sizeof word);
                result.checksum=(result.checksum*131)^word^arena[b.at+kBytes-1];
                result.updates++;
            }
        }
        result.peakFrameStaging=std::max<uint64_t>(result.peakFrameStaging,at);
    }
    result.cpuMs=(processSeconds()-start)*1000;
    for(const auto &b:buffers) result.checksum^=checksum(b.bytes.data(),kBytes);
    return result;
}
double median(std::vector<double> v) { std::sort(v.begin(),v.end());return v[v.size()/2]; }
std::string rawTimes(const std::vector<double> &values) {
    std::string result="[";
    for (std::size_t i=0;i<values.size();i++) {
        char number[64];std::snprintf(number,sizeof number,"%s%.9f",i?",":"",values[i]);result+=number;
    }
    return result+"]";
}
}
int main() {
    for(auto mode: {std::pair<const char *,unsigned>{"stable-within-frame",0},{"one-change-per-four-draws",4},{"changes-every-draw",1}}) {
        (void)run<false>(mode.second);(void)run<true>(mode.second);
        std::vector<double> oldTimes,newTimes;Result before,after;
        for(unsigned trial=0;trial<9;trial++) {
            if(trial%2==0) { before=run<false>(mode.second);after=run<true>(mode.second); }
            else { after=run<true>(mode.second);before=run<false>(mode.second); }
            if(before.checksum!=after.checksum || before.updates!=after.updates) {
                std::fputs("ERROR: observable draw result mismatch\n",stderr);return 1;
            }
            oldTimes.push_back(before.cpuMs);newTimes.push_back(after.cpuMs);
        }
        double oldMedian=median(oldTimes),newMedian=median(newTimes);
        std::printf("{\"workload\":\"%s\",\"kind\":\"constant-upload-subsystem-microbenchmark\",\"trials\":9,\"trial_order\":\"before-after on even trial indices, after-before on odd indices\",\"cpu_clock\":\"CLOCK_PROCESS_CPUTIME_ID: all process threads during the update and staging loop\",\"updates\":%llu,\"shadow_bytes\":%u,\"before_cpu_ms_median\":%.6f,\"after_cpu_ms_median\":%.6f,\"before_cpu_ms_trials\":%s,\"after_cpu_ms_trials\":%s,\"cpu_reduction_percent\":%.3f,\"before_staging_bytes\":%llu,\"after_staging_bytes\":%llu,\"before_peak_frame_staging_bytes\":%llu,\"after_peak_frame_staging_bytes\":%llu,\"before_snapshots\":%llu,\"after_snapshots\":%llu,\"observable_checksum\":\"%016llx\",\"same_observable_draws\":true,\"gpu_time_measured\":false,\"before_gpu_ms_trials\":null,\"after_gpu_ms_trials\":null,\"app_rss_measured\":false}\n",
            mode.first,(unsigned long long)before.updates,kBytes,oldMedian,newMedian,rawTimes(oldTimes).c_str(),rawTimes(newTimes).c_str(),100*(oldMedian-newMedian)/oldMedian,
            (unsigned long long)before.stagingBytes,(unsigned long long)after.stagingBytes,
            (unsigned long long)before.peakFrameStaging,(unsigned long long)after.peakFrameStaging,
            (unsigned long long)before.snapshots,(unsigned long long)after.snapshots,(unsigned long long)after.checksum);
    }
}
