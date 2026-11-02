# Build 24: streaming, audio and combined bike input

Baseline: `1c33cd1ae5d5e22a5ba631adeb977578a6119f44` (the temporal-camera/depth experiment, already containing the native-optimization branch).
Engine inspected: release `ylt410/000`, tag `11`, `game.wasm` SHA-256
`11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0`.
The release contains the engine binary, not the original game C++ source. Engine findings below were checked against WABT 1.0.42 disassembly and its function-name section.

## Confirmed mechanisms and changes

### Demand reads and optional prefetch

`wasm_httpfs_pread` (function 198 in the pinned engine) publishes a ready slot, increments the request-table doorbell, notifies **one** waiter, and waits for its own result. Previously, six demand workers and the optional hint worker waited on that same doorbell. The hint worker cannot service demand slots, so a notification received by it could leave an otherwise idle demand request waiting for a 10 ms polling timeout.

The hint worker now retains its independent 10 ms pumping cadence without waiting on the demand doorbell. Default demand workers wait for events, not periodic timeouts. They capture the doorbell before scanning; the existing futex implementation checks its value again while joining the wait queue, preserving publication-during-scan correctness. The old combined-hint compatibility path retains its timeout. A relaxed status load also skips the claim CAS on idle/completed/busy slots; the CAS still exclusively claims ready slots. Read data, checksums, cache persistence, network limits and preload policy are unchanged.

This removes a concrete avoidable wait and idle polling. It does not establish that this one issue explains every region hitch: disk/network latency, engine resource construction and GPU work can still stall.

### Texture upload and pipeline creation

BC decode and BC1/BC3-to-ASTC transcode now accept caller-owned destinations. For private/in-flight textures that require a blit, the renderer decodes directly into its existing serial-retained upload ring, avoiding a full temporary allocation followed by a whole-image copy. Completed shared textures keep their direct `replaceRegion` path. No command-buffer completion wait or new lifetime/eviction policy is introduced; compressed shadows and format choices remain intact.

Sizing validates source spans, normalized pitches, overflow and a 256 MiB output ceiling before any destination allocation. Allocating compatibility APIs and direct-output APIs share these checks. Rejected malformed data must not grow the upload ring or allocate a large temporary.

Pipeline keys use an exact, ordered canonical vertex-layout identity instead of a transient object handle. Identical layouts recreated during streaming can reuse existing PSOs. Every layout field is included; hash collisions are checked by full descriptor equality. Vertex shader input signatures, binding strides and the existing render/blend/topology state remain in the PSO key. Shader translation and the precompiled shader-cache contract are unchanged.

### Audio decoding and discontinuities

The engine mixer (`rage::audMixerDeviceWasmDiscard::MixThread`, function 7394) is a separate paced thread; the inspected build produces 256-frame blocks into a 16,384-frame ring with a 3,072-frame target backlog. The native callback still advances only by actual producer frames and updates the same heartbeat. No extra buffering or output latency is added.

The previous native Opus cache held one mutex across decoding every stream. The replacement retains **128 globally shared slots**, reserves/pins active and queued leases, and serializes each decoder with FIFO tickets. Different decoders may run concurrently; one busy hash bucket cannot prematurely evict a decoder. Initialization failures, reconfiguration and full-cache waits are covered by tests. Verification mode remains opt-in, with thread-local scratch and separate brief statistics locking.

At a PCM underrun, abruptly changing a nonzero sample to zero creates a discontinuity; the output helper fades the previous sample to zero over 32 frames and joins resumed PCM over 32 frames (about 0.67 ms at 48 kHz). It is allocation-free and lock-free in the device callback. Uninterrupted PCM is copied bit-for-bit, including ring wrap. Long producer stalls can still cause silence; concealment cannot recover missing source samples.

Disassembly also shows a mixer device critical section shared with audio metadata updates. That alone does **not** prove generic map streaming holds it long enough to starve audio. This build does not blindly change engine lock semantics, thread priorities or mixer timing.

