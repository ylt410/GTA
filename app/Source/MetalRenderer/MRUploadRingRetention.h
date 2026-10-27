// Retain recently needed upload blocks; release a temporary loading peak only
// after a complete quieter window. Call only AFTER this frame slot's last
// command buffer has completed. The policy itself never touches GPU resources.
#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>

namespace mr {

class UploadRingRetention {
    unsigned window_, samples_ = 0;
    std::size_t peak_ = 0;
public:
    static constexpr unsigned defaultWindow = 60;
    explicit UploadRingRetention(unsigned completedFrames = defaultWindow)
        : window_(std::max(1u, completedFrames)) {}

    // An 8 MiB spare block avoids immediately reallocating after a small
    // fluctuation. No block is created by this function, including on startup.
    std::size_t afterCompletedFrame(std::size_t used, std::size_t allocated) {
        peak_ = std::max(peak_, used);
        if (++samples_ < window_) return allocated;
        std::size_t keep = peak_;
        if (keep < std::numeric_limits<std::size_t>::max()) ++keep;
        keep = std::max<std::size_t>(1, keep);
        samples_ = 0;
        peak_ = 0;
        return std::min(allocated, keep);
    }
};

} // namespace mr
