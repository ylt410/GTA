#include "../Source/MetalRenderer/MRLayoutIdentity.h"

#include <cassert>
#include <cstdio>

using mr::LayoutIdentityInterner;

static uint64_t forcedCollision(const LayoutIdentityInterner::Description &) { return 7; }

static LayoutIdentityInterner::Description oneElement() {
	return {{3, 1, 28, 2, 12, 0, 1}};
}

int main() {
	LayoutIdentityInterner intern;
	const auto original = oneElement();
	const uint64_t first = intern.intern(original);
	assert(first != 0);

	// Distinct engine handles carrying the same exact ordered description map
	// to one identity, avoiding redundant PSO cache entries across streaming.
	mr::LayoutIdentityCache objectCache;
	const uint64_t objectA = objectCache.identityForObject(100, original);
	const uint64_t objectB = objectCache.identityForObject(101, oneElement());
	assert(objectA == objectB && objectCache.identityForObject(100) == objectA);
	for (uint32_t syntheticObjectId = 102; syntheticObjectId < 1100; ++syntheticObjectId)
		assert(objectCache.identityForObject(syntheticObjectId, oneElement()) == objectA);
	objectCache.forgetObject(100);
	assert(objectCache.identityForObject(100, oneElement()) == objectA); // Destroy/recreate same descriptor.
	objectCache.forgetObject(101);
	auto replacement = original;
	replacement[0].fmt++;
	const uint64_t replacementIdentity = objectCache.identityForObject(101, replacement);
	assert(replacementIdentity != objectB); // Reused handle with changed descriptor.
	assert(intern.intern({}) != first);
	auto reversed = original;
	reversed.push_back({8, 0, 6, 1, 0, 1, 2});
	const uint64_t ordered = intern.intern(reversed);
	std::swap(reversed[0], reversed[1]);
	assert(intern.intern(reversed) != ordered);

	// Every field participates in exact equality, including semantic/index,
	// format, slot, byte offset, input class, and instance rate.
	for (unsigned field = 0; field < 7; ++field) {
		auto changed = original;
		uint32_t *fields[] = {&changed[0].sem, &changed[0].idx, &changed[0].fmt,
			&changed[0].slot, &changed[0].offset, &changed[0].cls, &changed[0].rate};
		++*fields[field];
		assert(intern.intern(changed) != first);
	}

	// Simulate source object destruction/recreation: the interner keeps a value
	// copy, not a pointer or ownership of the original descriptor storage.
	uint64_t recreated = 0;
	{
		auto temporary = original;
		recreated = intern.intern(temporary);
		temporary.clear();
	}
	assert(recreated == first);
	assert(intern.intern(original) == first);

	// Hash equality is only a bucket lookup. A forced collision must not merge
	// distinct layouts into the same identity.
	LayoutIdentityInterner colliding(&forcedCollision);
	const uint64_t collisionA = colliding.intern(original);
	auto different = original;
	different[0].fmt++;
	const uint64_t collisionB = colliding.intern(different);
	assert(collisionA != collisionB);
	assert(colliding.intern(original) == collisionA);

	std::puts("PASS: 1,000 streaming object IDs share one exact layout identity; recreation and collision-safe (not a gameplay FPS measurement)");
}
