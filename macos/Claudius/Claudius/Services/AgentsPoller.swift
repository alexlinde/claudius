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
        var env = ProcessInfo.processInfo.environment
        let extras = "/opt/homebrew/bin:/usr/local/bin:\(NSHomeDirectory())/.local/bin"
        env["PATH"] = extras + ":" + (env["PATH"] ?? "/usr/bin:/bin")
        proc.environment = env
        let out = Pipe()
        proc.standardOutput = out
        proc.standardError = Pipe()

        do {
            try proc.run()
        } catch {
            return .binaryMissing
        }

        let group = DispatchGroup()
        group.enter()
        DispatchQueue.global().async {
            proc.waitUntilExit()
            group.leave()
        }
        if group.wait(timeout: .now() + 8) == .timedOut {
            proc.terminate()
            return .keepLast
        }

        let data = out.fileHandleForReading.readDataToEndOfFile()
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

    private static func resolveExecutable(_ name: String) -> URL? {
        if name.contains("/") {
            let url = URL(fileURLWithPath: name)
            return FileManager.default.isExecutableFile(atPath: url.path) ? url : nil
        }
        return which(name).map { URL(fileURLWithPath: $0) }
    }

    private static func which(_ name: String) -> String? {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/which")
        proc.arguments = [name]
        let out = Pipe()
        proc.standardOutput = out
        proc.standardError = Pipe()
        var env = ProcessInfo.processInfo.environment
        let extras = "/opt/homebrew/bin:/usr/local/bin:\(NSHomeDirectory())/.local/bin"
        env["PATH"] = extras + ":" + (env["PATH"] ?? "/usr/bin:/bin")
        proc.environment = env
        do {
            try proc.run()
            proc.waitUntilExit()
        } catch {
            return nil
        }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        let s = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return s.isEmpty ? nil : s
    }
}
