#import <Foundation/Foundation.h>
#include "HostInternal.h"
#include "WasmFutex.h"
#include "wasm-rt-impl.h"
#include "NativeOIV.h"
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#undef wasm_rt_impl_try
#define wasm_rt_impl_try() 0
void pg_log(const char *message) {}
void PGMeshTouch(const void *start,uint64_t length) {}
NSArray<NSArray *> *pg_oiv_expand(NSString *folder,NSString *(^gameFile)(NSString *path),bool readable,bool *needsGame,NSArray<NSString *> **skip) {
  if(needsGame)*needsGame=false;if(skip)*skip=@[];return @[];
}
void *pg_pointer(struct w2c_env *host,uint64_t address,uint64_t bytes){abort();}
wasm_rt_shared_memory_t *w2c_env_memory(struct w2c_env *host){return &host->memory;}
uint32_t pg_atomic_wait32(wasm_rt_shared_memory_t *m,uint64_t a,uint32_t v,uint64_t t){return 0;}
uint32_t pg_atomic_notify(wasm_rt_shared_memory_t *m,uint64_t a,uint32_t n){return 0;}

// Inject faults into the real local-read loop without signals or a server.
static ssize_t probe_pread(int fd,void *buffer,size_t count,off_t offset);
static int probe_fcntl(int fd,int command,...);
#define pread probe_pread
#define fcntl probe_fcntl
#include "../Source/NativeAssets.m"
#undef pread
#undef fcntl
static struct {dev_t device;ino_t inode;off_t offset;int length,descriptor;} adviceRecords[64];
static unsigned adviceCount;
static bool failAdvice,realAdvice;
static int lastRealAdviceResult,lastRealAdviceError;
static int probe_fcntl(int descriptor,int command,...){
  assert(command==F_RDADVISE&&adviceCount<64);
  va_list arguments;va_start(arguments,command);struct radvisory *a=va_arg(arguments,struct radvisory *);va_end(arguments);
  struct stat identity;assert(!fstat(descriptor,&identity));
  adviceRecords[adviceCount++]=(__typeof__(adviceRecords[0])){identity.st_dev,identity.st_ino,a->ra_offset,a->ra_count,descriptor};
  if(failAdvice){errno=EIO;return -1;}
  if(realAdvice){lastRealAdviceResult=fcntl(descriptor,command,a);lastRealAdviceError=errno;return lastRealAdviceResult;}
  return 0;
}
static _Thread_local unsigned calls,interruptOn,errorOn;
static _Thread_local bool forceShortReads=true;
static ssize_t probe_pread(int fd,void *buffer,size_t count,off_t offset){
  calls++;
  if(calls==interruptOn){errno=EINTR;return -1;}
  if(calls==errorOn){errno=EIO;return -1;}
  return pread(fd,buffer,forceShortReads?MIN(count,3):count,offset); // also exercise positive short reads
}

enum { streamingSlots=16, streamingThreads=6, streamingReads=2048, streamingFileBytes=1024 };
struct StreamingReadTask { const uint8_t *expected; uint32_t size, seed; };
static void *streamingReadsThread(void *pointer) {
  @autoreleasepool {
    struct StreamingReadTask *task=pointer;
    forceShortReads=false;
    uint32_t state=task->seed;
    uint8_t output[509];
    for(unsigned i=0;i<streamingReads;i++) {
      state=state*1664525u+1013904223u;
      uint32_t offset=state%task->size;
      state=state*1664525u+1013904223u;
      uint32_t length=MIN((uint32_t)(1+state%sizeof output),task->size-offset);
      assert(readAsset(0,offset,output,length)==length);
      assert(!memcmp(output,task->expected+offset,length));
    }
  }
  return NULL;
}

