#include "NativeMemoryBudget.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 PGEngineMemoryPolicy p=pg_engine_memory_policy(6144,5120,0);
 assert(p.physicalLimitMB==6144&&p.physicalCapMB==5120&&p.engineReservationMB==5120);
 // Before the split, applying the 5120 MB physical cap first selected 4096 MB virtual.
 assert((p.physicalLimitMB-1024)==5120&&p.engineReservationMB!=(p.physicalCapMB-1024));
 p=pg_engine_memory_policy(6144,4096,0);
 assert(p.physicalLimitMB==6144&&p.physicalCapMB==4096&&p.engineReservationMB==5120);
 p=pg_engine_memory_policy(6144,0,0);
 assert(p.physicalCapMB==6144&&p.engineReservationMB==5120);
 p=pg_engine_memory_policy(4096,3072,0);
 assert(p.physicalLimitMB==4096&&p.physicalCapMB==3072&&p.engineReservationMB==4095);
 p=pg_engine_memory_policy(0,2048,0);
 assert(p.physicalLimitMB==0&&p.physicalCapMB==0&&p.engineReservationMB==4095);
 p=pg_engine_memory_policy(0,2048,6144);
 assert(p.physicalCapMB==0&&p.engineReservationMB==6144);
 p=pg_engine_memory_policy(UINT64_MAX,UINT64_MAX-1,0);
 assert(p.physicalLimitMB==UINT64_MAX&&p.physicalCapMB==UINT64_MAX-1&&p.engineReservationMB==7168);
 p=pg_engine_memory_policy(6144,5120,8192);
 assert(p.physicalCapMB==5120&&p.engineReservationMB==8192);
 p=pg_engine_memory_policy(6144,5120,1023);
 assert(p.physicalCapMB==5120&&p.engineReservationMB==5120);
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
