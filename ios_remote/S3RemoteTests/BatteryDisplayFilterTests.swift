/*
 * BatteryDisplayFilterTests.swift — mirrors the C6 battery-ui regression
 * scenarios (esp32c6_car commit cee1189): median spike absorption, EMA
 * golden sequence, ≤2 Hz gate, 20 mV downward hysteresis, 1.5 s percent
 * drop confirmation, single-dip rejection, gap reset, not-ready samples.
 * EMA numbers hand-computed with the same exp(-dt/500) formula.
 */

import XCTest
@testable import S3Remote

final class BatteryDisplayFilterTests: XCTestCase {
    private var now = 1_000_000.0
    private var filter = BatteryDisplayFilter()

    override func setUp() {
        super.setUp()
        now = 1_000_000.0
        filter = BatteryDisplayFilter()
    }

    private func feed(pct: Int, mv: Int, stepMs: Double = 0) -> (mv: Int?, pct: Int?) {
        now += stepMs
        return filter.apply(pct: pct, mv: mv, nowMs: now)
    }

    // ---- not-ready samples ---------------------------------------------------

    func testUnreadyVoltageDoesNotInitialize() {
        XCTAssertNil(feed(pct: 50, mv: 0).mv)
        XCTAssertNil(feed(pct: 50, mv: 0).pct) // percent waits for valid voltage
        let first = feed(pct: 50, mv: 8000, stepMs: 100)
        XCTAssertEqual(first.mv, 8000)
        XCTAssertEqual(first.pct, 50)
    }

    func testPercentRangeChecks() {
        _ = feed(pct: 80, mv: 8000)
        XCTAssertNil(feed(pct: 101, mv: 8000, stepMs: 100).pct)
        XCTAssertNil(feed(pct: -1, mv: 8000, stepMs: 100).pct)
    }

    func testPercentZeroIsValidWhenVoltagePresent() {
        XCTAssertEqual(feed(pct: 80, mv: 8000).pct, 80)
        // sustained 0 %: candidate from the 3rd sample, confirmed >1.5 s later,
        // afterwards median == shown publishes nothing more
        var last: Int?
        for _ in 0..<12 { last = feed(pct: 0, mv: 8000, stepMs: 200).pct }
        XCTAssertEqual(filter.percentShown, 0)
        XCTAssertNil(last)
    }

    // ---- voltage plane --------------------------------------------------------

    func testMedianAbsorbsSingleSpike() {
        for _ in 0..<5 { _ = feed(pct: 80, mv: 8000, stepMs: 100) }
        XCTAssertEqual(filter.voltageShownMv, 8000)
        XCTAssertNil(feed(pct: 80, mv: 15000, stepMs: 500).mv) // 1-of-5 spike: median untouched
        XCTAssertEqual(filter.voltageShownMv, 8000)
    }

    func testVoltageGoldenEmaSequence() {
        // constant 8000 then sustained 7900, sampled every 500 ms:
        // EMA 8000 → 7936.79 → 7913.53 → 7904.98 → 7901.83; rounding to 10 mV
        // plus the 20 mV deadband yields 8000 → 7940 → 7910 then holds.
        XCTAssertEqual(feed(pct: 80, mv: 8000).mv, 8000)
        XCTAssertNil(feed(pct: 80, mv: 7900, stepMs: 500).mv) // median still 8000 (upper middle of 2)
        XCTAssertEqual(feed(pct: 80, mv: 7900, stepMs: 500).mv, 7940)
        XCTAssertEqual(feed(pct: 80, mv: 7900, stepMs: 500).mv, 7910)
        XCTAssertNil(feed(pct: 80, mv: 7900, stepMs: 500).mv) // 7900 blocked by deadband
        XCTAssertNil(feed(pct: 80, mv: 7900, stepMs: 500).mv)
        XCTAssertEqual(filter.voltageShownMv, 7910)
    }

