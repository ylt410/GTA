// Bounded, opt-in bridge from generated wasm2c producer hooks to native diagnostics.
// Included by EngineHost.c after the generated engine type declarations are available.
#include "NativeTemporalDiagnostics.h"
#include <math.h>
#include <string.h>

static int pgtd_guest_range(w2c_game *instance, uint64_t address, uint64_t length) {
  if (!instance || !instance->w2c_env_memory || !instance->w2c_env_memory->data) return 0;
  const uint64_t size = instance->w2c_env_memory->size;
  return address <= size && length <= size - address;
}

static int pgtd_read(w2c_game *instance, uint64_t address, void *out, uint64_t length) {
  if (!pgtd_guest_range(instance, address, length)) return 0;
  memcpy(out, (const uint8_t *)(const void *)instance->w2c_env_memory->data + address,
         (size_t)length);
  return 1;
}

static int pgtd_camera_float_valid(float value) { return isfinite(value) != 0; }

int pg_td_enabled(void) { return PGTDEnabled(); }

void pg_td_packet(w2c_game *instance, u64 packetAddr) {
  // This gate must precede every guest-memory access: ordinary play pays only one native check.
  if (!PGTDEnabled() || !instance || !instance->w2c_env_memory) return;
  uint32_t op = 0, words = 0;
  if (!pgtd_guest_range(instance, packetAddr, 8u)) return;
  const uint8_t *packet = (const uint8_t *)(const void *)instance->w2c_env_memory->data + packetAddr;
  memcpy(&op, packet, sizeof op);
  memcpy(&words, packet + sizeof op, sizeof words);
  // Only draw, present, and render-target packets can carry diagnostic metadata.
  // Avoid entering the collector mutex for the engine's much larger state-command stream.
  if (op != 32u && op != 7u && op != 26u) return;
  if (!PGTDClaimPacket(op)) return;

  PGTDCamera camera;
  memset(&camera, 0, sizeof camera);
  uint64_t viewport = 0;
  if (pgtd_read(instance, 12374976ull, &viewport, sizeof viewport) && viewport &&
      viewport <= UINT64_MAX - 876ull) {
    camera.rawViewport = viewport;
    uint32_t width = 0, height = 0;
    float nearPlane = 0.0f, farPlane = 0.0f;
    float verifyView[16], verifyProjection[16];
    uint64_t verifyViewport = 0;
    int ok = pgtd_read(instance, viewport + 192ull, camera.view, sizeof camera.view) &&
             pgtd_read(instance, viewport + 320ull, camera.projection, sizeof camera.projection) &&
             pgtd_read(instance, viewport + 864ull, &width, sizeof width) &&
             pgtd_read(instance, viewport + 868ull, &height, sizeof height) &&
             pgtd_read(instance, viewport + 872ull, &nearPlane, sizeof nearPlane) &&
             pgtd_read(instance, viewport + 876ull, &farPlane, sizeof farPlane) &&
             pgtd_read(instance, 12374976ull, &verifyViewport, sizeof verifyViewport) &&
             verifyViewport == viewport &&
             pgtd_read(instance, viewport + 192ull, verifyView, sizeof verifyView) &&
             pgtd_read(instance, viewport + 320ull, verifyProjection, sizeof verifyProjection) &&
             memcmp(camera.view, verifyView, sizeof verifyView) == 0 &&
             memcmp(camera.projection, verifyProjection, sizeof verifyProjection) == 0;
    if (ok && width && height && width <= 32768u && height <= 32768u &&
        pgtd_camera_float_valid(nearPlane) && pgtd_camera_float_valid(farPlane) &&
        nearPlane > 0.0f && farPlane > nearPlane) {
      ok = 1;
      for (unsigned i = 0; i < 16; ++i) {
        if (!pgtd_camera_float_valid(camera.view[i]) ||
            !pgtd_camera_float_valid(camera.projection[i])) { ok = 0; break; }
      }
      if (ok) {
        camera.valid = 1;
        camera.width = width;
        camera.height = height;
        camera.nearPlane = nearPlane;
        camera.farPlane = farPlane;
      }
    }
  }
  PGTDProducerPacket((uint64_t)packetAddr, op, words, &camera);
}

u64 pg_td_entity_enter(w2c_game *instance, u32 kind, u64 data, u32 keyOffset, u64 aux) {
  if (!PGTDEnabled()) return 0;
  uint64_t key = 0;
  uint32_t flags = 0;
  if (data && keyOffset != UINT32_MAX && data <= UINT64_MAX - (uint64_t)keyOffset &&
      pgtd_read(instance, data + (uint64_t)keyOffset, &key, sizeof key)) flags = 1u;
  return (u64)PGTDScopeEnter(kind, (uint64_t)data, (uint64_t)aux, key, flags);
}

u64 pg_td_scope_enter(w2c_game *instance, u32 kind, u64 identity, u64 aux) {
  (void)instance;
  if (!PGTDEnabled()) return 0;
  return (u64)PGTDScopeEnter(kind, (uint64_t)identity, (uint64_t)aux, 0u, 0u);
}

void pg_td_scope_leave(u64 cookie) {
  if (cookie) PGTDScopeLeave((uint64_t)cookie);
}
