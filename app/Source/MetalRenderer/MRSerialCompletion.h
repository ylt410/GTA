#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>

namespace mr {
// Completion is a level, not one semaphore token per command buffer. The
// predicate and notification use the same mutex, so no wake can be lost.
class SerialCompletion {
    std::atomic<std::uint64_t> completed_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::multiset<std::uint64_t> targets_;
    std::set<std::uint64_t> outOfOrder_;
public:
    struct Stats { std::uint64_t blocks = 0, wakeups = 0, notifications = 0; };
private:
    Stats stats_;
public:
    std::uint64_t completed() const { return completed_.load(); }
    void publish(std::uint64_t serial) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto before = completed_.load(), contiguous = before;
        if (serial <= before) return;
        if (serial != before + 1) { outOfOrder_.insert(serial); return; }
        contiguous = serial;
        while (!outOfOrder_.empty() && *outOfOrder_.begin() == contiguous + 1) {
            contiguous = *outOfOrder_.begin(); outOfOrder_.erase(outOfOrder_.begin());
        }
        // A later GPU completion does not imply an earlier host callback has
        // finished reading visibility/readback buffers. Retire only a fully
        // completed prefix, including those callback finalizers.
        completed_.store(contiguous);
        if (!targets_.empty() && *targets_.begin() <= contiguous) { ++stats_.notifications; changed_.notify_all(); }
    }
    void wait(std::uint64_t serial) {
        if (completed() >= serial) return;
        std::unique_lock<std::mutex> lock(mutex_);
        if (completed() >= serial) return;
        auto target = targets_.insert(serial); ++stats_.blocks;
        while (completed() < serial) { changed_.wait(lock); ++stats_.wakeups; }
        targets_.erase(target);
    }
    Stats takeStats() {
        std::lock_guard<std::mutex> lock(mutex_);
        Stats result = stats_; stats_ = {}; return result;
    }
};
} // namespace mr
