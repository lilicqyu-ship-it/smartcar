/*
 * CameraClient.swift — MJPEG viewer for the s3-gateway camera plane
 * (GET http://<host>:81/stream). Contract (doc/19-camera.md + contracts/camera):
 *   · one viewer at a time — a busy slot answers 503, but only after the
 *     handler yields, so requests must use a SHORT timeout and back off;
 *   · chunked multipart with a fixed boundary, every part carries
 *     Content-Length — frames are cut by length, never by scanning JPEG EOI;
 *   · viewers must detach when leaving the page / background to hand the
 *     single slot back.
 * Reconnect design mirrors the gateway's own web viewer (s3-gateway
 * assets_src/app.js): exponential backoff 0.8 s → 4 s, stall detection
 * (2 s without bytes → immediate reconnect).
 */

import UIKit

// ---- pure policy (unit-testable) ---------------------------------------------------

public struct CameraSessionPolicy: Sendable {
    public static let baseRetryS: Double = 0.8
    public static let maxRetryS: Double = 4.0
    public static let stallMs: Double = 2000

    private var attempt = 0

    public init() {}

    /// Exponential backoff for failed/busy attempts: 0.8, 1.6, 3.2, 4, 4 …
    public mutating func nextRetryDelay() -> Double {
        let delay = Self.baseRetryS * pow(2, Double(attempt))
        attempt += 1
        return min(delay, Self.maxRetryS)
    }

    /// A connection that produced bytes was healthy — backoff starts over.
    public mutating func reset() {
        attempt = 0
    }

    public static func stalled(lastByteAtMs: Double, nowMs: Double) -> Bool {
        nowMs - lastByteAtMs > Self.stallMs
    }

    public static func classify(status: Int) -> CameraClient.State {
        switch status {
        case 200: return .connecting
        case 503: return .busy
        default: return .failed("HTTP \(status)")
        }
    }
}

/// Incremental multipart/x-mixed-replace parser. Pure logic — fed arbitrary
/// network chunks, emits completed JPEG frames. Part layout from
/// camera_stream.c: "\r\n--<boundary>\r\n" + headers + "\r\n\r\n" + body,
/// with NO trailing CRLF after the body (the next part header carries its
/// own leading CRLF; the stream preamble also starts with it).
public struct MjpegParser: Sendable {
    public static let boundary = "123456789000000000000987654321"
    static let marker = Array("\r\n--\(Self.boundary)".utf8)
    static let headerTerminator: [UInt8] = [13, 10, 13, 10]
    static let headerCap = 1024

    enum Phase: Equatable, Sendable { case marker, headers, body }

    var phase: Phase = .marker
    var bodyRemaining = 0
    public private(set) var framesParsed = 0

    private var buf: [UInt8] = []

    public init() {}

    /// Feed one network chunk; returns every JPEG frame completed by it.
    public mutating func feed(_ data: Data) -> [Data] {
        buf.append(contentsOf: data)
        var frames: [Data] = []
        loop: while true {
            switch phase {
            case .marker:
                guard let r = buf.firstRange(of: Self.marker) else {
                    // keep a tail that could complete a marker, drop the rest
                    let keep = Self.marker.count - 1
                    if buf.count > keep { buf.removeFirst(buf.count - keep) }
                    break loop
                }
                buf.removeFirst(r.upperBound) // marker consumed; headers follow
                phase = .headers
            case .headers:
                guard let end = buf.firstRange(of: Self.headerTerminator) else {
                    if buf.count > Self.headerCap {
                        // runaway headers: skip this part, hunt the next marker
                        buf.removeAll()
                        phase = .marker
                    }
                    break loop
                }
                let headerText = String(decoding: buf[..<end.lowerBound], as: UTF8.self)
                buf.removeFirst(end.upperBound)
                phase = .marker
                if let len = Self.contentLength(in: headerText), len > 0 {
                    bodyRemaining = len
                    phase = .body
                }
                // no Content-Length → part skipped (contract guarantees it; tolerance)
            case .body:
                guard buf.count >= bodyRemaining else { break loop }
                let body = Data(buf.prefix(bodyRemaining))
                buf.removeFirst(bodyRemaining)
                framesParsed += 1
                frames.append(body)
                phase = .marker
            }
        }
        return frames
    }

    public mutating func reset() {
        buf.removeAll()
        phase = .marker
        bodyRemaining = 0
    }

    static func contentLength(in headers: String) -> Int? {
        for line in headers.split(separator: "\r\n") {
            let parts = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2 else { continue }
            if parts[0].trimmingCharacters(in: .whitespaces).lowercased() == "content-length" {
                return Int(parts[1].trimmingCharacters(in: .whitespaces))
            }
        }
        return nil
    }
}

