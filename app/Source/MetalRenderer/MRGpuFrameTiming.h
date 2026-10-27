#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace mr {
struct GpuFrameSample { bool ready=false, valid=false; double milliseconds=0; uint64_t commands=0; };

// One open-frame reference plus one reference for each submitted command.
// Completion handlers may run out of order. Closing at PRESENT cannot publish
// a partial frame, and the final callback publishes exactly once.
class GpuFrameTiming {
 std::atomic<uint64_t> references{1}, nanos{0}, commands{0};
 std::atomic<bool> invalid{false};
 GpuFrameSample release() {
  if(references.fetch_sub(1,std::memory_order_acq_rel)!=1)return {};
  uint64_t count=commands.load(std::memory_order_relaxed);
  return {true,count>0&&!invalid.load(std::memory_order_relaxed),nanos.load(std::memory_order_relaxed)/1e6,count};
 }
public:
 void submit(){commands.fetch_add(1,std::memory_order_relaxed);references.fetch_add(1,std::memory_order_relaxed);}
 GpuFrameSample complete(double start,double end,bool succeeded){
  double duration=end-start;
  if(!succeeded||!std::isfinite(start)||!std::isfinite(end)||start<=0||duration<=0||duration>60)
   invalid.store(true,std::memory_order_relaxed);
  else nanos.fetch_add((uint64_t)std::llround(duration*1e9),std::memory_order_relaxed);
  return release();
 }
 GpuFrameSample close(){return release();}
};
} // namespace mr
