#define _POSIX_C_SOURCE 200809L
#define PG_LOCAL_ASSET_TESTING 1
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic int interruptOpen;
static _Atomic int pauseOpen;
static pthread_mutex_t openPauseLock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t openPauseCondition=PTHREAD_COND_INITIALIZER;
static int openPaused,openContinue;
static int probe_open(const char *path, int flags);
#define open probe_open
#include "../Source/NativeLocalAssetReader.c"
#undef open

static int probe_open(const char *path, int flags) {
  int fault=atomic_exchange(&interruptOpen, 0);
  if (fault) { errno = fault==1?EINTR:EMFILE; return -1; }
  int fd=open(path, flags);
  if(fd>=0&&atomic_exchange(&pauseOpen,0)){
    pthread_mutex_lock(&openPauseLock);openPaused=1;
    pthread_cond_signal(&openPauseCondition);
    while(!openContinue)pthread_cond_wait(&openPauseCondition,&openPauseLock);
    pthread_mutex_unlock(&openPauseLock);
  }
  return fd;
}

static char root[256], paths[70][320];
static unsigned char payload[4096];

static void makeFile(const char *path, const void *bytes, size_t count) {
  int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0600); assert(fd >= 0);
  assert(write(fd, bytes, count) == (ssize_t)count); assert(close(fd) == 0);
}

static void readCheck(const char *path, size_t start, size_t count) {
  PGLocalAssetLease lease;
  int fd = pg_local_asset_acquire(path, &lease); assert(fd >= 0);
  unsigned char bytes[256]; assert(count <= sizeof bytes);
  assert(pread(fd, bytes, count, (off_t)start) == (ssize_t)count);
  assert(!memcmp(bytes, payload+start, count));
  pg_local_asset_release(&lease); assert(lease.descriptor == -1);
}

static void testReuseAndFailures(void) {
  pg_local_asset_reader_clear();
  PGLocalAssetReaderStats before = pg_local_asset_reader_stats();
  for (int i=0;i<10000;i++) readCheck(paths[0], (unsigned)i%3000, 64);
  PGLocalAssetReaderStats after = pg_local_asset_reader_stats();
  assert(after.opens-before.opens == 1 && after.hits-before.hits == 9999);
  assert(after.cached == 1 && after.leased == 0);
  PGLocalAssetLease lease;
  errno=0; assert(pg_local_asset_acquire("/path/that/does/not/exist", &lease) == -1 && errno == ENOENT);
  assert(lease.descriptor == -1 && lease.slot == -1);
  assert(pg_local_asset_acquire(NULL, &lease) == -1 && errno == EINVAL);
  assert(pg_local_asset_acquire(paths[0], NULL) == -1 && errno == EINVAL);
  pg_local_asset_reader_clear();
  atomic_store(&interruptOpen,1);
  assert(pg_local_asset_acquire(paths[0], &lease)>=0);
  unsigned char bytes[16];
  assert(pread(lease.descriptor,bytes,sizeof bytes,sizeof payload)==0);
  assert(pread(lease.descriptor,bytes,sizeof bytes,-1)==-1&&errno==EINVAL);
  errno=EIO;pg_local_asset_release(&lease);assert(errno==EIO);
  atomic_store(&interruptOpen,2);
  assert(pg_local_asset_acquire(paths[1],&lease)>=0);
  pg_local_asset_release(&lease);
  assert(pg_local_asset_reader_stats().cached==1); // idle cache gave back a descriptor
  puts("PASS: 10000 repeated ranges use one open (9999 hits); missing files, open EINTR, EOF and read errno preserved.");
}

static void testClearAndReplace(void) {
  pg_local_asset_reader_clear();
  PGLocalAssetLease old;
  assert(pg_local_asset_acquire(paths[0],&old)>=0);
  int oldDescriptor=old.descriptor;
  char replacement[350];snprintf(replacement,sizeof replacement,"%s/replacement",root);
  const char updated[]="new inode";
  makeFile(replacement,updated,sizeof updated);
  assert(rename(replacement,paths[0])==0);
  // Game-file configuration is immutable while playing. An existing lease
  // remains a valid snapshot; reset/reapply starts new reads on the new inode.
  pg_local_asset_reader_clear();
  unsigned char byte=0;
  assert(pread(oldDescriptor,&byte,1,13)==1&&byte==payload[13]);
  PGLocalAssetLease now;
  assert(pg_local_asset_acquire(paths[0],&now)>=0);
  char got[sizeof updated];assert(pread(now.descriptor,got,sizeof got,0)==sizeof got);
  assert(!memcmp(got,updated,sizeof got));
  pg_local_asset_release(&old);assert(fcntl(oldDescriptor,F_GETFD)==-1&&errno==EBADF);
  assert(unlink(paths[0])==0);pg_local_asset_reader_clear();
  assert(pread(now.descriptor,got,sizeof got,0)==sizeof got);
  PGLocalAssetLease missing;
  assert(pg_local_asset_acquire(paths[0],&missing)==-1&&errno==ENOENT);
  pg_local_asset_release(&now);
  makeFile(paths[0],payload,sizeof payload);
  puts("PASS: atomic replacement/deletion after clear sees current paths; retired leases remain valid until release.");
}

