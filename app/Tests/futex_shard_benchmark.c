#include "WasmFutex.h"
// Host-only blocking-handoff benchmark. Build beside futex_waits.c with the
// futex_support platform shim and compare this source against an older
// WasmFutex.c revision. It measures futex subsystem CPU/wall time, not game FPS.
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_PAIRS 32u
#define STRIDE 128u
static _Alignas(64) uint8_t heap[MAX_PAIRS * STRIDE];
static wasm_rt_shared_memory_t memory = {sizeof heap, heap};
static _Atomic unsigned ready;
static _Atomic bool start;
static _Atomic unsigned errors;

_Noreturn void wasm_rt_trap(wasm_rt_trap_t trap) {
  (void)trap;
  abort();
}

struct PairTask { unsigned pair, iterations; };

static double seconds(clockid_t clock) {
  struct timespec t;
  clock_gettime(clock, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void *sender(void *pointer) {
  struct PairTask *task = pointer;
  _Atomic uint32_t *request = (_Atomic uint32_t *)(void *)(heap + task->pair * STRIDE);
  _Atomic uint32_t *reply = request + 16;
  atomic_fetch_add(&ready, 1);
  while (!atomic_load_explicit(&start, memory_order_acquire)) { }
  for (uint32_t i = 1; i <= task->iterations; ++i) {
    atomic_store_explicit(request, i, memory_order_release);
    pg_atomic_notify(&memory, task->pair * STRIDE, 1);
    uint32_t value;
    while ((value = atomic_load_explicit(reply, memory_order_acquire)) < i) {
      if (pg_atomic_wait32(&memory, task->pair * STRIDE + 64, value, UINT64_MAX) == 2)
        atomic_fetch_add(&errors, 1);
    }
  }
  return NULL;
}

static void *receiver(void *pointer) {
  struct PairTask *task = pointer;
  _Atomic uint32_t *request = (_Atomic uint32_t *)(void *)(heap + task->pair * STRIDE);
  _Atomic uint32_t *reply = request + 16;
  atomic_fetch_add(&ready, 1);
  while (!atomic_load_explicit(&start, memory_order_acquire)) { }
  for (uint32_t i = 1; i <= task->iterations; ++i) {
    uint32_t value;
    while ((value = atomic_load_explicit(request, memory_order_acquire)) < i) {
      if (pg_atomic_wait32(&memory, task->pair * STRIDE, value, UINT64_MAX) == 2)
        atomic_fetch_add(&errors, 1);
    }
    atomic_store_explicit(reply, i, memory_order_release);
    pg_atomic_notify(&memory, task->pair * STRIDE + 64, 1);
  }
  return NULL;
}

int main(int argc, char **argv) {
  unsigned pairs = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 16;
  unsigned iterations = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 20000;
  if (!pairs || pairs > MAX_PAIRS || !iterations) return 2;
  pthread_t threads[MAX_PAIRS * 2];
  struct PairTask tasks[MAX_PAIRS];
  for (unsigned i = 0; i < pairs; ++i) {
    tasks[i] = (struct PairTask){i, iterations};
    assert(!pthread_create(&threads[i * 2], NULL, sender, &tasks[i]));
    assert(!pthread_create(&threads[i * 2 + 1], NULL, receiver, &tasks[i]));
  }
  while (atomic_load(&ready) != pairs * 2) { }
  double cpuStart = seconds(CLOCK_PROCESS_CPUTIME_ID);
  double wallStart = seconds(CLOCK_MONOTONIC);
  atomic_store_explicit(&start, true, memory_order_release);
  for (unsigned i = 0; i < pairs * 2; ++i) pthread_join(threads[i], NULL);
  double wall = seconds(CLOCK_MONOTONIC) - wallStart;
  double cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - cpuStart;
  for (unsigned i = 0; i < pairs; ++i) {
    _Atomic uint32_t *request = (_Atomic uint32_t *)(void *)(heap + i * STRIDE);
    assert(atomic_load(request) == iterations);
    assert(atomic_load(request + 16) == iterations);
  }
  if (atomic_load(&errors)) return 1;
  printf("pairs=%u threads=%u iterations=%u handoffs=%u cpu_seconds=%.6f wall_seconds=%.6f handoffs_per_second=%.0f\n",
         pairs, pairs * 2, iterations, pairs * iterations, cpu, wall,
         (double)pairs * iterations / wall);
  return 0;
}
