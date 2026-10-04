/*
 * BatteryDisplayFilterTests.swift — mirrors the C6 battery-ui regression
 * scenarios (esp32c6_car commit cee1189): median spike absorption, EMA
 * golden sequence, ≤2 Hz gate, 20 mV downward hysteresis, 1.5 s percent
 * drop confirmation, single-dip rejection, gap reset, not-ready samples.
 * Extended for charge recovery: sustained-rise latching (percent ≥+2 % for
 * 10 s, voltage 50 mV upward deadband) and vehicle-reboot reset. EMA numbers
 * hand-computed with the same exp(-dt/500) formula.
 */

import XCTest
@testable import S3Remote

final class BatteryDisplayFilterTests: XCTestCase {
    private var now = 1_000_000.0
    private var uptime: UInt32 = 1_000_000
    private var filter = BatteryDisplayFilter()

    override func setUp() {
        super.setUp()
        now = 1_000_000.0
        uptime = 1_000_000
        filter = BatteryDisplayFilter()
    }

    private func feed(pct: Int, mv: Int, stepMs: Double = 0, uptimeMs: UInt32? = nil) -> (mv: Int?, pct: Int?) {
        now += stepMs
        uptime = uptimeMs ?? uptime &+ UInt32(stepMs)
        return filter.apply(pct: pct, mv: mv, uptimeMs: uptime, nowMs: now)
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

    /// Charging raises the pack voltage well above the latched value: with the
    /// 50 mV upward deadband the display must follow sustained recovery.
    func testVoltageFollowsChargingRecovery() {
        for _ in 0..<5 { _ = feed(pct: 40, mv: 7400, stepMs: 100) }
        XCTAssertEqual(filter.voltageShownMv, 7400)
        // charger connected: sustained +400 mV — the display climbs back up
        var sawRise = false
        for _ in 0..<20 {
            if feed(pct: 60, mv: 7800, stepMs: 500).mv != nil { sawRise = true }
        }
        XCTAssertTrue(sawRise, "sustained charge voltage must raise the display")
        XCTAssertGreaterThan(filter.voltageShownMv!, 7400 + BatteryDisplayFilter.riseDeadbandMv)
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

    // ---- charge recovery (the reason for the rise plane) ------------------------

    /// Charging with the car powered on: sustained +2 % or more walks the
    /// display up in confirmed steps, eventually to 100 %.
    func testPercentRisesAfterCharging() {
        XCTAssertEqual(feed(pct: 60, mv: 7600).pct, 60)
        // charger attached, pack climbs 60 → 95 over "minutes"
        var latched: Int?
        for _ in 0..<80 {
            if let published = feed(pct: 95, mv: 8300, stepMs: 500).pct { latched = published }
        }
        XCTAssertEqual(filter.percentShown, 95)
        XCTAssertEqual(latched, 95, "the confirm step must publish once")
        // keep charging: after another sustained stretch it reaches full
        for _ in 0..<40 { _ = feed(pct: 100, mv: 8400, stepMs: 500) }
        XCTAssertEqual(filter.percentShown, 100)
    }

    /// A brief voltage bounce (load released → percent estimate pops up for a
    /// few seconds) must NOT drag the display up — 10 s confirmation gate.
    func testPercentBriefRiseBlipNotLatched() {
        XCTAssertEqual(feed(pct: 60, mv: 7600).pct, 60)
        // ~6 s at +10 % — under the 10 s confirm window
        for _ in 0..<12 { XCTAssertNil(feed(pct: 70, mv: 7900, stepMs: 500).pct) }
        XCTAssertEqual(filter.percentShown, 60)
        // back to normal: nothing latched from the blip
        for _ in 0..<10 { _ = feed(pct: 61, mv: 7620, stepMs: 500) }
        XCTAssertEqual(filter.percentShown, 60)
    }

    /// Sub-hysteresis rises (median within shown..<shown+2 %) never arm a rise.
    func testPercentRiseBelowHysteresisIgnored() {
        XCTAssertEqual(feed(pct: 60, mv: 7600).pct, 60)
        for _ in 0..<40 { XCTAssertNil(feed(pct: 61, mv: 7620, stepMs: 500).pct) }
        XCTAssertEqual(filter.percentShown, 60)
    }

    // ---- vehicle restart ------------------------------------------------------------

    /// Power-off → charge → power-on: uptime regresses → both planes reset and
    /// re-seed from the current frame (display must snap to the truth, not stay
    /// latched at the old drained value).
    func testRebootResetsAndReseeds() {
        // drain first: display latched to a low value
        XCTAssertEqual(feed(pct: 60, mv: 7600).pct, 60)
        for _ in 0..<12 { _ = feed(pct: 55, mv: 7500, stepMs: 200) }
        XCTAssertLessThanOrEqual(filter.percentShown!, 60)
        // overnight charge, power cycled: uptime restarts from ~3 s
        let first = feed(pct: 100, mv: 8400, stepMs: 200, uptimeMs: 3_000)
        XCTAssertEqual(first.pct, 100) // immediate re-seed
        XCTAssertEqual(first.mv, 8400)
        XCTAssertEqual(filter.percentShown, 100)
        XCTAssertEqual(filter.voltageShownMv, 8400)
        // subsequent frames behave normally (no repeated resets)
        XCTAssertNil(feed(pct: 100, mv: 8400, stepMs: 200).pct)
        XCTAssertEqual(filter.percentShown, 100)
    }
}
