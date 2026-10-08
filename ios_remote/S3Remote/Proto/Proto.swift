/*
 * Proto.swift — SmartDrive proto v2 codec, Swift port of
 * smartcar_remote/main/proto/proto_frames.h/.c (byte-exact semantics).
 *
 * Frame:  AA 55 VER=0x02 CMD SEQ LEN DATA[LEN<=64] CRC16
 * CRC16-CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB first, no reflection,
 * no final xor; check("123456789") == 0x29B1.
 */

import Foundation

public enum Proto {
    public static let sync1: UInt8 = 0xAA
    public static let sync2: UInt8 = 0x55
    public static let ver: UInt8 = 0x02
    public static let maxPayload = 64
    public static let headerLen = 6
    public static let maxFrame = headerLen + maxPayload + 2

    public enum Cmd {
        public static let stop: UInt8 = 0x01
        public static let emergencyStop: UInt8 = 0x32
        public static let telemetry: UInt8 = 0x41
        public static let linkState: UInt8 = 0x42
        public static let drive: UInt8 = 0x50
        public static let pair: UInt8 = 0x51
        public static let cfg: UInt8 = 0x52
        public static let diag: UInt8 = 0x53

        // DPT bench tool family (contracts/link/proto_frames.h §0x70-0x79):
        // C6 maps the whole range onto SF CMD/CID_DPT with payload[0]=op, so
        // the v2 payload here is the bare command body (no op byte).
        public static let dptCalDir: UInt8 = 0x70   // 编码器判向标定, ∅ → EVT 0x22
        public static let dptMotorJog: UInt8 = 0x71 // {motor u8, duty i16LE}
        public static let dptRecGet: UInt8 = 0x72   // ∅ → EVT 0x23
        public static let dptRecSet: UInt8 = 0x73   // 12 B {pos, invert, fs, wd}
        public static let dptRecClear: UInt8 = 0x74 // ∅ → EVT 0x23
        public static let dptImuCalSet: UInt8 = 0x75 // {axis i8x3, trackMm u16LE} → EVT 0x23
    }

    /// CRC16-CCITT-FALSE (proto_crc16).
    public static func crc16(_ bytes: [UInt8]) -> UInt16 {
        var crc: UInt16 = 0xFFFF
        for b in bytes {
            crc ^= UInt16(b) << 8
            for _ in 0..<8 {
                if crc & 0x8000 != 0 {
                    crc = (crc << 1) ^ 0x1021
                } else {
                    crc <<= 1
                }
            }
        }
        return crc
    }

    public static func crc16(_ data: Data) -> UInt16 {
        crc16([UInt8](data))
    }
}

// ---- explicit little-endian field helpers (proto_put/get_*) ----------------

public enum Wire {
    public static func putU16(_ v: UInt16) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8(v >> 8)]
    }

    public static func putU32(_ v: UInt32) -> [UInt8] {
        [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF), UInt8((v >> 16) & 0xFF), UInt8(v >> 24)]
    }

    public static func putI16(_ v: Int16) -> [UInt8] {
        putU16(UInt16(bitPattern: v))
    }

    public static func getU16(_ b: [UInt8], _ off: Int) -> UInt16 {
        UInt16(b[off]) | UInt16(b[off + 1]) << 8
    }

    public static func getU32(_ b: [UInt8], _ off: Int) -> UInt32 {
        UInt32(b[off]) | UInt32(b[off + 1]) << 8 | UInt32(b[off + 2]) << 16 | UInt32(b[off + 3]) << 24
    }

    public static func getI16(_ b: [UInt8], _ off: Int) -> Int16 {
        Int16(bitPattern: getU16(b, off))
    }
}

// ---- frame ------------------------------------------------------------------

public struct ProtoFrame: Equatable, Sendable {
    public var ver: UInt8
    public var cmd: UInt8
    public var seq: UInt8
    public var data: [UInt8]

    public init(ver: UInt8 = Proto.ver, cmd: UInt8, seq: UInt8, data: [UInt8] = []) {
        precondition(data.count <= Proto.maxPayload)
        self.ver = ver
        self.cmd = cmd
        self.seq = seq
        self.data = data
    }

    /// Serialize (proto_encode): header + data + CRC16 (big-endian on wire).
    public func encode() -> Data {
        var out = [UInt8]()
        out.reserveCapacity(Proto.maxFrame)
        out += [Proto.sync1, Proto.sync2, ver, cmd, seq, UInt8(data.count)]
        out += data
        let crc = Proto.crc16(out)
        out += [UInt8(crc >> 8), UInt8(crc & 0xFF)]
        return Data(out)
    }

    /// Convenience build+serialize (proto_build).
    public static func build(cmd: UInt8, seq: UInt8, data: [UInt8] = []) -> Data {
        ProtoFrame(cmd: cmd, seq: seq, data: data).encode()
    }
}

// ---- byte-wise parser (proto_parser_feed) -----------------------------------

public enum ProtoRxEvent: Equatable, Sendable {
    case none
    case frame(ProtoFrame)
    case crcError
    case formatError
    case versionError
}

public struct ProtoParser: Sendable {
    private enum State {
        case sync1, sync2, ver, cmd, seq, len, data, crcHi, crcLo
    }

    private var state: State = .sync1
    private var buf: [UInt8] = []
    private var need = 0

    public init() {}

    public mutating func reset() {
        state = .sync1
        buf.removeAll(keepingCapacity: true)
        need = 0
    }

    /// Feed one byte; returns an event when a frame completes or is rejected.
    public mutating func feed(_ byte: UInt8) -> ProtoRxEvent {
        switch state {
        case .sync1:
            if byte == Proto.sync1 { state = .sync2 }

        case .sync2:
            if byte == Proto.sync2 {
                state = .ver
                buf = [Proto.sync1, Proto.sync2]
            } else if byte == Proto.sync1 {
                // AA AA before 55: stay in sync2 (resync tolerance, as in C impl)
            } else {
                state = .sync1
            }

        case .ver:
            buf.append(byte)
            if byte == Proto.ver {
                state = .cmd
            } else {
                reset()
                return .versionError
            }

        case .cmd, .seq:
            buf.append(byte)
            state = state == .cmd ? .seq : .len

        case .len:
            buf.append(byte)
            need = Int(byte)
            if need > Proto.maxPayload {
                reset()
                return .formatError
            }
            state = need > 0 ? .data : .crcHi

        case .data:
            buf.append(byte)
            if buf.count >= Proto.headerLen + need { state = .crcHi }

        case .crcHi:
            buf.append(byte)
            state = .crcLo

        case .crcLo:
            buf.append(byte)
            let wireCrc = UInt16(buf[buf.count - 2]) << 8 | UInt16(buf[buf.count - 1])
            let computed = Proto.crc16(Array(buf[0..<(buf.count - 2)]))
            let frame = ProtoFrame(
                cmd: buf[3], seq: buf[4],
                data: Array(buf[Proto.headerLen..<(Proto.headerLen + need)]))
            reset()
            if wireCrc == computed { return .frame(frame) }
            return .crcError
        }
        return .none
    }

    /// Feed a byte stream, returning every event produced (byte order kept).
    public mutating func feed(_ bytes: Data) -> [ProtoRxEvent] {
        [UInt8](bytes).map { feed($0) }
    }
}
