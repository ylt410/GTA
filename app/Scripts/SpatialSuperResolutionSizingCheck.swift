import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif

@main
struct SpatialSuperResolutionSizingCheck {
    static func check(_ condition: @autoclosure () -> Bool, _ message: String) {
        guard condition() else {
            fputs("FAIL: \(message)\n", stderr)
            exit(1)
        }
    }

    static func main() {
        let quality720 = SpatialSuperResolution.dimensions(outputWidth: 1280, outputHeight: 720,
                                                           mode: .quality, supported: true)
        check(quality720 == SpatialResolution(renderWidth: 986, renderHeight: 554,
                                              outputWidth: 1280, outputHeight: 720), "720p Quality scale")

        let balanced720 = SpatialSuperResolution.dimensions(outputWidth: 1280, outputHeight: 720,
                                                            mode: .balanced, supported: true)
        check(balanced720.renderWidth == 858 && balanced720.renderHeight == 482, "720p Balanced scale")

        let performance720 = SpatialSuperResolution.dimensions(outputWidth: 1280, outputHeight: 720,
                                                               mode: .performance, supported: true)
        check(performance720.renderWidth == 640 && performance720.renderHeight == 360, "720p Performance scale")

        let lowResolution = SpatialSuperResolution.dimensions(outputWidth: 936, outputHeight: 432,
                                                              mode: .performance, supported: true)
        check(lowResolution.renderWidth == 780 && lowResolution.renderHeight == 360,
              "360px floor lifts both axes uniformly")
        let lowAspect = Double(lowResolution.renderWidth) / Double(lowResolution.renderHeight)
        let outputAspect = Double(lowResolution.outputWidth) / Double(lowResolution.outputHeight)
        check(abs(lowAspect - outputAspect) < 0.002, "minimum render height preserves output aspect")

        let tv = SpatialSuperResolution.dimensions(outputWidth: 1920, outputHeight: 800,
                                                   mode: .quality, supported: true)
        let tvAspect = Double(tv.renderWidth) / Double(tv.renderHeight)
        check(abs(tvAspect - 2.4) < 0.004, "wide TV aspect preserved")

        let portrait = SpatialSuperResolution.dimensions(outputWidth: 432, outputHeight: 936,
                                                         mode: .performance, supported: true)
        check(portrait.renderWidth == 360 && portrait.renderHeight == 780,
              "portrait shorter-axis minimum scales both axes uniformly")
        let portraitAspect = Double(portrait.renderWidth) / Double(portrait.renderHeight)
        check(abs(portraitAspect - Double(portrait.outputWidth) / Double(portrait.outputHeight)) < 0.002,
              "portrait aspect preserved")

        let belowMinimum = SpatialSuperResolution.dimensions(outputWidth: 320, outputHeight: 640,
                                                            mode: .performance, supported: true)
        check(belowMinimum == SpatialResolution(renderWidth: 320, renderHeight: 640,
                                                outputWidth: 320, outputHeight: 640),
              "output below the minimum is not enlarged")

        let off = SpatialSuperResolution.dimensions(outputWidth: 1200, outputHeight: 675,
                                                    mode: .off, supported: true)
        check(off == SpatialResolution(renderWidth: 1200, renderHeight: 675,
                                       outputWidth: 1200, outputHeight: 675), "Off retains legacy render size")

        let unsupported = SpatialSuperResolution.dimensions(outputWidth: 1200, outputHeight: 675,
                                                            mode: .performance, supported: false)
        check(unsupported == SpatialResolution(renderWidth: 1200, renderHeight: 675,
                                               outputWidth: 1200, outputHeight: 675), "unsupported device safely falls back")

        print("Spatial super-resolution sizing checks passed.")
    }
}
