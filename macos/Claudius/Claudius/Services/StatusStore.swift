import Foundation

@Observable
@MainActor
final class StatusStore {
    enum RunState: Equatable {
        case stopped
        /// Listener created but not yet bound — NWListener confirms asynchronously,
        /// so "Running" must wait for .ready or the UI lies about a dead port.
        case starting
        case running
        case error(String)
    }

    var runState: RunState = .stopped
    var sessions: [AgentSession] = []
    var usage: UsageSnapshot = UsageSnapshot()
    var connectedClients: Int = 0
    var lastError: String?
    var claudeMissing = false

    var hasActiveSession: Bool {
        sessions.contains { $0.isActive }
    }

    var menuBarSymbol: String {
        switch runState {
        case .error, .stopped:
            return "display.trianglebadge.exclamationmark"
        case .starting:
            return "display"
        case .running:
            if claudeMissing {
                return "exclamationmark.triangle"
            }
            if hasActiveSession {
                return "display.and.arrow.down"
            }
            if connectedClients > 0 {
                return "display"
            }
            return "display"
        }
    }

    var statusLine: String {
        switch runState {
        case .stopped:
            return "Stopped"
        case .starting:
            return "Starting…"
        case .error(let msg):
            return "Error: \(msg)"
        case .running:
            if !usage.isLoaded {
                return "Running · \(connectedClients) screen\(connectedClients == 1 ? "" : "s") · usage…"
            }
            let pct = Int((usage.sessionPct * 100).rounded())
            let wPct = Int((usage.weeklyPct * 100).rounded())
            return "Running · \(connectedClients) screen\(connectedClients == 1 ? "" : "s") · session \(pct)% · weekly \(wPct)%"
        }
    }

    func snapshot() -> StatusPayload {
        StatusPayload(usage: usage, sessions: sessions)
    }
}
