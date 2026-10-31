#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#if defined(MR_MESH_SLAB_ALLOCATOR_BASELINE)
#include "perf_baselines/MRMeshSlabAllocator-build23.h"
#define MR_BENCH_BUILD "build23-baseline"
#else
#include "../Source/MetalRenderer/MRMeshSlabAllocator.h"
#define MR_BENCH_BUILD "watermark"
#endif

static std::size_t parsePositive(const char *text, std::size_t fallback) {
    if (!text || !*text) return fallback;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end || value == 0 || value > std::numeric_limits<std::uint32_t>::max())
        return 0;
    return static_cast<std::size_t>(value);
}

int main(int argc, char **argv) {
    const std::size_t pendingCount = parsePositive(argc > 1 ? argv[1] : nullptr, 32768);
    const std::size_t repeatCount = parsePositive(argc > 2 ? argv[2] : nullptr, 1000);
    if (!pendingCount || !repeatCount || pendingCount == std::numeric_limits<std::uint32_t>::max()) {
        std::fputs("usage: benchmark_mesh_slab_reclaim [pending-count] [repeat-count]\n", stderr);
        return 2;
    }

    // One slab contains a distinct live byte for every pending record. Setup
    // happens outside the timed loop; every queued serial is safely in future.
    mr::MeshSlabAllocator allocator(static_cast<std::uint32_t>(pendingCount + 1), 1);
    const std::size_t slab = allocator.addSlot();
    for (std::size_t i = 0; i < pendingCount; ++i) {
        mr::MeshSlabAllocator::Range range;
        if (!allocator.allocate(1, 0, range) || range.slab != slab || range.offset != i) {
            std::fputs("allocator setup failed\n", stderr);
            return 1;
        }
        allocator.release(static_cast<std::uint64_t>(pendingCount) + i + 1,
                          range.slab, range.offset, 1);
    }

    // A volatile load keeps each call's completion argument observable, so
    // optimizing compilers cannot collapse the repeated old-path scans.
    volatile std::uint64_t completedSerial = 0;
    volatile std::size_t retiredCount = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < repeatCount; ++i) {
        const auto retired = allocator.reclaim(completedSerial, false, false);
        retiredCount = retiredCount + retired.size();
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    // Retire all records once after timing to check that benchmark setup left
    // the pending ranges valid and the actual production implementation works.
    (void)allocator.reclaim(static_cast<std::uint64_t>(pendingCount) * 2, false, false);
    const auto stats = allocator.stats();
    if (stats.liveBytes != 0 || stats.emptySlabs != 1 || retiredCount != 0) {
        std::fputs("post-benchmark reclaim verification failed\n", stderr);
        return 1;
    }

    std::printf("{\"build\":\"%s\",\"pending_records\":%zu,\"reclaim_calls\":%zu,"
                "\"elapsed_ns\":%lld,\"ns_per_call\":%.2f,\"postcheck_live_bytes\":%llu}\n",
                MR_BENCH_BUILD, pendingCount, repeatCount, static_cast<long long>(elapsed),
                static_cast<double>(elapsed) / static_cast<double>(repeatCount),
                static_cast<unsigned long long>(stats.liveBytes));
    return 0;
}
