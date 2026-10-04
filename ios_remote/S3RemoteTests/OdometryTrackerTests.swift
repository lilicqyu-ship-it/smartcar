/*
 * OdometryTrackerTests — unicycle dead-reckoning from the two wheel-speed
 * channels: straight lines, in-place spins, dt clamping, gap reset and
 * point-cap decimation.
 */

import XCTest
@testable import S3Remote

final class OdometryTrackerTests: XCTestCase {
    func testStraightRunTracksForward() {
        var o = OdometryTracker()
        // 500 mm/s both wheels, 50 Hz for 2 s → ~1000 mm forward, no lateral
        for _ in 0..<100 {
            o.onTelemetry(vL: 500, vR: 500, dtMs: 20)
        }
        let last = o.points.last!
        XCTAssertEqual(last.x, 1000, accuracy: 5)
        XCTAssertEqual(last.y, 0, accuracy: 1)
        XCTAssertEqual(o.heading, 0, accuracy: 0.001)
        XCTAssertEqual(o.distanceMm, 1000, accuracy: 5)
    }

    /// Spin in place: vR = −vL → heading rotates, body position stays put.
    func testInPlaceSpinRotatesWithoutTravelling() {
        var o = OdometryTracker()
        for _ in 0..<100 { // ω = (500−(−500))/150 rad/s ≈ 6.67 rad/s, 2 s ≈ 13.3 rad
            o.onTelemetry(vL: -500, vR: 500, dtMs: 20)
        }
        XCTAssertEqual(o.heading, 13.33, accuracy: 0.1)
        let last = o.points.last!
        XCTAssertEqual(hypot(last.x, last.y), 0, accuracy: 15)
        XCTAssertEqual(o.distanceMm, 0, accuracy: 5) // body speed is zero mid-spin
    }

    func testDtClamped() {
        var o = OdometryTracker()
        o.onTelemetry(vL: 1000, vR: 1000, dtMs: 10_000) // absurd dt
        XCTAssertEqual(o.points.last!.x, 200, accuracy: 1) // clamped to 200 ms
    }

    func testResetClearsEverything() {
        var o = OdometryTracker()
        o.onTelemetry(vL: 500, vR: 400, dtMs: 20)
        o.reset()
        XCTAssertEqual(o.points.count, 1)
        XCTAssertEqual(o.points[0].x, 0)
        XCTAssertEqual(o.heading, 0)
        XCTAssertEqual(o.distanceMm, 0)
    }

    func testPointCapDecimatesButKeepsGrowing() {
        var o = OdometryTracker()
        for _ in 0..<4000 { o.onTelemetry(vL: 300, vR: 300, dtMs: 20) }
        XCTAssertLessThanOrEqual(o.points.count, OdometryTracker.maxPoints)
        XCTAssertGreaterThan(o.points.count, 1)
        XCTAssertEqual(o.points.last!.x, 300.0 * 0.02 * 4000, accuracy: 50) // shape survives
    }
}
