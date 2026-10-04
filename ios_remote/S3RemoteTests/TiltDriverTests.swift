/*
 * TiltDriverTests — gravity → axis mapping: signs, calibration, dead zone,
 * expo shaping and clamping.
 */

import XCTest
@testable import S3Remote

final class TiltDriverTests: XCTestCase {
    func testUprightNeutralYieldsZero() {
        let t = TiltDriver()
        let a = t.axes(gx: 0, gz: 0)
        XCTAssertEqual(a.v, 0)
        XCTAssertEqual(a.w, 0)
    }

    /// Top edge tipped away drives g.z negative → forward (v > 0).
    func testTiltAwayIsForward() {
        let t = TiltDriver()
        XCTAssertEqual(t.axes(gx: 0, gz: -TiltDriver.fullG).v, 1, accuracy: 0.001)
        XCTAssertGreaterThan(t.axes(gx: 0, gz: -0.25).v, 0)
        XCTAssertLessThan(t.axes(gx: 0, gz: 0.25).v, 0) // tipped toward user = reverse
    }

    /// Right edge down drives g.x positive → w NEGATIVE (wire: w > 0 = left).
    func testRollRightSteersRight() {
        let t = TiltDriver()
        XCTAssertEqual(t.axes(gx: TiltDriver.fullG, gz: 0).w, -1, accuracy: 0.001)
        XCTAssertGreaterThan(t.axes(gx: -0.25, gz: 0).w, 0) // left tilt = left steer
    }

    func testCalibrationShiftsNeutral() {
        var t = TiltDriver()
        t.calibrate(gx: 0.1, gz: -0.2) // user holds the phone slightly tilted
        let a = t.axes(gx: 0.1, gz: -0.2)
        XCTAssertEqual(a.v, 0)
        XCTAssertEqual(a.w, 0)
        XCTAssertGreaterThan(t.axes(gx: 0.1, gz: -0.7).v, 0.5)
    }

    func testDeadzoneAndSensitivity() {
        XCTAssertTrue(TiltDriver.shape(TiltDriver.deadG - 0.01) == 0)
        XCTAssertGreaterThan(TiltDriver.shape(TiltDriver.deadG + 0.05), 0)
        var t = TiltDriver()
        t.sensitivity = 2
        let gentle = TiltDriver().axes(gx: 0, gz: -0.25).v
        let boosted = t.axes(gx: 0, gz: -0.25).v
        XCTAssertGreaterThan(boosted, gentle)
    }

    func testClampsAtFullDeflection() {
        var t = TiltDriver()
        t.sensitivity = 2
        let a = t.axes(gx: 0, gz: -2)
        XCTAssertEqual(a.v, 1, accuracy: 0.001)
        XCTAssertEqual(TiltDriver.shape(99), 1, accuracy: 0.001)
        XCTAssertEqual(TiltDriver.shape(-99), -1, accuracy: 0.001)
    }
}
