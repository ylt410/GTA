#pragma once
// Small ABI fixture for compiling the production futex implementation before
// the generated WASM toolchain is restored. The full native-runtime test still
// checks the real wasm-rt type and converted memory64 engine entry points.
#include <stdint.h>
typedef struct { uint64_t size; uint8_t *data; } wasm_rt_shared_memory_t;
typedef enum { WASM_RT_TRAP_UNALIGNED = 1, WASM_RT_TRAP_OOB = 2 } wasm_rt_trap_t;
_Noreturn void wasm_rt_trap(wasm_rt_trap_t);
