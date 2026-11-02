#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

// The mixer and device callback run independently. A missing mixer block must
// not turn the last non-zero sample into an instantaneous step to/from zero.
// This is gap concealment, not extra buffering: complete, uninterrupted PCM is
// copied bit-for-bit, no frames are held back, and the read index advances only
// for actual producer frames. The callback owns this small, allocation-free
// state; it never takes a lock, calls the filesystem, or waits for the producer.
#define PG_AUDIO_EDGE_FRAMES 32u // 0.67 ms at the engine's 48 kHz
typedef struct {
  float lastLeft, lastRight, anchorLeft, anchorRight;
  uint32_t edge;
  uint8_t gap;
} PGAudioContinuity;

static inline void pg_audio_continuity_reset(PGAudioContinuity *state) {
  memset(state, 0, sizeof *state);
}

static inline void pg_audio_continuity_render(PGAudioContinuity *state,
    const float *interleaved, uint32_t capacity, uint32_t read, uint32_t take,
    float *left, float *right, uint32_t count) {
  // Caller validates the power-of-two ring and limits take to available data.
  for (uint32_t i = 0; i < take; ++i) {
    uint32_t at = ((read + i) & (capacity - 1u)) * 2u;
    float l = interleaved[at], r = interleaved[at + 1u];
    if (state->gap) {
      state->gap = 0;
      state->anchorLeft = state->lastLeft;
      state->anchorRight = state->lastRight;
      state->edge = PG_AUDIO_EDGE_FRAMES;
    }
    if (state->edge) {
      // A resumed block joins the most recently emitted sample, even when a
      // short gap ended before the fade-out reached silence.
      float weight = (float)(PG_AUDIO_EDGE_FRAMES - --state->edge) / PG_AUDIO_EDGE_FRAMES;
      l = state->anchorLeft + (l - state->anchorLeft) * weight;
      r = state->anchorRight + (r - state->anchorRight) * weight;
    }
    left[i] = l; right[i] = r;
  }
  if (take) { state->lastLeft = left[take-1]; state->lastRight = right[take-1]; }
  if (take < count && !state->gap) {
    state->gap = 1;
    state->anchorLeft = isfinite(state->lastLeft) ? state->lastLeft : 0;
    state->anchorRight = isfinite(state->lastRight) ? state->lastRight : 0;
    state->edge = PG_AUDIO_EDGE_FRAMES;
  }
  for (uint32_t i = take; i < count; ++i) {
    float weight = state->edge ? (float)--state->edge / PG_AUDIO_EDGE_FRAMES : 0.0f;
    left[i] = state->anchorLeft * weight;
    right[i] = state->anchorRight * weight;
  }
  if (count) { state->lastLeft = left[count-1]; state->lastRight = right[count-1]; }
}
