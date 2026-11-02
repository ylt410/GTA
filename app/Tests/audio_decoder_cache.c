#define _POSIX_C_SOURCE 200809L
#include "../Source/AudioDecoderCache.c"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void pauseBriefly(void) { struct timespec t = { .tv_nsec = 20000000 }; nanosleep(&t, NULL); }
static void freeValue(void *value) { free(value); }
static int *newValue(uint64_t value) { int *p = malloc(sizeof *p); assert(p); *p = (int)value; return p; }

typedef struct { PGAudioDecoderCache *cache; uint64_t key; atomic_bool started, done; bool initializing; int value; } AcquireJob;
static void *acquireThread(void *arg) {
  AcquireJob *job = arg;
  atomic_store(&job->started, true);
  PGAudioDecoderLease lease;
  assert(PGAudioDecoderCacheAcquire(job->cache, job->key, &lease));
  job->initializing = lease.initializing;
  if (lease.initializing) {
    freeValue(PGAudioDecoderLeaseValue(&lease));
    PGAudioDecoderLeaseSetValue(&lease, newValue(job->key));
    PGAudioDecoderCachePublish(&lease);
  } else job->value = *(int *)PGAudioDecoderLeaseValue(&lease);
  atomic_store(&job->done, true);
  PGAudioDecoderCacheRelease(&lease);
  return NULL;
}

static void testCapacityAndEviction(void) {
  PGAudioDecoderCache cache; assert(PGAudioDecoderCacheInit(&cache));
  PGAudioDecoderLease leases[PG_AUDIO_DECODER_CACHE_CAPACITY];
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
    assert(PGAudioDecoderCacheAcquire(&cache, i + 1, &leases[i]));
    assert(leases[i].initializing);
    PGAudioDecoderLeaseSetValue(&leases[i], newValue(i + 1));
    PGAudioDecoderCachePublish(&leases[i]);
  }

  AcquireJob waiting = { .cache = &cache, .key = 1000 };
  pthread_t thread; assert(!pthread_create(&thread, NULL, acquireThread, &waiting));
  while (!atomic_load(&waiting.started)) pauseBriefly();
  pauseBriefly();
  assert(!atomic_load(&waiting.done));
  assert(cache.slots[leases[0].slot - cache.slots].key == 1);
  PGAudioDecoderCacheRelease(&leases[0]);
  assert(!pthread_join(thread, NULL));
  assert(waiting.initializing);
  for (unsigned i = 1; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
    assert(*(int *)PGAudioDecoderLeaseValue(&leases[i]) == (int)i + 1);
    PGAudioDecoderCacheRelease(&leases[i]);
  }

  // New key takes the single unpinned LRU victim; every live key above survived.
  PGAudioDecoderLease hit; assert(PGAudioDecoderCacheAcquire(&cache, 1000, &hit));
  assert(!hit.initializing && *(int *)PGAudioDecoderLeaseValue(&hit) == 1000);
  PGAudioDecoderCacheRelease(&hit);
  PGAudioDecoderLease evicted; assert(PGAudioDecoderCacheAcquire(&cache, 1, &evicted));
  assert(evicted.initializing);
  freeValue(PGAudioDecoderLeaseValue(&evicted));
  PGAudioDecoderLeaseSetValue(&evicted, newValue(1));
  PGAudioDecoderCachePublish(&evicted);
  PGAudioDecoderCacheRelease(&evicted);
  PGAudioDecoderCacheDestroy(&cache, freeValue);
}

typedef struct { PGAudioDecoderCache *cache; int id, *order, *position; atomic_bool attempted; } SerialJob;
static void *sameKeyThread(void *arg) {
  SerialJob *job = arg; atomic_store(&job->attempted, true);
  PGAudioDecoderLease lease; assert(PGAudioDecoderCacheAcquire(job->cache, 7, &lease));
  job->order[(*job->position)++] = job->id;
  PGAudioDecoderCacheRelease(&lease);
  return NULL;
}

