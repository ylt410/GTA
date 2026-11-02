#include "AudioDecoderCache.h"
#include <string.h>

#define PG_AUDIO_CACHE_CLOSED (UINT64_C(1) << 63)
#define PG_AUDIO_CACHE_CALLERS (PG_AUDIO_CACHE_CLOSED - 1)

bool PGAudioDecoderCacheInit(PGAudioDecoderCache *cache) {
  if (!cache) return false;
  memset(cache, 0, sizeof *cache);
  atomic_init(&cache->lifecycle, 0);
  if (pthread_mutex_init(&cache->lock, NULL)) return false;
  if (pthread_cond_init(&cache->changed, NULL)) { pthread_mutex_destroy(&cache->lock); return false; }
  unsigned initialized = 0;
  for (; initialized < PG_AUDIO_DECODER_CACHE_CAPACITY; initialized++) {
    PGAudioDecoderCacheSlot *slot = &cache->slots[initialized];
    if (pthread_mutex_init(&slot->decodeLock, NULL)) break;
    if (pthread_cond_init(&slot->decodeChanged, NULL)) {
      pthread_mutex_destroy(&slot->decodeLock);
      break;
    }
  }
  if (initialized == PG_AUDIO_DECODER_CACHE_CAPACITY) return true;
  while (initialized) {
    PGAudioDecoderCacheSlot *slot = &cache->slots[--initialized];
    pthread_cond_destroy(&slot->decodeChanged);
    pthread_mutex_destroy(&slot->decodeLock);
  }
  pthread_cond_destroy(&cache->changed);
  pthread_mutex_destroy(&cache->lock);
  return false;
}

static void clearLease(PGAudioDecoderLease *lease) {
  lease->cache = NULL;
  lease->slot = NULL;
  lease->initializing = false;
}

static bool enterAcquire(PGAudioDecoderCache *cache) {
  uint_fast64_t state = atomic_load_explicit(&cache->lifecycle, memory_order_acquire);
  for (;;) {
    if (state & PG_AUDIO_CACHE_CLOSED || (state & PG_AUDIO_CACHE_CALLERS) == PG_AUDIO_CACHE_CALLERS) return false;
    if (atomic_compare_exchange_weak_explicit(&cache->lifecycle, &state, state + 1,
                                               memory_order_acq_rel, memory_order_acquire)) return true;
  }
}

static void leaveAcquireLocked(PGAudioDecoderCache *cache) {
  atomic_fetch_sub_explicit(&cache->lifecycle, 1, memory_order_release);
  pthread_cond_broadcast(&cache->changed);
}

bool PGAudioDecoderCacheAcquire(PGAudioDecoderCache *cache, uint64_t key, PGAudioDecoderLease *lease) {
  if (!cache || !lease) return false;
  clearLease(lease);
  if (!enterAcquire(cache)) return false;
  pthread_mutex_lock(&cache->lock);
  for (;;) {
    if (cache->closing) {
      leaveAcquireLocked(cache);
      pthread_mutex_unlock(&cache->lock);
      return false;
    }
    PGAudioDecoderCacheSlot *hit = NULL;
    for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
      PGAudioDecoderCacheSlot *slot = &cache->slots[i];
      if (slot->state != PG_AUDIO_CACHE_EMPTY && slot->key == key) { hit = slot; break; }
    }
    if (hit && hit->state == PG_AUDIO_CACHE_INITIALIZING) {
      pthread_cond_wait(&cache->changed, &cache->lock);
      continue;
    }
    if (hit) {
      hit->pins++;
      hit->used = ++cache->useTick;
      uint64_t ticket = hit->nextTicket++;
      pthread_mutex_unlock(&cache->lock);
      pthread_mutex_lock(&hit->decodeLock);
      while (ticket != hit->servingTicket) pthread_cond_wait(&hit->decodeChanged, &hit->decodeLock);
      *lease = (PGAudioDecoderLease){ .cache = cache, .slot = hit, .ticket = ticket };
      pthread_mutex_lock(&cache->lock);
      leaveAcquireLocked(cache);
      pthread_mutex_unlock(&cache->lock);
      return true;
    }

    PGAudioDecoderCacheSlot *target = NULL;
    for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
      PGAudioDecoderCacheSlot *slot = &cache->slots[i];
      if (slot->state == PG_AUDIO_CACHE_EMPTY) { target = slot; break; }
      if (slot->state == PG_AUDIO_CACHE_READY && !slot->pins &&
          (!target || slot->used < target->used)) target = slot;
    }
    if (!target) {
      pthread_cond_wait(&cache->changed, &cache->lock);
      continue;
    }
    target->key = key;
    target->used = ++cache->useTick;
    target->nextTicket = 1;
    target->servingTicket = 0;
    target->pins = 1;
    target->state = PG_AUDIO_CACHE_INITIALIZING;
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_lock(&target->decodeLock);
    *lease = (PGAudioDecoderLease){ .cache = cache, .slot = target, .ticket = 0, .initializing = true };
    pthread_mutex_lock(&cache->lock);
    leaveAcquireLocked(cache);
    pthread_mutex_unlock(&cache->lock);
    return true;
  }
}

