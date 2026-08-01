import Foundation

enum CompanionConstants {
    static let wsPortDefault = 8765
    static let usageInterval: TimeInterval = 60
    static let agentsInterval: TimeInterval = 2.0
    static let maxSessions = 8
    static let agentID = "claude"
    static let agentDisplay = "Claude"
    static let agentColor = "#DE7356"
    static let usageAPI = URL(string: "https://claude.ai/api/oauth/usage")!
    /// Treat access tokens as stale this far before expiresAt.
    static let oauthRefreshSkew: TimeInterval = 5 * 60
    static let claudeCredsPath = NSString(
        string: "~/.claude/.credentials.json"
    ).expandingTildeInPath
    static let claudeSettingsPath = NSString(
        string: "~/.claude/settings.json"
    ).expandingTildeInPath
    static let claudeKeychainService = "Claude Code-credentials"
    static let mdnsType = "_claudius._tcp."
    static let mdnsRefreshInterval: TimeInterval = 10
    static let broadcastMinInterval: TimeInterval = 0.5
    static let broadcastDedupeWindow: TimeInterval = 5.0
    /// Trailing edge for a broadcast the min-interval guard deferred (> minInterval).
    static let broadcastTrailingDelay: TimeInterval = 0.6
    /// Match companion/claudius.py websockets ping_interval / ping_timeout.
    static let wsPingInterval: TimeInterval = 20
    /// Peers are declared dead this long after the last pong (> pingInterval, so a
    /// single dropped ping doesn't reap a live screen).
    static let wsPongTimeout: TimeInterval = 30
    /// Half-open peers that never subscribe are dropped after this.
    static let wsHandshakeTimeout: TimeInterval = 20
    static let wsWatchdogInterval: TimeInterval = 5
    /// Backoff for re-binding the listener (a stale claudius.py usually frees the port).
    static let listenerRetryBase: TimeInterval = 5
    static let listenerRetryMax: TimeInterval = 60
    /// Consecutive `claude agents` failures before the menu bar reports it.
    static let agentsFailureAlertCount = 5
    /// How long `claude agents` may keep failing before displayed sessions are dropped.
    static let agentsStaleTimeout: TimeInterval = 600
    /// Don't re-open a Keychain dialog for Claude Code's item more often than this.
    static let keychainPromptCooldown: TimeInterval = 3600

    /// Same base64 bitmap as companion/claudius.py — 64×64 1-bit logo for the screen.
    static let logoBitmap =
        "AAAAAAAAAAYAAAAAAA+AGAAAAA+APAAAAA/APAAAAA/AOAAAAAfAOAcAAAfgOA8AAAPgOB+"
        + "AAAPweB8AB4HweD8AB8D4cH4AB+D4cP4AB/B8cfwAAfx8c/gAAP4+c/gAAD8eZ/AAAB/ef"
        + "+AAAAf//8AAAAP//8AMAAD//4H+AAB////8AAA////wf/8f//4AP////+AAAAD//wAAAAA"
        + "f///AAAA////8AAD//w/+AAP3/4D+AAfO/8AIAB8c/+AAAH4c3vAAAPg5z3gAA/BxzxwAA"
        + "8Dhx44AAQHhg8cAAAHBg8OAAAODgeDAAAcDgPBAAA4DgPAAAA4DgHAAABwDgDAAAAAHgAA"
        + "AAAAHgAAAAAADgAAAAAADAAAAAAAAAAAA"

    static let activeStates: Set<String> = ["working", "blocked"]
    static let activeStatuses: Set<String> = ["busy", "waiting"]
}
