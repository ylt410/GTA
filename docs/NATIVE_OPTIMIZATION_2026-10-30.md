# Native CPU, memory and asset-integrity optimization

Baseline: `72f81231b12828b5fb001f609b6dcb723207e230` (build 23).

This change optimizes the native host/renderer, not the external engine binary
or game assets. It does not alter render resolution, shaders, graphics quality,
GPU blits, frame concurrency, the two-core engine default, signing, or release
configuration. It is **not** evidence of an overall iPhone FPS, CPU-utilization,
GPU-time, or process-memory percentage improvement.

## Production changes

| Path | Change and preserved contract |
| --- | --- |
| `app/Source/EngineHost.c`, `NativeArchiveLookup.h` | Sort requested mod paths once, resolve them in one archive DFS, apply replacements in original order. Case-insensitive matching, duplicate-request order, first matching leaf, and missing-file handling remain. One request or allocation failure uses the single-request path. No engine addresses survive mount/ReInit. Overlong paths are rejected before descent rather than using `snprintf`'s untruncated length as an array index; the game-index walker uses the same bounded path helper. |
| `app/Source/MetalRenderer/MRMeshSlabAllocator.h` | Track the earliest pending serial. Repeated allocation-time reclamation skips the pending queue until any entry can complete. The watermark is updated during the existing compaction pass. Out-of-order releases and `UINT64_MAX` remain valid. Memory-pressure/backing trimming still runs when requested and cannot reclaim GPU-busy ranges. |
| `app/Source/MetalRenderer/MRInternal.h`, `MRRenderer.mm`, `MRCopyOnWriteBytes.h` | Full-resource copies share original BC CPU payloads; upload/region writes detach first. Source snapshots protect aliasing and map rehash. The original small-texture retention limit remains. GPU resources and blits are unchanged. Logs distinguish logical block bytes from unique live payload capacity, accounted peak and cloned bytes. |
| `app/Source/NativeAssets.m`, `NativeHTTPRange.h` | Before cached bytes can be accepted, require HTTP 206, the exact requested start/end, manifest total and body length. Reject same-length wrong-offset responses, malformed/overflowing fields and nonempty 416 responses. Preserve manifest mismatch signaling and network-traffic counters. Accept case-insensitive units and surrounding HTTP whitespace. |

`restore_precompiled_shaders.py` still pins all shader inputs and resource
checksums. The reviewed `MRInternal.h` fingerprint changes only for the new
CPU-shadow include/member type. A regression test reverses exactly those two
edits and requires the complete original header SHA-256. Shader interfaces and
reflection are not exempted from validation.

## Reproduce

```sh
python3 app/Scripts/check_performance_update.py
python3 app/Scripts/benchmark_native_optimization.py --rounds 5
python3 app/Tests/archive_production.py --benchmark --runs 7
```

The existing IPA workflow already calls the extended preflight. The benchmark
script prints raw JSON and does not use timing thresholds as correctness gates.
The historical allocator and archive functions are immutable fixtures under
`app/Tests/perf_baselines`; no Git history or downloaded game data is needed.

## Measurements and limits

Host: Linux x86-64, GCC/G++ 13.3.0, `-O2`. Five independent benchmark processes;
the numbers below are medians. Raw samples, compiler/platform details and scope
are in `native-optimization-benchmarks-2026-10-30.json`.

### CPU: pending-queue reclamation

Each measurement runs 1,000 calls with all pending serials still in the future;
setup is excluded, and a final completed reclamation verifies all bytes return.

| Pending records | Old time, 1,000 calls | New time, 1,000 calls | Scoped time reduction |
| ---: | ---: | ---: | ---: |
| 64 | 68,391 ns | 3,860 ns | 94.36% |
| 1,024 | 1,267,579 ns | 3,473 ns | 99.73% |
| 32,768 | 43,335,285 ns | 3,465 ns | 99.9920% |

This deliberately isolates the avoided scan; it is not a representative whole
frame. When GPU completion makes entries eligible, the necessary scan still
occurs. Actual queue lengths and call frequency on a phone are not measured.

### CPU: actual archive-mount functions

The real baseline/current `pg_packfile_mounted` functions run against the same
synthetic 8,192-leaf table, including allocation, sort, traversal and patching.
Each process alternates old/new order across seven trials; the table below is
the median of the five process medians. Every run verifies the patched entries.

