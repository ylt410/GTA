#include "../Source/MetalRenderer/MRMeshSlabAllocator.h"
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <limits>

int main() {
    using mr::MeshSlabAllocator;
    auto growAllocate = [](MeshSlabAllocator &allocator, std::uint32_t bytes,
                           std::uint64_t completed, MeshSlabAllocator::Range &range) {
        if (allocator.allocate(bytes, completed, range)) return true;
        allocator.addSlot();
        return allocator.allocate(bytes, completed, range);
    };

    // An in-flight range cannot be reused or counted empty until the
    // contiguous completed prefix reaches its serial.
    MeshSlabAllocator serialSafe(1024, 1, 3);
    auto first = serialSafe.addSlot();
    MeshSlabAllocator::Range a;
    assert(serialSafe.allocate(768, 0, a) && a.slab == first && a.offset == 0);
    serialSafe.release(5, a.slab, a.offset, 768);
    assert(serialSafe.reclaim(4).empty());
    MeshSlabAllocator::Range b;
    assert(growAllocate(serialSafe, 512, 4, b) && b.slab != first);
    assert(serialSafe.stats().emptySlabs == 0);
    assert(serialSafe.reclaim(5).empty());
    MeshSlabAllocator::Range reused;
    assert(serialSafe.allocate(768, 5, reused));
    assert(reused.slab == first && reused.offset == 0);

    // Pressure trims backing in place, leaving active sparse IDs stable.
    MeshSlabAllocator sparse(1024, 1, 100);
    MeshSlabAllocator::Range live[4];
    for (int i = 0; i < 4; ++i) assert(growAllocate(sparse, 1024, 0, live[i]));
    assert(live[0].slab == 0 && live[1].slab == 1 && live[2].slab == 2 && live[3].slab == 3);
    sparse.release(2, live[1].slab, live[1].offset, 1024);
    sparse.release(2, live[3].slab, live[3].offset, 1024);
    const auto dropped = sparse.reclaim(1, true);
    assert(dropped.empty());
    assert(sparse.reclaim(2, true).size() == 1);
    assert(!sparse.hasBacking(1));
    assert(sparse.hasBacking(0) && sparse.hasBacking(2) && sparse.hasBacking(3));
    const auto sparseStats = sparse.stats();
    assert(sparseStats.activeSlabs == 2 && sparseStats.emptySlabs == 1);
    assert(sparseStats.liveBytes == 2048 && sparseStats.retainedBytes == 3072);
    assert(sparseStats.emptyBytes == 1024 && sparseStats.releasedBytes == 1024);
    MeshSlabAllocator::Range restored;
    assert(sparse.allocate(1024, 2, restored));
    assert(restored.slab == 3 && restored.offset == 0 && !restored.needsBacking);
    assert(sparse.allocate(256, 2, restored));
    assert(restored.slab == 1 && restored.offset == 0 && restored.needsBacking);
    MeshSlabAllocator::Range nextRange, tailRange;
    assert(sparse.allocate(256, 2, nextRange));
    assert(nextRange.slab == 1 && nextRange.offset == 256 && !nextRange.needsBacking);
    assert(sparse.allocate(512, 2, tailRange));
    assert(tailRange.slab == 1 && tailRange.offset == 512 && !tailRange.needsBacking);
    MeshSlabAllocator::Range overlap;
    assert(!sparse.allocate(1, 2, overlap)); // The revived slab is fully occupied.
    assert(sparse.hasBacking(2) && live[2].slab == 2);

    // Failed platform allocation can cancel a revived slot without leaving
    // overlapping free ranges; the next attempt receives the whole slot.
    MeshSlabAllocator cancellation(1024, 0, 0);
    MeshSlabAllocator::Range whole;
    assert(cancellation.allocate(1024, 0, whole) == false);
    cancellation.addSlot();
    assert(cancellation.allocate(1024, 0, whole));
    cancellation.release(1, whole.slab, whole.offset, 1024);
    assert(cancellation.reclaim(1, true).size() == 1);
    MeshSlabAllocator::Range failed;
    assert(cancellation.allocate(512, 1, failed) && failed.needsBacking);
    cancellation.cancelNewBacking(failed.slab);
    assert(cancellation.stats().retainedBytes == 0);
    MeshSlabAllocator::Range retried;
    assert(cancellation.allocate(1024, 1, retried) && retried.needsBacking && retried.offset == 0);
    assert(!cancellation.allocate(0, 1, overlap));
    assert(!cancellation.allocate(1025, 1, overlap));

    // A traversal peak drains back to the configured warm floor after an
    // idle completed-serial window; live ranges and floor backing survive.
    MeshSlabAllocator traversal(1024, 1, 3);
    MeshSlabAllocator::Range peak[5];
    for (int i = 0; i < 5; ++i) assert(growAllocate(traversal, 1024, 0, peak[i]));
    for (int i = 1; i < 5; ++i) traversal.release(10, peak[i].slab, 0, 1024);
    assert(traversal.reclaim(10).empty());
    const auto trimmed = traversal.reclaim(13);
    assert(trimmed.size() == 3);
    const auto steady = traversal.stats();
    assert(steady.activeSlabs == 1 && steady.emptySlabs == 1);
    assert(steady.liveBytes == 1024 && steady.retainedBytes == 2048);
    assert(steady.releasedBytes == 3072);
    // Allocation-side collection does not scan/retire warm backing. A later
    // frame-level pressure sweep can trim the same completed empty ranges.
    MeshSlabAllocator frameOnly(1024, 1, 0);
    MeshSlabAllocator::Range frames[3];
    for (auto &range : frames) assert(growAllocate(frameOnly, 1024, 0, range));
    for (auto &range : frames) frameOnly.release(1, range.slab, 0, 1024);
    assert(frameOnly.reclaim(2, true, false).empty());
    assert(frameOnly.stats().retainedBytes == 3072);
    assert(frameOnly.reclaim(2, true).size() == 2);

    // Completion can free an older slab during a backing-allocation retry.
    // Allocating after adding a new slot may validly choose that older ID.
    MeshSlabAllocator retryRace(1024, 2, 100);
    MeshSlabAllocator::Range raced;
    assert(growAllocate(retryRace, 1024, 0, raced));
    retryRace.release(3, raced.slab, 0, 1024);
    assert(!retryRace.allocate(1024, 2, overlap));
    retryRace.reclaim(3, true);
    const auto extra = retryRace.addSlot();
    assert(retryRace.allocate(1024, 3, overlap) && overlap.slab == raced.slab && overlap.slab != extra);
    assert(retryRace.stats().liveBytes == 1024 && retryRace.stats().retainedBytes == 2048);

    // Repeated traversal/reuse under delayed, unordered retirement: the model
    // tracks every live and still-in-flight byte, including revived backing.
    struct Held { MeshSlabAllocator::Range range; std::uint32_t length; std::uint64_t retired; };
    const auto liveSerial = std::numeric_limits<std::uint64_t>::max();
    MeshSlabAllocator random(4096, 2, 8);
    std::vector<Held> held;
    std::uint32_t seed = 0x7a21d593;
    auto next = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    std::uint64_t completed = 0;
    for (unsigned step = 0; step < 30000; ++step) {
        completed += next() & 1;
        const auto retired = random.reclaim(completed, step % 17 == 0, step % 3 == 0);
        held.erase(std::remove_if(held.begin(), held.end(), [&](const Held &h) { return h.retired <= completed; }), held.end());
        for (auto index : retired) for (const auto &h : held) assert(h.range.slab != index);
        if ((next() & 1) && held.size() < 400) {
            MeshSlabAllocator::Range range;
            const std::uint32_t length = (1 + next() % 24) * 64;
            assert(growAllocate(random, length, completed, range));
            for (const auto &h : held) if (h.range.slab == range.slab)
                assert(range.offset + length <= h.range.offset || h.range.offset + h.length <= range.offset);
            held.push_back({range, length, liveSerial});
        } else if (!held.empty()) {
            auto &h = held[next() % held.size()];
            if (h.retired == liveSerial) {
                h.retired = completed + 1 + next() % 12;
                random.release(h.retired, h.range.slab, h.range.offset, h.length);
            }
        }
        std::uint64_t expected = 0;
        for (const auto &h : held) expected += h.length;
        assert(random.stats().liveBytes == expected);
    }
    for (const auto &h : held) if (h.retired == liveSerial) random.release(completed + 1, h.range.slab, h.range.offset, h.length);
    random.reclaim(completed + 10000, true);
    const auto final = random.stats();
    assert(final.liveBytes == 0 && final.retainedBytes == 4096 && final.releasedBytes > 0);
    std::puts("Metal mesh slab allocator: serial safety, sparse stable IDs, pressure trimming, slot reuse, and traversal peak decay PASS");
}
