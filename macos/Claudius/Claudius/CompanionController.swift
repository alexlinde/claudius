import AppKit
import Foundation
import ServiceManagement

@MainActor
@Observable
final class CompanionController {
    /// Marker so an agents-failure banner can be cleared without clobbering
    /// unrelated errors (login item, Keychain).
    private static let agentsErrorPrefix = "claude agents failing: "

    let store = StatusStore()

    private var server: WebSocketServer?
    private var mdns: MDNSAdvertiser?
    private var agentsTask: Task<Void, Never>?
    private var usageTask: Task<Void, Never>?
    private var bootTask: Task<Void, Never>?
    private var retryTask: Task<Void, Never>?
    /// Bumped on every stop/restart. Async work re-checks it after each await so a
    /// cancelled poller can't repopulate the UI (or broadcast) after shutdown.
    private var generation = 0
    private var retryDelay = CompanionConstants.listenerRetryBase
    private var agentsFailureStreak = 0
    private var agentsFailingSince: Date?

    func start() async {
        retryDelay = CompanionConstants.listenerRetryBase
        await startInternal()
    }

    func quit() {
        shutdownForTermination()
        NSApplication.shared.terminate(nil)
    }

    /// Synchronous, bounded teardown — app termination only, where the close frames
    /// must be queued before the process exits. Safe to call more than once
    /// (Quit + applicationWillTerminate). Every other path uses the async teardown.
    func shutdownForTermination() {
        let dying = detachAll(clearState: true)
        dying?.stopBlocking()
        store.runState = .stopped
    }

    // MARK: - Lifecycle

    private func startInternal() async {
        await stopInternal(clearState: false)
        let gen = generation

        let name = AppPreferences.name.trimmingCharacters(in: .whitespacesAndNewlines)
        let port = AppPreferences.port

        // Fail closed. An unreadable Keychain is not "no secret configured" — that
        // would put an unauthenticated server on the LAN.
        let secret: String
        do {
            secret = try AppPreferences.loadSecret()
        } catch {
            NSLog("[companion] \(error.localizedDescription)")
            store.runState = .error(
                "\(error.localizedDescription) — refusing to serve unauthenticated"
            )
            scheduleRetry()
            return
        }

        let bridge = StatusSnapshotBridge.shared
        bridge.update(store.snapshot())

        let realServer = WebSocketServer(
            port: port,
            secret: secret,
            statusProvider: { bridge.snapshot() },
            onClientCount: { [weak self] count in
                Task { @MainActor in
                    guard let self, self.generation == gen else { return }
                    self.store.connectedClients = count
                }
            },
            onListenerState: { [weak self] state in
                Task { @MainActor in
                    await self?.handleListenerState(state, generation: gen)
                }
            }
        )

        do {
            try realServer.start()
        } catch {
            store.runState = .error("Bind failed: \(error.localizedDescription)")
            scheduleRetry()
            return
        }

        let advertiser = MDNSAdvertiser(name: name, port: port)
        advertiser.start()

        server = realServer
        mdns = advertiser
        // Deliberately not .running: NWListener reports the bind result
        // asynchronously, so wait for .ready before claiming to serve anything.
        store.runState = .starting
        store.lastError = nil
        agentsFailureStreak = 0
        agentsFailingSince = nil

        // Fetch usage first so a reconnecting screen doesn't get a 0% status
        // snapshot from the agents poller before OAuth usage is ready.
        usageTask = Task { [weak self] in
            await self?.usageLoop(generation: gen)
        }
        agentsTask = Task { [weak self] in
            await self?.agentsLoop(generation: gen)
        }
        bootTask = Task { [weak self] in
            await self?.bootOnce(generation: gen)
        }
    }

    private func handleListenerState(
        _ state: WebSocketServer.ListenerState,
        generation gen: Int
    ) async {
        guard gen == generation else { return }
        switch state {
        case .ready(let port):
            retryDelay = CompanionConstants.listenerRetryBase
            store.runState = .running
            store.lastError = nil
            NSLog("[companion] serving on :\(port)")
        case .waiting(let message):
            // Not bound yet — say so instead of "Running", but leave the listener
            // in place: it may bind itself once the port frees up. The armed retry
            // rebuilds it if it hasn't.
            NSLog("[companion] listener not serving: \(message)")
            store.runState = .error(message)
            scheduleRetry()
        case .failed(let message):
            // Nothing is listening: tear everything down (mDNS would otherwise
            // advertise a dead port) and retry — the usual cause is a stale
            // companion/claudius.py that exits within a few seconds.
            NSLog("[companion] listener unusable: \(message)")
            await stopInternal(clearState: true)
            store.runState = .error(message)
            scheduleRetry()
        }
    }

