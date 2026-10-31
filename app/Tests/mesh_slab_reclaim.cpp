#if defined(MR_MESH_SLAB_ALLOCATOR_BASELINE)
#include "perf_baselines/MRMeshSlabAllocator-build23.h"
#else
#include "../Source/MetalRenderer/MRMeshSlabAllocator.h"
#endif

#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

static mr::MeshSlabAllocator::Range allocate(mr::MeshSlabAllocator &allocator,
                                             std::uint32_t bytes,
                                             std::uint64_t completed = 0) {
    mr::MeshSlabAllocator::Range range;
    assert(allocator.allocate(bytes, completed, range));
    return range;
}

static void testEmptyPending() {
    mr::MeshSlabAllocator allocator(64);
    assert(allocator.reclaim(100, false, false).empty());
    assert(allocator.reclaim(100, true, false).empty());
}

static void testFuturePendingAndPressure() {
    mr::MeshSlabAllocator allocator(64, 1, 0);
    const auto slab = allocator.addSlot();
    const auto live = allocate(allocator, 64);
    allocator.release(100, live.slab, live.offset, 64);

    // A pressure reclaim cannot make a range reusable before its serial ends.
    assert(allocator.reclaim(99, true, false).empty());
    auto stats = allocator.stats();
    assert(stats.activeSlabs == 1 && stats.liveBytes == 64);
    mr::MeshSlabAllocator::Range unavailable;
    assert(!allocator.allocate(1, 99, unavailable));
    assert(allocator.hasBacking(slab));

    // trimBacking still performs the backing scan when pending work is future.
    mr::MeshSlabAllocator trimming(32, 1, 0);
    trimming.addSlot();
    const auto busy = allocate(trimming, 32).slab;
    for (int i = 0; i < 3; ++i) trimming.addSlot();
    trimming.release(100, busy, 0, 32);
    const auto retired = trimming.reclaim(99, true, true);
    assert(retired.size() == 2);
    assert(trimming.hasBacking(busy));
    assert(trimming.stats().activeSlabs == 1);
}

static void testOutOfOrderReleasesAndWatermarkRefresh() {
    mr::MeshSlabAllocator allocator(128, 2, 1800);
    const auto slab = allocator.addSlot();
    const auto first = allocate(allocator, 8);
    const auto second = allocate(allocator, 8);
    const auto third = allocate(allocator, 8);
    assert(first.slab == slab && first.offset == 0);
    assert(second.offset == 8 && third.offset == 16);

    // Later serials are queued first; the subsequently released earlier range
    // must lower the watermark and be reclaimed immediately at serial 10.
    allocator.release(20, second.slab, second.offset, 8);
    allocator.release(30, third.slab, third.offset, 8);
    allocator.release(10, first.slab, first.offset, 8);
    assert(allocator.stats().liveBytes == 24);
    assert(allocator.reclaim(10, false, false).empty());
    assert(allocator.stats().liveBytes == 16);

    // Once the earliest range was removed, the watermark must be recomputed
    // from remaining serials. A still-later completion cannot release 20/30.
    const auto reused = allocate(allocator, 8, 10);
    assert(reused.offset == first.offset);
    allocator.release(5, reused.slab, reused.offset, 8);
    assert(allocator.reclaim(5, false, false).empty());
    assert(allocator.stats().liveBytes == 16);
    const auto reusedAgain = allocate(allocator, 8, 5);
    assert(reusedAgain.offset == first.offset);
    assert(allocator.reclaim(19, false, false).empty());
    assert(allocator.stats().liveBytes == 24);
    assert(allocator.reclaim(30, false, false).empty());
    assert(allocator.stats().liveBytes == 8);
}

static void testMaximumSerialIsNotSentinel() {
    mr::MeshSlabAllocator allocator(16, 1, 0);
    allocator.addSlot();
    const auto range = allocate(allocator, 8);
    allocator.release(std::numeric_limits<std::uint64_t>::max(),
                      range.slab, range.offset, 8);
    assert(allocator.reclaim(std::numeric_limits<std::uint64_t>::max(), false, false).empty());
    assert(allocator.stats().liveBytes == 0);
    const auto reused = allocate(allocator, 8, std::numeric_limits<std::uint64_t>::max());
    assert(reused.offset == range.offset);
}

int main() {
    testEmptyPending();
    testFuturePendingAndPressure();
    testOutOfOrderReleasesAndWatermarkRefresh();
    testMaximumSerialIsNotSentinel();
}
