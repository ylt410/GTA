#define _POSIX_C_SOURCE 200809L
#include "../Source/NativeAssetRangeSearch.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SEARCH_REQUESTS 40000
#define ROUNDS 7
// Mirror PGModSlot's record layout; PATH_MAX and pointer width follow the
// benchmark host just as they do in NativeAssets.m on the build target.
typedef struct { const char *inner; uint64_t offset,length; uint32_t low,high;int resource; } BenchPiece;
typedef struct { BenchPiece info; char inner[256],file[PATH_MAX];uint64_t skip;int reported; } BenchRange;
static uint64_t positions[SEARCH_REQUESTS];
static volatile uint64_t consumed;
static double clockSeconds(clockid_t clock) {
  struct timespec t;assert(!clock_gettime(clock,&t));return (double)t.tv_sec+(double)t.tv_nsec/1e9;
}
static uint64_t randomNext(uint64_t *seed) {
  *seed^=*seed<<13;*seed^=*seed>>7;*seed^=*seed<<17;return *seed;
}
static size_t linearSlot(const BenchRange *slots,size_t count,uint64_t at) {
  for(size_t i=0;i<count;i++) if(at>=slots[i].info.offset&&at<slots[i].info.offset+slots[i].info.length)return i;
  uint64_t next=UINT64_MAX;size_t successor=count;
  for(size_t i=0;i<count;i++)if(slots[i].info.offset>at&&slots[i].info.offset<next){next=slots[i].info.offset;successor=i;}
  return successor;
}
static size_t indexedSlot(const BenchRange *slots,size_t count,uint64_t at) {
  size_t after=pg_asset_range_upper_bound(slots,count,sizeof *slots,offsetof(BenchRange,info.offset),at);
  if(after && at-slots[after-1].info.offset<slots[after-1].info.length)return after-1;
  return after;
}
static uint64_t searchWork(const BenchRange *slots,size_t count,int indexed) {
  uint64_t sum=0;
  for(unsigned i=0;i<SEARCH_REQUESTS;i++) {
    uint64_t at=positions[i]%(count*512+1024);
    sum+=indexed?indexedSlot(slots,count,at):linearSlot(slots,count,at);
  }
  return sum;
}
static void reportSearch(const BenchRange *slots,size_t count,int indexed,unsigned round) {
  double wall=clockSeconds(CLOCK_MONOTONIC),cpu=clockSeconds(CLOCK_PROCESS_CPUTIME_ID);
  uint64_t sum=searchWork(slots,count,indexed);
  cpu=clockSeconds(CLOCK_PROCESS_CPUTIME_ID)-cpu;wall=clockSeconds(CLOCK_MONOTONIC)-wall;consumed=sum;
  printf("{\"workload\":\"mod_slot_lookup\",\"variant\":\"%s\",\"slots\":%zu,\"requests\":%u,\"round\":%u,\"cpu_ms\":%.6f,\"wall_ms\":%.6f,\"checksum\":%" PRIu64 ",\"index_allocation_bytes\":0,\"record_stride_bytes\":%zu}\n",indexed?"optimized":"baseline",count,SEARCH_REQUESTS,round,cpu*1000,wall*1000,sum,sizeof *slots);
}
int main(void) {
  uint64_t seed=UINT64_C(0x83e714c0c355);
  for(unsigned i=0;i<SEARCH_REQUESTS;i++)positions[i]=randomNext(&seed);
  BenchRange *ranges=calloc(4096,sizeof *ranges);assert(ranges);
  for(unsigned i=0;i<4096;i++){ranges[i].info.offset=(uint64_t)i*512;ranges[i].info.length=1+(i*71)%511;}
  for(unsigned countIndex=0;countIndex<5;countIndex++) {
    size_t count=(size_t[]){1,8,64,256,4096}[countIndex];
    assert(searchWork(ranges,count,0)==searchWork(ranges,count,1));
    for(unsigned round=0;round<ROUNDS;round++) {
      reportSearch(ranges,count,(int)(round&1),round);
      reportSearch(ranges,count,(int)!(round&1),round);
    }
  }
  free(ranges);return 0;
}
