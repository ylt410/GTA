#include "WasmFutex.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static _Alignas(64) uint8_t heap[8192];
static wasm_rt_shared_memory_t memory = {sizeof heap, heap};
static _Thread_local jmp_buf trapTarget;
static _Thread_local bool expectTrap;
_Noreturn void wasm_rt_trap(wasm_rt_trap_t code) {
  if (!expectTrap) abort();
  longjmp(trapTarget, (int)code);
}
static _Atomic unsigned finished;
static _Atomic uint32_t finishedMask;
static _Atomic bool start;
static _Atomic uint64_t resultSum;
static void pauseBriefly(void) { struct timespec delay = {0, 100000}; nanosleep(&delay, NULL); }
static double seconds(clockid_t which) {
  struct timespec value; clock_gettime(which, &value);
  return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}
static void untilFinished(unsigned count) {
  double deadline = seconds(CLOCK_MONOTONIC) + 5;
  while (atomic_load(&finished) < count) { assert(seconds(CLOCK_MONOTONIC) < deadline); pauseBriefly(); }
}
static uint32_t notifyEventually(wasm_rt_shared_memory_t *m, uint64_t at, uint32_t count) {
  double deadline = seconds(CLOCK_MONOTONIC) + 5;
  uint32_t total = 0;
  while (total < count) {
    uint32_t next = pg_atomic_notify(m, at, count - total);
    assert(next <= count - total); total += next;
    assert(seconds(CLOCK_MONOTONIC) < deadline); if (total < count) pauseBriefly();
  }
  return total;
}
struct WaitTask { unsigned index, width; uint64_t at, expected, timeout; uint32_t result; };
static void *oneWait(void *pointer) {
  struct WaitTask *task = pointer;
  task->result = task->width == 8
    ? pg_atomic_wait64(&memory, task->at, task->expected, task->timeout)
    : pg_atomic_wait32(&memory, task->at, (uint32_t)task->expected, task->timeout);
  atomic_fetch_or(&finishedMask, UINT32_C(1) << task->index);
  atomic_fetch_add(&finished, 1); return NULL;
}
static void reset(void) {
  memset(heap, 0, sizeof heap); atomic_store(&finished, 0); atomic_store(&finishedMask, 0);
}
static void fastAndTrapCases(void) {
  reset(); atomic_store((_Atomic uint64_t *)(void *)(heap + 8), UINT64_C(0xabcdef0123456789));
  assert(pg_atomic_wait32(&memory, 8, 99, UINT64_MAX) == 1);
  assert(pg_atomic_wait64(&memory, 8, UINT64_C(0xabcdef0123456788), 0) == 1);
  assert(pg_atomic_wait64(&memory, 8, UINT64_C(0xabcdef0123456789), 0) == 2);
  assert(pg_atomic_wait32(&memory, 0, 0, 0) == 2);
  assert(pg_atomic_wait32(&memory, 0, 0, 1000000) == 2);
  assert(pg_atomic_wait64(&memory, 0, 0, 1000000) == 2);
  assert(pg_atomic_notify(&memory, 0, 0) == 0);
  assert(pg_atomic_notify(&memory, 0, UINT32_MAX) == 0);
  expectTrap = true;
  int trapped = setjmp(trapTarget);
  if (!trapped) { pg_atomic_wait32(&memory, 1, 1, 0); abort(); }
  assert(trapped == WASM_RT_TRAP_UNALIGNED);
  trapped = setjmp(trapTarget);
  if (!trapped) { pg_atomic_wait64(&memory, 4, 0, 0); abort(); }
  assert(trapped == WASM_RT_TRAP_UNALIGNED);
  trapped = setjmp(trapTarget);
  if (!trapped) { pg_atomic_notify(&memory, 1, 0); abort(); }
  assert(trapped == WASM_RT_TRAP_UNALIGNED); // notify(0) must still validate.
  trapped = setjmp(trapTarget);
  if (!trapped) { pg_atomic_notify(&memory, sizeof heap, 0); abort(); }
  assert(trapped == WASM_RT_TRAP_OOB);
  trapped = setjmp(trapTarget);
  if (!trapped) { pg_atomic_wait64(&memory, UINT64_MAX - 7, 0, 0); abort(); }
  assert(trapped == WASM_RT_TRAP_OOB); expectTrap = false;
}
static void partialAndMixedWaits(void) {
  reset(); pthread_t threads[3]; struct WaitTask tasks[3];
  for (unsigned i = 0; i < 3; i++) {
    tasks[i] = (struct WaitTask){i, i == 0 ? 8 : 4, 0, 0, UINT64_MAX, 99};
    assert(!pthread_create(threads + i, NULL, oneWait, tasks + i));
  }
  assert(pg_atomic_notify(&memory, 0, 0) == 0);
  assert(notifyEventually(&memory, 0, 2) == 2); untilFinished(2);
  assert(atomic_load(&finished) == 2);
  wasm_rt_shared_memory_t alias = memory; // Queue identity is the backing address.
  assert(notifyEventually(&alias, 0, 1) == 1); untilFinished(3);
  for (unsigned i = 0; i < 3; i++) { pthread_join(threads[i], NULL); assert(tasks[i].result == 0); }
  assert(pg_atomic_notify(&memory, 0, UINT32_MAX) == 0);
}
static void isolatedAddresses(void) {
  reset(); pthread_t threads[4]; struct WaitTask tasks[4];
  for (unsigned i = 0; i < 4; i++) {
    tasks[i] = (struct WaitTask){i, 4, i * 64, 0, UINT64_MAX, 99};
    assert(!pthread_create(threads + i, NULL, oneWait, tasks + i));
  }
  for (unsigned i = 0; i < 4; i++) {
    assert(notifyEventually(&memory, i * 64, 1) == 1); untilFinished(i + 1);
    assert(atomic_load(&finishedMask) == (UINT32_C(1) << (i + 1)) - 1);
  }
  for (unsigned i = 0; i < 4; i++) { pthread_join(threads[i], NULL); assert(tasks[i].result == 0); }
}
static void timeoutNotifyRaces(void) {
  for (unsigned trial = 0; trial < 128; trial++) {
    reset(); pthread_t thread;
    struct WaitTask task = {0, trial % 2 ? 8 : 4, 0, 0, 200000, 99};
    assert(!pthread_create(&thread, NULL, oneWait, &task));
    if (trial % 2) pauseBriefly();
    uint32_t woken = pg_atomic_notify(&memory, 0, 1);
    pthread_join(thread, NULL);
    assert(woken <= 1); assert(task.result == (woken ? 0u : 2u));
    assert(pg_atomic_notify(&memory, 0, UINT32_MAX) == 0);
  }
}
struct LoopTask { unsigned index, iterations, mode; };
static void *loop(void *pointer) {
  struct LoopTask *task = pointer;
  unsigned index = task->index;
  while (!atomic_load(&start)) { }
  uint64_t sum = 0;
  if (task->mode < 2) {
    uint64_t offset = (uint64_t)index * 128;
    for (unsigned i = 0; i < task->iterations; i++)
      sum += pg_atomic_wait32(&memory, offset, task->mode ? 0 : 1, 0);
    assert(sum == (uint64_t)task->iterations * (task->mode ? 2 : 1));
  } else {
    uint64_t request = (uint64_t)(index / 2) * 128, reply = request + 64;
    for (uint32_t i = 1; i <= task->iterations; i++) {
      uint32_t value;
      if (!(index % 2)) {
        atomic_store((_Atomic uint32_t *)(void *)(heap + request), i);
        pg_atomic_notify(&memory, request, UINT32_MAX);
        while ((value = atomic_load((_Atomic uint32_t *)(void *)(heap + reply))) < i)
          pg_atomic_wait32(&memory, reply, value, UINT64_MAX);
      } else {
        while ((value = atomic_load((_Atomic uint32_t *)(void *)(heap + request))) < i)
          pg_atomic_wait32(&memory, request, value, UINT64_MAX);
        atomic_store((_Atomic uint32_t *)(void *)(heap + reply), i);
        pg_atomic_notify(&memory, reply, UINT32_MAX);
      }
    }
  }
  atomic_fetch_add(&resultSum, sum); return NULL;
}
static void runLoops(unsigned count, unsigned iterations, unsigned mode, bool print) {
  assert(count && count <= 32 && (mode < 2 || !(count % 2)));
  reset(); atomic_store(&start, false); atomic_store(&resultSum, 0);
  pthread_t threads[32]; struct LoopTask tasks[32];
  for (unsigned i = 0; i < count; i++) {
    tasks[i] = (struct LoopTask){i, iterations, mode};
    assert(!pthread_create(threads + i, NULL, loop, tasks + i));
  }
  double cpu = seconds(CLOCK_PROCESS_CPUTIME_ID), wall = seconds(CLOCK_MONOTONIC);
  atomic_store(&start, true);
  for (unsigned i = 0; i < count; i++) pthread_join(threads[i], NULL);
  wall = seconds(CLOCK_MONOTONIC) - wall; cpu = seconds(CLOCK_PROCESS_CPUTIME_ID) - cpu;
  if (mode == 2) for (unsigned pair = 0; pair < count / 2; pair++) {
    assert(atomic_load((_Atomic uint32_t *)(void *)(heap + pair * 128)) == iterations);
    assert(atomic_load((_Atomic uint32_t *)(void *)(heap + pair * 128 + 64)) == iterations);
  }
  if (print) printf("mode=%s threads=%u iterations=%u cpu_seconds=%.9f wall_seconds=%.9f sum=%" PRIu64 "\n",
    mode == 0 ? "mismatch" : mode == 1 ? "zero-time" : "blocking-handoff", count, iterations,
    cpu, wall, atomic_load(&resultSum));
}
int main(int argc, char **argv) {
  alarm(60);
  if (argc == 5 && !strcmp(argv[1], "--benchmark")) {
    runLoops((unsigned)strtoul(argv[2], NULL, 10), (unsigned)strtoul(argv[3], NULL, 10),
      (unsigned)strtoul(argv[4], NULL, 10), true); return 0;
  }
  fastAndTrapCases(); partialAndMixedWaits(); isolatedAddresses(); timeoutNotifyRaces();
  runLoops(2, 10000, 2, false); runLoops(8, 10000, 2, false);
  runLoops(8, 10000, 0, false); runLoops(8, 10000, 1, false);
  puts("PASS: production futex fast paths, traps, exact partial/mixed wake, aliases, address isolation, timeout races and concurrent handoffs");
}
