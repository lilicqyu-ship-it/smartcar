/*
 * SystemTests — AppState-level integration over complete user journeys:
 * connection lifecycle, the drive loop (limits/STOP/e-stop), the play loop
 * (stunt → trail/records), display pipelines (speed EMA / battery incl.
 * vehicle-reboot reset), camera ownership, and the pairing HTTP journey
 * against a stubbed URLSession (403/409/504/503/200 → token → Keychain).
 * LinkEngine-side network I/O is mocked via URLProtocol; everything else is
 * the real production state machine.
 */

import XCTest
@testable import S3Remote

// ---- URLProtocol stub for LinkEngine.pair ------------------------------------------

final class MockPairURLProtocol: URLProtocol {
    nonisolated(unsafe) static var handler: ((URLRequest) -> (Int, Data))?

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        guard let handler = Self.handler, let url = request.url else { return }
        let (status, data) = handler(request)
        let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: nil, headerFields: nil)!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

@MainActor
final class SystemTests: XCTestCase {
    /// Fresh world per test (no shared non-Sendable state across the XCTest
    /// bridge): isolated suite defaults, in-memory token store, connected app.
    private func makeWorld(cameraEnabled: Bool = false) -> (app: AppState, store: MockTokenStore,
                                                            defaults: UserDefaults, suite: String) {
        let suite = "test.system.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        let store = MockTokenStore()
        let app = AppState(settings: AppSettings(host: "127.0.0.1", cameraEnabled: cameraEnabled),
                           tokenStore: store, defaults: defaults)
        return (app, store, defaults, suite)
    }

    // ---- helpers -----------------------------------------------------------------

    private func connectAsCtrl(_ app: AppState) {
        app.handleConnecting()
        app.handleOpen()
        app.applyHello(role: .ctrl, ver: "vTest", tcUp: true, pair: nil, ctrlHeld: true, rssi: nil)
    }

    private func telemetry(vL: Int16, vR: Int16, pct: UInt8 = 80, mv: UInt16 = 8000,
                           uptime: UInt32 = 100_000, odoSession: UInt32 = 0) -> Telemetry {
        var t = Telemetry()
        t.seq &+= 1
        t.uptimeMs = uptime
        t.vMeasL = vL
        t.vMeasR = vR
        t.batteryPct = pct
        t.batteryMv = mv
        t.odoSessionMm = odoSession
        return t
    }

    // ---- journey 1: connection lifecycle -----------------------------------------

    func testConnectionJourneyAndFullResetOnDown() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        app.handleConnecting()
        XCTAssertEqual(app.connState, .connecting)
        connectAsCtrl(app)
        XCTAssertTrue(app.ctrlRole)

        for _ in 0..<10 { app.controlTick() } // heartbeat with zero joystick
        XCTAssertEqual(app.outV, 0)

        app.joystickMoved(v: 1, w: 0)
        for _ in 0..<3 { app.controlTick() }
        XCTAssertEqual(app.outV, 480, "NORMAL caps at 80 % of 600 mm/s")

        app.applyTelemetry(telemetry(vL: 500, vR: 500))
        XCTAssertNotEqual(app.displaySpeedMmS, 0)

