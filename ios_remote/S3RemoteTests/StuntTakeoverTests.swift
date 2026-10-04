/*
 * StuntTakeoverTests — stunt lifecycle through AppState: a started stunt must
 * actually drive the axes, UI zero-writes (joystick disabling itself when the
 * stunt starts, tab switches) must NOT kill it, and only a real push takes
 * over. Regression for "tapping a stunt did nothing": the drive joystick's
 * disabled-reset emitted onChange(0,0) which was treated as a takeover and
 * aborted every stunt the instant it started.
 */

import XCTest
@testable import S3Remote

@MainActor
final class StuntTakeoverTests: XCTestCase {
    private func makeConnectedApp() -> AppState {
        let app = AppState(settings: AppSettings())
        app.handleConnecting()
        app.handleOpen()
        app.applyHello(role: .ctrl, ver: "test", tcUp: true, pair: nil, ctrlHeld: true, rssi: nil)
        return app
    }

    func testStartedStuntDrivesTheAxes() {
        let app = makeConnectedApp()
        app.startStunt(Stunt.library[0]) // 原地左旋
        XCTAssertTrue(app.sequencer.active)
        app.controlTick()
        XCTAssertEqual(app.controller.joyV, 0, accuracy: 0.001)
        XCTAssertGreaterThan(abs(app.controller.joyW), 0.01, "spin must feed nonzero steering")
        XCTAssertGreaterThan(abs(app.outW), 0, "a nonzero DRIVE frame must go out")
    }

    func testZeroJoystickWriteDoesNotAbortStunt() {
        let app = makeConnectedApp()
        app.startStunt(Stunt.library[0])
        app.controlTick()
        // the drive joystick zeroes itself when the stunt disables it
        app.joystickMoved(v: 0, w: 0)
        XCTAssertTrue(app.sequencer.active, "UI zero-write must not kill the stunt")
        app.controlTick()
        XCTAssertGreaterThan(abs(app.controller.joyW), 0.01, "axes still sequencer-driven")
        // tab-leave zeroing likewise
        app.joystickMoved(v: 0, w: 0)
        XCTAssertTrue(app.sequencer.active)
    }

    func testNonzeroJoystickPushTakesOver() {
        let app = makeConnectedApp()
        app.startStunt(Stunt.library[0])
        XCTAssertTrue(app.sequencer.active)
        app.joystickMoved(v: 0.5, w: -0.2)
        XCTAssertFalse(app.sequencer.active, "a real push must abort the stunt")
        XCTAssertEqual(app.controller.joyV, 0.5, accuracy: 0.001)
        XCTAssertEqual(app.controller.joyW, -0.2, accuracy: 0.001)
    }

    func testSecondTapOnRunningStuntAborts() {
        let app = makeConnectedApp()
        let stunt = Stunt.library[0]
        app.startStunt(stunt)
        XCTAssertTrue(app.sequencer.active)
        app.startStunt(stunt)
        XCTAssertFalse(app.sequencer.active, "tapping the running stunt again aborts it")
        XCTAssertEqual(app.controller.joyV, 0)
        XCTAssertEqual(app.controller.joyW, 0)
    }
}
