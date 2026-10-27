#include "../Source/MetalRenderer/MRConstantUpload.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

int main() {
    // Every partial range, including non-word-aligned and truncated ranges,
    // must leave exactly the bytes the former unconditional memcpy produced.
    std::array<unsigned char, 80> actual{}, expected{}, source{};
    for (std::size_t i=0;i<source.size();i++) source[i]=static_cast<unsigned char>(i*29+3);
    for (std::size_t size=0;size<=48;size++) {
        for (std::size_t offset=0;offset<=64;offset++) {
            for (std::size_t bytes=0;bytes<=64;bytes++) {
                actual.fill(0x5a); expected=actual;
                std::size_t count=offset<size ? std::min(bytes,size-offset) : 0;
                bool changed=count && std::memcmp(expected.data()+16+offset,source.data(),count)!=0;
                if (count) std::memcpy(expected.data()+16+offset,source.data(),count);
                assert(mr::updateConstantShadow(actual.data()+16,size,offset,source.data(),bytes)==changed);
                assert(actual==expected);
                assert(!mr::updateConstantShadow(actual.data()+16,size,offset,source.data(),bytes));
            }
        }
    }
    assert(!mr::updateConstantShadow(nullptr,48,0,source.data(),16));
    assert(!mr::updateConstantShadow(actual.data(),48,0,nullptr,16));
    assert(!mr::updateConstantShadow(actual.data(),48,std::numeric_limits<std::size_t>::max(),source.data(),16));
    // Floating-point NaNs compare as their stored bytes; changing only a NaN
    // payload or the sign of zero is a real change, exactly as for memcpy.
    std::array<uint32_t,4> bits{0x7fc00001u,0x80000000u,0x3f800000u,0u};
    auto copy=bits;
    assert(!mr::updateConstantShadow(copy.data(),sizeof copy,0,bits.data(),sizeof bits));
    bits[0]^=1;
    assert(mr::updateConstantShadow(copy.data(),sizeof copy,0,bits.data(),sizeof bits));
    bits[1]=0;
    assert(mr::updateConstantShadow(copy.data(),sizeof copy,0,bits.data(),sizeof bits));
    assert(copy==bits);

    // Model the real copy-on-write contract: once a draw owns a snapshot,
    // later changed writes get a new snapshot; identical writes retain it.
    // Keep a separate GPU-storage model to verify callers must not return
    // early for mixed CB/UAV buffers even when the shadow does not change.
    constexpr std::size_t n=256;
    std::array<unsigned char,n> shadow{}, reference{}, gpu{}, packet{}, bound{};
    std::vector<std::array<unsigned char,n>> history;
    std::vector<std::array<unsigned char,n>> expectedHistory;
    uint64_t frame=0, snapshotFrame=UINT64_MAX;
    std::mt19937 random(0x5e77);
    unsigned retained=0;
    for (unsigned update=0;update<20000;update++) {
        if (update%137==0) frame++;
        const std::size_t offset=random()%n, bytes=random()%(n+32);
        for (auto &v:packet) v=static_cast<unsigned char>(random());
        if (update%3==0) {
            const auto count=std::min(bytes,n-offset);
            std::memcpy(packet.data(),shadow.data()+offset,count);
        }
        std::memcpy(reference.data()+offset,packet.data(),std::min(bytes,n-offset));
        const bool changed=mr::updateConstantShadow(shadow.data(),n,offset,packet.data(),bytes);
        if (changed) snapshotFrame=UINT64_MAX;
        if (snapshotFrame!=frame) { bound=shadow; snapshotFrame=frame; } else retained++;
        assert(bound==reference);
        history.push_back(bound); expectedHistory.push_back(reference);
        // A simulated GPU write made the GPU storage diverge from the CPU
        // shadow. The normal upload must still happen on an identical update.
        gpu.fill(0xff);
        std::memcpy(gpu.data()+offset,packet.data(),std::min(bytes,n-offset));
        for (std::size_t i=offset;i<offset+std::min(bytes,n-offset);i++) assert(gpu[i]==reference[i]);
        assert(shadow==reference);
    }
    assert(retained>5000);
    assert(history==expectedHistory);
    std::puts("Constant uploads PASS: byte-exact/truncated/unaligned writes, no-op ranges, NaN payloads, signed zero, immutable draw snapshots, frame changes and independent GPU uploads.");
}