        app.handleDown("unit")
        XCTAssertEqual(app.connState, .disconnected)
        XCTAssertFalse(app.ctrlRole)
        XCTAssertEqual(app.outV, 0)
        XCTAssertEqual(app.displaySpeedMmS, 0, "speed display resets with the link")
        XCTAssertEqual(app.camera.state, .idle)
        XCTAssertFalse(app.sequencer.active)
    }

    // ---- journey 2: drive loop (limits / STOP / e-stop) ----------------------------

    func testDriveLoopLimitsStopAndEmergency() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        connectAsCtrl(app)
        app.setMode(.eco)
        app.joystickMoved(v: 1, w: 0)
        for _ in 0..<2 { app.controlTick() }
        XCTAssertEqual(app.outV, 300, "ECO caps at 50 %")

        app.stopPressed() // single click: latch + immediate DRIVE(0,0)
        XCTAssertEqual(app.outV, 0)
        XCTAssertTrue(app.stopLatched)
        app.joystickMoved(v: 1, w: 0) // axes set, but latch gates the output
        for _ in 0..<2 { app.controlTick() }
        XCTAssertEqual(app.outV, 0)

        app.joystickTouch() // touching the joystick clears the STOP latch
        app.joystickMoved(v: 1, w: 0)
        for _ in 0..<2 { app.controlTick() }
        XCTAssertEqual(app.outV, 300)

        app.emergencyTriggered()
        XCTAssertTrue(app.emergActive)
        for _ in 0..<2 { app.controlTick() }
        XCTAssertEqual(app.outV, 0, "emergency latches everything to zero")
        app.emergencyRelease()
        XCTAssertFalse(app.emergActive)
        XCTAssertTrue(app.stopLatched, "RELEASE keeps the stop latch until takeover")
        app.joystickTouch()
        for _ in 0..<2 { app.controlTick() }
        XCTAssertEqual(app.outV, 300)
    }

    // ---- journey 3: play loop (stunt drives, STOP kills, trail/records grow) -------

    func testPlayJourneyStuntTrailAndRecords() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        connectAsCtrl(app)
        app.startStunt(Stunt.library[0]) // 原地左旋
        XCTAssertTrue(app.sequencer.active)
        app.controlTick()
        XCTAssertNotEqual(app.controller.joyW, 0, "stunt axes drive the controller")

        app.stopPressed()
        XCTAssertFalse(app.sequencer.active, "STOP aborts the stunt")
        XCTAssertTrue(app.stopLatched)

        app.joystickTouch()
        // a burst of forward telemetry: trail + speed record must grow
        for _ in 0..<30 { app.applyTelemetry(telemetry(vL: 500, vR: 500, odoSession: 2_000)) }
        XCTAssertGreaterThan(app.odometry.points.count, 2)
        XCTAssertGreaterThan(app.odometry.distanceMm, 0)
        XCTAssertEqual(app.recordsStore.records.topSpeedKmh, 1.8, accuracy: 0.05)
        app.clearTrail()
        XCTAssertEqual(app.odometry.points.count, 1)
    }

    // ---- journey 4: display pipelines -----------------------------------------------

    func testDisplayPipelineSpeedSnapAndBatteryReboot() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        connectAsCtrl(app)
        // speed EMA snaps on the first frame instead of ramping from zero
        app.applyTelemetry(telemetry(vL: 400, vR: 400))
        XCTAssertEqual(app.displaySpeedMmS, 400, accuracy: 0.001)

        // battery seeds from the first valid sample
        app.applyTelemetry(telemetry(vL: 0, vR: 0, pct: 50, mv: 7500))
        XCTAssertEqual(app.batteryDisplayPct, 50)

        // vehicle restart (uptime regression): display snaps to the truth
        app.applyTelemetry(telemetry(vL: 0, vR: 0, pct: 100, mv: 8400, uptime: 3_000))
        XCTAssertEqual(app.batteryDisplayPct, 100)
        XCTAssertEqual(app.batteryDisplayMv, 8400)
    }

    // ---- journey 5: camera ownership -------------------------------------------------

    func testCameraOwnershipFollowsTabAndSetting() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        app.syncCamera(activeTab: Tab.drive.rawValue)
        XCTAssertFalse(app.camera.isRunning, "disabled camera never starts")

        app.settings.cameraEnabled = true
        app.syncCamera(activeTab: Tab.drive.rawValue)
        XCTAssertTrue(app.camera.isRunning)

        app.syncCamera(activeTab: Tab.play.rawValue)
        XCTAssertFalse(app.camera.isRunning, "leaving the drive tab frees the viewer slot")

        app.syncCamera(activeTab: Tab.drive.rawValue)
        XCTAssertTrue(app.camera.isRunning)
        app.handleBackground()
        XCTAssertFalse(app.camera.isRunning, "background hands the slot back")
    }

    // ---- journey 6: pairing HTTP mapping ----------------------------------------------

    private func makePairEngine(_ app: AppState) -> LinkEngine {
        let engine = LinkEngine(app: app)
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [MockPairURLProtocol.self]
        engine.pairSession = URLSession(configuration: config)
        app.link = engine
        return engine
    }

    private func stubPair(_ status: Int, body: String = "{}") {
        MockPairURLProtocol.handler = { _ in (status, Data(body.utf8)) }
    }

    func testPairingFailureMapping() async {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        let engine = makePairEngine(app)
        stubPair(403)
        var message = await engine.pair()
        XCTAssertTrue(message.contains("配对键"), "403 → open the window first")
        stubPair(409)
        message = await engine.pair()
        XCTAssertTrue(message.contains("占用"), "409 → occupied by another controller")
        stubPair(504)
        message = await engine.pair()
        XCTAssertTrue(message.contains("超时"), "504 → timeout")
        stubPair(503)
        message = await engine.pair()
        XCTAssertTrue(message.contains("TC275"), "503 → TC275 offline")
    }

    func testPairingSuccessStoresTokenInKeychain() async {
        let (app, store, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        let engine = makePairEngine(app)
        stubPair(200, body: #"{"token":"tok-abc123"}"#)
        let message = await engine.pair()
        XCTAssertTrue(message.contains("配对成功"))
        XCTAssertEqual(store.get(), "tok-abc123", "token must persist via the Keychain store")
        XCTAssertEqual(app.settings.token, "tok-abc123", "runtime copy feeds the WS URL")
    }

    // ---- journey 7: stunt/zero-write semantics at the app level -----------------------

    func testZeroJoystickWriteDoesNotKillRunningStunt() {
        let (app, _, _, suite) = makeWorld()
        defer { UserDefaults(suiteName: suite)!.removePersistentDomain(forName: suite) }
        connectAsCtrl(app)
        app.startStunt(Stunt.library[0])
        app.joystickMoved(v: 0, w: 0) // UI reset emitted by the disabled joystick
        XCTAssertTrue(app.sequencer.active)
        app.controlTick()
        XCTAssertNotEqual(app.controller.joyW, 0, "sequencer still owns the axes")
        app.joystickMoved(v: 0.5, w: 0.5) // a real push takes over
        XCTAssertFalse(app.sequencer.active)
    }
}
