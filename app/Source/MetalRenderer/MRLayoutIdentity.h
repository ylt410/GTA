// CPU-only canonical identity for D3D-style vertex input-layout descriptors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mr {

struct LayoutElementIdentity {
	uint32_t sem, idx, fmt, slot, offset, cls, rate;

	bool operator==(const LayoutElementIdentity &other) const {
		return sem == other.sem && idx == other.idx && fmt == other.fmt && slot == other.slot &&
			offset == other.offset && cls == other.cls && rate == other.rate;
	}
};

class LayoutIdentityInterner {
public:
	using Description = std::vector<LayoutElementIdentity>;
	using HashFunction = uint64_t (*)(const Description &);

	explicit LayoutIdentityInterner(HashFunction hash = &hashDescription) : hash_(hash ? hash : &hashDescription) {}

	// IDs are stable for the lifetime of the interner. Each hash bucket retains
	// copied descriptors and resolves collisions with exact ordered equality.
	uint64_t intern(const Description &description) {
		const uint64_t hash = hash_(description);
		auto &bucket = buckets_[hash];
		for (const Entry &entry : bucket) if (entry.description == description) return entry.id;
		if (nextId_ == 0) return 0; // Exhaustion must fail closed rather than recycle an identity.
		const uint64_t id = nextId_++;
		bucket.push_back(Entry{id, description});
		return id;
	}

	static uint64_t hashDescription(const Description &description) {
		uint64_t hash = UINT64_C(1469598103934665603);
		auto add = [&hash](uint32_t value) {
			hash ^= value;
			hash *= UINT64_C(1099511628211);
		};
		add(static_cast<uint32_t>(description.size()));
		for (const auto &element : description) {
			add(element.sem); add(element.idx); add(element.fmt); add(element.slot);
			add(element.offset); add(element.cls); add(element.rate);
		}
		return hash;
	}

private:
	struct Entry { uint64_t id; Description description; };
	HashFunction hash_;
	uint64_t nextId_ = 1;
	std::unordered_map<uint64_t, std::vector<Entry>> buckets_;
};

// Object IDs are renderer handles, not cache identity. This thin layer avoids
// re-hashing a layout on every draw while allowing handle destruction/reuse.
class LayoutIdentityCache {
public:
	explicit LayoutIdentityCache(LayoutIdentityInterner::HashFunction hash = &LayoutIdentityInterner::hashDescription)
		: interner_(hash) {}

	uint64_t identityForObject(uint32_t objectId) const {
		auto found = objectIds_.find(objectId);
		return found == objectIds_.end() ? 0 : found->second;
	}

	uint64_t identityForObject(uint32_t objectId, const LayoutIdentityInterner::Description &description) {
		auto found = objectIds_.find(objectId);
		if (found != objectIds_.end()) return found->second;
		const uint64_t identity = interner_.intern(description);
		objectIds_.emplace(objectId, identity);
		return identity;
	}

	void forgetObject(uint32_t objectId) { objectIds_.erase(objectId); }

private:
	LayoutIdentityInterner interner_;
	std::unordered_map<uint32_t, uint64_t> objectIds_;
};

} // namespace mr