void *PGAudioDecoderLeaseValue(const PGAudioDecoderLease *lease) {
  return lease && lease->slot ? lease->slot->value : NULL;
}

void PGAudioDecoderLeaseSetValue(PGAudioDecoderLease *lease, void *value) {
  if (lease && lease->slot) lease->slot->value = value;
}

bool PGAudioDecoderLeaseEnsureValue(PGAudioDecoderLease *lease, int32_t sampleRate, int32_t channels,
                                   PGAudioCacheValueCreate createValue, PGAudioCacheValueMatches matches,
                                   PGAudioCacheValueDestroy destroyValue, void **value) {
  if (!lease || !lease->cache || !lease->slot || !createValue || !matches || !destroyValue || !value) return false;
  void *current = PGAudioDecoderLeaseValue(lease);
  if (!lease->initializing && current && matches(current, sampleRate, channels)) {
    *value = current;
    return true;
  }
  if (current) destroyValue(current);
  void *replacement = createValue(sampleRate, channels);
  if (!replacement) {
    if (lease->initializing) PGAudioDecoderCacheCancel(lease);
    else { PGAudioDecoderLeaseSetValue(lease, NULL); PGAudioDecoderCacheRelease(lease); }
    return false;
  }
  PGAudioDecoderLeaseSetValue(lease, replacement);
  if (lease->initializing) PGAudioDecoderCachePublish(lease);
  *value = replacement;
  return true;
}

void PGAudioDecoderCachePublish(PGAudioDecoderLease *lease) {
  if (!lease || !lease->cache || !lease->slot || !lease->initializing) return;
  PGAudioDecoderCache *cache = lease->cache;
  pthread_mutex_lock(&cache->lock);
  lease->slot->state = PG_AUDIO_CACHE_READY;
  lease->initializing = false;
  pthread_cond_broadcast(&cache->changed);
  pthread_mutex_unlock(&cache->lock);
}

void PGAudioDecoderCacheCancel(PGAudioDecoderLease *lease) {
  if (!lease || !lease->cache || !lease->slot || !lease->initializing) return;
  PGAudioDecoderCache *cache = lease->cache;
  PGAudioDecoderCacheSlot *slot = lease->slot;
  pthread_mutex_lock(&cache->lock);
  slot->key = 0;
  slot->used = 0;
  slot->value = NULL;
  slot->state = PG_AUDIO_CACHE_EMPTY;
  slot->nextTicket = slot->servingTicket = 0;
  slot->pins--;
  lease->initializing = false;
  pthread_cond_broadcast(&cache->changed);
  pthread_mutex_unlock(&cache->lock);
  pthread_cond_broadcast(&slot->decodeChanged);
  pthread_mutex_unlock(&slot->decodeLock);
  clearLease(lease);
}

void PGAudioDecoderCacheRelease(PGAudioDecoderLease *lease) {
  if (!lease || !lease->cache || !lease->slot || lease->initializing) return;
  PGAudioDecoderCache *cache = lease->cache;
  PGAudioDecoderCacheSlot *slot = lease->slot;
  slot->servingTicket++;
  pthread_cond_broadcast(&slot->decodeChanged);
  pthread_mutex_unlock(&slot->decodeLock);
  pthread_mutex_lock(&cache->lock);
  slot->pins--;
  pthread_cond_broadcast(&cache->changed);
  pthread_mutex_unlock(&cache->lock);
  clearLease(lease);
}