static void *pausedAcquire(void *context){
  PGLocalAssetLease *lease=context;
  assert(pg_local_asset_acquire(paths[0],lease)>=0);
  return NULL;
}
static void testClearDuringOpen(void){
  pg_local_asset_reader_clear();
  PGLocalAssetLease lease;pthread_t reader;
  openPaused=openContinue=0;atomic_store(&pauseOpen,1);
  assert(!pthread_create(&reader,NULL,pausedAcquire,&lease));
  pthread_mutex_lock(&openPauseLock);
  while(!openPaused)pthread_cond_wait(&openPauseCondition,&openPauseLock);
  pthread_mutex_unlock(&openPauseLock);
  char replacement[350];snprintf(replacement,sizeof replacement,"%s/next-generation",root);
  const char current[]="current generation";makeFile(replacement,current,sizeof current);
  assert(!rename(replacement,paths[0]));pg_local_asset_reader_clear();
  pthread_mutex_lock(&openPauseLock);openContinue=1;
  pthread_cond_signal(&openPauseCondition);pthread_mutex_unlock(&openPauseLock);
  assert(!pthread_join(reader,NULL));assert(lease.slot==-1);
  unsigned char old;assert(pread(lease.descriptor,&old,1,37)==1&&old==payload[37]);
  pg_local_asset_release(&lease);
  PGLocalAssetLease next;assert(pg_local_asset_acquire(paths[0],&next)>=0);
  char actual[sizeof current];assert(pread(next.descriptor,actual,sizeof actual,0)==sizeof actual);
  assert(!memcmp(actual,current,sizeof actual));pg_local_asset_release(&next);
  pg_local_asset_reader_clear();makeFile(paths[0],payload,sizeof payload);
  puts("PASS: clear during open cannot publish an old-generation descriptor into the new cache.");
}

static void testSaturationAndEviction(void) {
  pg_local_asset_reader_clear();
  PGLocalAssetLease held[64],overflow;
  for(int i=0;i<64;i++)assert(pg_local_asset_acquire(paths[i],&held[i])>=0);
  PGLocalAssetReaderStats before=pg_local_asset_reader_stats();
  assert(before.cached==64&&before.leased==64);
  assert(pg_local_asset_acquire(paths[64],&overflow)>=0&&overflow.slot==-1);
  assert(pg_local_asset_reader_stats().uncached==before.uncached+1);
  unsigned char b;assert(pread(overflow.descriptor,&b,1,17)==1&&b==payload[17]);
  pg_local_asset_release(&overflow);
  pg_local_asset_reader_clear();
  for(int i=0;i<64;i++){
    assert(pread(held[i].descriptor,&b,1,19)==1&&b==payload[19]);
    pg_local_asset_release(&held[i]);
  }
  assert(pg_local_asset_reader_stats().cached==0);
  for(int i=0;i<70;i++)readCheck(paths[i],42,64);
  PGLocalAssetReaderStats after=pg_local_asset_reader_stats();
  assert(after.cached==64&&after.leased==0&&after.evictions>=6);
  pg_local_asset_reader_clear();
  puts("PASS: all 64 leased entries fall back without eviction; clear and LRU eviction close only idle entries.");
}

