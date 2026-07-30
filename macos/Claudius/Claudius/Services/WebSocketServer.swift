import Foundation
import Network
import Security

/// WebSocket companion server — protocol-compatible with companion/claudius.py.
final class WebSocketServer: @unchecked Sendable {
    typealias StatusProvider = () -> StatusPayload
    typealias ClientCountHandler = (Int) -> Void

    private let port: UInt16
    private let secret: String
    private let statusProvider: StatusProvider
    private let onClientCount: ClientCountHandler

    private var listener: NWListener?
    private let queue = DispatchQueue(label: "com.gmclaude.claudius.ws")
    private var clients: [ObjectIdentifier: Client] = [:]

    private var lastBroadcastKey = ""
    private var lastBroadcastMono: TimeInterval = 0

    init(
        port: Int,
        secret: String,
        statusProvider: @escaping StatusProvider,
        onClientCount: @escaping ClientCountHandler
    ) {
        self.port = UInt16(port)
        self.secret = secret
        self.statusProvider = statusProvider
        self.onClientCount = onClientCount
    }

    func start() throws {
        let parameters = NWParameters.tcp
        let wsOptions = NWProtocolWebSocket.Options()
        wsOptions.autoReplyPing = true
        parameters.defaultProtocolStack.applicationProtocols.insert(wsOptions, at: 0)
        parameters.allowLocalEndpointReuse = true

        let listener = try NWListener(using: parameters, on: NWEndpoint.Port(rawValue: port)!)
        self.listener = listener

        listener.newConnectionHandler = { [weak self] connection in
            self?.accept(connection)
        }
        listener.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .failed(let error):
                NSLog("[ws] listener failed: \(error)")
            case .ready:
                NSLog("[ws] listening on :\(self.port)")
            default:
                break
            }
        }
        listener.start(queue: queue)
    }

    func stop() {
        let group = DispatchGroup()
        // Run teardown on the WS queue, but block the caller until close frames
        // have been queued/cancelled so Quit doesn't race process exit.
        queue.sync {
            let pending = Array(self.clients.values)
            self.clients.removeAll()
            for client in pending {
                group.enter()
                client.shutdown(reason: "shutdown", code: 1001) {
                    group.leave()
                }
            }
            self.listener?.cancel()
            self.listener = nil
            DispatchQueue.main.async { self.onClientCount(0) }
        }
        if group.wait(timeout: .now() + 1.5) == .timedOut {
            NSLog("[ws] stop: timed out waiting for client close")
        }
    }

    func broadcast(force: Bool = false) {
        queue.async {
            let payload = self.statusProvider()
            let key = payload.statusKey
            let now = ProcessInfo.processInfo.systemUptime
            if !force {
                if key == self.lastBroadcastKey,
                   (now - self.lastBroadcastMono) < CompanionConstants.broadcastDedupeWindow {
                    return
                }
                if (now - self.lastBroadcastMono) < CompanionConstants.broadcastMinInterval {
                    return
                }
            }
            self.lastBroadcastKey = key
            self.lastBroadcastMono = now

            guard let data = try? payload.jsonData() else { return }
            let active = payload.sessions.filter(\.isActive).count
            NSLog(
                "[status] sessions=\(payload.sessions.count) active=\(active) "
                    + "session=\(Int(payload.usage.sessionPct * 100))% "
                    + "weekly=\(Int(payload.usage.weeklyPct * 100))%"
            )

            for (_, client) in self.clients where client.subscribed {
                client.send(data)
            }
        }
    }

    private func accept(_ connection: NWConnection) {
        let client = Client(
            connection: connection,
            secret: secret,
            statusProvider: statusProvider,
            queue: queue
        )
        let id = ObjectIdentifier(client)
        clients[id] = client
        client.onClose = { [weak self] in
            guard let self else { return }
            self.clients.removeValue(forKey: id)
            let count = self.clients.values.filter(\.subscribed).count
            DispatchQueue.main.async { self.onClientCount(count) }
            NSLog("[ws] client disconnected (\(count) remaining)")
        }
        client.onSubscribed = { [weak self] in
            guard let self else { return }
            let count = self.clients.values.filter(\.subscribed).count
            DispatchQueue.main.async { self.onClientCount(count) }
            NSLog("[ws] client connected / subscribed (\(count) total)")
        }
        client.start()
    }
}

// MARK: - Client

private final class Client: @unchecked Sendable {
    var onClose: (() -> Void)?
    var onSubscribed: (() -> Void)?
    private(set) var subscribed = false

    private let connection: NWConnection
    private let secret: String
    private let statusProvider: WebSocketServer.StatusProvider
    private let queue: DispatchQueue
    private var authenticated = false
    private var closed = false
    private var authTimeoutWork: DispatchWorkItem?

