#include "NativeMemoryBudget.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 assert(pg_streaming_headroom_budget(1000,2000,384,0)==1000);
 assert(pg_streaming_headroom_budget(1000,600,384,0)==216);
 assert(pg_streaming_headroom_budget(1000,384,384,0)==0);
 assert(pg_streaming_headroom_budget(1000,0,384,0)==0);
 assert(pg_streaming_headroom_budget(0,1000,384,0)==0);
 assert(pg_streaming_headroom_budget(UINT64_MAX,UINT64_MAX,384,0)==UINT64_MAX-384);
 assert(pg_streaming_resource_budget(1000,2000,384,0)==1000);
 assert(pg_streaming_resource_budget(1000,600,384,0)==216);
 assert(pg_streaming_resource_budget(1000,384,384,0)==0);
 assert(pg_streaming_resource_budget(1000,100,384,0)==-284);
 assert(pg_streaming_resource_budget(1000,0,384,0)==-384);
 assert(pg_streaming_resource_budget(-1000,100,384,0)==-1000);
 assert(pg_streaming_resource_budget(0,100,384,0)==-284);
 assert(pg_streaming_resource_budget(INT64_MAX,UINT64_MAX,384,0)==INT64_MAX);
 assert(pg_streaming_resource_budget(INT64_MAX,0,UINT64_MAX,0)==INT64_MIN);
 assert(pg_streaming_resource_budget(INT64_MIN,UINT64_MAX,384,0)==INT64_MIN);
 // The band: growth stops 256 above the reserve, eviction only below the reserve.
 assert(pg_streaming_headroom_budget(1000,2000,384,256)==1000);
 assert(pg_streaming_headroom_budget(1000,800,384,256)==160);
 assert(pg_streaming_headroom_budget(1000,500,384,256)==0);
 assert(pg_streaming_resource_budget(1000,800,384,256)==160);
 assert(pg_streaming_resource_budget(1000,500,384,256)==0);
 assert(pg_streaming_resource_budget(1000,384,384,256)==0);
 assert(pg_streaming_resource_budget(1000,100,384,256)==-284);
 assert(pg_streaming_resource_budget(INT64_MAX,UINT64_MAX,384,UINT64_MAX)==0);
 puts("PASS: native streaming reserve, unsigned heap clamp, signed eviction deficit, original deficit preservation and overflow boundaries.");
}
