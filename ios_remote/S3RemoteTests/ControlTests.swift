/*
 * ControlTests.swift — scr_ctrl semantics port: gate, mode limiting,
 * heartbeat policy, STOP / emergency latch sequencing (verified against
 * scr_ctrl.c lines 229-331), safety watchdog debounce + battery hysteresis,
 * telemetry loss accounting.
 */

import XCTest
@testable import S3Remote

final class DriveControllerTests: XCTestCase {
    func testGateClosedSendsNothing() {
        var c = DriveController()
        c.joyV = 1
        XCTAssertNil(c.tick(connUp: true, ctrlRole: false))
        XCTAssertNil(c.tick(connUp: false, ctrlRole: true))
        XCTAssertNil(c.tick(connUp: false, ctrlRole: false))
    }

    func testZeroAxesSendHeartbeatZeroDrive() {
        var c = DriveController()
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0))
    }

    func testModeLimiting() {
        var c = DriveController()
        c.joyV = 1.0
        c.joyW = -1.0
        c.mode = .eco
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 300, w: -150))
        c.mode = .normal
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 480, w: -240))
        c.mode = .sport
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 600, w: -300))
    }

    func testStopClickLatchesUntilJoystickTouch() {
        var c = DriveController()
        c.joyV = 0.8
        let zero = c.stopClick() // immediate zero + latch + axis freeze
        XCTAssertEqual(zero, DriveCommand(v: 0, w: 0))
        XCTAssertEqual(c.joyV, 0)
        // even with axes pushed again before touch, the latch holds zero
        c.joyV = 0.5
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0))
        // touching the joystick re-takes control
        c.joystickTouch()
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 240, w: 0))
    }

    func testEmergencySetsBothLatchesAndSurvivesRelease() {
        var c = DriveController()
        c.joyV = 1.0
        _ = c.emergency() // caller additionally sends 0x32
        XCTAssertTrue(c.emergLatch)
        XCTAssertTrue(c.stopLatch) // scr_ctrl_emergency sets stop too
        XCTAssertEqual(c.joyV, 0)
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0))

        c.emergencyRelease() // emerg clears, stop latch REMAINS (spec 105)
        XCTAssertFalse(c.emergLatch)
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0))

        c.joystickTouch() // explicit re-take required
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0)) // axes are still zero
        c.joyV = 1.0
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 480, w: 0))
    }

    func testJoystickTouchIgnoredWhileEmergencyLatched() {
        var c = DriveController()
        _ = c.emergency()
        c.joystickTouch() // must NOT clear the stop latch under emergency
        XCTAssertTrue(c.stopLatch)
    }

    func testControlLostFreezesAndLatches() {
        var c = DriveController()
        c.joyV = 1.0
        c.controlLost() // err auth path (scr_ctrl_control_lost)
        XCTAssertEqual(c.joyV, 0)
        XCTAssertTrue(c.stopLatch)
        XCTAssertEqual(c.tick(connUp: true, ctrlRole: true), DriveCommand(v: 0, w: 0))
    }
}

final class SafetyMonitorTests: XCTestCase {
    func testRadioLostDebounce() {
        var m = SafetyMonitor()
        m.debounceMs = 1200
        var now = 1000.0
        // brief hiccup: no alert within the debounce window
        XCTAssertFalse(m.radioLostActive(nowMs: now, linkUp: false, teleFresh: false))
        now += 500
        XCTAssertFalse(m.radioLostActive(nowMs: now, linkUp: false, teleFresh: false))
        now += 400
        XCTAssertFalse(m.radioLostActive(nowMs: now, linkUp: false, teleFresh: false)) // 900 ms
        // recovery resets the window
        now += 100
        _ = m.radioLostActive(nowMs: now, linkUp: true, teleFresh: true)
        now += 100
        XCTAssertFalse(m.radioLostActive(nowMs: now, linkUp: false, teleFresh: false))
        // sustained loss past 1200 ms alerts
        now += 1200
        XCTAssertTrue(m.radioLostActive(nowMs: now, linkUp: false, teleFresh: false))
    }

    func testRadioLostNeedsLinkAndTelemetry() {
        var m = SafetyMonitor()
        XCTAssertFalse(m.radioLostActive(nowMs: 0, linkUp: true, teleFresh: false))
        XCTAssertFalse(m.radioLostActive(nowMs: 100, linkUp: false, teleFresh: true))
    }

    func testBatteryThresholdsAndHysteresis() {
        var m = SafetyMonitor()
        XCTAssertEqual(m.batteryWatch(50), nil)
        XCTAssertEqual(m.batteryWatch(20), .lowBattery)
        XCTAssertEqual(m.batteryWatch(10), .criticalBattery)
        // severity follows the current level: ≤10 CRIT, ≤20 LOW,
        // recovery only at LOW+5 = 25
        XCTAssertEqual(m.batteryWatch(15), .lowBattery)
        XCTAssertEqual(m.batteryWatch(10), .criticalBattery)
        XCTAssertEqual(m.batteryWatch(24), .lowBattery)
        XCTAssertEqual(m.batteryWatch(25), nil)
        XCTAssertEqual(m.batteryWatch(30), nil)
        // re-arms after recovery
        XCTAssertEqual(m.batteryWatch(19), .lowBattery)
    }

    func testFaultWatch() {
        XCTAssertFalse(SafetyMonitor.faultActive(0))
        XCTAssertTrue(SafetyMonitor.faultActive(0x0002))
    }
}

final class TelemetryLossCounterTests: XCTestCase {
    func testGapCountsLoss() {
        var c = TelemetryLossCounter()
        c.onTelemetry(seq: 10)
        c.onTelemetry(seq: 13) // gap of 3 → 2 lost
        c.onTelemetry(seq: 14)
        XCTAssertEqual(c.lost, 2)
        XCTAssertEqual(c.received, 3)
        XCTAssertEqual(c.lossPerMille, 2.0 / 5.0 * 1000, accuracy: 0.001)
    }

    func testWrapGapCountedLikeC() {
        var c = TelemetryLossCounter()
        c.onTelemetry(seq: .max - 5)
        c.onTelemetry(seq: 3) // Δ = 9 (mod 2^32) → 8 lost, same as the C impl
        XCTAssertEqual(c.lost, 8)
    }

    func testDuplicateAndBackwardsIgnored() {
        var c = TelemetryLossCounter()
        c.onTelemetry(seq: 100)
        c.onTelemetry(seq: 100) // Δ=0
        c.onTelemetry(seq: 99)  // Δ=0xFFFFFFFF → ignored
        XCTAssertEqual(c.lost, 0)
        XCTAssertEqual(c.received, 3)
    }

    func testLargeGapBeyond1000Ignored() {
        var c = TelemetryLossCounter()
        c.onTelemetry(seq: 1)
        c.onTelemetry(seq: 1001) // Δ=1000, not <1000 → ignored (C parity)
        XCTAssertEqual(c.lost, 0)
    }
}
