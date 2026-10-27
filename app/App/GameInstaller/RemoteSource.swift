import Foundation

/// What identifies the remote archive across runs: sizes and HTTP validators per part.
struct RemoteIdentity {
    var partLengths: [Int64?]
    var etags: [String?]
    var lastModifieds: [String?]
    /// Whether a Range request was answered with 206 (nil = not seen yet).
    var rangeSupported: Bool?
}

/// Downloads the archive into small rolling chunk files and hands them to libarchive.
/// At most `maxPendingBytes` of downloaded-but-unread data sit on disk; a chunk file is
/// deleted as soon as it was read. A dropped connection is resumed with an HTTP Range request
/// from the exact byte where it stopped; meanwhile the unpacker simply waits for more bytes.
final class RemoteSource: NSObject, ArchiveByteSource, URLSessionDataDelegate, @unchecked Sendable {
    private let urls: [URL]
    private let chunkFolder: URL
    private let options: GameInstaller.Options
    private let log: (String) -> Void
    private let readSize = 4 << 20
    private let buffers: [UnsafeMutableRawPointer]
    private var flip = 0
    /// Resume: the identity recorded by the interrupted run (checked against the first answer).
    private let expected: RemoteIdentity?
    /// True when this run continues an earlier one: then 401/403/404/410 or a web page mean
    /// "the link expired", not "wrong link".
    private let resuming: Bool

    private let cond = NSCondition()
    // --- guarded by cond ---
    /// Chunk files ready for the reader, in order. `skip`: bytes at the start of the file that are
    /// before the resume point (only for a chunk reused from an interrupted run).
    private var sealed: [(url: URL, size: Int64, skip: Int64)] = []
    /// Bytes reused from chunk files an interrupted run had downloaded (not downloaded again).
    private(set) var reused: Int64 = 0
    private var pending: Int64 = 0
    private(set) var peakPending: Int64 = 0
    private var downloadDone = false
    private var cancelled = false
    private var stopError: InstallError?
    private var consumed: Int64 = 0
    private(set) var downloaded: Int64 = 0
    private(set) var retries = 0
    private(set) var contentTotal: Int64?
    private var identityStore: RemoteIdentity
    private var rejection: ResumeRejected?
    // --------------------------

    // Download state (delegate queue only).
    private let queue: OperationQueue
    private var session: URLSession!
    private var task: URLSessionDataTask?
    private var partIndex = 0
    private var partOffset: Int64 = 0
    private var partLength: Int64?
    private var partLengths: [Int64?]
    private var validator: (etag: String?, lastModified: String?)?
    private var discardBytes: Int64 = 0
    private var sniffed = false
    private var chunkFD: Int32 = -1
    private var chunkURL: URL?
    private var chunkBytes: Int64 = 0
    private var chunkStart: Int64 = 0
    /// Resume: chunk files of the interrupted run that continue the archive at `adoptAt` (after the
    /// gap between the resume point and the first of them is downloaded again).
    private var adoptRun: [(url: URL, size: Int64)] = []
    private var adoptAt: Int64?
    private var adoptEnd: Int64 = 0
    private var waitingForRoom = false
    private var streamingFallback = false
    private var requestedEnd: Int64?
    private var lastProgress = Date()
    private var attempt = 0
    private var responseFailure: InstallError?
    private var verifyPending: Bool
    /// A resumed install's first request got 200: decide on the first bytes (an expired portal link
    /// may send a web page as a "file"), then apply this verdict (nil = carry on).
    private var pendingVerdict: ResumeRejected?
    private var verdictPending = false

    // Reading state (unpacker thread only).
    private var readFD: Int32 = -1
    private var readChunk: (url: URL, size: Int64, skip: Int64)?

