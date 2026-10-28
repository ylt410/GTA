#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mr {

// Mirrors the mip/slice selection in srvTexture for the view kinds which can
// be used by a depth comparison shader. Keeping this independent of Metal lets
// the range and validity rules be checked on non-Apple build hosts.
enum class DepthProxyViewKind : uint8_t { Single, Array, Cube, CubeArray };

struct DepthProxyRange {
	uint32_t baseMip = 0;
	uint32_t mipCount = 0;
	uint32_t firstLayer = 0;
	uint32_t layerCount = 0;
	bool valid = false;
};

inline DepthProxyRange depthProxyRange(uint32_t mips, uint32_t layers,
									 uint32_t viewBaseMip, uint32_t viewMipCount,
									 uint32_t viewFirstLayer, uint32_t viewLayerCount,
									 DepthProxyViewKind kind) {
	DepthProxyRange out;
	if (!mips || !layers) return out;
	out.baseMip = std::min(viewBaseMip, mips - 1);
	out.mipCount = (viewMipCount == UINT32_MAX || viewMipCount == 0)
		? mips - out.baseMip : std::min(viewMipCount, mips - out.baseMip);
	out.firstLayer = std::min(viewFirstLayer, layers - 1);
	switch (kind) {
	case DepthProxyViewKind::Cube:
		out.layerCount = 6;
		break;
	case DepthProxyViewKind::CubeArray:
		if (viewLayerCount > (layers - out.firstLayer) / 6) return out;
		out.layerCount = viewLayerCount ? viewLayerCount * 6 : (layers - out.firstLayer) / 6 * 6;
		break;
	case DepthProxyViewKind::Array:
		out.layerCount = viewLayerCount ? std::min(viewLayerCount, layers - out.firstLayer) : layers - out.firstLayer;
		break;
	case DepthProxyViewKind::Single:
		out.layerCount = 1;
		break;
	}
	if (!out.mipCount || !out.layerCount) return out;
	if ((kind == DepthProxyViewKind::Cube || kind == DepthProxyViewKind::CubeArray) &&
	    (out.firstLayer + out.layerCount > layers)) return out;
	out.valid = true;
	return out;
}

// Tracks which proxy mip/layer pairs contain the current source contents.
// Source/proxy identities and dimensions detect object replacement or resize;
// source writes invalidate all entries, while a sampled view fills only its
// demanded range.
class DepthProxyValidity {
	uintptr_t source_ = 0, proxy_ = 0;
	uint32_t mips_ = 0, layers_ = 0;
	std::vector<uint8_t> valid_;
public:
	bool configure(uint32_t mips, uint32_t layers, uintptr_t source, uintptr_t proxy) {
		if (mips_ == mips && layers_ == layers && source_ == source && proxy_ == proxy && !valid_.empty()) return false;
		mips_ = mips; layers_ = layers; source_ = source; proxy_ = proxy;
		valid_.assign((std::size_t)mips * layers, 0);
		return true;
	}
	void invalidateAll() { std::fill(valid_.begin(), valid_.end(), 0); }
	bool valid(uint32_t mip, uint32_t layer) const {
		return mip < mips_ && layer < layers_ && valid_[(std::size_t)mip * layers_ + layer] != 0;
	}
	void markValid(uint32_t mip, uint32_t layer) {
		if (mip < mips_ && layer < layers_) valid_[(std::size_t)mip * layers_ + layer] = 1;
	}
	uint32_t mips() const { return mips_; }
	uint32_t layers() const { return layers_; }
};

} // namespace mr