static void waitForTicket(PGAudioDecoderCache *cache, size_t slotIndex, uint64_t ticket) {
  for (;;) {
    pthread_mutex_lock(&cache->lock);
    bool assigned = cache->slots[slotIndex].nextTicket >= ticket;
    pthread_mutex_unlock(&cache->lock);
    if (assigned) return;
    pauseBriefly();
  }
}

static void testSameKeySerialAndDifferentKeyProgress(void) {
  PGAudioDecoderCache cache; assert(PGAudioDecoderCacheInit(&cache));
  PGAudioDecoderLease held; assert(PGAudioDecoderCacheAcquire(&cache, 7, &held));
  PGAudioDecoderLeaseSetValue(&held, newValue(7)); PGAudioDecoderCachePublish(&held);
  size_t slotIndex = (size_t)(held.slot - cache.slots);
  int order[2] = {0}, position = 0;
  SerialJob first = { .cache = &cache, .id = 1, .order = order, .position = &position };
  SerialJob second = { .cache = &cache, .id = 2, .order = order, .position = &position };
  pthread_t firstThread, secondThread;
  assert(!pthread_create(&firstThread, NULL, sameKeyThread, &first));
  while (!atomic_load(&first.attempted)) pauseBriefly();
  waitForTicket(&cache, slotIndex, 2);
  assert(!pthread_create(&secondThread, NULL, sameKeyThread, &second));
  while (!atomic_load(&second.attempted)) pauseBriefly();
  waitForTicket(&cache, slotIndex, 3);
  pauseBriefly();
  assert(position == 0);

  AcquireJob other = { .cache = &cache, .key = 8 };
  pthread_t otherThread; assert(!pthread_create(&otherThread, NULL, acquireThread, &other));
  assert(!pthread_join(otherThread, NULL));
  assert(atomic_load(&other.done) && other.initializing);
  PGAudioDecoderCacheRelease(&held);
  assert(!pthread_join(firstThread, NULL));
  assert(!pthread_join(secondThread, NULL));
  assert(position == 2 && order[0] == 1 && order[1] == 2);
  PGAudioDecoderCacheDestroy(&cache, freeValue);
}

typedef struct { PGAudioDecoderCache *cache; atomic_bool started, done; } DestroyJob;
static void *destroyThread(void *arg) {
  DestroyJob *job = arg; atomic_store(&job->started, true);
  PGAudioDecoderCacheDestroy(job->cache, freeValue);
  atomic_store(&job->done, true);
  return NULL;
}

static void testDestroyWaitsForLease(void) {
  PGAudioDecoderCache cache; assert(PGAudioDecoderCacheInit(&cache));
  PGAudioDecoderLease held; assert(PGAudioDecoderCacheAcquire(&cache, 1, &held));
  PGAudioDecoderLeaseSetValue(&held, newValue(1)); PGAudioDecoderCachePublish(&held);
  DestroyJob job = { .cache = &cache };
  pthread_t thread; assert(!pthread_create(&thread, NULL, destroyThread, &job));
  while (!atomic_load(&job.started)) pauseBriefly();
  pauseBriefly(); assert(!atomic_load(&job.done));
  PGAudioDecoderCacheRelease(&held);
  assert(!pthread_join(thread, NULL));
  assert(atomic_load(&job.done));
}

typedef struct { PGAudioDecoderCache *cache; atomic_bool started, done; bool acquired; } ClosingWaiterJob;
static void *closingWaiterThread(void *arg) {
  ClosingWaiterJob *job = arg; atomic_store(&job->started, true);
  PGAudioDecoderLease lease;
  job->acquired = PGAudioDecoderCacheAcquire(job->cache, 9999, &lease);
  if (job->acquired) PGAudioDecoderCacheRelease(&lease);
  atomic_store(&job->done, true);
  return NULL;
}

