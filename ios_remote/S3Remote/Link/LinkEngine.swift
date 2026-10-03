/*
 * LinkEngine.swift — port of scr_link.c (doc/04-link.md):
 *   WebSocket ws://<host>/ws?token=...  (binary = proto v2, text = JSON)
 *   on open → wait for hello (role decides CTRL); heartbeat via 30 Hz DRIVE
 *   1 Hz app-level {"t":"ping"} → pong RTT sample
 *   10 s silence watchdog → forced reconnect; 3 s retry after failures
 *   POST http://<host>/api/pair → token (403/409/504/503 mapped to actions)
 * All network callbacks hop onto the MainActor-owned AppState.
 */

import Foundation

@MainActor
public final class LinkEngine: NSObject, URLSessionWebSocketDelegate {
    private let app: AppState
    private var session: URLSession?
    private var task: URLSessionWebSocketTask?
    private var parser = ProtoParser()

    private var lastRxAtMs: Double = 0
    private var lastConnectAtMs: Double = 0
    private var pingPending = false
    private var pingSentMs: Double = 0
    private var txCount = 0
    private var rxCount = 0
    private var downHandled = false

    private var driveTask: Task<Void, Never>?
    private var housekeepingTask: Task<Void, Never>?
    private var reconnectTask: Task<Void, Never>?

    public static let watchdogMs: Double = 10_000
    public static let reconnectDelayS: UInt64 = 3
    public static let controlRateHz: Int = 30

    public init(app: AppState) {
        self.app = app
        super.init()
    }

    deinit {
        session?.invalidateAndCancel()
    }

    // ---- lifecycle -------------------------------------------------------------

