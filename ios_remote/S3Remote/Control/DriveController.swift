/*
 * DriveController.swift — port of smartcar_remote scr_ctrl.c control state
 * machine (doc/05-ctrl.md): 30 Hz tick, mode limiting, STOP / emergency
 * latches, heartbeat policy. Pure logic; the caller (LinkEngine) owns the
 * 33 ms beat and turns returned commands into 0x50 DRIVE frames.
 *
 * Semantics (doc/05-ctrl.md §2/§3):
 *   gate closed (no conn / no CTRL role)  → nothing sent at all
 *   gate open + joystick nonzero          → DRIVE(joy * full * mode_pct)
 *   gate open + zero axes or latched stop → DRIVE(0,0) heartbeat continues
 *   STOP click  → immediate DRIVE(0,0) + stop latch, cleared by joystick touch
 *   STOP ≥1.2 s → EMERGENCY_STOP 0x32 + DRIVE(0,0) + full-screen overlay,
 *                 RELEASE clears emergency but stop latch REMAINS
 */

import Foundation

public struct DriveCommand: Equatable, Sendable {
    public var v: Int16 // mm/s
    public var w: Int16 // deg/s
    public init(v: Int16, w: Int16) {
        self.v = v
        self.w = w
    }
}

public enum DriveMode: String, CaseIterable, Identifiable, Codable, Sendable {
    case eco, normal, sport

    public var id: String { rawValue }

    public var pct: Int {
        switch self {
        case .eco: 50
        case .normal: 80
        case .sport: 100
        }
    }

    public var label: String {
        switch self {
        case .eco: "ECO"
        case .normal: "NORMAL"
        case .sport: "SPORT"
        }
    }
}

public struct DriveController: Sendable {
    /// Full-scale axis outputs, same as phone page app.js / S3 remote (spec 95-97).
    public static let fullV: Double = 600 // mm/s
    public static let fullW: Double = 300 // deg/s

    public var mode: DriveMode = .normal
    public var joyV: Double = 0 // throttle axis -1...1, + = forward
    public var joyW: Double = 0 // steering axis -1...1, + = right

    public private(set) var stopLatch = false
    public private(set) var emergLatch = false

    public init() {}

    public var gateOpen: Bool { !stopLatch && !emergLatch }

    /// STOP single click: immediate DRIVE(0,0) + latch (spec 19).
    /// Zeroes the axes first, exactly like scr_ctrl_stop_button().
    public mutating func stopClick() -> DriveCommand {
        joyV = 0
        joyW = 0
        stopLatch = true
        return DriveCommand(v: 0, w: 0)
    }

    /// STOP long press ≥1.2 s: caller additionally sends 0x32 EMERGENCY_STOP.
    /// scr_ctrl_emergency() sets BOTH latches and freezes the axes.
    public mutating func emergency() -> DriveCommand {
        joyV = 0
        joyW = 0
        emergLatch = true
        stopLatch = true
        return DriveCommand(v: 0, w: 0)
    }

    /// Overlay [RELEASE]: clears emergency; stop latch REMAINS — the vehicle
    /// stays stopped until the joystick is touched again (spec 105).
    public mutating func emergencyRelease() {
        emergLatch = false
    }

    /// Joystick touch = re-take control. Only clears the STOP latch when no
    /// emergency is latched (scr_ctrl_joystick_touch guard).
    public mutating func joystickTouch() {
        if stopLatch && !emergLatch {
            stopLatch = false
        }
    }

    /// Control lost (err auth / role switch): freeze inputs and latch stop
    /// (scr_ctrl_control_lost()).
    public mutating func controlLost() {
        joyV = 0
        joyW = 0
        stopLatch = true
    }

    /// One control tick. Returns the DRIVE payload to send, or nil when the
    /// send gate is closed (C6 rejects unauthorized frames with err{auth}).
    public mutating func tick(connUp: Bool, ctrlRole: Bool) -> DriveCommand? {
        guard connUp && ctrlRole else { return nil }
        if !gateOpen { return DriveCommand(v: 0, w: 0) }
        if joyV == 0 && joyW == 0 { return DriveCommand(v: 0, w: 0) }
        let scale = Double(mode.pct) / 100.0
        let v = Int16((joyV * Self.fullV * scale).rounded())
        let w = Int16((joyW * Self.fullW * scale).rounded())
        return DriveCommand(v: v, w: w)
    }
}
