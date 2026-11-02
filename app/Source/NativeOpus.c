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
#ifdef PG_NATIVE_OPUS_TESTING
#include "../Tests/native_opus_stubs.h"
#else
#include "HostInternal.h"
#include "engine-functions.h"
#include "opus.h"
#endif
#include "AudioDecoderCache.h"
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

typedef struct { OpusDecoder *dec; int32_t fs, channels, gain; } Decoder;
static PGAudioDecoderCache decoderCache;
static int decoderCacheReady;
static pthread_once_t decoderCacheOnce = PTHREAD_ONCE_INIT;
static _Thread_local int insideWasm;
static PGAudioModeState modeState = PG_AUDIO_MODE_STATE_INITIALIZER;
static PGAudioDecoderStats stats;
static int statsReady;
static pthread_once_t statsOnce = PTHREAD_ONCE_INIT;
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void initCache(void) { decoderCacheReady = PGAudioDecoderCacheInit(&decoderCache); }
static void initStats(void) { statsReady = PGAudioDecoderStatsInit(&stats); }
static int selectMode(void *unused) {
  (void)unused;
  const char *native = getenv("PG_NATIVE_OPUS"), *verify = getenv("PG_OPUS_VERIFY");
  int mode = native && !strcmp(native, "0") ? 0 : verify && *verify && strcmp(verify, "0") ? 2 : 1;
  pg_log(mode == 0 ? "Opus: wasm decoder (PG_NATIVE_OPUS=0)." : mode == 2 ? "Opus: native libopus, verified against the wasm decoder." : "Opus: native libopus.");
  return mode;
}

static int32_t r32(const uint8_t *base, uint64_t size, uint64_t address) {
  int32_t v = 0; if (address <= size && size - address >= 4) memcpy(&v, base + address, 4); return v;
}

static void destroyDecoderValue(void *value) {
  Decoder *decoder = value;
  if (!decoder) return;
  if (decoder->dec) opus_decoder_destroy(decoder->dec);
  free(decoder);
}

