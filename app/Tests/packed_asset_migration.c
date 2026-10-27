#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
static int fail_sync=0,sync_calls=0;
static int migration_fsync(int fd){sync_calls++;if(fail_sync==1||(fail_sync==3&&sync_calls==4)){errno=EIO;return -1;}return fsync(fd);}
static int migration_msync(void *p,size_t n,int flags){if(fail_sync==2){errno=EIO;return -1;}return msync(p,n,flags);}
#define PG_MIGRATION_FSYNC migration_fsync
#define PG_MIGRATION_MSYNC migration_msync
#include "NativePackedAssetMigration.h"
#include <assert.h>
#include <stdlib.h>

int main(void){
 char root[]="/tmp/pg-migration-XXXXXX";assert(mkdtemp(root));
 char data[512],map[512],legacy[512],packed[512],chunk[1024],link[512],outside[512];
 snprintf(packed,sizeof packed,"%s/Packed-v1",root);assert(mkdir(packed,0700)==0);
 snprintf(data,sizeof data,"%s/Packed-v1/data",root);snprintf(map,sizeof map,"%s/Packed-v1/map",root);
 snprintf(legacy,sizeof legacy,"%s/legacy",root);assert(mkdir(legacy,0700)==0);
 snprintf(chunk,sizeof chunk,"%s/0",legacy);
 unsigned char source[4096];for(unsigned i=0;i<sizeof source;i++)source[i]=(unsigned char)i;
 int fd=open(chunk,O_WRONLY|O_CREAT,0600);assert(fd>=0&&write(fd,source,sizeof source)==sizeof source);close(fd);
 PGPackedAssetCache cache;assert(pg_packed_open(&cache,data,map,sizeof source));
 assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(chunk,F_OK)==0);
 int savedFD=cache.dataFD;cache.dataFD=-1;
 assert(!pg_packed_store(&cache,0,source,sizeof source));
 assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(chunk,F_OK)==0);
 cache.dataFD=savedFD;
 assert(pg_packed_store(&cache,0,source,sizeof source));
 fail_sync=1;assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(chunk,F_OK)==0);
 fail_sync=2;assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(chunk,F_OK)==0);
 fail_sync=3;sync_calls=0;assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(chunk,F_OK)==0);
 fail_sync=0;
 // Neither a symlinked directory nor a symlinked numeric chunk is followed.
 snprintf(link,sizeof link,"%s/link",root);assert(symlink(legacy,link)==0);
 assert(pg_packed_prune_legacy(&cache,0,0,link,packed)==0&&access(chunk,F_OK)==0);unlink(link);
 assert(symlink(packed,link)==0);
 assert(pg_packed_prune_legacy(&cache,0,0,legacy,link)==0&&access(chunk,F_OK)==0);unlink(link);
 snprintf(outside,sizeof outside,"%s/protected",root);assert(rename(chunk,outside)==0&&symlink(outside,chunk)==0);
 assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==0&&access(outside,F_OK)==0);
 unlink(chunk);assert(rename(outside,chunk)==0);
 assert(pg_packed_prune_legacy(&cache,0,0,legacy,packed)==1&&access(chunk,F_OK)!=0);
 unsigned char copy[4096];assert(pg_packed_read(&cache,0,copy,sizeof copy)==sizeof copy&&!memcmp(copy,source,sizeof copy));
 pg_packed_close(&cache);assert(pg_packed_open(&cache,data,map,sizeof source));
 assert(pg_packed_read(&cache,0,copy,sizeof copy)==sizeof copy&&!memcmp(copy,source,sizeof copy));
 pg_packed_close(&cache);unlink(data);unlink(map);rmdir(legacy);rmdir(packed);rmdir(root);
 puts("PASS: packed migration preserves offline chunks on missing bits/sync failure, rejects symlinks, removes only durable duplicates, restart reads exact.");
 return 0;
}