    func testVoltageDisplayGateIs2Hz() {
        _ = feed(pct: 80, mv: 8000) // t0: first display immediate
        // within 500 ms of the first display the gate blocks even real drops
        // (the EMA still advances: [8000,7800] upper-median stays 8000, then
        // 7800 from the 3rd sample on)
        XCTAssertNil(feed(pct: 80, mv: 7800, stepMs: 100).mv)
        XCTAssertNil(feed(pct: 80, mv: 7800, stepMs: 100).mv)
        XCTAssertNil(feed(pct: 80, mv: 7800, stepMs: 100).mv) // t0+300, still gated
        // gate opens: EMA 7934.06 + (7800-7934.06)·(1-e^-0.6) = 7873.6 → 7870
        XCTAssertEqual(feed(pct: 80, mv: 7800, stepMs: 300).mv, 7870)
    }

    func testVoltageNeverRisesWithinSession() {
        _ = feed(pct: 80, mv: 8000)
        XCTAssertNil(feed(pct: 80, mv: 7900, stepMs: 500).mv) // [7900,8000] upper-median = 8000
        XCTAssertEqual(feed(pct: 80, mv: 7900, stepMs: 500).mv, 7940)
        XCTAssertEqual(feed(pct: 80, mv: 7900, stepMs: 500).mv, 7910)
        // real recovery: the display holds the latched lower value
        for _ in 0..<6 {
            XCTAssertNil(feed(pct: 80, mv: 8500, stepMs: 500).mv)
        }
        XCTAssertEqual(filter.voltageShownMv, 7910)
    }

    // ---- percent plane ----------------------------------------------------------

    func testPercentSingleDipNotLatched() {
        XCTAssertEqual(feed(pct: 80, mv: 8000).pct, 80)
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 100).pct) // [79,80] upper middle = 80
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 100).pct) // median 79 → candidate
        XCTAssertNil(feed(pct: 80, mv: 8000, stepMs: 100).pct) // [80,79,79,80] still 79
        XCTAssertNil(feed(pct: 80, mv: 8000, stepMs: 100).pct) // median back to 80: candidate cleared
        XCTAssertNil(feed(pct: 80, mv: 8000, stepMs: 1600).pct) // >1.5 s later, nothing latched
        XCTAssertEqual(filter.percentShown, 80) // the brief dip never won
    }

    func testPercentSustainedDropConfirms() {
        XCTAssertEqual(feed(pct: 80, mv: 8000).pct, 80)
        // candidate from the 3rd sample (median 79); early calls stay nil
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 200).pct) // [79,80] upper middle
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 200).pct) // candidate set
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 200).pct) // 400 ms in, pending
        XCTAssertNil(feed(pct: 79, mv: 8000, stepMs: 200).pct) // 600 ms in
        var last: Int?
        for _ in 0..<10 { last = feed(pct: 79, mv: 8000, stepMs: 200).pct }
        XCTAssertEqual(filter.percentShown, 79) // confirmed somewhere ≥1.5 s
        XCTAssertNil(last)                      // after latch: median == shown
    }

    func testPercentDeepDipDoesNotOvershoot() {
        XCTAssertEqual(feed(pct: 80, mv: 8000).pct, 80)
        // fluctuating 79/75 window: the candidate takes the HIGHEST lower median
        for step in [100, 200, 300, 400, 500] {
            let pct = step % 200 == 0 ? 75 : 79
            _ = feed(pct: pct, mv: 8000, stepMs: 100).pct
        }
        // confirm with fluctuation continuing: shown must be 79, not 75
        var r: Int?
        repeat { r = feed(pct: 79, mv: 8000, stepMs: 200).pct } while r == nil && now < 1_003_000
        XCTAssertEqual(r, 79)
        XCTAssertEqual(filter.percentShown, 79)
    }

    func testPercentGapResetsCandidateKeepsShown() {
        XCTAssertEqual(feed(pct: 80, mv: 8000).pct, 80)
        _ = feed(pct: 79, mv: 8000, stepMs: 100).pct // median still 80
        _ = feed(pct: 79, mv: 8000, stepMs: 100).pct // candidate set
        now += 1100 // telemetry gap > 1 s: candidate dropped, shown kept
        XCTAssertNil(feed(pct: 80, mv: 8000).pct)
        XCTAssertEqual(filter.percentShown, 80)
    }
}
