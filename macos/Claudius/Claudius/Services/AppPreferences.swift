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
            let v = UserDefaults.standard.integer(forKey: Keys.port)
            return v > 0 ? v : CompanionConstants.wsPortDefault
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

    static var secret: String {
        get { KeychainSecret.load() ?? "" }
        set {
            if newValue.isEmpty {
                KeychainSecret.delete()
            } else {
                KeychainSecret.save(newValue)
            }
        }
    }
}

private enum KeychainSecret {
    private static let service = "com.gmclaude.claudius"
    private static let account = "companionSecret"

    static func load() -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data,
              let s = String(data: data, encoding: .utf8)
        else { return nil }
        return s
    }

    static func save(_ value: String) {
        delete()
        let data = Data(value.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecValueData as String: data,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlock,
        ]
        SecItemAdd(query as CFDictionary, nil)
    }

    static func delete() {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