    /// `startPart`/`startOffset`: where to begin (resume). `expected`: the archive identity saved by
    /// the interrupted run; when it doesn't match the server's first answer, the source stops with
    /// `resumeRejection` set (and the installer starts over).
    init(urls: [URL], workFolder: URL, options: GameInstaller.Options, startPart: Int = 0, startOffset: Int64 = 0,
         expected: RemoteIdentity? = nil, resuming: Bool = false, log: @escaping (String) -> Void) throws {
        self.urls = urls
        self.options = options
        self.log = log
        self.expected = expected
        self.resuming = resuming
        verifyPending = expected != nil
        chunkFolder = workFolder.appendingPathComponent("chunks", isDirectory: true)
        let keepChunks = startOffset > 0 || startPart > 0
        if !keepChunks { try? FileManager.default.removeItem(at: chunkFolder) }
        do { try FileManager.default.createDirectory(at: chunkFolder, withIntermediateDirectories: true) } catch {
            throw InstallError(.writeFailed, "The download folder could not be created.", detail: error.localizedDescription)
        }
        partLengths = Array(repeating: nil, count: urls.count)
        identityStore = RemoteIdentity(partLengths: partLengths, etags: partLengths.map { _ in nil },
                                       lastModifieds: partLengths.map { _ in nil }, rangeSupported: nil)
        if let expected, startPart > 0 {
            // Parts before the resume point are not requested again: take their sizes from the record.
            for i in 0..<min(startPart, urls.count) where i < expected.partLengths.count {
                partLengths[i] = expected.partLengths[i]
                identityStore.partLengths[i] = expected.partLengths[i]
                identityStore.etags[i] = expected.etags[safe: i] ?? nil
                identityStore.lastModifieds[i] = expected.lastModifieds[safe: i] ?? nil
            }
        }
        partIndex = min(startPart, max(0, urls.count - 1))
        partOffset = startOffset
        if startOffset > 0, let expected {
            // Lets the server answer If-Range with the whole (new) file if it changed.
            validator = (expected.etags[safe: partIndex] ?? nil, expected.lastModifieds[safe: partIndex] ?? nil)
        }
        buffers = [UnsafeMutableRawPointer.allocate(byteCount: readSize, alignment: 16),
                   UnsafeMutableRawPointer.allocate(byteCount: readSize, alignment: 16)]
        queue = OperationQueue()
        queue.maxConcurrentOperationCount = 1
        queue.name = "GameInstaller.download"
        super.init()
        if keepChunks { adoptChunks(part: partIndex, offset: startOffset) }
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = options.requestTimeout
        config.timeoutIntervalForResource = 7 * 24 * 3600
        // Not ...AndRemote...: that adds "Cache-Control: no-cache", which browsers don't send.
        config.requestCachePolicy = .reloadIgnoringLocalCacheData
        config.urlCache = nil
        config.httpMaximumConnectionsPerHost = 1
        config.waitsForConnectivity = false
        if !options.httpCookies.isEmpty {
            config.httpShouldSetCookies = true
            config.httpCookieAcceptPolicy = .always
            for cookie in options.httpCookies { config.httpCookieStorage?.setCookie(cookie) }
        }
        streamingFallback = options.singleRequest
        session = URLSession(configuration: config, delegate: self, delegateQueue: queue)
    }

    deinit {
        buffers.forEach { $0.deallocate() }
        if readFD >= 0 { close(readFD) }
    }

    func start() { queue.addOperation { self.startRequest() } }

    /// Resume: chunk files the interrupted run downloaded (and the reader had not used up yet)
    /// survive a kill. Keep the run of contiguous chunks that covers or follows the resume point;
    /// delete the rest. Chunk names are "c-<part>-<offset in part>" ("....part" while being written).
    private func adoptChunks(part: Int, offset: Int64) {
        let fm = FileManager.default
        var found: [(url: URL, off: Int64, size: Int64)] = []
        for name in (try? fm.contentsOfDirectory(atPath: chunkFolder.path)) ?? [] {
            let url = chunkFolder.appendingPathComponent(name)
            let f = name.split(separator: "-")
            var st = stat()
            if f.count == 3, f[0] == "c", Int(f[1]) == part, let off = Int64(f[2]), stat(url.path, &st) == 0, st.st_size > 0 {
                found.append((url, off, Int64(st.st_size)))
            } else {
                unlink(url.path) // other parts, half-written chunks, anything else
            }
        }
        found.sort { $0.off < $1.off }
        var run: [(url: URL, off: Int64, size: Int64)] = []
        for c in found {
            if run.isEmpty {
                if c.off + c.size > offset { run.append(c) }
            } else if c.off == run.last!.off + run.last!.size {
                run.append(c)
            }
        }
        let keep = Set(run.map(\.url))
        for c in found where !keep.contains(c.url) { unlink(c.url.path) }
        guard let first = run.first, let last = run.last else { return }
        let end = last.off + last.size
        if first.off <= offset {
            // The chunks continue the archive right at the resume point.
            let skip = offset - first.off
            sealed = run.enumerated().map { (i, c) in (c.url, c.size, i == 0 ? skip : 0) }
            pending = run.reduce(0) { $0 + $1.size }
            peakPending = pending
            reused = end - offset
            partOffset = end
            log("Reusing \(formatBytes(end - offset)) the interrupted run had already downloaded (archive bytes \(offset)-\(end - 1)); downloading continues at byte \(end).")
        } else {
            // The reader had already used up the chunk with the resume point: download the gap again,
            // then use the chunks.
            adoptRun = run.map { ($0.url, $0.size) }
            adoptAt = first.off
            adoptEnd = end
            log("Will reuse \(formatBytes(end - first.off)) the interrupted run had already downloaded (archive bytes \(first.off)-\(end - 1)) after downloading bytes \(offset)-\(first.off - 1) again.")
        }
    }