static void testConcurrentModArchiveStreaming(NSString *root) {
  const uint32_t baseSize=8192,virtualSize=12288+(streamingSlots-1)*4096+streamingFileBytes;
  uint8_t *expected=calloc(1,virtualSize),*output=malloc(virtualSize);
  assert(expected&&output);
  for(uint32_t i=0;i<baseSize;i++)expected[i]=(uint8_t)(i*31+7);
  NSString *basePath=[root stringByAppendingPathComponent:@"streaming.rpf"];
  assert([[NSData dataWithBytes:expected length:baseSize] writeToFile:basePath atomically:YES]);
  PGModSlot slots[streamingSlots]={0};
  for(unsigned slot=0;slot<streamingSlots;slot++) {
    uint32_t offset=12288+slot*4096;
    for(unsigned i=0;i<streamingFileBytes;i++)expected[offset+i]=(uint8_t)(slot*13+i*17+1);
    NSString *path=[root stringByAppendingPathComponent:[NSString stringWithFormat:@"streaming-slot-%u.bin",slot]];
    assert([[NSData dataWithBytes:expected+offset length:streamingFileBytes] writeToFile:path atomically:YES]);
    snprintf(slots[slot].file,sizeof slots[slot].file,"%s",path.fileSystemRepresentation);
    slots[slot].info.offset=offset;slots[slot].info.length=streamingFileBytes;
  }
  PGModArchive archive={.id=0,.realSize=baseSize,.count=streamingSlots,.slots=slots};
  NSArray *previousFiles=assetFiles;
  PGModArchive *previousArchives=modArchives;int previousCount=modArchiveCount;
  assetFiles=@[@[@"streaming.rpf",@(virtualSize)]];modArchives=&archive;modArchiveCount=1;
  pg_local_asset_reader_clear();forceShortReads=false;calls=interruptOn=errorOn=0;
  PGLocalAssetReaderStats before=pg_local_asset_reader_stats();
  assert(readAsset(0,0,output,virtualSize)==virtualSize&&!memcmp(output,expected,virtualSize));
  assert(pg_local_asset_reader_stats().cached==1); // Cold whole-slot reads do not pollute the LRU.
  for(unsigned slot=0;slot<streamingSlots;slot++)
    assert(readAsset(0,12288+slot*4096+1,output,streamingFileBytes-1)==streamingFileBytes-1);
  PGLocalAssetReaderStats warm=pg_local_asset_reader_stats();
  assert(warm.opens-before.opens==2*streamingSlots+1&&warm.cached==streamingSlots+1&&!warm.leased);
  pthread_t threads[streamingThreads];struct StreamingReadTask tasks[streamingThreads];
  for(unsigned i=0;i<streamingThreads;i++) {
    tasks[i]=(struct StreamingReadTask){expected,virtualSize,0x674521u+i*1237u};
    assert(!pthread_create(threads+i,NULL,streamingReadsThread,tasks+i));
  }
  for(unsigned i=0;i<streamingThreads;i++)assert(!pthread_join(threads[i],NULL));
  PGLocalAssetReaderStats after=pg_local_asset_reader_stats();
  assert(after.opens==warm.opens&&after.hits>warm.hits&&!after.leased);
  // Folder/mod changes clear the shared reader cache; new pathname contents
  // must then be visible to the exact production archive read path.
  memset(expected+12288,0x5a,streamingFileBytes);
  assert([[NSData dataWithBytes:expected+12288 length:streamingFileBytes] writeToFile:@(slots[0].file) atomically:YES]);
  pg_local_asset_reader_clear();
  assert(readAsset(0,12288,output,streamingFileBytes)==streamingFileBytes);
  assert(!memcmp(output,expected+12288,streamingFileBytes));
  pg_local_asset_reader_clear();
  PGLocalAssetReaderStats closed=pg_local_asset_reader_stats();
  assert(!closed.cached&&!closed.leased);
  assert(closed.opens-before.opens==closed.closes-before.closes);
  assetFiles=previousFiles;modArchives=previousArchives;modArchiveCount=previousCount;
  forceShortReads=true;free(expected);free(output);
  puts("PASS: production mod archive streaming: 16 slots plus base, 6 threads, 12288 exact random reads, cache reuse, atomic replacement after clear, and balanced descriptor lifetime.");
}

