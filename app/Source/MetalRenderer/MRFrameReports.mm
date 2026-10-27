// Local, bounded whole-frame reports every 15 seconds of active world rendering.
// The old --frame-reports flag alone opts into sending to the player's own server.
#include "MRFrameReports.h"
#include "NativeFrameMetrics.h"
#include "NativeAssetServer.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#include <TargetConditionals.h>
#include <atomic>
#include <mach/mach.h>
#include <os/proc.h>
#include <sys/resource.h>
#include <time.h>
#include <mutex>
#include <string>
#include <cstdlib>
#include "NativeEngineConfiguration.h"

extern "C" void pg_log(const char *);

namespace mr {
static bool enabled, remoteEnabled;
static PGFrameMetricsWindow window;
static double windowStart;
static uint64_t sequence;
static std::atomic<bool> inFlight{false}, resetPending{false}, applicationActive{true};
static NSURLSession *session;
static NSString *runId, *build;
static id<MTLDevice> device;
static double processCpuStart, rendererCpuStart;
static std::mutex gpuLock;
static PGFrameMetricsWindow gpuWindow;
static uint64_t gpuInvalid, healthGpuFrames;
static double healthGpuMs;
static std::atomic<bool> recordingGpu{false};
static std::atomic<bool> localInFlight{false};

static double processCpuMs() {
	struct rusage usage = {};
	if (getrusage(RUSAGE_SELF, &usage)) return -1;
	return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 + (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}
static double rendererCpuMs() {
	struct timespec now = {};
	return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) ? -1 : now.tv_sec * 1000.0 + now.tv_nsec / 1e6;
}
void frameReportsGpuComplete(double gpuMs, bool valid) {
	std::lock_guard<std::mutex> lock(gpuLock);
	valid = valid && std::isfinite(gpuMs) && gpuMs > 0;
	if (valid) { healthGpuMs += gpuMs; ++healthGpuFrames; }
	if (!recordingGpu.load() || !applicationActive.load()) return;
	if (valid) gpuWindow.record(gpuMs, 0, {}); else ++gpuInvalid;
}
double frameReportsTakeHealthGpuMs() {
	std::lock_guard<std::mutex> lock(gpuLock);
	double result = healthGpuFrames ? healthGpuMs / healthGpuFrames : 0;
	healthGpuFrames = 0; healthGpuMs = 0; return result;
}
static void resetLocalWindow() {
	window.reset(); windowStart = 0; recordingGpu.store(false);
	std::lock_guard<std::mutex> lock(gpuLock); gpuWindow.reset(); gpuInvalid = 0;
}
static void enqueueLocal(PGFrameMetricsSnapshot stats, PGFrameMetricsSnapshot gpu,
                         uint64_t invalidGpu, double cpuMs, double renderCpuMs) {
	bool expected = false;
	if (!localInFlight.compare_exchange_strong(expected, true)) return;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
		task_vm_info_data_t memory = {}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		bool memoryKnown = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&memory, &count) == KERN_SUCCESS;
		double frames = (double)stats.frames;
		const char *cores = getenv("PG_ENGINE_CORES");
		NSDictionary *report = @{
			@"schema": @2, @"build": build ?: @"unknown", @"sourceFrames": @(stats.frames),
			@"sourceFps": @(stats.fps()), @"sourceP50Ms": @(stats.p50Ms), @"sourceP95Ms": @(stats.p95Ms),
			@"sourceP99Ms": @(stats.p99Ms), @"sourceMaxMs": @(stats.maxMs),
			@"over50Ms": @(stats.over50), @"over100Ms": @(stats.over100), @"over250Ms": @(stats.over250),
			@"cpuMsPerSourceFrame": cpuMs >= 0 ? @(cpuMs / frames) : NSNull.null,
			@"rendererCpuMsPerSourceFrame": renderCpuMs >= 0 ? @(renderCpuMs / frames) : NSNull.null,
			@"gpuCompletedFrames": @(gpu.frames), @"gpuInvalidFrames": @(invalidGpu),
			@"gpuMsPerCompletedFrame": gpu.frames ? @(gpu.totalMs / gpu.frames) : NSNull.null,
			@"gpuP95Ms": gpu.frames ? @(gpu.p95Ms) : NSNull.null,
			@"gpuSampleScope": @"all command buffers of completed source frames; completion window may lag the CPU window",
			@"idleMsPerFrame": @(stats.work.idle / frames), @"pipelineMsPerFrame": @(stats.work.pipeline / frames),
			@"uploadMsPerFrame": @(stats.work.uploads / frames), @"uploadBytes": @(stats.work.uploadBytes),
			@"gpuWaitMsPerFrame": @(stats.work.gpuWait / frames), @"submitMsPerFrame": @(stats.work.submit / frames),
			@"drawableWaitMsPerFrame": @(stats.work.drawableWait / frames), @"retireMsPerFrame": @(stats.work.retire / frames),
			@"worstDrawableWaitMs": @(stats.worstWork.drawableWait), @"worstRetireMs": @(stats.worstWork.retire),
			@"worstGpuWaitMs": @(stats.worstWork.gpuWait), @"drawsPerFrame": @(stats.draws / frames),
			@"footprintMB": memoryKnown ? @(memory.phys_footprint / 1048576.0) : NSNull.null,
			@"metalAllocatedMB": device ? @(device.currentAllocatedSize / 1048576.0) : NSNull.null,
			@"thermal": @(NSProcessInfo.processInfo.thermalState),
			@"engineCores": @(pg_engine_core_configuration(cores, NULL, 0))
		};
		NSData *data = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
		if (data) {
			NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
			std::string line = "[performance] "; line += text.UTF8String; pg_log(line.c_str());
			NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
			[data writeToFile:[docs stringByAppendingPathComponent:@"native-performance.json"] options:NSDataWritingAtomic error:nil];
		}
		localInFlight.store(false);
	}});
}

