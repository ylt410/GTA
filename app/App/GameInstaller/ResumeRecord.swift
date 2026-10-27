import Foundation

/// What an interrupted install left behind, as the app sees it (for a "Continue download (37%)" button).
public struct ResumableInstall {
    /// URL installs: the link(s) the install used last. Empty for local files.
    /// (Cookies and other HTTP headers are never stored: pass them again when continuing.)
    public var urls: [URL]
    /// Local-file installs: the archive's file name(s). Empty for URL installs.
    public var fileNames: [String]
    /// The archive's total size, when it was known.
    public var archiveBytes: Int64?
    /// Archive bytes that will NOT be read or downloaded again (0 when the download restarts).
    public var resumeOffset: Int64
    /// True when the install can pick up the download in the middle of the archive (zip on a
    /// server with HTTP Range, or a local zip). False for rar/rar5/tar and servers without Range:
    /// then the archive is read again from the start, but files already unpacked are not written again.
    public var continuesDownload: Bool
    /// Game bytes and files already unpacked and checked.
    public var extractedBytes: Int64
    public var totalBytes: Int64
    public var filesDone: Int
    public var filesTotal: Int
    /// Share of the game already unpacked (0...1). This is what the progress bar showed.
    public var fraction: Double { totalBytes > 0 ? min(1, Double(extractedBytes) / Double(totalBytes)) : 0 }
    /// Why the last attempt stopped (e.g. `.linkExpired`, `.networkLost`, `.cancelled`), if it said so.
    /// nil when the app was closed or killed while installing.
    public var lastError: InstallError.Kind?
    public var lastErrorMessage: String?
    /// libarchive's name for the archive format, e.g. "ZIP 2.0 (deflation)".
    public var format: String
    public var savedAt: Date
}

extension GameInstaller {
    /// The interrupted install at `destination`, if there is one with something to continue
    /// (a file unpacked or a resume point past the start). Cheap: reads one small JSON file.
    public static func resumableInstall(at destination: URL) -> ResumableInstall? {
        guard let record = ResumeStore(destination: destination).load(),
              record.filesDone > 0 || record.offset > 0 else { return nil }
        return ResumableInstall(
            urls: (record.urls ?? []).compactMap(URL.init(string:)),
            fileNames: record.sourceKind == "file" ? record.partNames : [],
            archiveBytes: record.archiveBytes,
            resumeOffset: record.canContinueDownload ? record.offset : 0,
            continuesDownload: record.canContinueDownload,
            extractedBytes: record.extracted, totalBytes: record.manifestBytes,
            filesDone: record.filesDone, filesTotal: record.manifestFiles,
            lastError: record.lastError.flatMap(InstallError.Kind.init(rawValue:)),
            lastErrorMessage: record.lastErrorMessage,
            format: record.formatName, savedAt: Date(timeIntervalSince1970: record.savedAt))
    }

    /// Forgets the interrupted install at `destination` (the next install starts from the beginning
    /// and rewrites every file). Game files already unpacked are left in place.
    public static func discardResumableInstall(at destination: URL) {
        ResumeStore(destination: destination).remove()
        let work = destination.appendingPathComponent(".gameinstaller", isDirectory: true)
        try? FileManager.default.removeItem(at: work)
    }
}

