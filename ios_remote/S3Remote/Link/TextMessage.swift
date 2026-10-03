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

        default:
            return nil
        }
    }
}
