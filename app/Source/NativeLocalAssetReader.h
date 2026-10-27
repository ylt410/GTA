#ifndef PG_NATIVE_LOCAL_ASSET_READER_H
#define PG_NATIVE_LOCAL_ASSET_READER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A read owns its descriptor until release. Clearing the cache retires leased
// descriptors instead of closing them underneath an in-flight pread.
typedef struct {
  int descriptor;
  int slot;
} PGLocalAssetLease;

int pg_local_asset_acquire(const char *path, PGLocalAssetLease *lease);
// A complete one-shot loose resource can reuse an existing entry, but a miss
// must not evict hot archive descriptors just to retain a file already read.
int pg_local_asset_acquire_transient(const char *path, PGLocalAssetLease *lease);
// Optional readahead must not wait on demand reads or populate their LRU.
// EBUSY means the cache lock was occupied; ENOENT means no cached descriptor.
int pg_local_asset_try_cached(const char *path, PGLocalAssetLease *lease);
void pg_local_asset_release(PGLocalAssetLease *lease);
void pg_local_asset_reader_clear(void);

#ifdef PG_LOCAL_ASSET_TESTING
typedef struct {
  uint64_t opens, closes, hits, evictions, uncached;
  unsigned cached, leased;
} PGLocalAssetReaderStats;
PGLocalAssetReaderStats pg_local_asset_reader_stats(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
