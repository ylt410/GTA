#ifndef PG_AUDIO_DECODER_CACHE_H
#define PG_AUDIO_DECODER_CACHE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

enum { PG_AUDIO_DECODER_CACHE_CAPACITY = 128 };

typedef enum {
  PG_AUDIO_CACHE_EMPTY,
  PG_AUDIO_CACHE_INITIALIZING,
  PG_AUDIO_CACHE_READY
} PGAudioCacheState;

typedef struct {
  uint64_t key, used, nextTicket, servingTicket;
  void *value;
  unsigned pins;
  PGAudioCacheState state;
  pthread_mutex_t decodeLock;
  pthread_cond_t decodeChanged;
} PGAudioDecoderCacheSlot;

typedef struct {
  PGAudioDecoderCacheSlot slots[PG_AUDIO_DECODER_CACHE_CAPACITY];
  pthread_mutex_t lock;
  pthread_cond_t changed;
  uint64_t useTick;
  atomic_uint_fast64_t lifecycle; // high bit closes entry; low bits count Acquire callers in flight
  bool closing;
} PGAudioDecoderCache;

typedef struct {
  PGAudioDecoderCache *cache;
  PGAudioDecoderCacheSlot *slot;
  uint64_t ticket;
  bool initializing;
} PGAudioDecoderLease;

typedef void (*PGAudioCacheValueDestroy)(void *value);
typedef void *(*PGAudioCacheValueCreate)(int32_t sampleRate, int32_t channels);
typedef bool (*PGAudioCacheValueMatches)(void *value, int32_t sampleRate, int32_t channels);

bool PGAudioDecoderCacheInit(PGAudioDecoderCache *cache);
// On success, returns with the slot's decodeLock held and the slot pinned.
// New keys reserve an initializing slot; the caller must Publish or Cancel.
bool PGAudioDecoderCacheAcquire(PGAudioDecoderCache *cache, uint64_t key, PGAudioDecoderLease *lease);
void *PGAudioDecoderLeaseValue(const PGAudioDecoderLease *lease);
void PGAudioDecoderLeaseSetValue(PGAudioDecoderLease *lease, void *value);
// Replaces an evicted or incompatible payload. On creation failure, cancels an
// initializing lease or releases a ready lease so no cache pin/lock is stranded.
bool PGAudioDecoderLeaseEnsureValue(PGAudioDecoderLease *lease, int32_t sampleRate, int32_t channels,
                                   PGAudioCacheValueCreate createValue, PGAudioCacheValueMatches matches,
                                   PGAudioCacheValueDestroy destroyValue, void **value);
void PGAudioDecoderCachePublish(PGAudioDecoderLease *lease);
void PGAudioDecoderCacheCancel(PGAudioDecoderLease *lease);
void PGAudioDecoderCacheRelease(PGAudioDecoderLease *lease);
// Closes entry, wakes/waits for Acquire callers already waiting or entering, then waits
// for all leases and destroys payloads. Owner must stop launching calls before Destroy
// starts and join API-call threads before freeing the cache storage.
void PGAudioDecoderCacheDestroy(PGAudioDecoderCache *cache, PGAudioCacheValueDestroy destroyValue);

typedef struct {
  pthread_mutex_t lock;
  uint64_t frames, lost, mismatched;
  float maxDiff;
  double lastLog, nativeSeconds, wasmSeconds;
} PGAudioDecoderStats;

typedef struct {
  uint64_t frames, lost, mismatched;
  float maxDiff;
  double lastLog, nativeSeconds, wasmSeconds;
} PGAudioDecoderStatsSnapshot;

bool PGAudioDecoderStatsInit(PGAudioDecoderStats *stats);
void PGAudioDecoderStatsRecord(PGAudioDecoderStats *stats, uint64_t frames, uint64_t lost,
                               uint64_t mismatched, float maxDiff, double nativeSeconds, double wasmSeconds);
PGAudioDecoderStatsSnapshot PGAudioDecoderStatsRead(PGAudioDecoderStats *stats);
bool PGAudioDecoderStatsLogDue(PGAudioDecoderStats *stats, double now, double interval,
                               PGAudioDecoderStatsSnapshot *snapshot);
void PGAudioDecoderStatsDestroy(PGAudioDecoderStats *stats);

typedef struct {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  bool initialized, initializing;
  atomic_int value;
} PGAudioModeState;

typedef int (*PGAudioModeInitializer)(void *context);

#define PG_AUDIO_MODE_STATE_INITIALIZER { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, false, false, ATOMIC_VAR_INIT(-1) }
int PGAudioModeGet(PGAudioModeState *state, PGAudioModeInitializer initialize, void *context);
void PGAudioModeDestroy(PGAudioModeState *state);

#endif
