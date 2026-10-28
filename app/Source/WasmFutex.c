#include "WasmFutex.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>

// One queue shared by all instances of a module's imported shared memory.
// Per-waiter condition variables let notify wake exactly the requested count.
typedef struct Waiter {
  const void *address;
  bool notified;
  pthread_cond_t condition;
  struct Waiter *next;
} Waiter;

// Separate engine futex addresses should not contend on one process-wide lock.
// Hash the actual host address so aliases of the same shared memory still meet
// in the same queue, regardless of which memory wrapper was passed in.
#define FUTEX_BUCKET_COUNT 64u
typedef struct WaiterBucket {
  _Alignas(128) pthread_mutex_t lock;
  Waiter *waiters;
} WaiterBucket;
_Static_assert(sizeof(WaiterBucket) % 128 == 0, "futex buckets must not share cache lines");
static WaiterBucket buckets[FUTEX_BUCKET_COUNT];
static pthread_once_t bucketsOnce = PTHREAD_ONCE_INIT;

static void initializeBuckets(void) {
  for (unsigned i = 0; i < FUTEX_BUCKET_COUNT; ++i)
    if (pthread_mutex_init(&buckets[i].lock, NULL) != 0) abort();
}

static unsigned bucketIndex(const void *address) {
  uintptr_t value = (uintptr_t)address;
  // Hash cache-line addresses so independent futex lines remain well spread
  // even when their offsets share the same low alignment bits.
  value >>= 6;
  value ^= value >> 17;
  value *= (uintptr_t)0xed5ad4bbU;
  value ^= value >> 11;
  return (unsigned)value & (FUTEX_BUCKET_COUNT - 1u);
}

static WaiterBucket *bucketFor(const void *address) {
  if (pthread_once(&bucketsOnce, initializeBuckets) != 0) abort();
  return &buckets[bucketIndex(address)];
}

static void *checkedAddress(wasm_rt_shared_memory_t *memory, uint64_t offset,
                            uint32_t width) {
  if (offset % width) wasm_rt_trap(WASM_RT_TRAP_UNALIGNED);
  if (offset > memory->size || memory->size - offset < width)
    wasm_rt_trap(WASM_RT_TRAP_OOB);
  return (uint8_t *)(void *)memory->data + offset;
}

static uint32_t waitFor(wasm_rt_shared_memory_t *memory, uint64_t offset,
                        uint64_t expected, uint64_t timeoutBits, uint32_t width) {
  void *address = checkedAddress(memory, offset, width);
  // Completed waits and zero-time probes do not join the queue. Their atomic
  // load is the linearization point, so unrelated engine jobs need no mutex.
  uint64_t observed = width == 4 ? atomic_load((_Atomic uint32_t *)address)
                                 : atomic_load((_Atomic uint64_t *)address);
  if (observed != expected) return 1;
  if (!timeoutBits) return 2;

  WaiterBucket *bucket = bucketFor(address);
  pthread_mutex_lock(&bucket->lock);
  // A notification may have raced the initial load. Check again while holding
  // the queue lock before enqueuing, preserving the no-lost-wakeup protocol.
  uint64_t value = width == 4 ? atomic_load((_Atomic uint32_t *)address)
                              : atomic_load((_Atomic uint64_t *)address);
  if (value != expected) {
    pthread_mutex_unlock(&bucket->lock);
    return 1;
  }
  int64_t timeout = (int64_t)timeoutBits;
  Waiter waiter = {.address = address, .notified = false, .next = bucket->waiters};
  pthread_cond_init(&waiter.condition, NULL);
  bucket->waiters = &waiter;
  struct timespec deadline;
  if (timeout > 0) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout / 1000000000;
    deadline.tv_nsec += timeout % 1000000000;
    if (deadline.tv_nsec >= 1000000000) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }
  }
  int result = 0;
  while (!waiter.notified && !result) {
    if (timeout < 0) {
      result = pthread_cond_wait(&waiter.condition, &bucket->lock);
    } else {
      // Apple's relative wait uses monotonic time, so wall-clock changes cannot
      // extend or shorten an engine sleep. Recalculate after a spurious wakeup.
      struct timespec now, remaining;
      clock_gettime(CLOCK_MONOTONIC, &now);
      remaining.tv_sec = deadline.tv_sec - now.tv_sec;
      remaining.tv_nsec = deadline.tv_nsec - now.tv_nsec;
      if (remaining.tv_nsec < 0) { remaining.tv_sec--; remaining.tv_nsec += 1000000000; }
      if (remaining.tv_sec < 0) result = ETIMEDOUT;
      else result = pthread_cond_timedwait_relative_np(&waiter.condition, &bucket->lock, &remaining);
    }
  }
  Waiter **link = &bucket->waiters;
  while (*link != &waiter) link = &(*link)->next;
  *link = waiter.next;
  bool notified = waiter.notified;
  pthread_mutex_unlock(&bucket->lock);
  pthread_cond_destroy(&waiter.condition);
  return notified ? 0 : 2;
}

uint32_t pg_atomic_wait32(wasm_rt_shared_memory_t *m, uint64_t a, uint32_t v,
                           uint64_t timeout) { return waitFor(m, a, v, timeout, 4); }
uint32_t pg_atomic_wait64(wasm_rt_shared_memory_t *m, uint64_t a, uint64_t v,
                           uint64_t timeout) { return waitFor(m, a, v, timeout, 8); }

uint32_t pg_atomic_notify(wasm_rt_shared_memory_t *m, uint64_t offset,
                           uint32_t count) {
  void *address = checkedAddress(m, offset, 4);
  if (!count) return 0;
  uint32_t total = 0;
  WaiterBucket *bucket = bucketFor(address);
  pthread_mutex_lock(&bucket->lock);
  for (Waiter *w = bucket->waiters; w && total < count; w = w->next) {
    if (w->address == address && !w->notified) {
      w->notified = true;
      pthread_cond_signal(&w->condition);
      total++;
    }
  }
  pthread_mutex_unlock(&bucket->lock);
  return total;
}

#if defined(PG_FUTEX_TESTING)
// Narrow test hooks: they let futex_waits.c hold one address bucket while
// proving that operations on another bucket can still make progress.
void pg_futex_test_lock_address(const void *address) {
  pthread_mutex_lock(&bucketFor(address)->lock);
}
void pg_futex_test_unlock_address(const void *address) {
  pthread_mutex_unlock(&bucketFor(address)->lock);
}
unsigned pg_futex_test_bucket_index(const void *address) {
  (void)bucketFor(address);
  return bucketIndex(address);
}
unsigned pg_futex_test_waiter_count(const void *address) {
  unsigned count = 0;
  WaiterBucket *bucket = bucketFor(address);
  pthread_mutex_lock(&bucket->lock);
  for (Waiter *w = bucket->waiters; w; w = w->next)
    if (w->address == address) ++count;
  pthread_mutex_unlock(&bucket->lock);
  return count;
}
#endif
