#include "../Source/MetalRenderer/MRUploadRingRetention.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {
constexpr std::size_t blockBytes = 8u << 20;

void correctness() {
    using mr::UploadRingRetention;
    UploadRingRetention empty;
    for (int i = 0; i < 200; ++i) assert(empty.afterCompletedFrame(0, 0) == 0);

    // The initial peak remains warm throughout its window. Only a complete
    // subsequent window of smaller completed frames permits reclamation.
    UploadRingRetention decay(4);
    assert(decay.afterCompletedFrame(8, 8) == 8);
    for (int i = 0; i < 3; ++i) assert(decay.afterCompletedFrame(1, 8) == 8);
    for (int i = 0; i < 3; ++i) assert(decay.afterCompletedFrame(1, 8) == 8);
    assert(decay.afterCompletedFrame(1, 8) == 2);
    for (int i = 0; i < 20; ++i) assert(decay.afterCompletedFrame(1, 2) == 2);

    UploadRingRetention quiet(3);
    assert(quiet.afterCompletedFrame(0, 9) == 9);
    assert(quiet.afterCompletedFrame(0, 9) == 9);
    assert(quiet.afterCompletedFrame(0, 9) == 1);

    // Repeated bursts retain their storage and do not oscillate between
    // freeing and recreating blocks. A late burst must also protect its frame.
    UploadRingRetention bursts(6);
    for (int i = 0; i < 90; ++i)
        assert(bursts.afterCompletedFrame(i % 3 ? 1 : 8, 8) == 8);
    UploadRingRetention late(6);
    for (int i = 0; i < 5; ++i) assert(late.afterCompletedFrame(0, 8) == 8);
    assert(late.afterCompletedFrame(8, 8) == 8);

    // A recent peak can never grow this policy's allocation or trim below a
    // currently used block, even for malformed/overflow-sized bookkeeping.
    UploadRingRetention saturated(1);
    assert(saturated.afterCompletedFrame(1, 1) == 1);
    assert(saturated.afterCompletedFrame(100, 2) == 2);
    const auto maximum = std::numeric_limits<std::size_t>::max();
    assert(saturated.afterCompletedFrame(maximum, maximum) == maximum);
    UploadRingRetention zeroWindow(0);
    assert(zeroWindow.afterCompletedFrame(0, 2) == 1);

    // Simulated ownership tests use actual resource destructors. Reclaim is
    // deliberately invoked after completion; policy evaluation must not be
    // moved into allocation/encode while the slot remains in flight.
    struct Lifetime { std::size_t alive = 0, destroyed = 0; } lifetime;
    struct Resource {
        Lifetime *state;
        explicit Resource(Lifetime &s) : state(&s) { ++state->alive; }
        ~Resource() { --state->alive; ++state->destroyed; }
    };
    {
        std::vector<std::unique_ptr<Resource>> blocks;
        for (int i = 0; i < 8; ++i) blocks.emplace_back(new Resource(lifetime));
        UploadRingRetention lifetimePolicy(2);
        bool completed = false;
        auto resetCompletedSlot = [&](std::size_t used) {
            assert(completed);
            blocks.resize(lifetimePolicy.afterCompletedFrame(used, blocks.size()));
        };
        assert(lifetime.alive == 8 && lifetime.destroyed == 0);
        completed = true;
        resetCompletedSlot(8);
        resetCompletedSlot(1);
        assert(lifetime.alive == 8 && lifetime.destroyed == 0);
        resetCompletedSlot(1);
        resetCompletedSlot(1);
        assert(lifetime.alive == 2 && lifetime.destroyed == 6);
    }
    assert(lifetime.alive == 0 && lifetime.destroyed == 8);

    // Buffer::cbBuf is a strong reference in the actual renderer. Removing
    // tail entries alone does NOT release those resources. Reproduce that
    // counterexample, then clear only retired snapshots before reclamation.
    {
        std::vector<std::shared_ptr<Resource>> chunks, snapshots;
        for (int i = 0; i < 8; ++i) {
            auto resource = std::make_shared<Resource>(lifetime);
            chunks.push_back(resource);
            snapshots.push_back(resource);
        }
        auto retired = std::vector<std::shared_ptr<Resource>>(chunks.begin() + 2, chunks.end());
        chunks.resize(2);
        retired.clear();
        assert(lifetime.alive == 8); // leaked high-water storage if cbBuf is not cleared
        for (std::size_t i = 2; i < snapshots.size(); ++i) snapshots[i].reset();
        assert(lifetime.alive == 2);
        assert(chunks[0] == snapshots[0] && chunks[1] == snapshots[1]);
    }
    assert(lifetime.alive == 0 && lifetime.destroyed == 16);
    std::puts("Upload ring retention: completed-window peak decay, spare block, no startup growth, bursts, overflow, retirement and strong snapshot-owner regression PASS");
}

std::uint64_t residentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info_data_t data{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&data), &count) == KERN_SUCCESS)
        return data.resident_size;
#elif defined(__linux__)
    std::ifstream status("/proc/self/statm");
    std::uint64_t size = 0, resident = 0;
    if (status >> size >> resident) return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
    return 0;
}

struct Measurement {
    std::uint64_t retained = 0, rssStart = 0, rssPeak = 0, rssSteady = 0;
    unsigned allocations = 0, retirements = 0;
};

