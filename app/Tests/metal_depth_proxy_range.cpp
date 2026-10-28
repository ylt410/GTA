#include "../Source/MetalRenderer/MRDepthProxyRange.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace mr;

static std::vector<std::vector<uint32_t>> makeMips(uint32_t width, uint32_t height,
                                                   uint32_t mips, uint32_t layers,
                                                   uint32_t seed) {
    std::vector<std::vector<uint32_t>> result((size_t)mips * layers);
    for (uint32_t mip = 0; mip < mips; ++mip) {
        const uint32_t w = width >> mip ? width >> mip : 1;
        const uint32_t h = height >> mip ? height >> mip : 1;
        for (uint32_t layer = 0; layer < layers; ++layer) {
            auto &plane = result[(size_t)mip * layers + layer];
            plane.resize((size_t)w * h);
            for (size_t i = 0; i < plane.size(); ++i)
                plane[i] = seed ^ (mip * 0x9e3779b9u) ^ (layer * 0x85ebca6bu) ^ (uint32_t)i;
        }
    }
    return result;
}

static void copyDemanded(const DepthProxyRange &range, uint32_t layers,
                         const std::vector<std::vector<uint32_t>> &source,
                         std::vector<std::vector<uint32_t>> &proxy,
                         DepthProxyValidity &validity) {
    for (uint32_t mip = range.baseMip; mip < range.baseMip + range.mipCount; ++mip) {
        for (uint32_t layer = range.firstLayer; layer < range.firstLayer + range.layerCount; ++layer) {
            if (validity.valid(mip, layer)) continue;
            proxy[(size_t)mip * layers + layer] = source[(size_t)mip * layers + layer];
            validity.markValid(mip, layer);
        }
    }
}

int main() {
    // Full view and bounded array view use the same mip/layer layout as the
    // source texture, including the last mip's 1x1 extent.
    auto full = depthProxyRange(4, 6, 0, UINT32_MAX, 0, 0, DepthProxyViewKind::Array);
    assert(full.valid && full.baseMip == 0 && full.mipCount == 4 && full.firstLayer == 0 && full.layerCount == 6);
    auto partial = depthProxyRange(4, 6, 2, 1, 3, 2, DepthProxyViewKind::Array);
    assert(partial.valid && partial.baseMip == 2 && partial.mipCount == 1 && partial.firstLayer == 3 && partial.layerCount == 2);
    auto lastMip = depthProxyRange(4, 6, 3, 1, 5, 1, DepthProxyViewKind::Single);
    assert(lastMip.valid && lastMip.baseMip == 3 && lastMip.firstLayer == 5);

    // Cube ranges retain the renderer's six-face rule and reject the same
    // malformed tail ranges srvTexture rejects.
    auto cube = depthProxyRange(3, 12, 1, 0, 6, 0, DepthProxyViewKind::Cube);
    assert(cube.valid && cube.baseMip == 1 && cube.mipCount == 2 && cube.firstLayer == 6 && cube.layerCount == 6);
    auto cubeArray = depthProxyRange(3, 18, 0, 1, 6, 2, DepthProxyViewKind::CubeArray);
    assert(cubeArray.valid && cubeArray.firstLayer == 6 && cubeArray.layerCount == 12);
    assert(!depthProxyRange(2, 10, 0, 1, 6, 0, DepthProxyViewKind::Cube).valid);
    assert(!depthProxyRange(2, 10, 0, 1, 6, 2, DepthProxyViewKind::CubeArray).valid);
    assert(!depthProxyRange(2, 12, 0, 1, 0, UINT32_MAX, DepthProxyViewKind::CubeArray).valid);
    // This count wraps to six if multiplied in uint32 before bounds checking.
    assert(!depthProxyRange(2, 12, 0, 1, 0, 2147483649u, DepthProxyViewKind::CubeArray).valid);
    assert(!depthProxyRange(0, 4, 0, 1, 0, 0, DepthProxyViewKind::Single).valid);

    // Pixel-like fixture: a partial request copies only demanded planes,
    // repeated requests reuse valid planes, and a write invalidates all.
    constexpr uint32_t width = 8, height = 4, mips = 4, layers = 6;
    auto source = makeMips(width, height, mips, layers, 0x13579bdfu);
    std::vector<std::vector<uint32_t>> proxy((size_t)mips * layers);
    DepthProxyValidity validity;
    assert(validity.configure(mips, layers, 0x111, 0x222));
    auto onePlane = depthProxyRange(mips, layers, 2, 1, 4, 1, DepthProxyViewKind::Array);
    copyDemanded(onePlane, layers, source, proxy, validity);
    assert(validity.valid(2, 4) && !validity.valid(2, 3) && !validity.valid(1, 4));
    auto before = proxy;
    copyDemanded(onePlane, layers, source, proxy, validity);
    assert(proxy == before);
    assert(proxy[(size_t)2 * layers + 4] == source[(size_t)2 * layers + 4]);

    auto wholeArray = depthProxyRange(mips, layers, 0, 0, 0, 0, DepthProxyViewKind::Array);
    copyDemanded(wholeArray, layers, source, proxy, validity);
    for (uint32_t mip = 0; mip < mips; ++mip) for (uint32_t layer = 0; layer < layers; ++layer) {
        assert(validity.valid(mip, layer));
        assert(proxy[(size_t)mip * layers + layer] == source[(size_t)mip * layers + layer]);
    }
    validity.invalidateAll();
    source = makeMips(width, height, mips, layers, 0x2468ace0u);
    copyDemanded(onePlane, layers, source, proxy, validity);
    assert(proxy[(size_t)2 * layers + 4] == source[(size_t)2 * layers + 4]);
    assert(!validity.valid(0, 0) && !validity.valid(3, 5));

    // New source/proxy identities and dimensions discard stale validity.
    assert(validity.configure(mips, layers, 0x333, 0x222));
    assert(!validity.valid(2, 4));
    validity.markValid(2, 4);
    assert(validity.configure(mips, layers + 1, 0x333, 0x222));
    assert(!validity.valid(2, 4) && validity.layers() == layers + 1);

    std::puts("Metal depth proxy ranges: mip/slice selection, cube bounds, cached partial copies, invalidation, identity and pixel fixture PASS");
}
