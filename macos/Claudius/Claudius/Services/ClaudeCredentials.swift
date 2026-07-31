import Foundation
import Security

/// Claude OAuth access token for the usage API.
///
/// After a successful read of Claude Code's credentials (file or Keychain),
/// the blob is cached under Claudius's own Keychain identity so the 60s usage
/// loop does not keep touching the foreign item. Claude Code alone refreshes
/// tokens; when our cached access token expires (or the usage API returns 401)
/// we re-read from Claude Code — which may prompt if that item's ACL was reset.
enum ClaudeCredentials {
    private static let lock = NSLock()

    /// Load a usable access token from cache, or re-seed from Claude Code.
    static func loadAccessToken() -> String? {
        lock.lock()
        defer { lock.unlock() }
        return loadAccessTokenLocked(forceReseed: false)
    }

    /// Ignore cache validity and re-read from Claude Code (e.g. after a 401).
    static func loadAccessTokenForcingRefresh() -> String? {
        lock.lock()
        defer { lock.unlock() }
        return loadAccessTokenLocked(forceReseed: true)
    }

    private static func loadAccessTokenLocked(forceReseed: Bool) -> String? {
        if !forceReseed, let owned = OwnStore.load(), accessTokenValid(owned) {
            return owned.accessToken
        }

        // Silent re-read when possible (no Keychain UI).
        if let foreign = loadForeignOAuth(allowUI: false) {
            OwnStore.save(foreign)
            return foreign.accessToken
        }

        // Bootstrap / ACL was reset — may show Keychain UI once.
        if let foreign = loadForeignOAuth(allowUI: true) {
            OwnStore.save(foreign)
            return foreign.accessToken
        }

        // Last resort: stale cached token (may 401; caller can force-reseed).
        return forceReseed ? nil : OwnStore.load()?.accessToken
    }

    private static func accessTokenValid(_ oauth: OAuthTokens) -> Bool {
        guard !oauth.accessToken.isEmpty else { return false }
        guard oauth.expiresAtMs > 0 else { return true }
        let expires = Date(timeIntervalSince1970: Double(oauth.expiresAtMs) / 1000.0)
        return expires.timeIntervalSinceNow > CompanionConstants.oauthRefreshSkew
    }

    private static func loadForeignOAuth(allowUI: Bool) -> OAuthTokens? {
        if let fromFile = parseOAuth(fromFile: CompanionConstants.claudeCredsPath) {
            return fromFile
        }
        return parseOAuth(fromClaudeKeychainAllowUI: allowUI)
    }

    private static func parseOAuth(fromFile path: String) -> OAuthTokens? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return OAuthTokens(json: obj)
    }

    private static func parseOAuth(fromClaudeKeychainAllowUI allowUI: Bool) -> OAuthTokens? {
        var query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: CompanionConstants.claudeKeychainService,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        if !allowUI {
            query[kSecUseAuthenticationUI as String] = kSecUseAuthenticationUIFail
        }
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        guard status == errSecSuccess,
              let data = item as? Data,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return OAuthTokens(json: obj)
    }
}

// MARK: - Token model

private struct OAuthTokens {
    var accessToken: String
    var refreshToken: String
    var expiresAtMs: Int64
    var scopes: [String]
    var subscriptionType: String?
    var rateLimitTier: String?

    init?(json: [String: Any]) {
        let oauth = (json["claudeAiOauth"] as? [String: Any]) ?? json
        guard let access = oauth["accessToken"] as? String, !access.isEmpty else { return nil }
        accessToken = access
        refreshToken = (oauth["refreshToken"] as? String) ?? ""
        switch oauth["expiresAt"] {
        case let i as Int: expiresAtMs = Int64(i)
        case let i as Int64: expiresAtMs = i
        case let n as NSNumber: expiresAtMs = n.int64Value
        case let d as Double: expiresAtMs = Int64(d)
        default: expiresAtMs = 0
        }
        if let s = oauth["scopes"] as? [String] {
            scopes = s
        } else {
            scopes = []
        }
        subscriptionType = oauth["subscriptionType"] as? String
        rateLimitTier = oauth["rateLimitTier"] as? String
    }

    func envelopeJSON() -> [String: Any] {
        var oauth: [String: Any] = [
            "accessToken": accessToken,
            "refreshToken": refreshToken,
            "expiresAt": expiresAtMs,
            "scopes": scopes,
        ]
        if let subscriptionType { oauth["subscriptionType"] = subscriptionType }
        if let rateLimitTier { oauth["rateLimitTier"] = rateLimitTier }
        return ["claudeAiOauth": oauth]
    }
}

// MARK: - Claudius-owned Keychain

private enum OwnStore {
    private static let service = "com.gmclaude.claudius"
    private static let account = "claudeOAuth"

    static func load() -> OAuthTokens? {
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
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return OAuthTokens(json: obj)
    }

    static func save(_ tokens: OAuthTokens) {
        guard let data = try? JSONSerialization.data(withJSONObject: tokens.envelopeJSON()) else {
            return
        }
        delete()
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
