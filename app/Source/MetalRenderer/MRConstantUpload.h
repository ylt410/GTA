// Exact-content updates for the renderer's CPU-owned constant-buffer shadow.
// No GPU-written data is compared here. A caller must still execute any
// separate GPU-buffer upload required by mixed CB/SRV/UAV buffers.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mr {

// The renderer has already bounded the packet; truncate to the shadow just as
// its previous memcpy did. Return true only when the observable bytes change,
// allowing an existing frame-ring snapshot to survive identical updates.
inline bool updateConstantShadow(void *shadow, std::size_t shadowBytes,
                                 std::size_t offset, const void *source,
                                 std::size_t sourceBytes) {
    if (!shadow || !source || offset >= shadowBytes) return false;
    const std::size_t count = std::min(sourceBytes, shadowBytes - offset);
    if (!count) return false;
    auto *destination = static_cast<unsigned char *>(shadow) + offset;
    // Matrices and draw-local parameters commonly change at the start of the
    // block. A fixed-size prefix comparison inlines on the native compilers,
    // avoiding a libc memcmp call for that all-changing case. The full compare
    // still covers every byte before declaring an upload identical.
    const bool prefixChanged=count>=16 && std::memcmp(destination,source,16)!=0;
    if (!prefixChanged && std::memcmp(destination, source, count) == 0) return false;
    std::memcpy(destination, source, count);
    return true;
}

} // namespace mr
