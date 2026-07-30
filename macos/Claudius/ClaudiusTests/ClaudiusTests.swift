import XCTest
@testable import Claudius

final class AuthTests: XCTestCase {
    func testHMACMatchesKnownVector() {
        // HMAC-SHA256("secret", "nonce") — verify against CryptoKit / Python parity.
        let hex = Auth.hmacHex(secret: "secret", nonce: "nonce")
        XCTAssertEqual(
            hex,
            "042df25f9d1981866dfcab8bce6f4e3721168616b0a02d94a879c29e9f710b58"
        )
    }

    func testValidAuth() {
        let nonce = "abc123"
        let secret = "mypassword"
        let provided = Auth.hmacHex(secret: secret, nonce: nonce)
        XCTAssertTrue(Auth.isValid(provided: provided, secret: secret, nonce: nonce))
        XCTAssertFalse(Auth.isValid(provided: "deadbeef", secret: secret, nonce: nonce))
        XCTAssertFalse(Auth.isValid(provided: "", secret: secret, nonce: nonce))
    }
}

final class SessionNormalizerTests: XCTestCase {
    func testNormalizeAndSortActiveFirst() {
        let raw: [[String: Any]] = [
            [
                "cwd": "/Users/a/idle-proj",
                "kind": "local",
                "name": "idle",
                "startedAt": 100,
                "state": "idle",
                "status": "idle",
            ],
            [
                "cwd": "/Users/a/busy-proj",
                "kind": "local",
                "name": "busy",
                "startedAt": 50,
                "state": "working",
                "status": "busy",
                "waitingFor": "",
                "sessionId": "sess-1",
            ],
        ]
        let sessions = SessionNormalizer.normalizeList(raw)
        XCTAssertEqual(sessions.count, 2)
        XCTAssertEqual(sessions[0].name, "busy")
        XCTAssertTrue(sessions[0].isActive)
        XCTAssertEqual(sessions[1].name, "idle")
    }

    func testTruncate() {
        XCTAssertEqual(SessionNormalizer.truncate("hi", 10), "hi")
        XCTAssertEqual(SessionNormalizer.truncate("hello-world", 5), "hell…")
    }

    func testMaxSessions() {
        var raw: [[String: Any]] = []
        for i in 0..<12 {
            raw.append([
                "cwd": "/tmp/p\(i)",
                "name": "s\(i)",
                "startedAt": i,
                "state": "idle",
            ])
        }
        let sessions = SessionNormalizer.normalizeList(raw)
        XCTAssertEqual(sessions.count, 8)
    }
}

final class StatusPayloadTests: XCTestCase {
    func testStatusJSONShape() throws {
        var session = AgentSession(
            cwd: "proj",
            kind: "local",
            name: "proj",
            startedAt: 1,
            state: "working",
            status: "busy"
        )
        session.waitingFor = "permission"
        let payload = StatusPayload(
            usage: UsageSnapshot(
                sessionPct: 0.25,
                weeklyPct: 0.5,
                sessionReset: "1h 0m",
                weeklyReset: "2d 3h"
            ),
            sessions: [session]
        )
        let data = try payload.jsonData()
        let obj = try XCTUnwrap(
            JSONSerialization.jsonObject(with: data) as? [String: Any]
        )
        XCTAssertEqual(obj["agent_id"] as? String, "claude")
        XCTAssertEqual(obj["agent_display"] as? String, "Claude")
        XCTAssertEqual(obj["session_pct"] as? Double, 0.25)
        XCTAssertEqual(obj["weekly_pct"] as? Double, 0.5)
        let sessions = try XCTUnwrap(obj["sessions"] as? [[String: Any]])
        XCTAssertEqual(sessions.count, 1)
        XCTAssertEqual(sessions[0]["state"] as? String, "working")
        XCTAssertEqual(sessions[0]["waitingFor"] as? String, "permission")
    }

    func testConfigMessage() throws {
        let data = ProtocolMessages.config()
        let obj = try XCTUnwrap(
            JSONSerialization.jsonObject(with: data) as? [String: Any]
        )
        XCTAssertEqual(obj["type"] as? String, "config")
        XCTAssertEqual(obj["remote_control"] as? Bool, false)
        XCTAssertNotNil(obj["utc_offset"] as? Int)
        let agents = try XCTUnwrap(obj["agents"] as? [String: Any])
        let claude = try XCTUnwrap(agents["claude"] as? [String: Any])
        XCTAssertEqual(claude["color"] as? String, "#DE7356")
        XCTAssertEqual(
            claude["logo_bitmap"] as? String,
            CompanionConstants.logoBitmap
        )
    }

    func testLogoBitmapMatchesPython() {
        // Spot-check against known substring from claudius.py
        XCTAssertTrue(CompanionConstants.logoBitmap.contains("QHhg8cAAA"))
        XCTAssertFalse(CompanionConstants.logoBitmap.contains("QHbg8cAAA"))
    }
}

final class UsageTimeTests: XCTestCase {
    func testCountdown() {
        XCTAssertEqual(UsageTime.formatCountdown(diffSecs: 0), "--")
        XCTAssertEqual(UsageTime.formatCountdown(diffSecs: 90), "1m")
        XCTAssertEqual(UsageTime.formatCountdown(diffSecs: 3700), "1h 1m")
        XCTAssertEqual(UsageTime.formatCountdown(diffSecs: 90000), "1d 1h")
    }
}
