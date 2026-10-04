/*
 * SpeedDisplayFilter.swift — port of the C6 control-page speed display EMA
 * (esp32c6_car assets_src/app.js renderSpeed): the raw 50 Hz mean wheel speed
 * flickers the digits constantly, so the DISPLAY value is a dt-aware EMA
 * (tau 150 ms) that snaps instead of ramping on the first frame, after a
 * >400 ms transmission gap, and whenever crossing in/out of rest (<30 mm/s
 * counts as stopped ≈ one 0.1 km/h digit). Display layer ONLY: trail
 * odometry, records and safety keep using the raw telemetry.
 */

import Foundation

public struct SpeedDisplayFilter: Sendable {
    public static let stopMmS: Double = 30
    public static let tauMs: Double = 150
    public static let gapResetMs: Double = 400
    static let maxDtMs: Double = 1000

    private var dispVMmS: Double = 0
    private var seeded = false
    private var lastTsMs: Double?

    public init() {}

    /// Feed one body speed (signed mean wheel speed, mm/s); returns the
    /// smoothed display speed in mm/s.
    public mutating func apply(_ bodyMmS: Double, nowMs: Double) -> Double {
        let dt = lastTsMs.map { min(Self.maxDtMs, max(0, nowMs - $0)) } ?? 0
        lastTsMs = nowMs
        if !seeded || dt > Self.gapResetMs || abs(bodyMmS) < Self.stopMmS {
            dispVMmS = bodyMmS // snap: first frame / gap / rest crossing
            seeded = true
        } else {
            dispVMmS += (bodyMmS - dispVMmS) * (1 - exp(-dt / Self.tauMs))
        }
        return dispVMmS
    }

    public mutating func reset() {
        dispVMmS = 0
        seeded = false
        lastTsMs = nil
    }
}
