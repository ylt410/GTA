// Serial-aware free-range and backing-retention policy for shared mesh slabs.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

namespace mr {

class MeshSlabAllocator {
public:
    struct Range { std::size_t slab = 0; std::uint32_t offset = 0; bool needsBacking = false; };
    struct Stats {
        std::uint64_t liveBytes = 0, retainedBytes = 0, emptyBytes = 0, releasedBytes = 0;
        std::size_t activeSlabs = 0, retainedSlabs = 0, emptySlabs = 0;
    };
private:
    struct Slot {
        std::map<std::uint32_t, std::uint32_t> free;
        std::uint64_t emptySince = 0;
        bool backed = false;
    };
    struct Pending { std::uint64_t serial; std::size_t slab; std::uint32_t offset, length; };
    std::vector<Slot> slots_;
    std::vector<Pending> pending_;
    std::uint32_t slabBytes_, warmFloor_;
    std::uint64_t emptyAge_, releasedBytes_ = 0;

    static void insertFree(Slot &slot, std::uint32_t offset, std::uint32_t length) {
        auto next = slot.free.lower_bound(offset);
        if (next != slot.free.end() && offset + length == next->first) {
            length += next->second;
            next = slot.free.erase(next);
        }
        if (next != slot.free.begin()) {
            auto previous = std::prev(next);
            if (previous->first + previous->second == offset) {
                previous->second += length;
                return;
            }
        }
        slot.free[offset] = length;
    }
    bool empty(const Slot &slot) const {
        return slot.free.size() == 1 && slot.free.begin()->first == 0 && slot.free.begin()->second == slabBytes_;
    }
public:
    explicit MeshSlabAllocator(std::uint32_t slabBytes, std::uint32_t warmFloor = 2,
                               std::uint64_t emptyAge = 1800)
        : slabBytes_(slabBytes), warmFloor_(warmFloor), emptyAge_(emptyAge) {}

    // Slot indices are permanent. A released backing can be replaced in place.
    std::size_t addSlot() {
        Slot slot;
        slot.free.emplace(0, slabBytes_);
        slot.backed = true;
        slots_.push_back(std::move(slot));
        return slots_.size() - 1;
    }
    bool hasBacking(std::size_t slab) const { return slab < slots_.size() && slots_[slab].backed; }

    bool allocate(std::uint32_t length, std::uint64_t completedSerial, Range &result) {
        if (!length || length > slabBytes_) return false;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot &slot = slots_[i];
            if (!slot.backed) continue;
            for (auto it = slot.free.begin(); it != slot.free.end(); ++it) {
                if (it->second < length) continue;
                const auto offset = it->first;
                const auto remainder = it->second - length;
                slot.free.erase(it);
                if (remainder) slot.free.emplace(offset + length, remainder);
                result = {i, offset};
                return true;
            }
        }
        // Reuse the stable index of a previously trimmed slot before growing
        // the vector. The caller creates the corresponding platform backing.
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            Slot &slot = slots_[i];
            if (slot.backed) continue;
            const auto remainder = slabBytes_ - length;
            slot.free.clear();
            if (remainder) slot.free.emplace(length, remainder);
            slot.backed = true;
            slot.emptySince = completedSerial;
            result = {i, 0, true};
            return true;
        }
        (void)completedSerial;
        return false;
    }
    void release(std::uint64_t serial, std::size_t slab, std::uint32_t offset, std::uint32_t length) {
        if (slab < slots_.size() && slots_[slab].backed) pending_.push_back({serial, slab, offset, length});
    }
    // Must be called with the renderer's contiguous completed-serial prefix.
    // Pressure bypasses age and retains one warm slot; ordinary trimming keeps
    // the configured floor and a full idle window to avoid allocation churn.
    std::vector<std::size_t> reclaim(std::uint64_t completedSerial, bool pressure = false, bool trimBacking = true) {
        std::size_t keep = 0;
        for (const Pending &p : pending_) {
            if (p.serial > completedSerial) { pending_[keep++] = p; continue; }
            Slot &slot = slots_[p.slab];
            insertFree(slot, p.offset, p.length);
            if (empty(slot)) slot.emptySince = completedSerial;
        }
        pending_.resize(keep);
        // Allocation calls only collect newly completed ranges. Empty-backing
        // scans and trimming run at frame retirement, not for each mesh buffer.
        if (!trimBacking) return {};
        std::size_t warm = 0;
        for (const Slot &slot : slots_) if (slot.backed && empty(slot)) ++warm;
        const std::size_t floor = pressure ? std::min<std::size_t>(1, warmFloor_) : warmFloor_;
        std::vector<std::size_t> retired;
        for (std::size_t i = 0; i < slots_.size() && warm > floor; ++i) {
            Slot &slot = slots_[i];
            if (!slot.backed || !empty(slot)) continue;
            const bool aged = completedSerial >= slot.emptySince && completedSerial - slot.emptySince >= emptyAge_;
            if (!pressure && !aged) continue;
            slot.backed = false;
            slot.free.clear();
            retired.push_back(i);
            --warm;
            releasedBytes_ += slabBytes_;
        }
        return retired;
    }
    void cancelNewBacking(std::size_t slab) {
        Slot &slot = slots_.at(slab);
        slot.free.clear();
        slot.backed = false;
    }
    Stats stats() const {
        Stats result;
        result.releasedBytes = releasedBytes_;
        for (const Slot &slot : slots_) {
            if (!slot.backed) continue;
            ++result.retainedSlabs;
            result.retainedBytes += slabBytes_;
            if (empty(slot)) { ++result.emptySlabs; result.emptyBytes += slabBytes_; }
            else {
                ++result.activeSlabs;
                std::uint64_t freeBytes = 0;
                for (const auto &range : slot.free) freeBytes += range.second;
                result.liveBytes += slabBytes_ - freeBytes;
            }
        }
        return result;
    }
};

} // namespace mr