    private func scheduleRetry() {
        let delay = retryDelay
        retryDelay = min(retryDelay * 2, CompanionConstants.listenerRetryMax)
        let gen = generation
        retryTask?.cancel()
        retryTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard !Task.isCancelled, let self, self.generation == gen else { return }
            // A .waiting listener may have bound itself in the meantime.
            if case .running = self.store.runState { return }
            NSLog("[companion] retrying start after \(Int(delay))s")
            await self.startInternal()
        }
    }

    // MARK: - Teardown

    /// Detach every moving part *synchronously* (so `generation` is bumped before
    /// any suspension point) and hand back the server still needing teardown.
    private func detachAll(clearState: Bool) -> WebSocketServer? {
        generation &+= 1
        agentsTask?.cancel()
        usageTask?.cancel()
        bootTask?.cancel()
        retryTask?.cancel()
        agentsTask = nil
        usageTask = nil
        bootTask = nil
        retryTask = nil
        mdns?.stop()
        mdns = nil
        let dying = server
        server = nil
        if dying != nil {
            // Every socket dies with the server. The old server's own
            // onClientCount(0) is ignored (stale generation), so reset here or a
            // failed restart would keep showing screens that are gone.
            store.connectedClients = 0
        }
        if clearState {
            store.sessions = []
        }
        return dying
    }

    private func stopInternal(clearState: Bool) async {
        let dying = detachAll(clearState: clearState)
        // Awaited off the main thread — the old stop() blocked it for up to 1.5s.
        await dying?.stop()
    }

    // MARK: - Pollers

    private func bootOnce(generation gen: Int) async {
        let usage = await Task.detached {
            UsageFetcher.fetch()
        }.value
        guard !Task.isCancelled, gen == generation else { return }
        if let usage {
            store.usage = usage
            StatusSnapshotBridge.shared.update(store.snapshot())
        }

        let binary = AppPreferences.claudeBinary
        let agentResult = await Task.detached {
            AgentsPoller.fetch(claudeBinary: binary)
        }.value
        guard !Task.isCancelled, gen == generation else { return }
        applyAgents(agentResult)
        server?.broadcast(force: true)
    }

    private func agentsLoop(generation gen: Int) async {
        while !Task.isCancelled {
            let binary = AppPreferences.claudeBinary
            let result = await Task.detached {
                AgentsPoller.fetch(claudeBinary: binary)
            }.value
            guard !Task.isCancelled, gen == generation else { return }
            applyAgents(result)
            try? await Task.sleep(
                nanoseconds: UInt64(CompanionConstants.agentsInterval * 1_000_000_000)
            )
        }
    }

    private func usageLoop(generation gen: Int) async {
        var first = true
        while !Task.isCancelled {
            let usage = await Task.detached {
                UsageFetcher.fetch()
            }.value
            guard !Task.isCancelled, gen == generation else { return }
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
            noteAgentsSuccess()
            if sessions != store.sessions {
                store.sessions = sessions
                StatusSnapshotBridge.shared.update(store.snapshot())
                server?.broadcast()
            }
        case .keepLast(let reason):
            noteAgentsFailure(reason: reason)
        case .binaryMissing:
            store.claudeMissing = true
        }
    }

    private func noteAgentsSuccess() {
        agentsFailureStreak = 0
        agentsFailingSince = nil
        if store.lastError?.hasPrefix(Self.agentsErrorPrefix) == true {
            store.lastError = nil
        }
    }

    /// Persistent `claude agents` failures used to freeze the session list forever
    /// with no signal: report them, then stop showing sessions that may be long dead.
    private func noteAgentsFailure(reason: String) {
        agentsFailureStreak += 1
        let since = agentsFailingSince ?? Date()
        agentsFailingSince = since
        if agentsFailureStreak >= CompanionConstants.agentsFailureAlertCount {
            store.lastError = Self.agentsErrorPrefix + reason
        }
        let failingFor = Date().timeIntervalSince(since)
        guard failingFor >= CompanionConstants.agentsStaleTimeout,
              !store.sessions.isEmpty
        else { return }
        NSLog("[agents] dropping stale sessions after \(Int(failingFor))s of failures")
        store.sessions = []
        StatusSnapshotBridge.shared.update(store.snapshot())
        server?.broadcast(force: true)
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
