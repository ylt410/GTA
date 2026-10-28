#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mr {
inline uint64_t spatialUpscaleConfiguration(int mode, int width, int height) {
	if (mode < 1 || mode > 3 || width < 2 || height < 2 || width > 8192 || height > 8192) return 0;
	return ((uint64_t)mode << 32) | ((uint64_t)(uint32_t)height << 16) | (uint32_t)width;
}
struct SpatialUpscaleTarget { uint32_t width = 0, height = 0; bool explicitTarget = false; };
inline SpatialUpscaleTarget spatialUpscaleTarget(uint64_t config, bool legacy,
		uint32_t inputWidth, uint32_t inputHeight, double screenWidth, double screenHeight) {
	SpatialUpscaleTarget result;
	if (!inputWidth || !inputHeight || (!config && !legacy)) return result;
	result.explicitTarget = config != 0;
	double width = config ? (double)(config & 65535) : screenWidth;
	double height = config ? (double)((config >> 16) & 65535) : screenHeight;
	if (!std::isfinite(width) || !std::isfinite(height) || width < 2 || height < 2) return {};
	// Keep the existing supported expansion limit; never shrink or crop via MetalFX.
	width = std::min(width, inputWidth * 3.0); height = std::min(height, inputHeight * 3.0);
	if (width < inputWidth * 1.05 || height < inputHeight * 1.05) return {};
	result.width = (uint32_t)width; result.height = (uint32_t)height;
	return result;
}
} // namespace mr
