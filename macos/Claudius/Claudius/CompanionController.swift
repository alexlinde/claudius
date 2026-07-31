import AppKit
import Foundation
import ServiceManagement

@MainActor
@Observable
final class CompanionController {
    let store = StatusStore()

    private var server: WebSocketServer?
    private var mdns: MDNSAdvertiser?
    private var agentsTask: Task<Void, Never>?
    private var usageTask: Task<Void, Never>?

    func start() {
        stopInternal(clearState: false)

        let name = AppPreferences.name.trimmingCharacters(in: .whitespacesAndNewlines)

        let port = AppPreferences.port
        let secret = AppPreferences.secret
        let bridge = StatusSnapshotBridge.shared
        bridge.update(store.snapshot())

        let realServer = WebSocketServer(
            port: port,
            secret: secret,
            statusProvider: { bridge.snapshot() },
            onClientCount: { [weak self] count in
                Task { @MainActor in
                    self?.store.connectedClients = count
                }
            }
        )

        do {
            try realServer.start()
        } catch {
            store.runState = .error("Bind failed: \(error.localizedDescription)")
            return
        }

        let advertiser = MDNSAdvertiser(name: name, port: port)
        advertiser.start()

        self.server = realServer
        self.mdns = advertiser
        store.runState = .running
        store.lastError = nil

        // Fetch usage first so a reconnecting screen doesn't get a 0% status
        // snapshot from the agents poller before OAuth usage is ready.
        usageTask = Task { [weak self] in
            await self?.usageLoop()
        }
        agentsTask = Task { [weak self] in
            await self?.agentsLoop()
        }

        Task { [weak self] in
            await self?.bootOnce()
        }
    }

    func quit() {
        shutdown()
        NSApplication.shared.terminate(nil)
    }

    /// Tear down WS/mDNS/pollers. Safe to call more than once (e.g. Quit + willTerminate).
    func shutdown() {
        stopInternal(clearState: true)
        store.runState = .stopped
    }

    func restartIfRunning() {
        if case .running = store.runState {
            start()
        }
    }

    // MARK: - Private

    private func stopInternal(clearState: Bool) {
        agentsTask?.cancel()
        usageTask?.cancel()
        agentsTask = nil
        usageTask = nil
        server?.stop()
        server = nil
        mdns?.stop()
        mdns = nil
        if clearState {
            store.sessions = []
            store.connectedClients = 0
        }
    }

    private func bootOnce() async {
        let usage = await Task.detached {
            UsageFetcher.fetch()
        }.value
        if let usage {
            store.usage = usage
            StatusSnapshotBridge.shared.update(store.snapshot())
        }

        let binary = AppPreferences.claudeBinary
        let agentResult = await Task.detached {
            AgentsPoller.fetch(claudeBinary: binary)
        }.value
        applyAgents(agentResult)
        server?.broadcast(force: true)
    }

    private func agentsLoop() async {
        while !Task.isCancelled {
            let binary = AppPreferences.claudeBinary
            let result = await Task.detached {
                AgentsPoller.fetch(claudeBinary: binary)
            }.value
            applyAgents(result)
            try? await Task.sleep(
                nanoseconds: UInt64(CompanionConstants.agentsInterval * 1_000_000_000)
            )
        }
    }

    private func usageLoop() async {
        var first = true
        while !Task.isCancelled {
            let usage = await Task.detached {
                UsageFetcher.fetch()
            }.value
            if let usage {
                let changed = usage != store.usage
                store.usage = usage
                if changed || first {
                    StatusSnapshotBridge.shared.update(store.snapshot())
                    server?.broadcast(force: first)
                }
                first = false
            }
            try? await Task.sleep(
                nanoseconds: UInt64(CompanionConstants.usageInterval * 1_000_000_000)
            )
        }
    }

    private func applyAgents(_ result: AgentsPoller.Result) {
        switch result {
        case .sessions(let sessions):
            store.claudeMissing = false
            if sessions != store.sessions {
                store.sessions = sessions
                StatusSnapshotBridge.shared.update(store.snapshot())
                server?.broadcast()
            }
        case .keepLast:
            break
        case .binaryMissing:
            store.claudeMissing = true
        }
    }
}

/// Cross-queue status snapshot for WebSocket broadcasts.
final class StatusSnapshotBridge: @unchecked Sendable {
    static let shared = StatusSnapshotBridge()
    private let lock = NSLock()
    private var payload = StatusPayload()

    func update(_ payload: StatusPayload) {
        lock.lock()
        self.payload = payload
        lock.unlock()
    }

    func snapshot() -> StatusPayload {
        lock.lock()
        defer { lock.unlock() }
        return payload
    }
}
