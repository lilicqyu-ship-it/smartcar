/*
 * LinkMessageTests.swift — JSON text plane parsing, verified against the
 * exact strings emitted by esp32c6_car/components/c6_http/http_server.c.
 */

import XCTest
@testable import S3Remote

final class LinkMessageTests: XCTestCase {
    private func parse(_ s: String) -> TextMessage? {
        TextMessage.parse(Data(s.utf8))
    }

    func testHelloCtrl() {
        let json = #"{"t":"hello","role":"ctrl","ver":"1.2.0","tc":"down","pair":"open","ctrl":true}"#
        guard case .hello(let role, let ver, let tcUp, let pair, let ctrlHeld, let rssi) = parse(json) else {
            return XCTFail("not hello")
        }
        XCTAssertEqual(role, .ctrl)
        XCTAssertEqual(ver, "1.2.0")
        XCTAssertFalse(tcUp) // hello carries the "down" placeholder
        XCTAssertEqual(pair, .open)
        XCTAssertTrue(ctrlHeld)
        XCTAssertNil(rssi) // legacy gateway: no RSSI field
    }

    func testHelloSpectator() {
        let json = #"{"t":"hello","role":"spectator","ver":"1.2.0","tc":"down","pair":"idle","ctrl":true}"#
        guard case .hello(let role, _, _, let pair, let ctrlHeld, _) = parse(json) else {
            return XCTFail("not hello")
        }
        XCTAssertEqual(role, .spectator)
        XCTAssertEqual(pair, .idle)
        XCTAssertTrue(ctrlHeld) // someone else (us after role loss) holds CTRL
    }

    func testHelloBoolTc() {
        let json = #"{"t":"hello","role":"ctrl","ver":"1","tc":true,"pair":"claimed","ctrl":false}"#
        guard case .hello(_, _, let tcUp, let pair, _, _) = parse(json) else {
            return XCTFail("not hello")
        }
        XCTAssertTrue(tcUp)
        XCTAssertEqual(pair, .claimed)
    }

    func testHelloWithRssi() {
        // future gateway: per-client RSSI travels inside hello
        let json = #"{"t":"hello","role":"ctrl","ver":"1.3","tc":"down","pair":"idle","ctrl":false,"rssi":-52}"#
        guard case .hello(let role, _, _, _, _, let rssi) = parse(json) else {
            return XCTFail("not hello")
        }
        XCTAssertEqual(role, .ctrl)
        XCTAssertEqual(rssi, -52)
    }

    func testRssiBeacon() {
        XCTAssertEqual(parse(#"{"t":"rssi","dbm":-70}"#), .rssi(dbm: -70))
        XCTAssertNil(parse(#"{"t":"rssi"}"#)) // missing dbm → ignore
    }

    func testTcMessage() {
        guard case .tc(let on) = parse(#"{"t":"tc","on":true}"#) else { return XCTFail() }
        XCTAssertTrue(on)
        guard case .tc(let off) = parse(#"{"t":"tc","on":false}"#) else { return XCTFail() }
        XCTAssertFalse(off)
    }

    func testPong() {
        XCTAssertEqual(parse(#"{"t":"pong"}"#), .pong)
    }

    func testErrAuth() {
        XCTAssertEqual(parse(#"{"t":"err","e":"auth"}"#), .errAuth)
        XCTAssertNil(parse(#"{"t":"err","e":"other"}"#))
    }

    func testTcVer() {
        guard case .tcVer(let app, let sbl) = parse(#"{"t":"tcver","app":"1.4.2","sbl":"0.9"}"#) else {
            return XCTFail()
        }
        XCTAssertEqual(app, "1.4.2")
        XCTAssertEqual(sbl, "0.9")
    }

    func testGarbageReturnsNil() {
        XCTAssertNil(parse("not json"))
        XCTAssertNil(parse(#"{"t":"unknown"}"#))
        XCTAssertNil(parse(#"{"no":"t"}"#))
    }
}
