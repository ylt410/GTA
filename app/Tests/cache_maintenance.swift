import Foundation

@main struct CacheMaintenanceChecks {
    static func main() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent("cache-maintenance-check-\(UUID().uuidString)")
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }

        // An old sparse multi-GB log is bounded without reading it into memory.
        let sparse = root.appendingPathComponent("old.log")
        fm.createFile(atPath: sparse.path, contents: Data("BOOT MARKER\n".utf8))
        let old = try FileHandle(forUpdating: sparse)
        try old.seek(toOffset: UInt64(3) << 30)
        try old.write(contentsOf: Data("\nNative trap: final failure\n".utf8)); try old.close()
        let preview = CacheMaintenance.readLog(sparse, limit: 4096)!
        precondition(preview.count <= 4096)
        precondition(String(decoding: preview, as: UTF8.self).contains("BOOT MARKER"))
        precondition(String(decoding: preview, as: UTF8.self).contains("final failure"))
        CacheMaintenance.boundLog(sparse, maximum: 8192)
        let bounded = try Data(contentsOf: sparse)
        precondition(bounded.count <= 8192 && String(decoding: bounded, as: UTF8.self).contains("final failure"))

        // Repeated compaction preserves boot diagnostics and the newest error.
        let log = root.appendingPathComponent("bounded.log")
        fm.createFile(atPath: log.path, contents: Data())
        let writer = BoundedLogFile(maximum: 8192, head: 256, tail: 4096, batch: 1024)
        writer.open(log); writer.append("BOOT PRESERVED", critical: true)
        for i in 0..<20_000 { writer.append("line \(i) — 测试音频与纹理 😀") }
        writer.append(String(repeating: "🚓", count: 100_000))
        writer.append("Native trap: most recent failure", critical: true)
        writer.flush()
        let result = try Data(contentsOf: log)
        precondition(result.count <= 8192)
        let text = String(decoding: result, as: UTF8.self)
        precondition(text.contains("BOOT PRESERVED") && text.contains("most recent failure"))
        // A prior truncated UTF-8 line remains readable with replacement scalars.
        precondition(!text.isEmpty)

        var ui = BoundedLogLines()
        for i in 0..<10_000 { ui.append("\(i): " + String(repeating: "界", count: 400)) }
        let batch = ui.drain()
        precondition(batch.count <= 129 && batch.last!.hasPrefix("9999:"))
        precondition(batch.reduce(0) { $0 + $1.utf8.count } < 34 << 10)
        precondition(ui.drain().isEmpty)
        ui.append("Mods active: 4 files, 1 add-on pack")
        ui.append("Script mods active: 2")
        ui.append("ASSET_WARM_PROGRESS 1 42")
        ui.append("ASSET_WARM_PROGRESS 99 123")
        ui.append("ASSET_WARM_COMPLETE")
        ui.append("WORLD_READY")
        ui.append("{\"label\":\"First frame\"}")
        ui.append("Native game thread stopped: retained failure")
        for i in 0..<10_000 { ui.append("noisy diagnostic \(i)") }
        let signals = ui.drain()
        precondition(signals.contains("WORLD_READY") && signals.contains("ASSET_WARM_COMPLETE"))
        precondition(signals.contains("Native game thread stopped: retained failure"))
        precondition(signals.contains("{\"label\":\"First frame\"}"))
        precondition(signals.contains("Mods active: 4 files, 1 add-on pack") && signals.contains("Script mods active: 2"))
        precondition(signals.contains("ASSET_WARM_PROGRESS 99 123") && !signals.contains("ASSET_WARM_PROGRESS 1 42"))
        precondition(signals.firstIndex(of: "WORLD_READY")! < signals.firstIndex(of: "Native game thread stopped: retained failure")!)
        for _ in 0..<1000 { ui.append("Phone memory: " + String(repeating: "😀", count: 100_000)) }
        let unicodeSignal = ui.drain()
        precondition(unicodeSignal.count <= 129 && unicodeSignal.reduce(0) { $0 + $1.utf8.count } < 64 << 10)

        let temporary = root.appendingPathComponent("tmp")
        let outside = root.appendingPathComponent("protected")
        try fm.createDirectory(at: temporary, withIntermediateDirectories: true)
        try fm.createDirectory(at: outside, withIntermediateDirectories: true)
        let marker = outside.appendingPathComponent("save-and-game-data")
        try Data("KEEP".utf8).write(to: marker)
        let now = Date(), yesterday = now.addingTimeInterval(-2 * 24 * 60 * 60)
        func work(_ name: String, stale: Bool = true) throws -> URL {
            let dir = temporary.appendingPathComponent(name)
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
            try Data("temporary".utf8).write(to: dir.appendingPathComponent("file"))
            if stale { try fm.setAttributes([.modificationDate: yesterday], ofItemAtPath: dir.path) }
            return dir
        }
        let abandoned = try work("mod-install-\(UUID().uuidString)")
        let active = try work("mod-install-\(UUID().uuidString)")
        let recent = try work("mod-install-\(UUID().uuidString)", stale: false)
        let unrelated = try work("Mods")
        let malformed = try work("mod-install-not-a-uuid")
        let symlink = temporary.appendingPathComponent("mod-install-\(UUID().uuidString)")
        try fm.createSymbolicLink(at: symlink, withDestinationURL: outside)
        CacheMaintenance.beginImport(active)
        precondition(CacheMaintenance.pruneImports(in: temporary, now: now) == 1)
        precondition(!fm.fileExists(atPath: abandoned.path))
        for path in [active, recent, unrelated, malformed, symlink, marker] {
            precondition(fm.fileExists(atPath: path.path))
        }
        let rootLink = root.appendingPathComponent("linked-temp")
        try fm.createSymbolicLink(at: rootLink, withDestinationURL: temporary)
        precondition(CacheMaintenance.pruneImports(in: rootLink, now: now) == 0)
        // Removing a stale work folder unlinks nested symlinks, not their target.
        let nested = try work("mod-install-\(UUID().uuidString)")
        try fm.createSymbolicLink(at: nested.appendingPathComponent("escape"), withDestinationURL: outside)
        try fm.setAttributes([.modificationDate: yesterday], ofItemAtPath: nested.path)
        precondition(CacheMaintenance.pruneImports(in: temporary, now: now) == 1)
        precondition(fm.fileExists(atPath: marker.path))
        CacheMaintenance.removeImport(active)
        precondition(!fm.fileExists(atPath: active.path) && fm.fileExists(atPath: marker.path))
        print("PASS: sparse huge logs, bounded reads/writes/compaction, UTF-8 flooding, bounded UI, stale/recent/active imports and symlink/protected data.")
    }
}
