import Foundation

enum AgentsPoller {
    enum Result: Sendable {
        case sessions([AgentSession])
        /// Hard failure — keep last sessions (timeout / bad exit / JSON).
        case keepLast
        case binaryMissing
    }

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

        do {
            try proc.run()
        } catch {
            return .binaryMissing
        }
        // Parent must close its copy of the write end or the read may hang and FDs leak.
        out.fileHandleForWriting.closeFile()

        let group = DispatchGroup()
        group.enter()
        DispatchQueue.global().async {
            proc.waitUntilExit()
            group.leave()
        }
        if group.wait(timeout: .now() + 8) == .timedOut {
            proc.terminate()
            // Drain/close so a timed-out child cannot leave pipes open.
            _ = out.fileHandleForReading.readDataToEndOfFile()
            out.fileHandleForReading.closeFile()
            return .keepLast
        }

        let data = out.fileHandleForReading.readDataToEndOfFile()
        out.fileHandleForReading.closeFile()
        if proc.terminationStatus != 0 {
            return .keepLast
        }
        guard let obj = try? JSONSerialization.jsonObject(with: data),
              let arr = obj as? [[String: Any]]
        else {
            return .keepLast
        }
        return .sessions(SessionNormalizer.normalizeList(arr))
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
        proc.waitUntilExit()
        let data = out.fileHandleForReading.readDataToEndOfFile()
        out.fileHandleForReading.closeFile()
        let s = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return s.isEmpty ? nil : s
    }
}
