#include "MetalRenderer/MRGpuFrameTiming.h"
#include <cassert>
#include <cmath>
#include <thread>
#include <vector>
#include <atomic>
#include <cstdio>

int main(){
 using mr::GpuFrameTiming;
 GpuFrameTiming frame;
 for(int i=0;i<20;i++)frame.submit();
 for(int i=0;i<19;i++)assert(!frame.complete(10,10.001,true).ready);
 assert(!frame.close().ready);
 auto s=frame.complete(10,10.002,true);
 assert(s.ready&&s.valid&&s.commands==20&&std::abs(s.milliseconds-21)<1e-6);
 GpuFrameTiming early;
 early.submit();assert(!early.complete(2,2.001,true).ready);
 s=early.close();assert(s.ready&&s.valid&&s.commands==1);
 for(int type=0;type<4;type++){
  GpuFrameTiming invalid;invalid.submit();invalid.submit();assert(!invalid.close().ready);
  assert(!invalid.complete(1,1.003,true).ready);
  s=invalid.complete(type==0?0:1,type==1?NAN:type==2?.5:1.001,type!=3);
  assert(s.ready&&!s.valid&&s.commands==2);
 }
 // Every completion racing close, repeated across distinct frame generations.
 for(int trial=0;trial<200;trial++){
  GpuFrameTiming concurrent;std::atomic<unsigned> published{0};std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for(int i=0;i<8;i++){
   concurrent.submit();
   threads.emplace_back([&]{while(!go.load()){};auto r=concurrent.complete(3,3.002,true);if(r.ready){assert(r.valid&&r.commands==8&&std::abs(r.milliseconds-16)<1e-6);published++;}});
  }
  go=true;s=concurrent.close();if(s.ready){assert(s.valid&&s.commands==8);published++;}
  for(auto &t:threads)t.join();
  assert(published==1);
 }
 puts("PASS: all-command GPU frame grouping, callbacks before/after present, invalid timestamps, out-of-order completion and exactly-once concurrent publication");
}