static NSString *number(double value) { return [NSString stringWithFormat:@"%.3f", value]; }

static void enqueue(PGFrameMetricsSnapshot stats) {
	if (!remoteEnabled) return;
	bool expected = false;
	if (!inFlight.compare_exchange_strong(expected, true)) return;
	uint64_t seq = ++sequence;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
		NSURLComponents *url = [NSURLComponents componentsWithString:[PGAssetServer() stringByAppendingString:@"/install/native-performance.json"]];
		double frames = (double)stats.frames;
		task_vm_info_data_t memory = {};
		mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
		bool memoryKnown = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&memory, &count) == KERN_SUCCESS;
		NSDictionary<NSString *, NSString *> *values = @{
			@"schema": @"1", @"renderer": @"metal", @"qos": @"0", @"build": build, @"run": runId, @"seq": [NSString stringWithFormat:@"%llu", seq],
			@"frames": [NSString stringWithFormat:@"%llu", stats.frames], @"fps": number(stats.fps()), @"p95": number(stats.p95Ms), @"p99": number(stats.p99Ms), @"max": number(stats.maxMs),
			@"over33": [NSString stringWithFormat:@"%llu", stats.over33], @"over50": [NSString stringWithFormat:@"%llu", stats.over50],
			@"over100": [NSString stringWithFormat:@"%llu", stats.over100], @"over250": [NSString stringWithFormat:@"%llu", stats.over250],
			@"draws": number(stats.draws / frames), @"idle": number(stats.work.idle / frames), @"pipe": number(stats.work.pipeline / frames),
			@"modules": number(0), @"upload": number(stats.work.uploads / frames), @"uploadMB": number(stats.work.uploadBytes / 1048576.0),
			@"wait": number(stats.work.gpuWait / frames), @"submit": number(stats.work.submit / frames),
			@"worstIdle": number(stats.worstWork.idle), @"worstPipe": number(stats.worstWork.pipeline), @"worstModules": number(0),
			@"worstUpload": number(stats.worstWork.uploads), @"worstWait": number(stats.worstWork.gpuWait), @"worstSubmit": number(stats.worstWork.submit),
			@"memoryClamps": number(0), @"metalMB": number(device ? device.currentAllocatedSize / 1048576.0 : 0),
			@"footprintMB": number(memoryKnown ? memory.phys_footprint / 1048576.0 : 0),
#if TARGET_OS_SIMULATOR
			@"headroomMB": number(0),
#else
			@"headroomMB": number(os_proc_available_memory() / 1048576.0),
#endif
			@"thermal": [NSString stringWithFormat:@"%ld", (long)NSProcessInfo.processInfo.thermalState],
		};
		NSMutableArray<NSURLQueryItem *> *items = [NSMutableArray arrayWithCapacity:values.count];
		for (NSString *key in [[values allKeys] sortedArrayUsingSelector:@selector(compare:)]) [items addObject:[NSURLQueryItem queryItemWithName:key value:values[key]]];
		url.queryItems = items;
		NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url.URL cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:3];
		NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:^(NSData *, NSURLResponse *, NSError *) { inFlight.store(false); }];
		task.priority = NSURLSessionTaskPriorityLow;
		[task resume];
	}});
}