static void testDestroyWakesAcquireWaiter(void) {
  PGAudioDecoderCache cache; assert(PGAudioDecoderCacheInit(&cache));
  PGAudioDecoderLease leases[PG_AUDIO_DECODER_CACHE_CAPACITY];
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
    assert(PGAudioDecoderCacheAcquire(&cache, i + 1, &leases[i]));
    PGAudioDecoderLeaseSetValue(&leases[i], newValue(i + 1)); PGAudioDecoderCachePublish(&leases[i]);
  }
  ClosingWaiterJob waiter = { .cache = &cache };
  pthread_t waiterThread; assert(!pthread_create(&waiterThread, NULL, closingWaiterThread, &waiter));
  while (!atomic_load(&waiter.started)) pauseBriefly();
  while ((atomic_load(&cache.lifecycle) & (UINT64_MAX >> 1)) == 0) pauseBriefly();

  DestroyJob destroy = { .cache = &cache };
  pthread_t destroyer; assert(!pthread_create(&destroyer, NULL, destroyThread, &destroy));
  for (;;) {
    pthread_mutex_lock(&cache.lock);
    bool closing = cache.closing;
    pthread_mutex_unlock(&cache.lock);
    if (closing) break;
    pauseBriefly();
  }
  assert(!pthread_join(waiterThread, NULL));
  assert(atomic_load(&waiter.done) && !waiter.acquired);
  assert(!atomic_load(&destroy.done));
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) PGAudioDecoderCacheRelease(&leases[i]);
  assert(!pthread_join(destroyer, NULL));
  assert(atomic_load(&destroy.done));
}

typedef struct { int32_t sampleRate, channels, gain; unsigned resets; } FakeDecoder;
static atomic_int fakeCreateFailures, fakeDestroyed;
static void *newFakeDecoder(int32_t sampleRate, int32_t channels) {
  if (atomic_load(&fakeCreateFailures)) { atomic_fetch_sub(&fakeCreateFailures, 1); return NULL; }
  FakeDecoder *decoder = calloc(1, sizeof *decoder); assert(decoder);
  decoder->sampleRate = sampleRate; decoder->channels = channels;
  return decoder;
}
static bool fakeDecoderMatches(void *value, int32_t sampleRate, int32_t channels) {
  FakeDecoder *decoder = value;
  return decoder->sampleRate == sampleRate && decoder->channels == channels;
}
static void destroyFakeDecoder(void *value) {
  if (value) { atomic_fetch_add(&fakeDestroyed, 1); free(value); }
}

static void testPayloadCreateFormatChangeAndFailure(void) {
  PGAudioDecoderCache cache; assert(PGAudioDecoderCacheInit(&cache));
  PGAudioDecoderLease lease; void *opaque = NULL;
  assert(PGAudioDecoderCacheAcquire(&cache, 41, &lease));
  assert(PGAudioDecoderLeaseEnsureValue(&lease, 48000, 2, newFakeDecoder, fakeDecoderMatches,
                                        destroyFakeDecoder, &opaque));
  FakeDecoder *decoder = opaque; assert(decoder->gain == 0 && decoder->resets == 0);
  decoder->gain = 11; decoder->resets = 3;
  PGAudioDecoderCacheRelease(&lease);

  assert(PGAudioDecoderCacheAcquire(&cache, 41, &lease));
  assert(!lease.initializing);
  assert(PGAudioDecoderLeaseEnsureValue(&lease, 48000, 2, newFakeDecoder, fakeDecoderMatches,
                                        destroyFakeDecoder, &opaque));
  assert(opaque == decoder && ((FakeDecoder *)opaque)->gain == 11 && ((FakeDecoder *)opaque)->resets == 3);
  PGAudioDecoderCacheRelease(&lease);

  assert(PGAudioDecoderCacheAcquire(&cache, 41, &lease));
  assert(PGAudioDecoderLeaseEnsureValue(&lease, 44100, 1, newFakeDecoder, fakeDecoderMatches,
                                        destroyFakeDecoder, &opaque));
  decoder = opaque;
  assert(decoder->sampleRate == 44100 && decoder->channels == 1 && decoder->gain == 0 && decoder->resets == 0);
  assert(atomic_load(&fakeDestroyed) == 1);  // format change destroyed the previous decoder
  PGAudioDecoderCacheRelease(&lease);

  atomic_store(&fakeCreateFailures, 1);
  assert(PGAudioDecoderCacheAcquire(&cache, 42, &lease));
  assert(!PGAudioDecoderLeaseEnsureValue(&lease, 48000, 2, newFakeDecoder, fakeDecoderMatches,
                                         destroyFakeDecoder, &opaque));
  assert(!lease.cache);  // failed new-slot creation canceled the reservation and unpinned it
  assert(PGAudioDecoderCacheAcquire(&cache, 42, &lease));
  assert(PGAudioDecoderLeaseEnsureValue(&lease, 48000, 2, newFakeDecoder, fakeDecoderMatches,
                                        destroyFakeDecoder, &opaque));
  PGAudioDecoderCacheRelease(&lease);

  atomic_store(&fakeCreateFailures, 1);
  assert(PGAudioDecoderCacheAcquire(&cache, 42, &lease));
  assert(!PGAudioDecoderLeaseEnsureValue(&lease, 44100, 1, newFakeDecoder, fakeDecoderMatches,
                                         destroyFakeDecoder, &opaque));
  assert(!lease.cache);  // failed reconfiguration also releases the lease
  assert(PGAudioDecoderCacheAcquire(&cache, 42, &lease));
  assert(PGAudioDecoderLeaseEnsureValue(&lease, 44100, 1, newFakeDecoder, fakeDecoderMatches,
                                        destroyFakeDecoder, &opaque));
  PGAudioDecoderCacheRelease(&lease);
  PGAudioDecoderCacheDestroy(&cache, destroyFakeDecoder);
}

