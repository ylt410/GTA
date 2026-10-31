# Temporal input-production experiment (not a Temporal upscaler)

Base: `c01f7c84ddc3f9fd02f655614c34909a3a847f5e` on the successful input-diagnostics branch.
The experiment is opt-in, does not alter displayed color, and does not instantiate a MetalFX scaler.
Do not publish or label this as completed Temporal Super Resolution. The purpose is to prove real
scene depth and camera-motion inputs before implementing a complete reconstruction path.

## Corrected producer ABI

Pinned engine SHA-256: `11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0`.
The official WABT 1.0.42 disassembly of this exact engine establishes:

* `rage::grcViewport::SetCameraMtx(rage::Mat34V const&)` stores camera-to-world at
  viewport offsets 192/208/224/240 and its inverse view at 256/272/288/304.
* `SetWorldMtx` writes the current world transform at offsets 0..48.
* `RegenerateMatrices` uses inverse view at 256 and projection at 320; it writes
  the composed WVP at 128/144/160/176. The active-viewport branch copies the first
  256 bytes into rendering state. `SetCurrent` assigns global current viewport 12374976.
* Projection is at 320..383; width/height are at 864/868 and near/far at 872/876.

The original schema-1 report mislabeled camera-to-world (+192) as `viewCandidate`.
It must NOT be used directly as a view matrix. New captures read and recheck +256.
Schema 2 includes `cameraLayoutVersion: 2`. The analyzer retains old numbers unchanged
and warns explicitly; it does not silently reinterpret or rewrite the user's report.

Pinned shader ZIP SHA-256: `d628d3a7edbf2f76d9c8055017c3642ac9217fadf69c57d1a6c83e1bd6d39146`.
Verified default, ped and vehicle-basic shaders use:

```
world = x * b1[0] + y * b1[1] + z * b1[2] + b1[3]
clip  = x * b1[8] + y * b1[9] + z * b1[10] + b1[11]
```

Only these source-verified layouts may validate the live world/view/projection contract:
`6a6a0898a51cf403`, `134025f0da917112`, `2411632420e981ff`, `f524dc3c430c8361`.
Matching names or anonymous tokens are not a substitute for verified layout.
Static tire shader `3b7f4089f56426d3` instead uses b4[4..7] for clip position.
The shader source does not apply an explicit Y/Z remap at builtin position.

## Implemented experiment

* Default metadata-only capture remains available. A separate explicit UI action enables
  the camera/depth probe. Both preserve the existing delay, stop, report-sharing and overwrite guard.
* Consumer-side records are bounded to 32 per source frame (at most 1,920 across 60 complete
  frames). PRESENT, stage boundaries and selected identities have dedicated quotas; retained
  records are sorted by producer sequence. Intentional quota drops are separate from overflow.
* GPU resource, candidate-entity and transient draw-data identity pools are separate. Transient
  eviction cannot consume the persistent pools; opaque tokens are not raw pointers or proven
  generation-safe game identities.
* Actual main-G-buffer draw candidates require four matching color attachments, an actual
  single-sample readable Depth32 texture, full viewport, reversed-Z depth write/compare,
  finite camera data, no unsupported subresource/instancing/shader replacement, and an encoded DRAW.
  This is deliberately narrower than all game passes; it was supported by 763 rows over
  40 frames of the first capture. Scene-target SET_RT alone is not evidence of drawing.
* The current object world matrix and actual shader WVP are compared against captured view/projection.
  Unknown shader layouts do not establish this proof. A known-layout mismatch or multiple main
  cameras/depth resources in one frame fails closed.
* At the first stage-105 UI binding, before executing that binding, the renderer ends its previous
  encoders and samples retained scene depth. It never assumes the new UI target already holds
  finished clean scene color. Stage-103 writes and subsequent HUD writes are tracked as metadata.
* The production Metal kernel reads a 64x36 grid of actual depth and computes camera-only
  current-to-previous motion in full input-resolution pixels. CPU double-precision reference
  math checks the returned values. No synthetic game-depth values, zero-vector substitute,
  screenshot, full-resolution readback, or reconstruction output is used.
