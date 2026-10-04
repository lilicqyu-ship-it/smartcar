/*
 * OdometryTracker.swift — trail dead-reckoning from the two wheel-speed
 * channels of the 0x41 telemetry (left/right side composite, mm/s).
 * Unicycle model in the firmware's CCW-positive frame: ω = (vR − vL) / track.
 * Pure logic; AppState feeds every telemetry frame with the real inter-frame
 * dt and resets after a stream gap so reconnects don't draw jump lines.
 */

import Foundation

public struct OdometryPoint: Equatable, Sendable {
    public var x: Double // mm, + = forward (initial heading)
    public var y: Double // mm, + = left
    public init(x: Double, y: Double) {
        self.x = x
        self.y = y
    }
}

public struct OdometryTracker: Equatable, Sendable {
    /// Point cap with 2× decimation — hours of driving stay bounded.
    public static let maxPoints = 1500

    public var trackWidthMm: Double = 150
    public private(set) var points: [OdometryPoint] = [OdometryPoint(x: 0, y: 0)]
    /// rad, 0 = +x (initial forward), CCW-positive (firmware convention).
    public private(set) var heading: Double = 0
    public private(set) var distanceMm: Double = 0

    public init() {}

    public mutating func reset() {
        points = [OdometryPoint(x: 0, y: 0)]
        heading = 0
        distanceMm = 0
    }

    public mutating func onTelemetry(vL: Double, vR: Double, dtMs: Double) {
        let dt = min(max(dtMs, 5), 200) / 1000
        let v = (vL + vR) / 2
        let omega = trackWidthMm > 1 ? (vR - vL) / trackWidthMm : 0
        heading += omega * dt
        let last = points[points.count - 1]
        let step = v * dt // mm/s × s = mm
        points.append(OdometryPoint(x: last.x + cos(heading) * step,
                                    y: last.y + sin(heading) * step))
        distanceMm += abs(step)
        if points.count > Self.maxPoints {
            // keep every other sample (origin included) — halves the count,
            // preserves the overall shape
            points = points.enumerated().filter { $0.offset % 2 == 0 }.map(\.element)
        }
    }
}
