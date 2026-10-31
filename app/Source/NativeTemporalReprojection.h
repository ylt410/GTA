#pragma once

// Portable CPU reference math for camera-only temporal reprojection.
// Matrices are 16 row-major coefficients and vectors multiply on the left:
//     clip = position * viewProjection
// No renderer, platform, or GPU types are required.

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace temporal_reprojection {

enum class MatrixConvention { Unknown, RowMajorRowVector, ColumnMajorColumnVector };

struct Mat4 { std::array<double, 16> m{}; };
struct Vec4 { double x{}, y{}, z{}, w{}; };
struct Viewport { int width{}, height{}; };

inline bool matchingViewport(Viewport depth, Viewport render) {
    return depth.width > 0 && depth.height > 0 && render.width > 0 && render.height > 0 &&
           depth.width == render.width && depth.height == render.height;
}

struct Transform {
    Mat4 currentClipToPreviousClip;
    std::array<float, 16> gpuCurrentClipToPreviousClip{};
    Viewport viewport;
    double epsilon{1e-9};
    double maxMotionPixels{1.0e7};
};

struct MotionResult {
    bool valid{false};
    double xPixels{};
    double yPixels{};
    double previousU{};
    double previousV{};
};

inline bool finite(const Mat4& a) {
    for (double v : a.m) if (!std::isfinite(v)) return false;
    return true;
}
inline bool finite(Vec4 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}
inline Vec4 multiply(Vec4 v, const Mat4& a) {
    const double q[4]{v.x, v.y, v.z, v.w};
    double r[4]{};
    for (int c = 0; c < 4; ++c)
        for (int k = 0; k < 4; ++k) r[c] += q[k] * a.m[k * 4 + c];
    return {r[0], r[1], r[2], r[3]};
}
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k) r.m[row * 4 + col] += a.m[row * 4 + k] * b.m[k * 4 + col];
    return r;
}

// Gauss-Jordan with partial pivoting. Rejects singular/ill-conditioned pivots
// rather than returning a plausible but unstable inverse.
inline bool inverse(const Mat4& input, Mat4& output, double epsilon = 1e-12) {
    if (!finite(input) || !std::isfinite(epsilon) || epsilon <= 0.0) return false;
    double a[4][8]{};
    double scale = 0.0;
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) {
        a[r][c] = input.m[r * 4 + c]; scale = std::fmax(scale, std::fabs(a[r][c]));
    }
    if (scale == 0.0) return false;
    for (int r = 0; r < 4; ++r) a[r][r + 4] = 1.0;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) <= epsilon * scale) return false;
        if (pivot != c) for (int j = 0; j < 8; ++j) { double t = a[c][j]; a[c][j] = a[pivot][j]; a[pivot][j] = t; }
        const double d = a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) {
            const double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    Mat4 result{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) result.m[r * 4 + c] = a[r][c + 4];
    if (!finite(result)) return false;
    output = result;
    return true;
}

// Checks the supported row-vector, Metal [0,1] reverse-Z perspective layout
// while allowing device-specific focal scales. Near/far depth must map to 1/0;
// 2e-6 depth tolerance accommodates float-rounded camera coefficients.
inline bool isKnownReverseZProjection(const Mat4& p, double nearPlane = 0.05,
                                     double farPlane = 10000.0, double tolerance = 2e-5) {
    if (!finite(p) || !std::isfinite(nearPlane) || !std::isfinite(farPlane) ||
        !std::isfinite(tolerance) || !(nearPlane > 0.0) || !(farPlane > nearPlane) || !(tolerance > 0.0)) return false;
    // Perspective form: [sx 0 0 0; 0 sy 0 0; 0 0 A -1; 0 0 B 0].
    // Nonzero skew/jitter/off-axis terms are deliberately not accepted.
    const int allowedNonzero[]{0,5,10,11,14};
    bool allowed[16]{};
    for (int i : allowedNonzero) allowed[i] = true;
    const double structuralTolerance = std::fmin(2e-8, tolerance * 1e-3);
    for (int i = 0; i < 16; ++i) if (!allowed[i] && std::fabs(p.m[i]) > structuralTolerance) return false;
    if (!(p.m[0] > 0.0) || !(p.m[5] > 0.0) || !(p.m[10] >= 0.0) ||
        std::fabs(p.m[11] + 1.0) > structuralTolerance || std::fabs(p.m[15]) > structuralTolerance ||
        !(p.m[14] > 0.0)) return false;
    // Perspective depth must decrease from approximately 1 at near to 0 at far.
    const double zn = (p.m[14] - nearPlane * p.m[10]) / nearPlane;
    const double zf = (p.m[14] - farPlane * p.m[10]) / farPlane;
    constexpr double endpointTolerance = 2e-6;
    return std::isfinite(zn) && std::isfinite(zf) && std::fabs(zn - 1.0) <= endpointTolerance &&
           std::fabs(zf) <= endpointTolerance && zn > zf;
}

