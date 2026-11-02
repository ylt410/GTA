import Foundation

/// An independent, clamped thumbstick axis. Bikes use vertical lean at full
/// strength while horizontal input steers; a circular stick gate otherwise
/// reduces lean as soon as the player turns.
enum BikeInputAxes {
    static func independentAxis(displacement: Double, travel: Double) -> Double {
        guard travel > 0, displacement.isFinite else { return 0 }
        return max(-1, min(1, displacement / travel))
    }
}
