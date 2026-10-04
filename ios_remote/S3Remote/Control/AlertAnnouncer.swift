/*
 * AlertAnnouncer.swift — VoiceOver announcements for full-screen alerts.
 * AppState owns one and fires it whenever an alert KIND appears, so blind
 * users hear "紧急停止" instead of staring at a silent overlay. The closure
 * is injected (UIAccessibility at the call site, recorder in tests).
 */

import Foundation

/// MainActor-confined by usage (AppState.safetyTick); not Sendable so the
/// announce closure may call MainActor-isolated UIKit directly.
public struct AlertAnnouncer {
    public let announce: (String) -> Void

    public init(announce: @escaping (String) -> Void) {
        self.announce = announce
    }

    public func alertAppeared(_ alert: Alert) {
        let detail = alert.detail.isEmpty ? "" : "\(alert.detail)。"
        announce("\(alert.kind.title)。\(detail)")
    }
}

public extension AlertKind {
    /// Spoken/display title per kind — the AlertOverlayView uses the same copy.
    var title: String {
        switch self {
        case .emergency: return "紧急停止"
        case .radioLost: return "连接已中断"
        case .vehicleFault: return "车辆需要检查"
        case .criticalBattery: return "电量即将耗尽"
        case .lowBattery: return "电量偏低"
        }
    }
}
