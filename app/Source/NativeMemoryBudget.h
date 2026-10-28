#pragma once
#include <stdint.h>

typedef struct {
	uint64_t physicalLimitMB;
	uint64_t physicalCapMB;
	uint64_t engineReservationMB;
} PGEngineMemoryPolicy;

// The physical cap controls streaming and video budgets. The wasm heap's virtual
// reservation follows the device ceiling so a low physical cap cannot shrink its
// address space. A valid explicit reservation remains the final override.
static inline PGEngineMemoryPolicy pg_engine_memory_policy(uint64_t physicalLimitMB,uint64_t physicalCapMB,uint64_t overrideMB){
	PGEngineMemoryPolicy policy={.physicalLimitMB=physicalLimitMB};
	if(physicalLimitMB){
		policy.physicalCapMB=physicalCapMB&&physicalCapMB<physicalLimitMB?physicalCapMB:physicalLimitMB;
		policy.engineReservationMB=physicalLimitMB>4095u+1024u?
			(physicalLimitMB-1024u<7168u?physicalLimitMB-1024u:7168u):4095u;
	}else{
		policy.engineReservationMB=4095u;
	}
	if(overrideMB>=1024u&&overrideMB<=8192u)policy.engineReservationMB=overrideMB;
	return policy;
}

// Keep a 256 MiB resource-pool growth step plus transient upload headroom.
// These budget queries only inform streaming eviction; reservation is selected above.
// Two thresholds: streaming stops growing `band` bytes above the reserve, and only evicts below
// the reserve itself. One shared threshold made the engine evict and reload the same textures
// whenever memory hovered around it.
static inline uint64_t pg_streaming_headroom_budget(uint64_t engineAvailable,uint64_t processAvailable,uint64_t reserve,uint64_t band){
 uint64_t grow=reserve>UINT64_MAX-band?UINT64_MAX:reserve+band;
 uint64_t available=processAvailable>grow?processAvailable-grow:0;
 return engineAvailable<available?engineAvailable:available;
}
// The resource budget is signed. Below the reserve, a negative answer asks
// the existing streamer to evict that many bytes; zero only stops growth.
static inline int64_t pg_streaming_resource_budget(int64_t engineAvailable,uint64_t processAvailable,uint64_t reserve,uint64_t band){
 uint64_t grow=reserve>UINT64_MAX-band?UINT64_MAX:reserve+band;
 int64_t available;
 if(processAvailable>=grow){
  uint64_t remaining=processAvailable-grow;
  available=remaining>INT64_MAX?INT64_MAX:(int64_t)remaining;
 }else if(processAvailable>=reserve){
  available=0;
 }else{
  uint64_t deficit=reserve-processAvailable;
  available=deficit>INT64_MAX?INT64_MIN:-(int64_t)deficit;
 }
 return engineAvailable<available?engineAvailable:available;
}
