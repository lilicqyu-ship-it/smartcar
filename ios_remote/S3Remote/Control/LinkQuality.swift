/*
 * LinkQuality.swift — signal strength model.
 *
 * Real RSSI path: the gateway may include per-client RSSI in the WS text
 * plane (hello "rssi" or {"t":"rssi","dbm":N}); grading uses the same
 * thresholds as the S3 remote's Kconfig (spec 26: −60/−67/−75/−85 dBm,
 * bench defaults until field-calibrated).
 *
 * Estimated path: iOS public APIs cannot read Wi-Fi RSSI, so until the
 * gateway reports it, bars are synthesized from RTT + telemetry loss —
 * labeled "预估" in the UI. Thresholds are bench defaults too.
 */

import Foundation

public enum LinkQuality {
    // S3 remote Kconfig parity (SCR_RSSI_EXCELLENT/GOOD/FAIR/WEAK)
    public static let rssiExcellent = -60
    public static let rssiGood = -67
    public static let rssiFair = -75
    public static let rssiWeak = -85

    /// 4 bars = EXCELLENT … 1 bar = WEAK or below.
    public static func bars(forRssi rssi: Int) -> Int {
        if rssi >= rssiExcellent { return 4 }
        if rssi >= rssiGood { return 3 }
        if rssi >= rssiFair { return 2 }
        return 1
    }

    public static func label(forRssi rssi: Int) -> String {
        if rssi >= rssiExcellent { return "优" }
        if rssi >= rssiGood { return "良" }
        if rssi >= rssiFair { return "中" }
        if rssi >= rssiWeak { return "弱" }
        return "极弱"
    }

    /// Composite estimate when no RSSI field exists: RTT-driven base,
    /// penalized by telemetry loss.
    public static func estimatedBars(rttMs: Int, lossPerMille: Double) -> Int {
        var base: Int
        if rttMs <= 0 {
            base = 1 // no RTT sample yet
        } else if rttMs <= 40 {
            base = 4
        } else if rttMs <= 80 {
            base = 3
        } else if rttMs <= 150 {
            base = 2
        } else {
            base = 1
        }
        if lossPerMille > 300 {
            base = 1
        } else if lossPerMille > 100 {
            base = min(base, 2)
        }
        return base
    }

    public static func estimatedLabel(bars: Int) -> String {
        switch bars {
        case 4: "优"
        case 3: "良"
        case 2: "中"
        case 1: "弱"
        default: "极弱"
        }
    }
}