static void resetAdviceProbe(void){memset(&localAdvice,0,sizeof localAdvice);memset(&normalHints,0,sizeof normalHints);memset(&lowHints,0,sizeof lowHints);adviceCount=0;failAdvice=realAdvice=false;calls=interruptOn=errorOn=0;}
static void assertAdvice(unsigned index,NSString *path,uint64_t offset,unsigned length){
  struct stat identity;assert(index<adviceCount&&!stat(path.fileSystemRepresentation,&identity));
  assert(adviceRecords[index].device==identity.st_dev&&adviceRecords[index].inode==identity.st_ino);
  assert(adviceRecords[index].offset==(off_t)offset&&adviceRecords[index].length==(int)length);
}
static void testLocalAdvisoryHints(NSString *root){
  NSString *basePath=[root stringByAppendingPathComponent:@"advice.rpf"],*slotPath=[root stringByAppendingPathComponent:@"advice-slot.bin"];
  const uint8_t base[]={1,2,3,4,5,6,7,8},mod[]={10,20,30,40,50,60,70,80,90};
  assert([[NSData dataWithBytes:base length:sizeof base] writeToFile:basePath atomically:YES]);
  assert([[NSData dataWithBytes:mod length:sizeof mod] writeToFile:slotPath atomically:YES]);
  PGModSlot slots[4]={0};
  for(unsigned i=0;i<4;i++)snprintf(slots[i].file,sizeof slots[i].file,"%s",slotPath.fileSystemRepresentation);
  slots[0].info.offset=512;
  slots[1].info.offset=512;slots[1].info.length=7;slots[1].skip=2;
  slots[2].info.offset=1024;slots[2].info.length=3;
  slots[3].info.offset=1536;slots[3].info.length=7;
  PGModArchive archive={.id=0,.realSize=sizeof base,.count=4,.slots=slots};
  NSArray *previousFiles=assetFiles;PGModArchive *previousArchives=modArchives;int previousCount=modArchiveCount;
  assetFiles=@[@[@"advice.rpf",@1543]];modArchives=&archive;modArchiveCount=1;
  uint8_t output[1543];forceShortReads=false;calls=interruptOn=errorOn=0;
  pg_local_asset_reader_clear();assert(readAsset(0,0,output,sizeof output)==sizeof output);
  // Whole-slot reads intentionally use transient descriptors. Only the base
  // is retained so far; a partial demand read warms the shared slot pathname.
  assert(pg_local_asset_reader_stats().cached==1);
  assert(readAsset(0,513,output,6)==6&&!memcmp(output,mod+3,6));
  assert(pg_local_asset_reader_stats().cached==2);
  PGLocalAssetReaderStats warm=pg_local_asset_reader_stats();
  resetAdviceProbe();
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=4,.length=1539});pumpLocalAssetHints(1000);
  assert(!calls&&adviceCount==4&&localAdvice.bytes==21&&localAdvice.errors==0);
  assertAdvice(0,basePath,4,4);assertAdvice(1,slotPath,2,7);assertAdvice(2,slotPath,0,3);assertAdvice(3,slotPath,0,7);
  assert(pg_local_asset_reader_stats().opens==warm.opens&&!pg_local_asset_reader_stats().leased);
  // Exact/nested duplicate hints are coalesced; advisory work has no demand reads.
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=512,.length=7});pumpLocalAssetHints(1100);
  assert(localAdvice.duplicates==1&&adviceCount==4&&!calls);
  // With only the base cached, the three slots share a cold pathname but
  // each advisory lease is temporary. The two-open budget must still apply.
  pg_local_asset_reader_clear();assert(readAsset(0,0,output,sizeof output)==sizeof output);
  assert(pg_local_asset_reader_stats().cached==1);
  resetAdviceProbe();
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=4,.length=1539});pumpLocalAssetHints(1200);
  assert(!calls&&adviceCount==3&&localAdvice.bytes==14&&localAdvice.errors==0);
  assert(localAdvice.coldOpens==2&&localAdvice.windowCold==2);
  assert(pg_local_asset_reader_stats().cached==1&&!pg_local_asset_reader_stats().leased);
  assertAdvice(0,basePath,4,4);assertAdvice(1,slotPath,2,7);assertAdvice(2,slotPath,0,3);
  // A skipped optional hint must not change the subsequent demand read.
  assert(readAsset(0,1536,output,7)==7&&!memcmp(output,mod,7));
  // An unsupported/failing advisory syscall is nonfatal; demand data still works.
  resetAdviceProbe();failAdvice=true;
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=0,.length=4});pumpLocalAssetHints(2000);
  assert(localAdvice.errors==1&&localAdvice.bytes==0&&!calls);
  assert(readAsset(0,0,output,4)==4&&!memcmp(output,base,4));
  // Exercise the real Darwin API too. Its availability is checked by compilation;
  // a filesystem refusing optional advice must not prevent an ordinary read.
  resetAdviceProbe();realAdvice=true;
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=0,.length=4});pumpLocalAssetHints(3000);
  assert(adviceCount==1&&!calls&&(lastRealAdviceResult==0||localAdvice.errors==1));
  printf("Darwin F_RDADVISE runtime result: %d (errno %d); copied reads %u\n",lastRealAdviceResult,lastRealAdviceResult?lastRealAdviceError:0,calls);
  // More than four physical requests in a 100ms window stay queued; a later
  // poll resumes them. A cold hint opens only a temporary descriptor, not an LRU entry.
  resetAdviceProbe();modArchiveCount=0;assetFiles=@[@[@"advice.rpf",@(sizeof base)]];
  for(unsigned i=0;i<8;i++)pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=i,.length=1});
  pumpLocalAssetHints(4000);assert(adviceCount==4&&normalHints.count==4);
  pumpLocalAssetHints(4010);assert(adviceCount==4&&localAdvice.throttled==1);
  pumpLocalAssetHints(4100);assert(adviceCount==8&&normalHints.count==0&&!calls);
  resetAdviceProbe();pg_local_asset_reader_clear();
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=0,.length=4});pumpLocalAssetHints(5000);
  assert(localAdvice.coldOpens==1&&adviceCount==1&&!pg_local_asset_reader_stats().cached&&!calls);
  assert(fcntl(adviceRecords[0].descriptor,F_GETFD)==-1&&errno==EBADF);
  // Bounds remain 64-bit while each advice request and the total window bytes
  // stay small. The probe does not read or allocate this logical file size.
  resetAdviceProbe();assetFiles=@[@[@"advice.rpf",@((UINT64_C(1)<<32)+(2u<<20))]];
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=UINT64_C(1)<<32,.length=1u<<20});
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=(UINT64_C(1)<<32)+(1u<<20),.length=1u<<20});
  pg_asset_hint_push(&normalHints,(PGAssetHint){.id=0,.offset=(UINT64_C(1)<<32)+512,.length=1});
  pumpLocalAssetHints(6000);assert(adviceCount==2&&localAdvice.windowBytes==(2u<<20)&&normalHints.count==1&&!calls);
  assertAdvice(0,basePath,UINT64_C(1)<<32,1u<<20);assertAdvice(1,basePath,(UINT64_C(1)<<32)+(1u<<20),1u<<20);
  resetAdviceProbe();
  for(unsigned i=0;i<100;i++)pg_asset_hint_push(&normalHints,(PGAssetHint){.id=UINT32_MAX,.length=1});
  for(unsigned i=0;i<20;i++)pg_asset_hint_push(&lowHints,(PGAssetHint){.id=UINT32_MAX,.length=1,.speculative=true});
  pumpLocalAssetHints(7000);assert(localAdvice.dropped==40&&normalHints.count<=64&&lowHints.count<=16&&!calls);
  assetFiles=previousFiles;modArchives=previousArchives;modArchiveCount=previousCount;
  pg_local_asset_reader_clear();resetAdviceProbe();forceShortReads=true;
  puts("PASS: local hints advise translated base/slot/skip ranges, omit gaps, avoid copied reads/LRU pollution, coalesce overlaps, throttle calls/bytes, preserve 64-bit offsets and tolerate advisory failures.");
}

