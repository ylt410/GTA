import Foundation

/// Spatial MetalFX input resolutions. These modes use one spatial image per frame; they do not
/// describe temporal reconstruction or frame generation.
enum SuperResolutionMode: Int, CaseIterable {
    case off = 0, quality = 1, balanced = 2, performance = 3

    /// Input pixel scale relative to the selected output resolution.
    var inputScale: Double {
        switch self {
        case .off: return 1
        case .quality: return 0.77
        case .balanced: return 0.67
        case .performance: return 0.50
        }
    }

    var isEnabled: Bool { self != .off }
}

struct SpatialResolution: Equatable {
    let renderWidth: Int
    let renderHeight: Int
    let outputWidth: Int
    let outputHeight: Int
}

/// Pure sizing policy shared by the app and the standalone sizing checks.
enum SpatialSuperResolution {
    static let minimumRenderAxis = 360

    static func dimensions(outputWidth: Int, outputHeight: Int, mode: SuperResolutionMode,
                           supported: Bool) -> SpatialResolution {
        guard supported, mode.isEnabled, outputWidth > 0, outputHeight > 0,
              min(outputWidth, outputHeight) >= minimumRenderAxis else {
            return SpatialResolution(renderWidth: outputWidth, renderHeight: outputHeight,
                                     outputWidth: outputWidth, outputHeight: outputHeight)
        }

        // Keep the same scale on both axes so ultrawide and TV aspect ratios remain intact.
        // Use the shorter axis to retain the minimum in either landscape or portrait orientation.
        let shortAxis = min(outputWidth, outputHeight)
        let scale = max(mode.inputScale, Double(minimumRenderAxis) / Double(shortAxis))
        func even(_ value: Double) -> Int { Int(value.rounded()) & ~1 }
        let width = even(Double(outputWidth) * scale)
        let height = even(Double(outputHeight) * scale)
        return SpatialResolution(renderWidth: width, renderHeight: height,
                                 outputWidth: outputWidth, outputHeight: outputHeight)
    }
}
