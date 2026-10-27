#define _POSIX_C_SOURCE 200809L
#include "NativeLocalAssetReader.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PG_LOCAL_ASSET_SLOTS 64

typedef struct {
  char *path;
  int descriptor;
  unsigned readers;
  uint64_t used;
  bool occupied, retired;
} PGLocalAssetSlot;

static pthread_mutex_t localAssetLock = PTHREAD_MUTEX_INITIALIZER;
static PGLocalAssetSlot localAssets[PG_LOCAL_ASSET_SLOTS];
static uint64_t localAssetClock, localAssetGeneration;
#ifdef PG_LOCAL_ASSET_TESTING
static _Atomic uint64_t readerOpens, readerCloses, readerHits, readerEvictions, readerUncached;
#define COUNT(counter) atomic_fetch_add_explicit(&(counter), 1, memory_order_relaxed)
#else
#define COUNT(counter) ((void)0)
#endif

static void closeDescriptor(int descriptor) {
  // Do not retry close after EINTR: the descriptor may already have been
  // consumed, and another thread could acquire its number in the meantime.
  if (descriptor >= 0) { close(descriptor); COUNT(readerCloses); }
}

static int matchingSlot(const char *path) {
  for (int i = 0; i < PG_LOCAL_ASSET_SLOTS; ++i)
    if (localAssets[i].occupied && !localAssets[i].retired && !strcmp(localAssets[i].path, path)) return i;
  return -1;
}

static int leaseSlot(int slot, PGLocalAssetLease *lease) {
  PGLocalAssetSlot *entry = &localAssets[slot];
  entry->readers++;
  entry->used = ++localAssetClock;
  lease->slot = slot;
  lease->descriptor = entry->descriptor;
  COUNT(readerHits);
  return entry->descriptor;
}

int pg_local_asset_try_cached(const char *path, PGLocalAssetLease *lease) {
  if (!lease) { errno=EINVAL; return -1; }
  lease->descriptor=lease->slot=-1;
  if (!path || !*path) { errno=EINVAL; return -1; }
  if (pthread_mutex_trylock(&localAssetLock)) { errno=EBUSY; return -1; }
  int found=matchingSlot(path);
  int descriptor=found>=0?leaseSlot(found,lease):-1;
  pthread_mutex_unlock(&localAssetLock);
  if (descriptor<0) errno=ENOENT;
  return descriptor;
}

static bool releaseIdleDescriptor(void) {
  int slot = -1, descriptor = -1;
  char *path = NULL;
  pthread_mutex_lock(&localAssetLock);
  for (int i = 0; i < PG_LOCAL_ASSET_SLOTS; ++i)
    if (localAssets[i].occupied && !localAssets[i].readers &&
        (slot < 0 || localAssets[i].used < localAssets[slot].used)) slot = i;
  if (slot >= 0) {
    descriptor = localAssets[slot].descriptor;
    path = localAssets[slot].path;
    localAssets[slot] = (PGLocalAssetSlot){0};
    COUNT(readerEvictions);
  }
  pthread_mutex_unlock(&localAssetLock);
  closeDescriptor(descriptor);
  free(path);
  return descriptor >= 0;
}

