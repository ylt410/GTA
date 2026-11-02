#define PG_NATIVE_OPUS_TESTING 1
#include "native_opus_stubs.h"
#include "../Source/AudioDecoderCache.c"
#include "../Source/NativeOpus.c"
#include <assert.h>
#include <stdarg.h>

int pg_test_create_failures, pg_test_created, pg_test_destroyed, pg_test_wasm_calls, pg_test_reentry_result;
bool pg_test_reenter_wasm;
w2c_game *pg_test_instance;

OpusDecoder *opus_decoder_create(opus_int32 fs, int channels, int *error) {
  if (pg_test_create_failures) { pg_test_create_failures--; *error = OPUS_BAD_ARG; return NULL; }
  OpusDecoder *decoder = calloc(1, sizeof *decoder);
  if (!decoder) { *error = OPUS_BAD_ARG; return NULL; }
  decoder->fs = fs; decoder->channels = channels; *error = 0; pg_test_created++;
  return decoder;
}

void opus_decoder_destroy(OpusDecoder *decoder) {
  if (decoder) { pg_test_destroyed++; free(decoder); }
}

int opus_decoder_ctl(OpusDecoder *decoder, int request, ...) {
  // In Opus 1.5.2 OPUS_RESET_STATE clears bytes beginning at
  // OPUS_DECODER_RESET_START (stream_channels); decode_gain precedes it.
  if (request == OPUS_RESET_STATE) { decoder->resets++; return 0; }
  if (request == 6000) {
    va_list args; va_start(args, request); decoder->gain = va_arg(args, int); va_end(args); return 0;
  }
  return OPUS_BAD_ARG;
}

int opus_decode_float(OpusDecoder *decoder, const unsigned char *data, opus_int32 len,
                      float *pcm, int frameSize, int decodeFec) {
  (void)data; (void)len; (void)decodeFec;
  decoder->decodes++;
  for (int i = 0; i < frameSize * decoder->channels; i++) pcm[i] = (float)decoder->decodes / 1000.0f;
  return frameSize;
}

void pg_log(const char *message) { (void)message; }

u32 PG_FN_OPUS_DECODE_FRAME(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frameSize) {
  (void)st; (void)data; (void)len;
  pg_test_wasm_calls++;
  float *out = (float *)(instance->w2c_env_memory->data + pcm);
  for (u32 i = 0; i < frameSize * 2; i++) out[i] = 0.25f;
  if (pg_test_reenter_wasm) {
    u32 result = 123;
    pg_test_reentry_result = (int)pg_opus_frame(instance, st, data, len, pcm, frameSize, &result);
    assert(result == 123);
  }
  return frameSize;
}

static void store32(uint8_t *memory, uint64_t address, int32_t value) { memcpy(memory + address, &value, 4); }
static int32_t load32(uint8_t *memory, uint64_t address) { int32_t value; memcpy(&value, memory + address, 4); return value; }

static void setupStream(uint8_t *memory, uint64_t st, int32_t fs, int32_t channels) {
  memset(memory + st, 0, 96);
  store32(memory, st + ST_CHANNELS, channels);
  store32(memory, st + ST_FS, fs);
  store32(memory, st + ST_GAIN, 5);
  store32(memory, st + ST_STREAM_CHANNELS, channels);
  store32(memory, st + ST_BANDWIDTH, OPUS_BANDWIDTH_FULLBAND);
  store32(memory, st + ST_MODE, MODE_CELT);
  store32(memory, st + ST_FRAME_SIZE, 960);
  memory[512] = 0xA5; memory[513] = 0x5A;
}

static Decoder *cachedDecoder(uint64_t key) {
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++) {
    PGAudioDecoderCacheSlot *slot = &decoderCache.slots[i];
    if (slot->state != PG_AUDIO_CACHE_EMPTY && slot->key == key) return slot->value;
  }
  return NULL;
}

static void cleanupProductionState(void) {
  if (decoderCacheReady) { PGAudioDecoderCacheDestroy(&decoderCache, destroyDecoderValue); decoderCacheReady = 0; }
  if (statsReady) { PGAudioDecoderStatsDestroy(&stats); statsReady = 0; }
  PGAudioModeDestroy(&modeState);
}

