// The game's Opus frames, decoded by native libopus (Scripts/prepare_native_opus.py, Scripts/build_opus.py).
//
// The engine is libopus 1.5 (float) compiled to wasm. Its opus_decode, inlined into
// rage::audDecoderOpus::Update, parses a packet and sets the decoder's mode, bandwidth, frame size and
// stream channels from the packet's first byte, then calls opus_decode_frame for each frame with
// that frame's bytes (no first byte). Here every such call goes to a native OpusDecoder kept per
// engine decoder: the frame is put back behind a one-frame first byte built from those fields and
// decoded into the engine's output buffer. Both decoders see the same frames in the same order.
//
// PG_NATIVE_OPUS=0 keeps the wasm decoder. PG_OPUS_VERIFY=1 decodes every frame both ways, keeps the
// engine's result, and logs the largest sample difference every 10 seconds.
#include "HostInternal.h"
#include "engine-functions.h"
#include "opus.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

u32 PG_FN_OPUS_DECODE_FRAME(w2c_game *, u64, u64, u32, u64, u32);

// OpusDecoder fields in the engine's libopus 1.5 build (wasm64, no deep PLC).
enum { ST_CHANNELS = 8, ST_FS = 12, ST_GAIN = 44, ST_STREAM_CHANNELS = 60, ST_BANDWIDTH = 64, ST_MODE = 68,
       ST_PREV_MODE = 72, ST_FRAME_SIZE = 76 };
enum { MODE_SILK = 1000, MODE_HYBRID = 1001, MODE_CELT = 1002 };

typedef struct { uint64_t st; OpusDecoder *dec; int32_t fs, channels, gain; uint64_t used; } Slot;
static Slot slots[128];
static uint64_t useTick;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int insideWasm;
static int mode = -1;   // 0 wasm, 1 native, 2 verify
static struct { uint64_t frames, lost, mismatched; float maxDiff; double lastLog, nativeSeconds, wasmSeconds; } stats;
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int32_t r32(const uint8_t *base, uint64_t size, uint64_t address) {
  int32_t v = 0; if (address + 4 <= size) memcpy(&v, base + address, 4); return v;
}

static Slot *decoderFor(uint64_t st, int32_t fs, int32_t channels) {
  Slot *slot = NULL, *oldest = &slots[0];
  for (int i = 0; i < 128; i++) {
    if (slots[i].st == st && slots[i].dec) { slot = &slots[i]; break; }
    if (slots[i].used < oldest->used) oldest = &slots[i];
  }
  if (slot && (slot->fs != fs || slot->channels != channels)) { opus_decoder_destroy(slot->dec); slot->dec = NULL; }
  if (!slot || !slot->dec) {
    if (!slot) { slot = oldest; if (slot->dec) opus_decoder_destroy(slot->dec); }
    int error = 0;
    *slot = (Slot){ .st = st, .dec = opus_decoder_create(fs, channels, &error), .fs = fs, .channels = channels };
    if (!slot->dec) { slot->st = 0; return NULL; }
  }
  slot->used = ++useTick;
  return slot;
}

// The one-frame packet header ("code 0") for this mode, bandwidth and frame length; -1 if none fits.
static int tocFor(int32_t mode, int32_t bandwidth, int32_t frameSize, int32_t fs, int32_t streamChannels) {
  if (fs <= 0) return -1;
  int units = frameSize * 400 / fs;            // 2.5 ms steps
  if (units * fs != frameSize * 400) return -1;
  int config = -1, band = bandwidth - OPUS_BANDWIDTH_NARROWBAND;
  if (mode == MODE_SILK && band >= 0 && band <= 2) {
    int d = units == 4 ? 0 : units == 8 ? 1 : units == 16 ? 2 : units == 24 ? 3 : -1;
    if (d >= 0) config = band * 4 + d;
  } else if (mode == MODE_HYBRID && (bandwidth == OPUS_BANDWIDTH_SUPERWIDEBAND || bandwidth == OPUS_BANDWIDTH_FULLBAND)) {
    int d = units == 4 ? 0 : units == 8 ? 1 : -1;
    if (d >= 0) config = (bandwidth == OPUS_BANDWIDTH_FULLBAND ? 14 : 12) + d;
  } else if (mode == MODE_CELT && band >= 0 && band <= 4) {
    int d = units == 1 ? 0 : units == 2 ? 1 : units == 4 ? 2 : units == 8 ? 3 : -1;
    int b = bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ? 0 : band == 0 ? 0 : band - 1;  // CELT has no mediumband
    if (d >= 0) config = 16 + b * 4 + d;
  }
  return config < 0 ? -1 : config << 3 | (streamChannels == 2 ? 4 : 0);
}

