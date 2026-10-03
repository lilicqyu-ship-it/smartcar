/*
 * AppState.swift — single source of truth for the whole UI (same role as
 * smartcar_remote app_state, spec 98-101): snapshot fields the pages read,
 * event ring log, alert state, plus the pure logic objects wired together.
 * @MainActor by design: LinkEngine hops network events onto it.
 */

import Foundation
import Observation

public enum ConnState: Equatable, Sendable {
    case disconnected
    case connecting
    case connected
}

public struct EventEntry: Identifiable, Equatable, Sendable {
    public let id = UUID()
    public let at: Date
    public let level: String // INFO / WARN / CRIT
    public let text: String

    init(level: String, text: String) {
        self.at = Date()
        self.level = level
        self.text = text
    }
}

public struct AppSettings: Equatable, Codable, Sendable {
    public var host: String
    public var token: String
    public var deadzone: Double // 0...0.4
    public var mode: DriveMode

    public init(host: String = "192.168.4.1", token: String = "",
                deadzone: Double = 0.08, mode: DriveMode = .normal) {
        self.host = host
        self.token = token
        self.deadzone = deadzone
        self.mode = mode
    }

    static let defaultsKey = "s3remote.settings.v1"

    public static func load() -> AppSettings {
        guard
            let data = UserDefaults.standard.data(forKey: defaultsKey),
            let s = try? JSONDecoder().decode(AppSettings.self, from: data)
        else { return AppSettings() }
        return s
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: Self.defaultsKey)
        }
    }
}

@MainActor
@Observable
public final class AppState {
    // ---- persisted ---------------------------------------------------------
    public var settings: AppSettings {
        didSet { settings.save() }
    }

    // ---- link --------------------------------------------------------------
    public private(set) var connState: ConnState = .disconnected
    public private(set) var ctrlRole = false
    public private(set) var ctrlHeld = false
    public private(set) var c6Ver = ""
    public private(set) var tcUp = false
    public private(set) var pairWindow: PairState?
    public var pairStatus = ""
    /// Per-client RSSI from the gateway when provided; nil = not available
    /// (signal bars then fall back to the RTT/loss estimate).
    public private(set) var rssiDbm: Int?
    /// TC275 version beacon (`{"t":"tcver","app":..,"sbl":..}` from the C6
    /// bridge). Empty = not received yet this session.
    public private(set) var tcAppVer = ""
    public private(set) var tcSblVer = ""

    // ---- telemetry (snapshot; freshness per spec 101: 600 ms → "--") -------
    public private(set) var telemetry: Telemetry?
    public private(set) var teleAtMs: Double = 0
    public private(set) var nowMs: Double = 0

    // battery display debounce (C6 cee1189 strategy: median + EMA + latches);
    // alarms/colors keep using the real telemetry above
    private var batteryFilter = BatteryDisplayFilter()
    public private(set) var batteryDisplayMv: Int?
    public private(set) var batteryDisplayPct: Int?

    public var teleFresh: Bool {
        telemetry != nil && (nowMs - teleAtMs) < 600
    }

    // ---- link statistics (spec 24-28) ---------------------------------------
    public private(set) var lossCounter = TelemetryLossCounter()
    public private(set) var txRate = 0
    public private(set) var rxRate = 0
    public private(set) var rttLast = 0
    public private(set) var rttMin = 0
    public private(set) var rttMax = 0
    /// RTT sample history for the diagnostics sparkline (UI support state).
    public private(set) var rttHistory: [Int] = []

    // ---- control ------------------------------------------------------------
    public private(set) var controller = DriveController()
    public private(set) var outV: Int16 = 0 // actually-sent values (spec 110)
    public private(set) var outW: Int16 = 0

    public var emergActive: Bool { controller.emergLatch }
    public var stopLatched: Bool { controller.stopLatch }

    // ---- safety / alerts -----------------------------------------------------
    public var monitor = SafetyMonitor()
    public private(set) var alert: Alert?
    private var ackDismissed: Set<AlertKind> = []
    private var radioLostActive = false
    private var faultActive = false
    private var batteryKind: AlertKind?

    // ---- event ring (spec 83) -------------------------------------------------
    public private(set) var events: [EventEntry] = []