void frameReportsInstall(id<MTLDevice> metalDevice) {
	device = metalDevice;
	enabled = !(getenv("PG_FRAME_METRICS") && atoi(getenv("PG_FRAME_METRICS")) == 0);
	remoteEnabled = [NSProcessInfo.processInfo.arguments containsObject:@"--frame-reports"];
	if (!enabled && !remoteEnabled) return;
	build = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"probe";
	runId = NSUUID.UUID.UUIDString;
	if (remoteEnabled) {
	NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
	configuration.URLCache = nil; configuration.HTTPCookieStorage = nil; configuration.HTTPMaximumConnectionsPerHost = 1;
	configuration.timeoutIntervalForRequest = 3; configuration.timeoutIntervalForResource = 5;
	session = [NSURLSession sessionWithConfiguration:configuration];
	}
	dispatch_async(dispatch_get_main_queue(), ^{
		[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *) { applicationActive.store(false); resetPending.store(true); }];
		[NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *) { resetPending.store(true); applicationActive.store(true); }];
	});
	pg_log("Local frame metrics: 15-second source-frame percentiles, CPU time, grouped GPU time and waits; native-performance.json is overwritten, not appended.");
}

void frameReportsRecord(double timeMs, double frameMs, uint64_t draws, bool world, const FrameWork &w) {
	if (!enabled && !remoteEnabled) return;
	if (resetPending.exchange(false)) { resetLocalWindow(); return; }
	if (!applicationActive.load() || !world) { if (windowStart) resetLocalWindow(); return; }
	if (!windowStart) {
		// Drop the first interval so CPU baselines and counted frames have
		// identical boundaries, including after returning from the background.
		windowStart = timeMs; processCpuStart = processCpuMs(); rendererCpuStart = rendererCpuMs();
		recordingGpu.store(true); return;
	}
	PGFrameWork work{w.idle, w.pipeline, 0, w.uploads, w.uploadBytes, w.gpuWait, w.submit, w.drawableWait, w.retire};
	if (!window.record(frameMs, draws, work)) return;
	if (timeMs - windowStart >= 15000) {
		double cpu = processCpuMs(), renderCpu = rendererCpuMs();
		PGFrameMetricsSnapshot gpu; uint64_t invalid;
		{ std::lock_guard<std::mutex> lock(gpuLock); gpu = gpuWindow.snapshot(); invalid = gpuInvalid; gpuWindow.reset(); gpuInvalid = 0; }
		auto stats = window.snapshot();
		if (enabled) enqueueLocal(stats, gpu, invalid, processCpuStart >= 0 && cpu >= processCpuStart ? cpu - processCpuStart : -1,
		                          rendererCpuStart >= 0 && renderCpu >= rendererCpuStart ? renderCpu - rendererCpuStart : -1);
		enqueue(stats); window.reset(); windowStart = timeMs; processCpuStart = cpu; rendererCpuStart = renderCpu;
	}
}
}  // namespace mr
