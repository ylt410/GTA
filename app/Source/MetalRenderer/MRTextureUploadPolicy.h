#pragma once
#include <cstdint>

namespace mr {

// Shared CPU-visible textures can be replaced directly only after the last
// command buffer that used them has completed. Private or in-flight textures
// must take the staged blit path.
constexpr bool canReplaceSharedTexture(bool shared, uint64_t lastUse, uint64_t completed) {
	return shared && lastUse <= completed;
}

} // namespace mr
