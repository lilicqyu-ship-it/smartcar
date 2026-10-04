/*
 * SettingsMigrationTests — the v1.2→v1.3 launch migration as a whole:
 * a legacy UserDefaults blob that still carries the pairing token is decoded,
 * the token moves to the (mock) Keychain, the persisted blob is rewritten
 * WITHOUT the token, and host/preferences survive untouched.
 */

import XCTest
@testable import S3Remote

@MainActor
final class SettingsMigrationTests: XCTestCase {
    private var suiteName: String!
    private var defaults: UserDefaults!

    override func setUp() {
        super.setUp()
        suiteName = "test.migration.\(UUID().uuidString)"
        defaults = UserDefaults(suiteName: suiteName)
    }

    override func tearDown() {
        defaults.removePersistentDomain(forName: suiteName)
        defaults = nil
        suiteName = nil
        super.tearDown()
    }

    private func seedLegacyBlob() {
        let legacy = #"{"host":"10.0.0.7","token":"legacy-tok","deadzone":0.12,"mode":"sport","soundEnabled":true}"#
        defaults.set(Data(legacy.utf8), forKey: "s3remote.settings.v1")
    }

    func testLegacyTokenMovesToKeychainAndLeavesTheBlob() throws {
        seedLegacyBlob()
        let store = MockTokenStore()
        let app = AppState(settings: .load(from: defaults), tokenStore: store, defaults: defaults)

        XCTAssertEqual(store.get(), "legacy-tok", "token must land in the Keychain")
        XCTAssertEqual(app.settings.token, "legacy-tok", "runtime value stays available for the WS URL")

        // the persisted blob must be rewritten without the token
        let saved = try XCTUnwrap(defaults.data(forKey: "s3remote.settings.v1"))
        let json = try XCTUnwrap(try JSONSerialization.jsonObject(with: saved) as? [String: Any])
        XCTAssertNil(json["token"], "token must not persist in UserDefaults anymore")
        XCTAssertEqual(json["host"] as? String, "10.0.0.7")
        XCTAssertEqual(json["soundEnabled"] as? Bool, true)

        // relaunch with the clean blob: token comes from the Keychain
        let second = AppState(settings: .load(from: defaults), tokenStore: store, defaults: defaults)
        XCTAssertEqual(second.settings.token, "legacy-tok")
    }

    func testFreshInstallHasNoTokenAndNoMigration() {
        let store = MockTokenStore()
        let app = AppState(settings: .load(from: defaults), tokenStore: store, defaults: defaults)
        XCTAssertTrue(store.get().isEmpty)
        XCTAssertTrue(app.settings.token.isEmpty)
        XCTAssertTrue(OnboardingState.shouldShow(defaults), "fresh install shows onboarding")
    }

    func testResetPairingClearsBothLayers() {
        seedLegacyBlob()
        let store = MockTokenStore()
        let app = AppState(settings: .load(from: defaults), tokenStore: store, defaults: defaults)
        app.setToken("")
        XCTAssertTrue(store.get().isEmpty)
        XCTAssertTrue(app.settings.token.isEmpty)
        let saved = try? JSONSerialization.jsonObject(with: defaults.data(forKey: "s3remote.settings.v1") ?? Data()) as? [String: Any]
        XCTAssertNil(saved?["token"])
    }
}
