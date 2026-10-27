#pragma once
#include "NativePackedAssetCache.h"
#include <stdio.h>

// Tests inject sync failures without making production cache writes unreliable.
#ifndef PG_MIGRATION_FSYNC
#define PG_MIGRATION_FSYNC fsync
#endif
#ifndef PG_MIGRATION_MSYNC
#define PG_MIGRATION_MSYNC msync
#endif

// A legacy chunk may be the player's only offline copy. Remove it only after
// both packed files and their directory entries are durable. The caller holds
// the same per-asset lock used for reading/migrating these numeric chunks.
static inline unsigned pg_packed_prune_legacy(PGPackedAssetCache *cache,
 uint64_t first,uint64_t last,const char *legacyDirectory,const char *packedDirectory){
 if(!cache||!cache->bits||first>last||last>=cache->blocks)return 0;
 for(uint64_t block=first;block<=last;block++)if(!pg_packed_has(cache,block))return 0;
 if(PG_MIGRATION_FSYNC(cache->dataFD)||PG_MIGRATION_MSYNC(cache->bits,cache->mapBytes,MS_SYNC)||PG_MIGRATION_FSYNC(cache->mapFD))return 0;
 int packed=open(packedDirectory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
 if(packed<0)return 0;
 int synced=PG_MIGRATION_FSYNC(packed);close(packed);if(synced)return 0;
 int legacy=open(legacyDirectory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);
 if(legacy<0)return 0;
 // Packed-v1 may have just been created next to this legacy directory. Its
 // own fsync does not make that link in cacheRoot durable.
 int parent=openat(legacy,"..",O_RDONLY|O_DIRECTORY);
 if(parent<0){close(legacy);return 0;}
 synced=PG_MIGRATION_FSYNC(parent);close(parent);
 if(synced){close(legacy);return 0;}
 unsigned removed=0;
 for(uint64_t block=first;block<=last;block++){
  char name[32];snprintf(name,sizeof name,"%llu",(unsigned long long)block);
  struct stat status;
  if(!fstatat(legacy,name,&status,AT_SYMLINK_NOFOLLOW)&&S_ISREG(status.st_mode)&&unlinkat(legacy,name,0)==0)removed++;
 }
 close(legacy);return removed;
}