/// On-disk resume record: `<destination>/data/.install-resume.json`.
struct ResumeRecord: Codable {
    var version = 1
    /// "url" or "file".
    var sourceKind: String
    var urls: [String]?
    var partNames: [String]
    /// Per part: total length (Content-Length / Content-Range total, or file size) when known.
    var partSizes: [Int64?]
    /// Local files: modification times when the install started (not checked when the source is modified).
    var partMtimes: [Double?]
    var etags: [String?]
    var lastModifieds: [String?]
    /// URL: whether the server answered a Range request with 206. nil = not known yet.
    var serverSupportsRange: Bool?
    var modifiesSource: Bool
    /// libarchive's format name; "" until the first header was read.
    var formatName: String = ""
    /// True when the archive is a plain zip: the install can restart at `offset`.
    var isZip = false
    /// A non-solid, single-volume RAR5 (the usual game archive): its first bytes (signature and main
    /// archive header, base64). Each file in it starts with a self-contained header, so the install can
    /// restart at `offset` too: these bytes are handed to the unpacker first, then the archive from
    /// `offset`. nil for every other archive.
    var rarHead: String?
    /// Folder inside the archive that holds the game (nil = not found yet).
    var prefix: String?
    /// Archive offset of the local header of the first entry that is not yet fully written.
    var offset: Int64 = 0
    /// Path of the entry at `offset` (checked when resuming), nil when unknown.
    var nextEntry: String?
    /// Local + allowModifyingSource: archive bytes before this offset may have been freed.
    var freedUpTo: Int64 = 0
    /// Non-zip + allowModifyingSource: the archive was (partly) freed, so it can't be read again.
    var sourceConsumed = false
    var manifestFiles: Int
    var manifestBytes: Int64
    /// Bitmap (base64) of manifest indexes fully written.
    var done: String = ""
    /// Written sizes that differ from the manifest (tolerated mismatches), by manifest index.
    var doneSizes: [String: Int64] = [:]
    var extracted: Int64 = 0
    var filesDone = 0
    var skippedEntries = 0
    var savedAt: Double = 0
    var lastError: String?
    var lastErrorMessage: String?

    var archiveBytes: Int64? {
        partSizes.allSatisfy { $0 != nil } ? partSizes.reduce(0) { $0 + ($1 ?? 0) } : nil
    }
    var canContinueDownload: Bool {
        (isZip || rarHead != nil) && offset > 0 && (sourceKind == "file" || serverSupportsRange != false)
    }

    static func encodeBitmap(_ bits: [Bool]) -> String {
        var bytes = [UInt8](repeating: 0, count: (bits.count + 7) / 8)
        for (i, b) in bits.enumerated() where b { bytes[i >> 3] |= 1 << UInt8(i & 7) }
        return Data(bytes).base64EncodedString()
    }
    static func decodeBitmap(_ text: String, count: Int) -> [Bool] {
        let bytes = [UInt8](Data(base64Encoded: text) ?? Data())
        return (0..<count).map { i in (i >> 3) < bytes.count && bytes[i >> 3] & (1 << UInt8(i & 7)) != 0 }
    }
}

struct ResumeStore {
    let url: URL
    init(destination: URL) {
        url = destination.appendingPathComponent("data", isDirectory: true).appendingPathComponent(".install-resume.json")
    }

    func load() -> ResumeRecord? {
        guard let data = try? Data(contentsOf: url),
              let record = try? JSONDecoder().decode(ResumeRecord.self, from: data), record.version == 1 else { return nil }
        return record
    }

    /// Atomic: written to a temporary file and renamed over the old one, so a kill at any moment
    /// leaves either the old or the new record. `durable` also flushes it to the disk first
    /// (needed before freeing archive bytes the record says are no longer needed).
    @discardableResult
    func save(_ record: ResumeRecord, durable: Bool) -> Bool {
        var record = record
        record.savedAt = Date().timeIntervalSince1970
        guard let data = try? JSONEncoder().encode(record) else { return false }
        let temp = url.path + ".tmp"
        let fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0o644)
        guard fd >= 0 else { return false }
        let ok: Bool = data.withUnsafeBytes { raw in
            var p = raw.baseAddress!, left = raw.count
            while left > 0 {
                let n = write(fd, p, left)
                if n < 0 { if errno == EINTR { continue }; return false }
                p += n; left -= n
            }
            return true
        }
        if ok && durable { _ = fsync(fd) }
        close(fd)
        guard ok, rename(temp, url.path) == 0 else { unlink(temp); return false }
        return true
    }

    func remove() {
        unlink(url.path)
        unlink(url.path + ".tmp")
    }
}

/// Thrown inside the job when a resume can't go on (the job then starts over).
struct ResumeRejected: Error {
    let reason: String
    /// true: the archive is the same, only the place to start differs; files already unpacked are kept
    /// and not written again. false: the archive changed; everything is written again.
    let keepDone: Bool
}
