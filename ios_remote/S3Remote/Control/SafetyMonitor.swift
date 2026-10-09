/*
 * SafetyMonitor.swift — port of scr_ctrl.c safety_watch (doc/05-ctrl.md §4):
 * radio-lost watch with 1.2 s debounce (SCR_ALERT_DEBOUNCE_MS) — presented
 * as the non-blocking top banner, not a full-screen alert —, vehicle fault
 * watch, battery low/critical watch with +5 % hysteresis.
 * Pure logic; the caller polls at the 30 Hz control beat.
 */

import Foundation

public enum AlertKind: Equatable, Sendable {
    case emergency
    case radioLost
    case vehicleFault
    case criticalBattery
    case lowBattery
}

public enum AlertLevel: Sendable {
    case warning
    case critical
}

public struct Alert: Equatable, Sendable {
    public var kind: AlertKind
    public var level: AlertLevel
    public var detail: String

    public init(kind: AlertKind, level: AlertLevel, detail: String = "") {
        self.kind = kind
        self.level = level
        self.detail = detail
    }
}

public struct SafetyMonitor: Sendable {
    public var battLowPct: Int = 20
    public var battCritPct: Int = 10
    public var debounceMs: Double = 1200

    private var lossSinceMs: Double?

    public init() {}

    /// Radio watch: alert only after the link+telemetry have been down for
    /// debounceMs continuously; clears immediately on recovery. Returns true
    /// while the sustained-loss alert is active.
    public mutating func radioLostActive(nowMs: Double, linkUp: Bool, teleFresh: Bool) -> Bool {
        let ok = linkUp && teleFresh
        if ok {
            lossSinceMs = nil
            return false
        }
        if let since = lossSinceMs {
            return (nowMs - since) >= debounceMs
        }
        lossSinceMs = nowMs
        return false
    }

    /// Battery watch with recovery hysteresis: alert at LOW(20)/CRIT(10),
    /// clears only when the level rises to LOW+5. Returns the active kind.
    public mutating func batteryWatch(_ pct: Int) -> AlertKind? {
        if let threshold = recoverAt {
            if pct >= threshold {
                recoverAt = nil
                return nil
            }
            return pct <= battCritPct ? .criticalBattery : .lowBattery
        }
        if pct <= battCritPct {
            recoverAt = battLowPct + 5
            return .criticalBattery
        }
        if pct <= battLowPct {
            recoverAt = battLowPct + 5
            return .lowBattery
        }
        return nil
    }

    private var recoverAt: Int?

    /// Fault watch: active while fault_code != 0.
    public static func faultActive(_ code: UInt16) -> Bool {
        code != 0
    }
}
