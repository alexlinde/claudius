import Foundation

enum UsageFetcher {
    static func fetch() -> UsageSnapshot? {
        guard let token = ClaudeCredentials.loadAccessToken() else {
            return nil
        }
        switch request(token: token) {
        case .success(let snapshot):
            return snapshot
        case .unauthorized:
            guard let refreshed = ClaudeCredentials.loadAccessTokenForcingRefresh(),
                  refreshed != token
            else { return nil }
            if case .success(let snapshot) = request(token: refreshed) {
                return snapshot
            }
            return nil
        case .failed:
            return nil
        }
    }

    private enum Outcome: Sendable {
        case success(UsageSnapshot)
        case unauthorized
        case failed
    }

    /// The completion handler runs on URLSession's delegate queue while this
    /// (synchronous, Task.detached) caller waits — the result needs a lock.
    private final class OutcomeBox: @unchecked Sendable {
        private let lock = NSLock()
        private var value: Outcome = .failed

        func set(_ outcome: Outcome) {
            lock.lock()
            value = outcome
            lock.unlock()
        }

        func get() -> Outcome {
            lock.lock()
            defer { lock.unlock() }
            return value
        }
    }

    private static func request(token: String) -> Outcome {
        var req = URLRequest(url: CompanionConstants.usageAPI, timeoutInterval: 10)
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        req.setValue("claude-code/1.0.0", forHTTPHeaderField: "User-Agent")
        req.setValue("application/json", forHTTPHeaderField: "Accept")

        let sem = DispatchSemaphore(value: 0)
        let box = OutcomeBox()
        let task = URLSession.shared.dataTask(with: req) { data, response, _ in
            defer { sem.signal() }
            guard let http = response as? HTTPURLResponse else { return }
            if http.statusCode == 401 {
                box.set(.unauthorized)
                return
            }
            guard let data,
                  (200..<300).contains(http.statusCode),
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            else { return }

            let session = obj["five_hour"] as? [String: Any] ?? [:]
            let weekly = obj["seven_day"] as? [String: Any] ?? [:]
            let sessionUtil = double(session["utilization"]) / 100.0
            let weeklyUtil = double(weekly["utilization"]) / 100.0
            box.set(.success(UsageSnapshot(
                sessionPct: sessionUtil,
                weeklyPct: weeklyUtil,
                sessionReset: UsageTime.formatISOCountdown(string(session["resets_at"])),
                weeklyReset: UsageTime.formatISOCountdown(string(weekly["resets_at"])),
                isLoaded: true
            )))
        }
        task.resume()
        // On timeout the handler may still be about to run: cancel the task and
        // report failure rather than reading a value being written concurrently.
        guard sem.wait(timeout: .now() + 12) == .success else {
            task.cancel()
            NSLog("[usage] request timed out")
            return .failed
        }
        return box.get()
    }

    private static func double(_ any: Any?) -> Double {
        switch any {
        case let d as Double: return d
        case let i as Int: return Double(i)
        case let n as NSNumber: return n.doubleValue
        case let s as String: return Double(s) ?? 0
        default: return 0
        }
    }

    private static func string(_ any: Any?) -> String {
        (any as? String) ?? ""
    }
}
