/**
 * BenchmarkModule.mm
 *
 * Native Module for benchmarking Traditional vs ShmProxy data conversion.
 *
 * 正确的测试方案：
 * 1. 预加载阶段：启动时把 JSON 加载成 NSDictionary 存在内存中（不计时）
 * 2. Traditional：只测量 NSDictionary → folly → jsi 的转换时间
 * 3. ShmProxy：NSDictionary → shm，然后返回 HostObject proxy（lazy loading）
 */

#import "BenchmarkModule.h"
#import "ShmProxyModule.h"
#import "ShmProxyObject.h"
#import "NSDictionaryToShm.h"
#import "JsObjectToShm.h"
#import "ShmToNSDictionary.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTLog.h>
#import <React/RCTBridge.h>
#import <jsi/jsi.h>
#import <mach/mach_time.h>
#include <string>
#include <atomic>
#include <mutex>
#include <objc/runtime.h>

using namespace facebook;

// Shared memory handle for benchmark (exported for AppDelegate)
shm_handle_t g_benchmarkShmHandle = nullptr;
static std::atomic<uint64_t> g_benchmarkKeyCounter{0};

// 预加载的测试数据（内存中的 NSDictionary）
static NSMutableDictionary<NSString*, NSDictionary*> *g_preloadedData = nil;
static std::mutex g_preloadMutex;

// 全局 bridge 引用（用于 JSI）
static __weak RCTBridge *g_bridge = nil;

// JSI bindings 安装标志
static BOOL g_jsiBindingsInstalled = NO;

@implementation BenchmarkModule {
    BOOL _shmInitialized;
    BOOL _dataPreloaded;
}

RCT_EXPORT_MODULE();

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

- (instancetype)init {
    if (self = [super init]) {
        _shmInitialized = NO;
        _dataPreloaded = NO;
        [self initializeShm];
    }
    return self;
}

- (void)setBridge:(RCTBridge *)bridge {
    RCTLogInfo(@"[BenchmarkModule] setBridge called");
    g_bridge = bridge;
}

- (void)initializeShm {
    if (g_benchmarkShmHandle == nullptr) {
        g_benchmarkShmHandle = shm_create(
            "/benchmark_shm",
            4096,      // n_buckets
            65536,     // n_nodes
            64 * 1024 * 1024  // 64MB payload
        );

        if (g_benchmarkShmHandle != nullptr) {
            _shmInitialized = YES;
            RCTLogInfo(@"[BenchmarkModule] Shared memory initialized");
        } else {
            RCTLogError(@"[BenchmarkModule] Failed to initialize shared memory");
        }
    } else {
        _shmInitialized = YES;
    }
}

- (void)invalidate {
    // 重置 JSI bindings 标志，因为 reload 后需要重新安装
    g_jsiBindingsInstalled = NO;

    if (g_benchmarkShmHandle != nullptr) {
        shm_close(g_benchmarkShmHandle);
        shm_destroy("/benchmark_shm");
        g_benchmarkShmHandle = nullptr;
        RCTLogInfo(@"[BenchmarkModule] Shared memory destroyed");
    }

    std::lock_guard<std::mutex> lock(g_preloadMutex);
    g_preloadedData = nil;
}

#pragma mark - Helper Methods

// High-precision timer (nanoseconds)
static double getTimeNanos(void) {
    static mach_timebase_info_data_t info;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        mach_timebase_info(&info);
    });
    uint64_t time = mach_absolute_time();
    return (double)time * info.numer / info.denom;
}

// Load JSON file from bundle
- (NSDictionary *)loadTestData:(NSString *)filename error:(NSError **)error {
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *path = [bundle pathForResource:filename ofType:@"json"];

    if (!path) {
        if (error) {
            *error = [NSError errorWithDomain:@"BenchmarkModule"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey: @"File not found"}];
        }
        return nil;
    }

    NSData *data = [NSData dataWithContentsOfFile:path options:0 error:error];
    if (!data) {
        return nil;
    }

    return [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
}

