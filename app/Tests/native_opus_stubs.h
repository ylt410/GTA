#ifndef PG_NATIVE_OPUS_STUBS_H
#define PG_NATIVE_OPUS_STUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t opus_int32;

typedef struct { uint8_t *data; uint64_t size; } pg_test_memory;
typedef struct { pg_test_memory *w2c_env_memory; } w2c_game;
typedef struct OpusDecoder OpusDecoder;

struct OpusDecoder { int32_t fs, channels, gain, resets, decodes; };
enum { OPUS_BAD_ARG = -1, OPUS_RESET_STATE = 4028, OPUS_BANDWIDTH_NARROWBAND = 1101,
       OPUS_BANDWIDTH_MEDIUMBAND = 1102, OPUS_BANDWIDTH_WIDEBAND = 1103,
       OPUS_BANDWIDTH_SUPERWIDEBAND = 1104, OPUS_BANDWIDTH_FULLBAND = 1105 };
#define OPUS_SET_GAIN(gain) 6000, (gain)

extern int pg_test_create_failures;
extern int pg_test_created, pg_test_destroyed, pg_test_wasm_calls, pg_test_reentry_result;
extern bool pg_test_reenter_wasm;
extern w2c_game *pg_test_instance;

OpusDecoder *opus_decoder_create(opus_int32 fs, int channels, int *error);
void opus_decoder_destroy(OpusDecoder *decoder);
int opus_decoder_ctl(OpusDecoder *decoder, int request, ...);
int opus_decode_float(OpusDecoder *decoder, const unsigned char *data, opus_int32 len,
                      float *pcm, int frameSize, int decodeFec);
void pg_log(const char *message);
u32 PG_FN_OPUS_DECODE_FRAME(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frameSize);
u32 pg_opus_frame(w2c_game *instance, u64 st, u64 data, u32 len, u64 pcm, u32 frameSize, u32 *result);

#endif
