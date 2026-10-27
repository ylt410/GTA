// Entities deleted while still on a render list (GTA-18, Scripts/prepare_render_list_guard.py).
//
// The visibility scan (rage::fwWorldMgr::PreScan) fills the frame's render lists with entity pointers;
// the draw lists are built from them later in the same frame. fwBasePool::Delete fills a deleted
// entity's slot with 0xDD, so drawing an entity deleted in between follows a pointer made of 0xDD
// bytes and the app crashes. Such an entry is skipped here and logged once, with what the slot held
// and when it was deleted. PG_RENDER_GUARD=0 turns the skip off; PG_RENDER_GUARD_TRACE=1 also keeps
// each deletion's native call stack (image offsets, for atos) and logs it with the skipped entry.
#include "HostInternal.h"
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RING = 2048, FRAMES = 20 };
typedef struct { uint64_t item, first; uint32_t scan, frames; uintptr_t trace[FRAMES]; } Deletion;
static Deletion ring[RING];
static uint32_t next;
static _Atomic uint32_t scans;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int mode = -1;   // bit 0 skip, bit 1 trace

static void configure(void) {
  if (mode >= 0) return;
  const char *guard = getenv("PG_RENDER_GUARD"), *trace = getenv("PG_RENDER_GUARD_TRACE");
  mode = (guard && !strcmp(guard, "0") ? 0 : 1) | (trace && *trace && strcmp(trace, "0") ? 2 : 0);
}

void pg_render_scan_started(void) { atomic_fetch_add(&scans, 1); }

void pg_pool_deleted(w2c_game *instance, u64 pool, u64 item) {
  (void)pool;
  configure();
  const uint8_t *base = instance->w2c_env_memory->data; const uint64_t size = instance->w2c_env_memory->size;
  if (!item || item + 8 > size) return;
  Deletion d = { .item = item, .scan = atomic_load(&scans) };
  memcpy(&d.first, base + item, 8);
  if (mode & 2) {
    void *frames[FRAMES + 1];
    int n = backtrace(frames, FRAMES + 1);
    const uintptr_t image = (uintptr_t)_dyld_get_image_header(0);
    for (int i = 1; i < n; i++) d.trace[i - 1] = (uintptr_t)frames[i] - image;
    d.frames = n > 1 ? (uint32_t)n - 1 : 0;
  }
  pthread_mutex_lock(&lock);
  ring[next++ % RING] = d;
  pthread_mutex_unlock(&lock);
}

u32 pg_render_entity_gone(w2c_game *instance, u64 entity) {
  configure();
  const uint8_t *base = instance->w2c_env_memory->data; const uint64_t size = instance->w2c_env_memory->size;
  if (!entity || entity + 80 > size) return 0;
  uint64_t handler, second;
  memcpy(&handler, base + entity + 72, 8); memcpy(&second, base + entity + 8, 8);
  if (handler != 0xddddddddddddddddull || second != 0xddddddddddddddddull) return 0;
  static uint64_t lastEntity; static uint32_t lastScan, logged;
  const uint32_t scan = atomic_load(&scans);
  if ((entity != lastEntity || scan != lastScan) && logged < 40) {
    lastEntity = entity; lastScan = scan; logged++;
    Deletion found = {0};
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < RING && i < next; i++) {
      const Deletion *d = &ring[(next - 1 - i) % RING];
      if (d->item == entity) { found = *d; break; }
    }
    pthread_mutex_unlock(&lock);
    char line[600];
    int n = snprintf(line, sizeof line, "Render list: entity 0x%llx was deleted while on this frame's render list; %s.",
                     (unsigned long long)entity, mode & 1 ? "skipped it" : "drawing it anyway (PG_RENDER_GUARD=0)");
    if (found.item)
      n += snprintf(line + n, sizeof line - n, " Deleted %s (scan %u, now %u), its slot began 0x%016llx.",
                    found.scan == scan ? "after this frame's scan" : "before this frame's scan", found.scan, scan,
                    (unsigned long long)found.first);
    else
      n += snprintf(line + n, sizeof line - n, " Not among the last %d pool deletions.", RING);
    if (found.frames) {
      n += snprintf(line + n, sizeof line - n, " Stack (image offsets):");
      for (uint32_t i = 0; i < found.frames && n < (int)sizeof line - 20; i++) n += snprintf(line + n, sizeof line - n, " 0x%lx", (unsigned long)found.trace[i]);
    }
    pg_log(line);
  }
  return mode & 1;
}
