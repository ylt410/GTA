# Native performance update — v1.4.6 (18)

This private update removes frame generation completely: setting, native API,
GPU work, extra history textures and engine scene markers. Ordinary MetalFX
spatial upscaling remains a separate optional setting. It does not insert frames.

The comparison baseline for new optimizations is build 17 **with frame generation
off**. Turning a previously disabled feature into deleted code is not counted as
a measured optimization.

Changes at identical graphics settings:

- Keep an existing render pass when all 37 render-target/depth-binding fields
  match. Existing dirty state from clears or encoder changes remains in force.
- Reuse a constant-buffer snapshot for byte-identical updates within a frame.
  Partial/changed updates still produce immutable draw snapshots. Mixed
  constant/UAV buffers retain their normal GPU-upload behavior.
- Find ranges in the existing sorted mod-archive slot array by binary search;
  no duplicate index, descriptor policy or resource-format change.
- Reclaim an upload ring's temporary high-water tail after a complete quiet
  window and GPU completion. Expired strong constant-buffer references are
  released first. Steady sessions already using one 8 MiB chunk per slot need
  no trimming and receive no claimed memory reduction from this policy.
- Preserve build-17 local-read, immediate-binding and bounded log/cache work.
  Game files, saves, installed mods and required offline data are protected.
- Show the actual build number in Settings and startup logs. Retain the bundle
  identifier and both Files sharing declarations.
- Correct the coarse GPU Health average denominator: multiple command buffers
  belong to a source frame. Older Health values averaged per command buffer and
  are not directly comparable with this corrected per-source-frame value.

## Measurement method and limits

`Scripts/run_performance_benchmarks.py --report Performance-Benchmarks.json`
compares the previous paths with production optimization helpers on identical
synthetic workloads. CPU cases use repeated alternating trials and check
observable data. The memory stress case uses real touched CPU heap blocks and
isolated processes to model a burst followed by quiet use; it is not a game RSS
or Metal-memory measurement.

On macOS, Metal cases encode the same draws on both paths and compare rendered
pixels. They measure CPU encoding time and completed command-buffer GPU time;
they report actual resource allocations separately. Missing GPU access/timestamps
are explicitly skipped/null. A host GPU is not the user's iPhone GPU.

Do not turn a subsystem percentage into a whole-game percentage or an FPS claim.
The JSON retains unchanged/all-changing cases and regressions. Full-game CPU,
GPU and memory improvements remain unmeasured until a matched device A/B test.
The build checks these paths before the expensive engine compile, validates the
private device IPA, and keeps previous releases for rollback.

## Installation

Back up the existing game, mod and save directories before changing installs.
Use the same signing identity and bundle identifier to attempt an update. If the
installed page still shows the previous build, stop and check the signing tool's
installed app identity; do not delete the old app until its data has been exported.
Build 18 is shown as `v1.4.6 (18)` in Settings and the startup log.
