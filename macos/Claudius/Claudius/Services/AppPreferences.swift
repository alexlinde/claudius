import Foundation
import Security

enum AppPreferences {
    private enum Keys {
        static let name = "companionName"
        static let port = "companionPort"
        static let claudeBinary = "claudeBinary"
    }

    static var name: String {
        get {
            let stored = UserDefaults.standard.string(forKey: Keys.name)?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            return stored.isEmpty ? defaultHostName() : stored
        }
        set { UserDefaults.standard.set(newValue, forKey: Keys.name) }
    }

    /// Bonjour-friendly local hostname (e.g. `aspern-tallow`).
    static func defaultHostName() -> String {
        let host = ProcessInfo.processInfo.hostName
            .replacingOccurrences(of: ".local", with: "")
            .lowercased()
        return host.isEmpty ? "my-mac" : host
    }

    static var port: Int {
        get {
            // Range-check: a bogus defaults value must not trap the UInt16 conversion
            // in WebSocketServer (`defaults write … companionPort 999999`).
            let v = UserDefaults.standard.integer(forKey: Keys.port)
            return (v > 0 && v <= 65535) ? v : CompanionConstants.wsPortDefault
        }
        set { UserDefaults.standard.set(newValue, forKey: Keys.port) }
    }

    static var claudeBinary: String {
        get {
            let v = UserDefaults.standard.string(forKey: Keys.claudeBinary) ?? ""
            return v.isEmpty ? "claude" : v
        }
        set { UserDefaults.standard.set(newValue, forKey: Keys.claudeBinary) }
    }

    /// Shared secret for WebSocket auth.
    ///
    /// Throws rather than defaulting to `""` on Keychain trouble: an empty secret
    /// means "no authentication", so a locked/denied Keychain must fail closed and
    /// keep the server from coming up wide open.
    static func loadSecret() throws -> String {
        try KeychainSecret.load()
    }

    /// Store (or clear, when empty) the shared secret. Throws on Keychain failure so
    /// the UI can't claim "Saved" for a write that never landed.
    static func setSecret(_ value: String) throws {
        if value.isEmpty {
            try KeychainSecret.delete()
        } else {
            try KeychainSecret.save(value)
        }
    }
}

/// Keychain failure that must not be mistaken for "no secret configured".
struct KeychainError: LocalizedError {
    let operation: String
    let status: OSStatus

    var errorDescription: String? {
        let detail = SecCopyErrorMessageString(status, nil) as String? ?? "OSStatus \(status)"
        return "Keychain \(operation) failed: \(detail) (\(status))"
    }
}

private enum KeychainSecret {
    private static let service = "com.gmclaude.claudius"
    private static let account = "companionSecret"

    /// Empty string only when the item genuinely doesn't exist.
    static func load() throws -> String {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound {
            return ""
        }
        guard status == errSecSuccess else {
            throw KeychainError(operation: "read", status: status)
        }
        guard let data = item as? Data,
              let s = String(data: data, encoding: .utf8)
        else {
            throw KeychainError(operation: "read", status: errSecDecode)
        }
        return s
    }

    static func save(_ value: String) throws {
        try delete()
        let data = Data(value.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]
        let status = SecItemAdd(query as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw KeychainError(operation: "write", status: status)
        }
    }

    static func delete() throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw KeychainError(operation: "delete", status: status)
        }
    }
}
