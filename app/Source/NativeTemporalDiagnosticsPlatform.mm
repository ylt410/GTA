#import <Foundation/Foundation.h>
#include "NativeGraphics.h"
#include "NativeTemporalDiagnosticsPlatform.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>

namespace {
std::mutex control;
std::atomic<int> status{0};
uint64_t requestGeneration = 0;
std::once_flag initialized;
std::string reportPath;
struct ShaderLabel { uint32_t token; char hash[33], label[97]; };
ShaderLabel labels[256]{};
size_t labelCount = 0;
uint64_t labelOverflow = 0;

void initialize() {
    std::call_once(initialized, [] {
        NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
        if (docs) reportPath = [[docs stringByAppendingPathComponent:@"temporal-diagnostics.json"] UTF8String];
        if (!reportPath.empty() && [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:reportPath.c_str()]]) status.store(4);
    });
}
NSString *hex(uint64_t value) { return [NSString stringWithFormat:@"%016llx", (unsigned long long)value]; }
id finiteNumber(float value) { return std::isfinite(value) ? (id)@(value) : (id)[NSNull null]; }
NSArray *floats(const float *values, size_t count) {
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; ++i) [result addObject:std::isfinite(values[i]) ? (id)@(values[i]) : (id)[NSNull null]];
    return result;
}
NSArray *words(const uint32_t *values, size_t count) {
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:count];
    for (size_t i = 0; i < count; ++i) [result addObject:@(values[i])];
    return result;
}
NSDictionary *sampleJSON(const PGTDMetalSample &s) {
    const auto &t = s.tag;
    NSMutableArray *hashes = [NSMutableArray arrayWithCapacity:4];
    for (unsigned i = 0; i < 4; ++i) [hashes addObject:hex(s.uniformHash[i])];
    return @{
        @"frame": @(s.frame), @"opcode": @(s.opcode), @"sequence": hex(t.sequence), @"thread": @(t.producerThread),
        @"stage": @(t.stage), @"entityClass": @(t.entityClass), @"candidateEntityToken": @(t.entityKey),
        @"entityToken": @(t.entityToken), @"auxiliaryToken": @(t.auxiliaryToken),
        @"rootMatrixToken": @(t.rootMatrixToken), @"paletteToken": @(t.paletteToken),
        @"scopeDepth": @(t.scopeDepth), @"tagFlags": @(t.flags),
        @"camera": @{@"viewportToken": @(t.viewportToken), @"valid": @(t.camera.valid),
            @"size": @[@(t.camera.width), @(t.camera.height)],
            @"nearFar": @[finiteNumber(t.camera.nearPlane), finiteNumber(t.camera.farPlane)],
            @"viewCandidate": floats(t.camera.view, 16), @"projectionCandidate": floats(t.camera.projection, 16)},
        @"draw": @[@(s.drawKind), @(s.indexCount), @(s.instanceCount)],
        @"shaderTokens": @[@(s.vsToken), @(s.psToken)], @"layoutToken": @(s.layoutToken),
        @"color": @{@"tokens": words(s.colorToken, 4), @"formats": words(s.colorFormat, 4),
            @"widths": words(s.colorWidth, 4), @"heights": words(s.colorHeight, 4), @"samples": words(s.colorSamples, 4)},
        @"depth": @{@"token": @(s.depthToken), @"format": @(s.depthFormat), @"width": @(s.depthWidth),
            @"height": @(s.depthHeight), @"samples": @(s.depthSamples), @"mip": @(s.depthMip), @"slice": @(s.depthSlice)},
        @"sourcePresentationTexture": @{@"token": @(s.presentedToken), @"width": @(s.presentedWidth),
            @"height": @(s.presentedHeight), @"format": @(s.presentedFormat)},
        @"pixelShaderInputs": @{@"slots": words(s.srvSlot, 4), @"tokens": words(s.srvToken, 4),
            @"formats": words(s.srvFormat, 4), @"widths": words(s.srvWidth, 4), @"heights": words(s.srvHeight, 4)},
        @"viewport": floats(s.viewport, 4),
        @"uniform": @{@"tokens": words(s.uniformToken, 4), @"bytesHashed": words(s.uniformBytes, 4),
            @"firstRows": words(s.uniformFirst, 4), @"hashes": hashes},
        @"wvpCandidate": floats(s.wvp, 16), @"flags": @(s.flags)
    };
}
void finishLocked() {
    if (status.load() != 2) return;
    PGTDEnd();
    auto report = std::shared_ptr<PGTDReport>(new (std::nothrow) PGTDReport);
    if (!report || !PGTDCopyReport(report.get())) { status.store(5); return; }
    status.store(3);
    NSMutableArray *shaderManifest = [NSMutableArray arrayWithCapacity:labelCount];
    for (size_t i = 0; i < labelCount; ++i) {
        [shaderManifest addObject:@{@"token": @(labels[i].token),
            @"hash": [NSString stringWithUTF8String:labels[i].hash] ?: @"",
            @"label": [NSString stringWithUTF8String:labels[i].label] ?: @""}];
    }
    const uint64_t lostLabels = labelOverflow;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool {
            NSMutableArray *records = [NSMutableArray arrayWithCapacity:report->recordCount];
            for (uint32_t i = 0; i < report->recordCount; ++i) [records addObject:sampleJSON(report->samples[i])];
            const auto &c = report->stats;
            NSMutableDictionary *root = [@{
                @"schema": @1, @"purpose": @"Temporal input diagnostics ONLY; reconstruction is not enabled",
                @"engineSHA256": @"11ca8d2c04c5e843d18ff4aea4899d72c86973c6b031df334e67c446b2ae83e0",
                @"baseCommit": @"704e526cd1ab8c265371f3fa4995371c8b1e853d",
                @"session": hex(report->session), @"sourcePresentCount": @(report->frameCount),
                @"privacy": @"No account, paths, URLs, screenshots, audio, raw pointers or OS thread IDs. Camera numbers describe game space.",
                @"limitations": @"Candidate identities are not generation-safe; sampled/unknown/overflow paths are not proof of absence. No performance measurements.",
                @"uniformSlots": @[@1, @2, @3, @0], @"uniformHashByteLimit": @4096,
                @"stats": @{@"claimed": @(c.claimed), @"published": @(c.published), @"matched": @(c.matched),
                    @"misses": @(c.misses), @"collisions": @(c.collisions), @"overflow": @(c.overflow),
                    @"invalidCamera": @(c.invalidCamera), @"scopeErrors": @(c.scopeErrors),
                    @"tokenOverflow": @(c.tokenOverflow), @"truncatedRecords": @(c.truncatedRecords),
                    @"recordOverflow": @(c.recordOverflow),
                    @"invalidRecord": @(c.invalidRecord), @"threadOverflow": @(c.threadOverflow),
                    @"shaderManifestOverflow": @(lostLabels)},
                @"shaderManifest": shaderManifest, @"records": records
            } mutableCopy];
            NSError *error = nil;
            NSData *data = nil;
            // Explicitly bound the on-disk report. Trim complete records, never JSON bytes.
            do {
                root[@"fileOmittedRecords"] = @(report->recordCount - records.count);
                data = [NSJSONSerialization dataWithJSONObject:root options:0 error:&error];
                if (!data || data.length <= 4 * 1024 * 1024 || records.count == 0) break;
                NSRange tail = NSMakeRange(records.count > 128 ? records.count - 128 : 0, MIN((NSUInteger)128, records.count));
                [records removeObjectsInRange:tail];
            } while (true);
            const bool ok = data && data.length <= 4 * 1024 * 1024 && !reportPath.empty() &&
                [data writeToFile:[NSString stringWithUTF8String:reportPath.c_str()] options:NSDataWritingAtomic error:&error];
            status.store(ok ? 4 : 5);
        }
    });
}
} // namespace