    /// The gap before the reusable chunks is downloaded: queue them for the reader.
    private func adoptNow() {
        seal()
        cond.lock()
        for c in adoptRun { sealed.append((c.url, c.size, 0)); pending += c.size }
        reused += adoptEnd - (adoptAt ?? adoptEnd)
        peakPending = max(peakPending, pending)
        cond.broadcast()
        cond.unlock()
        log("Gap downloaded; reusing the interrupted run's chunks, downloading continues at byte \(adoptEnd).")
        partOffset = adoptEnd
        adoptRun = []
        adoptAt = nil
    }

    var bytesConsumed: Int64 { cond.lock(); defer { cond.unlock() }; return consumed }
    var pendingBytes: Int64 { cond.lock(); defer { cond.unlock() }; return pending }
    var stats: (downloaded: Int64, total: Int64?, pending: Int64, peak: Int64, retries: Int, reused: Int64) {
        cond.lock(); defer { cond.unlock() }
        return (downloaded, contentTotal, pending, peakPending, retries, reused)
    }
    var failure: InstallError? { cond.lock(); defer { cond.unlock() }; return rejection == nil ? stopError : nil }
    var resumeRejection: ResumeRejected? { cond.lock(); defer { cond.unlock() }; return rejection }
    /// Sizes and validators seen so far (saved in the resume record).
    var identity: RemoteIdentity { cond.lock(); defer { cond.unlock() }; return identityStore }

    func cancel() {
        cond.lock()
        cancelled = true
        if stopError == nil { stopError = InstallError(.cancelled, "The install was cancelled.") }
        cond.broadcast()
        cond.unlock()
        queue.addOperation { self.task?.cancel(); self.session.invalidateAndCancel() }
    }

    /// Leave the downloaded chunk files in place at finish (the next run continues and reuses them).
    var keepChunks = false

    func finish(success: Bool) {
        queue.addOperation {
            self.task?.cancel()
            self.session.invalidateAndCancel()
            if self.chunkFD >= 0 { close(self.chunkFD); self.chunkFD = -1 }
        }
        queue.waitUntilAllOperationsAreFinished()
        if readFD >= 0 { close(readFD); readFD = -1 }
        if !keepChunks { try? FileManager.default.removeItem(at: chunkFolder) }
    }

    // MARK: Reading (unpacker thread)

    func nextBlock() -> (UnsafeRawPointer, Int)? {
        flip ^= 1
        let buffer = buffers[flip]
        while true {
            if readFD >= 0 {
                let n = read(readFD, buffer, readSize)
                if n > 0 {
                    cond.lock(); consumed += Int64(n); cond.unlock()
                    return (UnsafeRawPointer(buffer), n)
                }
                if n < 0 {
                    fail(InstallError(.writeFailed, "Reading a downloaded chunk failed.", detail: String(cString: strerror(errno))))
                    return nil
                }
                // Chunk used up: delete it and make room for more downloading.
                close(readFD); readFD = -1
                if let chunk = readChunk { unlink(chunk.url.path) }
                cond.lock()
                pending -= readChunk?.size ?? 0
                readChunk = nil
                cond.unlock()
                queue.addOperation { self.resumeIfRoom() }
            }
            cond.lock()
            while sealed.isEmpty && !downloadDone && stopError == nil { cond.wait() }
            if let error = stopError { cond.unlock(); _ = error; return nil }
            if sealed.isEmpty && downloadDone { cond.unlock(); return (UnsafeRawPointer(buffer), 0) }
            let next = sealed.removeFirst()
            cond.unlock()
            readFD = open(next.url.path, O_RDONLY)
            readChunk = next
            if readFD < 0 || (next.skip > 0 && lseek(readFD, off_t(next.skip), SEEK_SET) != off_t(next.skip)) {
                fail(InstallError(.writeFailed, "A downloaded chunk could not be opened.", detail: String(cString: strerror(errno))))
                return nil
            }
        }
    }