    public var ownerText: String {
        guard connState == .connected else { return "OFFLINE" }
        if ctrlRole { return "CTRL" }
        return ctrlHeld ? "NO CONTROL" : "SPECTATOR"
    }

    /// 0...4 signal bars: real RSSI from the gateway when present, otherwise
    /// the RTT/loss composite estimate ("预估" in the label).
    public var signalBars: Int {
        guard connState == .connected else { return 0 }
        if let rssi = rssiDbm {
            return LinkQuality.bars(forRssi: rssi)
        }
        return LinkQuality.estimatedBars(rttMs: rttLast, lossPerMille: lossCounter.lossPerMille)
    }

    public var signalLabel: String? {
        guard connState == .connected else { return nil }
        if let rssi = rssiDbm {
            return "\(rssi) dBm · \(LinkQuality.label(forRssi: rssi))"
        }
        return "预估 · \(LinkQuality.estimatedLabel(bars: signalBars))"
    }

    weak var link: LinkEngine?
    private var seq: UInt8 = 0

    public init(settings: AppSettings = .load()) {
        self.settings = settings
        monitor.debounceMs = 1200
        log("INFO", "S3 Remote 就绪")
    }

    func nextSeq() -> UInt8 {
        seq &+= 1
        return seq
    }

    func now() -> Double {
        Date.timeIntervalSinceReferenceDate * 1000
    }

    public func log(_ level: String, _ text: String) {
        events.insert(EventEntry(level: level, text: text), at: 0)
        if events.count > 200 {
            events.removeLast(events.count - 200)
        }
    }

    // ---- link event handlers (called by LinkEngine) ---------------------------

    func handleConnecting() {
        connState = .connecting
    }

    func handleOpen() {
        connState = .connected
        log("INFO", "WebSocket 已连接")
    }

    func handleDown(_ reason: String) {
        guard connState != .disconnected else { return }
        connState = .disconnected
        ctrlRole = false
        tcUp = false
        rssiDbm = nil
        tcAppVer = ""
        tcSblVer = ""
        outV = 0
        outW = 0
        log("WARN", "连接断开：\(reason) — 车辆由 TC275 心跳看门狗停车")
    }

    func applyHello(role: HelloRole, ver: String, tcUp: Bool, pair: PairState?, ctrlHeld: Bool, rssi: Int?) {
        c6Ver = ver
        self.ctrlHeld = ctrlHeld
        pairWindow = pair
        if let rssi { rssiDbm = rssi }
        switch role {
        case .ctrl:
            if !ctrlRole { log("INFO", "控制权获得 (CTRL)") }
            ctrlRole = true
        case .spectator:
            if ctrlRole { log("WARN", "控制权丢失 — 停止发送控制帧") }
            ctrlRole = false
            controller.controlLost()
        case .other(let s):
            ctrlRole = false
            log("WARN", "未知角色 \(s)")
        }
        self.tcUp = tcUp
    }

    func applyTc(_ up: Bool) {
        if up != tcUp {
            log("INFO", up ? "Vehicle link up (TC275)" : "Vehicle link down (TC275)")
        }
        tcUp = up
    }

    func applyAuthRejected() {
        if ctrlRole { log("WARN", "控制权被拒 (err auth) — 网关已将 CTRL 交给其他客户端") }
        ctrlRole = false
        controller.controlLost()
    }

    func applyTelemetry(_ t: Telemetry) {
        lossCounter.onTelemetry(seq: t.seq)
        telemetry = t
        teleAtMs = nowMs
        let display = batteryFilter.apply(pct: Int(t.batteryPct), mv: Int(t.batteryMv), nowMs: nowMs)
        if let mv = display.mv { batteryDisplayMv = mv }
        if let pct = display.pct { batteryDisplayPct = pct }
    }

    func applyRtt(_ ms: Int) {
        rttLast = ms
        if rttMin == 0 || ms < rttMin { rttMin = ms }
        if ms > rttMax { rttMax = ms }
        rttHistory.append(ms)
        if rttHistory.count > 40 {
            rttHistory.removeFirst(rttHistory.count - 40)
        }
    }

    func applyRssi(_ dbm: Int) {
        rssiDbm = dbm
    }

    /// TC275 版本信标（C6 bridge 周期广播），拓扑页与版本清单展示用
    func applyTcVer(app: String, sbl: String) {
        tcAppVer = app
        tcSblVer = sbl
    }

