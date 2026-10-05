/*
 * CalibSessionTests.swift — TC275 台架标定会话的纯逻辑回归：判向运行窗口
 * 与迟到回执、点动帧编码（对齐 CALIBREC_jogDecode）、REC_SET 载荷与校验、
 * recVsCalib 一致性判据。载荷布局对齐 tc275_car mw/calib/calib_record.c。
 */

import XCTest
@testable import S3Remote

final class CalibSessionTests: XCTestCase {
    // ---- jog 引擎 ------------------------------------------------------------

    func testJogPressPayloadMatchesWireContract() {
        var s = CalibSession()
        // 0x71 载荷 {motor u8, duty i16LE}：channel 2 前进 = +500 = 0x01F4 LE
        let f = s.jogPress(channel: 2, forward: true, nowMs: 1000)
        XCTAssertEqual(f?.payload, [0x02, 0xF4, 0x01])
        XCTAssertTrue(s.jogUsed)
    }

    func testJogBackwardAndReleaseZero() {
        var s = CalibSession()
        let press = s.jogPress(channel: 0, forward: false, nowMs: 0)
        XCTAssertEqual(press?.payload, [0x00, 0x0C, 0xFE]) // -500 = 0xFE0C → LE
        let release = s.jogRelease()
        XCTAssertEqual(release?.payload, [0x00, 0x00, 0x00])
        XCTAssertNil(s.jogKeepalive())
        XCTAssertNil(s.jogRelease()) // 幂等：无按压时不再发帧
    }

    func testJogKeepaliveResendsWhileHeld() {
        var s = CalibSession()
        _ = s.jogPress(channel: 1, forward: true, nowMs: 0)
        XCTAssertEqual(s.jogKeepalive()?.payload, [0x01, 0xF4, 0x01])
    }

    func testJogDutyClampedToPlusMinus500() {
        XCTAssertEqual(Array(JogFrame(channel: 0, duty: Int16(900)).payload.dropFirst()),
                       Wire.putI16(500))
        XCTAssertEqual(Array(JogFrame(channel: 0, duty: Int16(-900)).payload.dropFirst()),
                       Wire.putI16(-500))
    }

    func testJogInvalidChannelIgnored() {
        var s = CalibSession()
        XCTAssertNil(s.jogPress(channel: 4, forward: true, nowMs: 0))
        XCTAssertNil(s.jogChannel)
    }

    // ---- 判向运行窗口 -----------------------------------------------------------

    func testWindowAndProgress() {
        var s = CalibSession()
        XCTAssertFalse(s.windowOpen)
        s.start(nowMs: 1_000)
        XCTAssertTrue(s.windowOpen)
        XCTAssertTrue(s.pending)
        XCTAssertEqual(s.progress, 0, accuracy: 0.01)
        s.tick(nowMs: 1_000 + 700) // 1.4 s 的一半
        XCTAssertEqual(s.progress, 0.5, accuracy: 0.01)
        s.tick(nowMs: 1_000 + 1_400)
        XCTAssertEqual(s.progress, 1.0, accuracy: 0.001)
        XCTAssertTrue(s.windowOpen) // 3 s 窗口未到
        s.tick(nowMs: 1_000 + 3_001)
        XCTAssertFalse(s.windowOpen)
        XCTAssertTrue(s.awaitingLateReceipt) // 窗口关、回执未到 → 待确认文案
    }

    func testReceiptWithinWindowSettlesPending() {
        var s = CalibSession()
        s.start(nowMs: 1_000)
        s.receive(result: CalibResult(status: 0, saved: 1,
                                      invert: [-1, 1, 1, 1], delta: [9, 8, 7, 6]),
                  nowMs: 2_400)
        XCTAssertFalse(s.pending)
        XCTAssertFalse(s.awaitingLateReceipt)
        XCTAssertEqual(s.result?.saved, 1)
    }

    func testLateReceiptStillUpdatesResult() {
        var s = CalibSession()
        s.start(nowMs: 0)
        s.tick(nowMs: 10_000) // 远超 3 s 窗口（DFlash 等静止/重试场景）
        XCTAssertTrue(s.awaitingLateReceipt)
        s.receive(result: CalibResult(status: 0, saved: 2,
                                      invert: [1, 1, 1, 1], delta: [1, 1, 1, 1]),
                  nowMs: 12_000)
        XCTAssertFalse(s.awaitingLateReceipt)
        XCTAssertEqual(s.result?.saved, 2) // 迟到的失败回执也要显示
    }

    func testNewStartReplacesOldResult() {
        var s = CalibSession()
        s.receive(result: CalibResult(status: 0, saved: 0,
                                      invert: [1, 1, 1, 1], delta: [1, 1, 1, 1]),
                  nowMs: 0)
        s.start(nowMs: 5_000)
        XCTAssertNil(s.result) // 旧 DONE 不再代表本轮（doc 34 §9.4）
        XCTAssertTrue(s.pending)
    }