    private func fail(_ error: InstallError) {
        cond.lock()
        if stopError == nil { stopError = error }
        cond.broadcast()
        cond.unlock()
        queue.addOperation { self.task?.cancel() }
    }

    // MARK: Downloading (delegate queue)

    private var isStopped: Bool { cond.lock(); defer { cond.unlock() }; return cancelled || stopError != nil }

    /// Bytes asked for per request. The next window is only requested when it fits under
    /// maxPendingBytes, which is what keeps the disk bound strict (URLSessionDataTask.suspend()
    /// does not reliably stop data from arriving).
    private var windowSize: Int64 { max(options.chunkSize, min(64 << 20, options.maxPendingBytes / 2)) }

    private func startRequest() {
        task = nil
        guard !isStopped else { return }
        if let at = adoptAt, partOffset >= at {
            adoptNow()
            if let length = partLength, partOffset >= length { partFinished(); return }
        }
        var request = URLRequest(url: urls[partIndex])
        for (k, v) in options.httpHeaders { request.setValue(v, forHTTPHeaderField: k) }
        let first = partIndex == 0 && partOffset == 0
        if first && options.httpMethod != "GET" {
            request.httpMethod = options.httpMethod
            request.httpBody = options.httpBody
        } else {
            request.setValue(nil, forHTTPHeaderField: "Content-Type")
            request.setValue(nil, forHTTPHeaderField: "Origin")
        }
        // A browser's first request asks for compression like Safari; resumes need the exact bytes.
        if !(options.singleRequest && first) { request.setValue("identity", forHTTPHeaderField: "Accept-Encoding") }
        if streamingFallback {
            requestedEnd = nil
            if partOffset > 0 { request.setValue("bytes=\(partOffset)-", forHTTPHeaderField: "Range") }
        } else {
            cond.lock(); let room = options.maxPendingBytes - pending; cond.unlock()
            var want = min(windowSize, room)
            if let length = partLength { want = min(want, length - partOffset) }
            if let at = adoptAt { want = min(want, at - partOffset) } // the gap only
            // Wait for the unpacker to make room (it calls resumeIfRoom after each chunk), unless
            // what is left to ask for (rest of the part, or of the gap) is smaller than that.
            var smallest = windowSize / 4
            if let length = partLength { smallest = min(smallest, length - partOffset) }
            if let at = adoptAt { smallest = min(smallest, at - partOffset) }
            if want < smallest || want <= 0 {
                waitingForRoom = true
                return
            }
            requestedEnd = partOffset + want
            request.setValue("bytes=\(partOffset)-\(partOffset + want - 1)", forHTTPHeaderField: "Range")
        }
        if partOffset > 0 {
            if let v = validator?.etag, !v.hasPrefix("W/") { request.setValue(v, forHTTPHeaderField: "If-Range") }
            else if let v = validator?.lastModified { request.setValue(v, forHTTPHeaderField: "If-Range") }
        }
        discardBytes = 0
        responseFailure = nil
        let task = session.dataTask(with: request)
        self.task = task
        task.resume()
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        guard dataTask === task else { completionHandler(.cancel); return }
        guard let http = response as? HTTPURLResponse else {
            responseFailure = InstallError(.httpError, "The link did not answer like a web server.", detail: response.url?.absoluteString)
            completionHandler(.cancel); return
        }
        let status = http.statusCode
        let type = (http.value(forHTTPHeaderField: "Content-Type") ?? "").lowercased()
        // Part way through (or continuing an earlier run), these mean the link stopped working.
        let continuing = partOffset > 0 || resuming || partIndex > 0
        if continuing, [401, 403, 404, 410].contains(status) {
            responseFailure = linkExpiredError(detail: "HTTP \(status) at byte \(partOffset)", status: status)
            completionHandler(.cancel); return
        }
        if status == 200 || status == 206, type.hasPrefix("text/html") || type.hasPrefix("application/xhtml") {
            responseFailure = continuing ? linkExpiredError(detail: "Content-Type: \(type) at byte \(partOffset)", status: nil)
                                         : webPageError(detail: "Content-Type: \(type)")
            completionHandler(.cancel); return
        }
        if verifyPending, status == 200 || status == 206 {
            // First answer of a resumed install: is it still the same file?
            verifyPending = false
            let etag = http.value(forHTTPHeaderField: "ETag"), modified = http.value(forHTTPHeaderField: "Last-Modified")
            var total: Int64?
            if status == 206 { total = (http.value(forHTTPHeaderField: "Content-Range") ?? "").split(separator: "/").last.flatMap { Int64($0) } }
            else if http.expectedContentLength > 0 { total = http.expectedContentLength }
            let want = expected?.partLengths[safe: partIndex] ?? nil
            let wantTag = expected?.etags[safe: partIndex] ?? nil, wantModified = expected?.lastModifieds[safe: partIndex] ?? nil
            var changed: String?
            if let want, let total, want != total { changed = "size is now \(total) bytes, was \(want)" }
            else if let wantTag, let etag, wantTag != etag { changed = "ETag is now \(etag), was \(wantTag)" }
            else if wantTag == nil || etag == nil, let wantModified, let modified, wantModified != modified { changed = "Last-Modified is now \(modified), was \(wantModified)" }
            else if want != nil && total == nil { changed = "the server no longer says how big the file is" }
            let changedVerdict = changed.map { ResumeRejected(reason: "the file on the server changed (\($0))", keepDone: false) }
            if status == 200 {
                // Look at the first bytes before deciding: an expired portal link may answer with a
                // web page sent as a "file", which must be reported as an expired link.
                verdictPending = true
                pendingVerdict = changedVerdict
                if pendingVerdict == nil && partOffset > 0 {
                    // Same file, but the server ignores Range.
                    pendingVerdict = ResumeRejected(reason: "the server ignores HTTP Range requests, so the download can't continue in the middle", keepDone: true)
                    cond.lock(); identityStore.rangeSupported = false; cond.unlock()
                }
                if pendingVerdict != nil { completionHandler(.allow); return }
            } else if let changedVerdict {
                reject(changedVerdict)
                completionHandler(.cancel); return
            }
            log("Resume check passed: same size\(etag != nil && wantTag != nil ? " and ETag" : modified != nil && wantModified != nil ? " and Last-Modified" : "") as before")
        }
        switch status {
        case 200:
            if partOffset == 0 && !streamingFallback {
                // Asked for a window, got the whole file: the server does not do Range.
                streamingFallback = true
                requestedEnd = nil
                cond.lock(); identityStore.rangeSupported = false; cond.unlock()
                log("The server does not support resuming (HTTP Range); streaming the whole file. Downloaded data is held back by pausing reads instead of by request size.")
            }
            if partOffset > 0 {
                if validator != nil && (http.value(forHTTPHeaderField: "ETag") != validator?.etag || http.value(forHTTPHeaderField: "Last-Modified") != validator?.lastModified) {
                    responseFailure = InstallError(.fileChangedOnServer, "The file on the server changed while it was downloading. Start the install again.", detail: "HTTP 200 to a resume request")
                    completionHandler(.cancel); return
                }
                // The server ignores Range: skip what we already have.
                discardBytes = partOffset
                log("Server ignored the resume request; skipping \(partOffset) bytes already received.")
            }
            let encoding = (http.value(forHTTPHeaderField: "Content-Encoding") ?? "identity").lowercased()
            if encoding == "identity" { noteLength(http.expectedContentLength > 0 ? http.expectedContentLength : nil) }
            else { log("The server compresses the download (\(encoding)); its size is known once it ends.") }
        case 206:
            let range = http.value(forHTTPHeaderField: "Content-Range") ?? ""
            // "bytes start-end/total"
            let numbers = range.split(whereSeparator: { !$0.isNumber }).compactMap { Int64($0) }
            guard numbers.count >= 2, numbers[0] == partOffset else {
                responseFailure = InstallError(.httpError, "The server sent the wrong part of the file when resuming.", detail: "Content-Range: \(range), wanted \(partOffset)")
                completionHandler(.cancel); return
            }
            if numbers.count >= 3 { noteLength(numbers[2]) }
            cond.lock(); identityStore.rangeSupported = true; cond.unlock()
        case 416:
            // Asked past the end: fine when we already have the whole part.
            let total = (http.value(forHTTPHeaderField: "Content-Range") ?? "").split(separator: "/").last.flatMap { Int64($0) }
            if verifyPending, let want = expected?.partLengths[safe: partIndex] ?? nil, let total, want != total {
                reject(ResumeRejected(reason: "the file on the server changed (size is now \(total) bytes, was \(want))", keepDone: false))
                completionHandler(.cancel); return
            }
            if let total { noteLength(total) }
            guard let length = partLength, partOffset >= length else {
                responseFailure = httpError(status, retryAfter: nil)
                completionHandler(.cancel); return
            }
        default:
            let server = ["Server", "CF-RAY", "CF-Mitigated", "Content-Type", "Content-Length"].compactMap { name in
                http.value(forHTTPHeaderField: name).map { "\(name): \($0)" } }.joined(separator: ", ")
            log("The server answered HTTP \(status) (\(server)) for \(http.url?.host ?? "?")\(partOffset == 0 ? ", before any data" : " at byte \(partOffset)").")
            responseFailure = httpError(status, retryAfter: http.value(forHTTPHeaderField: "Retry-After"))
            if partIndex == 0 && partOffset == 0, var detail = responseFailure?.detail {
                detail += ", first request"   // GameInstallRun may hand such a link to the browser
                responseFailure = InstallError(.httpError, responseFailure!.message, detail: detail)
            }
            completionHandler(.cancel); return
        }
        if validator == nil || partOffset == 0 {
            validator = (http.value(forHTTPHeaderField: "ETag"), http.value(forHTTPHeaderField: "Last-Modified"))
            cond.lock()
            identityStore.etags[partIndex] = validator?.etag
            identityStore.lastModifieds[partIndex] = validator?.lastModified
            cond.unlock()
        }
        if status == 416 { requestedEnd = nil }
        attempt = 0
        completionHandler(.allow)
    }

