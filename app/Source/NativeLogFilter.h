#ifndef PG_NATIVE_LOG_FILTER_H
#define PG_NATIVE_LOG_FILTER_H

#include <stdbool.h>
#include <string.h>

// These are routine, high-volume diagnostics observed in native-port.log.
// Keep the allowlist narrow: new engine warnings and unknown messages pass.
static inline bool pg_native_log_is_routine_noise(const char *line) {
  if (!line) return false;

  // Check for one of the narrowly reviewed routine forms first. Most engine
  // lines are ordinary diagnostics, so they avoid the safety-marker scans.
  bool candidate = strstr(line, "MiniMap: SONAR_BLIP - ") != NULL ||
                   strncmp(line, "fLastWaveDelta:", sizeof("fLastWaveDelta:") - 1) == 0 ||
                   strstr(line, "[Audio] Triggering deferred sound ") != NULL;
  const char *audio = strstr(line, "[Audio_DynamicMixer] ");
  if (audio) {
    audio += sizeof("[Audio_DynamicMixer] ") - 1;
    static const char *const routine[] = {
        "numPatchgroups ", "Starting audio scene ",
        "Stopping audio scene ", "ShutDown patch "};
    for (unsigned i = 0; i < sizeof routine / sizeof routine[0]; ++i)
      if (strncmp(audio, routine[i], strlen(routine[i])) == 0) { candidate = true; break; }
  }
  if (!candidate) return false;

  // Never hide failures or actionable diagnostics, even if wording overlaps a
  // routine prefix. This intentionally favors retaining false positives.
  static const char *const keep[] = {
      "Fatal", "FATAL", "fatal", "Error:", "ERROR", "error:",
      "Warning:", "WARNING", "warning:", "Assert", "assert", "TRAP",
      "Trap", "trap", "Pool", "pool"};
  for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; ++i)
    if (strstr(line, keep[i])) return false;
  return true;
}

#endif
