/*
 * RawImuAttitudeTracker.swift — display-only attitude preview while the TC275
 * IMU-to-car axis mapping is uncalibrated. Angles stay in the sensor frame:
 * gravity supplies approximate tilt; gyro Z supplies relative rotation only.
 * No value from this tracker feeds steering, safety or car-frame telemetry.
 */

import Foundation

public struct RawImuAttitude: Equatable, Sendable {
    public let rollDeg: Double
    public let pitchDeg: Double
    public let relativeYawDeg: Double
    public let yawRateDegS: Double
}

public struct RawImuAttitudeTracker: Sendable {
    public private(set) var attitude: RawImuAttitude?
    private var previousStampMs: Int?

    public init() {}

    public mutating func observe(_ sample: ImuSample) {
        let x = Double(sample.accMg[0])
        let y = Double(sample.accMg[1])
        let z = Double(sample.accMg[2])
        let magnitude = (x * x + y * y + z * z).squareRoot()
        let rate = Double(sample.gyroMdps[2]) / 1000
        let dtMs = previousStampMs.map { sample.stampMs - $0 } ?? 0
        previousStampMs = sample.stampMs
        let dt = (1...250).contains(dtMs) ? Double(dtMs) / 1000 : 0
        let yaw = (attitude?.relativeYawDeg ?? 0) + (abs(rate) > 0.6 ? rate * dt : 0)

        // Strong linear acceleration is not a gravity reference. Keep the
        // previous tilt until the magnitude returns near 1 g.
        let gravityUsable = (700...1300).contains(magnitude)
        let measuredRoll = atan2(y, z) * 180 / .pi
        let measuredPitch = atan2(-x, (y * y + z * z).squareRoot()) * 180 / .pi
        let alpha = dt > 0 ? min(1, dt / 0.12) : 1
        let roll = gravityUsable
            ? Self.followAngle(attitude?.rollDeg, measuredRoll, alpha: alpha)
            : (attitude?.rollDeg ?? 0)
        let pitch = gravityUsable
            ? Self.followAngle(attitude?.pitchDeg, measuredPitch, alpha: alpha)
            : (attitude?.pitchDeg ?? 0)
        attitude = RawImuAttitude(rollDeg: roll, pitchDeg: pitch,
                                  relativeYawDeg: yaw, yawRateDegS: rate)
    }

    public mutating func reset() {
        attitude = nil
        previousStampMs = nil
    }

    private static func followAngle(_ current: Double?, _ target: Double, alpha: Double) -> Double {
        guard let current else { return target }
        let delta = (target - current + 540).truncatingRemainder(dividingBy: 360) - 180
        return current + delta * alpha
    }
}