inline bool sameProjection(const Mat4& a, const Mat4& b) {
    if (!finite(a) || !finite(b)) return false;
    for (int i = 0; i < 16; ++i) if (a.m[i] != b.m[i]) return false;
    return true;
}

// Camera view matrix is row-vector affine, with translation in coefficients 12..14
// and an orthonormal 3x3 rotation. This catches accidental projection/view swaps.
inline bool isRigidView(const Mat4& v, double tolerance = 2e-4) {
    if (!finite(v) || !std::isfinite(tolerance) || tolerance <= 0.0) return false;
    if (std::fabs(v.m[3]) > tolerance || std::fabs(v.m[7]) > tolerance ||
        std::fabs(v.m[11]) > tolerance || std::fabs(v.m[15] - 1.0) > tolerance) return false;
    for (int r = 0; r < 3; ++r) for (int s = 0; s < 3; ++s) {
        double dot = 0.0;
        for (int c = 0; c < 3; ++c) dot += v.m[r * 4 + c] * v.m[s * 4 + c];
        if (std::fabs(dot - (r == s ? 1.0 : 0.0)) > tolerance) return false;
    }
    const double det = v.m[0] * (v.m[5]*v.m[10] - v.m[6]*v.m[9]) -
                       v.m[1] * (v.m[4]*v.m[10] - v.m[6]*v.m[8]) +
                       v.m[2] * (v.m[4]*v.m[9] - v.m[5]*v.m[8]);
    return std::isfinite(det) && std::fabs(det - 1.0) <= 3.0 * tolerance;
}

// Build clipCurrent -> clipPrevious, since row-vector clipCurrent * inverse(CVP) * PVP.
inline bool buildTransform(const Mat4& currentViewProjection,
                           const Mat4& previousViewProjection,
                           MatrixConvention convention, Viewport viewport,
                           Transform& output, double epsilon = 1e-9,
                           double maxMotionPixels = 1.0e7) {
    if (convention != MatrixConvention::RowMajorRowVector || viewport.width <= 0 || viewport.height <= 0 ||
        !finite(currentViewProjection) || !finite(previousViewProjection) ||
        !std::isfinite(epsilon) || epsilon <= 0.0 || !std::isfinite(maxMotionPixels) || maxMotionPixels <= 0.0)
        return false;
    Mat4 invCurrent{};
    if (!inverse(currentViewProjection, invCurrent)) return false;
    const Mat4 transform = multiply(invCurrent, previousViewProjection);
    if (!finite(transform)) return false;
    Transform candidate{};
    candidate.currentClipToPreviousClip = transform;
    for (std::size_t i = 0; i < 16; ++i) {
        candidate.gpuCurrentClipToPreviousClip[i] = static_cast<float>(transform.m[i]);
        if (!std::isfinite(candidate.gpuCurrentClipToPreviousClip[i])) return false;
    }
    candidate.viewport = viewport;
    candidate.epsilon = epsilon;
    candidate.maxMotionPixels = maxMotionPixels;
    output = candidate;
    return true;
}

// Strict camera-facing entry point: both frames must use the recognized projection
// and rigid view convention before the generic clip transform can be constructed.
inline bool buildGameCameraTransform(const Mat4& currentView, const Mat4& currentProjection,
                                    const Mat4& previousView, const Mat4& previousProjection,
                                    MatrixConvention convention, Viewport viewport, Transform& output,
                                    double nearPlane = 0.05, double farPlane = 10000.0) {
    if (convention != MatrixConvention::RowMajorRowVector ||
        !isRigidView(currentView) || !isRigidView(previousView) ||
        !isKnownReverseZProjection(currentProjection, nearPlane, farPlane) ||
        !isKnownReverseZProjection(previousProjection, nearPlane, farPlane) ||
        !sameProjection(currentProjection, previousProjection)) return false;
    return buildTransform(multiply(currentView, currentProjection),
                          multiply(previousView, previousProjection), convention, viewport, output);
}