// ---- live client --------------------------------------------------------------------

@MainActor
@Observable
public final class CameraClient {
    public enum State: Equatable, Sendable {
        case idle, connecting, live, busy, failed(String)
    }

    public private(set) var state: State = .idle
    public private(set) var frame: UIImage?
    public private(set) var fps: Int = 0

    /// Injected by AppState: (enabled, resolved host) from current settings.
    var configProvider: (() -> (enabled: Bool, host: String))?

    private var policy = CameraSessionPolicy()
    private var streamTask: Task<Void, Never>?
    private var monitorTask: Task<Void, Never>?
    private var lastByteAtMs: Double = 0
    private var framesThisSecond = 0
    private var fpsWindowStartMs: Double = 0
    private var runningHost = ""
    private var generation = 0

    public init() {}

    public var isRunning: Bool { streamTask != nil }

    /// Start streaming (idempotent); a host change while running restarts.
    func start() {
        guard let cfg = configProvider?(), cfg.enabled, !cfg.host.isEmpty else {
            stop()
            return
        }
        if streamTask != nil {
            if runningHost != cfg.host { restartStream() }
            return
        }
        policy.reset()
        spawnLoop()
    }

    /// Detach: frees the gateway's single-viewer slot (page leave/background).
    func stop() {
        generation += 1
        streamTask?.cancel()
        streamTask = nil
        monitorTask?.cancel()
        monitorTask = nil
        state = .idle
    }

    private func spawnLoop() {
        guard let cfg = configProvider?() else { return }
        generation += 1
        let gen = generation
        runningHost = cfg.host
        state = .connecting
        lastByteAtMs = Self.nowMs()
        streamTask = Task { [weak self] in await self?.runLoop(generation: gen) }
        if monitorTask == nil {
            monitorTask = Task { [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(for: .milliseconds(500))
                    self?.monitorTick()
                }
            }
        }
    }

    /// Immediate reconnect (stall or host change): no backoff — the stream
    /// was healthy, this is a transport blip, not a rejected client.
    private func restartStream() {
        generation += 1
        streamTask?.cancel()
        policy.reset()
        spawnLoop()
    }

    private func monitorTick() {
        guard streamTask != nil else { return }
        let now = Self.nowMs()
        if CameraSessionPolicy.stalled(lastByteAtMs: lastByteAtMs, nowMs: now) {
            restartStream()
            return
        }
        if now - fpsWindowStartMs >= 1000 {
            fps = framesThisSecond
            framesThisSecond = 0
            fpsWindowStartMs = now
        }
    }

    private func runLoop(generation gen: Int) async {
        var parser = MjpegParser()
        while generation == gen, !Task.isCancelled {
            guard let cfg = configProvider?(), cfg.enabled, !cfg.host.isEmpty else {
                stop()
                return
            }
            await readStream(host: cfg.host, parser: &parser)
            guard generation == gen, !Task.isCancelled else { return }
            let delay = policy.nextRetryDelay()
            state = .connecting // busy/failed chip reappears on the next response
            try? await Task.sleep(for: .seconds(delay))
        }
    }

    private func readStream(host: String, parser: inout MjpegParser) async {
        guard let url = URL(string: "http://\(host):81/stream") else {
            state = .failed("无效地址")
            return
        }
        var request = URLRequest(url: url)
        request.timeoutInterval = 4 // busy 503 arrives only after the handler yields
        do {
            let (bytes, response) = try await URLSession.shared.bytes(for: request)
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            state = CameraSessionPolicy.classify(status: status)
            guard status == 200 else { return }
            lastByteAtMs = Self.nowMs()
            fpsWindowStartMs = lastByteAtMs
            framesThisSecond = 0
            // AsyncBytes only offers byte iteration (its iterator buffers
            // internally); batch into ~8 KB chunks for the parser.
            var pending: [UInt8] = []
            pending.reserveCapacity(8192)
            for try await byte in bytes {
                lastByteAtMs = Self.nowMs()
                pending.append(byte)
                if pending.count >= 8192 {
                    for jpeg in parser.feed(Data(pending)) {
                        deliver(jpeg)
                    }
                    pending.removeAll(keepingCapacity: true)
                }
            }
            for jpeg in parser.feed(Data(pending)) {
                deliver(jpeg)
            }
        } catch is CancellationError {
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    private func deliver(_ jpeg: Data) {
        guard let image = UIImage(data: jpeg) else { return }
        frame = image
        framesThisSecond += 1
        if state != .live { state = .live }
    }

    static func nowMs() -> Double {
        Date.timeIntervalSinceReferenceDate * 1000
    }
}
