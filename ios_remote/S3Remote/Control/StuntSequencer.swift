/*
 * StuntSequencer.swift — one-tap stunt macros, pure logic. A Stunt is a list
 * of keyframes (duration + target axes −1...1); tick() linearly interpolates
 * from the previous keyframe (the first ramps up from rest), so moves blend
 * smoothly through the vehicle's own 0.5 s slew ramp. Output axes feed
 * DriveController.joyV/joyW, i.e. stunts ride the same 30 Hz gate, mode
 * limiting and latches as manual driving. Abort rules (STOP / e-stop /
 * joystick takeover / link loss) live in AppState — here only time and math.
 *
 * Axis convention is the wire convention (firmware arcade mix, link.c):
 *   v > 0 = forward, w > 0 = CCW (LEFT turn).
 */

import Foundation

public struct StuntKeyframe: Equatable, Sendable {
    /// Interpolation time from the previous keyframe's axes to this target.
    public var ms: Double
    public var v: Double // −1...1
    public var w: Double // −1...1, + = left
    public init(ms: Double, v: Double, w: Double) {
        self.ms = ms
        self.v = v
        self.w = w
    }
}

public struct Stunt: Identifiable, Equatable, Sendable {
    public let id: String
    public let name: String
    public let subtitle: String
    public let icon: String
    public let keyframes: [StuntKeyframe]

    public init(id: String, name: String, subtitle: String, icon: String,
                keyframes: [StuntKeyframe]) {
        self.id = id
        self.name = name
        self.subtitle = subtitle
        self.icon = icon
        self.keyframes = keyframes
    }

    public var durationMs: Double { keyframes.reduce(0) { $0 + $1.ms } }
}

public struct StuntSequencer: Equatable, Sendable {
    public private(set) var stunt: Stunt?
    public private(set) var elapsedMs: Double = 0
    /// Current interpolated axes (last value returned by tick).
    public private(set) var v: Double = 0
    public private(set) var w: Double = 0

    public var active: Bool { stunt != nil }
    public var progress: Double {
        guard let stunt, stunt.durationMs > 0 else { return 0 }
        return min(elapsedMs / stunt.durationMs, 1)
    }

    public init() {}

    public mutating func start(_ stunt: Stunt) {
        self.stunt = stunt
        elapsedMs = 0
        v = 0
        w = 0
    }

    /// Abort → rest axes; the caller's next tick sends DRIVE(0,0) as usual.
    public mutating func abort() {
        stunt = nil
        elapsedMs = 0
        v = 0
        w = 0
    }

    /// One control tick. Past the last keyframe the stunt ends and rest axes
    /// are returned (the DRIVE(0,0) heartbeat keeps flowing either way).
    @discardableResult
    public mutating func tick(dtMs: Double) -> (v: Double, w: Double) {
        guard let stunt else { return (0, 0) }
        elapsedMs += max(0, dtMs)
        if elapsedMs >= stunt.durationMs {
            abort()
            return (0, 0)
        }
        var boundary = 0.0
        var from = (v: 0.0, w: 0.0)
        for kf in stunt.keyframes {
            boundary += kf.ms
            guard elapsedMs < boundary else {
                from = (kf.v, kf.w)
                continue
            }
            let t = kf.ms > 0 ? min(max((elapsedMs - (boundary - kf.ms)) / kf.ms, 0), 1) : 1
            v = from.v + (kf.v - from.v) * t
            w = from.w + (kf.w - from.w) * t
            return (v, w)
        }
        return (0, 0)
    }
}

// ---- curated move set -----------------------------------------------------------