typedef struct { PGAudioModeState *state; atomic_int *calls; int value; } ModeJob;
static int chooseMode(void *context) {
  ModeJob *job = context; atomic_fetch_add(job->calls, 1); pauseBriefly(); return job->value;
}
static void *modeThread(void *arg) {
  ModeJob *job = arg; assert(PGAudioModeGet(job->state, chooseMode, job) == job->value); return NULL;
}

typedef struct { PGAudioDecoderStats *stats; unsigned count; } StatsJob;
static void *statsThread(void *arg) {
  StatsJob *job = arg;
  for (unsigned i = 0; i < job->count; i++)
    PGAudioDecoderStatsRecord(job->stats, 1, i % 2, i % 3 == 0, (float)(i % 100) / 100.0f, 0.001, 0.002);
  return NULL;
}

static void testModeAndStatsConcurrency(void) {
  PGAudioModeState mode = PG_AUDIO_MODE_STATE_INITIALIZER; atomic_int calls = 0;
  ModeJob modeJob = { .state = &mode, .calls = &calls, .value = 2 };
  pthread_t modes[24];
  for (unsigned i = 0; i < 24; i++) assert(!pthread_create(&modes[i], NULL, modeThread, &modeJob));
  for (unsigned i = 0; i < 24; i++) assert(!pthread_join(modes[i], NULL));
  assert(atomic_load(&calls) == 1);
  PGAudioModeDestroy(&mode);

  PGAudioDecoderStats stats; assert(PGAudioDecoderStatsInit(&stats));
  enum { THREADS = 8, RECORDS = 5000 };
  StatsJob job = { .stats = &stats, .count = RECORDS }; pthread_t workers[THREADS];
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_create(&workers[i], NULL, statsThread, &job));
  for (unsigned i = 0; i < THREADS; i++) assert(!pthread_join(workers[i], NULL));
  PGAudioDecoderStatsSnapshot snap = PGAudioDecoderStatsRead(&stats);
  assert(snap.frames == (uint64_t)THREADS * RECORDS);
  assert(snap.lost == (uint64_t)THREADS * (RECORDS / 2));
  assert(snap.mismatched == (uint64_t)THREADS * ((RECORDS + 2) / 3));
  assert(snap.maxDiff == 0.99f);
  assert(PGAudioDecoderStatsLogDue(&stats, 12, 10, &snap));
  assert(!PGAudioDecoderStatsLogDue(&stats, 13, 10, &snap));
  PGAudioDecoderStatsDestroy(&stats);
}

int main(void) {
  testCapacityAndEviction();
  testSameKeySerialAndDifferentKeyProgress();
  testDestroyWaitsForLease();
  testDestroyWakesAcquireWaiter();
  testPayloadCreateFormatChangeAndFailure();
  testModeAndStatsConcurrency();
  puts("Audio decoder cache tests passed.");
  return 0;
}
