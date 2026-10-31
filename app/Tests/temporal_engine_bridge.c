// Standalone bridge preflight: synthetic wasm memory only; never links or executes game code.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef struct { uint8_t *data; uint64_t size; } test_memory;
typedef struct w2c_game { test_memory *w2c_env_memory; } w2c_game;

#include "../Source/NativeTemporalDiagnostics.h"

static int enabled;
static int claim = 1;
static unsigned claim_calls;
static unsigned producer_calls;
static unsigned scope_calls;
static uint64_t last_key;
static uint32_t last_flags;
static PGTDTag last_tag;
static uint8_t *tracked_guest_data;
static uint64_t tracked_viewport;
static int mutate_after_view_copy;

static void *pgtd_test_memcpy(void *destination, const void *source, size_t length) {
  void *result = memcpy(destination, source, length);
  if (mutate_after_view_copy && tracked_guest_data &&
      source == tracked_guest_data + tracked_viewport + 192u && length == 16u * sizeof(float)) {
    float changed = 2.0f;
    memcpy(tracked_guest_data + tracked_viewport + 192u, &changed, sizeof changed);
    mutate_after_view_copy = 0;
  }
  return result;
}

int PGTDEnabled(void) { return enabled; }
int PGTDClaimPacket(uint32_t op) { (void)op; claim_calls++; return claim; }
void PGTDProducerPacket(uint64_t address, uint32_t op, uint32_t words, const PGTDCamera *camera) {
  (void)address; (void)op; (void)words;
  memset(&last_tag, 0, sizeof last_tag);
  if (camera) last_tag.camera = *camera;
  producer_calls++;
}
uint64_t PGTDScopeEnter(uint32_t kind, uint64_t identity, uint64_t auxiliary,
                        uint64_t candidateKey, uint32_t flags) {
  (void)kind; (void)identity; (void)auxiliary;
  last_key = candidateKey; last_flags = flags; scope_calls++;
  return scope_calls;
}
void PGTDScopeLeave(uint64_t cookie) { (void)cookie; }

#define memcpy pgtd_test_memcpy
#include "../Source/NativeTemporalEngine.inc"
#undef memcpy

static void put(uint8_t *data, uint64_t offset, const void *value, size_t size) {
  memcpy(data + offset, value, size);
}

static void set_camera(uint8_t *data, uint64_t size, uint64_t viewport) {
  const uint64_t global = 12374976ull;
  assert(global + 8 <= size);
  put(data, global, &viewport, sizeof viewport);
  if (!viewport || viewport > size - 876) return;
  float view[16] = {0}, projection[16] = {0};
  view[0] = view[5] = view[10] = view[15] = 1.0f;
  projection[0] = projection[5] = projection[10] = projection[15] = 1.0f;
  uint32_t width = 1280, height = 720;
  float nearPlane = 0.1f, farPlane = 1000.0f;
  put(data, viewport + 192, view, sizeof view);
  put(data, viewport + 320, projection, sizeof projection);
  put(data, viewport + 864, &width, sizeof width);
  put(data, viewport + 868, &height, sizeof height);
  put(data, viewport + 872, &nearPlane, sizeof nearPlane);
  put(data, viewport + 876, &farPlane, sizeof farPlane);
}

int main(void) {
  // Disabled callbacks must return before dereferencing even a deliberately invalid instance.
  w2c_game invalid = { (test_memory *)(uintptr_t)1 };
  enabled = 0;
  pg_td_packet(&invalid, UINT64_MAX);
  assert(pg_td_entity_enter(&invalid, 1, 0, 56, 0) == 0);
  assert(producer_calls == 0 && scope_calls == 0);

  // Packet range guards reject both a near-end header and integer-overflow-style address.
  uint8_t small_data[64] = {0};
  test_memory small = { small_data, sizeof small_data };
  w2c_game small_instance = { &small };
  enabled = 1;
  const uint32_t irrelevantOp = 11, irrelevantWords = 4;
  put(small_data, 0, &irrelevantOp, sizeof irrelevantOp);
  put(small_data, 4, &irrelevantWords, sizeof irrelevantWords);
  pg_td_packet(&small_instance, 0);
  assert(claim_calls == 0 && producer_calls == 0); // irrelevant op skips claim and camera snapshot
  pg_td_packet(&small_instance, 60);
  pg_td_packet(&small_instance, UINT64_MAX - 3u);
  assert(claim_calls == 0 && producer_calls == 0);

  const uint64_t bytes = 12374984ull + 8192ull;
  uint8_t *data = calloc(1, (size_t)bytes);
  assert(data);
  test_memory memory = { data, bytes };
  w2c_game instance = { &memory };
  const uint32_t op = 32, words = 12;
  put(data, 256, &op, sizeof op);
  put(data, 260, &words, sizeof words);
  set_camera(data, bytes, 4096);
  pg_td_packet(&instance, 256);
  assert(producer_calls == 1 && last_tag.camera.valid == 1);
  assert(last_tag.camera.width == 1280 && last_tag.camera.height == 720);

  // A bad viewport address is non-trapping and remains explicitly invalid.
  set_camera(data, bytes, bytes - 64);
  pg_td_packet(&instance, 256);
  assert(producer_calls == 2 && last_tag.camera.valid == 0);

  // Non-finite matrix contents and unreasonable dimensions cannot be reported as valid.
  set_camera(data, bytes, 4096);
  float nan_value = NAN;
  put(data, 4096 + 192, &nan_value, sizeof nan_value);
  pg_td_packet(&instance, 256);
  assert(last_tag.camera.valid == 0);
  set_camera(data, bytes, 4096);
  uint32_t unreasonable = 50000;
  put(data, 4096 + 864, &unreasonable, sizeof unreasonable);
  pg_td_packet(&instance, 256);
  assert(last_tag.camera.valid == 0);

  // A matrix changed between bounded copies is rejected as unstable, not reported as valid.
  set_camera(data, bytes, 4096);
  tracked_guest_data = data;
  tracked_viewport = 4096;
  mutate_after_view_copy = 1;
  pg_td_packet(&instance, 256);
  assert(mutate_after_view_copy == 0 && last_tag.camera.valid == 0);

  // Entity candidate IDs retain all 64 bits; no raw value is logged by this test bridge.
  const uint64_t candidate = UINT64_C(0xfedcba9876543210);
  put(data, 1024 + 56, &candidate, sizeof candidate);
  (void)pg_td_entity_enter(&instance, 1, 1024, 56, 0);
  assert(last_key == candidate && last_flags == PGTD_SCOPE_FLAG_KEY_VALID);

  // Unknown-key sentinel and address zero never trigger a guest-memory read.
  (void)pg_td_entity_enter(&invalid, 2, 0, UINT32_MAX, 0);
  assert(last_key == 0 && last_flags == 0);
  (void)pg_td_entity_enter(&invalid, 2, 0, 64, 0);
  assert(last_key == 0 && last_flags == 0);

  free(data);
  return 0;
}
