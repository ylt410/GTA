#include "../Source/NativeTemporalReprojection.h"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace temporal_reprojection;
static bool near(double a, double b, double e = 1e-8) { return std::fabs(a-b) <= e; }
static Mat4 identity() { Mat4 a{}; a.m = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; return a; }
static Mat4 gameProjection() {
    Mat4 p{}; p.m = {0.9897723794,0,0,0, 0,2.1445069313,0,0,
                      0,0,5.006790161e-6,-1, 0,0,0.05000025034,0}; return p;
}
static Mat4 dynamicProjection(double sx, double sy, double n, double f) {
    Mat4 p{}; const double a=n/(f-n), b=n*f/(f-n);
    p.m={sx,0,0,0, 0,sy,0,0, 0,0,a,-1, 0,0,b,0}; return p;
}
struct ProjectedPoint { double u, v, depth; };
static ProjectedPoint projectWorld(Vec4 world, const Mat4& view, const Mat4& projection) {
    const Vec4 clip = multiply(multiply(world, view), projection);
    assert(clip.w > 0.0);
    return {(clip.x/clip.w+1.0)*.5, (1.0-clip.y/clip.w)*.5, clip.z/clip.w};
}
static void assertSameMotion(const MotionResult& got, ProjectedPoint current,
                             ProjectedPoint previous, Viewport viewport) {
    assert(got.valid);
    assert(near(got.xPixels,(previous.u-current.u)*viewport.width,1e-7));
    assert(near(got.yPixels,(previous.v-current.v)*viewport.height,1e-7));
}