* At most 12 GPU dispatches are scheduled, one per five source frames. Three shared output
  buffers (216 KiB total payload) cannot be reused until GPU completion. Shader compilation
  occurs on a utility queue. There is no extra commit or synchronous renderer wait.
* New-session ownership, GPU completion and report capture are separate. Report saving waits
  at most two seconds on a utility queue and explicitly records pending/timed-out results.
  Stopping mid-frame is labeled `partialFrame`; it does not manufacture a PRESENT.
* Projection/viewport/range/resource discontinuities, missing history and conservative camera
  cuts reset or reject history. Clear/replacement of selected depth after observation invalidates
  that frame. Out-of-bounds rejection is NOT disocclusion correctness.
* Schema 2 records the actual built source SHA from the app's Info.plist, separately from its
  optimization baseline SHA. Metadata and sparse aggregate probe results remain bounded to 4 MiB.

## Verification and release gate

Run `python3 -B app/Scripts/check_performance_update.py` from the repository root.
It includes the production diagnostic core, C engine bridge, portable reprojection and
world/WVP consistency tests, patcher tests and schema-1/schema-2 analyzer tests.

On macOS, the same preflight also typechecks Swift and syntax-checks the iPhone Objective-C++
sources. `Tests/temporal_camera_probe.mm` compiles the real production module/kernel and uses
real Metal Depth32 attachments for controlled GPU checks. No Metal device is a distinct
SKIP (exit 77), never a GPU-test success. Linux cannot establish Apple SDK or GPU correctness.

Before offering an experimental IPA: require successful SDK compilation, inspect actual Metal
test status (not merely overall workflow success), verify the exact experimental branch/build SHA,
and retain the artifact-only workflow. Do not change main, the working Release, signing secrets,
or turn a diagnostic build into a Release. No CPU/GPU/RSS/FPS or quality gain is claimed here.

Recorded local verification: the complete Linux preflight passed after the +256 correction,
including 14 analyzer tests, three patcher tests, the C11 bridge, C++ reprojection/world-WVP
checks and the production diagnostic-core suite. The core also passed AddressSanitizer plus
UndefinedBehaviorSanitizer with leak detection explicitly disabled: LeakSanitizer itself cannot
inspect process-thread metadata in this sandbox and was NOT validated. `git diff --check` and
`bash -n build_ipa.sh` passed. Apple SDK/Metal execution remains pending; do not infer its success
from these Linux results. The C bridge fixture is tested as C11 (the generated engine is C);
it is not a C++-portable fixture because of its existing uncast `calloc` assignment.

## What still blocks full Temporal reconstruction

1. **All-scene motion:** the verified b4 current skin palette is `vec4[765]` (12,240 bytes),
   not a previous-pose palette. The first report did not capture b4. Candidate ped IDs are not
   proven lifetime/generation-safe; normal vehicle draws had no established entity identity.
   Camera-only motion is wrong for moving actors, wheels, vegetation and deformation.
2. **Producer coverage:** f44359 `CDrawVehicleVariationDC::ExecuteStatic` and f43044
   `CBreakable::AddToDrawBucket` are source-backed future candidate scopes. f43044 forwards
   root Matrix34 p2/current matrix-set p3 into the skinned draw path. They are not yet implemented
   or proven to cover normal main-scene draws. The old f10704 observations covered shadows.
3. **Jitter gating:** the active-viewport WVP construction is identified, but a correct main-scene-only
   producer scope spanning all relevant passes is not. RT-lock helpers and DeferredStart/End
   are short markers, not a long-lived scene scope. Blind b1-only jitter misses static tires;
   ungated viewport jitter can affect shadows/HUD. No jitter is injected by this experiment.
4. **Final color/HUD:** observed stage-103 ping-pong colors precede UI/HUD, but the exact final
   composed pre-HUD color/tone-map position is not certified. PRESENT contains HUD.
5. **History correctness:** disocclusion, object lifetime, pose/mesh identity, camera cuts, exposure,
   alpha/particles and reset policy need validation before real MetalFX Temporal integration.

After the safe experimental build passes its gates, collect separate short captures of a static
scene, slow camera pan over static geometry, and stationary-camera moving pedestrians/vehicles.
The first two validate actual depth and camera reprojection; the third must remain explicitly
unsupported for full motion until the production object/pose paths are solved.
