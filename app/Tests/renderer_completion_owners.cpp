#include "../Source/MetalRenderer/MRSerialCompletion.h"
#include "../Source/MetalRenderer/MRSnapshotOwners.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

static mr::SerialCompletion::Stats awaitBlocks(mr::SerialCompletion &done, unsigned count) {
    mr::SerialCompletion::Stats total;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        auto stats = done.takeStats();
        total.blocks += stats.blocks; total.wakeups += stats.wakeups;
        total.notifications += stats.notifications;
        if (total.blocks >= count) return total;
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    } while (true);
}

static void completion() {
    mr::SerialCompletion done;
    // A long completed history leaves no notifications/tokens to drain when a
    // future GPU serial genuinely needs waiting.
    for (unsigned i = 1; i <= 100000; ++i) done.publish(i);
    auto history = done.takeStats();
    assert(history.notifications == 0 && history.blocks == 0);
    done.publish(2); // out-of-order callbacks cannot regress the level.
    assert(done.completed() == 100000);
    done.wait(99999);
    auto pending = std::async(std::launch::async, [&] { done.wait(100001); });
    auto registered = awaitBlocks(done, 1);
    assert(pending.wait_for(20ms) == std::future_status::timeout);
    done.publish(100001);
    assert(pending.wait_for(2s) == std::future_status::ready); pending.get();
    auto active = done.takeStats();
    assert(registered.blocks + active.blocks == 1 && active.notifications == 1 && active.wakeups >= 1);
    // Two independent target levels share the same condition safely.
    auto first = std::async(std::launch::async, [&] { done.wait(100002); });
    auto second = std::async(std::launch::async, [&] { done.wait(100003); });
    awaitBlocks(done, 2);
    assert(first.wait_for(10ms) == std::future_status::timeout);
    assert(second.wait_for(10ms) == std::future_status::timeout);
    done.publish(100002);
    assert(first.wait_for(2s) == std::future_status::ready); first.get();
    assert(second.wait_for(10ms) == std::future_status::timeout);
    done.publish(100003);
    assert(second.wait_for(2s) == std::future_status::ready); second.get();
    // Race publication directly against wait registration. Every target must
    // eventually wake; the predicate and notification are the production code.
    mr::SerialCompletion race;
    std::atomic<unsigned> request{0};
    auto waiter = std::async(std::launch::async, [&] {
        for (unsigned i = 1; i <= 20000; ++i) {
            request.store(i); race.wait(i);
        }
    });
    std::thread publisher([&] {
        for (unsigned i = 1; i <= 20000; ++i) {
            while (request.load() < i) std::this_thread::yield();
            race.publish(i);
        }
    });
    assert(waiter.wait_for(10s) == std::future_status::ready);
    waiter.get(); publisher.join();
    assert(race.completed() == 20000);
    mr::SerialCompletion reordered;
    reordered.publish(3); assert(reordered.completed() == 0);
    reordered.publish(1); assert(reordered.completed() == 1);
    auto later = std::async(std::launch::async, [&] { reordered.wait(3); });
    awaitBlocks(reordered, 1);
    assert(later.wait_for(10ms) == std::future_status::timeout);
    reordered.publish(2); assert(reordered.completed() == 3);
    assert(later.wait_for(2s) == std::future_status::ready); later.get();
    mr::SerialCompletion targeted;
    auto final = std::async(std::launch::async, [&] { targeted.wait(100); });
    awaitBlocks(targeted, 1);
    for(unsigned i=1;i<100;i++)targeted.publish(i);
    assert(targeted.takeStats().notifications==0);
    assert(final.wait_for(10ms)==std::future_status::timeout);
    targeted.publish(100);
    assert(final.wait_for(2s)==std::future_status::ready);final.get();
    assert(targeted.takeStats().notifications==1);
}

static void owners() {
    struct Resource { unsigned *alive; explicit Resource(unsigned &n):alive(&n) {++n;} ~Resource(){--*alive;} };
    struct Buffer {
        std::shared_ptr<Resource> cbBuf;
        std::uint64_t cbFrame = UINT64_MAX;
        unsigned cbOff = 0, cbCap = 0;
    };
    unsigned alive = 0;
    auto retired = std::make_shared<Resource>(alive);
    auto retained = std::make_shared<Resource>(alive);
    auto newer = std::make_shared<Resource>(alive);
    std::unordered_map<unsigned, Buffer> table;
    mr::SnapshotOwners index;
    auto lookup = [&](unsigned id) -> Buffer * {
        auto it = table.find(id); return it == table.end() ? nullptr : &it->second;
    };
    auto isRetired = [&](const auto &resource) { return resource == retired; };
    // Unrelated meshes never enter the owner set, even in a large object table.
    for (unsigned id = 1000; id < 101000; ++id) table.emplace(id, Buffer{});
    table[1] = {retired, 1, 12, 64}; index.remember(1); index.remember(1);
    table[2] = {retired, 10, 24, 128}; index.remember(2); // current frame protected.
    table[3] = {retained, 1, 36, 256}; index.remember(3); // old block retained.
    table[4] = {retired, UINT64_MAX, 48, 512}; index.remember(4); // upload invalidated snapshot.
    table[5] = {retired, 1, 60, 512}; index.remember(5);
    table.erase(5); // destroy/replacement must not leave a dangling pointer.
    index.remember(6); table[6] = {}; // missing/empty ownership pruned.
    table[7] = {retired, 1, 72, 512}; index.remember(7);
    table.erase(7); table[7] = {newer, 9, 84, 1024}; // reused ID, different owner.
    assert(index.size() == 7);
    auto stats = index.retire(10, lookup, isRetired);
    assert(stats.visited == 7 && stats.released == 2 && stats.stale == 2);
    assert(!table[1].cbBuf && table[1].cbFrame == UINT64_MAX && !table[1].cbOff && !table[1].cbCap);
    assert(!table[4].cbBuf);
    assert(table[2].cbBuf == retired && table[3].cbBuf == retained && table[7].cbBuf == newer);
    assert(index.size() == 3);
    // Owners of kept chunks persist across frame-slot recycling until the
    // actual resource is retired, rather than disappearing with one frame list.
    stats = index.retire(11, lookup, isRetired);
    assert(stats.visited == 3 && stats.released == 1);
    stats = index.retire(12, lookup, [&](const auto &r) { return r == retained; });
    assert(stats.visited == 2 && stats.released == 1);
    index.forget(7); table.erase(7); assert(index.size() == 0);
    retired.reset(); retained.reset(); newer.reset();
    assert(alive == 0);
}

int main() {
    completion(); owners();
    std::puts("PASS: production completion levels, backlog-free waits, lost-wakeup races, and targeted snapshot ownership/lifetimes");
}
