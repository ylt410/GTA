#include "../Source/MetalRenderer/MRRenderTargetCache.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

struct Color { uint32_t tex, mip, slice, fmt; };
struct Depth { uint32_t tex, mip, slice, fmt, flags; };
struct State {
	Color colors[8] = {};
	Depth depth = {};
	bool targetsDirty = false, pipelineDirty = false;
	void reference(const uint32_t *packet) {
		for (unsigned i = 0; i < 8; i++) {
			const uint32_t *p = packet + i * 4;
			colors[i] = {p[0], p[1], p[2], p[3]};
		}
		const uint32_t *p = packet + 32;
		depth = {p[0], p[1], p[2], p[3], p[4]};
		targetsDirty = pipelineDirty = true;
	}
};

static void assertSameBindings(const State &a, const State &b) {
	for (unsigned i = 0; i < 8; i++) {
		assert(a.colors[i].tex == b.colors[i].tex);
		assert(a.colors[i].mip == b.colors[i].mip);
		assert(a.colors[i].slice == b.colors[i].slice);
		assert(a.colors[i].fmt == b.colors[i].fmt);
	}
	assert(a.depth.tex == b.depth.tex && a.depth.mip == b.depth.mip);
	assert(a.depth.slice == b.depth.slice && a.depth.fmt == b.depth.fmt && a.depth.flags == b.depth.flags);
}

int main() {
	std::array<uint32_t, 37> packet{};
	State state;
	assert(mr::RenderTargetBindingsEqual(state.colors, state.depth, packet.data()));
	// Zero attachments and a clean state remain clean, including the first call.
	assert(!mr::SetRenderTargetBindings(state.colors, state.depth, packet.data(), state.targetsDirty, state.pipelineDirty));
	assert(!state.targetsDirty && !state.pipelineDirty);
	// Every dirty-state combination survives an unchanged binding. A prior
	// clear/destroy/encoder reset must still cause its required state update.
	for (unsigned dirty = 0; dirty < 4; dirty++) {
		state.targetsDirty = (dirty & 1) != 0;
		state.pipelineDirty = (dirty & 2) != 0;
		assert(!mr::SetRenderTargetBindings(state.colors, state.depth, packet.data(), state.targetsDirty, state.pipelineDirty));
		assert(state.targetsDirty == ((dirty & 1) != 0));
		assert(state.pipelineDirty == ((dirty & 2) != 0));
	}
	// All eight RTs and all five depth fields participate, including flags for
	// read-only depth/stencil and views of a different mip/slice/format.
	for (unsigned word = 0; word < packet.size(); word++) {
		State candidate;
		packet.fill(0);
		packet[word] = 0x80000000u | (word + 1);
		assert(!mr::RenderTargetBindingsEqual(candidate.colors, candidate.depth, packet.data()));
		assert(mr::SetRenderTargetBindings(candidate.colors, candidate.depth, packet.data(), candidate.targetsDirty, candidate.pipelineDirty));
		assert(candidate.targetsDirty && candidate.pipelineDirty);
		assert(mr::RenderTargetBindingsEqual(candidate.colors, candidate.depth, packet.data()));
		candidate.targetsDirty = candidate.pipelineDirty = false;
		assert(!mr::SetRenderTargetBindings(candidate.colors, candidate.depth, packet.data(), candidate.targetsDirty, candidate.pipelineDirty));
		assert(!candidate.targetsDirty && !candidate.pipelineDirty);
		packet[word] = 0;
		assert(mr::SetRenderTargetBindings(candidate.colors, candidate.depth, packet.data(), candidate.targetsDirty, candidate.pipelineDirty));
	}
	// Deterministic mixed traffic is compared with the production's original
	// unconditional binding assignment at every draw boundary.
	State reference, cached;
	uint32_t random = 0x91e10da5;
	unsigned changes = 0, hits = 0;
	packet.fill(0);
	for (unsigned draw = 0; draw < 100000; draw++) {
		random ^= random << 13; random ^= random >> 17; random ^= random << 5;
		if ((random & 7) == 0) packet[(random >> 3) % packet.size()] = random;
		if (draw % 127 == 0) cached.targetsDirty = true; // e.g. attachment destroyed
		if (draw % 193 == 0) cached.pipelineDirty = true; // other pipeline state changed
		bool wasTargetsDirty = cached.targetsDirty, wasPipelineDirty = cached.pipelineDirty;
		reference.reference(packet.data());
		bool changed = mr::SetRenderTargetBindings(cached.colors, cached.depth, packet.data(), cached.targetsDirty, cached.pipelineDirty);
		if (changed) { changes++; assert(cached.targetsDirty && cached.pipelineDirty); }
		else { hits++; assert(cached.targetsDirty == wasTargetsDirty && cached.pipelineDirty == wasPipelineDirty); }
		assertSameBindings(reference, cached);
		cached.targetsDirty = cached.pipelineDirty = false;
	}
	assert(changes > 0 && hits > changes);
	std::printf("PASS: all 37 binding fields, zero/unbind, retained dirty flags, and 100000 reference-equivalent draws (%u changes, %u repeats).\n", changes, hits);
}