| Mod requests | Old mount time | New mount time | Scoped time reduction |
| ---: | ---: | ---: | ---: |
| 1 (fallback) | 397,171 ns | 410,495 ns | -3.35% |
| 8 | 3,431,892 ns | 595,262 ns | 82.65% |
| 64 | 31,522,672 ns | 817,654 ns | 97.41% |
| 512 | 238,352,225 ns | 1,107,406 ns | 99.54% |

The one-request path deliberately avoids allocating an index and showed no
speedup (a small slowdown in this host run); it also includes the safer path
bound checks. For a separate 32,769-leaf / 512-request matcher corpus, exact
comparison calls fell from 16,777,728 to 332,208 (98.02%). That matcher benchmark
does not include construction of real engine paths; the production-function
benchmark above does. These measurements are not whole-game startup times.

### Memory: shared BC shadow payloads

The helper benchmark uses a flattened 128-square BC1 mip-chain payload of
10,936 bytes. Both columns include the source and all live copies; shared-owner,
map/vector metadata, allocator overhead and GPU textures are excluded.

| Copies / write pattern | Old payload | New unique payload | Payload reduction |
| --- | ---: | ---: | ---: |
| 1, read-only | 21,872 B | 10,936 B | 50.00% |
| 16, read-only | 185,912 B | 10,936 B | 94.12% |
| 64, read-only | 710,840 B | 10,936 B | 98.46% |
| 16, one modified copy | 185,912 B | 21,872 B | 88.24% |
| 64, four modified copies | 710,840 B | 54,680 B | 92.31% |
| 1, immediately modified | 21,872 B | 21,872 B | 0.00% |

The negative case matters: one immediately modified copy was slower with COW
(median 20,771 ns versus 11,306 ns for 100 clone/write batches). Sharing helps
only while copies remain shared, and metadata can outweigh benefits for tiny
or frequently overwritten payloads. Timing is helper-only, not the entire
renderer; full production mip/layer behavior is tested separately. No phone
RSS/physical-footprint improvement is asserted. The logged accounted peak does
not include transient old/new allocations during vector growth. Clone bytes and
payload owners created are cumulative since process launch, not per-report deltas
or the number of currently live owners.

### GPU and whole-App measurements

GPU work is intentionally unchanged. No Metal device or iOS SDK is available in
this Linux environment, so GPU-time, whole-App CPU, FPS and process RSS percentage
changes are **unmeasured**, not zero and not inferred from these microbenchmarks.
The actual iOS build and on-device route validation remain distinct gates.

## Correctness coverage

- Existing portable performance/cache tests, futex tests and removed-feature
  static checks remain enabled.
- Archive tests compile the real production walker/mount code with engine-memory
  stubs, including matching order and allocation-failure fallback.
- Renderer tests compile the actual CPU shadow upload/copy/readback blocks and
  check full bytes across mips/layers, aliasing, map rehash and source destruction.
- Allocator tests include 300,000 deterministic old/new differential operations,
  pressure, trimming, sparse slot reuse/cancellation and maximum serials.
- HTTP tests cover exact/clamped ranges, wrong offsets, totals, lengths, statuses,
  malformed fields, whitespace/case, decimal/request overflow and mismatch output.
- Ten shader-reuse fault tests retain exact ABI, IPA and resource integrity gates.

Final Linux preflight passed: 23 portable C/C++ executables, the two production
extraction suites, ten shader-reuse tests, futex and seven-file static checks.
Additional ASan+UBSan checks passed for archive production paths, COW and HTTP
validation; allocator differential testing also passed UBSan. LeakSanitizer was
disabled because this executor's tracing setup cannot support it. Existing GCC
warnings in the unchanged packed-cache tests/header remain; this is not a claim
of a warning-free or Apple-SDK build.

## Device validation still required

Use the baseline and new IPA on the same device, game files, save/route, graphics
settings and thermal state. Separate cold-start/mod mounting, warm-route
streaming and repeated copy/readback workloads. Capture at least three runs per
build: process CPU time, frame p50/p95, actual Metal GPU timing and peak physical
footprint, plus the new unique-payload log. Check texture readbacks/visuals and
cache behavior against a range server, including a deliberately wrong range.
Do not compare different upscaling settings or count shared payload savings as
an equal percentage of process memory.