static void testNativeMode(w2c_game *instance) {
  uint8_t *memory = instance->w2c_env_memory->data;
  const u64 st = 128, data = 512, pcm = 1024;
  setupStream(memory, st, 48000, 2);
  u32 result = 0;
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1 && result == 960);
  Decoder *decoder = cachedDecoder(st); assert(decoder && decoder->dec);
  OpusDecoder *first = decoder->dec;
  assert(first->resets == 1 && first->gain == 5 && first->decodes == 1);
  assert(load32(memory, st + ST_PREV_MODE) == MODE_CELT);

  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1 && result == 960);
  assert(cachedDecoder(st)->dec == first && first->resets == 1 && first->gain == 5 && first->decodes == 2);

  // Explicit state reset preserves decode_gain in libopus 1.5.2; cached gain
  // tracking must therefore avoid a redundant SET_GAIN and retain the value.
  store32(memory, st + ST_PREV_MODE, 0);
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1);
  assert(first->resets == 2 && first->gain == 5);

  // A decoder creation failure cancels the initializing reservation and releases its slot.
  const u64 secondSt = 256;
  setupStream(memory, secondSt, 48000, 2);
  pg_test_create_failures = 1;
  assert(pg_opus_frame(instance, secondSt, data, 2, pcm, 960, &result) == 0);
  for (unsigned i = 0; i < PG_AUDIO_DECODER_CACHE_CAPACITY; i++)
    assert(decoderCache.slots[i].key != secondSt || decoderCache.slots[i].state == PG_AUDIO_CACHE_EMPTY);
  assert(pg_opus_frame(instance, secondSt, data, 2, pcm, 960, &result) == 1 && result == 960);

  // An unsupported TOC falls back to WASM without advancing native state or leaking the lease.
  store32(memory, st + ST_MODE, 9999);
  const int nativeDecodes = first->decodes, nativeResets = first->resets;
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 0);
  assert(first->decodes == nativeDecodes && first->resets == nativeResets);
  assert(cachedDecoder(st) && cachedDecoder(st)->dec == first);
  store32(memory, st + ST_MODE, MODE_CELT);
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1);
  assert(first->resets == nativeResets);

  // Stream format changes replace state, resetting gain/history as a fresh decoder would.
  store32(memory, st + ST_CHANNELS, 1); store32(memory, st + ST_STREAM_CHANNELS, 1);
  store32(memory, st + ST_PREV_MODE, 0);
  const int createdBeforeReconfigure = pg_test_created, destroyedBeforeReconfigure = pg_test_destroyed;
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1);
  Decoder *changed = cachedDecoder(st);
  assert(changed && changed->fs == 48000 && changed->channels == 1);
  assert(pg_test_created == createdBeforeReconfigure + 1 && pg_test_destroyed == destroyedBeforeReconfigure + 1);
  assert(changed->dec->resets == 1 && changed->dec->gain == 5);

  // Failed reconfiguration drops the lease and can be retried cleanly.
  store32(memory, st + ST_FS, 24000); store32(memory, st + ST_FRAME_SIZE, 480); store32(memory, st + ST_PREV_MODE, 0);
  pg_test_create_failures = 1;
  assert(pg_opus_frame(instance, st, data, 2, pcm, 480, &result) == 0);
  assert(cachedDecoder(st) == NULL);
  assert(pg_opus_frame(instance, st, data, 2, pcm, 480, &result) == 1);
  Decoder *retried = cachedDecoder(st);
  assert(retried && retried->fs == 24000 && retried->channels == 1 && retried->dec->resets == 1);
}

