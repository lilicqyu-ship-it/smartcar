/*
 * ImuKinematics.swift — derived IMU quantities for the sensor tab's
 * instruments. Pure value logic so the math is unit-testable without a
 * link: the g-ball's gravity compensation, the strip charts' auto-range
 * and the peak-hold tracker.
 */

import Foundation

public enum ImuKinematics {
    /// Horizontal dynamic acceleration (body frame: fwd = car nose, lat = left)
    /// in g, after removing the gravity component the accelerometer reports
    /// when the car stands still.
    ///
    /// The signs mirror tc275_car app/fusion.c's own attitude definitions —
    /// pitch = atan2(−ax, |ay,az|) and roll = atan2(ay, az) — so at rest
    ///   ax_rest = −g·sin(pitch),   ay_rest = +g·sin(roll)
    /// Specific force f = a − g, so the car's linear acceleration is the
    /// live reading minus that rest reading. bench note: if the ball runs
    /// backwards under forward acceleration on the real car, flip `fwd`.
    public static func horizontalG(accMgX x: Double, y: Double, z: Double,
                                   rollDeg: Double, pitchDeg: Double)
        -> (fwd: Double, lat: Double, magnitude: Double) {
        let roll = rollDeg * .pi / 180
        let pitch = pitchDeg * .pi / 180
        let fwd = (x + 1000 * sin(pitch)) / 1000
        let lat = (y - 1000 * sin(roll)) / 1000
        return (fwd, lat, (fwd * fwd + lat * lat).squareRoot())
    }

    /// Shared auto-range for the strip charts: min/max over every visible
    /// series, symmetric around zero so the zero line stays centred. The
    /// bound takes 15 % headroom above the data, but never shrinks inside
    /// `minimumSpan` (which is a floor, not another input to amplify).
    public static func autoRange(values: [[Double]], minimumSpan: Double) -> ClosedRange<Double> {
        let flat = values.flatMap { $0 }
        guard let lo = flat.min(), let hi = flat.max() else {
            return -minimumSpan / 2...minimumSpan / 2
        }
        let headroom = max(abs(lo), abs(hi)) * 1.15
        let bound = max(headroom, minimumSpan / 2)
        return -bound...bound
    }
}

/// Monotone peak hold behind the g-ball's "最大水平加速度" readout.
public struct PeakGTracker: Sendable {
    public private(set) var peak: Double = 0

    public mutating func observe(_ magnitude: Double) {
        if magnitude > peak { peak = magnitude }
    }

    public mutating func reset() { peak = 0 }
}
