/*
 * TokenStoreTests — token persistence contract (get/set/remove) on the mock,
 * plus a real-Keychain smoke test with a unique service so it never collides
 * with the app's stored pairing token.
 */

import XCTest
@testable import S3Remote

final class TokenStoreTests: XCTestCase {
    func testMockSemantics() {
        let store = MockTokenStore()
        XCTAssertEqual(store.get(), "")
        store.set("tok-1")
        XCTAssertEqual(store.get(), "tok-1")
        store.set("tok-2")
        XCTAssertEqual(store.get(), "tok-2")
        store.remove()
        XCTAssertEqual(store.get(), "")
    }

    /// Real SecItem round trip (simulator keychain). Unique service per run.
    func testKeychainRoundTrip() {
        let store = KeychainTokenStore(service: "test.s3remote.\(UUID().uuidString)")
        defer { store.remove() }
        XCTAssertEqual(store.get(), "")
        store.set("pairing-token")
        XCTAssertEqual(store.get(), "pairing-token")
        store.set("rotated-token")
        XCTAssertEqual(store.get(), "rotated-token")
        store.remove()
        XCTAssertEqual(store.get(), "")
    }

    func testKeychainSetEmptyRemoves() {
        let store = KeychainTokenStore(service: "test.s3remote.\(UUID().uuidString)")
        defer { store.remove() }
        store.set("x")
        store.set("")
        XCTAssertEqual(store.get(), "")
    }
}
