#include "../Source/NativeLogFilter.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  // Captured minimap trace lines vary every frame and therefore evade
  // EngineHost's adjacent-identical-line suppression.
  assert(pg_native_log_is_routine_noise(
      "<RenderThread> [ui] MiniMap: SONAR_BLIP - Setting as new blip - stealth_blip_78 78 - pos:298.82,843.51 - size:28.00 - alpha:100.00"));
  assert(pg_native_log_is_routine_noise(
      "<RenderThread> [ui] MiniMap: SONAR_BLIP - Setting as new blip - stealth_blip_78 78 - pos:299.10,842.52 - size:18.00 - alpha:100.00"));
  assert(pg_native_log_is_routine_noise(
      "fLastWaveDelta: 0.000000"));
  assert(pg_native_log_is_routine_noise(
      "<North Audio > [Audio_DynamicMixer] Starting audio scene with settings SLO_MO_SCENE at time 1094862"));
  assert(pg_native_log_is_routine_noise(
      "<North Audio > [Audio_DynamicMixer] ShutDown patch SLO_MO_PATCH at time 1094984, frame 26632"));
  assert(pg_native_log_is_routine_noise(
      "[Audio] Triggering deferred sound VEH_TYRE_BURST (1625953) from entity police3"));

  // Preserve every failure class, even if a routine marker appears in text.
  assert(!pg_native_log_is_routine_noise(
      "Fatal Error: [Audio] Triggering deferred sound failed"));
  assert(!pg_native_log_is_routine_noise(
      "Warning: <RenderThread> [ui] MiniMap: SONAR_BLIP - malformed position"));
  assert(!pg_native_log_is_routine_noise(
      "Error: [Audio_DynamicMixer] Starting audio scene failed"));
  assert(!pg_native_log_is_routine_noise(
      "[Audio] Triggering deferred sound pool allocation Pool Full"));

  // Other engine diagnostics remain visible by default.
  assert(!pg_native_log_is_routine_noise("Game loaded successfully."));
  assert(!pg_native_log_is_routine_noise("[performance] {\"sourceFps\":27}"));
  assert(!pg_native_log_is_routine_noise("Native trap: unreachable"));
  puts("PASS: narrow native log filter preserves failures and ordinary diagnostics");
  return 0;
}
