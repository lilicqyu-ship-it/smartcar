/*
 * ProtoTests.swift — G1 gate port: CRC check vector 0x29B1, golden frames
 * (precomputed with an independent Python implementation of the C codec),
 * encode↔parser roundtrip, resync and error paths.
 */

import XCTest
@testable import S3Remote

final class ProtoTests: XCTestCase {
    func testCrcCheckVector() {
        // check("123456789") == 0x29B1 — same assertion as the C host tests
        let bytes = Array("123456789".utf8)
        XCTAssertEqual(Proto.crc16(bytes), 0x29B1)
    }

    func testCrcMatchesCLinkage() {
        // CRC over the DRIVE frame header+payload, computed by python3
        let bytes: [UInt8] = [0xAA, 0x55, 0x02, 0x50, 0x01, 0x04, 0x58, 0x02, 0x00, 0x00]
        XCTAssertEqual(Proto.crc16(bytes), 0xE425)
    }

    func testDriveGoldenFrame() {
        // DRIVE v=600 w=0, seq=1 — byte-exact against the C codec
        let frame = ProtoFrame(cmd: Proto.Cmd.drive, seq: 1,
                               data: Wire.putI16(600) + Wire.putI16(0))
        XCTAssertEqual(Array(frame.encode()),
                       [0xAA, 0x55, 0x02, 0x50, 0x01, 0x04, 0x58, 0x02, 0x00, 0x00, 0xE4, 0x25])
    }

    func testEmergencyStopGoldenFrame() {
        let frame = ProtoFrame(cmd: Proto.Cmd.emergencyStop, seq: 2)
        XCTAssertEqual(Array(frame.encode()),
                       [0xAA, 0x55, 0x02, 0x32, 0x02, 0x00, 0x7F, 0x90])
    }

    func testEncodeParseRoundtrip() {
        var parser = ProtoParser()
        let payload = (0..<64).map { UInt8($0 & 0xFF) }
        let wire = ProtoFrame(cmd: 0x77, seq: 0xAB, data: payload).encode()
        let events = parser.feed(wire)
        guard case .frame(let f) = events.last else {
            return XCTFail("no frame")
        }
        XCTAssertEqual(f.cmd, 0x77)
        XCTAssertEqual(f.seq, 0xAB)
        XCTAssertEqual(f.data, payload)
    }

    func testSplitDeliveryRoundtrip() {
        var parser = ProtoParser()
        let wire = ProtoFrame.build(cmd: Proto.Cmd.drive, seq: 9,
                                    data: Wire.putI16(-1234) + Wire.putI16(567))
        var got: ProtoFrame?
        for b in [UInt8](wire) { // one byte at a time
            if case .frame(let f) = parser.feed(b) { got = f }
        }
        XCTAssertEqual(got?.data, Wire.putI16(-1234) + Wire.putI16(567))
    }

    func testGarbageThenResync() {
        var parser = ProtoParser()
        let events = parser.feed(Data([0x00, 0x13, 0xAA, 0xAA, 0x37, 0x13]))
            + parser.feed(ProtoFrame.build(cmd: 0x01, seq: 1, data: [0x42]))
        XCTAssertTrue(events.contains { if case .frame(let f) = $0 { return f.data == [0x42] } ; return false })
    }

    func testVersionError() {
        var parser = ProtoParser()
        let bad: [UInt8] = [0xAA, 0x55, 0x03, 0x50, 0x00, 0x00, 0x00, 0x00]
        XCTAssertTrue(eventsContain(parser.feed(Data(bad))) { if case .versionError = $0 { return true }; return false })
    }

    func testFormatErrorLenOver64() {
        var parser = ProtoParser()
        var bad: [UInt8] = [0xAA, 0x55, 0x02, 0x50, 0x00, 65]
        bad += [UInt8](repeating: 0, count: 10)
        XCTAssertTrue(eventsContain(parser.feed(Data(bad))) { if case .formatError = $0 { return true }; return false })
    }

    func testCrcErrorOnTamper() {
        var parser = ProtoParser()
        var wire = [UInt8](ProtoFrame.build(cmd: 0x50, seq: 1, data: [1, 2, 3, 4]))
        wire[wire.count - 1] ^= 0xFF
        XCTAssertTrue(eventsContain(parser.feed(Data(wire))) { if case .crcError = $0 { return true }; return false })
    }

    func testAaAaSyncTolerance() {
        // AA AA 55 stream still parses (resync rule from the C state machine)
        var parser = ProtoParser()
        let events = parser.feed(Data([0xAA])) + parser.feed(Data([0xAA]))
            + parser.feed(ProtoFrame.build(cmd: 0x02, seq: 3, data: []))
        XCTAssertTrue(eventsContain(events) { if case .frame(let f) = $0 { return f.cmd == 0x02 && f.seq == 3 }; return false })
    }

    private func eventsContain(_ events: [ProtoRxEvent], _ p: (ProtoRxEvent) -> Bool) -> Bool {
        events.contains(where: p)
    }
}
