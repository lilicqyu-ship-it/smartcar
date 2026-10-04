/*
 * StuntSequencerTests — keyframe interpolation, lifecycle (start → run →
 * finish → rest) and abort semantics for the stunt macro player.
 */

import XCTest
@testable import S3Remote

final class StuntSequencerTests: XCTestCase {
    private let stunt = Stunt(id: "t", name: "test", subtitle: "", icon: "star",
                              keyframes: [StuntKeyframe(ms: 1000, v: 1, w: -0.5),
                                          StuntKeyframe(ms: 1000, v: 0, w: 0)])

    func testInactiveByDefault() {
        var s = StuntSequencer()
        XCTAssertFalse(s.active)
        XCTAssertEqual(s.tick(dtMs: 33).v, 0)
    }

    func testFirstKeyframeRampsFromRest() {
        var s = StuntSequencer()
        s.start(stunt)
        let at = s.tick(dtMs: 500)
        XCTAssertEqual(at.v, 0.5, accuracy: 0.001)
        XCTAssertEqual(at.w, -0.25, accuracy: 0.001)
    }

    func testSecondKeyframeInterpolatesFromPreviousTarget() {
        var s = StuntSequencer()
        s.start(stunt)
        _ = s.tick(dtMs: 1000) // reached keyframe 1 exactly
        let at = s.tick(dtMs: 500)
        XCTAssertEqual(at.v, 0.5, accuracy: 0.001)
        XCTAssertEqual(at.w, -0.25, accuracy: 0.001)
    }

    func testFinishesWithRestAxes() {
        var s = StuntSequencer()
        s.start(stunt)
        var last: (v: Double, w: Double) = (1, 1)
        for _ in 0..<70 { last = s.tick(dtMs: 33) } // 2310 ms > 2000 ms total
        XCTAssertFalse(s.active)
        XCTAssertEqual(last.v, 0)
        XCTAssertEqual(last.w, 0)
    }

    func testAbortResetsImmediately() {
        var s = StuntSequencer()
        s.start(stunt)
        _ = s.tick(dtMs: 300)
        s.abort()
        XCTAssertFalse(s.active)
        XCTAssertEqual(s.progress, 0)
        XCTAssertEqual(s.tick(dtMs: 33).v, 0)
    }

    func testRestartResetsProgress() {
        var s = StuntSequencer()
        s.start(stunt)
        _ = s.tick(dtMs: 900)
        s.start(stunt)
        XCTAssertEqual(s.progress, 0)
        let at = s.tick(dtMs: 0)
        XCTAssertEqual(at.v, 0, accuracy: 0.001) // back at rest
    }

    func testLibraryIsSane() {
        XCTAssertGreaterThanOrEqual(Stunt.library.count, 6)
        for stunt in Stunt.library {
            XCTAssertFalse(stunt.keyframes.isEmpty, stunt.id)
            XCTAssertGreaterThan(stunt.durationMs, 0, stunt.id)
            for kf in stunt.keyframes {
                XCTAssertGreaterThan(kf.ms, 0, stunt.id)
                XCTAssertTrue((-1.0...1.0).contains(kf.v), stunt.id)
                XCTAssertTrue((-1.0...1.0).contains(kf.w), stunt.id)
            }
        }
        // ids unique for grid identity
        let ids = Stunt.library.map(\.id)
        XCTAssertEqual(Set(ids).count, ids.count)
    }

    /// Spins must turn the documented way: w > 0 = left (firmware link.c).
    func testSpinLibraryDirections() {
        let spinL = Stunt.library.first { $0.id == "spinL" }!
        let spinR = Stunt.library.first { $0.id == "spinR" }!
        XCTAssertGreaterThan(spinL.keyframes[0].w, 0)
        XCTAssertLessThan(spinR.keyframes[0].w, 0)
    }
}
