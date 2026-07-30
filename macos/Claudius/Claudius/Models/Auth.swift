import CryptoKit
import Foundation

enum Auth {
    /// HMAC-SHA256(secret, nonce) as lowercase hex — matches claudius.py / firmware.
    static func hmacHex(secret: String, nonce: String) -> String {
        let key = SymmetricKey(data: Data(secret.utf8))
        let mac = HMAC<SHA256>.authenticationCode(
            for: Data(nonce.utf8),
            using: key
        )
        return mac.map { String(format: "%02x", $0) }.joined()
    }

    static func isValid(provided: String, secret: String, nonce: String) -> Bool {
        guard !provided.isEmpty else { return false }
        let expected = hmacHex(secret: secret, nonce: nonce)
        return constantTimeEqual(provided, expected)
    }

    private static func constantTimeEqual(_ a: String, _ b: String) -> Bool {
        let ad = Array(a.utf8)
        let bd = Array(b.utf8)
        guard ad.count == bd.count else { return false }
        var diff: UInt8 = 0
        for i in 0..<ad.count {
            diff |= ad[i] ^ bd[i]
        }
        return diff == 0
    }
}
