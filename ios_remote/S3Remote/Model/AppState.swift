/*
 * AppState.swift — single source of truth for the whole UI (same role as
 * smartcar_remote app_state, spec 98-101): snapshot fields the pages read,
 * event ring log, alert state, plus the pure logic objects wired together.
 * @MainActor by design: LinkEngine hops network events onto it.
 */

import Foundation
import Observation
import UIKit // UIAccessibility announcements for full-screen alerts

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
    /// Runtime value only — NEVER serialized; the persistent copy lives in
    /// the Keychain (TokenStore). Decoding still accepts it for v1.2 blobs
    /// (migrated to the Keychain on launch).
    public var token: String
    public var deadzone: Double // 0...0.4
    public var mode: DriveMode
    // ---- play features ----
    public var soundEnabled: Bool
    public var trackWidthMm: Double // odometry guess for the trail view
    public var tiltSensitivity: Double // 0.5...2
    // ---- camera plane (s3-gateway :81) ----
    public var cameraEnabled: Bool
    public var cameraHost: String // empty = follow the control-plane host
    // ---- driving-assist banner (fusion warnings above the joystick) ----
    /// Display switch for the drive page's guard hint banner ("前进限速 …" /
    /// "近障停车 …"). Display-only: the vehicle-side protection and the event
    /// log keep working regardless.
    public var guardHintEnabled: Bool

    public init(host: String = "192.168.4.1", token: String = "",
                deadzone: Double = 0.08, mode: DriveMode = .normal,
                soundEnabled: Bool = false, trackWidthMm: Double = 150,
                tiltSensitivity: Double = 1.0,
                cameraEnabled: Bool = false, cameraHost: String = "",
                guardHintEnabled: Bool = true) {
        self.host = host
        self.token = token
        self.deadzone = deadzone
        self.mode = mode
        self.soundEnabled = soundEnabled
        self.trackWidthMm = trackWidthMm
        self.tiltSensitivity = tiltSensitivity
        self.cameraEnabled = cameraEnabled
        self.cameraHost = cameraHost
        self.guardHintEnabled = guardHintEnabled
    }

    static let defaultsKey = "s3remote.settings.v1"

    public static func load(from defaults: UserDefaults = .standard) -> AppSettings {
        guard
            let data = defaults.data(forKey: defaultsKey),
            let s = try? JSONDecoder().decode(AppSettings.self, from: data)
        else { return AppSettings() }
        return s
    }

    func save(to defaults: UserDefaults = .standard) {
        if let data = try? JSONEncoder().encode(self) {
            defaults.set(data, forKey: Self.defaultsKey)
        }
    }

    private enum CodingKeys: String, CodingKey {
        case host, token, deadzone, mode, soundEnabled, trackWidthMm, tiltSensitivity
        case cameraEnabled, cameraHost, guardHintEnabled
    }

    /// Lenient decode: a v1 JSON without the play/camera keys must not fail
    /// (load() would fall back to defaults and wipe the saved host).
    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        host = try c.decodeIfPresent(String.self, forKey: .host) ?? "192.168.4.1"
        token = try c.decodeIfPresent(String.self, forKey: .token) ?? ""
        deadzone = try c.decodeIfPresent(Double.self, forKey: .deadzone) ?? 0.08
        mode = try c.decodeIfPresent(DriveMode.self, forKey: .mode) ?? .normal
        soundEnabled = try c.decodeIfPresent(Bool.self, forKey: .soundEnabled) ?? false
        trackWidthMm = try c.decodeIfPresent(Double.self, forKey: .trackWidthMm) ?? 150
        tiltSensitivity = try c.decodeIfPresent(Double.self, forKey: .tiltSensitivity) ?? 1.0
        cameraEnabled = try c.decodeIfPresent(Bool.self, forKey: .cameraEnabled) ?? false
        cameraHost = try c.decodeIfPresent(String.self, forKey: .cameraHost) ?? ""
        guardHintEnabled = try c.decodeIfPresent(Bool.self, forKey: .guardHintEnabled) ?? true
    }

    public func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: CodingKeys.self)
        try c.encode(host, forKey: .host)
        // token deliberately omitted — it lives in the Keychain now
        try c.encode(deadzone, forKey: .deadzone)
        try c.encode(mode, forKey: .mode)
        try c.encode(soundEnabled, forKey: .soundEnabled)
        try c.encode(trackWidthMm, forKey: .trackWidthMm)
        try c.encode(tiltSensitivity, forKey: .tiltSensitivity)
        try c.encode(cameraEnabled, forKey: .cameraEnabled)
        try c.encode(cameraHost, forKey: .cameraHost)
        try c.encode(guardHintEnabled, forKey: .guardHintEnabled)
    }
}