- (NSString *)filenameForSize:(NSString *)dataSize {
    if ([dataSize isEqualToString:@"128KB"]) {
        return @"test_data_128KB_205";
    } else if ([dataSize isEqualToString:@"256KB"]) {
        return @"test_data_256KB_420";
    } else if ([dataSize isEqualToString:@"512KB"]) {
        return @"test_data_512KB_845";
    } else if ([dataSize isEqualToString:@"1MB"]) {
        return @"test_data_1MB_1693";
    } else if ([dataSize isEqualToString:@"2MB"]) {
        return @"test_data_2MB_3390";
    } else if ([dataSize isEqualToString:@"5MB"]) {
        return @"test_data_5MB_8493";
    } else if ([dataSize isEqualToString:@"10MB"]) {
        return @"test_data_10MB_16986";
    } else if ([dataSize isEqualToString:@"20MB"]) {
        return @"test_data_20MB_33973";
    }
    return nil;
}

#pragma mark - 预加载数据（不计时）

RCT_EXPORT_METHOD(preloadAllData:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {

    std::lock_guard<std::mutex> lock(g_preloadMutex);

    if (g_preloadedData == nil) {
        g_preloadedData = [NSMutableDictionary new];
    }

    NSArray *sizes = @[@"128KB", @"256KB", @"512KB", @"1MB", @"2MB", @"5MB", @"10MB", @"20MB"];
    NSMutableDictionary *loadTimes = [NSMutableDictionary new];

    for (NSString *size in sizes) {
        NSString *filename = [self filenameForSize:size];
        NSError *error = nil;

        double t0 = getTimeNanos();
        NSDictionary *data = [self loadTestData:filename error:&error];
        double t1 = getTimeNanos();

        if (data) {
            g_preloadedData[size] = data;
            loadTimes[size] = @((t1 - t0) / 1000000.0);
            RCTLogInfo(@"[BenchmarkModule] Preloaded %@: %.3fms", size, (t1 - t0) / 1000000.0);
        } else {
            RCTLogError(@"[BenchmarkModule] Failed to preload %@: %@", size, error);
        }
    }

    _dataPreloaded = YES;

    resolve(@{
        @"success": @YES,
        @"loadTimes": loadTimes,
        @"message": @"All data preloaded into memory"
    });
}

#pragma mark - Traditional Method (只测量 NSDictionary → JS 转换)

RCT_EXPORT_METHOD(getDataTraditional:(NSString *)dataSize
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {

    // 从预加载的内存中获取数据
    NSDictionary *testData = nil;
    {
        std::lock_guard<std::mutex> lock(g_preloadMutex);
        testData = g_preloadedData[dataSize];
    }

    if (!testData) {
        reject(@"E_NOT_PRELOADED", @"Data not preloaded. Call preloadAllData first.", nil);
        return;
    }

    // 开始计时 - 只测量 NSDictionary → bridge → JS 的时间
    double t0 = getTimeNanos();

    // 返回数据（RN bridge 会在这里做 NSDictionary → folly::dynamic → jsi::Value 转换）
    // 我们把计时信息也放进去，这样 JS 端可以计算完整的 E2E 时间
    NSDictionary *result = @{
        @"data": testData,
        @"timing": @{
            @"native_start_ns": @(t0)
        },
        @"method": @"traditional"
    };

    resolve(result);
}

#pragma mark - JSON String Method (NSDictionary → JSON String → Bridge → JS parse)

RCT_EXPORT_METHOD(getDataJsonString:(NSString *)dataSize
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {

    // 从预加载的内存中获取数据
    NSDictionary *testData = nil;
    {
        std::lock_guard<std::mutex> lock(g_preloadMutex);
        testData = g_preloadedData[dataSize];
    }

    if (!testData) {
        reject(@"E_NOT_PRELOADED", @"Data not preloaded. Call preloadAllData first.", nil);
        return;
    }

    // 开始计时 - 测量 NSDictionary → JSON String 的时间
    double t0 = getTimeNanos();

    NSError *error = nil;
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:testData options:0 error:&error];

    double t1 = getTimeNanos();

    if (!jsonData || error) {
        reject(@"E_JSON_SERIALIZE", @"JSON serialization failed", error);
        return;
    }

    NSString *jsonString = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];

    double t2 = getTimeNanos();

    // 返回 JSON 字符串，JS 端需要 JSON.parse()
    resolve(@{
        @"jsonString": jsonString,
        @"timing": @{
            @"native_serialize_ms": @((t1 - t0) / 1000000.0),
            @"native_to_string_ms": @((t2 - t1) / 1000000.0),
            @"native_total_ms": @((t2 - t0) / 1000000.0),
            @"native_start_ns": @(t0),
            @"native_end_ns": @(t2),
            @"json_size_bytes": @(jsonData.length)
        },
        @"method": @"json_string"
    });
}

#pragma mark - ShmProxy Method (NSDictionary → shm，返回 HostObject)

RCT_EXPORT_METHOD(prepareShmProxy:(NSString *)dataSize
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {

    // 从预加载的内存中获取数据
    NSDictionary *testData = nil;
    {
        std::lock_guard<std::mutex> lock(g_preloadMutex);
        testData = g_preloadedData[dataSize];
    }

    if (!testData) {
        reject(@"E_NOT_PRELOADED", @"Data not preloaded. Call preloadAllData first.", nil);
        return;
    }

    if (g_benchmarkShmHandle == nullptr) {
        reject(@"E_SHM_NOT_INIT", @"Shared memory not initialized", nil);
        return;
    }

    // 开始计时 - 测量 NSDictionary → shm 的时间
    double t0 = getTimeNanos();

    // Generate unique key
    uint64_t keyId = g_benchmarkKeyCounter.fetch_add(1);
    std::string key = "bench_" + std::to_string(keyId);

    // Convert NSDictionary to shm
    shm_error_t err = convertNSDictionaryToShm(g_benchmarkShmHandle,
                                                key.c_str(),
                                                key.size(),
                                                testData);

    double t1 = getTimeNanos();

    if (err != SHM_OK) {
        reject(@"E_SHM_CONVERT",
               [NSString stringWithFormat:@"SHM conversion failed: %d", err],
               nil);
        return;
    }

    // 返回 key 和计时信息
    // JS 端会用这个 key 通过 JSI 获取 HostObject proxy
    resolve(@{
        @"shmKey": [NSString stringWithUTF8String:key.c_str()],
        @"timing": @{
            @"native_convert_ms": @((t1 - t0) / 1000000.0),
            @"native_start_ns": @(t0),
            @"native_end_ns": @(t1)
        },
        @"method": @"shmproxy"
    });
}

#pragma mark - JSI Installation

// 添加一个同步方法让 JS 端主动触发 JSI 安装
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(installJSIBindingsSync) {
    if (g_jsiBindingsInstalled) {
        return @YES;
    }

    RCTBridge *bridge = g_bridge;
    if (!bridge) {
        bridge = [RCTBridge currentBridge];
        if (bridge) {
            g_bridge = bridge;
        }
    }

    if (!bridge) {
        RCTLogError(@"[BenchmarkModule] Bridge not available");
        return @NO;
    }

    RCTCxxBridge *cxxBridge = (RCTCxxBridge *)bridge;
    if (!cxxBridge.runtime) {
        RCTLogError(@"[BenchmarkModule] Runtime not available");
        return @NO;
    }

    [self installJSIBindings];
    return @(g_jsiBindingsInstalled);
}

- (void)installJSIBindings {
    if (g_jsiBindingsInstalled) {
        RCTLogInfo(@"[BenchmarkModule] JSI bindings already installed");
        return;
    }

    RCTBridge *bridge = g_bridge;
    if (!bridge) {
        RCTLogError(@"[BenchmarkModule] Bridge not available");
        return;
    }

    RCTCxxBridge *cxxBridge = (RCTCxxBridge *)bridge;
    if (!cxxBridge.runtime) {
        RCTLogError(@"[BenchmarkModule] Runtime not available");
        return;
    }

    @try {
        jsi::Runtime &runtime = *(jsi::Runtime *)cxxBridge.runtime;

        // Install __benchmark_getShmProxy function
        // 这个函数返回普通 JS 对象，确保 Object.values() 等方法正常工作
        auto getShmProxy = jsi::Function::createFromHostFunction(
            runtime,
            jsi::PropNameID::forAscii(runtime, "__benchmark_getShmProxy"),
            1,
            [](jsi::Runtime& rt,
               const jsi::Value& thisVal,
               const jsi::Value* args,
               size_t count) -> jsi::Value {

                if (count < 1 || !args[0].isString()) {
                    throw jsi::JSError(rt, "getShmProxy requires a string key argument");
                }

                if (g_benchmarkShmHandle == nullptr) {
                    throw jsi::JSError(rt, "Shared memory not initialized");
                }

                std::string key = args[0].asString(rt).utf8(rt);

                // Check if the key exists
                shm_value_type_t type;
                shm_error_t err = shm_get_value_type(g_benchmarkShmHandle, key.c_str(), key.size(), &type);
                if (err != SHM_OK) {
                    return jsi::Value::undefined();
                }

                // Convert to plain JS object for full compatibility with Object.values/entries/spread
                return react::ShmProxyObject::convertTopLevelToJsObject(rt, g_benchmarkShmHandle, key);
            }
        );
        runtime.global().setProperty(runtime, "__benchmark_getShmProxy", std::move(getShmProxy));

        // Install __benchmark_getTimeNanos function for precise timing in JS
        auto getTimeNanosJS = jsi::Function::createFromHostFunction(
            runtime,
            jsi::PropNameID::forAscii(runtime, "__benchmark_getTimeNanos"),
            0,
            [](jsi::Runtime& rt,
               const jsi::Value& thisVal,
               const jsi::Value* args,
               size_t count) -> jsi::Value {
                return jsi::Value(getTimeNanos());
            }
        );
        runtime.global().setProperty(runtime, "__benchmark_getTimeNanos", std::move(getTimeNanosJS));

        // Install __shm_write function - Write JS object to shared memory
        auto shmWrite = jsi::Function::createFromHostFunction(
            runtime,
            jsi::PropNameID::forAscii(runtime, "__shm_write"),
            1,
            [](jsi::Runtime& rt,
               const jsi::Value& thisVal,
               const jsi::Value* args,
               size_t count) -> jsi::Value {

                if (count < 1 || !args[0].isObject()) {
                    throw jsi::JSError(rt, "__shm_write requires an object argument");
                }

                if (g_benchmarkShmHandle == nullptr) {
                    throw jsi::JSError(rt, "Shared memory not initialized");
                }

                // Generate unique key
                uint64_t keyId = g_benchmarkKeyCounter.fetch_add(1);
                std::string key = "js_" + std::to_string(keyId);

                jsi::Object obj = args[0].asObject(rt);
                shm_error_t err;

                // Check if it's an array or object
                if (obj.isArray(rt)) {
                    err = react::convertJsArrayToShm(rt, g_benchmarkShmHandle, key, obj.asArray(rt));
                } else {
                    err = react::convertJsObjectToShm(rt, g_benchmarkShmHandle, key, obj);
                }

                if (err != SHM_OK) {
                    throw jsi::JSError(rt, "Failed to write to shared memory: " + std::to_string(err));
                }

                // Return the key
                return jsi::String::createFromUtf8(rt, key);
            }
        );
        runtime.global().setProperty(runtime, "__shm_write", std::move(shmWrite));

        // Install __shm_read function - Read from shared memory and return JS object
        auto shmRead = jsi::Function::createFromHostFunction(
            runtime,
            jsi::PropNameID::forAscii(runtime, "__shm_read"),
            1,
            [](jsi::Runtime& rt,
               const jsi::Value& thisVal,
               const jsi::Value* args,
               size_t count) -> jsi::Value {

                if (count < 1 || !args[0].isString()) {
                    throw jsi::JSError(rt, "__shm_read requires a string key argument");
                }

                if (g_benchmarkShmHandle == nullptr) {
                    throw jsi::JSError(rt, "Shared memory not initialized");
                }

                std::string key = args[0].asString(rt).utf8(rt);

                // Check if the key exists
                shm_value_type_t type;
                shm_error_t err = shm_get_value_type(g_benchmarkShmHandle, key.c_str(), key.size(), &type);
                if (err != SHM_OK) {
                    return jsi::Value::undefined();
                }

                // Convert to plain JS object
                return react::ShmProxyObject::convertTopLevelToJsObject(rt, g_benchmarkShmHandle, key);
            }
        );
        runtime.global().setProperty(runtime, "__shm_read", std::move(shmRead));

        g_jsiBindingsInstalled = YES;
        RCTLogInfo(@"[BenchmarkModule] JSI bindings installed successfully");
    } @catch (NSException *exception) {
        RCTLogError(@"[BenchmarkModule] Exception installing JSI bindings: %@", exception);
    }
}

#pragma mark - Utility Methods

RCT_EXPORT_METHOD(getAvailableDataSizes:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    resolve(@[@"128KB", @"256KB", @"512KB", @"1MB", @"2MB", @"5MB", @"10MB", @"20MB"]);
}

RCT_EXPORT_METHOD(getDataInfo:(NSString *)dataSize
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {

    NSDictionary *info = @{
        @"128KB": @{@"segments": @205, @"approx_size_kb": @125},
        @"256KB": @{@"segments": @420, @"approx_size_kb": @252},
        @"512KB": @{@"segments": @845, @"approx_size_kb": @503},
        @"1MB": @{@"segments": @1693, @"approx_size_kb": @1005},
        @"2MB": @{@"segments": @3390, @"approx_size_kb": @2016},
        @"5MB": @{@"segments": @8493, @"approx_size_kb": @5040},
        @"10MB": @{@"segments": @16986, @"approx_size_kb": @10080},
        @"20MB": @{@"segments": @33973, @"approx_size_kb": @20160}
    };

    NSDictionary *sizeInfo = info[dataSize];
    if (sizeInfo) {
        resolve(sizeInfo);
    } else {
        reject(@"E_INVALID_SIZE", @"Invalid data size", nil);
    }
}

RCT_EXPORT_METHOD(getShmStats:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    if (g_benchmarkShmHandle == nullptr) {
        reject(@"E_SHM_NOT_INIT", @"Shared memory not initialized", nil);
        return;
    }

    shm_stats_t stats;
    shm_get_stats(g_benchmarkShmHandle, &stats);

    resolve(@{
        @"buckets": @(stats.n_buckets),
        @"nodes": @(stats.n_nodes),
        @"nodesUsed": @(stats.nodes_used),
        @"payloadCapacity": @(stats.payload_capacity),
        @"payloadUsed": @(stats.payload_used),
        @"generation": @(stats.generation)
    });
}

RCT_EXPORT_METHOD(clearShm:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    if (g_benchmarkShmHandle == nullptr) {
        reject(@"E_SHM_NOT_INIT", @"Shared memory not initialized", nil);
        return;
    }

    // Close and recreate shared memory to clear it
    shm_close(g_benchmarkShmHandle);
    shm_destroy("/benchmark_shm");

    g_benchmarkShmHandle = shm_create(
        "/benchmark_shm",
        4096,      // n_buckets
        65536,     // n_nodes
        64 * 1024 * 1024  // 64MB payload
    );

    g_benchmarkKeyCounter = 0;

    RCTLogInfo(@"[BenchmarkModule] Shared memory cleared");
    resolve(@YES);
}

RCT_EXPORT_METHOD(isDataPreloaded:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    std::lock_guard<std::mutex> lock(g_preloadMutex);
    resolve(@(g_preloadedData != nil && g_preloadedData.count == 8));
}

#pragma mark - JS to Native Data Transfer

// Read data from shared memory that was written by JS
RCT_EXPORT_METHOD(readFromShm:(NSString *)shmKey
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    if (g_benchmarkShmHandle == nullptr) {
        reject(@"E_SHM_NOT_INIT", @"Shared memory not initialized", nil);
        return;
    }

    const char* key = [shmKey UTF8String];
    size_t keyLen = strlen(key);

    double t0 = getTimeNanos();

    id result = nil;
    shm_error_t err = convertShmToNSObject(g_benchmarkShmHandle, key, keyLen, &result);

    double t1 = getTimeNanos();

    if (err != SHM_OK) {
        reject(@"E_SHM_READ",
               [NSString stringWithFormat:@"Failed to read from shm: %d", err],
               nil);
        return;
    }

    resolve(@{
        @"data": result ?: [NSNull null],
        @"timing": @{
            @"native_read_ms": @((t1 - t0) / 1000000.0)
        }
    });
}

// Test JS to Native round-trip: JS writes to shm, Native reads and returns
RCT_EXPORT_METHOD(testJsToNativeRoundTrip:(NSString *)shmKey
                  resolver:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject) {
    if (g_benchmarkShmHandle == nullptr) {
        reject(@"E_SHM_NOT_INIT", @"Shared memory not initialized", nil);
        return;
    }

    const char* key = [shmKey UTF8String];
    size_t keyLen = strlen(key);

    double t0 = getTimeNanos();

    // Read from shm to NSDictionary
    id nativeData = nil;
    shm_error_t err = convertShmToNSObject(g_benchmarkShmHandle, key, keyLen, &nativeData);

    double t1 = getTimeNanos();

    if (err != SHM_OK) {
        reject(@"E_SHM_READ",
               [NSString stringWithFormat:@"Failed to read from shm: %d", err],
               nil);
        return;
    }

    // Now write it back to shm with a new key (to test full round-trip)
    uint64_t keyId = g_benchmarkKeyCounter.fetch_add(1);
    std::string newKey = "roundtrip_" + std::to_string(keyId);

    double t2 = getTimeNanos();

    if ([nativeData isKindOfClass:[NSDictionary class]]) {
        err = convertNSDictionaryToShm(g_benchmarkShmHandle,
                                        newKey.c_str(),
                                        newKey.size(),
                                        (NSDictionary*)nativeData);
    } else if ([nativeData isKindOfClass:[NSArray class]]) {
        err = convertNSArrayToShm(g_benchmarkShmHandle,
                                   newKey.c_str(),
                                   newKey.size(),
                                   (NSArray*)nativeData);
    } else {
        reject(@"E_INVALID_TYPE", @"Data must be object or array", nil);
        return;
    }

    double t3 = getTimeNanos();

    if (err != SHM_OK) {
        reject(@"E_SHM_WRITE",
               [NSString stringWithFormat:@"Failed to write to shm: %d", err],
               nil);
        return;
    }

    resolve(@{
        @"originalKey": shmKey,
        @"newKey": [NSString stringWithUTF8String:newKey.c_str()],
        @"timing": @{
            @"shm_to_native_ms": @((t1 - t0) / 1000000.0),
            @"native_to_shm_ms": @((t3 - t2) / 1000000.0),
            @"total_ms": @((t3 - t0) / 1000000.0)
        }
    });
}

@end