    private func noteLength(_ length: Int64?) {
        guard let length else { return }
        partLength = length // 200: Content-Length of the whole file; 206: the total after "/"
        partLengths[partIndex] = length
        cond.lock()
        identityStore.partLengths[partIndex] = length
        if partLengths.allSatisfy({ $0 != nil }) { contentTotal = partLengths.reduce(0) { $0 + ($1 ?? 0) } }
        cond.unlock()
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard dataTask === task, !isStopped else { return }
        if verdictPending {
            verdictPending = false
            if looksLikeHTML(data.prefix(512)) {
                responseFailure = linkExpiredError(detail: "a resume request was answered with a web page", status: nil)
                dataTask.cancel(); return
            }
            if let verdict = pendingVerdict {
                pendingVerdict = nil
                reject(verdict)
                dataTask.cancel(); return
            }
        }
        var bytes = data
        if discardBytes > 0 {
            let drop = Int(min(discardBytes, Int64(bytes.count)))
            discardBytes -= Int64(drop)
            bytes = bytes.dropFirst(drop)
            if bytes.isEmpty { return }
        }
        if !sniffed && partOffset == 0 {
            sniffed = true
            if looksLikeHTML(bytes.prefix(512)) {
                responseFailure = resuming ? linkExpiredError(detail: "body starts with HTML", status: nil) : webPageError(detail: "body starts with HTML")
                dataTask.cancel(); return
            }
        }
        if !write(bytes) { dataTask.cancel(); return }
        partOffset += Int64(bytes.count)
        lastProgress = Date()
        cond.lock()
        downloaded += Int64(bytes.count)
        cond.unlock()
        if chunkBytes >= options.chunkSize { seal() }
        if streamingFallback {
            // No Range support: hold this delegate queue until the unpacker catches up.
            cond.lock()
            while pending >= options.maxPendingBytes - options.chunkSize && !cancelled && stopError == nil {
                _ = cond.wait(until: Date().addingTimeInterval(1))
            }
            cond.unlock()
            lastProgress = Date()
        }
    }

