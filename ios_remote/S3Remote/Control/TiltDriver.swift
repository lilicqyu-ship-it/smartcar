/*
 * TiltDriver.swift — tilt-steering mapping, pure math. MotionSource supplies
 * device-frame gravity in g units at ~30 Hz; a portrait phone standing upright
 * has gravity ≈ (0, −1, 0). Tipping the top edge away drives g.z negative
 * (throttle forward), dipping the right edge drives g.x positive.
 *
 * Wire axis convention (same as JoystickInput / firmware link.c):
 *   v > 0 = forward, w > 0 = CCW (LEFT turn) — so right tilt yields w < 0.
 */

import Foundation

public struct TiltDriver: Equatable, Sendable {
    /// Full deflection at 30° of tilt: |g| component there is sin(30°) = 0.5.
    public static let fullG: Double = 0.5
    /// Per-axis dead zone in g units (~4°).
    public static let deadG: Double = 0.07

    /// Response multiplier, 0.5...2 (settings).
    public var sensitivity: Double = 1
    /// Captured upright pose (calibration).
    public var neutralX: Double = 0
    public var neutralZ: Double = 0

    public init() {}

    /// Capture the current pose as neutral.
    public mutating func calibrate(gx: Double, gz: Double) {
        neutralX = gx
        neutralZ = gz
    }

    public func axes(gx: Double, gz: Double) -> (v: Double, w: Double) {
        let rawV = -(gz - neutralZ) / Self.fullG * sensitivity // top away → forward
        let rawW = -(gx - neutralX) / Self.fullG * sensitivity // right edge down → right (w < 0)
        return (Self.shape(rawV), Self.shape(rawW))
    }

    /// Dead zone → renormalize → soft expo curve → clamp to ±1.
    static func shape(_ a: Double) -> Double {
        let z = abs(a)
        guard z > deadG else { return 0 }
        let n = min((z - deadG) / (1 - deadG), 1)
        let curved = pow(n, 1.35)
        return a < 0 ? -curved : curved
    }
}
