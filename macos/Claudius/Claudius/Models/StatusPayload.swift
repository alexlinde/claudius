import Foundation

struct UsageSnapshot: Equatable, Sendable {
    var sessionPct: Double = 0
    var weeklyPct: Double = 0
    var sessionReset: String = "--"
    var weeklyReset: String = "--"
    /// False until the first successful Claude usage API fetch.
    /// Keeps meter titles out of status JSON so the screen doesn't flash 0%.
    var isLoaded: Bool = false
}

struct StatusPayload: Equatable, Sendable {
    var usage: UsageSnapshot = UsageSnapshot()
    var sessions: [AgentSession] = []

    var statusKey: String {
        let usageKey = usage.isLoaded
            ? "\(usage.sessionPct)|\(usage.weeklyPct)"
            : "pending"
        return "\(SessionNormalizer.fingerprint(sessions))|\(usageKey)"
    }

    /// Untyped status JSON expected by the firmware.
    func jsonObject() -> [String: Any] {
        var obj: [String: Any] = [
            "agent_id": CompanionConstants.agentID,
            "agent_display": CompanionConstants.agentDisplay,
            "sessions": sessions.map { s -> [String: Any] in
                var d: [String: Any] = [
                    "cwd": s.cwd,
                    "kind": s.kind,
                    "name": s.name,
                    "startedAt": s.startedAt,
                ]
                if let id = s.id { d["id"] = id }
                if let sessionId = s.sessionId { d["sessionId"] = sessionId }
                if let state = s.state { d["state"] = state }
                if let status = s.status { d["status"] = status }
                if let waitingFor = s.waitingFor { d["waitingFor"] = waitingFor }
                if let pid = s.pid { d["pid"] = pid }
                return d
            },
        ]
        // Omit meter fields until usage is loaded — empty titles hide the bars
        // on the screen, avoiding a 0% flash when the companion restarts.
        if usage.isLoaded {
            obj["session_pct"] = usage.sessionPct
            obj["weekly_pct"] = usage.weeklyPct
            obj["session_reset"] = usage.sessionReset
            obj["weekly_reset"] = usage.weeklyReset
            obj["weekly_title"] = "\(CompanionConstants.agentDisplay) Weekly"
            obj["session_title"] = "\(CompanionConstants.agentDisplay) Session"
        }
        return obj
    }

    func jsonData() throws -> Data {
        try JSONSerialization.data(withJSONObject: jsonObject())
    }
}

enum ProtocolMessages {
    static func challenge(nonce: String) -> Data {
        json(["type": "challenge", "nonce": nonce])
    }

    static func unauthorized() -> Data {
        json([
            "error": "unauthorized",
            "message": "Wrong password",
        ])
    }

    static func config() -> Data {
        let utcOffset = Int(TimeZone.current.secondsFromGMT())
        return json([
            "type": "config",
            "utc_offset": utcOffset,
            "remote_control": false,
            "default_agent_id": CompanionConstants.agentID,
            "agents": [
                CompanionConstants.agentID: [
                    "display": CompanionConstants.agentDisplay,
                    "color": CompanionConstants.agentColor,
                    "logo_bitmap": CompanionConstants.logoBitmap,
                ] as [String: Any],
            ],
        ])
    }

    private static func json(_ obj: [String: Any]) -> Data {
        // swiftlint:disable:next force_try
        try! JSONSerialization.data(withJSONObject: obj)
    }
}
