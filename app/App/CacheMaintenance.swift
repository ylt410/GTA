import Foundation

/// Only generated logs and abandoned installer work are maintained here. Game data,
/// saves, Mods, ModsExpanded and downloaded/offline asset caches are never swept.
enum CacheMaintenance {
    static let logLimit = 16 << 20
    static let logHead = 256 << 10
    static let logTail = 8 << 20
    private static let activeLock = NSLock()
    private static var activeImports = Set<String>()

    /// Reads a bounded head and tail even when an older app left a multi-GB log.
    static func readLog(_ url: URL, limit: Int, headBytes: Int = logHead) -> Data? {
        guard limit > 0, let file = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? file.close() }
        guard let size = try? file.seekToEnd() else { return nil }
        if size <= UInt64(limit) {
            try? file.seek(toOffset: 0)
            return try? file.read(upToCount: limit)
        }
        let head = min(max(0, headBytes), limit / 2)
        let marker = Data("\n… earlier log lines omitted …\n".utf8)
        let tail = max(0, limit - head - marker.count)
        try? file.seek(toOffset: 0)
        var result = (try? file.read(upToCount: head)) ?? Data()
        result.append(marker.prefix(max(0, limit - result.count)))
        try? file.seek(toOffset: size - UInt64(tail))
        result.append((try? file.read(upToCount: tail)) ?? Data())
        return result
    }

    /// Called before launch rotation, so stopped/previous copies are bounded too.
    static func boundLog(_ url: URL, maximum: Int = logLimit) {
        guard let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey]),
              values.isRegularFile == true, values.isSymbolicLink != true,
              (values.fileSize ?? 0) > maximum,
              let data = readLog(url, limit: min(maximum, logHead + logTail), headBytes: logHead) else { return }
        try? data.write(to: url, options: .atomic)
    }

    static func prepareLaunch(documents: URL, temporary: URL = FileManager.default.temporaryDirectory) {
        for name in ["native-port.log", "native-port-previous.log", "native-port-stopped.log"] {
            boundLog(documents.appendingPathComponent(name))
        }
        pruneImports(in: temporary)
    }

    static func beginImport(_ folder: URL) {
        activeLock.lock(); activeImports.insert(folder.standardizedFileURL.path); activeLock.unlock()
    }

    static func removeImport(_ folder: URL) {
        activeLock.lock(); defer { activeLock.unlock() }
        // Only folders created by ModInstaller.unpack may reach this method.
        guard isImportName(folder.lastPathComponent), activeImports.contains(folder.standardizedFileURL.path) else { return }
        try? FileManager.default.removeItem(at: folder)
        activeImports.remove(folder.standardizedFileURL.path)
    }

    private static func isImportName(_ name: String) -> Bool {
        let prefix = "mod-install-"
        return name.hasPrefix(prefix) && UUID(uuidString: String(name.dropFirst(prefix.count))) != nil
    }

    /// Enumerates one known temporary directory, never recursively follows links.
    /// A long-running import/confirmation remains protected by its active lease.
    @discardableResult static func pruneImports(in root: URL, now: Date = Date()) -> Int {
        guard let rootValues = try? root.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey]),
              rootValues.isDirectory == true, rootValues.isSymbolicLink != true else { return 0 }
        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isSymbolicLinkKey, .contentModificationDateKey, .creationDateKey]
        guard let children = try? FileManager.default.contentsOfDirectory(at: root, includingPropertiesForKeys: Array(keys)) else { return 0 }
        var removed = 0
        activeLock.lock(); defer { activeLock.unlock() }
        for child in children {
            guard isImportName(child.lastPathComponent), !activeImports.contains(child.standardizedFileURL.path),
                  let values = try? child.resourceValues(forKeys: keys), values.isDirectory == true,
                  values.isSymbolicLink != true,
                  let modified = values.contentModificationDate ?? values.creationDate,
                  now.timeIntervalSince(modified) > 24 * 60 * 60 else { continue }
            do { try FileManager.default.removeItem(at: child); removed += 1 } catch { }
        }
        return removed
    }
}

/// One open handle, bounded memory and disk. Callers serialize access with their
/// logger lock. Compaction keeps boot diagnostics and the newest failure context.
final class BoundedLogFile {
    private var file: FileHandle?
    private var size: UInt64 = 0
    private var pending = Data()
    private let maximum: Int
    private let head: Int
    private let tail: Int
    private let batch: Int