    private func resumeIfRoom() {
        guard waitingForRoom, !isStopped else { return }
        waitingForRoom = false
        lastProgress = Date()
        startRequest() // waits again if there is still not enough room
    }

    func urlSession(_ session: URLSession, task finished: URLSessionTask, didCompleteWithError error: Error?) {
        guard finished === task else { return }
        seal()
        if isStopped { return }
        if let failure = responseFailure {
            if failure.kind == .httpError, failure.detail?.hasPrefix("retry") == true {
                retry(reason: failure.message, hardFailure: failure); return
            }
            stop(failure); return
        }
        if let error = error as NSError? {
            if error.domain == NSURLErrorDomain, [NSURLErrorBadURL, NSURLErrorUnsupportedURL, NSURLErrorCannotFindHost,
                NSURLErrorSecureConnectionFailed, NSURLErrorServerCertificateUntrusted, NSURLErrorServerCertificateHasBadDate,
                NSURLErrorServerCertificateNotYetValid, NSURLErrorAppTransportSecurityRequiresSecureConnection].contains(error.code) {
                // Retry a missing host a few times (DNS hiccup), but these usually mean a bad link.
                if error.code == NSURLErrorCannotFindHost && Date().timeIntervalSince(lastProgress) < 20 {
                    retry(reason: error.localizedDescription, hardFailure: nil); return
                }
                stop(InstallError(.httpError, "The link could not be opened: \(error.localizedDescription)", detail: "\(error.domain) \(error.code)")); return
            }
            retry(reason: error.localizedDescription, hardFailure: nil); return
        }
        // Finished without error: did we get the whole window / part?
        if let end = requestedEnd, partOffset < end, partLength.map({ partOffset < $0 }) ?? true {
            retry(reason: "connection closed early at \(partOffset) of \(end) bytes", hardFailure: nil); return
        }
        if let length = partLength, partOffset < length {
            if requestedEnd != nil { attempt = 0; startRequest(); return } // next window
            retry(reason: "connection closed early at \(partOffset) of \(length) bytes", hardFailure: nil); return
        }
        if partLength == nil && requestedEnd != nil {
            // Window answered without a total size: keep asking until the server says 416.
            attempt = 0; startRequest(); return
        }
        partFinished()
    }

