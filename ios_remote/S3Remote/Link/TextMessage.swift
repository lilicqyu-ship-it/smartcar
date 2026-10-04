/*
 * TextMessage.swift — JSON text plane over the same /ws WebSocket, port of
 * scr_link.c ws_handle_text(). Wire format verified against
 * esp32c6_car/components/c6_http/http_server.c ws_send_hello():
 *   {"t":"hello","role":"ctrl|spectator","ver":"..","tc":"down|up","pair":"idle|open|claimed","ctrl":true|false}
 *   {"t":"tc","on":true|false} / {"t":"pong"} / {"t":"err","e":"auth"} / {"t":"tcver","app":..,"sbl":..}
 */

import Foundation

public enum HelloRole: Equatable, Sendable {
    case ctrl
    case spectator
    case other(String)
}

public enum PairState: String, Equatable, Sendable {
    case idle
    case open
    case claimed
}

/// CPU0 driving guard, forwarded by C6 as the optional fusion JSON beacon.
public struct FusionStatus: Equatable, Sendable {
    public let reason: Int
    public let flags: Int
    public let distance: Int
    public let cap: Int

    public var warning: String? {
        if reason == 4 { return "倾斜保护停车，请扶正车辆" }
        if reason == 5 { return "车速反馈未就绪，前进暂停" }
        if flags & 64 != 0 { return "前向保护停车 · 松开油门后再试" }
        if flags & 129 == 0 { return "前方测距已失效，前进暂停" }
        if cap == 0 { return "检测到近距离障碍，前进暂停" }
        if flags & 128 != 0 {
            return String(format: "测距覆盖不足 · 前进限速 %.2f km/h", Double(cap) * 0.0036)
        }
        if reason == 1 { return "接近障碍，已限制前进速度" }
        return nil
    }
}

public enum TextMessage: Equatable, Sendable {
    /// - Parameters:
    ///   - ctrlHeld: hello.ctrl — SOME session holds CTRL (not necessarily us).
    ///   - rssi: optional per-client RSSI from the gateway (dBm), when present.
    case hello(role: HelloRole, ver: String, tcUp: Bool, pair: PairState?, ctrlHeld: Bool, rssi: Int?)
    case tc(on: Bool)
    case pong
    case errAuth
    case tcVer(app: String, sbl: String)
    /// {"t":"rssi","dbm":N} — optional periodic signal-strength beacon.
    case rssi(dbm: Int)
    case fusion(FusionStatus)

    public static func parse(_ data: Data) -> TextMessage? {
        guard
            let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
            let t = obj["t"] as? String
        else { return nil }

        switch t {
        case "hello":
            let role: HelloRole
            switch obj["role"] as? String {
            case "ctrl": role = .ctrl
            case "spectator": role = .spectator
            case .some(let s): role = .other(s)
            case .none: return nil
            }
            // tc arrives as a string placeholder on older C6, as bool elsewhere
            let tcUp: Bool
            if let b = obj["tc"] as? Bool { tcUp = b }
            else { tcUp = (obj["tc"] as? String) == "up" }
            let ver = obj["ver"] as? String ?? ""
            let pair = PairState(rawValue: obj["pair"] as? String ?? "")
            let ctrlHeld = (obj["ctrl"] as? Bool) == true
            let rssi = (obj["rssi"] as? NSNumber)?.intValue
            return .hello(role: role, ver: ver, tcUp: tcUp, pair: pair, ctrlHeld: ctrlHeld, rssi: rssi)

        case "tc":
            return .tc(on: (obj["on"] as? Bool) == true)

        case "pong":
            return .pong

        case "err":
            guard (obj["e"] as? String) == "auth" else { return nil }
            return .errAuth

        case "tcver":
            return .tcVer(app: obj["app"] as? String ?? "", sbl: obj["sbl"] as? String ?? "")

        case "rssi":
            guard let dbm = (obj["dbm"] as? NSNumber)?.intValue else { return nil }
            return .rssi(dbm: dbm)

        case "fusion":
            guard let reason = obj["reason"] as? Int, (0...5).contains(reason),
                  let flags = obj["flags"] as? Int, (0...65535).contains(flags),
                  let distance = obj["distance"] as? Int, (0...65535).contains(distance),
                  let cap = obj["cap"] as? Int, (0...65535).contains(cap) else { return nil }
            return .fusion(FusionStatus(reason: reason, flags: flags, distance: distance, cap: cap))

        default:
            return nil
        }
    }
}