Measurement measure(bool trim) {
    Measurement m;
    m.rssStart = residentBytes();
    std::array<mr::UploadRingRetention, 3> policies;
    std::array<std::vector<std::shared_ptr<unsigned char[]>>, 3> slots, snapshots;
    auto grow = [&](auto &slot, auto &owners, std::size_t count) {
        while (slot.size() < count) {
            auto block = std::shared_ptr<unsigned char[]>(new unsigned char[blockBytes]);
            // Touch all bytes so this is genuinely backed process memory,
            // rather than merely virtual capacity or a simulated byte count.
            std::memset(block.get(), 0x5a, blockBytes);
            if (owners.size() <= slot.size()) owners.resize(slot.size() + 1);
            owners[slot.size()] = block;
            slot.push_back(std::move(block));
            ++m.allocations;
        }
    };
    for (unsigned frame = 0; frame < 3u * 131u; ++frame) {
        const unsigned index = frame % 3;
        auto &slot = slots[index];
        const std::size_t used = frame < 3 ? 8 : 1;
        grow(slot, snapshots[index], used);
        // Stand-in buffer work, identical in the before and after cases.
        slot[0][frame % blockBytes] ^= static_cast<unsigned char>(frame);
        if (frame == 2) m.rssPeak = residentBytes();
        if (trim) {
            const auto keep = policies[index].afterCompletedFrame(used, slot.size());
            m.retirements += static_cast<unsigned>(slot.size() - keep);
            // Mirrors the required renderer ownership step: stale constant
            // buffer snapshots must relinquish every retired tail resource.
            for (auto &owner : snapshots[index])
                for (std::size_t retired = keep; retired < slot.size(); ++retired)
                    if (owner == slot[retired]) { owner.reset(); break; }
            slot.resize(keep);
        }
    }
    for (const auto &slot : slots) m.retained += slot.size() * blockBytes;
    m.rssSteady = residentBytes();
    return m;
}

Measurement isolatedMeasurement(bool trim) {
#if defined(__APPLE__) || defined(__linux__)
    // Isolate allocator high-water state as well as resource lifetimes: a
    // previous case must not change malloc's mmap threshold for the next one.
    int channel[2];
    assert(pipe(channel) == 0);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(channel[0]);
        Measurement result = measure(trim);
        std::size_t sent = 0;
        while (sent < sizeof result) {
            const auto n = write(channel[1], reinterpret_cast<const unsigned char *>(&result) + sent, sizeof result - sent);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) _exit(2);
            sent += static_cast<std::size_t>(n);
        }
        close(channel[1]);
        _exit(0);
    }
    close(channel[1]);
    Measurement result{};
    std::size_t received = 0;
    while (received < sizeof result) {
        const auto n = read(channel[0], reinterpret_cast<unsigned char *>(&result) + received, sizeof result - received);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        received += static_cast<std::size_t>(n);
    }
    close(channel[0]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    return result;
#else
    return measure(trim);
#endif
}

void benchmark() {
    // Explicit burst -> steady micro-workload, not a recording of the user's
    // driving or an estimate of the entire GTA V app's memory. CPU heap blocks
    // model upload-ring resource retention; this is not a Metal GPU benchmark.
    std::vector<std::uint64_t> oldRSS, newRSS;
    Measurement before{}, after{};
    for (int repeat = 0; repeat < 5; ++repeat) {
        if (repeat % 2 == 0) { before = isolatedMeasurement(false); after = isolatedMeasurement(true); }
        else { after = isolatedMeasurement(true); before = isolatedMeasurement(false); }
        oldRSS.push_back(before.rssSteady);
        newRSS.push_back(after.rssSteady);
        assert(before.retained == 192u * 1024u * 1024u);
        assert(after.retained == 48u * 1024u * 1024u);
        assert(before.allocations == 24 && after.allocations == 24);
        assert(before.retirements == 0 && after.retirements == 18);
    }
    std::sort(oldRSS.begin(), oldRSS.end());
    std::sort(newRSS.begin(), newRSS.end());
    std::printf("{\"benchmark\":\"upload_ring_burst_to_steady\",\"repetitions\":5,"
                "\"resource\":\"touched_CPU_heap_blocks_not_Metal_buffers\","
                "\"isolated_process_cases\":true,"
                "\"before_retained_bytes\":%llu,\"after_retained_bytes\":%llu,"
                "\"retained_capacity_reduction_percent\":75.0,"
                "\"before_process_rss_median_bytes\":%llu,\"after_process_rss_median_bytes\":%llu,"
                "\"same_allocation_count\":24,\"retired_idle_blocks\":18,"
                "\"retired_strong_snapshot_references\":18,"
                "\"whole_game_memory_measured\":false,\"GPU_time_measured\":false}\n",
                static_cast<unsigned long long>(before.retained),
                static_cast<unsigned long long>(after.retained),
                static_cast<unsigned long long>(oldRSS[oldRSS.size()/2]),
                static_cast<unsigned long long>(newRSS[newRSS.size()/2]));
}
} // namespace

int main(int argc, char **argv) {
    correctness();
    if (argc > 1 && std::string(argv[1]) == "--benchmark") benchmark();
}
