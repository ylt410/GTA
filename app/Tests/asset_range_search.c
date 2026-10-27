#include "../Source/NativeAssetRangeSearch.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint64_t offset, length; unsigned char unrelated[31]; } Range;

static size_t reference(const Range *ranges, size_t count, uint64_t at) {
  for (size_t i=0;i<count;i++) if (ranges[i].offset>at) return i;
  return count;
}

static void check(const Range *ranges, size_t count, uint64_t at) {
  size_t wanted=reference(ranges,count,at);
  size_t got=pg_asset_range_upper_bound(ranges,count,sizeof *ranges,offsetof(Range,offset),at);
  assert(got==wanted);
  // The predecessor contains the position, or the successor identifies the
  // exact end of a zero-filled alignment gap. Zero-length slots are valid.
  const Range *slot=got?&ranges[got-1]:NULL;
  if (slot && at-slot->offset>=slot->length) slot=NULL;
  const Range *old=NULL;
  for (size_t i=0;i<count;i++)
    if (at>=ranges[i].offset && at-ranges[i].offset<ranges[i].length) { old=&ranges[i];break; }
  assert(slot==old);
}

int main(void) {
  assert(pg_asset_range_upper_bound(NULL,0,0,0,UINT64_MAX)==0);
  Range ranges[4096]; uint64_t cursor=513, random=0x35b819a012ULL;
  for (size_t i=0;i<4096;i++) {
    ranges[i]=(Range){.offset=cursor,.length=i%7?1+i%1023:0};
    cursor=(cursor+ranges[i].length+511)&~UINT64_C(511);
  }
  for (size_t count=1;count<=4096;count=count<16?count+1:count*2) {
    for (size_t i=0;i<count;i++) {
      check(ranges,count,ranges[i].offset);
      if(ranges[i].offset)check(ranges,count,ranges[i].offset-1);
      check(ranges,count,ranges[i].offset+ranges[i].length);
      if(ranges[i].length)check(ranges,count,ranges[i].offset+ranges[i].length-1);
    }
    check(ranges,count,UINT64_MAX);
    for (unsigned j=0;j<10000;j++) {
      random^=random<<13;random^=random>>7;random^=random<<17;
      check(ranges,count,random%(cursor+2048));
    }
  }
  // A deliberately unaligned start field must still be read safely.
  unsigned char unaligned[27]={0};
  uint64_t a=10,b=25,c=25;
  memcpy(unaligned+1,&a,8);memcpy(unaligned+10,&b,8);memcpy(unaligned+19,&c,8);
  assert(pg_asset_range_upper_bound(unaligned,3,9,1,9)==0);
  assert(pg_asset_range_upper_bound(unaligned,3,9,1,10)==1);
  assert(pg_asset_range_upper_bound(unaligned,3,9,1,25)==3);
  puts("PASS: strided mod-slot lookup matches linear reference at boundaries, gaps, zero-length duplicates, random offsets and unaligned fields.");
  return 0;
}
