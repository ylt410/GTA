// Exact D3D render-target binding comparisons, independent of Metal.
// Rebinding the same attachments is a no-op; it must not end the current
// render pass or clear a dirty flag raised by another command.
#pragma once
#include <cstdint>

namespace mr {

template <class Color, class Depth>
inline bool RenderTargetBindingsEqual(const Color (&colors)[8], const Depth &depth,
					 const uint32_t *packet) {
	for (unsigned i = 0; i < 8; i++) {
		const uint32_t *value = packet + i * 4;
		const Color &color = colors[i];
		if (color.tex != value[0] || color.mip != value[1] ||
			color.slice != value[2] || color.fmt != value[3]) return false;
	}
	const uint32_t *value = packet + 32;
	return depth.tex == value[0] && depth.mip == value[1] &&
		depth.slice == value[2] && depth.fmt == value[3] && depth.flags == value[4];
}

template <class Color, class Depth>
inline bool SetRenderTargetBindings(Color (&colors)[8], Depth &depth, const uint32_t *packet,
				   bool &targetsDirty, bool &pipelineDirty) {
	if (RenderTargetBindingsEqual(colors, depth, packet)) return false;
	for (unsigned i = 0; i < 8; i++) {
		const uint32_t *value = packet + i * 4;
		colors[i] = {value[0], value[1], value[2], value[3]};
	}
	const uint32_t *value = packet + 32;
	depth = {value[0], value[1], value[2], value[3], value[4]};
	targetsDirty = true;
	pipelineDirty = true;
	return true;
}

} // namespace mr