extern "C" int PGTemporalDiagnosticsStatus(void) { initialize(); return status.load(); }
extern "C" const char *PGTemporalDiagnosticsReportPath(void) { initialize(); return reportPath.c_str(); }
extern "C" void PGTemporalDiagnosticsStart(void) {
    initialize();
    std::lock_guard<std::mutex> guard(control);
    if (status.load() == 1 || status.load() == 2 || status.load() == 3) return;
    const uint64_t request = ++requestGeneration;
    status.store(1);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        std::lock_guard<std::mutex> lock(control);
        if (request != requestGeneration || status.load() != 1) return;
        labelCount = 0; labelOverflow = 0;
        PGTDBegin();
        status.store(2);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            std::lock_guard<std::mutex> timerLock(control);
            if (request == requestGeneration) finishLocked();
        });
    });
}
extern "C" void PGTemporalDiagnosticsStop(void) {
    initialize();
    std::lock_guard<std::mutex> guard(control);
    if (status.load() == 1) {
        ++requestGeneration;
        status.store(!reportPath.empty() && [[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithUTF8String:reportPath.c_str()]] ? 4 : 0);
    }
    else finishLocked();
}
extern "C" void PGTDPlatformFrame(void) {
    if (!PGTDEnabled()) return;
    if (PGTDNextFrame() < 60) return;
    std::lock_guard<std::mutex> guard(control);
    finishLocked();
}
extern "C" void PGTDPlatformShader(uint32_t token, const char *hash, const char *label) {
    if (!token || !PGTDEnabled()) return;
    std::lock_guard<std::mutex> guard(control);
    if (status.load() != 2) return;
    for (size_t i = 0; i < labelCount; ++i) if (labels[i].token == token) return;
    if (labelCount == 256) { ++labelOverflow; return; }
    auto &entry = labels[labelCount++]; entry.token = token;
    // Only shader identifiers are allowed into the report, even if a malformed
    // reflection table supplied a path/URL or arbitrary diagnostic message.
    size_t i = 0;
    for (; hash && hash[i] && i + 1 < sizeof entry.hash; ++i) {
        const char c = hash[i]; entry.hash[i] = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ? c : '_';
    }
    entry.hash[i] = 0;
    i = 0;
    for (; label && label[i] && i + 1 < sizeof entry.label; ++i) {
        const char c = label[i];
        entry.label[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-' ? c : '_';
    }
    entry.label[i] = 0;
}
