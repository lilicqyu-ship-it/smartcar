/*
 * LinkQualityTests.swift — RSSI grading boundaries identical to the S3
 * remote's Kconfig thresholds (−60/−67/−75/−85, spec 26) and the RTT/loss
 * composite estimate used while the gateway doesn't report RSSI.
 */

import XCTest
@testable import S3Remote

final class LinkQualityTests: XCTestCase {
    func testRssiGradingBoundaries() {
        XCTAssertEqual(LinkQuality.bars(forRssi: -50), 4)
        XCTAssertEqual(LinkQuality.bars(forRssi: -60), 4) // ≥ −60 excellent
        XCTAssertEqual(LinkQuality.bars(forRssi: -61), 3)
        XCTAssertEqual(LinkQuality.bars(forRssi: -67), 3)
        XCTAssertEqual(LinkQuality.bars(forRssi: -68), 2)
        XCTAssertEqual(LinkQuality.bars(forRssi: -75), 2)
        XCTAssertEqual(LinkQuality.bars(forRssi: -76), 1)
        XCTAssertEqual(LinkQuality.bars(forRssi: -85), 1)
        XCTAssertEqual(LinkQuality.bars(forRssi: -95), 1)
    }

    func testRssiLabels() {
        XCTAssertEqual(LinkQuality.label(forRssi: -55), "优")
        XCTAssertEqual(LinkQuality.label(forRssi: -65), "良")
        XCTAssertEqual(LinkQuality.label(forRssi: -70), "中")
        XCTAssertEqual(LinkQuality.label(forRssi: -80), "弱")
        XCTAssertEqual(LinkQuality.label(forRssi: -90), "极弱")
    }

    func testEstimatedBarsFromRtt() {
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 25, lossPerMille: 0), 4)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 40, lossPerMille: 0), 4)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 60, lossPerMille: 0), 3)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 80, lossPerMille: 0), 3)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 120, lossPerMille: 0), 2)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 500, lossPerMille: 0), 1)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 0, lossPerMille: 0), 1) // no sample yet
    }

    func testEstimatedBarsLossPenalty() {
        // heavy loss caps the estimate even with a fast RTT
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 25, lossPerMille: 150), 2)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 25, lossPerMille: 350), 1)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 60, lossPerMille: 150), 2)
        // light loss leaves the RTT-driven base untouched
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 25, lossPerMille: 20), 4)
        XCTAssertEqual(LinkQuality.estimatedBars(rttMs: 120, lossPerMille: 20), 2)
    }

    func testEstimatedLabels() {
        XCTAssertEqual(LinkQuality.estimatedLabel(bars: 4), "优")
        XCTAssertEqual(LinkQuality.estimatedLabel(bars: 1), "弱")
    }
}
