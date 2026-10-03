/*
 * Telemetry.swift — 0x41 TELEMETRY payload (38 B, explicit LE), port of
 * proto_telemetry_t encode/decode. Field offsets must stay byte-identical
 * with smartcar_remote/main/proto/proto_frames.h.
 */

import Foundation

public struct Telemetry: Equatable, Sendable {
    public var seq: UInt32 = 0
    public var uptimeMs: UInt32 = 0
    public var state: UInt8 = 0
    public var faultCode: UInt16 = 0
    public var vTargetL: Int16 = 0
    public var vTargetR: Int16 = 0
    public var vMeasL: Int16 = 0
    public var vMeasR: Int16 = 0
    public var batteryMv: UInt16 = 0
    public var batteryPct: UInt8 = 0
    public var odoSessionMm: UInt32 = 0
    public var odoTotalMm: UInt32 = 0
    public var linkRttMs: UInt16 = 0
    public var linkErrRate: UInt8 = 0 // 0.1 % units
    public var fwVer: UInt32 = 0      // 0x00MMmmpp
    public var hwRev: UInt8 = 0

    public static let payloadLen = 38

    public init() {}

    /// 0x00MMmmpp → "v1.5.0"
    public var fwVerText: String {
        String(format: "v%d.%d.%d", (fwVer >> 16) & 0xFF, (fwVer >> 8) & 0xFF, fwVer & 0xFF)
    }

    public static func decode(_ d: [UInt8]) -> Telemetry? {
        guard d.count >= payloadLen else { return nil }
        var t = Telemetry()
        t.seq = Wire.getU32(d, 0)
        t.uptimeMs = Wire.getU32(d, 4)
        t.state = d[8]
        t.faultCode = Wire.getU16(d, 9)
        t.vTargetL = Wire.getI16(d, 11)
        t.vTargetR = Wire.getI16(d, 13)
        t.vMeasL = Wire.getI16(d, 15)
        t.vMeasR = Wire.getI16(d, 17)
        t.batteryMv = Wire.getU16(d, 19)
        t.batteryPct = d[21]
        t.odoSessionMm = Wire.getU32(d, 22)
        t.odoTotalMm = Wire.getU32(d, 26)
        t.linkRttMs = Wire.getU16(d, 30)
        t.linkErrRate = d[32]
        t.fwVer = Wire.getU32(d, 33)
        t.hwRev = d[37]
        return t
    }

    public static func encode(_ t: Telemetry) -> [UInt8] {
        var d = [UInt8](repeating: 0, count: payloadLen)
        d.replaceSubrange(0..<4, with: Wire.putU32(t.seq))
        d.replaceSubrange(4..<8, with: Wire.putU32(t.uptimeMs))
        d[8] = t.state
        d.replaceSubrange(9..<11, with: Wire.putU16(t.faultCode))
        d.replaceSubrange(11..<13, with: Wire.putI16(t.vTargetL))
        d.replaceSubrange(13..<15, with: Wire.putI16(t.vTargetR))
        d.replaceSubrange(15..<17, with: Wire.putI16(t.vMeasL))
        d.replaceSubrange(17..<19, with: Wire.putI16(t.vMeasR))
        d.replaceSubrange(19..<21, with: Wire.putU16(t.batteryMv))
        d[21] = t.batteryPct
        d.replaceSubrange(22..<26, with: Wire.putU32(t.odoSessionMm))
        d.replaceSubrange(26..<30, with: Wire.putU32(t.odoTotalMm))
        d.replaceSubrange(30..<32, with: Wire.putU16(t.linkRttMs))
        d[32] = t.linkErrRate
        d.replaceSubrange(33..<37, with: Wire.putU32(t.fwVer))
        d[37] = t.hwRev
        return d
    }
}
