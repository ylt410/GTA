// Differential regression between the optimized allocator and the immutable
// build-23 implementation. Operations are valid renderer lifetimes only:
// allocated ranges are retired once, and the completed serial is contiguous.
#define mr baseline_mr
#include "perf_baselines/MRMeshSlabAllocator-build23.h"
#undef mr
#include "../Source/MetalRenderer/MRMeshSlabAllocator.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using OldAllocator = baseline_mr::MeshSlabAllocator;
using NewAllocator = mr::MeshSlabAllocator;

static void require(bool condition, unsigned step, const char *what) {
    if (condition) return;
    std::fprintf(stderr, "mesh slab differential mismatch at step %u: %s\n", step, what);
    std::exit(1);
}

static bool sameStats(const OldAllocator::Stats &oldStats,
                      const NewAllocator::Stats &newStats) {
    return oldStats.liveBytes == newStats.liveBytes &&
           oldStats.retainedBytes == newStats.retainedBytes &&
           oldStats.emptyBytes == newStats.emptyBytes &&
           oldStats.releasedBytes == newStats.releasedBytes &&
           oldStats.activeSlabs == newStats.activeSlabs &&
           oldStats.retainedSlabs == newStats.retainedSlabs &&
           oldStats.emptySlabs == newStats.emptySlabs;
}

static bool sameRange(const OldAllocator::Range &oldRange,
                      const NewAllocator::Range &newRange) {
    return oldRange.slab == newRange.slab &&
           oldRange.offset == newRange.offset &&
           oldRange.needsBacking == newRange.needsBacking;
}

static void compareState(const OldAllocator &oldAllocator,
                         const NewAllocator &newAllocator,
                         std::size_t slots, unsigned step) {
    require(sameStats(oldAllocator.stats(), newAllocator.stats()), step, "stats");
    for (std::size_t slab = 0; slab < slots; ++slab)
        require(oldAllocator.hasBacking(slab) == newAllocator.hasBacking(slab),
                step, "backing state");
}

struct LiveRange {
    OldAllocator::Range oldRange;
    NewAllocator::Range newRange;
    std::uint32_t length;
    std::uint64_t submitSerial;
};

