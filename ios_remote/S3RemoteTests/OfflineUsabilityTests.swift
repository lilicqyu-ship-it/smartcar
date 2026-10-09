/*
 * OfflineUsabilityTests — 离线可操作契约（radioLost 降级为非阻断横幅）：
 * 1. 失联（含冷启动从未连接）只点亮 radioLostActive 横幅状态，绝不进入
 *    模态 alert 管线 —— Wi-Fi 未连接时 UI 不得被锁死。
 * 2. 摇杆输入在链路层被接受（UI 跟手），但 DriveController 的 connUp 闸门
 *    与 controlTick 的输出归零保持原样 —— 线上协议行为零变化。
 */

import XCTest
@testable import S3Remote

@MainActor
final class OfflineUsabilityTests: XCTestCase {
    private func makeWorld() -> (app: AppState, suite: String) {
        let suite = "test.offline.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        let app = AppState(settings: AppSettings(host: "127.0.0.1", cameraEnabled: false),
                           tokenStore: MockTokenStore(), defaults: defaults)
        return (app, suite)
    }

    private func telemetry(pct: UInt8 = 80) -> Telemetry {
        var t = Telemetry()
        t.seq &+= 1
        t.batteryPct = pct
        return t
    }

    /// debounceMs = 0：第一个 tick 立起失联窗口，第二个 tick 越过去抖。
    private func tickPastDebounce(_ app: AppState) {
        app.controlTick()
        app.controlTick()
    }

    // ---- 失联 → 横幅状态，而非模态告警 ---------------------------------------

    func testColdStartRadioLostNeverRaisesModalAlert() {
        let (app, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        app.monitor.debounceMs = 0

        XCTAssertFalse(app.hasConnected, "冷启动尚未连上过")
        XCTAssertNil(app.alert)
        tickPastDebounce(app)

        XCTAssertTrue(app.radioLostActive, "冷启动离线同样走横幅提示")
        XCTAssertNil(app.alert, "radioLost 不得再产生全屏模态告警")
        XCTAssertEqual(app.connState, .disconnected)
    }

    func testRadioLostAfterDropAndClearsOnRecovery() {
        let (app, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        app.monitor.debounceMs = 0

        app.handleConnecting()
        app.handleOpen()
        app.applyHello(role: .ctrl, ver: "vTest", tcUp: true, pair: nil, ctrlHeld: true, rssi: nil)
        XCTAssertTrue(app.hasConnected)

        app.handleDown("unit")
        tickPastDebounce(app)
        XCTAssertTrue(app.radioLostActive)
        XCTAssertNil(app.alert)
        XCTAssertTrue(app.hasConnected, "断连不清除 hasConnected — 横幅文案保持「已中断」")

        // 链路恢复（WebSocket 开 + 遥测恢复新鲜）→ 横幅立即消失
        app.handleOpen()
        app.applyTelemetry(telemetry())
        app.controlTick()
        XCTAssertFalse(app.radioLostActive)
        XCTAssertNil(app.alert)
    }

    // ---- 离线摇杆：UI 跟手，输出闸门不动 -------------------------------------

    func testOfflineJoystickAcceptedAtInputButOutputStaysZero() {
        let (app, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        app.monitor.debounceMs = 0

        app.joystickMoved(v: 0.5, w: -0.25)
        XCTAssertEqual(app.controller.joyV, 0.5, accuracy: 0.0001,
                       "离线时输入层必须接受摇杆（UI 跟手）")
        XCTAssertEqual(app.controller.joyW, -0.25, accuracy: 0.0001)

        tickPastDebounce(app)
        XCTAssertTrue(app.radioLostActive)
        XCTAssertEqual(app.outV, 0, "connUp 闸门：离线不得产生输出速度")
        XCTAssertEqual(app.outW, 0)
    }

    func testDriveControllerGateUnchanged() {
        var c = DriveController()
        c.joyV = 1.0
        XCTAssertNil(c.tick(connUp: false, ctrlRole: true),
                     "离线可操作不放松控制闸门：未连接时零帧")
    }
}
