import Darwin
import Foundation

/// Bonjour advertiser for `_claudius._tcp` — mirrors claudius.py mDNS thread.
final class MDNSAdvertiser: NSObject, NetServiceDelegate, @unchecked Sendable {
    private let name: String
    private let port: Int32
    private var service: NetService?
    private var refreshTimer: Timer?
    private var currentIP: String?
    private let queue = DispatchQueue(label: "com.gmclaude.claudius.mdns")

    init(name: String, port: Int) {
        self.name = name
        self.port = Int32(port)
        super.init()
    }

    func start() {
        queue.async {
            self.publishIfNeeded()
            DispatchQueue.main.async {
                self.refreshTimer?.invalidate()
                self.refreshTimer = Timer.scheduledTimer(
                    withTimeInterval: 10,
                    repeats: true
                ) { [weak self] _ in
                    self?.queue.async { self?.publishIfNeeded() }
                }
            }
        }
    }

    func stop() {
        queue.async {
            DispatchQueue.main.async {
                self.refreshTimer?.invalidate()
                self.refreshTimer = nil
            }
            self.unpublish()
            self.currentIP = nil
        }
    }

    private func publishIfNeeded() {
        let ip = Self.localIP()
        if ip.hasPrefix("127.") {
            if currentIP != nil {
                unpublish()
                currentIP = nil
                NSLog("[mdns] network lost…")
            }
            return
        }
        if ip == currentIP, service != nil {
            return
        }
        unpublish()
        let svc = NetService(
            domain: "local.",
            type: CompanionConstants.mdnsType,
            name: name,
            port: port
        )
        svc.delegate = self
        // Include address so clients resolve reliably on some stacks.
        svc.includesPeerToPeer = false
        svc.publish()
        service = svc
        currentIP = ip
        NSLog("[mdns] advertising \(name) on \(ip):\(port)")
    }

    private func unpublish() {
        service?.stop()
        service = nil
    }

    static func localIP() -> String {
        var address = "127.0.0.1"
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return address }
        defer { freeifaddrs(ifaddr) }

        // Prefer the interface used for a UDP "connect" to a public IP (same as Python).
        let sock = socket(AF_INET, SOCK_DGRAM, 0)
        if sock >= 0 {
            var addr = sockaddr_in()
            addr.sin_family = sa_family_t(AF_INET)
            addr.sin_port = in_port_t(80).bigEndian
            inet_pton(AF_INET, "8.8.8.8", &addr.sin_addr)
            let bindResult = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    connect(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            if bindResult == 0 {
                var local = sockaddr_in()
                var len = socklen_t(MemoryLayout<sockaddr_in>.size)
                withUnsafeMutablePointer(to: &local) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        _ = getsockname(sock, $0, &len)
                    }
                }
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                inet_ntop(AF_INET, &local.sin_addr, &buf, socklen_t(INET_ADDRSTRLEN))
                address = String(cString: buf)
            }
            close(sock)
        }

        if !address.hasPrefix("127.") {
            return address
        }

        // Fallback: first non-loopback IPv4.
        var ptr: UnsafeMutablePointer<ifaddrs>? = first
        while let ifa = ptr {
            defer { ptr = ifa.pointee.ifa_next }
            guard let sa = ifa.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else {
                continue
            }
            var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(
                sa,
                socklen_t(sa.pointee.sa_len),
                &hostname,
                socklen_t(hostname.count),
                nil,
                0,
                NI_NUMERICHOST
            )
            let ip = String(cString: hostname)
            if !ip.hasPrefix("127.") {
                return ip
            }
        }
        return address
    }

    // MARK: NetServiceDelegate

    func netServiceDidPublish(_ sender: NetService) {
        NSLog("[mdns] published \(sender.name).\(sender.type)")
    }

    func netService(_ sender: NetService, didNotPublish errorDict: [String: NSNumber]) {
        NSLog("[mdns] failed: \(errorDict)")
        queue.async {
            self.service = nil
            self.currentIP = nil
        }
    }
}