int main() {
    // xorshift32 gives a stable sequence independent of the C library's rand().
    std::uint32_t seed = 0x7a21d593u;
    auto next = [&seed]() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };

    constexpr std::uint32_t slabBytes = 2048;
    constexpr unsigned operationCount = 300000;
    OldAllocator oldAllocator(slabBytes, 2, 9);
    NewAllocator newAllocator(slabBytes, 2, 9);
    std::size_t slotCount = 0;
    std::vector<LiveRange> live;
    live.reserve(512);
    std::uint64_t submitted = 0;
    std::uint64_t completed = 0;
    std::uint64_t allocationCount = 0, releaseCount = 0, reclaimCount = 0;
    std::uint64_t pressureCount = 0, trimEnabledCount = 0, trimSkippedCount = 0;
    std::uint64_t revivedCount = 0, cancelCount = 0, addSlotCount = 0, retiredCount = 0;

    // Starting with several real backings exercises trimming immediately.
    for (unsigned i = 0; i < 5; ++i) {
        const std::size_t oldSlot = oldAllocator.addSlot();
        const std::size_t newSlot = newAllocator.addSlot();
        require(oldSlot == newSlot, 0, "initial addSlot index");
        ++slotCount;
    }

    for (unsigned step = 1; step <= operationCount; ++step) {
        // This is a monotonic contiguous completion prefix, never beyond a
        // submitted allocation's serial. Bound the window to keep the legacy
        // implementation's deliberate full-queue scan practical.
        const std::uint64_t advance = next() % 4;
        completed = std::min(submitted, completed + advance);
        if (submitted - completed > 96) completed = submitted - 96;

        const unsigned operation = next() % 100;
        if (operation < 39 && live.size() < 512) {
            std::uint32_t length = 16 + (next() % 32) * 16;
            if (length > slabBytes) length = slabBytes;
            OldAllocator::Range oldRange;
            NewAllocator::Range newRange;
            bool oldAllocated = oldAllocator.allocate(length, completed, oldRange);
            bool newAllocated = newAllocator.allocate(length, completed, newRange);
            require(oldAllocated == newAllocated, step, "allocate result");

            // Like the renderer's backing-allocation retry, create one new
            // backing and repeat only when the existing allocator reports full.
            if (!oldAllocated) {
                if (slotCount < 64) {
                    const std::size_t oldSlot = oldAllocator.addSlot();
                    const std::size_t newSlot = newAllocator.addSlot();
                    require(oldSlot == newSlot, step, "retry addSlot index");
                    ++slotCount;
                    ++addSlotCount;
                    oldAllocated = oldAllocator.allocate(length, completed, oldRange);
                    newAllocated = newAllocator.allocate(length, completed, newRange);
                    require(oldAllocated && newAllocated, step, "allocate after addSlot");
                }
            }
            if (oldAllocated) {
                require(sameRange(oldRange, newRange), step, "allocate range/backing flag");
                ++submitted;
                ++allocationCount;

                // cancelNewBacking is only valid before the platform backing is
                // accepted, so exercise it immediately for a subset of revived slots.
                if (oldRange.needsBacking) ++revivedCount;
                if (oldRange.needsBacking && (next() % 5 == 0)) {
                    oldAllocator.cancelNewBacking(oldRange.slab);
                    newAllocator.cancelNewBacking(newRange.slab);
                    ++cancelCount;
                } else {
                    live.push_back({oldRange, newRange, length, submitted});
                }
            }
        } else if (operation < 68 && !live.empty()) {
            // Pick a random still-live allocation, making release order differ
            // from submission order without issuing invalid duplicate releases.
            const std::size_t index = next() % live.size();
            const LiveRange range = live[index];
            oldAllocator.release(range.submitSerial, range.oldRange.slab,
                                 range.oldRange.offset, range.length);
            newAllocator.release(range.submitSerial, range.newRange.slab,
                                 range.newRange.offset, range.length);
            ++releaseCount;
            live[index] = live.back();
            live.pop_back();
        } else if (operation < 95) {
            const bool pressure = (next() % 13 == 0);
            const bool trimBacking = (next() % 3 != 0);
            ++reclaimCount;
            if (pressure) ++pressureCount;
            if (trimBacking) ++trimEnabledCount;
            else ++trimSkippedCount;
            const std::vector<std::size_t> oldRetired =
                oldAllocator.reclaim(completed, pressure, trimBacking);
            const std::vector<std::size_t> newRetired =
                newAllocator.reclaim(completed, pressure, trimBacking);
            require(oldRetired == newRetired, step, "retired backing indices");
            retiredCount += oldRetired.size();
        } else if (slotCount < 64) {
            const std::size_t oldSlot = oldAllocator.addSlot();
            const std::size_t newSlot = newAllocator.addSlot();
            require(oldSlot == newSlot, step, "addSlot index");
            ++slotCount;
            ++addSlotCount;
        }

        compareState(oldAllocator, newAllocator, slotCount, step);
    }

    // Drain all remaining real allocations through one completed serial. This
    // is a valid final retirement and catches latent partition/accounting drift.
    ++submitted;
    for (const LiveRange &range : live) {
        oldAllocator.release(submitted, range.oldRange.slab,
                             range.oldRange.offset, range.length);
        newAllocator.release(submitted, range.newRange.slab,
                             range.newRange.offset, range.length);
    }
    completed = submitted;
    const auto oldFinalRetired = oldAllocator.reclaim(completed, true, true);
    const auto newFinalRetired = newAllocator.reclaim(completed, true, true);
    require(oldFinalRetired == newFinalRetired, operationCount, "final retired indices");
    retiredCount += oldFinalRetired.size();
    compareState(oldAllocator, newAllocator, slotCount, operationCount);
    require(oldAllocator.stats().liveBytes == 0, operationCount, "final live bytes");
    require(allocationCount > 1000 && releaseCount > 1000 && reclaimCount > 1000,
            operationCount, "random operation coverage");
    require(pressureCount > 0 && trimEnabledCount > 0 && trimSkippedCount > 0,
            operationCount, "reclaim mode coverage");
    require(revivedCount > 0 && cancelCount > 0 && addSlotCount > 0 && retiredCount > 0,
            operationCount, "backing lifecycle coverage");

    // Explicit maximum serial boundary comparison; this is a valid single
    // allocation retirement, not an invalid/double-free probe.
    OldAllocator maxOld(32, 1, 0);
    NewAllocator maxNew(32, 1, 0);
    maxOld.addSlot();
    maxNew.addSlot();
    OldAllocator::Range maxOldRange;
    NewAllocator::Range maxNewRange;
    require(maxOld.allocate(32, 0, maxOldRange) && maxNew.allocate(32, 0, maxNewRange),
            operationCount, "maximum-serial setup");
    maxOld.release(std::numeric_limits<std::uint64_t>::max(), maxOldRange.slab,
                   maxOldRange.offset, 32);
    maxNew.release(std::numeric_limits<std::uint64_t>::max(), maxNewRange.slab,
                   maxNewRange.offset, 32);
    const auto maxOldRetired = maxOld.reclaim(std::numeric_limits<std::uint64_t>::max(), false, false);
    const auto maxNewRetired = maxNew.reclaim(std::numeric_limits<std::uint64_t>::max(), false, false);
    require(maxOldRetired == maxNewRetired, operationCount, "maximum-serial reclaim");
    compareState(maxOld, maxNew, 1, operationCount);

    std::puts("Mesh slab allocator differential: 300000 deterministic valid operations PASS");
    return 0;
}
