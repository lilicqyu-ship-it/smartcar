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
/// The C6 has always broadcast the full 28-byte fusion snapshot (12 fields,
/// wire truth tc275_car app/fusion.c FUSION_encode) — the remote simply
/// never parsed the attitude/speed half before the sensor tab needed it.
public struct FusionStatus: Equatable, Sendable {
    public let reason: Int
    public let flags: Int
    public let distance: Int
    public let cap: Int
    // Remaining snapshot fields; 0 = unknown (older C6 that sends only the
    // original four). Display-only.
    public var speedMmS: Int = 0
    public var yawRateCdegS: Int = 0
    public var headingCdeg: Int = 0
    public var rollCdeg: Int = 0
    public var pitchCdeg: Int = 0
    public var tofAgeMs: Int = 0
    public var validZones: Int = 0
    public var brake: Int = 0

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

/// Raw CPU1 IMU sample forwarded by C6 as the optional {"t":"imu"} JSON
/// stream (EVT 0x2B, wire truth tc275_car app/sensor_stream.h). Values are
/// the car's body frame straight from the LSM6DSV16BX: acc in milligee,
/// gyro in millideg/s, die temperature in 0.01 °C units. ~20 Hz.
public struct ImuSample: Equatable, Sendable {
    public let seq: Int
    public let stampMs: Int
    public let accMg: [Int] // [x, y, z]
    public let gyroMdps: [Int] // [x, y, z]
    public let tempCentiC: Int
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
    /// {"t":"cal",...} — TC275 判向标定结果（EVT 0x22 经 C6 bridge 转发）
    case cal(CalibResult)
    /// {"t":"rec",...} — 当前生效标定记录（EVT 0x23）
    case rec(CalibRecord)
    /// {"t":"jogcnt",...} — 点动期间逐通道编码器计数增量（EVT 0x26，10 Hz）
    case jogCnt(on: Bool, deltas: [Int])
    /// {"t":"imu",...} — 车端原始 IMU 采样（EVT 0x2B，20 Hz）
    case imu(ImuSample)
    /// {"t":"tofz",...} — ToF 8×8 zone 图的一个分片（EVT 0x2A，随 15 Hz 帧
    /// 每帧 3 片；25/25/14 区，cell = mm/16，0xFF = 无可信目标）。
    /// 由 TofZoneAssembler 按 {seq, frag} 重组。
    case tofFragment(seq: Int, frag: Int, mode: Int, valid: Int, nearestMm: Int, zones: [Int])

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
            // 姿态/速度半边：i16/u16 量化字段，越界即整帧丢弃（不造数）。
            func i16(_ key: String) -> Int? {
                guard let v = obj[key] as? Int, (-32_768...32_767).contains(v) else { return nil }
                return v
            }
            func u16(_ key: String) -> Int? {
                guard let v = obj[key] as? Int, (0...65_535).contains(v) else { return nil }
                return v
            }
            func u8(_ key: String) -> Int? {
                guard let v = obj[key] as? Int, (0...255).contains(v) else { return nil }
                return v
            }
            guard let speed = i16("speed"), let yawRate = i16("yawRate"),
                  let heading = i16("heading"), let roll = i16("roll"),
                  let pitch = i16("pitch"), let age = u16("age"),
                  let zones = u8("zones"), let brake = u8("brake") else { return nil }
            return .fusion(FusionStatus(reason: reason, flags: flags, distance: distance, cap: cap,
                                        speedMmS: speed, yawRateCdegS: yawRate,
                                        headingCdeg: heading, rollCdeg: roll, pitchCdeg: pitch,
                                        tofAgeMs: age, validZones: zones, brake: brake))

        case "cal":
            // bridge_emit_cal: status 必有；saved V1.1 起才有（缺失=待确认）；
            // invert/delta 恒 4 元素。畸形载荷整体丢弃，不造数。
            guard let status = obj["status"] as? Int, (0...255).contains(status),
                  let invert = intArray(obj["invert"], count: 4),
                  let delta = intArray(obj["delta"], count: 4) else { return nil }
            let saved = (obj["saved"] as? NSNumber).flatMap { n in
                let v = n.intValue
                return (0...2).contains(v) ? v : nil
            }
            return .cal(CalibResult(status: status, saved: saved, invert: invert, delta: delta))

