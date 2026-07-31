import Foundation

enum AgentsPoller {
    enum Result: Sendable {
        case sessions([AgentSession])
        /// Hard failure — keep last sessions (timeout / bad exit / JSON).
        case keepLast
        case binaryMissing
    }

    private static let timeout: TimeInterval = 8

    static func fetch(claudeBinary: String) -> Result {
        guard let url = resolveExecutable(claudeBinary) else {
            return .binaryMissing
        }

        let proc = Process()
        proc.executableURL = url
        proc.arguments = ["agents", "--json"]
        proc.environment = enrichedEnvironment()
        let out = Pipe()
        proc.standardOutput = out
        // Discard stderr without allocating a pipe we have to drain (avoids FD leaks).
        proc.standardError = FileHandle.nullDevice

        // Drain stdout concurrently with termination. Waiting on waitUntilExit
        // *before* reading hung under the GUI app (child already gone, pipe still
        // full); the timeout path then drained+discarded valid JSON and returned
        // keepLast — freezing the menu bar on a stale busy snapshot.
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
            return .binaryMissing
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

        let timedOut = group.wait(timeout: .now() + timeout) == .timedOut
        if timedOut {
            if proc.isRunning {
                proc.terminate()
            }
            // Unblock a stuck readDataToEndOfFile and wait briefly for cleanup.
            try? out.fileHandleForReading.close()
            _ = group.wait(timeout: .now() + 1)
        } else {
            try? out.fileHandleForReading.close()
        }

        box.lock.lock()
        let data = box.data
        box.lock.unlock()

        if let sessions = parseSessions(data) {
            if timedOut {
                NSLog("[agents] recovered \(sessions.count) session(s) after wait timeout")
            }
            return .sessions(sessions)
        }
        if timedOut {
            NSLog("[agents] timed out (\(data.count) bytes)")
        } else if proc.terminationStatus != 0 {
            NSLog("[agents] exit \(proc.terminationStatus)")
        } else {
            NSLog("[agents] bad JSON (\(data.count) bytes)")
        }
        return .keepLast
    }

    static func parseSessions(_ data: Data) -> [AgentSession]? {
        guard !data.isEmpty,
              let obj = try? JSONSerialization.jsonObject(with: data),
              let arr = obj as? [[String: Any]]
        else {
            return nil
        }
        return SessionNormalizer.normalizeList(arr)
    }

    /// PATH for subprocesses: GUI apps only get `/usr/bin:/bin:/usr/sbin:/sbin`.
    private static func enrichedEnvironment() -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        let home = NSHomeDirectory()
        let extras = "/opt/homebrew/bin:/usr/local/bin:\(home)/.local/bin"
        env["PATH"] = extras + ":" + (env["PATH"] ?? "/usr/bin:/bin")
        return env
    }

    private static func resolveExecutable(_ name: String) -> URL? {
        let expanded = (name as NSString).expandingTildeInPath
        if expanded.contains("/") {
            return FileManager.default.isExecutableFile(atPath: expanded)
                ? URL(fileURLWithPath: expanded) : nil
        }
        // Prefer direct path checks — spawning `which` every 2s leaked pipes.
        let home = NSHomeDirectory()
        let candidates = [
            "\(home)/.local/bin/\(name)",
            "/opt/homebrew/bin/\(name)",
            "/usr/local/bin/\(name)",
            "/usr/bin/\(name)",
        ]
        for path in candidates {
            if FileManager.default.isExecutableFile(atPath: path) {
                return URL(fileURLWithPath: path)
            }
        }
        return which(name).map { URL(fileURLWithPath: $0) }
    }

    private static func which(_ name: String) -> String? {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/which")
        proc.arguments = [name]
        proc.environment = enrichedEnvironment()
        let out = Pipe()
        proc.standardOutput = out
        proc.standardError = FileHandle.nullDevice
        do {
            try proc.run()
        } catch {
            return nil
        }
        out.fileHandleForWriting.closeFile()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        out.fileHandleForReading.closeFile()
        if proc.isRunning {
            proc.waitUntilExit()
        }
        let s = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return s.isEmpty ? nil : s
    }
}
