import Foundation

/// Box handed to the C read callback. Keeps the last two blocks it handed out (the sources keep
/// them valid that long), so the exact archive offset of a zip local header can be found.
private final class SourceBox {
    let source: ArchiveByteSource
    var head = Data()
    /// RAR5 restart: the archive's opening bytes, handed before the source's first block.
    var prefix: NSData?
    var prefixLength: Int64 { Int64(prefix?.length ?? 0) }
    var prefixHanded = false
    /// Bytes handed to libarchive in this run.
    var handed: Int64 = 0
    var blocks: [(start: Int64, pointer: UnsafeRawPointer, length: Int)] = []
    init(_ source: ArchiveByteSource) { self.source = source }

    func byte(at offset: Int64) -> UInt8? {
        for b in blocks where offset >= b.start && offset < b.start + Int64(b.length) {
            return b.pointer.load(fromByteOffset: Int(offset - b.start), as: UInt8.self)
        }
        return nil
    }

    /// Stream offset of the zip local file header that ends at `end`. libarchive's consumed position
    /// right after it read a regular entry's header is exactly header + 30 + name length + extra
    /// length, so the header is the "PK\3\4" whose own lengths reach exactly `end`.
    /// (archive_read_header_position() is only a lower bound: the streamable zip reader records it
    /// before it consumes the previous entry's tail and data descriptor.)
    func zipHeaderStart(endingAt end: Int64, notBefore: Int64) -> Int64? {
        var h = end - 30
        let lowest = max(notBefore, end - 30 - 0x1FFFE)
        while h >= lowest {
            if byte(at: h) == 0x50, byte(at: h + 1) == 0x4B, byte(at: h + 2) == 3, byte(at: h + 3) == 4,
               let n0 = byte(at: h + 26), let n1 = byte(at: h + 27), let e0 = byte(at: h + 28), let e1 = byte(at: h + 29) {
                let size = 30 + (Int64(n0) | (Int64(n1) << 8)) + (Int64(e0) | (Int64(e1) << 8))
                if h + size == end { return h }
            }
            h -= 1
        }
        return nil
    }
}

private let readCallback: gi_read_fn = { context, bufferOut in
    let box = Unmanaged<SourceBox>.fromOpaque(context!).takeUnretainedValue()
    let next: (UnsafeRawPointer, Int)?
    if let prefix = box.prefix, !box.prefixHanded {
        box.prefixHanded = true
        next = (prefix.bytes, prefix.length)
    } else {
        next = box.source.nextBlock()
    }
    guard let (pointer, length) = next else { return -1 }
    // The first bytes: format checks, and a RAR5's signature and main header (at most a few hundred bytes).
    if box.head.count < 4096 && length > 0 {
        box.head.append(pointer.assumingMemoryBound(to: UInt8.self), count: min(4096 - box.head.count, length))
    }
    if length > 0 {
        box.blocks.append((box.handed, pointer, length))
        if box.blocks.count > 2 { box.blocks.removeFirst() }
        box.handed += Int64(length)
    }
    bufferOut!.pointee = pointer
    return length
}

final class InstallJob {
    /// Tests only (macOS harness): called with ("data", bytes written of the current file, its
    /// declared size) after each block, and ("filedone", files done, size) right after a file was
    /// renamed into place, before the resume record is updated. Always nil in the app.
    static var testHook: ((String, Int64, Int64) -> Void)?
    private let source: GameInstaller.Source
    private let destination: URL
    private let manifestURL: URL
    private let options: GameInstaller.Options
    private let progressHandler: (GameInstaller.Progress) -> Void
    private var completionHandler: ((Result<GameInstaller.Report, InstallError>) -> Void)?
    private weak var task: InstallTask?

    private let lock = NSLock()
    // --- guarded by lock ---
    private var cancelled = false
    private var phase: GameInstaller.Phase = .checking
    private var extracted: Int64 = 0
    private var filesDone = 0
    private var currentFile: String?
    private var samples: [(Date, Int64)] = []
    private var peakIncrease: Int64 = 0
    private var streamBase: Int64 = 0
    // ------------------------

    private var manifest: GameManifest!
    /// Replaced when a resume falls back to the start, while the progress timer reads it: locked.
    private let sourceLock = NSLock()
    private var sourceStore: ArchiveByteSource?
    private var byteSource: ArchiveByteSource? {
        get { sourceLock.lock(); defer { sourceLock.unlock() }; return sourceStore }
        set { sourceLock.lock(); let old = sourceStore; sourceStore = newValue; sourceLock.unlock(); _ = old }
    }
    private var local: LocalFileSource? { byteSource as? LocalFileSource }
    private var remote: RemoteSource? { byteSource as? RemoteSource }
    private var timer: DispatchSourceTimer?
    private var startFootprint: Int64 = 0
    private var partCount = 1
    private var partNames: [String] = []
    private var createdFolders = Set<String>()
    private let started = Date()

    // --- resume (unpacker thread) ---
    private enum StartPlan { case fresh, offset(Int64), skipDone }
    private let store: ResumeStore
    private var record: ResumeRecord?
    private var plan = StartPlan.fresh
    /// Manifest indexes fully written (this run or kept from an earlier one).
    private var done: [Bool] = []
    /// Sum of the sizes of the files in `done`.
    private var doneBytes: Int64 = 0
    /// An earlier run left a record with progress: link errors then mean "expired".
    private var resuming = false
    private var keptFromEarlier = 0
    private var resumeNote: String?
    private var zipMode = false
    /// A RAR5 the install can restart in the middle of (see ResumeRecord.rarHead).
    private var rarMode = false
    private var lastSave = Date.distantPast
    private var fileCompletedSinceSave = false
    private var durableOffset: Int64 = 0
    private var missedHeaderOffsets = 0
    private static let durableEvery: Int64 = 64 << 20
    private static let saveEvery: TimeInterval = 3

