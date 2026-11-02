#!/usr/bin/env python3
"""Compile the real NativeAssets demand-worker loop against the real WASM futex."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Source/NativeAssets.m"


def extract_worker() -> str:
    source = SOURCE.read_text()
    hint_start = source.index("static void *assetHintLoop(void *unused)")
    loop_start = source.index("static void *assetLoop(void *unused)")
    hint_body = source[hint_start:loop_start]
    if re.search(r"pg_atomic_wait32\s*\([^;]*assetTable", hint_body, re.S):
        raise AssertionError("assetHintLoop must not wait on the demand assetTable doorbell")
    if "nanosleep" not in hint_body:
        raise AssertionError("assetHintLoop must retain independent periodic pumping")

    end_marker = "void w2c_env_wasm_httpfs_io_start_js"
    function_end = source.index(end_marker, loop_start)
    worker = source[loop_start:function_end]
    hint_marker = "    // Consume optional prefetch hints. Demand reads remain authoritative."
    if hint_marker not in worker:
        raise AssertionError("assetLoop production hint boundary changed")
    prefix = worker[:worker.index(hint_marker)]
    wait_call = re.search(
        r"pg_atomic_wait32\s*\(\s*w2c_env_memory\(assetHost\)\s*,\s*assetTable\s*,\s*bell\s*,\s*(.*?)\s*\);",
        worker,
        re.S,
    )
    if not wait_call:
        raise AssertionError("could not locate the production assetLoop futex wait")
    timeout = re.sub(r"\s+", "", wait_call.group(1))
    if timeout != "assetQueuedHints?UINT64_MAX:10000000":
        raise AssertionError(f"unexpected assetLoop wait mode: {timeout}")

    status_load = "atomic_load_explicit(slot,memory_order_relaxed)!=1"
    compact = re.sub(r"\s+", "", prefix)
    precheck = compact.find(status_load)
    cas = compact.find("atomic_compare_exchange_strong(slot,&expected,3)")
    if precheck < 0 or cas < 0 or precheck > cas:
        raise AssertionError("ready-state relaxed load must filter slots before the claim CAS")

    prefix = re.sub(r"@autoreleasepool\s*\{", "{", prefix)
    test_wait = re.sub(r"\s+", " ", wait_call.group(1)).strip()
    prefix += (
        "\n    testAssetWait32(w2c_env_memory(assetHost), assetTable, bell, "
        + test_wait
        + ");\n  }\n  return NULL;\n}\n"
    )
    return prefix


HARNESS = r'''#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "WasmFutex.h"

#define WORKERS 6
#define MAX_SLOTS 4096
#define TOTAL_REQUESTS 10000
#define HEAP_SIZE (2u * 1024u * 1024u)
#define SLOT_BASE 64u
#define SLOT_BYTES 64u
#define DATA_BASE (SLOT_BASE + MAX_SLOTS * SLOT_BYTES + 64u)

struct w2c_env { int unused; };
static _Alignas(64) uint8_t heap[HEAP_SIZE];
static wasm_rt_shared_memory_t wasmMemory = {sizeof heap, heap};
static struct w2c_env host;
static struct w2c_env *assetHost = &host;
static uint64_t assetTable = 0;
static bool assetQueuedHints = true;
static _Atomic unsigned waitCalls;
static _Atomic unsigned readCalls[TOTAL_REQUESTS + 1];
static _Atomic unsigned activeReads;
static _Atomic unsigned maxActiveReads;
static _Atomic unsigned firstWaveReaders;
static _Atomic bool holdFirstWave;
static _Atomic bool releaseFirstWave;
static _Atomic bool holdNextWait;
static _Atomic bool reachedHeldWait;
static _Atomic bool releaseHeldWait;
static _Atomic uint32_t heldBell;
unsigned pg_futex_test_waiter_count(const void *address);

_Noreturn void wasm_rt_trap(wasm_rt_trap_t trap) { (void)trap; abort(); }
static wasm_rt_trap_t wasm_rt_impl_try(void) { return (wasm_rt_trap_t)0; }
static wasm_rt_shared_memory_t *w2c_env_memory(struct w2c_env *env) { (void)env; return &wasmMemory; }
static void *pg_pointer(struct w2c_env *env, uint64_t address, uint64_t length) {
  (void)env;
  if (address > sizeof heap || length > sizeof heap - address) abort();
  return heap + address;
}
static void pg_log(const char *message) { (void)message; }
#define pthread_setname_np(name) ((void)(name))

static void updateMaximum(_Atomic unsigned *maximum, unsigned value) {
  unsigned old = atomic_load(maximum);
  while (old < value && !atomic_compare_exchange_weak(maximum, &old, value)) {}
}

static int64_t readAsset(uint32_t id, uint64_t offset, uint8_t *destination, uint32_t length) {
  if (id > TOTAL_REQUESTS || !destination) abort();
  atomic_fetch_add(&readCalls[id], 1);
  unsigned active = atomic_fetch_add(&activeReads, 1) + 1;
  updateMaximum(&maxActiveReads, active);
  if (atomic_load(&holdFirstWave) && id >= 1 && id <= MAX_SLOTS) {
    atomic_fetch_add(&firstWaveReaders, 1);
    while (!atomic_load(&releaseFirstWave)) sched_yield();
  }
  for (uint32_t i = 0; i < length; ++i)
    destination[i] = (uint8_t)((id * 31u + offset + i) & 0xffu);
  atomic_fetch_sub(&activeReads, 1);
  return length;
}

static uint32_t testAssetWait32(wasm_rt_shared_memory_t *memory, uint64_t address,
                                uint32_t expected, uint64_t timeout) {
  if (timeout != UINT64_MAX) abort();
  atomic_fetch_add(&waitCalls, 1);
  if (atomic_exchange(&holdNextWait, false)) {
    atomic_store(&heldBell, expected);
    atomic_store(&reachedHeldWait, true);
    while (!atomic_load(&releaseHeldWait)) sched_yield();
  }
  return pg_atomic_wait32(memory, address, expected, timeout);
}

/* Source-extracted from NativeAssets.m; test-only platform and memory shims above. */
__ASSET_WORKER__

static double monotonicSeconds(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}
static void pauseBriefly(void) {
  struct timespec delay = {0, 1000000};
  nanosleep(&delay, NULL);
}
static void waitForBool(_Atomic bool *flag) {
  double deadline = monotonicSeconds() + 10.0;
  while (!atomic_load(flag)) { assert(monotonicSeconds() < deadline); pauseBriefly(); }
}
static void waitForCount(_Atomic unsigned *value, unsigned target) {
  double deadline = monotonicSeconds() + 10.0;
  while (atomic_load(value) < target) { assert(monotonicSeconds() < deadline); pauseBriefly(); }
}
static void waitForParkedWorkers(void) {
  double deadline = monotonicSeconds() + 10.0;
  while (pg_futex_test_waiter_count(heap + assetTable) != WORKERS) {
    assert(monotonicSeconds() < deadline);
    pauseBriefly();
  }
}
static _Atomic int32_t *tableWords(void) { return (_Atomic int32_t *)(void *)(heap + assetTable); }
static _Atomic int32_t *slotAt(unsigned slotIndex) {
  return (_Atomic int32_t *)(void *)(heap + SLOT_BASE + (uint64_t)slotIndex * SLOT_BYTES);
}
static void ringDoorbellOnly(void) {
  atomic_fetch_add_explicit(tableWords(), 1, memory_order_release);
  (void)pg_atomic_notify(&wasmMemory, assetTable, 1);
}
static void publishRequest(unsigned slotIndex, unsigned id) {
  _Atomic int32_t *slot = slotAt(slotIndex);
  uint64_t destination = DATA_BASE + (uint64_t)slotIndex * 16;
  uint64_t offset = (uint64_t)id * 17u;
  atomic_store(slot + 1, (int32_t)id);
  atomic_store(slot + 2, (int32_t)(uint32_t)offset);
  atomic_store(slot + 3, (int32_t)(uint32_t)(offset >> 32));
  atomic_store(slot + 4, (int32_t)(uint32_t)destination);
  atomic_store(slot + 5, (int32_t)(uint32_t)(destination >> 32));
  atomic_store(slot + 6, 8);
  atomic_store_explicit(slot, 1, memory_order_release); /* wasm_httpfs_pread publishes status=1 */
  atomic_fetch_add_explicit(tableWords(), 1, memory_order_release); /* doorbell++ */
  (void)pg_atomic_notify(&wasmMemory, assetTable, 1); /* notify(table, 1) */
}
static void waitForSlot(unsigned slotIndex) {
  double deadline = monotonicSeconds() + 10.0;
  while (atomic_load(slotAt(slotIndex)) != 2) {
    assert(monotonicSeconds() < deadline);
    pauseBriefly();
  }
}
static void verifyRequest(unsigned slotIndex, unsigned id) {
  _Atomic int32_t *slot = slotAt(slotIndex);
  uint64_t destination = DATA_BASE + (uint64_t)slotIndex * 16;
  uint64_t offset = (uint64_t)id * 17u;
  assert(atomic_load(slot) == 2);
  assert(atomic_load(slot + 7) == 8);
  assert(atomic_load(&readCalls[id]) == 1);
  for (unsigned i = 0; i < 8; ++i)
    assert(heap[destination + i] == (uint8_t)((id * 31u + offset + i) & 0xffu));
}
static void setCount(unsigned count) {
  assert(count <= MAX_SLOTS);
  atomic_store(tableWords() + 1, (int32_t)count);
  for (unsigned i = 0; i < count; ++i) atomic_store(slotAt(i), 0);
}
static void checkIdleDoesNotPoll(void) {
  struct timespec interval = {0, 50000000};
  double deadline = monotonicSeconds() + 2.0;
  for (;;) {
    waitForParkedWorkers();
    const unsigned before = atomic_load(&waitCalls);
    nanosleep(&interval, NULL);
    const unsigned after = atomic_load(&waitCalls);
    if (after == before) {
      assert(pg_futex_test_waiter_count(heap + assetTable) == WORKERS);
      return; /* Stable wait stats: event wait, no 10 ms idle wakeups. */
    }
    if (monotonicSeconds() >= deadline) {
      fprintf(stderr, "idle wait calls kept changing: %u -> %u (bell %d, queued %u)\n",
        before, after, atomic_load(tableWords()), pg_futex_test_waiter_count(heap + assetTable));
      abort();
    }
  }
}
static void runBatch(unsigned firstId, unsigned count, bool gateFirstReaders) {
  setCount(count);
  if (gateFirstReaders) {
    atomic_store(&firstWaveReaders, 0);
    atomic_store(&releaseFirstWave, false);
    atomic_store(&holdFirstWave, true);
  }
  for (unsigned i = 0; i < count; ++i) publishRequest(i, firstId + i);
  if (gateFirstReaders) {
    waitForCount(&firstWaveReaders, WORKERS);
    atomic_store(&holdFirstWave, false);
    atomic_store(&releaseFirstWave, true);
  }
  for (unsigned i = 0; i < count; ++i) waitForSlot(i);
  for (unsigned i = 0; i < count; ++i) verifyRequest(i, firstId + i);
  waitForParkedWorkers();
}

int main(void) {
  memset(heap, 0, sizeof heap);
  atomic_store(tableWords() + 1, 0);
  pthread_t workers[WORKERS];
  for (unsigned i = 0; i < WORKERS; ++i) assert(!pthread_create(&workers[i], NULL, assetLoop, (void *)(intptr_t)i));
  waitForParkedWorkers();
  checkIdleDoesNotPoll();

  // Wake one scanner, hold it after scanning but before the real futex call,
  // then publish status and notify exactly one other parked demand worker.
  atomic_store(tableWords() + 1, 1);
  atomic_store(&holdNextWait, true);
  ringDoorbellOnly();
  waitForBool(&reachedHeldWait);
  const uint32_t scannedBell = atomic_load(&heldBell);
  publishRequest(0, 0);
  const uint32_t publishedBell = (uint32_t)atomic_load(tableWords());
  assert(publishedBell != scannedBell);
  atomic_store(&releaseHeldWait, true);
  waitForSlot(0);
  verifyRequest(0, 0);
  waitForParkedWorkers();

  // 10,000 concurrent demand slots in the real producer pattern. Six actual
  // worker threads are forced to overlap their first reads; each result and
  // payload is checked, and every asset id must be read exactly once.
  runBatch(1, MAX_SLOTS, true);
  runBatch(1 + MAX_SLOTS, MAX_SLOTS, false);
  runBatch(1 + 2 * MAX_SLOTS, TOTAL_REQUESTS - 2 * MAX_SLOTS, false);
  assert(atomic_load(&maxActiveReads) >= WORKERS);
  checkIdleDoesNotPoll();
  puts("PASS: production demand loop: six parked workers, notify(1), no lost scan/notify wake, 10k concurrent slots exactly once, exact payloads, and idle event-wait without polling");
  fflush(stdout);
  exit(0); /* Workers intentionally model the process-lifetime production loops. */
}
'''


def main() -> None:
    production = extract_worker()
    generated = HARNESS.replace("__ASSET_WORKER__", production)
    with tempfile.TemporaryDirectory(prefix="asset-worker-production-") as temporary:
        c_file = Path(temporary) / "asset_worker_production.c"
        executable = Path(temporary) / "asset_worker_production"
        c_file.write_text(generated)
        subprocess.run(
            [
                "cc", "-std=c11", "-D_GNU_SOURCE", "-DPG_FUTEX_TESTING", "-O2",
                "-Wall", "-Wextra", "-pthread", "-I", str(ROOT / "Source"),
                "-I", str(ROOT / "Tests/futex_support"), "-include",
                str(ROOT / "Tests/futex_support/platform.h"), str(c_file),
                str(ROOT / "Source/WasmFutex.c"), "-o", str(executable),
            ],
            check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