    init(maximum: Int = CacheMaintenance.logLimit, head: Int = CacheMaintenance.logHead,
         tail: Int = CacheMaintenance.logTail, batch: Int = 64 << 10) {
        self.maximum = max(1024, maximum); self.head = max(0, head)
        self.tail = max(0, tail); self.batch = max(1024, batch)
    }
    deinit { flush(); try? file?.close() }

    func open(_ url: URL) {
        flush(); try? file?.close(); file = nil
        CacheMaintenance.boundLog(url, maximum: maximum)
        file = try? FileHandle(forUpdating: url)
        size = (try? file?.seekToEnd()) ?? 0
    }

    func append(_ line: String, critical: Bool = false) {
        // A malformed engine message must not make either queue unbounded.
        let limit = 64 << 10
        pending.append(contentsOf: line.utf8.prefix(limit)); pending.append(10)
        if line.utf8.count > limit { pending.append(Data("[long log line truncated]\n".utf8)) }
        if pending.count >= batch || critical { flush(synchronize: critical) }
    }

    func flush(synchronize: Bool = false) {
        guard let file, !pending.isEmpty else { return }
        do {
            try file.seek(toOffset: size)
            try file.write(contentsOf: pending)
            size += UInt64(pending.count); pending.removeAll(keepingCapacity: true)
            if size > UInt64(maximum) { try compact(file) }
            if synchronize { try file.synchronize() }
        } catch {
            // Logging is diagnostic: a full disk must not stop the game or retain
            // an ever-growing retry buffer.
            pending.removeAll(keepingCapacity: false)
            size = (try? file.seekToEnd()) ?? size
        }
    }

    private func compact(_ file: FileHandle) throws {
        let marker = Data("\n… earlier log lines omitted …\n".utf8)
        let keepHead = min(head, maximum / 4)
        let keepTail = min(tail, maximum - keepHead - marker.count)
        try file.seek(toOffset: 0)
        var compacted = try file.read(upToCount: keepHead) ?? Data()
        compacted.append(marker)
        try file.seek(toOffset: size - UInt64(keepTail))
        compacted.append(try file.read(upToCount: keepTail) ?? Data())
        try file.seek(toOffset: 0); try file.write(contentsOf: compacted)
        try file.truncate(atOffset: UInt64(compacted.count))
        size = UInt64(compacted.count)
    }
}

/// Bounded while the main thread is busy. The UI is diagnostic, so retaining the
/// newest messages is preferable to enqueuing millions of callbacks or strings.
struct BoundedLogLines {
    private struct Entry { let sequence: UInt64; let line: String }
    private var lines: [Entry] = []
    private var signals: [String: Entry] = [:]
    private var sequence: UInt64 = 0
    private var bytes = 0
    private var dropped = 0
    /// These lines drive startup/stop state in handleLine, so they cannot be
    /// discarded with ordinary diagnostic spam. One latest value per fixed key.
    private func signalKey(_ line: String) -> String? {
        if line == "WORLD_READY" || line == "ASSET_WARM_COMPLETE" || line == "iOS could not reserve the engine memory." { return line }
        if line.hasPrefix("{"), line.contains("\"label\":\"First frame\"") { return "first-frame" }
        for prefix in ["ASSET_WARM_PROGRESS ", "Native trap", "Native boot stopped:", "Native game thread stopped:", "Engine stopped at unported host function:", "Mods active: ", "Script mods active: ", "[wgpu] fps:", "[wgpu] draw costs", "Native frame costs:", "Phone memory:"] {
            if line.hasPrefix(prefix) { return prefix }
        }
        return nil
    }
    mutating func append(_ line: String) {
        let line = String(line.prefix(4096))
        sequence &+= 1
        let entry = Entry(sequence: sequence, line: line)
        if let key = signalKey(line) { signals[key] = entry }
        lines.append(entry); bytes += line.utf8.count
        while lines.count > 128 || bytes > 32 << 10 {
            bytes -= lines.removeFirst().line.utf8.count; dropped += 1
        }
    }
    mutating func drain() -> [String] {
        let inTail = Set(lines.map { $0.sequence })
        let retained = signals.values.filter { !inTail.contains($0.sequence) }
        var result = (lines + retained).sorted { $0.sequence < $1.sequence }.map { $0.line }
        if dropped > 0 { result.insert("[UI log: \(dropped) earlier messages omitted; file log retained]", at: 0) }
        lines.removeAll(keepingCapacity: true); signals.removeAll(keepingCapacity: true); bytes = 0; dropped = 0
        return result
    }
}