    public func start() {
        guard driveTask == nil else { return }
        driveTask = Task { [weak self] in
            guard let interval = Self.controlInterval else { return }
            var next = ContinuousClock.now
            while !Task.isCancelled {
                guard let self else { return }
                self.app.controlTick()
                next += interval
                try? await Task.sleep(until: next, tolerance: .milliseconds(5), clock: .continuous)
            }
        }
        housekeepingTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(1))
                guard let self else { return }
                self.housekeeping()
            }
        }
        connect()
    }

    public func stop() {
        driveTask?.cancel()
        housekeepingTask?.cancel()
        reconnectTask?.cancel()
        driveTask = nil
        housekeepingTask = nil
        reconnectTask = nil
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
    }

    public func reconnect() {
        connect()
    }

    static var controlInterval: Duration? {
        Duration.milliseconds(1000 / controlRateHz)
    }

    static func makeURL(host: String, token: String) -> URL? {
        var comp = URLComponents()
        comp.scheme = "ws"
        comp.host = host
        comp.path = "/ws"
        if !token.isEmpty {
            comp.queryItems = [URLQueryItem(name: "token", value: token)]
        }
        return comp.url
    }

    private func connect() {
        reconnectTask?.cancel()
        reconnectTask = nil
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
        downHandled = false
        parser.reset()

        guard let url = Self.makeURL(host: app.settings.host, token: app.settings.token) else {
            app.handleDown("无效地址")
            return
        }
        let sess: URLSession
        if let session {
            sess = session
        } else {
            let s = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
            session = s
            sess = s
        }
        lastConnectAtMs = app.now()
        app.handleConnecting()
        let t = sess.webSocketTask(with: url)
        task = t
        t.resume()
        receive(t)
    }

    private func scheduleReconnect() {
        guard reconnectTask == nil else { return }
        reconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(Self.reconnectDelayS))
            guard let self, !Task.isCancelled else { return }
            self.reconnectTask = nil
            self.connect()
        }
    }

    private func housekeeping() {
        let now = app.now()
        switch app.connState {
        case .connected:
            // app-level latency probe (1 Hz, aligns with app.js / scr_link)
            pingPending = true
            pingSentMs = now
            sendText("{\"t\":\"ping\"}")
            // half-open watchdog: no rx for 10 s → forced reconnect (doc 04 §2)
            if now - lastRxAtMs > Self.watchdogMs {
                app.log("WARN", "链路静默 >10 s — 强制重连")
                connect()
                return
            }
        case .connecting:
            // stalled handshake (Wi-Fi not associated yet) → retry
            if now - lastConnectAtMs > 15_000 {
                connect()
            }
        case .disconnected:
            if now - lastConnectAtMs > Double(Self.reconnectDelayS) * 1000 {
                connect()
            }
        }
        app.publishStats(tx: txCount, rx: rxCount)
        txCount = 0
        rxCount = 0
    }

    // ---- send (try-only, never queues: doc 04 §7) --------------------------------

    public func sendDrive(_ cmd: DriveCommand) {
        sendFrame(ProtoFrame(cmd: Proto.Cmd.drive, seq: app.nextSeq(),
                             data: Wire.putI16(cmd.v) + Wire.putI16(cmd.w)))
    }

    public func sendEmergencyStop() {
        sendFrame(ProtoFrame(cmd: Proto.Cmd.emergencyStop, seq: app.nextSeq()))
    }

    public func sendFrame(_ frame: ProtoFrame) {
        guard app.connState == .connected, let task else { return }
        task.send(.data(frame.encode())) { [weak self] error in
            Task { @MainActor [weak self] in
                if error == nil { self?.txCount += 1 }
            }
        }
    }

    public func sendText(_ text: String) {
        guard app.connState == .connected, let task else { return }
        task.send(.string(text)) { _ in }
    }

    // ---- pairing (doc 04 §4: every failure maps to an action) ----------------------

    public func pair() async -> String {
        var comp = URLComponents()
        comp.scheme = "http"
        comp.host = app.settings.host
        comp.path = "/api/pair"
        guard let url = comp.url else { return "无效地址" }
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 8
        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            if status == 200,
               let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
               let token = obj["token"] as? String, !token.isEmpty {
                app.settings.token = token
                app.log("INFO", "配对成功 — token 已保存，重连以取得控制权")
                connect()
                return "配对成功，已保存 token 并重连"
            }
            let msg: String
            switch status {
            case 403: msg = "未进入配对窗口：请先长按车侧配对键 3 秒，再重试"
            case 409: msg = "车辆已被占用：已有其他控制器完成配对"
            case 504: msg = "配对超时：请检查车辆是否在线，然后重试"
            case 503: msg = "车辆链路断开（TC275 离线）"
            default: msg = "配对失败（HTTP \(status)）：检查网关后重试"
            }
            app.log("WARN", "配对失败：\(msg)")
            return msg
        } catch {
            let msg = "配对请求失败：\(error.localizedDescription)"
            app.log("WARN", msg)
            return msg
        }
    }

    // ---- receive ------------------------------------------------------------------

    private func receive(_ t: URLSessionWebSocketTask) {
        t.receive { [weak self] result in
            Task { @MainActor [weak self] in
                guard let self else { return }
                switch result {
                case .success(let msg):
                    self.lastRxAtMs = self.app.now()
                    self.handle(msg)
                    if self.task === t { self.receive(t) }
                case .failure(let error):
                    if self.task === t { self.handleDown(error.localizedDescription) }
                }
            }
        }
    }

    private func handle(_ msg: URLSessionWebSocketTask.Message) {
        switch msg {
        case .data(let d):
            handleBinary(d)
        case .string(let s):
            handleText(Data(s.utf8))
        @unknown default:
            break
        }
    }

    private func handleBinary(_ data: Data) {
        for event in parser.feed(data) {
            switch event {
            case .frame(let f):
                if f.cmd == Proto.Cmd.telemetry, let t = Telemetry.decode(f.data) {
                    rxCount += 1
                    app.applyTelemetry(t)
                }
            case .crcError, .formatError, .versionError:
                break // malformed bytes: counted only via parser state; ignore
            case .none:
                break
            }
        }
    }

    private func handleText(_ data: Data) {
        guard let msg = TextMessage.parse(data) else { return }
        switch msg {
        case .hello(let role, let ver, let tcUp, let pair, let ctrlHeld, let rssi):
            app.applyHello(role: role, ver: ver, tcUp: tcUp, pair: pair, ctrlHeld: ctrlHeld, rssi: rssi)
        case .tc(let on):
            app.applyTc(on)
        case .pong:
            if pingPending {
                let rtt = Int(app.now() - pingSentMs)
                if rtt >= 0 && rtt < 60_000 {
                    app.applyRtt(rtt)
                }
                pingPending = false
            }
        case .errAuth:
            app.applyAuthRejected()
        case .tcVer(let a, let b):
            app.applyTcVer(app: a, sbl: b)
        case .rssi(let dbm):
            app.applyRssi(dbm)
        }
    }

    // ---- URLSessionWebSocketDelegate ------------------------------------------------

    public nonisolated func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                                       didOpenWithProtocol protocol: String?) {
        Task { @MainActor [weak self] in self?.handleOpen(task: webSocketTask) }
    }

    public nonisolated func urlSession(_ session: URLSession, webSocketTask: URLSessionWebSocketTask,
                                       didCloseWith closeCode: URLSessionWebSocketTask.CloseCode, reason: Data?) {
        Task { @MainActor [weak self] in self?.handleClose(task: webSocketTask) }
    }

    private func handleOpen(task: URLSessionWebSocketTask) {
        guard self.task === task else { return }
        lastRxAtMs = app.now()
        downHandled = false
        app.handleOpen()
    }

    private func handleClose(task: URLSessionWebSocketTask) {
        guard self.task === task else { return }
        handleDown("closed")
    }

    private func handleDown(_ reason: String) {
        guard !downHandled else { return }
        downHandled = true
        app.handleDown(reason)
        scheduleReconnect()
    }
}