    init(
        connection: NWConnection,
        secret: String,
        statusProvider: @escaping WebSocketServer.StatusProvider,
        queue: DispatchQueue
    ) {
        self.connection = connection
        self.secret = secret
        self.statusProvider = statusProvider
        self.queue = queue
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in
            guard let self else { return }
            switch state {
            case .ready:
                self.beginHandshake()
            case .failed, .cancelled:
                self.finish()
            default:
                break
            }
        }
        connection.start(queue: queue)
    }

    func send(_ data: Data) {
        guard !closed else { return }
        let meta = NWProtocolWebSocket.Metadata(opcode: .text)
        let context = NWConnection.ContentContext(identifier: "text", metadata: [meta])
        connection.send(
            content: data,
            contentContext: context,
            isComplete: true,
            completion: .contentProcessed { _ in }
        )
    }

    func closeUnauthorized(reason: String, code: UInt16) {
        shutdown(reason: reason, code: code, completion: nil)
    }

    /// Send a WebSocket close, then cancel the TCP connection.
    func shutdown(reason: String, code: UInt16, completion: (() -> Void)?) {
        guard !closed else {
            completion?()
            return
        }
        var didComplete = false
        let once: () -> Void = {
            guard !didComplete else { return }
            didComplete = true
            completion?()
        }
        let meta = NWProtocolWebSocket.Metadata(opcode: .close)
        switch code {
        case 1008:
            meta.closeCode = .protocolCode(.policyViolation)
        case 1011:
            meta.closeCode = .protocolCode(.internalServerError)
        default:
            meta.closeCode = .protocolCode(.goingAway)
        }
        let context = NWConnection.ContentContext(identifier: "close", metadata: [meta])
        connection.send(
            content: Data(reason.utf8),
            contentContext: context,
            isComplete: true,
            completion: .contentProcessed { [weak self] _ in
                self?.connection.cancel()
                self?.finish()
                once()
            }
        )
        // If send never completes (peer already gone), still tear down promptly.
        queue.asyncAfter(deadline: .now() + 0.4) { [weak self] in
            guard let self else {
                once()
                return
            }
            if !self.closed {
                self.connection.cancel()
                self.finish()
            }
            once()
        }
    }

    private func beginHandshake() {
        if secret.isEmpty {
            authenticated = true
            receiveLoop()
            return
        }
        let nonce = randomNonce()
        send(ProtocolMessages.challenge(nonce: nonce))
        scheduleAuthTimeout()
        receiveAuth(nonce: nonce)
    }

    private func scheduleAuthTimeout() {
        authTimeoutWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, !self.authenticated, !self.closed else { return }
            NSLog("[ws] auth error / timeout")
            self.closeUnauthorized(reason: "Authentication timeout", code: 1011)
        }
        authTimeoutWork = work
        queue.asyncAfter(deadline: .now() + 5, execute: work)
    }

    private func receiveAuth(nonce: String) {
        connection.receiveMessage { [weak self] content, _, _, error in
            guard let self else { return }
            self.authTimeoutWork?.cancel()
            if error != nil {
                self.closeUnauthorized(reason: "Authentication timeout", code: 1011)
                return
            }
            guard let content,
                  let obj = try? JSONSerialization.jsonObject(with: content) as? [String: Any]
            else {
                self.closeUnauthorized(reason: "Authentication timeout", code: 1011)
                return
            }
            let provided = (obj["auth_hmac"] as? String) ?? ""
            if Auth.isValid(provided: provided, secret: self.secret, nonce: nonce) {
                self.authenticated = true
                self.receiveLoop()
            } else {
                NSLog("[ws] auth failed")
                self.send(ProtocolMessages.unauthorized())
                self.closeUnauthorized(reason: "Unauthorized", code: 1008)
            }
        }
    }

    private func receiveLoop() {
        connection.receiveMessage { [weak self] content, context, _, error in
            guard let self else { return }
            if error != nil {
                self.finish()
                return
            }
            if let meta = context?.protocolMetadata(definition: NWProtocolWebSocket.definition)
                as? NWProtocolWebSocket.Metadata,
               meta.opcode == .close {
                self.finish()
                return
            }
            if let content {
                self.handleMessage(content)
            }
            if !self.closed {
                self.receiveLoop()
            }
        }
    }

    private func handleMessage(_ data: Data) {
        guard authenticated,
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              (obj["type"] as? String) == "subscribe"
        else { return }

        let client = (obj["client"] as? String) ?? "ws"
        send(ProtocolMessages.config())
        if let status = try? statusProvider().jsonData() {
            send(status)
        }
        subscribed = true
        onSubscribed?()
        NSLog("[ws] subscribed as \(client)")
    }

    private func finish() {
        guard !closed else { return }
        closed = true
        authTimeoutWork?.cancel()
        connection.cancel()
        onClose?()
    }

    private func randomNonce() -> String {
        var bytes = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        return bytes.map { String(format: "%02x", $0) }.joined()
    }
}