@MainActor
@Observable
public final class AppState {
    // ---- persisted ---------------------------------------------------------
    public var settings: AppSettings {
        didSet { settings.save(to: defaults) }
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
    public private(set) var fusion: FusionStatus?
    private var fusionAtMs: Double = 0

    public var fusionWarning: String? {
        guard connState == .connected && tcUp else { return nil }
        guard let fusion else { return "等待驾驶辅助状态" }
        guard nowMs - fusionAtMs < 1000 else { return "驾驶辅助状态已过期" }
        return fusion.warning
    }

    func applyFusion(_ status: FusionStatus) {
        let oldWarning = fusion?.warning
        fusion = status
        fusionAtMs = now()
        if let warning = status.warning, warning != oldWarning { log("WARN", warning) }
    }

    // ---- sensor streams (sensor tab: raw IMU + ToF zone map) -----------------
    // Diagnostic JSON side-channel ({"t":"imu"} @ 20 Hz, {"t":"tofz"} @ 15 Hz
    // reassembled). Display-only: nothing here feeds driving or safety.
    /// Freeze switch for the sensor tab's buffers ("暂停" button). While set,
    /// incoming samples are dropped so the charts hold still for reading;
    /// the connection and the event log are unaffected.
    public var sensorPaused = false
    /// Raw IMU samples, oldest last, capped at 200 (≈10 s at 20 Hz).
    public private(set) var imuHistory: [ImuSample] = []
    private var imuAtMs: Double = 0
    /// g-Ball trail: gravity-compensated horizontal acceleration (g, body
    /// fwd/lat), newest last, capped at 120 (≈6 s at 20 Hz).
    public private(set) var accelTrail: [(fwd: Double, lat: Double)] = []
    /// Peak |horizontal| acceleration since the link came up (g).
    public private(set) var peakHorizontalG: Double = 0
    private var peakTracker = PeakGTracker()
    /// Latest complete 8×8 zone map, nil until the first frame assembles.
    public private(set) var tofMap: TofZoneFrame?
    private var tofMapAtMs: Double = 0
    /// Nearest-distance trend from complete maps with ≥1 trusted zone.
    public private(set) var tofNearHistory: [Int] = []
    private var tofAssembler = TofZoneAssembler()

    public var imuFresh: Bool { nowMs - imuAtMs < 1000 }
    public var tofMapFresh: Bool { nowMs - tofMapAtMs < 1000 }

    /// Observed IMU stream rate from the car's own sample stamps (Hz);
    /// nil while fewer than two samples are buffered.
    public var imuRateHz: Double? {
        guard imuHistory.count >= 2 else { return nil }
        let dt = imuHistory[imuHistory.count - 1].stampMs - imuHistory[imuHistory.count - 2].stampMs
        guard dt > 0 else { return nil }
        return 1000.0 / Double(dt)
    }

    func applyImu(_ sample: ImuSample) {
        guard !sensorPaused else { return }
        imuAtMs = now()
        imuHistory.append(sample)
        if imuHistory.count > 200 {
            imuHistory.removeFirst(imuHistory.count - 200)
        }
        // g-Ball: gravity compensation needs the attitude; without fusion the
        // raw ax/ay still move the ball, the view just greys it out.
        let fusion = self.fusion
        let horizontal = ImuKinematics.horizontalG(
            accMgX: Double(sample.accMg[0]), y: Double(sample.accMg[1]),
            z: Double(sample.accMg[2]),
            rollDeg: Double(fusion?.rollCdeg ?? 0) / 100,
            pitchDeg: Double(fusion?.pitchCdeg ?? 0) / 100)
        accelTrail.append((fwd: horizontal.fwd, lat: horizontal.lat))
        if accelTrail.count > 120 {
            accelTrail.removeFirst(accelTrail.count - 120)
        }
        peakTracker.observe(horizontal.magnitude)
        peakHorizontalG = peakTracker.peak
    }

    func applyTofFragment(seq: Int, frag: Int, mode: Int, valid: Int,
                          nearestMm: Int, zones: [Int]) {
        guard !sensorPaused else { return }
        guard let frame = tofAssembler.add(seq: seq, frag: frag, mode: mode,
                                           valid: valid, nearestMm: nearestMm,
                                           zones: zones) else { return }
        tofMap = frame
        tofMapAtMs = now()
        // An all-invalid frame (nearest 0) is "no reading", not "wall at 0 mm":
        // leave the trend at its last value; the valid/freshness chips say why.
        if frame.validZones > 0 {
            tofNearHistory.append(frame.nearestMm)
            if tofNearHistory.count > 150 {
                tofNearHistory.removeFirst(tofNearHistory.count - 150)
            }
        }
    }

    private func clearSensorStreams() {
        imuHistory = []
        imuAtMs = 0
        accelTrail = []
        peakTracker.reset()
        peakHorizontalG = 0
        tofMap = nil
        tofMapAtMs = 0
        tofNearHistory = []
        tofAssembler.reset()
    }

    // battery display debounce (C6 cee1189 strategy: median + EMA + latches);
    // alarms/colors keep using the real telemetry above
    private var batteryFilter = BatteryDisplayFilter()
    public private(set) var batteryDisplayMv: Int?
    public private(set) var batteryDisplayPct: Int?

    // speed display EMA (C6 renderSpeed port): smoothed hero number; raw
    // telemetry keeps feeding trail/records/safety
    private var speedFilter = SpeedDisplayFilter()
    /// Smoothed signed body speed (mm/s) for the drive-page hero readout.
    public private(set) var displaySpeedMmS: Double = 0

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

    // ---- bench calibration (TC275 DPT 0x70~0x74) ------------------------------
    /// 判向标定/逐电机点动/记录读写会话；帧发送走 link.sendDPT，
    /// JSON 回执（cal/rec/jogcnt）由 LinkEngine 路由到 apply*。
    public private(set) var calib = CalibSession()

    public var emergActive: Bool { controller.emergLatch }
    public var stopLatched: Bool { controller.stopLatch }

    // ---- play (stunts / tilt / trail / records) -------------------------------
    /// Stunt macro player; output feeds the joystick axes in controlTick.
    public private(set) var sequencer = StuntSequencer()
    /// Tilt steering on/off; axes come from MotionSource gravity samples.
    public private(set) var tiltEnabled = false
    public private(set) var tiltAxes: (v: Double, w: Double) = (0, 0)
    public private(set) var tiltDriver = TiltDriver()
    /// Latest gravity sample (g units) for the bubble-level indicator.
    public private(set) var gravity: (x: Double, y: Double, z: Double)?
    /// Trail dead-reckoning from wheel speeds.
    public private(set) var odometry = OdometryTracker()
    /// Record wall (top speed / longest session / best lap), persisted.
    public private(set) var recordsStore = RecordsTracker()
    public private(set) var lapTimer = LapTimer()
    /// Synthesized audio; created eagerly, fails soft without an audio device.
    let sound = SoundEngine()
    var motion: MotionSource? // lazily created on first tilt enable
    /// s3-gateway camera plane; started/stopped by tab & scenePhase ownership.
    public private(set) var camera: CameraClient

    // ---- onboarding ------------------------------------------------------------
    /// Sheet binding; persisted via OnboardingState, replayable from Settings.
    public var showOnboarding: Bool

    let tokenStore: any TokenStoring
    private let defaults: UserDefaults
    private let announcer: AlertAnnouncer

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

    public init(settings: AppSettings = .load(), tokenStore: any TokenStoring = KeychainTokenStore(),
                defaults: UserDefaults = .standard) {
        self.tokenStore = tokenStore
        self.defaults = defaults
        announcer = AlertAnnouncer { text in
            UIAccessibility.post(notification: .announcement, argument: text)
        }
        camera = CameraClient()
        var s = settings
        var migrated = false
        // Keychain migration: v1.2 blobs carried the token in UserDefaults.
        if !s.token.isEmpty {
            if tokenStore.get().isEmpty { tokenStore.set(s.token) }
            s.token = ""
            migrated = true
        }
        showOnboarding = !ProcessInfo.processInfo.arguments.contains("--no-onboard")
            && OnboardingState.shouldShow(defaults)
        self.settings = s
        // @Observable: mutating any tracked property touches the shared
        // observation registration, so tracked mutations must wait until
        // every stored property above is initialized.
        self.settings.token = tokenStore.get() // runtime value; blob stays clean
        if migrated { settings.save(to: defaults) }
        monitor.debounceMs = 1200
        tiltDriver.sensitivity = settings.tiltSensitivity
        odometry.trackWidthMm = settings.trackWidthMm
        sound.setEnabled(settings.soundEnabled)
        // all stored properties are set — wiring the escape-hatch closure now
        camera.configProvider = { [weak self] in
            guard let self else { return (enabled: false, host: "") }
            let host = self.settings.cameraHost.isEmpty ? self.settings.host : self.settings.cameraHost
            return (enabled: self.settings.cameraEnabled, host: host)
        }
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
        fusion = nil
        rssiDbm = nil
        tcAppVer = ""
        tcSblVer = ""
        outV = 0
        outW = 0
        sequencer.abort()
        tiltAxes = (0, 0)
        calib.linkDown() // 车端因失联中止标定；本地清理点动与等待态
        calib.setWheelsOffConfirmed(false) // 重新连接后需重新确认安全前提
        clearSensorStreams()
        speedFilter.reset()
        displaySpeedMmS = 0
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
        if !up {
            fusion = nil
            clearSensorStreams()
        }
    }

    func applyAuthRejected() {
        if ctrlRole { log("WARN", "控制权被拒 (err auth) — 网关已将 CTRL 交给其他客户端") }
        ctrlRole = false
        controller.controlLost()
    }

    func applyTelemetry(_ t: Telemetry) {
        lossCounter.onTelemetry(seq: t.seq)
        let dtMs = nowMs - teleAtMs
        // trail: a stream gap means the car moved while we were blind —
        // restart the trace instead of drawing a jump line
        if teleAtMs > 0, dtMs > 2000 { odometry.reset() }
        odometry.onTelemetry(vL: Double(t.vMeasL), vR: Double(t.vMeasR),
                             dtMs: teleAtMs > 0 ? dtMs : 20)
        if recordsStore.observe(speedKmh: abs(Double(t.vMeasL) + Double(t.vMeasR)) / 2 * 0.0036,
                                 sessionMeters: Double(t.odoSessionMm) / 1000) {
            // tracker persists to the injected defaults itself
        }
        telemetry = t
        teleAtMs = nowMs
        displaySpeedMmS = speedFilter.apply(Double(t.vMeasL + t.vMeasR) / 2, nowMs: nowMs)
        let display = batteryFilter.apply(pct: Int(t.batteryPct), mv: Int(t.batteryMv),
                                          uptimeMs: t.uptimeMs, nowMs: nowMs)
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
        abortStunt(reason: "STOP")
        if let f = calib.jogRelease() { // 点动中的轮立即归零（newest-wins）
            link?.sendDPT(Proto.Cmd.dptMotorJog, data: f.payload)
        }
        let cmd = controller.stopClick()
        link?.sendDrive(cmd) // immediate, does not wait for the next tick
        log("WARN", "STOP — 已发送 DRIVE(0,0) 并锁存")
    }

    public func emergencyTriggered() {
        abortStunt(reason: "紧急停止")
        if let f = calib.jogRelease() {
            link?.sendDPT(Proto.Cmd.dptMotorJog, data: f.payload)
        }
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

    /// Joystick touch = re-take control; clears the STOP latch (spec 19/105)
    /// and aborts any running stunt (manual takeover always wins).
    public func joystickTouch() {
        abortStunt(reason: "摇杆接管")
        if controller.stopLatch {
            log("INFO", "触摸摇杆 — STOP 锁存解除")
        }
        controller.joystickTouch()
    }

    public func joystickMoved(v: Double, w: Double) {
        if sequencer.active {
            // A live stunt owns the axes. Zero writes are UI resets (the drive
            // joystick zeroes itself when the stunt disables it, tab switches),
            // not a takeover — only a real push takes over.
            if v == 0 && w == 0 { return }
            abortStunt(reason: "摇杆接管")
        }
        controller.joyV = v
        controller.joyW = w
    }

    public func setMode(_ m: DriveMode) {
        controller.mode = m
        settings.mode = m
        log("INFO", "模式 \(m.label) — 限幅 \(m.pct)%")
    }

    // ---- play actions ------------------------------------------------------------

    /// Tap a stunt to run; tapping the running one aborts it. Stunts ride the
    /// joystick axes path, so every latch/limit/watchdog still applies.
    public func startStunt(_ stunt: Stunt) {
        guard connState == .connected, ctrlRole else {
            log("WARN", "特技需已连接且取得控制权")
            return
        }
        guard !emergActive else {
            log("WARN", "急停锁存中 — 请先解除急停")
            return
        }
        if sequencer.stunt == stunt {
            abortStunt(reason: "再次点击")
            return
        }
        controller.joystickTouch() // explicit re-take: clears a STOP latch
        sequencer.start(stunt)
        sound.blip()
        log("INFO", "特技「\(stunt.name)」开始")
    }

    public func abortStunt(reason: String) {
        guard sequencer.active else { return }
        sequencer.abort()
        controller.joyV = 0
        controller.joyW = 0
        log("WARN", "特技中止（\(reason)）")
    }

    public func setTiltEnabled(_ on: Bool) {
        guard on != tiltEnabled else { return }
        tiltEnabled = on
        tiltAxes = (0, 0)
        if on {
            let src = motion ?? MotionSource()
            motion = src
            src.start { [weak self] gx, gy, gz in self?.applyGravity(gx: gx, gy: gy, gz: gz) }
            log("INFO", "体感驾驶开启 — 前倾加速，左右倾斜转向")
        } else {
            motion?.stop()
            log("INFO", "体感驾驶关闭")
        }
    }

    public func calibrateTilt() {
        guard let g = gravity else {
            log("WARN", "体感尚未就绪 — 稍候再校准")
            return
        }
        tiltDriver.calibrate(gx: g.x, gz: g.z)
        log("INFO", "体感已按当前姿态校准")
    }

    func applyGravity(gx: Double, gy: Double, gz: Double) {
        gravity = (gx, gy, gz)
        guard tiltEnabled else { return }
        // No stunt abort here: controlTick gives stunt axes priority over tilt,
        // so a stunt keeps playing and tilt resumes when it ends.
        tiltAxes = tiltDriver.axes(gx: gx, gz: gz)
    }

    public func clearTrail() {
        odometry.reset()
    }

    public func setTrackWidthMm(_ mm: Double) {
        settings.trackWidthMm = mm
        odometry.trackWidthMm = mm
    }

    public func setTiltSensitivity(_ s: Double) {
        settings.tiltSensitivity = s
        tiltDriver.sensitivity = s
    }

    public func setSoundEnabled(_ on: Bool) {
        settings.soundEnabled = on
        sound.setEnabled(on)
    }

    /// Pairing token writes go through here: runtime copy in settings (used
    /// by the WS URL), persistent copy in the Keychain. Empty = reset pairing.
    public func setToken(_ token: String) {
        settings.token = token
        if token.isEmpty {
            tokenStore.remove()
        } else {
            tokenStore.set(token)
        }
    }

    // ---- camera plane -------------------------------------------------------------

    /// Single ownership point: stream runs only while the drive tab is up,
    /// camera is enabled, and the app is active — otherwise the gateway's
    /// one-viewer slot is handed back.
    public func syncCamera(activeTab: Int) {
        let shouldRun = settings.cameraEnabled && activeTab == Tab.drive.rawValue
        if shouldRun {
            camera.start()
        } else {
            camera.stop()
        }
    }

    // ---- onboarding -----------------------------------------------------------------

    public func dismissOnboarding() {
        OnboardingState.markSeen(defaults)
        showOnboarding = false
    }

    public func replayOnboarding() {
        showOnboarding = true
    }

    public func hornPressed() {
        sound.horn()
        Haptics.light()
    }

    // ---- lap timer / records ------------------------------------------------------

    public func lapStart() {
        lapTimer.start(nowMs: nowMs)
        Haptics.medium()
        log("INFO", "圈速计时开始")
    }

    public func lapSplit() {
        guard let s = lapTimer.lap(nowMs: nowMs) else { return }
        if recordsStore.noteLap(seconds: s) { // persists internally
            Haptics.success()
            log("INFO", String(format: "新纪录单圈 %.2f s", s))
        } else {
            Haptics.medium()
            log("INFO", String(format: "单圈 %.2f s", s))
        }
    }

    public func lapStop() {
        guard let total = lapTimer.stop(nowMs: nowMs) else { return }
        Haptics.light()
        log("INFO", String(format: "计时结束，总时 %.2f s", total))
    }

    public func lapReset() {
        lapTimer.reset()
    }

    public func clearRecords() {
        recordsStore.clear() // persists
        log("INFO", "纪录已清空")
    }

    // ---- bench calibration (TC275 DPT 0x70~0x74, doc 17 §2.3/§8/§9) ----------

    /// 标定四项前提（缺几项显示在①标题右侧）：WS + CTRL + TC275 在线 + 离地确认。
    public var calibPrereqMissing: [String] {
        var missing: [String] = []
        if connState != .connected { missing.append("未连接") }
        if !ctrlRole { missing.append("无控制权") }
        if !tcUp { missing.append("TC275 离线") }
        if !calib.wheelsOffConfirmed { missing.append("未确认四轮离地") }
        return missing
    }

    public var calibStartGateOpen: Bool {
        calibPrereqMissing.isEmpty && !calib.windowOpen && !emergActive
    }

    /// 点动故障门禁（doc 17 §8.1 jogFaultGated）：只看新鲜遥测（1 s 内且
    /// fault != 0）；台架上没跑起来（无遥测）时不锁死按钮。
    public var jogFaultGated: Bool {
        guard let t = telemetry, (nowMs - teleAtMs) < 1000 else { return false }
        return SafetyMonitor.faultActive(t.faultCode)
    }

    public var jogGateOpen: Bool {
        connState == .connected && ctrlRole && tcUp
            && !emergActive && !stopLatched
            && !calib.windowOpen && calib.jogChannel == nil
            && !jogFaultGated
    }

    /// 四轮离地确认（①的安全前提）
    public func setWheelsOffConfirmed(_ on: Bool) {
        calib.setWheelsOffConfirmed(on)
    }

    /// 开始判向（0x70）：一次确认恰好一帧；运行窗口内按钮锁存。
    public func startDirectionCalib() {
        guard calibStartGateOpen else {
            log("WARN", "标定前提未满足：\(calibPrereqMissing.joined(separator: "、"))")
            return
        }
        calib.start(nowMs: nowMs)
        link?.sendDPT(Proto.Cmd.dptCalDir)
        log("WARN", "判向标定启动 — 车轮将逐个转动，保持四轮离地")
    }

    /// 按住即转（0x71 MOTOR_JOG，开环、不过伺服，受故障锁存门禁）
    public func jogPress(channel: Int, forward: Bool) {
        guard jogGateOpen else {
            log("WARN", "点动被拒：\(jogFaultGated ? "故障锁存中" : "前提未满足（连接/控制权/在线）")")
            return
        }
        if let f = calib.jogPress(channel: channel, forward: forward, nowMs: nowMs) {
            link?.sendDPT(Proto.Cmd.dptMotorJog, data: f.payload)
        }
    }

    public func jogRelease() {
        if let f = calib.jogRelease() {
            link?.sendDPT(Proto.Cmd.dptMotorJog, data: f.payload)
        }
    }

    /// REC_GET：读回当前生效参数（进入标定页时自动发一次）
    public func calibRecGet() {
        guard connState == .connected, ctrlRole else { return }
        link?.sendDPT(Proto.Cmd.dptRecGet)
    }

    /// REC_SET：参数生效 + TC275 写 DFlash；回执 0x23 是生效回显，
    /// 最终写入以标定回执 saved / 串口为准（doc 17 §8.3）。
    public func calibRecSet(pos: [Int], invert: [Int],
                            fullScaleMmS: Int, wheelDiaMm: Int) {
        guard connState == .connected, ctrlRole else {
            log("WARN", "写参数需已连接且取得控制权")
            return
        }
        guard CalibWire.recSetValid(pos: pos, invert: invert,
                                    fullScaleMmS: fullScaleMmS, wheelDiaMm: wheelDiaMm) else {
            log("WARN", "参数非法：fullScale 100..5000、轮径 30..200、位置需四值唯一")
            return
        }
        link?.sendDPT(Proto.Cmd.dptRecSet,
                      data: CalibWire.recSet(pos: pos, invert: invert,
                                             fullScaleMmS: fullScaleMmS, wheelDiaMm: wheelDiaMm))
        log("INFO", String(format: "REC_SET — fullScale %d mm/s · 轮径 %d mm",
                           fullScaleMmS, wheelDiaMm))
    }

    /// REC_CLEAR：擦 DFlash 恢复默认，闭环使能门随之重新关闭（doc 34 §13）
    public func calibRecClear() {
        guard connState == .connected, ctrlRole else {
            log("WARN", "恢复默认需已连接且取得控制权")
            return
        }
        link?.sendDPT(Proto.Cmd.dptRecClear)
        log("WARN", "REC_CLEAR — 擦除标定记录，系统回到默认值（开环等价）")
    }

    /// 标定页 STOP：点动归零 + 驾驶零目标锁存。永远可用（doc 17 §2.3 互锁 5）。
    public func calibStop() {
        jogRelease()
        stopPressed()
    }

    /// 离开标定页/切走 Tab：先停点动再关链路语义（doc 17「驾驶台返回」）
    public func calibLeave() {
        jogRelease()
    }

    // ---- calibration receive path (called by LinkEngine) ------------------------

    func applyCalibResult(_ r: CalibResult) {
        calib.receive(result: r, nowMs: nowMs)
        switch r.status {
        case 0:
            log("INFO", "判向标定完成 — \(r.savedText)；invert=\(r.invert.map { $0 > 0 ? "+1" : "-1" }.joined(separator: " "))")
            Haptics.medium()
        case 1:
            log("WARN", "判向标定中止（急停）")
        case 2:
            log("WARN", "判向标定忙 — 已有标定进行中")
        default:
            break
        }
    }

    func applyCalibRecord(_ r: CalibRecord) {
        calib.receive(record: r)
    }

    func applyJogCounts(on: Bool, deltas: [Int]) {
        calib.receiveJogCount(on: on, deltas: deltas)
    }

    // ---- 30 Hz beat: drive + safety watch (called by LinkEngine) -----------------

    func controlTick() {
        nowMs = now()
        let connected = connState == .connected
        calib.tick(nowMs: nowMs)

        // Stunt safety: any closed gate kills the macro within one tick.
        if sequencer.active,
           !connected || !ctrlRole || controller.emergLatch || controller.stopLatch {
            abortStunt(reason: !connected ? "连接断开" : "安全锁存")
        }

        // Axis priority: stunt macro > tilt steering > joystick (last writer
        // wins in DriveController; only one of the three is user-driven).
        if sequencer.active {
            let axes = sequencer.tick(dtMs: 1000 / Double(LinkEngine.controlRateHz))
            controller.joyV = axes.v
            controller.joyW = axes.w
        } else if tiltEnabled {
            controller.joyV = tiltAxes.v
            controller.joyW = tiltAxes.w
        }

        // Bench jog keepalive: 30 Hz while a wheel is held — the firmware's
        // 300 ms no-refresh auto-stop is the backstop (doc 34 §9.3).
        if connected, ctrlRole, let f = calib.jogKeepalive() {
            link?.sendDPT(Proto.Cmd.dptMotorJog, data: f.payload)
        }

        if let cmd = controller.tick(connUp: connected, ctrlRole: ctrlRole) {
            // Calibration window / active jog: TC275 忽略驾驶目标，但标定结束
            // 瞬间会恢复执行最新目标 — 心跳保持零目标防突跳（doc 17 §2.3 互锁 4）。
            let suppressed = calib.windowOpen || calib.jogChannel != nil
            let out = suppressed ? DriveCommand(v: 0, w: 0) : cmd
            link?.sendDrive(out)
            outV = out.v
            outW = out.w
        } else if !connected {
            outV = 0
            outW = 0
        }

        // engine hum follows the actually-sent output
        let moving = controller.gateOpen && (abs(Int(outV)) > 2 || abs(Int(outW)) > 20)
        sound.update(active: connected && ctrlRole && moving,
                     speedNorm: abs(Double(outV)) / DriveController.fullV)

        safetyTick()
    }

    /// ScenePhase → background: anything autonomous stops here. The vehicle
    /// itself is protected by its own heartbeats either way.
    public func handleBackground() {
        abortStunt(reason: "退到后台")
        if tiltEnabled { setTiltEnabled(false) }
        jogRelease() // 等价 web visibilitychange：后台必停点动
        joystickMoved(v: 0, w: 0)
        sound.update(active: false, speedNorm: 0)
        camera.stop() // hand back the single-viewer slot
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
                             detail: FaultText.describe(code)) // 中文 + 未知码保留 hex
            kinds.insert(.vehicleFault)
        } else if let b = batteryKind {
            newAlert = Alert(kind: b,
                             level: b == .criticalBattery ? .critical : .warning,
                             detail: "\(telemetry?.batteryPct ?? 0) %")
            kinds.insert(b)
        }
        ackDismissed.formIntersection(kinds)
        if let a = newAlert, ackDismissed.contains(a.kind) { newAlert = nil }
        let previousKind = alert?.kind
        alert = newAlert
        if let a = newAlert, a.kind != previousKind {
            announcer.alertAppeared(a) // VoiceOver: full-screen alerts are announced
        }
    }

    public func ackAlert() {
        guard let kind = alert?.kind else { return }
        ackDismissed.insert(kind)
        log("INFO", "告警确认 (ACK)")
    }
}
