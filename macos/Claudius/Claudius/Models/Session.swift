import Foundation

struct AgentSession: Codable, Equatable, Sendable {
    var cwd: String
    var kind: String
    var name: String
    var startedAt: Int
    var id: String?
    var sessionId: String?
    var state: String?
    var status: String?
    var waitingFor: String?
    var pid: Int?

    /// Stable identity for SwiftUI lists when Claude omits `id`.
    var listID: String {
        id ?? sessionId ?? "\(name)-\(startedAt)"
    }

    var isActive: Bool {
        let st = (state ?? "").lowercased()
        let su = (status ?? "").lowercased()
        return CompanionConstants.activeStates.contains(st)
            || CompanionConstants.activeStatuses.contains(su)
    }

    var statusLabel: String {
        if let waitingFor, !waitingFor.isEmpty { return waitingFor }
        if let state, !state.isEmpty { return state }
        if let status, !status.isEmpty { return status }
        return "idle"
    }
}

enum SessionNormalizer {
    static func truncate(_ s: String, _ n: Int) -> String {
        guard s.count > n else { return s }
        if n <= 1 { return String(s.prefix(n)) }
        return String(s.prefix(n - 1)) + "…"
    }

    static func cwdBasename(_ cwd: String) -> String {
        guard !cwd.isEmpty else { return "" }
        return (cwd as NSString).lastPathComponent
    }

    /// Compact one `claude agents --json` entry for the screen.
    static func normalize(_ raw: [String: Any]) -> AgentSession? {
        let kind = string(raw["kind"])
        let cwdRaw = string(raw["cwd"])
        let nameRaw = string(raw["name"])
        let name = nameRaw.isEmpty ? cwdBasename(cwdRaw) : nameRaw

        var out = AgentSession(
            cwd: truncate(cwdBasename(cwdRaw), 40),
            kind: truncate(kind, 16),
            name: truncate(name, 40),
            startedAt: int(raw["startedAt"]) ?? 0
        )
        if let id = raw["id"] {
            out.id = truncate(String(describing: id), 16)
        }
        if let sid = raw["sessionId"] {
            out.sessionId = truncate(String(describing: sid), 40)
        }
        if let state = raw["state"] as? String, !state.isEmpty {
            out.state = truncate(state, 24)
        }
        if let status = raw["status"] as? String, !status.isEmpty {
            out.status = truncate(status, 24)
        }
        if let waiting = raw["waitingFor"] as? String, !waiting.isEmpty {
            out.waitingFor = truncate(waiting, 48)
        }
        if let pid = int(raw["pid"]) {
            out.pid = pid
        }
        return out
    }

    static func sortKey(_ s: AgentSession) -> (Int, Int) {
        (s.isActive ? 0 : 1, -s.startedAt)
    }

    static func normalizeList(_ raw: [[String: Any]]) -> [AgentSession] {
        var sessions = raw.compactMap(normalize)
        sessions.sort {
            let a = sortKey($0)
            let b = sortKey($1)
            if a.0 != b.0 { return a.0 < b.0 }
            return a.1 < b.1
        }
        return Array(sessions.prefix(CompanionConstants.maxSessions))
    }

    static func fingerprint(_ sessions: [AgentSession]) -> String {
        sessions.map { s in
            let key = s.sessionId ?? s.id ?? s.name
            return "\(key)|\(s.state ?? "")|\(s.status ?? "")|\(s.waitingFor ?? "")"
        }.joined(separator: ";")
    }

    private static func string(_ any: Any?) -> String {
        guard let any else { return "" }
        if let s = any as? String { return s }
        return String(describing: any)
    }

    private static func int(_ any: Any?) -> Int? {
        switch any {
        case let i as Int: return i
        case let n as NSNumber: return n.intValue
        case let d as Double: return Int(d)
        case let s as String: return Int(s)
        default: return nil
        }
    }
}
