/*
 * TokenStore.swift — the pairing token lives in the Keychain, not in the
 * UserDefaults settings blob (README 已知限制的收口)。The protocol keeps
 * tests hermetic (in-memory mock); the Keychain store is a thin SecItem
 * wrapper around one generic-password item.
 */

import Foundation
import Security

public protocol TokenStoring: Sendable {
    func get() -> String
    func set(_ value: String)
    func remove()
}

public struct KeychainTokenStore: TokenStoring {
    public let service: String

    public init(service: String = "com.smartcar.s3remote.token") {
        self.service = service
    }

    private var baseQuery: [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: service,
         kSecAttrAccount as String: "pairing"]
    }

    public func get() -> String {
        var query = baseQuery
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        var result: AnyObject?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        guard status == errSecSuccess, let data = result as? Data else { return "" }
        return String(data: data, encoding: .utf8) ?? ""
    }

    public func set(_ value: String) {
        guard !value.isEmpty else { remove(); return }
        var add = baseQuery
        add[kSecValueData as String] = Data(value.utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        let status = SecItemAdd(add as CFDictionary, nil)
        if status == errSecDuplicateItem {
            SecItemUpdate(baseQuery as CFDictionary,
                          [kSecValueData as String: Data(value.utf8)] as CFDictionary)
        }
    }

    public func remove() {
        SecItemDelete(baseQuery as CFDictionary)
    }
}

/// In-memory stand-in for tests and previews.
public final class MockTokenStore: TokenStoring, @unchecked Sendable {
    public private(set) var value: String

    public init(initial: String = "") {
        value = initial
    }

    public func get() -> String { value }

    public func set(_ value: String) {
        self.value = value
    }

    public func remove() {
        value = ""
    }
}
