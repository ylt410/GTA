#include "MetalRenderer/MRSpatialUpscalePolicy.h"
#include <cassert>
#include <limits>
#include <cstdio>
int main() {
	using namespace mr;
	for (int mode = 1; mode <= 3; ++mode) {
		auto t = spatialUpscaleTarget(spatialUpscaleConfiguration(mode, 1560, 720), false, 1198, 554, 2556, 1179);
		assert(t.explicitTarget && t.width == 1560 && t.height == 720);
	}
	assert(!spatialUpscaleConfiguration(-1,1560,720));
	assert(!spatialUpscaleConfiguration(4,1560,720));
	assert(!spatialUpscaleConfiguration(1,0,720));
	assert(!spatialUpscaleConfiguration(1,9000,720));
	auto legacy = spatialUpscaleTarget(0,true,1560,720,2556,1179);
	assert(!legacy.explicitTarget && legacy.width == 2556 && legacy.height == 1179);
	assert(!spatialUpscaleTarget(0,false,1560,720,2556,1179).width);
	assert(!spatialUpscaleTarget(spatialUpscaleConfiguration(1,1560,720),false,1560,720,2556,1179).width);
	assert(!spatialUpscaleTarget(0,true,1560,720,std::numeric_limits<double>::quiet_NaN(),1179).width);
	assert(!spatialUpscaleTarget(0,true,1560,720,2556,-1).width);
	auto limited = spatialUpscaleTarget(0,true,400,360,8000,8000);
	assert(limited.width == 1200 && limited.height == 1080);
	assert(!spatialUpscaleTarget(0,true,0,720,2556,1179).width);
	std::puts("Spatial upscaling targets: explicit output, legacy mode, bounds, invalid dimensions, and no double upscale PASS");
}
