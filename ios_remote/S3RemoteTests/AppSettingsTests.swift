/*
 * AppSettingsTests — the persisted settings JSON must stay forward-compatible:
 * a v1 blob (before the play features) decodes with defaults for the new keys
 * instead of failing load() and wiping the saved host. Since v1.3 the token
 * is runtime-only: encoding must NOT include it (Keychain owns persistence),
 * while legacy blobs still decode the token for the launch migration.
 */

import XCTest
@testable import S3Remote

final class AppSettingsTests: XCTestCase {
    func testV1JSONWithoutPlayKeysDecodesWithDefaults() throws {
        let v1 = #"{"host":"10.0.0.7","token":"abc123","deadzone":0.12,"mode":"sport"}"#
        let s = try JSONDecoder().decode(AppSettings.self, from: Data(v1.utf8))
        XCTAssertEqual(s.host, "10.0.0.7")
        XCTAssertEqual(s.token, "abc123", "legacy token decodes for migration")
        XCTAssertEqual(s.deadzone, 0.12, accuracy: 0.0001)
        XCTAssertEqual(s.mode, .sport)
        XCTAssertFalse(s.soundEnabled)
        XCTAssertEqual(s.trackWidthMm, 150, accuracy: 0.001)
        XCTAssertEqual(s.tiltSensitivity, 1.0, accuracy: 0.001)
        XCTAssertFalse(s.cameraEnabled, "camera keys absent → defaults")
        XCTAssertEqual(s.cameraHost, "")
    }

    func testRoundTripOmitsTokenAndKeepsCameraKeys() throws {
        var s = AppSettings()
        s.soundEnabled = true
        s.trackWidthMm = 210
        s.tiltSensitivity = 1.4
        s.cameraEnabled = true
        s.cameraHost = "10.1.1.1"
        s.token = "secret-runtime-token"

        let data = try JSONEncoder().encode(s)
        let json = try XCTUnwrap(try JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertNil(json["token"], "token must never be serialized to UserDefaults")
        XCTAssertEqual(json["cameraEnabled"] as? Bool, true)
        XCTAssertEqual(json["cameraHost"] as? String, "10.1.1.1")

        var back = try JSONDecoder().decode(AppSettings.self, from: data)
        back.token = s.token // runtime field is not part of the persisted form
        XCTAssertEqual(back, s)
    }

    func testLoadAndSaveWithInjectedDefaults() {
        let name = "test.settings.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }

        var s = AppSettings(host: "1.2.3.4")
        s.cameraEnabled = true
        s.save(to: defaults)
        let loaded = AppSettings.load(from: defaults)
        XCTAssertEqual(loaded.host, "1.2.3.4")
        XCTAssertTrue(loaded.cameraEnabled)
    }
}