// Verify a raw shader WVP against the explicit CPU row-vector chain. The
// normalized residual is max element error divided by signal scale plus a
// bounded float-arithmetic uncertainty term. Cancellation-dominated cells are
// rejected when float uncertainty is itself too large to distinguish a mismatch.
// This is a consistency check, not proof of intended frame/camera semantics.
inline bool verifyWorldViewProjection(const Mat4& worldRows, const Mat4& view,
                                      const Mat4& projection, const Mat4& observedWVP,
                                      MatrixConvention convention, double& normalizedResidual,
                                      double relativeTolerance = 5e-5,
                                      double nearPlane = 0.05, double farPlane = 10000.0) {
    normalizedResidual = 1.0; // finite failure sentinel; greater than max accepted tolerance
    if (convention != MatrixConvention::RowMajorRowVector || !finite(worldRows) ||
        !isRigidView(view) || !isKnownReverseZProjection(projection, nearPlane, farPlane) ||
        !finite(observedWVP) || !std::isfinite(relativeTolerance) ||
        relativeTolerance <= 0.0 || relativeTolerance > 1e-2) return false;
    constexpr double affineTolerance = 2e-5;
    if (std::fabs(worldRows.m[3]) > affineTolerance || std::fabs(worldRows.m[7]) > affineTolerance ||
        std::fabs(worldRows.m[11]) > affineTolerance || std::fabs(worldRows.m[15] - 1.0) > affineTolerance)
        return false;
    const Mat4 worldView = multiply(worldRows, view);
    const Mat4 expected = multiply(worldView, projection);
    if (!finite(worldView) || !finite(expected)) return false;
    double maxResidual = 0.0;
    constexpr double floatRoundoffFactor = 16.0 * std::numeric_limits<float>::epsilon();
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) {
        double absoluteProductSum = 0.0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j)
            absoluteProductSum += std::fabs(worldRows.m[row*4+i] * view.m[i*4+j] * projection.m[j*4+col]);
        const double a = expected.m[row*4+col], b = observedWVP.m[row*4+col];
        const double signalScale = std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
        const double roundoff = floatRoundoffFactor * absoluteProductSum;
        if (roundoff > 4.0 * relativeTolerance * signalScale) return false;
        const double uncertaintyScale = std::fmax(signalScale, roundoff / relativeTolerance);
        if (!std::isfinite(absoluteProductSum) || !std::isfinite(uncertaintyScale)) return false;
        maxResidual = std::fmax(maxResidual, std::fabs(a-b) / uncertaintyScale);
    }
    if (!std::isfinite(maxResidual)) return false;
    normalizedResidual = maxResidual;
    return maxResidual <= relativeTolerance;
}

// uv uses top-left origin; z is Metal/device depth [0,1]. The caller must supply
// a depth-valid mask because clear/background depth is not a world surface.
inline MotionResult reproject(const Transform& transform, double u, double v,
                              double currentDepth, bool depthValid) {
    MotionResult result{};
    if (!depthValid || transform.viewport.width <= 0 || transform.viewport.height <= 0 ||
        !std::isfinite(transform.epsilon) || transform.epsilon <= 0.0 ||
        !std::isfinite(transform.maxMotionPixels) || transform.maxMotionPixels <= 0.0 ||
        !finite(transform.currentClipToPreviousClip) || !std::isfinite(u) || !std::isfinite(v) ||
        !std::isfinite(currentDepth) || u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0 ||
        currentDepth < 0.0 || currentDepth > 1.0) return result;
    const Vec4 previous = multiply(Vec4{2.0 * u - 1.0, 1.0 - 2.0 * v, currentDepth, 1.0},
                                   transform.currentClipToPreviousClip);
    if (!finite(previous) || previous.w <= transform.epsilon) return result;
    const double x = previous.x / previous.w, y = previous.y / previous.w, z = previous.z / previous.w;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        x < -1.0 || x > 1.0 || y < -1.0 || y > 1.0 || z < 0.0 || z > 1.0) return result;
    const double pu = (x + 1.0) * 0.5, pv = (1.0 - y) * 0.5;
    const double dx = (pu - u) * transform.viewport.width;
    const double dy = (pv - v) * transform.viewport.height;
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::fabs(dx) > transform.maxMotionPixels ||
        std::fabs(dy) > transform.maxMotionPixels) return result;
    return {true, dx, dy, pu, pv};
}

} // namespace temporal_reprojection
