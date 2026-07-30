import Foundation

enum UsageTime {
    static func formatCountdown(diffSecs: Int) -> String {
        if diffSecs <= 0 { return "--" }
        let days = diffSecs / 86400
        let hours = (diffSecs % 86400) / 3600
        let mins = (diffSecs % 3600) / 60
        if days > 0 { return "\(days)d \(hours)h" }
        if hours > 0 { return "\(hours)h \(mins)m" }
        return "\(mins)m"
    }

    static func formatISOCountdown(_ iso: String) -> String {
        guard !iso.isEmpty else { return "--" }
        let cleaned = iso.replacingOccurrences(of: "Z", with: "+00:00")
        let withFrac = ISO8601DateFormatter()
        withFrac.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let plain = ISO8601DateFormatter()
        plain.formatOptions = [.withInternetDateTime]
        let date = withFrac.date(from: cleaned)
            ?? plain.date(from: cleaned)
            ?? withFrac.date(from: iso)
            ?? plain.date(from: iso)
        guard let date else { return "--" }
        let diff = Int(date.timeIntervalSinceNow)
        return formatCountdown(diffSecs: diff)
    }
}
