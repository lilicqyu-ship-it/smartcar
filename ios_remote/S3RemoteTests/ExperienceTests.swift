/*
 * Small pure-component tests: glow modifier params (regression guard for the
 * former identity implementation), alert announcements, onboarding flag,
 * event export formatting, fault-code text.
 */

import SwiftUI
import XCTest
@testable import S3Remote

final class GlowModifierTests: XCTestCase {
    /// v1.2's glow() returned self (no effect). This pins the modifier's
    /// shadow construction: two layers, wide falloff — a third identity
    /// regression fails here.
    func testGlowBuildsTwoShadowLayers() {
        let shadows = GlowModifier.shadows(color: .red, radius: 8, opacity: 0.6)
        XCTAssertEqual(shadows.count, 2)
        XCTAssertEqual(shadows[0].radius, 8)
        XCTAssertEqual(shadows[1].radius, 8 * 2.4, accuracy: 0.0001)
        XCTAssertEqual(shadows[0].spread, 0.85, accuracy: 0.0001)
        XCTAssertEqual(shadows[1].spread, 0.35, accuracy: 0.0001)
    }
}

final class AlertAnnouncerTests: XCTestCase {
    private final class Recorder: @unchecked Sendable {
        var messages: [String] = []
    }

    func testAlertAnnouncementComposesTitleAndDetail() {
        let box = Recorder()
        let announcer = AlertAnnouncer { box.messages.append($0) }
        announcer.alertAppeared(Alert(kind: .radioLost, level: .critical, detail: "VEHICLE STOP"))
        XCTAssertEqual(box.messages, ["连接已中断。VEHICLE STOP。"])
        announcer.alertAppeared(Alert(kind: .lowBattery, level: .warning, detail: ""))
        XCTAssertEqual(box.messages.last, "电量偏低。")
    }

    func testAlertKindTitles() {
        XCTAssertEqual(AlertKind.emergency.title, "紧急停止")
        XCTAssertEqual(AlertKind.radioLost.title, "连接已中断")
        XCTAssertEqual(AlertKind.vehicleFault.title, "车辆需要检查")
        XCTAssertEqual(AlertKind.criticalBattery.title, "电量即将耗尽")
        XCTAssertEqual(AlertKind.lowBattery.title, "电量偏低")
    }
}

final class OnboardingStateTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "test.onboarding.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        super.tearDown()
    }

    func testFreshInstallShowsThenDismisses() {
        XCTAssertTrue(OnboardingState.shouldShow(defaults))
        OnboardingState.markSeen(defaults)
        XCTAssertFalse(OnboardingState.shouldShow(defaults))
        OnboardingState.reset(defaults)
        XCTAssertTrue(OnboardingState.shouldShow(defaults))
    }
}

final class EventExportTests: XCTestCase {
    func testEmptyLogHasPlaceholder() {
        let text = EventExport.text([], host: "192.168.4.1")
        XCTAssertTrue(text.contains("(无事件)"))
        XCTAssertTrue(text.contains("192.168.4.1"))
    }

    func testLinesCarryTimeLevelText() {
        let events = [
            EventEntry(level: "INFO", text: "S3 Remote 就绪"),
            EventEntry(level: "WARN", text: "连接断开：closed"),
        ]
        let text = EventExport.text(events, host: "10.0.0.1")
        XCTAssertTrue(text.contains("INFO S3 Remote 就绪"))
        XCTAssertTrue(text.contains("WARN 连接断开：closed"))
        XCTAssertTrue(text.contains("共 2 条"))
        for line in text.split(separator: "\n") where line.hasPrefix("2") {
            // timestamped body lines keep the HH:mm:ss.SSS shape
            XCTAssertNotNil(line.range(of: #"^\d{2}:\d{2}:\d{2}\.\d{3} "#, options: .regularExpression))
        }
    }

    func testCapAtMaxLines() {
        let events = (0..<250).map { EventEntry(level: "INFO", text: "e\($0)") }
        let text = EventExport.text(events, host: "h")
        let bodyLines = text.split(separator: "\n").filter { $0.contains(" INFO ") }
        XCTAssertEqual(bodyLines.count, EventExport.maxLines)
    }
}

final class FaultTextTests: XCTestCase {
    func testKnownFaultCodes() {
        XCTAssertEqual(FaultText.describe(0), "正常")
        XCTAssertEqual(FaultText.describe(1), "急停锁存")
        XCTAssertEqual(FaultText.describe(2), "通讯超时")
        XCTAssertEqual(FaultText.describe(3), "非法命令")
    }

    func testUnknownKeepsHex() {
        XCTAssertEqual(FaultText.describe(0x1234), "未知故障 0x1234")
    }

    func testRobotStates() {
        XCTAssertEqual(FaultText.robotState(0x00), "初始化")
        XCTAssertEqual(FaultText.robotState(0x01), "待命")
        XCTAssertEqual(FaultText.robotState(0x05), "运行中")
        XCTAssertEqual(FaultText.robotState(0x0A), "故障")
        XCTAssertEqual(FaultText.robotState(0xFF), "未知 0xFF")
    }
}