    init(source: GameInstaller.Source, destination: URL, manifestURL: URL, options: GameInstaller.Options,
         task: InstallTask, progress: @escaping (GameInstaller.Progress) -> Void,
         completion: @escaping (Result<GameInstaller.Report, InstallError>) -> Void) {
        self.source = source
        self.destination = destination.standardizedFileURL
        self.manifestURL = manifestURL
        self.options = options
        self.task = task
        self.progressHandler = progress
        self.completionHandler = completion
        store = ResumeStore(destination: destination.standardizedFileURL)
    }

    private func log(_ line: String) { options.log?(line) }

    func start() {
        let thread = Thread { [self] in run() }
        thread.name = "GameInstaller"
        thread.qualityOfService = .userInitiated
        thread.stackSize = 1 << 20
        thread.start()
    }

    func cancel() {
        lock.lock(); cancelled = true; lock.unlock()
        byteSource?.cancel()
    }

    private var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return cancelled }

    private var dataFolder: URL { destination.appendingPathComponent("data", isDirectory: true) }
    private var workFolder: URL { destination.appendingPathComponent(".gameinstaller", isDirectory: true) }

    // MARK: Main sequence

    private func run() {
        log("GameInstaller: \(String(cString: gi_libarchive_version()))")
        do {
            manifest = try GameManifest(url: manifestURL)
            log("Game file list: \(manifest.files.count) files, \(formatBytes(manifest.totalBytes))")
            do { try FileManager.default.createDirectory(at: dataFolder, withIntermediateDirectories: true) } catch {
                throw InstallError(.writeFailed, "The game folder could not be created.", detail: error.localizedDescription)
            }
            try planResume()
            try prepareSource()
            try checkSpace()
            if isCancelled { throw InstallError(.cancelled, "The install was cancelled.") }
            saveRecord()
            startTimer()
            var outcome: (prefix: String, skipped: Int, unsafe: [String], format: String)
            var restarts = 0
            while true {
                set(phase: byteSource is RemoteSource ? .downloading : .unpacking)
                remote?.start()
                do {
                    outcome = try extract()
                    break
                } catch let rejected as ResumeRejected {
                    byteSource?.cancel()
                    byteSource?.finish(success: false)
                    restarts += 1
                    guard restarts <= 2 else {
                        throw InstallError(.corrupted, "The archive could not be read consistently. Download it again.", detail: rejected.reason)
                    }
                    try startOver(after: rejected)
                }
            }
            let (prefix, skipped, unsafe, format) = outcome
            byteSource?.finish(success: true)
            set(phase: .verifying)
            try verify()
            if options.writeManifest { writeManifestCopy() }
            try? FileManager.default.removeItem(at: workFolder)
            store.remove()
            sampleDisk()
            stopTimer()
            set(phase: .finished)
            let stats = remote?.stats
            lock.lock()
            let report = GameInstaller.Report(
                dataFolder: dataFolder, filesWritten: filesDone, bytesWritten: extracted,
                duration: Date().timeIntervalSince(started), archivePrefix: prefix, skippedEntries: skipped,
                unsafeEntries: unsafe, peakPendingBytes: stats?.peak ?? 0, peakDiskIncrease: peakIncrease,
                downloadedBytes: stats?.downloaded ?? (local?.bytesConsumed ?? 0), retries: stats?.retries ?? 0, format: format,
                resumedFromByte: streamBase, filesKeptFromEarlier: keptFromEarlier, resumeNote: resumeNote,
                toleratedSizeMismatches: toleratedMismatches)
            lock.unlock()
            log("Install finished: \(report.filesWritten) files, \(formatBytes(report.bytesWritten)) in \(String(format: "%.0f", report.duration)) s; read/downloaded this run \(formatBytes(report.downloadedBytes)) (\(report.downloadedBytes) bytes) starting at byte \(report.resumedFromByte)\(stats.map { $0.reused > 0 ? "; reused \($0.reused) bytes the interrupted run had downloaded" : "" } ?? ""); kept from earlier \(report.filesKeptFromEarlier) files; peak pending \(formatBytes(report.peakPendingBytes)), peak disk increase \(formatBytes(report.peakDiskIncrease)), retries \(report.retries)")
            emitProgress()
            finish(.success(report))
        } catch let error as InstallError {
            var error = error
            if isCancelled { error = InstallError(.cancelled, "The install was cancelled.") }
            // The next run is expected to continue: keep what was downloaded but not unpacked yet.
            let keepChunks = record != nil && (error.kind == .linkExpired || error.kind == .networkLost)
            remote?.keepChunks = keepChunks
            byteSource?.cancel()
            byteSource?.finish(success: false)
            if !keepChunks { try? FileManager.default.removeItem(at: workFolder) }
            stopTimer()
            if record != nil {
                if error.kind == .verificationFailed {
                    store.remove() // continuing can't fix this
                } else {
                    record?.lastError = error.kind.rawValue
                    record?.lastErrorMessage = error.message
                    saveRecord()
                    log("Resume point kept: archive byte \(record?.offset ?? 0), \(filesDoneNow) files done")
                }
            }
            log("Install failed: \(error)")
            finish(.failure(error))
        } catch {
            stopTimer()
            finish(.failure(InstallError(.writeFailed, "The install failed: \(error.localizedDescription)")))
        }
    }

    private var filesDoneNow: Int { lock.lock(); defer { lock.unlock() }; return filesDone }

    private func finish(_ result: Result<GameInstaller.Report, InstallError>) {
        guard let completion = completionHandler else { return }
        completionHandler = nil
        options.callbackQueue.async { completion(result); self.task?.job = nil }
    }

    // MARK: Resume

    /// The record a fresh install of `source` starts with.
    private func freshRecord() -> ResumeRecord {
        var urls: [URL] = [], files: [URL] = []
        switch source {
        case .url(let u): urls = [u]
        case .urls(let u): urls = u
        case .file(let f): files = (try? ArchiveParts.resolve(f)) ?? [f]
        case .files(let f): files = f
        }
        let isURL = files.isEmpty
        let names = isURL ? urls.map(\.lastPathComponent) : files.map(\.lastPathComponent)
        var sizes: [Int64?] = names.map { _ in nil }, mtimes: [Double?] = names.map { _ in nil }
        for (i, f) in files.enumerated() {
            var st = stat()
            if stat(f.path, &st) == 0 {
                sizes[i] = Int64(st.st_size)
                mtimes[i] = Double(st.st_mtimespec.tv_sec) + Double(st.st_mtimespec.tv_nsec) / 1e9
            }
        }
        return ResumeRecord(sourceKind: isURL ? "url" : "file", urls: isURL ? urls.map(\.absoluteString) : nil,
                            partNames: names, partSizes: sizes, partMtimes: mtimes,
                            etags: names.map { _ in nil }, lastModifieds: names.map { _ in nil },
                            serverSupportsRange: nil, modifiesSource: !isURL && options.allowModifyingSource,
                            manifestFiles: manifest.files.count, manifestBytes: manifest.totalBytes)
    }

    /// Decides how this install starts: fresh, at the saved offset (zip), or from the start while
    /// keeping the files an earlier run finished (other formats, or when the offset can't be used).
    private func planResume() throws {
        let fresh = freshRecord()
        done = Array(repeating: false, count: manifest.files.count)
        record = fresh
        plan = .fresh
        if options.startFresh {
            if store.load() != nil { log("Starting fresh as asked; the earlier install's resume point is discarded.") }
            store.remove()
            return
        }
        guard let old = store.load() else { return }
        guard old.filesDone > 0 || old.offset > 0 else { return } // nothing worth continuing
        func startOver(_ why: String) {
            log("An earlier, interrupted install was found but can't be continued: \(why). Starting from the beginning; every file is written again.")
            resumeNote = "The earlier install could not be continued (\(why)), so it started from the beginning."
        }
        guard old.manifestFiles == manifest.files.count, old.manifestBytes == manifest.totalBytes else {
            return startOver("the game's file list changed")
        }
        // Local multi-part sets: parts the earlier run finished may be deleted already, so the given
        // file may resolve to fewer parts; those must all be parts of the earlier set.
        guard old.sourceKind == fresh.sourceKind,
              old.partNames.count == fresh.partNames.count || (old.sourceKind == "file" && fresh.partNames.allSatisfy(old.partNames.contains)) else {
            return startOver("it was installing from a different \(old.sourceKind == "url" ? "link" : "file")")
        }
        var merged = old
        if old.sourceKind == "file" {
            guard old.partNames == fresh.partNames || fresh.partNames.allSatisfy(old.partNames.contains) else {
                return startOver("it was installing from \(old.partNames.first ?? "another file"), not \(fresh.partNames.first ?? "?")")
            }
            for (i, name) in old.partNames.enumerated() {
                guard let j = fresh.partNames.firstIndex(of: name), let size = fresh.partSizes[j] else { continue } // gone: checked later
                if old.partSizes[i] != size { return startOver("\(name) has a different size now") }
                if !old.modifiesSource, let was = old.partMtimes[safe: i] ?? nil, let now = fresh.partMtimes[j] ?? nil, abs(was - now) > 0.001 {
                    return startOver("\(name) was modified since")
                }
            }
            merged.modifiesSource = options.allowModifyingSource || old.modifiesSource
        } else {
            if old.urls != fresh.urls {
                log("The link is different from the earlier install's; continuing only if the server reports the same file (size, ETag/Last-Modified).")
            }
            merged.urls = fresh.urls
            merged.partNames = fresh.partNames
        }
        // Files the earlier run finished: keep the ones that are still there with the right size.
        var bits = ResumeRecord.decodeBitmap(old.done, count: manifest.files.count)
        var kept = 0, dropped = 0, bytes: Int64 = 0
        for i in bits.indices where bits[i] {
            let want = old.doneSizes[String(i)] ?? manifest.files[i].size
            if fileOnDisk(i) == want { kept += 1; bytes += want } else { bits[i] = false; dropped += 1; merged.doneSizes[String(i)] = nil }
        }
        merged.lastError = nil
        merged.lastErrorMessage = nil
        merged.filesDone = kept
        merged.extracted = bytes
        record = merged
        done = bits
        doneBytes = bytes
        keptFromEarlier = kept
        resuming = kept > 0 || old.offset > 0
        lock.lock(); extracted = bytes; filesDone = kept; lock.unlock()
        let what = "\(kept) file\(kept == 1 ? "" : "s") (\(formatBytes(bytes))) already unpacked"
        if dropped > 0 { log("\(dropped) file\(dropped == 1 ? "" : "s") unpacked earlier are missing or have the wrong size now; they are written again.") }
        let canOffset = (old.isZip || old.rarHead != nil) && old.offset > 0 && (old.sourceKind == "file" || old.serverSupportsRange != false)
        if canOffset && dropped == 0 {
            plan = .offset(old.offset)
            resumeNote = "Continued the interrupted install at byte \(old.offset)\(old.archiveBytes.map { " of \($0)" } ?? "") of the archive; \(what) were kept."
            log("Continuing the interrupted install: from archive byte \(old.offset)\(old.archiveBytes.map { " of \($0)" } ?? "") (entry \"\(old.nextEntry ?? "?")\"), \(what).")
        } else {
            plan = .skipDone
            merged.offset = 0
            merged.nextEntry = nil
            record = merged
            let why: String
            if dropped > 0 && canOffset { why = "some files unpacked earlier are missing, so the archive is read again from the start" }
            else if (old.isZip || old.rarHead != nil) && old.serverSupportsRange == false { why = "the server can't continue a download in the middle (no HTTP Range support), so the download starts again from the beginning" }
            else if !old.isZip && old.rarHead == nil && !old.formatName.isEmpty { why = "this kind of archive (\(old.formatName)) can't be continued in the middle, so it is read again from the start\(old.sourceKind == "url" ? " (the download starts again from the beginning)" : "")" }
            else { why = "the earlier install had not got far enough to save a resume point, so the archive is read again from the start" }
            resumeNote = "Continued the interrupted install, but \(why); \(what) were not written again."
            log("Continuing the interrupted install, but \(why). \(what.prefix(1).uppercased() + what.dropFirst()) and are not written again.")
        }
    }

    /// Called when the saved resume point turned out unusable part way into `extract`.
    private func startOver(after rejected: ResumeRejected) throws {
        guard let old = record else { throw InstallError(.corrupted, "The archive could not be read.", detail: rejected.reason) }
        if old.modifiesSource && (old.freedUpTo > 0 || old.sourceConsumed) {
            throw InstallError(.sourceUnreadable, "The interrupted install had already used up part of the archive (it is freed as it unpacks), and the rest no longer matches, so the install can't continue. Get the archive again and start over.", detail: rejected.reason)
        }
        var next = freshRecord()
        next.isZip = old.isZip
        next.formatName = old.formatName
        if rejected.keepDone { next.prefix = old.prefix }
        lock.lock(); streamBase = 0; lock.unlock()
        if rejected.keepDone {
            plan = .skipDone
            next.done = ResumeRecord.encodeBitmap(done)
            next.doneSizes = old.doneSizes
            next.filesDone = old.filesDone
            next.extracted = doneBytes
            next.etags = old.etags; next.lastModifieds = old.lastModifieds; next.partSizes = old.partSizes
            next.serverSupportsRange = old.serverSupportsRange
            resumeNote = "Continued the interrupted install, but \(rejected.reason); the archive is read again from the start and the \(keptFromEarlier) files already unpacked were not written again."
            log("Starting again from the beginning of the archive (\(rejected.reason)); \(keptFromEarlier) files already unpacked are kept and not written again.")
        } else {
            plan = .fresh
            done = Array(repeating: false, count: manifest.files.count)
            doneBytes = 0
            keptFromEarlier = 0
            lock.lock(); extracted = 0; filesDone = 0; lock.unlock()
            resumeNote = "The interrupted install could not be continued (\(rejected.reason)), so it started from the beginning."
            log("Starting from the beginning (\(rejected.reason)); every file is written again.")
        }
        record = next
        resuming = rejected.keepDone && resuming
        durableOffset = 0
        try prepareSource()
        saveRecord()
    }

    /// Saves the resume record (atomically). Unpacker thread.
    private func saveRecord(durable: Bool = false) {
        guard var r = record else { return }
        if let remote {
            // Only what this run learned; a request that failed (expired link...) must not erase the
            // sizes and validators the next link is checked against.
            let id = remote.identity
            for i in r.partSizes.indices {
                if let v = id.partLengths[safe: i] ?? nil { r.partSizes[i] = v }
                if let v = id.etags[safe: i] ?? nil { r.etags[i] = v }
                if let v = id.lastModifieds[safe: i] ?? nil { r.lastModifieds[i] = v }
            }
            if let range = id.rangeSupported { r.serverSupportsRange = range }
        } else if let local {
            r.partSizes = local.partSizes.map { $0 }
        }
        r.done = ResumeRecord.encodeBitmap(done)
        r.extracted = doneBytes
        r.filesDone = filesDoneNow
        record = r
        if !store.save(r, durable: durable) { log("Saving the resume point failed (\(String(cString: strerror(errno)))).") }
        lastSave = Date()
        fileCompletedSinceSave = false
    }

    /// Size of the written game file `i` (regular files only), or -1.
    private func fileOnDisk(_ i: Int) -> Int64 {
        var st = stat()
        guard lstat(dataFolder.appendingPathComponent(manifest.files[i].path).path, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG else { return -1 }
        return Int64(st.st_size)
    }

    // MARK: Sources

    private var startOffset: Int64 { if case .offset(let o) = plan { return o }; return 0 }

    private func prepareSource() throws {
        switch source {
        case .url(let url): try makeRemote([url])
        case .urls(let urls): try makeRemote(urls)
        case .file(let file): try makeLocal(resolveLocal(file))
        case .files(let files): try makeLocal(files)
        }
    }

    /// Multi-part sets: an earlier run may have deleted parts it had finished (allowModifyingSource),
    /// so when continuing, the parts are the ones the record lists, in the given file's folder.
    private func resolveLocal(_ file: URL) throws -> [URL] {
        if let record, record.sourceKind == "file", record.partNames.count > 1, record.partNames.contains(file.lastPathComponent) {
            let folder = file.deletingLastPathComponent()
            return record.partNames.map { folder.appendingPathComponent($0) }
        }
        return try ArchiveParts.resolve(file)
    }

    private func makeRemote(_ urls: [URL]) throws {
        guard !urls.isEmpty else { throw InstallError(.sourceUnreadable, "No download link was given.") }
        for url in urls where !["http", "https"].contains(url.scheme?.lowercased() ?? "") {
            throw InstallError(.httpError, "The link must start with http:// or https:// (got \"\(url.absoluteString)\").")
        }
        partCount = urls.count
        partNames = urls.map(\.lastPathComponent)
        var startPart = 0, offsetInPart = startOffset
        if case .offset(let offset) = plan, let record {
            // Global archive offset -> (part, offset in part).
            while startPart < urls.count - 1, let size = record.partSizes[safe: startPart] ?? nil, offsetInPart >= size {
                offsetInPart -= size; startPart += 1
            }
            if startPart < urls.count - 1, record.partSizes[safe: startPart] == nil {
                log("Part sizes unknown: can't find byte \(offset) in the parts; reading from the start instead.")
                plan = .skipDone; offsetInPart = 0; startPart = 0
            }
        }
        var expected: RemoteIdentity?
        if resuming, let record {
            expected = RemoteIdentity(partLengths: record.partSizes, etags: record.etags, lastModifieds: record.lastModifieds,
                                      rangeSupported: record.serverSupportsRange)
        }
        lock.lock(); streamBase = startOffset; lock.unlock()
        byteSource = try RemoteSource(urls: urls, workFolder: workFolder, options: options, startPart: startPart, startOffset: offsetInPart,
                                      expected: expected, resuming: resuming, log: { [weak self] in self?.log($0) })
    }

    private func makeLocal(_ parts: [URL]) throws {
        guard !parts.isEmpty else { throw InstallError(.sourceUnreadable, "No archive file was given.") }
        partCount = parts.count
        partNames = parts.map(\.lastPathComponent)
        if parts.count > 1 { log("Multi-part archive: \(partNames.joined(separator: ", "))") }
        let source = try LocalFileSource(parts: parts, modify: options.allowModifyingSource, startOffset: startOffset,
                                         knownSizes: record?.partSizes ?? [], log: { [weak self] in self?.log($0) })
        if startOffset == 0 {
            // An archive an earlier install already partly freed can't be read from the start.
            for part in parts {
                var st = stat()
                if stat(part.path, &st) == 0, Int64(st.st_size) > 64 << 20, Int64(st.st_blocks) * 512 < Int64(st.st_size) / 2 {
                    throw InstallError(.sourceUnreadable, "The archive \"\(part.lastPathComponent)\" was partly used up by an earlier install that stopped (the installer frees the archive as it unpacks it), so it can't be read from the start again. Get the archive again.",
                                       detail: "allocated \(Int64(st.st_blocks) * 512) of \(st.st_size) bytes")
                }
            }
        }
        if let record { durableOffset = max(startOffset, record.freedUpTo) }
        source.setPunchLimit(min(startOffset, durableOffset))
        byteSource = source
        lock.lock(); streamBase = startOffset; lock.unlock()
        startFootprint = source.allocatedBytes()
    }

    // MARK: Free space

    private func checkSpace() throws {
        set(phase: .checking)
        // Game files already there get replaced (or kept), so they need no extra room.
        var existing: Int64 = 0
        for file in manifest.files {
            var st = stat()
            if stat(dataFolder.appendingPathComponent(file.path).path, &st) == 0 { existing += Int64(st.st_blocks) * 512 }
        }
        let game = max(0, manifest.totalBytes - existing)
        var needed: Int64
        var how: String
        if byteSource is RemoteSource {
            needed = game + options.maxPendingBytes
            how = "the game (\(formatBytes(game))) plus up to \(formatBytes(options.maxPendingBytes)) of download buffer"
        } else if let local, local.modify, sameVolume(local.parts.first(where: { FileManager.default.fileExists(atPath: $0.path) }) ?? local.parts[0], destination) {
            // The archive is freed only up to the last saved resume point, so the file being
            // unpacked (at most the largest game file) can't be freed yet.
            let largest = manifest.files.map(\.size).max() ?? 0
            let archive = local.allocatedBytes()
            needed = max(0, game - archive) + (256 << 20) + largest
            how = "the game minus the archive it replaces as it unpacks (\(formatBytes(max(0, game - archive)))), plus room for the largest file (\(formatBytes(largest)))"
        } else {
            needed = game
            how = "the game (\(formatBytes(game))) next to the archive"
        }
        needed += options.spaceMargin
        let free = options.freeSpaceOverride ?? freeSpace(at: destination)
        log("Space check: need \(formatBytes(needed)) (\(how), plus \(formatBytes(options.spaceMargin)) to spare); free \(formatBytes(free))")
        if free < needed {
            throw InstallError(.notEnoughSpace, "Not enough free storage. Installing needs \(formatBytes(needed)) free (\(how), plus \(formatBytes(options.spaceMargin)) to spare), but only \(formatBytes(free)) is free. Free up \(formatBytes(needed - free)) more and try again.",
                               detail: "need \(needed), free \(free)")
        }
    }

    private func freeSpace(at url: URL) -> Int64 {
        let values = try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey, .volumeAvailableCapacityKey])
        if let important = values?.volumeAvailableCapacityForImportantUsage, important > 0 { return important }
        if let plain = values?.volumeAvailableCapacity { return Int64(plain) }
        var sfs = statfs()
        if statfs(url.path, &sfs) == 0 { return Int64(sfs.f_bavail) * Int64(sfs.f_bsize) }
        return 0
    }

    private func sameVolume(_ a: URL, _ b: URL) -> Bool {
        var sa = stat(), sb = stat()
        return stat(a.path, &sa) == 0 && stat(b.path, &sb) == 0 && sa.st_dev == sb.st_dev
    }

    // MARK: Unpacking

    private func extract() throws -> (prefix: String, skipped: Int, unsafe: [String], format: String) {
        guard let byteSource else { throw InstallError(.sourceUnreadable, "No archive.") }
        let box = SourceBox(byteSource)
        // A RAR5 restarted in the middle: its signature and main header come first (the reader identifies
        // the format as soon as it opens).
        lock.lock(); let restartAt = streamBase; lock.unlock()
        if restartAt > 0, let head = record?.rarHead, let bytes = Data(base64Encoded: head) { box.prefix = bytes as NSData }
        let unmanaged = Unmanaged.passRetained(box)
        defer { unmanaged.release() }
        guard let reader = gi_reader_open(readCallback, unmanaged.toOpaque()) else {
            throw InstallError(.writeFailed, "Out of memory.")
        }
        defer { gi_reader_close(reader) }

        lock.lock(); let base = streamBase; lock.unlock()
        var matcher = GameRootMatcher(manifest: manifest, prefix: record?.prefix)
        var skipped = 0
        var unsafe: [String] = []
        var first = true
        var format = ""
        var sawEncrypted = false
        let buffers = WriteState()
        while true {
            if isCancelled { throw InstallError(.cancelled, "The install was cancelled.") }
            var info = gi_entry_info()
            let r = gi_reader_next(reader, &info)
            if r == GI_EOF { break }
            let path = info.path.map { String(cString: $0) } ?? ""
            if first { format = gi_reader_format_name(reader).map { String(cString: $0) } ?? "" }
            if r == GI_FATAL || r == GI_FAILED {
                if let rejected = byteSource.resumeRejection { throw rejected }
                if first && base > 0 && byteSource.failure == nil && !isCancelled {
                    let why = gi_reader_error(reader).map { String(cString: $0) } ?? "unreadable"
                    throw ResumeRejected(reason: "the archive does not continue as expected at the saved byte \(base) (\(why))", keepDone: false)
                }
                if info.encrypted != 0 { throw passwordError() }
                throw archiveError(reader, box: box, file: nil, firstHeader: first)
            }
            if first {
                log("Archive format: \(format)")
                first = false
                zipMode = gi_reader_is_plain_zip(reader) != 0
                if base > 0 {
                    // The saved resume point must be exactly the header of the entry it names.
                    if let want = record?.nextEntry, want != path {
                        throw ResumeRejected(reason: "the archive has \"\(path)\" at the saved byte \(base), not \"\(want)\"", keepDone: false)
                    }
                    log("Resumed at archive byte \(base): the entry there is \"\(path)\", as saved.")
                }
                record?.formatName = format
                record?.isZip = zipMode
                // RAR5: restartable in the middle when it isn't solid or split into volumes, and the
                // archive isn't freed as it is read (allowModifyingSource).
                if base == 0 { record?.rarHead = local?.modify == true ? nil : Self.rar5RestartHead(box.head)?.base64EncodedString() }
                rarMode = record?.rarHead != nil
                if rarMode && base == 0 { log("RAR5 archive that can be continued in the middle if the install is interrupted.") }
                if !zipMode {
                    // No mid-archive resume for this format: free the archive as it is read, as always.
                    // (Then this archive can't be read again: the record says so.)
                    if let local, local.modify { record?.sourceConsumed = true; saveRecord(durable: true); local.setPunchLimit(.max) }
                }
            }
            if zipMode && (info.kind == Int32(GI_ENTRY_FILE) || info.kind == Int32(GI_ENTRY_DIRECTORY)) {
                noteHeader(path: path, base: base, box: box, reader: reader)
            } else if rarMode && (info.kind == Int32(GI_ENTRY_FILE) || info.kind == Int32(GI_ENTRY_DIRECTORY)) {
                noteRarHeader(path: path, base: base, box: box, reader: reader)
            }
            if info.encrypted != 0 { sawEncrypted = true }
            switch info.kind {
            case Int32(GI_ENTRY_DIRECTORY): continue
            case Int32(GI_ENTRY_FILE): break
            default:
                // Links and devices are never created.
                if case .file = matcher.match(path) { unsafe.append(path) } else { skipped += 1 }
                if info.kind == Int32(GI_ENTRY_SYMLINK) && unsafe.last != path { unsafe.append(path) }
                continue
            }
            switch matcher.match(path) {
            case .unsafe:
                unsafe.append(path)
                log("Refused unsafe path in archive: \(path)")
            case .notGame:
                skipped += 1
            case .file(let index):
                if record?.prefix == nil {
                    record?.prefix = matcher.prefixString
                    log("Game found in the archive at \"\(matcher.prefixString)\"")
                }
                if info.encrypted != 0 { throw passwordError() }
                if done[index] {
                    let want = record?.doneSizes[String(index)] ?? manifest.files[index].size
                    if fileOnDisk(index) == want { continue } // unpacked by an earlier run: not written again
                    // Changed or removed since: write it again.
                    done[index] = false
                    doneBytes -= want
                    lock.lock(); extracted -= want; filesDone -= 1; lock.unlock()
                }
                try write(entry: index, declaredSize: info.size_known != 0 ? info.size : -1, reader: reader, box: box, state: buffers)
            }
        }
        if first && base == 0 {
            // Not even one entry: empty or not an archive.
            throw archiveError(reader, box: box, file: nil, firstHeader: true)
        }
        if missedHeaderOffsets > 0 { log("Note: the exact offset of \(missedHeaderOffsets) zip headers could not be found; the resume point stayed at the previous entry for those.") }
        lock.lock(); let total = filesDone; lock.unlock()
        if total == 0 {
            if sawEncrypted || gi_reader_has_encrypted_entries(reader) > 0 { throw passwordError() }
            throw InstallError(.noGameFiles, "None of the files in this archive belong to the game. It is probably a different archive (or a different game version).", detail: "skipped \(skipped) entries")
        }
        log("Unpacked \(total) game files (\(keptFromEarlier) kept from an earlier run); skipped \(skipped) other entries; refused \(unsafe.count) unsafe entries")
        return (matcher.prefixString, skipped, unsafe, format)
    }

    /// A zip entry header was just read: everything before it is done, so its exact archive offset is
    /// the new resume point. Saved after each finished file and every few seconds; in
    /// allowModifyingSource mode, flushed to disk before the archive is freed up to it.
    private func noteHeader(path: String, base: Int64, box: SourceBox, reader: OpaquePointer) {
        guard let h = box.zipHeaderStart(endingAt: gi_reader_consumed(reader), notBefore: gi_reader_header_position(reader)) else {
            missedHeaderOffsets += 1
            return
        }
        record?.offset = base + h
        record?.nextEntry = path
        if fileCompletedSinceSave || Date().timeIntervalSince(lastSave) >= Self.saveEvery { saveRecord() }
        if let local, local.modify, base + h - durableOffset >= Self.durableEvery {
            record?.freedUpTo = base + h
            saveRecord(durable: true)
            durableOffset = base + h
            local.setPunchLimit(durableOffset)
        }
    }

    /// A RAR5 file header was just read. libarchive's header position is exactly where its block starts,
    /// and the read position exactly where it ends (checked against the format; the first entry reports 0
    /// and isn't a restart point anyway). In stream bytes, minus the restart prefix.
    private func noteRarHeader(path: String, base: Int64, box: SourceBox, reader: OpaquePointer) {
        let start = gi_reader_header_position(reader), end = gi_reader_consumed(reader)
        guard start > box.prefixLength, end > start, end - start < 0x10000 else {
            if start > 0 { missedHeaderOffsets += 1 }
            return
        }
        record?.offset = base + start - box.prefixLength
        record?.nextEntry = path
        if fileCompletedSinceSave || Date().timeIntervalSince(lastSave) >= Self.saveEvery { saveRecord() }
    }

    /// A RAR5's signature and main archive header, when the archive can be restarted at any file header:
    /// not solid (a file's data depends on the files before it), not a volume of a set, headers not encrypted.
    static func rar5RestartHead(_ head: Data) -> Data? {
        let b = [UInt8](head)
        guard b.count > 12, b[0..<8].elementsEqual([0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00]) else { return nil }
        func vint(_ at: inout Int) -> UInt64? {
            var value: UInt64 = 0, shift: UInt64 = 0
            while at < b.count, shift < 64 {
                let x = b[at]; at += 1
                value |= UInt64(x & 0x7F) << shift; shift += 7
                if x < 0x80 { return value }
            }
            return nil
        }
        var i = 8 + 4                                   // CRC32 of the main header
        guard let size = vint(&i) else { return nil }
        let end = i + Int(size)
        guard end <= b.count, let type = vint(&i), type == 1, let flags = vint(&i) else { return nil }   // 1 = main archive header
        if flags & 1 != 0 { guard vint(&i) != nil else { return nil } }   // extra area size
        if flags & 2 != 0 { return nil }                                  // a main header has no data area
        guard let archiveFlags = vint(&i) else { return nil }
        if archiveFlags & 0x0005 != 0 { return nil }    // 0x0001 volume, 0x0004 solid
        return Data(b[0..<end])
    }

    private final class WriteState {}

    private func write(entry index: Int, declaredSize: Int64, reader: OpaquePointer, box: SourceBox, state: WriteState) throws {
        let file = manifest.files[index]
        let target = dataFolder.appendingPathComponent(file.path)
        let folder = target.deletingLastPathComponent().path
        if !createdFolders.contains(folder) {
            do { try FileManager.default.createDirectory(atPath: folder, withIntermediateDirectories: true) } catch {
                throw diskError(errno, file: file.path)
            }
            createdFolders.insert(folder)
        }
        lock.lock(); currentFile = file.path; lock.unlock()
        let temp = target.path + ".gipart"
        let fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0o644)
        guard fd >= 0 else { throw diskError(errno, file: file.path) }
        var ok = false
        defer { if !ok { close(fd); unlink(temp) } }
        var end: Int64 = 0
        var written: Int64 = 0
        defer { if !ok { lock.lock(); extracted -= written; lock.unlock() } }
        while true {
            var buffer: UnsafeRawPointer?
            var size = 0
            var offset: Int64 = 0
            let r = gi_reader_data(reader, &buffer, &size, &offset)
            if r == GI_EOF { break }
            if r != GI_OK {
                if let rejected = box.source.resumeRejection { throw rejected }
                throw archiveError(reader, box: box, file: file.path, firstHeader: false)
            }
            if isCancelled { throw InstallError(.cancelled, "The install was cancelled.") }
            guard size > 0, var p = buffer else { continue }
            var left = size, at = offset
            while left > 0 {
                let n = pwrite(fd, p, left, off_t(at))
                if n < 0 { if errno == EINTR { continue }; throw diskError(errno, file: file.path) }
                p += n; left -= n; at += Int64(n)
            }
            end = max(end, offset + Int64(size))
            written += Int64(size)
            lock.lock(); extracted += Int64(size); lock.unlock()
            Self.testHook?("data", written, declaredSize >= 0 ? declaredSize : file.size)
        }
        // Sparse tails (tar) end in a hole: extend to the size the ARCHIVE declares (never to the
        // manifest size, which would hide a short or wrong file from verification).
        if declaredSize > end, ftruncate(fd, off_t(declaredSize)) != 0 { throw diskError(errno, file: file.path) }
        if close(fd) != 0 { ok = true; unlink(temp); lock.lock(); extracted -= written; lock.unlock(); throw diskError(errno, file: file.path) }
        ok = true
        if rename(temp, target.path) != 0 { let e = errno; unlink(temp); lock.lock(); extracted -= written; lock.unlock(); throw diskError(e, file: file.path) }
        let finalSize = max(end, declaredSize)
        lock.lock()
        filesDone += 1; currentFile = nil
        extracted += finalSize - written // a sparse tail counts as written
        lock.unlock()
        Self.testHook?("filedone", Int64(filesDoneNow), finalSize)
        done[index] = true
        doneBytes += finalSize
        if finalSize != file.size { record?.doneSizes[String(index)] = finalSize }
        fileCompletedSinceSave = true
        saveRecord()
    }

    // MARK: Errors

    private func passwordError() -> InstallError {
        InstallError(.passwordProtected, "The archive is password-protected. The installer can't open protected archives: unpack it on a computer with the password and re-pack it without one (zip or rar).")
    }

    private func diskError(_ code: Int32, file: String) -> InstallError {
        if code == ENOSPC || code == EDQUOT {
            return InstallError(.outOfSpace, "The device ran out of storage while unpacking \(file). Free up space and try again; files unpacked so far are kept.", files: [file], detail: String(cString: strerror(code)))
        }
        return InstallError(.writeFailed, "Writing \(file) failed: \(String(cString: strerror(code))).", files: [file], detail: String(cString: strerror(code)))
    }

    private func archiveError(_ reader: OpaquePointer, box: SourceBox, file: String?, firstHeader: Bool) -> InstallError {
        if isCancelled { return InstallError(.cancelled, "The install was cancelled.") }
        if let failure = box.source.failure { return failure }
        let message = gi_reader_error(reader).map { String(cString: $0) } ?? "unknown error"
        let code = gi_reader_errno(reader)
        let lower = message.lowercased()
        if code == ENOSPC { return diskError(ENOSPC, file: file ?? "the game") }
        if lower.contains("encrypt") || lower.contains("passphrase") || lower.contains("password") { return passwordError() }
        if firstHeader && lower.contains("unrecognized archive format") || (firstHeader && box.head.isEmpty) {
            let head = [UInt8](box.head)
            if head.starts(with: [0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C]) {
                return InstallError(.unsupported, "7-Zip (.7z) archives are not supported. Re-pack the game as a zip or rar.", detail: message)
            }
            if box.head.isEmpty {
                return InstallError(.notAnArchive, "The file is empty.", detail: message)
            }
            if let text = String(data: box.head, encoding: .utf8)?.lowercased(), text.contains("<!doctype") || text.contains("<html") {
                return InstallError(.webPageNotFile, "This is a web page, not the game archive.", detail: message)
            }
            return InstallError(.notAnArchive, "This file is not an archive the installer can open. Supported: zip, rar, rar5, tar.", detail: message)
        }
        let multi = partCount > 1 || partNames.first.map { $0.range(of: #"\.part\d+\.rar$|\.r\d\d$|\.z\d\d$|\.\d{3}$"#, options: [.regularExpression, .caseInsensitive]) != nil } ?? false
        if lower.contains("crc") || lower.contains("checksum") {
            if let file {
                return InstallError(.corrupted, "The archive is damaged: \(file) failed its integrity check (CRC error). Download the archive again.", files: [file], detail: message)
            }
            return InstallError(.corrupted, "The archive is damaged (an index entry failed its integrity check). Download the archive again.", detail: message)
        }
        if lower.contains("multi-volume") || lower.contains("volume") && !lower.contains("invalid") {
            return InstallError(.missingPart, "A part of the multi-part archive is missing or out of order. Make sure all parts are there and come from the same set.", detail: message)
        }
        if lower.contains("truncat") || lower.contains("premature") || lower.contains("end of") || lower.contains("eof") {
            if multi {
                return InstallError(.missingPart, "The archive ends after \"\(partNames.last ?? "the last part")\" but continues in another part. A part of the multi-part set is missing: add the next part and try again.", files: file.map { [$0] } ?? [], detail: message)
            }
            return InstallError(.truncated, "The archive ends too early\(file.map { " (in \($0))" } ?? ""): it is incomplete. The download probably didn't finish, or the file was cut short. Get the archive again.", files: file.map { [$0] } ?? [], detail: message)
        }
        if lower.contains("unsupported") || lower.contains("not supported") {
            return InstallError(.unsupported, "The archive uses a compression method the installer can't unpack (\(message)). Re-pack it as an ordinary zip or rar.", detail: message)
        }
        return InstallError(.corrupted, "The archive could not be read\(file.map { " at \($0)" } ?? ""): it seems damaged. Download it again.", files: file.map { [$0] } ?? [], detail: message)
    }

    // MARK: Verify

    private var toleratedMismatches: [String] = []

    private func verify() throws {
        var missing: [String] = [], wrongSize: [String] = []
        for file in manifest.files {
            var st = stat()
            if lstat(dataFolder.appendingPathComponent(file.path).path, &st) != 0 || (st.st_mode & S_IFMT) != S_IFREG { missing.append(file.path) }
            else if Int64(st.st_size) != file.size {
                if options.toleratedSizeMismatches.contains(file.path) {
                    toleratedMismatches.append(file.path)
                    log("size differs but tolerated: \(file.path) (\(st.st_size), manifest \(file.size))")
                } else { wrongSize.append(file.path) }
            }
        }
        log("Verified \(manifest.files.count) files: \(missing.count) missing, \(wrongSize.count) wrong size")
        guard missing.isEmpty && wrongSize.isEmpty else {
            var parts: [String] = []
            if !missing.isEmpty { parts.append("\(missing.count) game file\(missing.count == 1 ? " is" : "s are") missing (e.g. \(missing.prefix(3).joined(separator: ", ")))") }
            if !wrongSize.isEmpty { parts.append("\(wrongSize.count) game file\(wrongSize.count == 1 ? " has" : "s have") the wrong size (e.g. \(wrongSize.prefix(3).joined(separator: ", ")))") }
            for name in missing.prefix(50) { log("missing: \(name)") }
            for name in wrongSize.prefix(50) { log("wrong size: \(name)") }
            throw InstallError(.verificationFailed, "After unpacking, \(parts.joined(separator: " and ")). The archive is incomplete or holds a different game version.",
                               files: missing + wrongSize, detail: "missing \(missing.count), wrong size \(wrongSize.count)")
        }
    }

    private func writeManifestCopy() {
        let target = dataFolder.appendingPathComponent("manifest.json")
        try? FileManager.default.removeItem(at: target)
        try? FileManager.default.copyItem(at: manifestURL, to: target)
    }

    // MARK: Progress

    private func set(phase: GameInstaller.Phase) { lock.lock(); self.phase = phase; lock.unlock() }

    private func startTimer() {
        let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
        timer.schedule(deadline: .now() + options.progressInterval, repeating: options.progressInterval)
        timer.setEventHandler { [weak self] in self?.sampleDisk(); self?.emitProgress() }
        timer.resume()
        self.timer = timer
    }

    private func stopTimer() { timer?.cancel(); timer = nil }

    /// Disk use of the install now, compared with the start.
    private func sampleDisk() {
        lock.lock(); let written = extracted; lock.unlock()
        var footprint = written
        if let local, local.modify { footprint += local.allocatedBytes() }
        else if let local { footprint += local.totalBytes }
        if let remote { footprint += remote.pendingBytes }
        let increase = footprint - (local != nil ? startFootprint : 0)
        lock.lock(); peakIncrease = max(peakIncrease, increase); lock.unlock()
    }

    private func emitProgress() {
        let now = Date()
        let downloaded: Int64, total: Int64?, pending: Int64, retries: Int
        if let remote { let s = remote.stats; downloaded = s.reused + s.downloaded; total = s.total; pending = s.pending; retries = s.retries }
        else { downloaded = byteSource?.bytesConsumed ?? 0; total = local?.totalBytes; pending = 0; retries = 0 }
        lock.lock()
        let base = streamBase
        samples.append((now, extracted))
        while let first = samples.first, now.timeIntervalSince(first.0) > 10 { samples.removeFirst() }
        var rate: Double?
        if let first = samples.first, let last = samples.last, last.0.timeIntervalSince(first.0) >= 2 {
            rate = Double(last.1 - first.1) / last.0.timeIntervalSince(first.0)
        }
        let progress = GameInstaller.Progress(
            phase: phase, downloadedBytes: base + downloaded, downloadTotalBytes: total, extractedBytes: extracted,
            totalBytes: manifest?.totalBytes ?? 0, filesDone: filesDone, filesTotal: manifest?.files.count ?? 0,
            currentFile: currentFile, bytesPerSecond: rate,
            secondsRemaining: rate.flatMap { $0 > 1000 ? Double(max(0, (manifest?.totalBytes ?? 0) - extracted)) / $0 : nil },
            pendingBytes: pending, retries: retries, resumedFromByte: base)
        lock.unlock()
        let handler = progressHandler
        options.callbackQueue.async { handler(progress) }
    }
}
