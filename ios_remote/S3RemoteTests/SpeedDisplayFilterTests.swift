/*
 * SpeedDisplayFilterTests — C6 renderSpeed port: first-frame snap, dt-aware
 * EMA convergence (hand-computed golden step), rest snapping, gap re-seed,
 * signed (reverse) passthrough.
 */

import XCTest
@testable import S3Remote

final class SpeedDisplayFilterTests: XCTestCase {
    private var now = 1_000_000.0
    private var filter = SpeedDisplayFilter()

    override func setUp() {
        super.setUp()
        now = 1_000_000.0
        filter = SpeedDisplayFilter()
    }

    private func feed(_ mmS: Double, stepMs: Double) -> Double {
        now += stepMs
        return filter.apply(mmS, nowMs: now)
    }

    func testFirstFrameSnapsInsteadOfRampingFromZero() {
        XCTAssertEqual(feed(400, stepMs: 0), 400, accuracy: 0.001)
    }

    func testEMAConvergesWithGoldenStep() {
        _ = feed(400, stepMs: 0)
        // one 50 ms step at tau 150 ms: 400 + (500−400)·(1−e^(−1/3)) = 428.35
        XCTAssertEqual(feed(500, stepMs: 50), 428.35, accuracy: 0.1)
        let v = feed(500, stepMs: 50)
        XCTAssertGreaterThan(v, 428.35)
        XCTAssertLessThan(v, 470)
    }

    func testSustainedSpeedConverges() {
        var last = 0.0
        for _ in 0..<60 { last = feed(500, stepMs: 20) } // 1.2 s ≫ tau
        XCTAssertEqual(last, 500, accuracy: 2)
    }

    /// Below 30 mm/s counts as stopped: the value snaps instead of lingering.
    func testRestCrossingSnaps() {
        for _ in 0..<40 { _ = feed(500, stepMs: 20) }
        XCTAssertEqual(feed(5, stepMs: 20), 5, accuracy: 0.001)
        XCTAssertEqual(feed(-8, stepMs: 20), -8, accuracy: 0.001)
    }

    func testTransmissionGapReSeeds() {
        _ = feed(400, stepMs: 0)
        for _ in 0..<10 { _ = feed(500, stepMs: 20) }
        // 500 ms blind → next frame must snap, not ramp across the gap
        XCTAssertEqual(feed(200, stepMs: 500), 200, accuracy: 0.001)
    }

    func testReverseSpeedStaysSigned() {
        _ = feed(-300, stepMs: 0)
        XCTAssertEqual(feed(-300, stepMs: 20), -300, accuracy: 1)
    }

    func testResetClearsSeed() {
        _ = feed(400, stepMs: 0)
        filter.reset()
        // after reset the next sample snaps again regardless of tau
        XCTAssertEqual(feed(900, stepMs: 10), 900, accuracy: 0.001)
    }
}