    func publishStats(tx: Int, rx: Int) {
        txRate = tx
        rxRate = rx
    }

    // ---- control actions (UI entry points, mirror scr_ctrl API) ----------------

    public func stopPressed() {
        let cmd = controller.stopClick()
        link?.sendDrive(cmd) // immediate, does not wait for the next tick
        log("WARN", "STOP — 已发送 DRIVE(0,0) 并锁存")
    }

    public func emergencyTriggered() {
        let cmd = controller.emergency()
        link?.sendEmergencyStop()
        link?.sendDrive(cmd)
        log("CRIT", "EMERGENCY STOP (0x32)")
    }

    public func emergencyRelease() {
        guard emergActive else { return }
        controller.emergencyRelease()
        log("INFO", "急停解除 — 车辆保持停止，触摸摇杆恢复")
    }

    /// Joystick touch = re-take control; clears the STOP latch (spec 19/105).
    public func joystickTouch() {
        if controller.stopLatch {
            log("INFO", "触摸摇杆 — STOP 锁存解除")
        }
        controller.joystickTouch()
    }

    public func joystickMoved(v: Double, w: Double) {
        controller.joyV = v
        controller.joyW = w
    }

    public func setMode(_ m: DriveMode) {
        controller.mode = m
        settings.mode = m
        log("INFO", "模式 \(m.label) — 限幅 \(m.pct)%")
    }

    // ---- 30 Hz beat: drive + safety watch (called by LinkEngine) -----------------

    func controlTick() {
        nowMs = now()
        let connected = connState == .connected
        if let cmd = controller.tick(connUp: connected, ctrlRole: ctrlRole) {
            link?.sendDrive(cmd)
            outV = cmd.v
            outW = cmd.w
        } else if !connected {
            outV = 0
            outW = 0
        }
        safetyTick()
    }

    private func safetyTick() {
        let lost = monitor.radioLostActive(
            nowMs: nowMs,
            linkUp: connState == .connected,
            teleFresh: teleFresh)
        if lost != radioLostActive {
            radioLostActive = lost
            if lost {
                log("CRIT", "RADIO LOST — 车辆停车（遥测断流 ≥1.2 s）")
            } else {
                log("INFO", "Radio recovered — 链路恢复")
            }
        }

        let fault = SafetyMonitor.faultActive(telemetry?.faultCode ?? 0)
        if fault != faultActive {
            faultActive = fault
            if fault, let code = telemetry?.faultCode {
                log("WARN", String(format: "VEHICLE FAULT 0x%04X", code))
            } else {
                log("INFO", "故障已清除")
            }
        }

        var batt: AlertKind?
        if teleFresh, let t = telemetry {
            batt = monitor.batteryWatch(Int(t.batteryPct))
        }
        if batt != batteryKind {
            batteryKind = batt
            if let b = batt {
                log(b == .criticalBattery ? "CRIT" : "WARN",
                    b == .criticalBattery ? "CRITICAL BATTERY" : "LOW BATTERY")
            } else {
                log("INFO", "电池电量恢复")
            }
        }

        // Highest-priority non-emergency alert; ACK dismissal re-arms when the
        // condition clears (overlay must not strobe, mirror spec 22/102).
        var kinds: Set<AlertKind> = []
        var newAlert: Alert?
        if radioLostActive {
            newAlert = Alert(kind: .radioLost, level: .critical, detail: "VEHICLE STOP")
            kinds.insert(.radioLost)
        } else if faultActive, let code = telemetry?.faultCode {
            newAlert = Alert(kind: .vehicleFault, level: .warning,
                             detail: String(format: "0x%04X", code))
            kinds.insert(.vehicleFault)
        } else if let b = batteryKind {
            newAlert = Alert(kind: b,
                             level: b == .criticalBattery ? .critical : .warning,
                             detail: "\(telemetry?.batteryPct ?? 0) %")
            kinds.insert(b)
        }
        ackDismissed.formIntersection(kinds)
        if let a = newAlert, ackDismissed.contains(a.kind) { newAlert = nil }
        alert = newAlert
    }

    public func ackAlert() {
        guard let kind = alert?.kind else { return }
        ackDismissed.insert(kind)
        log("INFO", "告警确认 (ACK)")
    }
}
