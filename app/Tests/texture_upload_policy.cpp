#include "../Source/MetalRenderer/MRTextureUploadPolicy.h"
#include <cassert>
#include <cstdint>
#include <cstdio>

int main() {
	using mr::canReplaceSharedTexture;
	assert(canReplaceSharedTexture(true, 0, 0));
	assert(canReplaceSharedTexture(true, 7, 7));
	assert(canReplaceSharedTexture(true, 6, 7));
	assert(!canReplaceSharedTexture(true, 8, 7));
	assert(!canReplaceSharedTexture(false, 0, UINT64_MAX));
	assert(!canReplaceSharedTexture(false, 3, 3));
	std::puts("PASS: production upload policy replaces only shared textures whose last-use serial has completed; private and in-flight textures require staging.");
	return 0;
}
