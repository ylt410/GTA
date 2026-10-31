#pragma once
#include "NativeTemporalDiagnostics.h"
#ifdef __cplusplus
extern "C" {
#endif
// Renderer thread: called after a completed source PRESENT, never a GPU callback.
void PGTDPlatformFrame(void);
void PGTDPlatformShader(uint32_t token, const char *hash, const char *label);
#ifdef __cplusplus
}
#endif