static Decoder *newDecoder(int32_t fs, int32_t channels) {
  int error = 0;
  Decoder *decoder = calloc(1, sizeof *decoder);
  if (!decoder) return NULL;
  decoder->dec = opus_decoder_create(fs, channels, &error);
  if (!decoder->dec) { free(decoder); return NULL; }
  decoder->fs = fs; decoder->channels = channels;
  return decoder;
}
static void *createDecoderValue(int32_t fs, int32_t channels) { return newDecoder(fs, channels); }
static bool decoderMatches(void *value, int32_t fs, int32_t channels) {
  Decoder *decoder = value;
  return decoder && decoder->fs == fs && decoder->channels == channels;
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

static int decodeNative(Decoder *decoder, const uint8_t *base, uint64_t size, uint64_t st, uint64_t data, uint32_t len,
                        float *out, int32_t frameSize, int writeBack) {
  // Do not let malformed guest addresses wrap, and reject them before changing
  // decoder state (notably the reset/gain fields) so WASM fallback is pristine.
  if (len > 1 && data && (data > size || (uint64_t)len > size - data)) return OPUS_BAD_ARG;
  if (r32(base, size, st + ST_PREV_MODE) == 0) {
    opus_decoder_ctl(decoder->dec, OPUS_RESET_STATE);  // new or reset stream
  }
  int32_t gain = r32(base, size, st + ST_GAIN);
  if (gain != decoder->gain) { opus_decoder_ctl(decoder->dec, OPUS_SET_GAIN(gain)); decoder->gain = gain; }
  int32_t frameMode = r32(base, size, st + ST_MODE);
  if (len <= 1 || !data) {
    int32_t limit = r32(base, size, st + ST_FRAME_SIZE);
    if (len <= 1 && data && limit > 0 && limit < frameSize) frameSize = limit;
    int n = opus_decode_float(decoder->dec, NULL, 0, out, frameSize, 0);
    if (n < 0) { memset(out, 0, (size_t)frameSize * decoder->channels * sizeof(float)); n = frameSize; }
    return n;
  }
  int toc = tocFor(frameMode, r32(base, size, st + ST_BANDWIDTH), r32(base, size, st + ST_FRAME_SIZE), decoder->fs,
                   r32(base, size, st + ST_STREAM_CHANNELS));
  if (toc < 0) return OPUS_BAD_ARG - 1000;  // not expressible: the caller lets the wasm decoder run
  uint8_t packet[1 + 1500];
  if (len > 1500) return OPUS_BAD_ARG;
  packet[0] = (uint8_t)toc; memcpy(packet + 1, base + data, len);
  int n = opus_decode_float(decoder->dec, packet, (opus_int32)len + 1, out, frameSize, 0);
  if (writeBack && n > 0) { int32_t m = frameMode; memcpy((uint8_t *)base + st + ST_PREV_MODE, &m, 4); }
  return n;
}

u32 pg_opus_frame(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frameSize, u32 *result) {
  int mode = PGAudioModeGet(&modeState, selectMode, NULL);
  if (mode == 0 || insideWasm) return 0;
  uint8_t *base = instance->w2c_env_memory->data;
  uint64_t size = instance->w2c_env_memory->size;
  // All stream fields used below lie in [st, st + ST_FRAME_SIZE + 4). Check
  // the containing range once, before any st + offset expressions are formed.
  if (st > size || size - st < ST_FRAME_SIZE + 4) return 0;
  int32_t fs = r32(base, size, st + ST_FS), channels = r32(base, size, st + ST_CHANNELS);
  if (channels < 1 || channels > 2 || frameSize == 0 || frameSize > 5760) return 0;
  const uint64_t pcmBytes = (uint64_t)frameSize * (uint32_t)channels * sizeof(float);
  if (pcm > size || pcmBytes > size - pcm) return 0;
  pthread_once(&decoderCacheOnce, initCache);
  PGAudioDecoderLease lease;
  if (!decoderCacheReady || !PGAudioDecoderCacheAcquire(&decoderCache, st, &lease)) return 0;
  Decoder *decoder;
  if (!PGAudioDecoderLeaseEnsureValue(&lease, fs, channels, createDecoderValue, decoderMatches,
                                     destroyDecoderValue, (void **)&decoder)) return 0;
  float *out = (float *)(base + pcm);
  if (mode == 1) {
    int n = decodeNative(decoder, base, size, st, data, len, out, (int32_t)frameSize, 1);
    PGAudioDecoderCacheRelease(&lease);
    if (n == OPUS_BAD_ARG - 1000) return 0;
    *result = (u32)n;
    return 1;
  }
  // Verify: native into a scratch buffer first (it reads the engine's fields before the wasm call changes them).
  static _Thread_local float scratch[5760 * 2];
  pthread_once(&statsOnce, initStats);
  if (!statsReady) { PGAudioDecoderCacheRelease(&lease); return 0; }
  double t0 = seconds();
  int n = decodeNative(decoder, base, size, st, data, len, scratch, (int32_t)frameSize, 0);
  double t1 = seconds();
  insideWasm = 1;
  u32 wasm = PG_FN_OPUS_DECODE_FRAME(instance, st, data, len, pcm, frameSize);
  insideWasm = 0;
  float maxDiff = 0;
  if (n != (int32_t)wasm) maxDiff = 0;
  else for (int i = 0; i < n * channels; i++) { float d = fabsf(scratch[i] - out[i]); if (d > maxDiff) maxDiff = d; }
  PGAudioDecoderStatsRecord(&stats, 1, len <= 1 || !data, n != (int32_t)wasm,
                            maxDiff, t1 - t0, seconds() - t1);
  double t = seconds();
  PGAudioDecoderStatsSnapshot snapshot;
  int shouldLog = PGAudioDecoderStatsLogDue(&stats, t, 10, &snapshot);
  PGAudioDecoderCacheRelease(&lease);
  if (shouldLog) {
    char line[260];
    snprintf(line, sizeof line, "Opus verify: %llu frames (%llu lost), %llu with a different length, largest sample difference %.6f; decode time native %.0f ms, wasm %.0f ms.",
             (unsigned long long)snapshot.frames, (unsigned long long)snapshot.lost,
             (unsigned long long)snapshot.mismatched, snapshot.maxDiff,
             snapshot.nativeSeconds * 1000, snapshot.wasmSeconds * 1000);
    pg_log(line);
  }
  *result = wasm;
  return 1;
}
