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

    func testFusionLimitedCoverageWarning() {
        guard case .fusion(let status) = parse(#"{"t":"fusion","reason":1,"flags":166,"distance":1280,"cap":150}"#) else {
            return XCTFail("missing driving guard")
        }
        XCTAssertEqual(status.warning, "测距覆盖不足 · 前进限速 0.54 km/h")
        XCTAssertNil(parse(#"{"t":"fusion","reason":1,"flags":166,"distance":1280}"#))
        XCTAssertNil(parse(#"{"t":"fusion","reason":1,"flags":166,"distance":1280,"cap":-1}"#))
        XCTAssertEqual(FusionStatus(reason: 2, flags: 192, distance: 100, cap: 0).warning,
                       "前向保护停车 · 松开油门后再试")
        XCTAssertEqual(FusionStatus(reason: 3, flags: 6, distance: 0, cap: 0).warning,
                       "前方测距已失效，前进暂停")
        XCTAssertEqual(FusionStatus(reason: 4, flags: 192, distance: 0, cap: 0).warning,
                       "倾斜保护停车，请扶正车辆")
        XCTAssertNil(FusionStatus(reason: 0, flags: 7, distance: 1000, cap: 600).warning)
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