static int acquireLocalAsset(const char *path, PGLocalAssetLease *lease, bool retain) {
  if (!lease) { errno = EINVAL; return -1; }
  lease->descriptor = lease->slot = -1;
  if (!path || !*path) { errno = EINVAL; return -1; }
  pthread_mutex_lock(&localAssetLock);
  int found = matchingSlot(path);
  if (found >= 0) {
    int descriptor = leaseSlot(found, lease);
    pthread_mutex_unlock(&localAssetLock);
    return descriptor;
  }
  uint64_t generation = localAssetGeneration;
  pthread_mutex_unlock(&localAssetLock);

  int descriptor;
  for (;;) {
    do { descriptor = open(path, O_RDONLY | O_CLOEXEC); } while (descriptor < 0 && errno == EINTR);
    if (descriptor >= 0) break;
    int error = errno;
    // A cache is optional: never make a read fail because retained idle
    // descriptors used up the process's available descriptor budget.
    if ((error == EMFILE || error == ENFILE) && releaseIdleDescriptor()) continue;
    errno = error;
    return -1;
  }
  if (descriptor < 0) return -1;
  COUNT(readerOpens);
  if (!retain) {
    lease->descriptor = descriptor; COUNT(readerUncached); return descriptor;
  }
  char *copy = strdup(path);
  int closeAfter = -1;
  char *freeAfter = NULL;
  pthread_mutex_lock(&localAssetLock);
  // A configuration change while open was in progress must not publish that
  // old generation's descriptor into the freshly cleared cache.
  if (generation == localAssetGeneration && copy) {
    found = matchingSlot(path);
    if (found >= 0) {
      closeAfter = descriptor;
      descriptor = leaseSlot(found, lease);
      freeAfter = copy;
    } else {
      int slot = -1;
      for (int i = 0; i < PG_LOCAL_ASSET_SLOTS; ++i) {
        if (!localAssets[i].occupied) { slot = i; break; }
        if (!localAssets[i].readers && (slot < 0 || localAssets[i].used < localAssets[slot].used)) slot = i;
      }
      if (slot >= 0) {
        PGLocalAssetSlot *entry = &localAssets[slot];
        if (entry->occupied) {
          closeAfter = entry->descriptor;
          freeAfter = entry->path;
          COUNT(readerEvictions);
        }
        *entry = (PGLocalAssetSlot){.path=copy, .descriptor=descriptor, .readers=1,
                                   .used=++localAssetClock, .occupied=true};
        lease->descriptor = descriptor;
        lease->slot = slot;
      }
    }
  }
  if (lease->descriptor < 0) {
    // All retained descriptors are leased (or the optional cache allocation
    // failed). Reading still works using the original open/read/close path.
    lease->descriptor = descriptor;
    freeAfter = copy;
    COUNT(readerUncached);
  }
  pthread_mutex_unlock(&localAssetLock);
  closeDescriptor(closeAfter);
  free(freeAfter);
  return descriptor;
}

int pg_local_asset_acquire(const char *path, PGLocalAssetLease *lease) {
  return acquireLocalAsset(path, lease, true);
}
int pg_local_asset_acquire_transient(const char *path, PGLocalAssetLease *lease) {
  return acquireLocalAsset(path, lease, false);
}

void pg_local_asset_release(PGLocalAssetLease *lease) {
  if (!lease || lease->descriptor < 0) return;
  int savedError = errno, closeAfter = -1;
  char *freeAfter = NULL;
  if (lease->slot < 0) closeAfter = lease->descriptor;
  else {
    pthread_mutex_lock(&localAssetLock);
    PGLocalAssetSlot *entry = &localAssets[lease->slot];
    if (entry->occupied && entry->descriptor == lease->descriptor && entry->readers) {
      entry->readers--;
      if (!entry->readers && entry->retired) {
        closeAfter = entry->descriptor;
        freeAfter = entry->path;
        *entry = (PGLocalAssetSlot){0};
      }
    }
    pthread_mutex_unlock(&localAssetLock);
  }
  lease->descriptor = lease->slot = -1;
  closeDescriptor(closeAfter);
  free(freeAfter);
  errno = savedError;
}

void pg_local_asset_reader_clear(void) {
  int savedError = errno;
  int descriptors[PG_LOCAL_ASSET_SLOTS], count = 0;
  char *paths[PG_LOCAL_ASSET_SLOTS];
  pthread_mutex_lock(&localAssetLock);
  localAssetGeneration++;
  for (int i = 0; i < PG_LOCAL_ASSET_SLOTS; ++i) {
    PGLocalAssetSlot *entry = &localAssets[i];
    if (!entry->occupied) continue;
    if (entry->readers) entry->retired = true;
    else {
      descriptors[count] = entry->descriptor;
      paths[count++] = entry->path;
      *entry = (PGLocalAssetSlot){0};
    }
  }
  pthread_mutex_unlock(&localAssetLock);
  for (int i = 0; i < count; ++i) { closeDescriptor(descriptors[i]); free(paths[i]); }
  errno = savedError;
}

#ifdef PG_LOCAL_ASSET_TESTING
PGLocalAssetReaderStats pg_local_asset_reader_stats(void) {
  PGLocalAssetReaderStats out = {.opens=atomic_load(&readerOpens), .closes=atomic_load(&readerCloses),
    .hits=atomic_load(&readerHits), .evictions=atomic_load(&readerEvictions), .uncached=atomic_load(&readerUncached)};
  pthread_mutex_lock(&localAssetLock);
  for (int i = 0; i < PG_LOCAL_ASSET_SLOTS; ++i) if (localAssets[i].occupied) {
    out.cached++;
    out.leased += localAssets[i].readers;
  }
  pthread_mutex_unlock(&localAssetLock);
  return out;
}
#endif