### Wheelie plus steering

The touch stick's circular normalization reduced the vertical lean value when horizontal steering was added. Bike/bicycle lean now uses an independently clamped vertical displacement; existing horizontal steering response remains. Bike/bicycle steering-button mode also retains the vertical stick, allowing a second finger to steer without releasing lean. Cars, boats and aircraft keep their existing mappings. The native virtual-pad merge and packed engine input already carry X and Y independently and are unchanged.

### Retired experiment and packaging

The temporal input sampler, camera/depth GPU probe, diagnostic UI, generated-engine injection, tests and experiment-only documents are removed. Existing spatial MetalFX and normal depth proxy behavior remain. Build 24 uses the existing app identifier and version 1.4.6; output is `GTA5Port_1.4.6_b24.ipa`, artifact `GTA5Port-b24-streaming-audio`. IPA validation rejects the retired diagnostic metadata/symbols.

## Regression gates and limits

Run `python3 app/Scripts/check_performance_update.py` from the repository root. Portable checks include:

- 27 C/C++ subsystem tests, including audio cache lifetime/order, audio edge continuity, canonical layout collision/handle reuse, texture upload policy and the inherited cache/memory/renderer safety tests.
- BC1–BC7 differential decode cases; caller-owned/allocating equivalence; ASTC block/layout/depth and malformed-input checks; zero-allocation rejection of truncated/oversized compressed regions.
- Real `NativeOpus.c` integration with controlled host/libopus stubs in native, verification and WASM-only modes. These test control flow, state ownership and failures, not acoustic equivalence of a full libopus/WASM decode.
- Source-extracted production demand-consumer code with real `WasmFutex.c`: six workers, 10,000 requests, single ownership, exact payload, publication at the scan/wait boundary, and stable idle wait counts. The producer protocol is simulated from the inspected engine; this is not execution of the complete game. Hint/preload behavior is source-reviewed rather than end-to-end runtime tested here.
- Existing actual archive/shadow CPU-path tests, shader-cache fault tests and futex race tests; retired-probe source guard.

Recorded local result on 2026-11-02: the complete integrated preflight passed, including the added zero-row-pitch BC1–BC7 multi-row/depth regression and overflow-safe Opus guest-memory boundaries. Shell syntax, all 80 Python files' syntax and `git diff --check` passed.

The audio cache was additionally exercised with ThreadSanitizer (`detect_deadlocks=0`, because the full-capacity fixture deliberately holds 128 decoder locks; this is a race check, not proof of deadlock freedom). Audio continuity and all three NativeOpus modes passed AddressSanitizer/UndefinedBehaviorSanitizer (LeakSanitizer disabled in this container). Existing packed-cache compiler warnings are not represented as a warning-free build.

On macOS the same preflight additionally runs the pure Swift bike-axis test, parses/typechecks the app, typechecks the combined UIKit control harness, and syntax-checks the renderer against the iPhone SDK. `Tests/run_airplane_controls.sh BOOTED_SIMULATOR_UDID` executes the UIKit regression on a simulator. It covers bike/bicycle and both lean/turn signs, button-plus-stick release orders, cancellation/context changes, and existing car/airplane mappings.

Local work was performed on Linux: Apple SDK compilation, UIKit execution, Metal execution, actual IPA validation and iPhone gameplay **remain separate required checks**. The workflow runs the SDK/preflight/archive gates before publishing the IPA artifact. No on-device FPS, p99 frame-time, RSS, power or audio-dropout improvement is claimed from host unit tests or staging microbenchmarks.

For device acceptance, compare the same save, mods, core count, texture/graphics/spatial-upscale settings, route and thermal state; test city/countryside high-speed travel with both first-pass and warm-cache laps. Check texture correctness, frame hitches and audible discontinuities, then test motorcycle/bicycle lean plus both steering directions and both release orders. Preserve before/after logs if a failure remains.
