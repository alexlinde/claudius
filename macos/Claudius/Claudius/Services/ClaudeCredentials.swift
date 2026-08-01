import Foundation
import Security

/// Claude OAuth access token for the usage API.
///
/// After a successful read of Claude Code's credentials (file or Keychain),
/// the blob is cached under Claudius's own Keychain identity so the 60s usage
/// loop does not keep touching the foreign item. Claude Code alone refreshes
/// tokens; when our cached access token expires (or the usage API returns 401)
/// we re-read from Claude Code.
///
/// Every read here is non-interactive. Claude Code's item lives in the file-based
/// login keychain, whose ACL names the applications allowed to read it, and we are
/// not one of them — `SecItemCopyMatching` from this process pops a dialog. Shelling
/// out to `/usr/bin/security`, which the item *does* trust (it is the tool that
/// manages it), reads the same bytes silently, exactly as companion/claudius.py does.
enum ClaudeCredentials {
    private static let lock = NSLock()
    /// Backoff after a failed `security` read (guarded by `lock`). The usage loop
    /// ticks every 60s; without this, a broken read means spawning a doomed
    /// subprocess every minute, forever.
    nonisolated(unsafe) private static var cliBlockedUntil: Date?

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

        if let foreign = loadForeignOAuth() {
            return adopt(foreign)
        }

        // Last resort: stale cached token (may 401; caller can force-reseed).
        return forceReseed ? nil : OwnStore.load()?.accessToken
    }

    /// Cache only tokens we can actually use — caching an already-expired blob just
    /// means re-reading (and re-rejecting) it every cycle.
    private static func adopt(_ oauth: OAuthTokens) -> String? {
        if accessTokenValid(oauth) {
            OwnStore.save(oauth)
        } else {
            NSLog("[creds] claude code token expired — not caching, waiting for refresh")
        }
        return oauth.accessToken
    }

    /// Call sites hold `lock`.
    private static func cliReadAllowed() -> Bool {
        guard let until = cliBlockedUntil else { return true }
        return until <= Date()
    }

    private static func accessTokenValid(_ oauth: OAuthTokens) -> Bool {
        guard !oauth.accessToken.isEmpty else { return false }
        guard oauth.expiresAtMs > 0 else { return true }
        let expires = Date(timeIntervalSince1970: Double(oauth.expiresAtMs) / 1000.0)
        return expires.timeIntervalSinceNow > CompanionConstants.oauthRefreshSkew
    }

    /// Call sites hold `lock`.
    private static func loadForeignOAuth() -> OAuthTokens? {
        if let fromFile = parseOAuth(fromFile: CompanionConstants.claudeCredsPath) {
            return fromFile
        }
        guard cliReadAllowed() else { return nil }
        guard let tokens = parseOAuthFromClaudeKeychain() else {
            cliBlockedUntil = Date().addingTimeInterval(
                CompanionConstants.keychainRetryBackoff
            )
            NSLog("[creds] keychain unavailable — retrying in "
                + "\(Int(CompanionConstants.keychainRetryBackoff) / 60)m")
            return nil
        }
        cliBlockedUntil = nil
        return tokens
    }

    private static func parseOAuth(fromFile path: String) -> OAuthTokens? {
        guard let data = try? Data(contentsOf: URL(fileURLWithPath: path)),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else { return nil }
        return OAuthTokens(json: obj)
    }

    /// Read Claude Code's item via `/usr/bin/security` rather than the Security
    /// framework — see the type doc for why this is the non-prompting path.
    private static func parseOAuthFromClaudeKeychain() -> OAuthTokens? {
        guard let data = runSecurity(arguments: [
            "find-generic-password",
            "-s", CompanionConstants.claudeKeychainService,
            "-w",
        ]) else { return nil }

        // `-w` writes the secret followed by a newline.
        let text = String(decoding: data, as: UTF8.self)
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty,
              let obj = try? JSONSerialization.jsonObject(with: Data(text.utf8))
                  as? [String: Any]
        else { return nil }
        return OAuthTokens(json: obj)
    }

    /// Spawn `security` and capture stdout, bounded by `keychainReadTimeout`.
    /// Returns nil on spawn failure, timeout, non-zero exit, or empty output.
    private static func runSecurity(arguments: [String]) -> Data? {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/security")
        proc.arguments = arguments
        let out = Pipe()
        proc.standardOutput = out
        // Discard stderr without allocating a pipe we have to drain (avoids FD leaks).
        proc.standardError = FileHandle.nullDevice

        // Drain stdout concurrently with termination — same hazard AgentsPoller hit:
        // waiting on exit before reading can wedge on an undrained pipe.
        final class StdoutBox: @unchecked Sendable {
            let lock = NSLock()
            var data = Data()
        }
        let box = StdoutBox()
        let group = DispatchGroup()
        group.enter() // balanced by terminationHandler
        proc.terminationHandler = { _ in
            group.leave()
        }

        do {
            try proc.run()
        } catch {
            group.leave()
            NSLog("[creds] security spawn failed: \(error.localizedDescription)")
            return nil
        }
        // Parent must close its copy of the write end or the read may hang.
        out.fileHandleForWriting.closeFile()

        group.enter() // balanced by stdout drain
        DispatchQueue.global().async {
            let data = out.fileHandleForReading.readDataToEndOfFile()
            box.lock.lock()
            box.data = data
            box.lock.unlock()
            group.leave()
        }

        let timedOut = group.wait(
            timeout: .now() + CompanionConstants.keychainReadTimeout
        ) == .timedOut
        if timedOut {
            if proc.isRunning {
                proc.terminate()
            }
            // Unblock a stuck readDataToEndOfFile and wait briefly for cleanup.
            try? out.fileHandleForReading.close()
            _ = group.wait(timeout: .now() + 1)
            NSLog("[creds] security timed out after "
                + "\(Int(CompanionConstants.keychainReadTimeout))s")
            return nil
        }
        try? out.fileHandleForReading.close()

        box.lock.lock()
        let data = box.data
        box.lock.unlock()

        guard proc.terminationStatus == 0 else {
            NSLog("[creds] security exit \(proc.terminationStatus)")
            return nil
        }
        return data.isEmpty ? nil : data
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
            // Never prompt for our own cache. Debug builds are ad-hoc signed
            // (project.yml CODE_SIGN_IDENTITY "-"), so the app's code identity —
            // and with it this item's ACL — changes on every rebuild. A miss is
            // cheap: we re-read Claude Code and `save()` re-creates the item under
            // the current identity.
            //
            // Deprecated in favour of kSecUseAuthenticationContext, but that routes
            // through LAContext and only governs the data-protection keychain. With
            // no keychain-access-groups entitlement this item lives in the file-based
            // login keychain, where this is still the constant that suppresses UI.
            kSecUseAuthenticationUI as String: kSecUseAuthenticationUIFail,
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
