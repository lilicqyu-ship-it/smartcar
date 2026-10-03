/*
 * TelemetryTests.swift — golden 38 B payload (independent python3 encoding),
 * offset assertions, encode↔decode roundtrip incl. negative speeds and
 * u32 boundaries.
 */

import XCTest
@testable import S3Remote

final class TelemetryTests: XCTestCase {
    // python3-encoded golden payload: seq=1000, uptime=60000, state=3,
    // fault=0x0002, vtL=500, vtR=-500, vmL=480, vmR=-479, batt=11000 mV/85 %,
    // odo 12345/999999, rtt=12, err=7, fw=0x00010500, hw=2
    static let goldenHex = "e803000060ea0000030200f4010cfee00121fef82a55393000003f420f000c00070005010002"

    private var goldenBytes: [UInt8] {
        var bytes: [UInt8] = []
        var i = Self.goldenHex.startIndex
        while i < Self.goldenHex.endIndex {
            let next = Self.goldenHex.index(i, offsetBy: 2)
            bytes.append(UInt8(Self.goldenHex[i..<next], radix: 16)!)
            i = next
        }
        return bytes
    }

    func testGoldenDecode() {
        let t = Telemetry.decode(goldenBytes)
        XCTAssertNotNil(t)
        XCTAssertEqual(t?.seq, 1000)
        XCTAssertEqual(t?.uptimeMs, 60000)
        XCTAssertEqual(t?.state, 3)
        XCTAssertEqual(t?.faultCode, 0x0002)
        XCTAssertEqual(t?.vTargetL, 500)
        XCTAssertEqual(t?.vTargetR, -500)
        XCTAssertEqual(t?.vMeasL, 480)
        XCTAssertEqual(t?.vMeasR, -479)
        XCTAssertEqual(t?.batteryMv, 11000)
        XCTAssertEqual(t?.batteryPct, 85)
        XCTAssertEqual(t?.odoSessionMm, 12345)
        XCTAssertEqual(t?.odoTotalMm, 999999)
        XCTAssertEqual(t?.linkRttMs, 12)
        XCTAssertEqual(t?.linkErrRate, 7)
        XCTAssertEqual(t?.fwVer, 0x00010500)
        XCTAssertEqual(t?.fwVerText, "v1.5.0")
        XCTAssertEqual(t?.hwRev, 2)
    }

    func testGoldenSurvivesFullFrameParse() {
        let frame = ProtoFrame(cmd: Proto.Cmd.telemetry, seq: 0, data: goldenBytes)
        var parser = ProtoParser()
        guard case .frame(let f) = parser.feed(frame.encode()).last,
              let t = Telemetry.decode(f.data) else {
            return XCTFail("no telemetry frame")
        }
        XCTAssertEqual(t.seq, 1000)
        XCTAssertEqual(t.batteryPct, 85)
    }

    func testFieldOffsets() {
        var t = Telemetry()
        t.seq = 0x01020304
        t.uptimeMs = 0x11121314
        t.state = 0x55
        t.faultCode = 0xBEEF
        let d = Telemetry.encode(t)
        XCTAssertEqual(d.count, 38)
        XCTAssertEqual(Array(d[0..<4]), [0x04, 0x03, 0x02, 0x01])
        XCTAssertEqual(Array(d[4..<8]), [0x14, 0x13, 0x12, 0x11])
        XCTAssertEqual(d[8], 0x55)
        XCTAssertEqual(Array(d[9..<11]), [0xEF, 0xBE]) // LE u16
        XCTAssertEqual(d[21], 0) // battery_pct placeholder position
    }

    func testRoundtripNegativeAndBounds() {
        var t = Telemetry()
        t.seq = .max
        t.uptimeMs = .max
        t.vTargetL = -32768
        t.vTargetR = 32767
        t.vMeasL = -1
        t.vMeasR = 1
        t.batteryMv = .max
        t.odoSessionMm = .max
        t.odoTotalMm = .max
        t.linkRttMs = .max
        let back = Telemetry.decode(Telemetry.encode(t))
        XCTAssertEqual(back, t)
    }

    func testDecodeRejectsShort() {
        XCTAssertNil(Telemetry.decode([UInt8](repeating: 0, count: 37)))
    }
}
