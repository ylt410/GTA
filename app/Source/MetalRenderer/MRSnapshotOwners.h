#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace mr {
// Track only buffers which materialised a constant snapshot. Resolve their
// IDs afresh at retirement; destroyed/replaced Buffer pointers are never kept.
class SnapshotOwners {
    std::unordered_set<std::uint32_t> ids_;
public:
    struct Stats { std::uint64_t visited = 0, released = 0, stale = 0; };
    void remember(std::uint32_t id) { ids_.insert(id); }
    void forget(std::uint32_t id) { ids_.erase(id); }
    std::size_t size() const { return ids_.size(); }
    template<class Lookup, class IsRetired>
    Stats retire(std::uint64_t currentFrame, Lookup lookup, IsRetired retired) {
        Stats stats;
        for (auto it = ids_.begin(); it != ids_.end();) {
            ++stats.visited;
            auto *buffer = lookup(*it);
            if (!buffer || !buffer->cbBuf) { ++stats.stale; it = ids_.erase(it); }
            else if (buffer->cbFrame != currentFrame && retired(buffer->cbBuf)) {
                buffer->cbBuf = {}; buffer->cbFrame = UINT64_MAX;
                buffer->cbOff = 0; buffer->cbCap = 0;
                ++stats.released; it = ids_.erase(it);
            } else ++it;
        }
        return stats;
    }
};
} // namespace mr