int main() {
    Transform t{};
    assert(buildTransform(identity(), identity(), MatrixConvention::RowMajorRowVector, {1920,1080}, t));
    auto m = reproject(t, .31, .72, .4, true);
    assert(m.valid && near(m.xPixels, 0) && near(m.yPixels, 0));
    assert(t.gpuCurrentClipToPreviousClip[0] == 1.f);

    // Known previous-camera translation in clip space: +0.2 NDC x = +10% width.
    Mat4 translated = identity(); translated.m[12] = .2;
    assert(buildTransform(identity(), translated, MatrixConvention::RowMajorRowVector, {1000,500}, t));
    m = reproject(t, .5, .5, .5, true);
    assert(m.valid && near(m.xPixels, 100) && near(m.yPixels, 0));
    // A 90-degree clip-space camera rotation: (x,y)->(y,-x); test top-left Y sign.
    Mat4 rotated = identity(); rotated.m[0]=0; rotated.m[1]=-1; rotated.m[4]=1; rotated.m[5]=0;
    assert(buildTransform(identity(), rotated, MatrixConvention::RowMajorRowVector, {200,100}, t));
    m = reproject(t, .75, .5, .5, true);
    assert(m.valid && near(m.xPixels, -50) && near(m.yPixels, 25));

    // Row-vector translation is at 12..14; a column-vector interpretation is rejected.
    Mat4 view = identity(); view.m[12]=3; view.m[13]=-2; view.m[14]=5;
    assert(isRigidView(view));
    Mat4 transposedTrap = identity(); transposedTrap.m[3]=3;
    assert(!isRigidView(transposedTrap));
    Mat4 reflection=identity(); reflection.m[0]=-1;
    assert(!isRigidView(reflection));
    assert(!buildTransform(identity(), identity(), MatrixConvention::Unknown, {10,10}, t));
    assert(!buildTransform(identity(), identity(), MatrixConvention::ColumnMajorColumnVector, {10,10}, t));

    // Asymmetric matrix inverse/multiply roundtrip catches accidental transpose.
    Mat4 a{}; a.m={2,.1,0,0, 0,3,.2,0, .1,0,4,0, 5,-7,9,1};
    Mat4 inv{}; assert(inverse(a, inv));
    Vec4 p{.4,-.2,.7,1}; Vec4 q=multiply(multiply(p,a),inv);
    assert(near(q.x,p.x) && near(q.y,p.y) && near(q.z,p.z) && near(q.w,p.w));
    Mat4 singular{}; assert(!inverse(singular, inv));
    Mat4 illConditioned=identity(); illConditioned.m[15]=1e-14;
    assert(!inverse(illConditioned,inv));
    a.m[0]=std::numeric_limits<double>::infinity(); assert(!inverse(a, inv));

    const Mat4 proj=gameProjection();
    assert(isKnownReverseZProjection(proj));
    // Device aspect/FOV focal scales and dynamic near/far remain supported.
    const Mat4 wide=dynamicProjection(.62,1.74,.1,5000.0);
    const Mat4 narrow=dynamicProjection(1.91,3.2,.02,25000.0);
    assert(isKnownReverseZProjection(wide,.1,5000.0));
    assert(isKnownReverseZProjection(narrow,.02,25000.0));
    assert(buildGameCameraTransform(identity(),wide,identity(),wide,
                                   MatrixConvention::RowMajorRowVector,{2560,1080},t,.1,5000.0));
    assert(buildGameCameraTransform(identity(),narrow,identity(),narrow,
                                   MatrixConvention::RowMajorRowVector,{1080,1920},t,.02,25000.0));
    // Camera looks along -Z: device depth = (offset - distance*P22)/distance.
    const auto depthAt = [&](double distance) { return (proj.m[14]-distance*proj.m[10])/distance; };
    assert(near(depthAt(.05), 1.0, 2e-5));
    assert(depthAt(.05) > depthAt(10000.0));
    // Given coefficients are rounded/engine-specific; nominal 10 km lands very
    // slightly below zero, while remaining monotonic reverse-Z at the far end.
    assert(depthAt(10000.0) < 0.0 && depthAt(10000.0) > -1e-7);
    Mat4 wrongNearFar=proj; wrongNearFar.m[10]=0.0; wrongNearFar.m[14]=.025;
    assert(!isKnownReverseZProjection(wrongNearFar,.05,10000.0));
    Mat4 wrongProjection=proj; wrongProjection.m[5]=0.0;
    assert(!isKnownReverseZProjection(wrongProjection));
    Mat4 wrongDepth=proj; wrongDepth.m[10]=-proj.m[10];
    assert(!isKnownReverseZProjection(wrongDepth));
    Mat4 asymmetric=wide; asymmetric.m[1]=.01;
    assert(!isKnownReverseZProjection(asymmetric,.1,5000.0));
    Mat4 nonfiniteProjection=wide; nonfiniteProjection.m[0]=std::numeric_limits<double>::quiet_NaN();
    assert(!isKnownReverseZProjection(nonfiniteProjection,.1,5000.0));
    assert(!isKnownReverseZProjection(wide,.1,std::numeric_limits<double>::infinity()));
    assert(!isKnownReverseZProjection(wide,.1,5000.0,std::numeric_limits<double>::infinity()));
    assert(!buildGameCameraTransform(identity(), proj, identity(), wrongProjection,
                                     MatrixConvention::RowMajorRowVector, {1920,1080}, t));
    Mat4 changedFov=wide; changedFov.m[0]+=.01;
    assert(isKnownReverseZProjection(changedFov,.1,5000.0));
    assert(!buildGameCameraTransform(identity(),wide,identity(),changedFov,
                                    MatrixConvention::RowMajorRowVector,{2560,1080},t,.1,5000.0));
    assert(buildGameCameraTransform(identity(), proj, identity(), proj,
                                    MatrixConvention::RowMajorRowVector, {1920,1080}, t));

    // Shader raw WVP verification: world may contain scale/shear but must be
    // affine. Compare a known non-symmetric chain and its float-rounded result.
    Mat4 worldRows=identity();
    worldRows.m[0]=1.2; worldRows.m[1]=.13; worldRows.m[5]=.8; worldRows.m[10]=1.5;
    worldRows.m[12]=4.25; worldRows.m[13]=-3.5; worldRows.m[14]=2.75;
    Mat4 cameraView=identity(); cameraView.m[12]=-.4; cameraView.m[13]=.2;
    const Mat4 expectedWVP=multiply(multiply(worldRows,cameraView),proj);
    double residual=1.0;
    assert(verifyWorldViewProjection(worldRows,cameraView,proj,expectedWVP,
            MatrixConvention::RowMajorRowVector,residual));
    assert(std::isfinite(residual) && residual<=5e-5);
    Mat4 floatRoundedWVP{};
    for(int i=0;i<16;++i) floatRoundedWVP.m[i]=static_cast<float>(expectedWVP.m[i]);
    assert(verifyWorldViewProjection(worldRows,cameraView,proj,floatRoundedWVP,
            MatrixConvention::RowMajorRowVector,residual));
    assert(std::isfinite(residual));
    Mat4 wrongCamera=cameraView; wrongCamera.m[12]+=.25;
    const Mat4 wrongCameraWVP=multiply(multiply(worldRows,wrongCamera),proj);
    assert(!verifyWorldViewProjection(worldRows,cameraView,proj,wrongCameraWVP,
            MatrixConvention::RowMajorRowVector,residual));
    assert(std::isfinite(residual) && residual>5e-5);
    Mat4 zeroWVP{};
    assert(!verifyWorldViewProjection(worldRows,cameraView,proj,zeroWVP,
            MatrixConvention::RowMajorRowVector,residual));
    assert(std::isfinite(residual));
    assert(!verifyWorldViewProjection(worldRows,cameraView,proj,expectedWVP,
            MatrixConvention::Unknown,residual));
    Mat4 transposedWVP{};
    for(int r=0;r<4;++r) for(int c=0;c<4;++c) transposedWVP.m[r*4+c]=expectedWVP.m[c*4+r];
    assert(!verifyWorldViewProjection(worldRows,cameraView,proj,transposedWVP,
            MatrixConvention::RowMajorRowVector,residual));
    Mat4 nonfiniteWVP=expectedWVP; nonfiniteWVP.m[2]=std::numeric_limits<double>::infinity();
    assert(!verifyWorldViewProjection(worldRows,cameraView,proj,nonfiniteWVP,
            MatrixConvention::RowMajorRowVector,residual));

    // Large absolute coordinates that remain well-conditioned still match;
    // origin-cancellation beyond float's reliable comparison range is rejected.
    Mat4 largeWorld=identity(); largeWorld.m[12]=1.0e6; largeWorld.m[13]=-2.0e6;
    const Mat4 largeWVP=multiply(multiply(largeWorld,identity()),proj);
    Mat4 largeFloatWVP{};
    for(int i=0;i<16;++i) largeFloatWVP.m[i]=static_cast<float>(largeWVP.m[i]);
    assert(verifyWorldViewProjection(largeWorld,identity(),proj,largeFloatWVP,
            MatrixConvention::RowMajorRowVector,residual));
    Mat4 cancelWorld=identity(); cancelWorld.m[12]=1.0e7;
    Mat4 cancelView=identity(); cancelView.m[12]=-1.0e7;
    const Mat4 cancelWVP=multiply(multiply(cancelWorld,cancelView),proj);
    assert(!verifyWorldViewProjection(cancelWorld,cancelView,proj,cancelWVP,
            MatrixConvention::RowMajorRowVector,residual));

    // Independent world-space oracle: project one fixed world point through each
    // camera separately, then compare with current-depth reprojection.
    const Viewport oracleViewport{1280,720};
    const Vec4 worldPoint{.2,.1,-5.0,1.0};
    const Mat4 currentView=identity();
    Mat4 previousView=identity(); previousView.m[12]=.25;
    assert(buildGameCameraTransform(currentView,proj,previousView,proj,
            MatrixConvention::RowMajorRowVector,oracleViewport,t));
    const auto currentProjected=projectWorld(worldPoint,currentView,proj);
    const auto previousProjected=projectWorld(worldPoint,previousView,proj);
    assertSameMotion(reproject(t,currentProjected.u,currentProjected.v,currentProjected.depth,true),
                     currentProjected,previousProjected,oracleViewport);

    const double angle=.04, c=std::cos(angle), s=std::sin(angle);
    Mat4 rotatedCamera=identity();
    rotatedCamera.m[0]=c; rotatedCamera.m[2]=-s;
    rotatedCamera.m[8]=s; rotatedCamera.m[10]=c;
    assert(isRigidView(rotatedCamera));
    assert(buildGameCameraTransform(currentView,proj,rotatedCamera,proj,
            MatrixConvention::RowMajorRowVector,oracleViewport,t));
    const auto rotatedProjected=projectWorld(worldPoint,rotatedCamera,proj);
    const auto rotatedMotion=reproject(t,currentProjected.u,currentProjected.v,currentProjected.depth,true);
    assertSameMotion(rotatedMotion,currentProjected,rotatedProjected,oracleViewport);
    assert(std::fabs(rotatedMotion.xPixels)>1.0);
    Mat4 pitchedCamera=identity();
    pitchedCamera.m[5]=c; pitchedCamera.m[6]=s;
    pitchedCamera.m[9]=-s; pitchedCamera.m[10]=c;
    assert(isRigidView(pitchedCamera));
    assert(buildGameCameraTransform(currentView,proj,pitchedCamera,proj,
            MatrixConvention::RowMajorRowVector,oracleViewport,t));
    const auto pitchedProjected=projectWorld(worldPoint,pitchedCamera,proj);
    const auto pitchedMotion=reproject(t,currentProjected.u,currentProjected.v,currentProjected.depth,true);
    assertSameMotion(pitchedMotion,currentProjected,pitchedProjected,oracleViewport);
    assert(pitchedMotion.yPixels<0.0); // top-left texture Y reverses the positive NDC-Y shift

    // Exported float coefficients are exactly the float conversion of the CPU
    // matrix and reproduce its current-clip -> previous-clip transform.
    for (int i=0;i<16;++i) assert(t.gpuCurrentClipToPreviousClip[i] ==
                                   static_cast<float>(t.currentClipToPreviousClip.m[i]));
    const Vec4 clipCurrent{2*currentProjected.u-1,1-2*currentProjected.v,currentProjected.depth,1};
    double cpuClip[4]{}; float gpuClip[4]{};
    const double cv[4]{clipCurrent.x,clipCurrent.y,clipCurrent.z,clipCurrent.w};
    for (int col=0;col<4;++col) for(int row=0;row<4;++row) {
        cpuClip[col]+=cv[row]*t.currentClipToPreviousClip.m[row*4+col];
        gpuClip[col]+=static_cast<float>(cv[row])*t.gpuCurrentClipToPreviousClip[row*4+col];
    }
    for(int i=0;i<4;++i) assert(std::fabs(cpuClip[i]-gpuClip[i])<2e-6);

    // Invalid/background depth, nonfinite values, invalid dimensions, clipping and
    // excessive motion are rejected. Out-of-bounds is not a claim of disocclusion.
    assert(!reproject(t,.5,.5,.5,false).valid);
    assert(!reproject(t,.5,.5,-.1,true).valid);
    assert(!reproject(t,std::numeric_limits<double>::quiet_NaN(),.5,.5,true).valid);
    assert(!buildTransform(identity(),identity(),MatrixConvention::RowMajorRowVector,{0,4},t));
    assert(!matchingViewport({1280,720},{1920,1080}));
    assert(matchingViewport({1920,1080},{1920,1080}));
    Mat4 behind=identity(); behind.m[15]=-1;
    assert(buildTransform(identity(),behind,MatrixConvention::RowMajorRowVector,{100,100},t));
    assert(!reproject(t,.5,.5,.5,true).valid);
    Mat4 offscreen=identity(); offscreen.m[12]=4;
    assert(buildTransform(identity(),offscreen,MatrixConvention::RowMajorRowVector,{100,100},t));
    assert(!reproject(t,.5,.5,.5,true).valid);
    Mat4 tooFar=identity(); tooFar.m[12]=.5;
    assert(buildTransform(identity(),tooFar,MatrixConvention::RowMajorRowVector,{100,100},t,1e-9,10));
    assert(!reproject(t,.5,.5,.5,true).valid);

    // Viewport mismatch is represented by rejecting unequal source/render dimensions
    // at the caller boundary; this API only accepts one shared viewport.
    std::cout << "temporal reprojection checks passed\n";
}