    func testLinkDownClearsWaitAndJogButKeepsResult() {
        var s = CalibSession()
        _ = s.jogPress(channel: 0, forward: true, nowMs: 0)
        s.start(nowMs: 0)
        s.receive(result: CalibResult(status: 0, saved: 1,
                                      invert: [1, 1, 1, 1], delta: [1, 1, 1, 1]),
                  nowMs: 1)
        s.linkDown()
        XCTAssertFalse(s.pending)
        XCTAssertFalse(s.windowOpen)
        XCTAssertNil(s.jogChannel)
        XCTAssertEqual(s.result?.saved, 1) // 结果表保留供查看
    }

    // ---- 判读语义（doc 34 §5.2） ------------------------------------------------

    func testVerdicts() {
        XCTAssertEqual(calibVerdict(delta: -1204, runComplete: true).kind, .flipped)
        XCTAssertEqual(calibVerdict(delta: 331, runComplete: true).kind, .ok)
        XCTAssertEqual(calibVerdict(delta: 0, runComplete: true).kind, .dead)   // 无计数：查接线
        XCTAssertEqual(calibVerdict(delta: 0, runComplete: false).kind, .untested) // 急停中止的未测轮
    }

    // ---- REC_SET 载荷与校验（对齐 CALIBREC_recSetDecode） ------------------------

    func testRecSetPayloadLayout() {
        let p = CalibWire.recSet(pos: [0, 2, 3, 1], invert: [-1, 1, -1, 1],
                                 fullScaleMmS: 1000, wheelDiaMm: 48)
        XCTAssertEqual(p, [0x00, 0x02, 0x03, 0x01, // pos u8×4
                           0xFF, 0x01, 0xFF, 0x01, // invert i8×4
                           0xE8, 0x03,             // fullScale i16LE (1000)
                           0x30, 0x00])            // wheelDia i16LE (48)
        XCTAssertEqual(p.count, 12)
    }

    func testRecSetValidation() {
        XCTAssertTrue(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [-1, 1, -1, 1],
                                            fullScaleMmS: 1000, wheelDiaMm: 48))
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 0, 3, 1], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 1000, wheelDiaMm: 48)) // 位置重复
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 4], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 1000, wheelDiaMm: 48)) // 位置越界
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [0, 1, 1, 1],
                                             fullScaleMmS: 1000, wheelDiaMm: 48)) // invert 非 ±1
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 99, wheelDiaMm: 48))   // 满量程下界
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 5001, wheelDiaMm: 48)) // 满量程上界
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 1000, wheelDiaMm: 29)) // 轮径下界
        XCTAssertFalse(CalibWire.recSetValid(pos: [0, 2, 3, 1], invert: [1, 1, 1, 1],
                                             fullScaleMmS: 1000, wheelDiaMm: 201)) // 轮径上界
    }

    // ---- recVsCalib（doc 17 §9） --------------------------------------------------

    func testInvertMismatchDetection() {
        var s = CalibSession()
        XCTAssertEmpty(s.invertMismatches) // 缺任一侧不判
        s.receive(record: CalibRecord(ver: 1, src: 1, pos: [0, 2, 3, 1],
                                      invert: [-1, 1, -1, 1],
                                      fullScaleMmS: 1000, wheelDiaMm: 48, crcOk: true))
        XCTAssertEmpty(s.invertMismatches) // 有记录无结果
        s.receive(result: CalibResult(status: 0, saved: 0,
                                      invert: [-1, 1, -1, 1], delta: [1, 1, 1, 1]),
                  nowMs: 0)
        XCTAssertEmpty(s.invertMismatches) // 逐轮一致
        s.receive(record: CalibRecord(ver: 1, src: 0, pos: [0, 2, 3, 1],
                                      invert: [1, 1, 1, 1],
                                      fullScaleMmS: 1000, wheelDiaMm: 48, crcOk: true))
        XCTAssertEqual(s.invertMismatches, [0, 2]) // A、C 两轮未落库
        s.receive(result: CalibResult(status: 1, saved: 0,
                                      invert: [1, 1, 1, 1], delta: [1, 1, 1, 1]),
                  nowMs: 0)
        XCTAssertEmpty(s.invertMismatches) // 急停中止的结果不构成判据
    }

    // ---- 默认位置表 ----------------------------------------------------------------

    func testDefaultPosMatchesFirmwareFillDefaults() {
        // calib_record.c fillDefaults：A 前左 / B 后左 / C 后右 / D 前右
        XCTAssertEqual(WheelChannel.defaultPos, [0, 2, 3, 1])
    }
}

/// XCTAssertEmpty 的最小实现（XCT 没有内建集合空断言）
private func XCTAssertEmpty<T: Collection>(_ c: T, _ message: String = "" ,
                                           file: StaticString = #filePath,
                                           line: UInt = #line) {
    XCTAssertTrue(c.isEmpty, "\(message)\(message.isEmpty ? "" : " — ")集合应为空，实际 \(c)",
                  file: file, line: line)
}
