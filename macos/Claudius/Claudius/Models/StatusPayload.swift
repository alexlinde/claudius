import Foundation

struct UsageSnapshot: Equatable, Sendable {
    var sessionPct: Double = 0
    var weeklyPct: Double = 0
    var sessionReset: String = "--"
    var weeklyReset: String = "--"
}

struct StatusPayload: Equatable, Sendable {
    var usage: UsageSnapshot = UsageSnapshot()
    var sessions: [AgentSession] = []

    var statusKey: String {
        "\(SessionNormalizer.fingerprint(sessions))|\(usage.sessionPct)|\(usage.weeklyPct)"
    }

    /// Untyped status JSON expected by the firmware.
    func jsonObject() -> [String: Any] {
        var obj: [String: Any] = [
            "session_pct": usage.sessionPct,
            "weekly_pct": usage.weeklyPct,
            "session_reset": usage.sessionReset,
            "weekly_reset": usage.weeklyReset,
            "agent_id": CompanionConstants.agentID,
            "agent_display": CompanionConstants.agentDisplay,
            "weekly_title": "\(CompanionConstants.agentDisplay) Weekly",
            "session_title": "\(CompanionConstants.agentDisplay) Session",
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