static int decodeNative(Slot *slot, const uint8_t *base, uint64_t size, uint64_t st, uint64_t data, uint32_t len,
                        float *out, int32_t frameSize, int writeBack) {
  if (r32(base, size, st + ST_PREV_MODE) == 0) opus_decoder_ctl(slot->dec, OPUS_RESET_STATE);  // new or reset stream
  int32_t gain = r32(base, size, st + ST_GAIN);
  if (gain != slot->gain) { opus_decoder_ctl(slot->dec, OPUS_SET_GAIN(gain)); slot->gain = gain; }
  int32_t frameMode = r32(base, size, st + ST_MODE);
  if (len <= 1 || !data) {
    stats.lost++;
    int32_t limit = r32(base, size, st + ST_FRAME_SIZE);
    if (len <= 1 && data && limit > 0 && limit < frameSize) frameSize = limit;
    int n = opus_decode_float(slot->dec, NULL, 0, out, frameSize, 0);
    if (n < 0) { memset(out, 0, (size_t)frameSize * slot->channels * sizeof(float)); n = frameSize; }
    return n;
  }
  if (data + len > size) return OPUS_BAD_ARG;
  int toc = tocFor(frameMode, r32(base, size, st + ST_BANDWIDTH), r32(base, size, st + ST_FRAME_SIZE), slot->fs,
                   r32(base, size, st + ST_STREAM_CHANNELS));
  if (toc < 0) return OPUS_BAD_ARG - 1000;  // not expressible: the caller lets the wasm decoder run
  uint8_t packet[1 + 1500];
  if (len > 1500) return OPUS_BAD_ARG;
  packet[0] = (uint8_t)toc; memcpy(packet + 1, base + data, len);
  int n = opus_decode_float(slot->dec, packet, (opus_int32)len + 1, out, frameSize, 0);
  if (writeBack && n > 0) { int32_t m = frameMode; memcpy((uint8_t *)base + st + ST_PREV_MODE, &m, 4); }
  return n;
}

u32 pg_opus_frame(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frameSize, u32 *result) {
  if (mode < 0) {
    const char *native = getenv("PG_NATIVE_OPUS"), *verify = getenv("PG_OPUS_VERIFY");
    mode = native && !strcmp(native, "0") ? 0 : verify && *verify && strcmp(verify, "0") ? 2 : 1;
    pg_log(mode == 0 ? "Opus: wasm decoder (PG_NATIVE_OPUS=0)." : mode == 2 ? "Opus: native libopus, verified against the wasm decoder." : "Opus: native libopus.");
  }
  if (mode == 0 || insideWasm) return 0;
  uint8_t *base = instance->w2c_env_memory->data;
  uint64_t size = instance->w2c_env_memory->size;
  int32_t fs = r32(base, size, st + ST_FS), channels = r32(base, size, st + ST_CHANNELS);
  if (channels < 1 || channels > 2 || (int64_t)frameSize <= 0 || frameSize > 5760 || pcm + (uint64_t)frameSize * channels * 4 > size) return 0;
  pthread_mutex_lock(&lock);
  Slot *slot = decoderFor(st, fs, channels);
  if (!slot) { pthread_mutex_unlock(&lock); return 0; }
  float *out = (float *)(base + pcm);
  if (mode == 1) {
    int n = decodeNative(slot, base, size, st, data, len, out, (int32_t)frameSize, 1);
    stats.frames++;
    pthread_mutex_unlock(&lock);
    if (n == OPUS_BAD_ARG - 1000) return 0;
    *result = (u32)n;
    return 1;
  }
  // Verify: native into a scratch buffer first (it reads the engine's fields before the wasm call changes them).
  static float scratch[5760 * 2];
  double t0 = seconds();
  int n = decodeNative(slot, base, size, st, data, len, scratch, (int32_t)frameSize, 0);
  double t1 = seconds();
  insideWasm = 1;
  u32 wasm = PG_FN_OPUS_DECODE_FRAME(instance, st, data, len, pcm, frameSize);
  insideWasm = 0;
  stats.nativeSeconds += t1 - t0; stats.wasmSeconds += seconds() - t1;
  stats.frames++;
  if (n != (int32_t)wasm) stats.mismatched++;
  else for (int i = 0; i < n * channels; i++) { float d = fabsf(scratch[i] - out[i]); if (d > stats.maxDiff) stats.maxDiff = d; }
  double t = seconds();
  if (t - stats.lastLog > 10) {
    stats.lastLog = t;
    char line[260];
    snprintf(line, sizeof line, "Opus verify: %llu frames (%llu lost), %llu with a different length, largest sample difference %.6f; decode time native %.0f ms, wasm %.0f ms.",
             (unsigned long long)stats.frames, (unsigned long long)stats.lost, (unsigned long long)stats.mismatched, stats.maxDiff,
             stats.nativeSeconds * 1000, stats.wasmSeconds * 1000);
    pg_log(line);
  }
  pthread_mutex_unlock(&lock);
  *result = wasm;
  return 1;
}
