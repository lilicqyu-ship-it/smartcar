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

    // ---- TC275 标定回执（c6_bridge bridge_emit_cal / _rec / _jogcnt）-----------

    func testCalDoneWithSaved() {
        let json = #"{"t":"cal","status":0,"saved":1,"invert":[-1,1,-1,1],"delta":[-1204,331,0,987]}"#
        guard case .cal(let r) = parse(json) else { return XCTFail("not cal") }
        XCTAssertEqual(r.status, 0)
        XCTAssertEqual(r.saved, 1)
        XCTAssertEqual(r.invert, [-1, 1, -1, 1])
        XCTAssertEqual(r.delta, [-1204, 331, 0, 987])
        XCTAssertEqual(r.statusText, "标定完成")
        XCTAssertEqual(r.savedText, "已写入 DFlash")
    }

    func testCalAbortAndBusy() {
        guard case .cal(let abort) = parse(#"{"t":"cal","status":1,"saved":0,"invert":[1,1,1,1],"delta":[100,0,0,0]}"#) else {
            return XCTFail()
        }
        XCTAssertEqual(abort.statusText, "急停中止")
        guard case .cal(let busy) = parse(#"{"t":"cal","status":2,"saved":0,"invert":[1,1,1,1],"delta":[0,0,0,0]}"#) else {
            return XCTFail()
        }
        XCTAssertEqual(busy.statusText, "忙（已有标定进行中）")
    }

    func testCalSavedMissingMeansPending() {
        // 旧固件（M2a 前的 22 B 帧）没有 saved 字段 → 待确认，不是失败
        let json = #"{"t":"cal","status":0,"invert":[1,1,1,1],"delta":[1,2,3,4]}"#
        guard case .cal(let r) = parse(json) else { return XCTFail() }
        XCTAssertNil(r.saved)
        XCTAssertEqual(r.savedText, "保存状态待确认")
        XCTAssertFalse(r.savedBad)
    }

    func testCalSavedOutOfRangeIsPending() {
        let json = #"{"t":"cal","status":0,"saved":9,"invert":[1,1,1,1],"delta":[1,2,3,4]}"#
        guard case .cal(let r) = parse(json) else { return XCTFail() }
        XCTAssertNil(r.saved)
    }

    func testCalMalformedDropped() {
        XCTAssertNil(parse(#"{"t":"cal","status":0,"saved":1,"invert":[1,1,1],"delta":[1,2,3,4]}"#)) // invert 短
        XCTAssertNil(parse(#"{"t":"cal","invert":[1,1,1,1],"delta":[1,2,3,4]}"#))                    // 缺 status
        XCTAssertNil(parse(#"{"t":"cal"}"#))
    }

    func testRecParse() {
        // bridge_emit_rec：15 B EVT 0x23 的逐字段 JSON
        let json = #"{"t":"rec","ver":1,"src":1,"pos":[0,2,3,1],"invert":[-1,-1,1,1],"fullScale":1000,"wheelDia":48,"crcOk":1}"#
        guard case .rec(let rec) = parse(json) else { return XCTFail("not rec") }
        XCTAssertEqual(rec.ver, 1)
        XCTAssertEqual(rec.src, 1)
        XCTAssertEqual(rec.pos, [0, 2, 3, 1])
        XCTAssertEqual(rec.invert, [-1, -1, 1, 1])
        XCTAssertEqual(rec.fullScaleMmS, 1000)
        XCTAssertEqual(rec.wheelDiaMm, 48)
        XCTAssertTrue(rec.crcOk)
        XCTAssertEqual(rec.srcText, "DFlash")
    }

    func testRecCrcFailAndDefaults() {
        let json = #"{"t":"rec","ver":1,"src":0,"pos":[0,2,3,1],"invert":[1,1,1,1],"fullScale":1000,"wheelDia":48,"crcOk":0}"#
        guard case .rec(let rec) = parse(json) else { return XCTFail() }
        XCTAssertFalse(rec.crcOk)
        XCTAssertEqual(rec.srcText, "默认值")
    }

    func testRecMalformedDropped() {
        XCTAssertNil(parse(#"{"t":"rec","ver":1,"src":1,"pos":[0,2,3],"invert":[1,1,1,1],"fullScale":1000,"wheelDia":48,"crcOk":1}"#))
        XCTAssertNil(parse(#"{"t":"rec","ver":1,"src":1,"pos":[0,2,3,1],"invert":[1,1,1,1],"fullScale":1000,"crcOk":1}"#)) // 缺 wheelDia
    }

    func testJogCntParse() {
        let json = #"{"t":"jogcnt","on":1,"d":[12,-3,0,456]}"#
        guard case .jogCnt(let on, let deltas) = parse(json) else { return XCTFail("not jogcnt") }
        XCTAssertTrue(on)
        XCTAssertEqual(deltas, [12, -3, 0, 456])
        XCTAssertNil(parse(#"{"t":"jogcnt","on":1,"d":[1,2,3]}"#))
        XCTAssertNil(parse(#"{"t":"jogcnt","d":[1,2,3,4]}"#))
    }
}