    private func partFinished() {
        log("Downloaded \(urls[partIndex].lastPathComponent): \(partOffset) bytes")
        partIndex += 1
        if partIndex < urls.count {
            partOffset = 0; partLength = nil; validator = nil; sniffed = false; attempt = 0; streamingFallback = false
            startRequest()
        } else {
            cond.lock(); downloadDone = true; cond.broadcast(); cond.unlock()
            session.finishTasksAndInvalidate()
        }
    }

    private func retry(reason: String, hardFailure: InstallError?) {
        let stalled = Date().timeIntervalSince(lastProgress)
        if stalled > options.giveUpAfter {
            stop(hardFailure ?? InstallError(.networkLost, "The internet connection was lost and did not come back for \(options.giveUpAfter >= 120 ? "\(Int(options.giveUpAfter / 60)) minutes" : "\(Int(options.giveUpAfter)) seconds"). Check your connection and start again; what was unpacked so far is kept.", detail: reason))
            return
        }
        attempt += 1
        cond.lock(); retries += 1; cond.unlock()
        let delay = min(15.0, 0.5 * pow(2.0, Double(min(attempt, 6))))
        log("Download interrupted at byte \(partOffset) (\(reason)); resuming in \(String(format: "%.1f", delay)) s (try \(attempt))")
        let deadline = DispatchTime.now() + delay
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: deadline) { [weak self] in
            self?.queue.addOperation { self?.startRequest() }
        }
    }

    private func reject(_ rejection: ResumeRejected) {
        log("Can't continue the earlier download: \(rejection.reason)")
        cond.lock()
        if self.rejection == nil && stopError == nil {
            self.rejection = rejection
            stopError = InstallError(.sourceUnreadable, "Resume not possible.", detail: rejection.reason)
        }
        cond.broadcast()
        cond.unlock()
    }

    private func stop(_ error: InstallError) {
        log("Download stopped: \(error)")
        cond.lock()
        if stopError == nil { stopError = error }
        cond.broadcast()
        cond.unlock()
    }

    // MARK: Chunk files

    private func write(_ data: Data) -> Bool {
        if chunkFD < 0 {
            chunkStart = partOffset
            let url = chunkFolder.appendingPathComponent("c-\(partIndex)-\(partOffset).part")
            chunkFD = open(url.path, O_WRONLY | O_CREAT | O_TRUNC, 0o600)
            guard chunkFD >= 0 else { stop(diskError(errno)); return false }
            chunkURL = url
            chunkBytes = 0
        }
        let ok: Bool = data.withUnsafeBytes { raw in
            var p = raw.baseAddress!, left = raw.count
            while left > 0 {
                let n = Darwin.write(chunkFD, p, left)
                if n < 0 { if errno == EINTR { continue }; return false }
                p += n; left -= n
            }
            return true
        }
        guard ok else { stop(diskError(errno)); return false }
        chunkBytes += Int64(data.count)
        cond.lock()
        pending += Int64(data.count)
        peakPending = max(peakPending, pending)
        cond.unlock()
        return true
    }

    /// Hands the current chunk file to the reader.
    private func seal() {
        guard chunkFD >= 0, let url = chunkURL else { return }
        close(chunkFD); chunkFD = -1
        if chunkBytes == 0 { unlink(url.path); return }
        // Only complete chunk files get their final name: those are what a resumed run may reuse.
        let final = chunkFolder.appendingPathComponent("c-\(partIndex)-\(chunkStart)")
        guard rename(url.path, final.path) == 0 else { stop(diskError(errno)); return }
        cond.lock()
        sealed.append((final, chunkBytes, 0))
        cond.broadcast()
        cond.unlock()
        chunkURL = nil
        chunkBytes = 0
    }

    // MARK: Messages

    private func looksLikeHTML(_ data: Data) -> Bool {
        guard var text = String(data: data, encoding: .utf8) ?? String(data: data, encoding: .isoLatin1) else { return false }
        text = text.trimmingCharacters(in: .whitespacesAndNewlines.union(CharacterSet(charactersIn: "\u{FEFF}"))).lowercased()
        return text.hasPrefix("<!doctype html") || text.hasPrefix("<html") || text.hasPrefix("<head") || text.hasPrefix("<!--") && text.contains("<html")
    }

    private func webPageError(detail: String) -> InstallError {
        InstallError(.webPageNotFile, "The link opens a web page, not the game file. On the download site, copy the address of the actual download (often behind a \"Download\" button), not the page's address.", detail: detail)
    }

    private func linkExpiredError(detail: String, status: Int?) -> InstallError {
        let what = status.map { "the server now refuses it (HTTP \($0))" } ?? "it now opens a web page instead of the file"
        return InstallError(.linkExpired, "The download link stopped working: \(what). Links from download sites often expire after a while. Open the download page again to get a fresh link; the install then continues from where it stopped.", detail: detail)
    }

    private func httpError(_ status: Int, retryAfter: String?) -> InstallError {
        let message: String
        var retryable = false
        switch status {
        case 401, 403: message = "The server refused to give out the file (HTTP \(status)). The link may have expired, need a login, or only work in a browser. Get a fresh link and try again."
        case 404, 410: message = "The file is not at that link (HTTP \(status)). The link is wrong or the file was removed."
        case 429: message = "The server says too many downloads were started (HTTP 429). Wait a while and try again."; retryable = true
        case 500...599: message = "The server had a problem (HTTP \(status)). This is on the server's side; try again later."; retryable = true
        case 300...399: message = "The link redirects somewhere that could not be followed (HTTP \(status))."
        default: message = "The server answered with an error (HTTP \(status))."
        }
        return InstallError(.httpError, message, detail: (retryable ? "retry " : "") + "HTTP \(status)" + (retryAfter.map { ", Retry-After \($0)" } ?? ""))
    }

    private func diskError(_ code: Int32) -> InstallError {
        if code == ENOSPC || code == EDQUOT {
            return InstallError(.outOfSpace, "The device ran out of storage while downloading. Free up space and try again.", detail: String(cString: strerror(code)))
        }
        return InstallError(.writeFailed, "Saving the download failed.", detail: String(cString: strerror(code)))
    }
}

extension Array {
    subscript(safe index: Int) -> Element? { indices.contains(index) ? self[index] : nil }
}
