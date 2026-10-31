#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mr {

// A byte vector whose copies share immutable storage until a caller asks for
// mutable access. Distinct wrappers may be read/written concurrently; callers
// must not concurrently mutate/copy the same wrapper. Pointers returned by
// data()/mutableData() are valid only until the next non-const operation on
// that wrapper (and must not outlive the wrapper). In particular, finish all
// writes through a mutableData() pointer before copying the wrapper: a raw
// pointer cannot enforce copy-on-write if it is used after that copy.
class CopyOnWriteBytes {
public:
	struct Statistics {
		uint64_t livePayloadCapacityBytes;
		uint64_t peakAccountedLivePayloadCapacityBytes;
		uint64_t cloneBytes;
		uint64_t payloadAllocations; // Payload owners created, not vector growth events.
		uint64_t cumulativePayloadCapacityAllocatedBytes;
	};

	CopyOnWriteBytes() = default;
	CopyOnWriteBytes(const CopyOnWriteBytes &) = default;
	CopyOnWriteBytes(CopyOnWriteBytes &&) noexcept = default;
	CopyOnWriteBytes &operator=(const CopyOnWriteBytes &) = default;
	CopyOnWriteBytes &operator=(CopyOnWriteBytes &&) noexcept = default;

	bool empty() const noexcept { return !payload_ || payload_->bytes.empty(); }
	size_t size() const noexcept { return payload_ ? payload_->bytes.size() : 0; }
	const uint8_t *data() const noexcept { return payload_ ? payload_->bytes.data() : nullptr; }

	void resize(size_t size) {
		if (!size) { payload_.reset(); return; }
		if (payload_ && payload_->bytes.size() == size) return;
		ensureUnique();
		payload_->resize(size);
	}

	uint8_t *mutableData() {
		ensureUnique();
		return payload_ ? payload_->bytes.data() : nullptr;
	}

	static Statistics statistics() noexcept {
		return {liveBytes_.load(std::memory_order_relaxed), peakBytes_.load(std::memory_order_relaxed),
			cloneBytes_.load(std::memory_order_relaxed), allocations_.load(std::memory_order_relaxed),
			allocatedBytes_.load(std::memory_order_relaxed)};
	}

private:
	struct Payload {
		Payload() { allocations_.fetch_add(1, std::memory_order_relaxed); }
		~Payload() { liveBytes_.fetch_sub(bytes.capacity(), std::memory_order_relaxed); }
		void resize(size_t size) {
			const size_t before = bytes.capacity();
			bytes.resize(size);
			accountCapacity(before, bytes.capacity());
		}
		std::vector<uint8_t> bytes;
	};

	static void accountCapacity(size_t before, size_t after) noexcept {
		if (before == after) return;
		uint64_t current;
		if (after > before) {
			const uint64_t delta = after - before;
			// A capacity change means vector obtained a new backing allocation;
			// count its full capacity, not merely net growth. This is cumulative
			// allocator payload requested, not resident or transient peak memory.
			allocatedBytes_.fetch_add(after, std::memory_order_relaxed);
			current = liveBytes_.fetch_add(delta, std::memory_order_relaxed) + delta;
			uint64_t peak = peakBytes_.load(std::memory_order_relaxed);
			while (peak < current && !peakBytes_.compare_exchange_weak(peak, current,
				std::memory_order_relaxed, std::memory_order_relaxed)) {}
		} else {
			liveBytes_.fetch_sub(before - after, std::memory_order_relaxed);
		}
	}

	void ensureUnique() {
		if (payload_ && payload_.unique()) return;
		const size_t oldSize = payload_ ? payload_->bytes.size() : 0;
		// Co-allocate the small payload owner and shared control block.
		auto replacement = std::make_shared<Payload>();
		if (oldSize) {
			replacement->resize(oldSize);
			std::copy(payload_->bytes.begin(), payload_->bytes.end(), replacement->bytes.begin());
			cloneBytes_.fetch_add(oldSize, std::memory_order_relaxed);
		}
		payload_ = std::move(replacement);
	}

	std::shared_ptr<Payload> payload_;
	inline static std::atomic<uint64_t> liveBytes_{0};
	inline static std::atomic<uint64_t> peakBytes_{0};
	inline static std::atomic<uint64_t> cloneBytes_{0};
	inline static std::atomic<uint64_t> allocations_{0};
	inline static std::atomic<uint64_t> allocatedBytes_{0};
};

} // namespace mr
