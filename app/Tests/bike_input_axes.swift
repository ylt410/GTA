@main enum BikeInputAxesChecks {
    static func main() {
        // A diagonal turn must not reduce the vertical bike-lean axis.
        let heldWheelie = BikeInputAxes.independentAxis(displacement: 48, travel: 48)
        let heldStoppie = BikeInputAxes.independentAxis(displacement: -48, travel: 48)
        assert(heldWheelie == 1)
        assert(heldStoppie == -1)
        assert(BikeInputAxes.independentAxis(displacement: 96, travel: 48) == 1)
        assert(BikeInputAxes.independentAxis(displacement: -96, travel: 48) == -1)
        assert(BikeInputAxes.independentAxis(displacement: 12, travel: 48) == 0.25)
        assert(BikeInputAxes.independentAxis(displacement: 12, travel: 0) == 0)
        assert(BikeInputAxes.independentAxis(displacement: .nan, travel: 48) == 0)
        print("BIKE INPUT AXIS CHECKS PASSED")
    }
}