static void *readWorker(void *context) {
  uintptr_t worker=(uintptr_t)context;
  for(unsigned i=0;i<3000;i++)readCheck(paths[(i*13+worker)%70],(i*37+worker)%3000,64);
  return NULL;
}
static void *clearWorker(void *unused) {
  (void)unused;
  for(unsigned i=0;i<2000;i++)pg_local_asset_reader_clear();
  return NULL;
}
static void testConcurrentClear(void) {
  pthread_t readers[8],clearer;
  for(uintptr_t i=0;i<8;i++)assert(!pthread_create(&readers[i],NULL,readWorker,(void *)i));
  assert(!pthread_create(&clearer,NULL,clearWorker,NULL));
  for(int i=0;i<8;i++)assert(!pthread_join(readers[i],NULL));
  assert(!pthread_join(clearer,NULL));
  pg_local_asset_reader_clear();
  PGLocalAssetReaderStats stats=pg_local_asset_reader_stats();
  assert(stats.cached==0&&stats.leased==0&&stats.opens==stats.closes);
  printf("PASS: 24000 multithreaded reads concurrent with 2000 clears; descriptors balanced (%llu opens/%llu closes).\n",
         (unsigned long long)stats.opens,(unsigned long long)stats.closes);
}

static void testNonblockingCachedLease(void) {
  pg_local_asset_reader_clear();PGLocalAssetLease lease,fill;
  PGLocalAssetReaderStats before=pg_local_asset_reader_stats();
  assert(pg_local_asset_try_cached(paths[0],&lease)<0&&errno==ENOENT);
  assert(lease.descriptor==-1&&lease.slot==-1&&pg_local_asset_reader_stats().opens==before.opens);
  assert(pg_local_asset_acquire(paths[0],&fill)>=0);pg_local_asset_release(&fill);
  assert(pg_local_asset_try_cached(paths[0],&lease)>=0);
  assert(pg_local_asset_reader_stats().cached==1&&pg_local_asset_reader_stats().leased==1);
  pg_local_asset_reader_clear();
  unsigned char byte;assert(pread(lease.descriptor,&byte,1,19)==1&&byte==payload[19]);
  errno=EIO;pg_local_asset_release(&lease);assert(errno==EIO);
  assert(!pg_local_asset_reader_stats().cached&&!pg_local_asset_reader_stats().leased);
  pthread_mutex_lock(&localAssetLock);
  assert(pg_local_asset_try_cached(paths[0],&lease)<0&&errno==EBUSY);
  pthread_mutex_unlock(&localAssetLock);
  assert(pg_local_asset_reader_stats().opens==before.opens+1);
  puts("PASS: optional cached leases never open/admit on a miss, never wait on a busy demand lock, and remain valid across clear.");
}

static void testTransientAdmission(void) {
  pg_local_asset_reader_clear();PGLocalAssetLease hot,one;
  assert(pg_local_asset_acquire(paths[0],&hot)>=0);int descriptor=hot.descriptor;pg_local_asset_release(&hot);
  PGLocalAssetReaderStats before=pg_local_asset_reader_stats();
  for(unsigned i=0;i<140;i++){
    assert(pg_local_asset_acquire_transient(paths[1+i%69],&one)>=0&&one.slot==-1);
    unsigned char byte;assert(pread(one.descriptor,&byte,1,19)==1&&byte==payload[19]);pg_local_asset_release(&one);
  }
  PGLocalAssetReaderStats after=pg_local_asset_reader_stats();
  assert(after.cached==1&&after.evictions==before.evictions&&after.opens-before.opens==140&&after.closes-before.closes==140);
  assert(pg_local_asset_acquire_transient(paths[0],&one)==descriptor&&one.slot>=0);
  pg_local_asset_reader_clear();unsigned char byte;assert(pread(one.descriptor,&byte,1,19)==1&&byte==payload[19]);pg_local_asset_release(&one);
  assert(!pg_local_asset_reader_stats().cached&&!pg_local_asset_reader_stats().leased);
  puts("PASS: one-shot resources do not displace hot archives; cached transient leases remain valid across clear.");
}

int main(void) {
  snprintf(root,sizeof root,"/tmp/pg-local-reader-XXXXXX");assert(mkdtemp(root));
  for(unsigned i=0;i<sizeof payload;i++)payload[i]=(unsigned char)(i*37u+19u);
  for(int i=0;i<70;i++){
    snprintf(paths[i],sizeof paths[i],"%s/%d.bin",root,i);
    makeFile(paths[i],payload,sizeof payload);
  }
  testReuseAndFailures();testClearAndReplace();testClearDuringOpen();testSaturationAndEviction();testConcurrentClear();testNonblockingCachedLease();testTransientAdmission();
  for(int i=0;i<70;i++)assert(!unlink(paths[i]));
  assert(!rmdir(root));
  return 0;
}
