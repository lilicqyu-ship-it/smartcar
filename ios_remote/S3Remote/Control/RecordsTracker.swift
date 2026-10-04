/*
 * RecordsTracker.swift — local record wall (top speed / longest session /
 * best lap) and the lap-split stopwatch, pure logic. Records persist to
 * UserDefaults as one JSON blob (AppState saves when a record improves);
 * decode is lenient so future fields don't wipe saved data.
 */

import Foundation

public struct Records: Equatable, Sendable {
    public var topSpeedKmh: Double = 0
    public var longestSessionM: Double = 0
    public var bestLapS: Double = 0 // 0 = none yet

    public init() {}

    static let defaultsKey = "s3remote.records.v1"

    public static func load(from defaults: UserDefaults = .standard) -> Records {
        guard
            let data = defaults.data(forKey: defaultsKey),
            let r = try? JSONDecoder().decode(Records.self, from: data)
        else { return Records() }
        return r
    }

    func save(to defaults: UserDefaults = .standard) {
        if let data = try? JSONEncoder().encode(self) {
            defaults.set(data, forKey: Self.defaultsKey)
        }
    }
}

extension Records: Codable {
    private enum CodingKeys: String, CodingKey {
        case topSpeedKmh, longestSessionM, bestLapS
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        topSpeedKmh = try c.decodeIfPresent(Double.self, forKey: .topSpeedKmh) ?? 0
        longestSessionM = try c.decodeIfPresent(Double.self, forKey: .longestSessionM) ?? 0
        bestLapS = try c.decodeIfPresent(Double.self, forKey: .bestLapS) ?? 0
    }
}

/// Not Sendable by design: UserDefaults is thread-safe but not marked as
/// such, and the tracker only ever runs inside @MainActor AppState.
public struct RecordsTracker: Equatable {
    public private(set) var records: Records
    private let defaults: UserDefaults

    public init(records: Records = .load(), defaults: UserDefaults = .standard) {
        self.records = records
        self.defaults = defaults
    }

    /// Feed per telemetry frame; returns true when a record improved
    /// (caller persists).
    @discardableResult
    public mutating func observe(speedKmh: Double, sessionMeters: Double) -> Bool {
        var changed = false
        if speedKmh > records.topSpeedKmh {
            records.topSpeedKmh = speedKmh
            changed = true
        }
        if sessionMeters > records.longestSessionM {
            records.longestSessionM = sessionMeters
            changed = true
        }
        if changed { records.save(to: defaults) }
        return changed
    }

    /// Returns true when this lap became the new best.
    @discardableResult
    public mutating func noteLap(seconds: Double) -> Bool {
        guard seconds > 0.5 else { return false } // ignore fat-finger taps
        if records.bestLapS == 0 || seconds < records.bestLapS {
            records.bestLapS = seconds
            records.save(to: defaults)
            return true
        }
        return false
    }

    public mutating func clear() {
        records = Records()
        records.save(to: defaults)
    }
}

// ---- lap stopwatch -----------------------------------------------------------------

/// Manual split timer: start → lap (per crossing) → stop → reset. Splits are
/// stored per lap; stopping without a final lap just ends the display time.
public struct LapTimer: Equatable, Sendable {
    public private(set) var running = false
    public private(set) var startMs: Double = 0
    /// Split seconds per completed lap, oldest first.
    public private(set) var laps: [Double] = []
    /// Total seconds when stopped, nil while running/never started.
    public private(set) var finishedS: Double?

    public init() {}

    public func currentS(nowMs: Double) -> Double {
        if running { return max(0, (nowMs - startMs) / 1000) }
        return finishedS ?? 0
    }

    public mutating func start(nowMs: Double) {
        running = true
        startMs = nowMs
        laps = []
        finishedS = nil
    }

    /// Records a split; returns it, or nil when not running.
    public mutating func lap(nowMs: Double) -> Double? {
        guard running else { return nil }
        let split = (nowMs - startMs) / 1000 - laps.reduce(0, +)
        guard split > 0 else { return nil }
        laps.append(split)
        return split
    }

    /// Stops the clock; returns the total, or nil when not running.
    public mutating func stop(nowMs: Double) -> Double? {
        guard running else { return nil }
        running = false
        finishedS = (nowMs - startMs) / 1000
        return finishedS
    }

    public mutating func reset() {
        running = false
        startMs = 0
        laps = []
        finishedS = nil
    }
}