public extension Stunt {
    /// w > 0 = left (firmware arcade-mix convention, see link.c).
    static let library: [Stunt] = [
        Stunt(id: "spinL", name: "原地左旋", subtitle: "约一圈回旋", icon: "arrow.counterclockwise",
              keyframes: [StuntKeyframe(ms: 250, v: 0, w: 0.9),
                          StuntKeyframe(ms: 1350, v: 0, w: 0.9),
                          StuntKeyframe(ms: 350, v: 0, w: 0)]),
        Stunt(id: "spinR", name: "原地右旋", subtitle: "约一圈回旋", icon: "arrow.clockwise",
              keyframes: [StuntKeyframe(ms: 250, v: 0, w: -0.9),
                          StuntKeyframe(ms: 1350, v: 0, w: -0.9),
                          StuntKeyframe(ms: 350, v: 0, w: 0)]),
        Stunt(id: "eight", name: "8 字巡航", subtitle: "双环 ∞ 走线", icon: "infinity",
              keyframes: sineCurve(durationMs: 9000, periodMs: 3000, v: 0.6, wAmp: 0.9)),
        Stunt(id: "slalom", name: "S 形绕桩", subtitle: "连续摆位走线", icon: "arrow.trianglehead.swap",
              keyframes: sineCurve(durationMs: 6000, periodMs: 2500, v: 0.6, wAmp: 0.85)),
        Stunt(id: "launch", name: "弹射起步", subtitle: "满油冲刺急收", icon: "hare.fill",
              keyframes: [StuntKeyframe(ms: 250, v: 1, w: 0),
                          StuntKeyframe(ms: 900, v: 1, w: 0),
                          StuntKeyframe(ms: 250, v: 0, w: 0)]),
        Stunt(id: "drift", name: "漂移甩尾", subtitle: "减速反打回正", icon: "arrow.trianglehead.partial.circular",
              keyframes: [StuntKeyframe(ms: 500, v: 0.95, w: 0),
                          StuntKeyframe(ms: 250, v: 0.9, w: 0),
                          StuntKeyframe(ms: 600, v: 0.45, w: -1),
                          StuntKeyframe(ms: 600, v: 0.65, w: 0.35),
                          StuntKeyframe(ms: 400, v: 0.6, w: 0)]),
        Stunt(id: "dance", name: "舞蹈串烧", subtitle: "旋转·倒退·摇摆", icon: "music.note",
              keyframes: [StuntKeyframe(ms: 200, v: 0, w: 0.9),
                          StuntKeyframe(ms: 600, v: 0, w: 0.9),
                          StuntKeyframe(ms: 200, v: 0, w: 0),
                          StuntKeyframe(ms: 350, v: -0.7, w: 0),
                          StuntKeyframe(ms: 200, v: 0, w: -0.9),
                          StuntKeyframe(ms: 600, v: 0, w: -0.9),
                          StuntKeyframe(ms: 200, v: 0, w: 0),
                          StuntKeyframe(ms: 250, v: 0.55, w: 0.8),
                          StuntKeyframe(ms: 250, v: 0.55, w: -0.8),
                          StuntKeyframe(ms: 250, v: 0.55, w: 0.8),
                          StuntKeyframe(ms: 250, v: 0.55, w: -0.8),
                          StuntKeyframe(ms: 250, v: 0, w: 0)]),
        Stunt(id: "dash", name: "往返冲刺", subtitle: "去程满油回程倒车", icon: "arrow.left.arrow.right",
              keyframes: [StuntKeyframe(ms: 250, v: 0.95, w: 0),
                          StuntKeyframe(ms: 850, v: 0.95, w: 0),
                          StuntKeyframe(ms: 200, v: 0, w: 0),
                          StuntKeyframe(ms: 250, v: -0.75, w: 0),
                          StuntKeyframe(ms: 850, v: -0.75, w: 0),
                          StuntKeyframe(ms: 250, v: 0, w: 0)]),
    ]

    /// Sampled sine steering: a chain of short interpolated keyframes blends
    /// into a smooth oscillation (figure-8 / slalom).
    static func sineCurve(durationMs: Double, periodMs: Double,
                          v: Double, wAmp: Double) -> [StuntKeyframe] {
        let step = 250.0
        var frames: [StuntKeyframe] = []
        var t = 0.0
        while t < durationMs {
            let w = wAmp * sin(2 * .pi * t / periodMs)
            frames.append(StuntKeyframe(ms: step, v: v, w: w))
            t += step
        }
        frames.append(StuntKeyframe(ms: 300, v: 0, w: 0))
        return frames
    }
}