        case "rec":
            // bridge_emit_rec: ver/src/pos/invert/fullScale/wheelDia/crcOk 全必填
            guard let ver = obj["ver"] as? Int, (0...255).contains(ver),
                  let src = obj["src"] as? Int, (0...255).contains(src),
                  let pos = intArray(obj["pos"], count: 4),
                  let invert = intArray(obj["invert"], count: 4),
                  let fullScale = obj["fullScale"] as? Int,
                  let wheelDia = obj["wheelDia"] as? Int,
                  let crcOk = (obj["crcOk"] as? NSNumber)?.boolValue else { return nil }
            return .rec(CalibRecord(ver: ver, src: src, pos: pos, invert: invert,
                                    fullScaleMmS: fullScale, wheelDiaMm: wheelDia,
                                    crcOk: crcOk,
                                    imuAxis: intArray(obj["imuAxis"], count: 3) ?? [0, 0, 0],
                                    trackMm: obj["trackMm"] as? Int ?? 0,
                                    imuSaved: obj["imuSaved"] as? Int,
                                    wheelCalibrated: (obj["wheelCalibrated"] as? NSNumber)?.boolValue))

        case "jogcnt":
            // bridge_emit_jogcnt: on 0/1，d = 4 × i32 LE
            guard let on = (obj["on"] as? NSNumber)?.intValue, (0...1).contains(on),
                  let deltas = intArray(obj["d"], count: 4) else { return nil }
            return .jogCnt(on: on == 1, deltas: deltas)

        case "imu":
            // bridge_emit_imu: seq/ms 无符号，acc/gyro 恒 3 元素（可为负）
            guard let seq = obj["seq"] as? Int, seq >= 0,
                  let ms = obj["ms"] as? Int, ms >= 0,
                  let acc = intArray(obj["acc"], count: 3),
                  let gyro = intArray(obj["gyro"], count: 3),
                  let tp = obj["tp"] as? Int else { return nil }
            return .imu(ImuSample(seq: seq, stampMs: ms, accMg: acc,
                                  gyroMdps: gyro, tempCentiC: tp))

        case "tofz":
            // bridge_emit_tofz: seq u16、frag 0..2、mode 0..3、valid 0..64、
            // near u16、z 恰 25 个 0..255 cell。畸形片整体丢弃，不造数。
            guard let seq = obj["seq"] as? Int, (0...65_535).contains(seq),
                  let frag = obj["f"] as? Int, (0...2).contains(frag),
                  let mode = obj["m"] as? Int, (0...3).contains(mode),
                  let valid = obj["v"] as? Int, (0...64).contains(valid),
                  let near = obj["near"] as? Int, (0...65_535).contains(near),
                  let zones = byteCellArray(obj["z"], count: 25) else { return nil }
            return .tofFragment(seq: seq, frag: frag, mode: mode, valid: valid,
                                nearestMm: near, zones: zones)

        default:
            return nil
        }
    }

    /// JSON number array → [Int]，恰好 count 个元素才算有效。
    private static func intArray(_ any: Any?, count: Int) -> [Int]? {
        guard let nums = any as? [NSNumber], nums.count == count else { return nil }
        return nums.map { $0.intValue }
    }

    /// tofz 的 zone cell：0...255 的整数数组（0xFF 合法 = 无目标）。
    private static func byteCellArray(_ any: Any?, count: Int) -> [Int]? {
        guard let nums = any as? [NSNumber], nums.count == count else { return nil }
        var cells = [Int]()
        cells.reserveCapacity(count)
        for n in nums {
            let v = n.intValue
            guard (0...255).contains(v) else { return nil }
            cells.append(v)
        }
        return cells
    }
}