static void testModArchiveReads(NSString *root) {
  const uint8_t base[]={3,1,4,1,5,9,2,6},mod[]={11,22,33,44,55,66,77};
  NSString *basePath=[root stringByAppendingPathComponent:@"base.rpf"];
  NSString *modPath=[root stringByAppendingPathComponent:@"slot.bin"];
  assert([[NSData dataWithBytes:base length:sizeof base] writeToFile:basePath atomically:YES]);
  assert([[NSData dataWithBytes:mod length:sizeof mod] writeToFile:modPath atomically:YES]);
  PGModSlot slots[4]={0};
  for(unsigned i=0;i<4;i++)snprintf(slots[i].file,sizeof slots[i].file,"%s",modPath.fileSystemRepresentation);
  slots[0].info.offset=512; // zero-length file at the same start as the next slot
  slots[1].info.offset=512;slots[1].info.length=sizeof mod;
  slots[2].info.offset=1024;slots[2].info.length=5;slots[2].skip=2;
  slots[3].info.offset=1536;slots[3].info.length=3;
  PGModArchive archive={.realSize=sizeof base,.count=4,.slots=slots};
  uint8_t expected[1539]={0},got[1539];
  memcpy(expected,base,sizeof base);memcpy(expected+512,mod,sizeof mod);
  memcpy(expected+1024,mod+2,5);memcpy(expected+1536,mod,3);
  pg_local_asset_reader_clear();calls=interruptOn=errorOn=0;
  assert(readModArchive(&archive,basePath,0,got,sizeof got)==sizeof got);
  assert(!memcmp(got,expected,sizeof got));
  for(unsigned offset=0;offset<sizeof got;offset+=7) {
    uint32_t length=MIN((uint32_t)43,(uint32_t)(sizeof got-offset));
    assert(readModArchive(&archive,basePath,offset,got,length)==length);
    assert(!memcmp(got,expected+offset,length));
  }
  // The original mod path reports partial/zero counts on pread failure; keep
  // that behaviour while changing only slot lookup and retrying interruptions.
  calls=0;interruptOn=1;errorOn=0;
  assert(readModArchive(&archive,basePath,512,got,7)==7&&!memcmp(got,mod,7));
  calls=0;interruptOn=0;errorOn=1;
  assert(readModArchive(&archive,basePath,512,got,7)==0);
  calls=0;errorOn=2;
  assert(readModArchive(&archive,basePath,512,got,7)==3);
  calls=0;errorOn=0;
  assert(truncate(modPath.fileSystemRepresentation,2)==0);
  assert(readModArchive(&archive,basePath,512,got,7)==2);
  // An empty changed archive still zero-fills reads after the original end.
  archive.count=0;archive.slots=NULL;
  memset(got,0xff,32);
  assert(readModArchive(&archive,basePath,8,got,32)==32);
  for(unsigned i=0;i<32;i++)assert(got[i]==0);
  // File removal must remain an error instead of reading another slot.
  archive.count=4;archive.slots=slots;
  assert(unlink(modPath.fileSystemRepresentation)==0);pg_local_asset_reader_clear();
  assert(readModArchive(&archive,basePath,512,got,7)==-1);
  pg_local_asset_reader_clear();
  puts("PASS: mod archive base/slot/gap boundaries, zero-length duplicates, cross-slot reads, skip offsets, EINTR, errors, truncation and removal preserve bytes and counts.");
}

