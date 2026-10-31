#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeTemporalDiagnostics.h"

// Opt-in input-production experiment. This never changes the displayed image,
// enables a MetalFX scaler, or treats camera-only motion as actor motion.
void PGTDProbeBegin(uint64_t session, bool requested);
void PGTDProbeEnd(uint64_t session);
bool PGTDProbeRequested(void);
// Utility queue only: waits at most two seconds for already submitted GPU work.
NSDictionary *PGTDProbeCopySummary(uint64_t session);

// Renderer-thread inputs use actual Metal resources, not anonymous report IDs.
// worldRows is the shader's observed current object-to-world matrix when known;
// its relationship to WVP must be validated before a camera transform is used.
struct PGTDProbeDrawState {
    id<MTLTexture> depth = nil;
    uint32_t depthCompare = 0;
    bool depthWrite = false;
    bool fullViewport = false;
    bool worldKnown = false;
    float worldRows[16]{};
};
void PGTDProbeObserveSample(const PGTDMetalSample &sample, const PGTDProbeDrawState &state);
void PGTDProbeInvalidateDepth(id<MTLTexture> depth);
// Called BEFORE executing the first UI-target binding. Caller must end active
// encoders first; the probe reads retained scene depth, never the new UI target.
bool PGTDProbeNeedsBoundary(const PGTDTag &tag, uint32_t opcode);
void PGTDProbeEncodeBoundary(id<MTLCommandBuffer> commandBuffer, const PGTDTag &tag);
void PGTDProbeSourcePresent(void);
