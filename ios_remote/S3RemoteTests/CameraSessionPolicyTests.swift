/*
 * CameraSessionPolicyTests — reconnect/backoff math, stall detection and
 * HTTP-status classification for the MJPEG viewer (gateway: busy 503 arrives
 * only after the handler yields → short timeouts + exponential backoff).
 */

import XCTest
@testable import S3Remote

final class CameraSessionPolicyTests: XCTestCase {
    func testBackoffSequenceCapsAtMax() {
        var p = CameraSessionPolicy()
        XCTAssertEqual(p.nextRetryDelay(), 0.8, accuracy: 0.0001)
        XCTAssertEqual(p.nextRetryDelay(), 1.6, accuracy: 0.0001)
        XCTAssertEqual(p.nextRetryDelay(), 3.2, accuracy: 0.0001)
        XCTAssertEqual(p.nextRetryDelay(), 4.0, accuracy: 0.0001)
        XCTAssertEqual(p.nextRetryDelay(), 4.0, accuracy: 0.0001) // stays capped
    }

    func testResetRestartsBackoff() {
        var p = CameraSessionPolicy()
        _ = p.nextRetryDelay()
        _ = p.nextRetryDelay()
        p.reset()
        XCTAssertEqual(p.nextRetryDelay(), 0.8, accuracy: 0.0001)
    }

    func testStallDetection() {
        XCTAssertFalse(CameraSessionPolicy.stalled(lastByteAtMs: 1000, nowMs: 3000)) // exactly at threshold
        XCTAssertTrue(CameraSessionPolicy.stalled(lastByteAtMs: 1000, nowMs: 3000.5))
        XCTAssertFalse(CameraSessionPolicy.stalled(lastByteAtMs: 1000, nowMs: 1500))
    }

    func testStatusClassification() {
        XCTAssertEqual(CameraSessionPolicy.classify(status: 200), .connecting)
        XCTAssertEqual(CameraSessionPolicy.classify(status: 503), .busy)
        XCTAssertEqual(CameraSessionPolicy.classify(status: 500), .failed("HTTP 500"))
        XCTAssertEqual(CameraSessionPolicy.classify(status: 404), .failed("HTTP 404"))
    }
}