static void testGuestAddressBounds(w2c_game *instance) {
  uint8_t *memory = instance->w2c_env_memory->data;
  const uint64_t size = instance->w2c_env_memory->size;
  u32 result = 77;
  // UINT64_MAX and partial-tail ranges must be rejected without wrapped
  // address arithmetic or output/result writes.
  assert(pg_opus_frame(instance, UINT64_MAX, 512, 2, 1024, 960, &result) == 0 && result == 77);
  assert(pg_opus_frame(instance, size - 79, 512, 2, 1024, 960, &result) == 0 && result == 77);
  setupStream(memory, 128, 48000, 2);
  assert(pg_opus_frame(instance, 128, UINT64_MAX, 2, 1024, 960, &result) == 1 && result == (u32)OPUS_BAD_ARG);
  assert(pg_opus_frame(instance, 128, size - 1, 2, 1024, 960, &result) == 1 && result == (u32)OPUS_BAD_ARG);
  assert(pg_opus_frame(instance, 128, 512, 2, UINT64_MAX, 960, &result) == 0 && result == (u32)OPUS_BAD_ARG);
  assert(pg_opus_frame(instance, 128, 512, 2, size - 1, 960, &result) == 0 && result == (u32)OPUS_BAD_ARG);

  // A stream ending exactly at linear-memory end is valid.
  const uint64_t endStream = size - (ST_FRAME_SIZE + 4);
  memset(memory + endStream, 0, ST_FRAME_SIZE + 4);
  store32(memory, endStream + ST_CHANNELS, 2);
  store32(memory, endStream + ST_FS, 48000);
  store32(memory, endStream + ST_GAIN, 5);
  store32(memory, endStream + ST_STREAM_CHANNELS, 2);
  store32(memory, endStream + ST_BANDWIDTH, OPUS_BANDWIDTH_FULLBAND);
  store32(memory, endStream + ST_MODE, MODE_CELT);
  store32(memory, endStream + ST_PREV_MODE, 0);
  store32(memory, endStream + ST_FRAME_SIZE, 960);
  assert(pg_opus_frame(instance, endStream, 512, 2, 1024, 960, &result) == 1);
  assert(result == 960);

  // Packet and PCM ranges also independently accept an exact end boundary.
  setupStream(memory, 128, 48000, 2);
  memory[size - 2] = 0xA5; memory[size - 1] = 0x5A;
  assert(pg_opus_frame(instance, 128, size - 2, 2, 1024, 960, &result) == 1 && result == 960);
  assert(pg_opus_frame(instance, 128, 512, 2, size - 960 * 2 * sizeof(float), 960, &result) == 1 && result == 960);
}

static void testVerifyMode(w2c_game *instance) {
  uint8_t *memory = instance->w2c_env_memory->data;
  const u64 st = 128, data = 512, pcm = 1024;
  setupStream(memory, st, 48000, 2);
  pg_test_reenter_wasm = true;
  u32 result = 0;
  assert(pg_opus_frame(instance, st, data, 2, pcm, 960, &result) == 1 && result == 960);
  assert(pg_test_wasm_calls == 1 && pg_test_reentry_result == 0);
  assert(((float *)(memory + pcm))[0] == 0.25f); // verify mode keeps the engine/WASM output
  assert(statsReady);
  PGAudioDecoderStatsSnapshot snapshot = PGAudioDecoderStatsRead(&stats);
  assert(snapshot.frames == 1 && snapshot.lost == 0 && snapshot.mismatched == 0);
  assert(snapshot.maxDiff > 0.24f && snapshot.maxDiff < 0.26f);
  pg_test_reenter_wasm = false;
}

static void testWasmModeOnly(w2c_game *instance) {
  uint8_t *memory = instance->w2c_env_memory->data;
  setupStream(memory, 128, 48000, 2);
  u32 result = 77;
  assert(pg_opus_frame(instance, 128, 512, 2, 1024, 960, &result) == 0);
  assert(result == 77 && !decoderCacheReady && pg_test_created == 0 && pg_test_wasm_calls == 0);
}

int main(void) {
  uint8_t memory[16384] = {0};
  pg_test_memory envMemory = { .data = memory, .size = sizeof memory };
  w2c_game instance = { .w2c_env_memory = &envMemory };
  pg_test_instance = &instance;
  const char *native = getenv("PG_NATIVE_OPUS");
  if (native && !strcmp(native, "0")) testWasmModeOnly(&instance);
  else if (getenv("PG_OPUS_VERIFY") && strcmp(getenv("PG_OPUS_VERIFY"), "0")) testVerifyMode(&instance);
  else { testNativeMode(&instance); testGuestAddressBounds(&instance); }
  cleanupProductionState();
  puts("Native Opus production-path tests passed.");
  return 0;
}