int main(void){@autoreleasepool {
  NSString *root=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
  NSFileManager *files=NSFileManager.defaultManager;
  assert([files createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil]);
  const uint8_t original[]={1,2,3,4,5,6,7,8};
  NSData *fixture=[NSData dataWithBytes:original length:sizeof original];
  NSString *path=[root stringByAppendingPathComponent:@"fixture.bin"];
  assert([fixture writeToFile:path atomically:YES]);
  @try {
    PGSetLocalGameFolder(root.fileSystemRepresentation);
    assetFiles=@[@[@"fixture.bin",@(sizeof original)]];
    uint8_t output[sizeof original]={0};
    // An interruption before any data or after a positive short read is retried.
    for(unsigned at=1;at<=2;at++){
      calls=0;interruptOn=at;errorOn=0;
      assert(readAsset(0,0,output,sizeof output)==sizeof output);
      assert(!memcmp(output,original,sizeof output));
    }
    // Real failures must remain errors, even after some bytes were read.
    for(unsigned at=1;at<=2;at++){
      calls=0;interruptOn=0;errorOn=at;
      assert(readAsset(0,0,output,sizeof output)==-1);
    }
    calls=0;errorOn=0;
    assert(readAsset(0,6,output,sizeof output)==2&&!memcmp(output,original+6,2));
    assert(readAsset(0,sizeof original,output,sizeof output)==0);
    assert(readAsset(0,0,output,0)==0);
    // EOF behavior is unchanged for a file shortened after folder validation.
    assert(truncate(path.fileSystemRepresentation,2)==0);
    assert(readAsset(0,0,output,sizeof output)==2);
    assert(readAsset(0,4,output,sizeof output)==0);
    assert(atomic_load(&healthReadErrors)==2);
    puts("PASS: interrupted local reads retry, short reads accumulate, real errors propagate, and EOF/range limits remain unchanged.");
    testModArchiveReads(root);
    testConcurrentModArchiveStreaming(root);
    testLocalAdvisoryHints(root);
  } @finally {
    PGSetLocalGameFolder(NULL);
    [files removeItemAtPath:root error:nil];
  }
}return 0;}
