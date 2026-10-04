/*
 * RecordsTrackerTests — record-wall peaks, best-lap tracking and the lap
 * stopwatch (splits, stop totals, reset). Persistence itself is UserDefaults
 * I/O and stays untested (device storage), the logic here is pure.
 */

import XCTest
@testable import S3Remote

final class RecordsTrackerTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "test.records.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        super.tearDown()
    }

    func testObserveUpdatesPeaksAndReportsChanges() {
        var r = RecordsTracker(records: Records(), defaults: defaults)
        XCTAssertTrue(r.observe(speedKmh: 1.5, sessionMeters: 10))
        XCTAssertEqual(r.records.topSpeedKmh, 1.5, accuracy: 0.001)
        XCTAssertEqual(r.records.longestSessionM, 10, accuracy: 0.001)
        XCTAssertFalse(r.observe(speedKmh: 1.0, sessionMeters: 5)) // below peaks
        XCTAssertTrue(r.observe(speedKmh: 2.0, sessionMeters: 20))
    }

    func testNoteLapKeepsBestAndIgnoresJunk() {
        var r = RecordsTracker(records: Records(), defaults: defaults)
        XCTAssertTrue(r.noteLap(seconds: 12.5))
        XCTAssertFalse(r.noteLap(seconds: 13.0))
        XCTAssertTrue(r.noteLap(seconds: 11.2))
        XCTAssertEqual(r.records.bestLapS, 11.2, accuracy: 0.001)
        XCTAssertFalse(r.noteLap(seconds: 0.2)) // fat-finger tap
    }

    func testClearResets() {
        var r = RecordsTracker(records: Records(), defaults: defaults)
        _ = r.observe(speedKmh: 2, sessionMeters: 30)
        _ = r.noteLap(seconds: 9)
        r.clear()
        XCTAssertEqual(r.records, Records())
    }

    /// Improvements land in the injected UserDefaults store immediately.
    func testImprovementsPersistToInjectedDefaults() {
        var r = RecordsTracker(records: Records(), defaults: defaults)
        _ = r.observe(speedKmh: 1.7, sessionMeters: 5)
        _ = r.noteLap(seconds: 8.8)
        let reloaded = RecordsTracker(records: .load(from: defaults), defaults: defaults)
        XCTAssertEqual(reloaded.records.topSpeedKmh, 1.7, accuracy: 0.001)
        XCTAssertEqual(reloaded.records.bestLapS, 8.8, accuracy: 0.001)
    }
}

final class LapTimerTests: XCTestCase {
    func testIdleTimerReadsZeroAndIgnoresLap() {
        var t = LapTimer()
        XCTAssertEqual(t.currentS(nowMs: 1000), 0)
        XCTAssertNil(t.lap(nowMs: 2000))
        XCTAssertNil(t.stop(nowMs: 2000))
    }

    func testSplitsAccumulateFromStart() {
        var t = LapTimer()
        t.start(nowMs: 0)
        XCTAssertEqual(t.currentS(nowMs: 1500), 1.5, accuracy: 0.001)
        XCTAssertEqual(t.lap(nowMs: 10_000)!, 10, accuracy: 0.001)
        XCTAssertEqual(t.lap(nowMs: 22_500)!, 12.5, accuracy: 0.001)
        XCTAssertEqual(t.laps, [10, 12.5])
        XCTAssertEqual(t.stop(nowMs: 30_000)!, 30, accuracy: 0.001)
        XCTAssertEqual(t.currentS(nowMs: 99_000), 30, accuracy: 0.001)
        XCTAssertFalse(t.running)
    }

    func testResetClears() {
        var t = LapTimer()
        t.start(nowMs: 0)
        _ = t.lap(nowMs: 5_000)
        t.stop(nowMs: 8_000)
        t.reset()
        XCTAssertEqual(t, LapTimer())
        XCTAssertEqual(t.currentS(nowMs: 9_000), 0)
    }

    func testRestartClearsPreviousSession() {
        var t = LapTimer()
        t.start(nowMs: 0)
        _ = t.lap(nowMs: 5_000)
        t.start(nowMs: 100_000)
        XCTAssertEqual(t.laps, [])
        XCTAssertEqual(t.currentS(nowMs: 101_000), 1, accuracy: 0.001)
    }
}