void PGAudioDecoderCacheDestroy(PGAudioDecoderCache *cache, PGAudioCacheValueDestroy destroyValue) {
  if (!cache) return;
  pthread_mutex_lock(&cache->lock);
  cache->closing = true;
  atomic_fetch_or_explicit(&cache->lifecycle, PG_AUDIO_CACHE_CLOSED, memory_order_acq_rel);
  pthread_cond_broadcast(&cache->changed);
  for (;;) {
    bool pinned = (atomic_load_explicit(&cache->lifecycle, memory_order_acquire) & PG_AUDIO_CACHE_CALLERS) != 0;
    for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++)
      if (cache->slots[i].pins) { pinned = true; break; }
    if (!pinned) break;
    pthread_cond_broadcast(&cache->changed);
    pthread_cond_wait(&cache->changed, &cache->lock);
  }
  pthread_mutex_unlock(&cache->lock);
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
    PGAudioDecoderCacheSlot *slot = &cache->slots[i];
    pthread_mutex_lock(&slot->decodeLock);
    if (slot->value && destroyValue) destroyValue(slot->value);
    slot->value = NULL;
    pthread_mutex_unlock(&slot->decodeLock);
    pthread_cond_destroy(&slot->decodeChanged);
    pthread_mutex_destroy(&slot->decodeLock);
  }
  pthread_cond_destroy(&cache->changed);
  pthread_mutex_destroy(&cache->lock);
}

bool PGAudioDecoderStatsInit(PGAudioDecoderStats *stats) {
  if (!stats) return false;
  memset(stats, 0, sizeof *stats);
  return pthread_mutex_init(&stats->lock, NULL) == 0;
}

void PGAudioDecoderStatsRecord(PGAudioDecoderStats *stats, uint64_t frames, uint64_t lost,
                               uint64_t mismatched, float maxDiff, double nativeSeconds, double wasmSeconds) {
  pthread_mutex_lock(&stats->lock);
  stats->frames += frames;
  stats->lost += lost;
  stats->mismatched += mismatched;
  if (maxDiff > stats->maxDiff) stats->maxDiff = maxDiff;
  stats->nativeSeconds += nativeSeconds;
  stats->wasmSeconds += wasmSeconds;
  pthread_mutex_unlock(&stats->lock);
}

PGAudioDecoderStatsSnapshot PGAudioDecoderStatsRead(PGAudioDecoderStats *stats) {
  pthread_mutex_lock(&stats->lock);
  PGAudioDecoderStatsSnapshot snapshot = { stats->frames, stats->lost, stats->mismatched,
    stats->maxDiff, stats->lastLog, stats->nativeSeconds, stats->wasmSeconds };
  pthread_mutex_unlock(&stats->lock);
  return snapshot;
}

bool PGAudioDecoderStatsLogDue(PGAudioDecoderStats *stats, double now, double interval,
                               PGAudioDecoderStatsSnapshot *snapshot) {
  pthread_mutex_lock(&stats->lock);
  bool due = now - stats->lastLog > interval;
  if (due) stats->lastLog = now;
  if (snapshot) *snapshot = (PGAudioDecoderStatsSnapshot){ stats->frames, stats->lost, stats->mismatched,
    stats->maxDiff, stats->lastLog, stats->nativeSeconds, stats->wasmSeconds };
  pthread_mutex_unlock(&stats->lock);
  return due;
}

void PGAudioDecoderStatsDestroy(PGAudioDecoderStats *stats) {
  if (stats) pthread_mutex_destroy(&stats->lock);
}

int PGAudioModeGet(PGAudioModeState *state, PGAudioModeInitializer initialize, void *context) {
  int current = atomic_load_explicit(&state->value, memory_order_acquire);
  if (current != -1) return current;
  pthread_mutex_lock(&state->lock);
  while (state->initializing && !state->initialized) pthread_cond_wait(&state->ready, &state->lock);
  if (state->initialized) {
    int value = atomic_load_explicit(&state->value, memory_order_relaxed);
    pthread_mutex_unlock(&state->lock);
    return value;
  }
  state->initializing = true;
  pthread_mutex_unlock(&state->lock);

  int value = initialize(context);

  pthread_mutex_lock(&state->lock);
  atomic_store_explicit(&state->value, value, memory_order_release);
  state->initialized = true;
  state->initializing = false;
  pthread_cond_broadcast(&state->ready);
  pthread_mutex_unlock(&state->lock);
  return value;
}

void PGAudioModeDestroy(PGAudioModeState *state) {
  if (!state) return;
  pthread_mutex_destroy(&state->lock);
  pthread_cond_destroy(&state->ready);
}
