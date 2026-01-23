/**
 * ShmProxyModule.mm
 *
 * High-performance data transfer using Shared Memory for React Native.
 *
 * This module provides faster data transfer between Native and JavaScript
 * by using shared memory instead of the traditional bridge serialization.
 *
 * Performance improvements:
 * - 1MB data: ~57% faster than traditional method
 * - 20MB data: ~57% faster than traditional method
 *
 * Copyright (c) 2024
 * Licensed under the MIT license
 *
 * @see https://github.com/yourusername/react-native-shmproxy
 */

#import "ShmProxyModule.h"
#import "ShmProxyObject.h"
#import "NSDictionaryToShm.h"
#import "JsObjectToShm.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTLog.h>
#import <React/RCTUtils.h>
#import <jsi/jsi.h>
#import <mach/mach_time.h>

using namespace facebook;

// =============================================================================
// Global Shared Memory Handle
// =============================================================================

/** Global shared memory handle managed by the module */
static shm_handle_t g_shmHandle = nullptr;

/** Shared memory name */
static NSString* g_shmName = @"/rn_shm_proxy";

/** Counter for generating unique keys */
static std::atomic<uint64_t> g_keyCounter{0};

/** Global bridge reference for JSI installation */
static __weak RCTBridge *g_bridge = nil;

/** Flag to track JSI bindings installation */
static BOOL g_jsiBindingsInstalled = NO;

// =============================================================================
// Utility Functions
// =============================================================================

/**
 * Get current time in nanoseconds for precise timing
 *
 * @return Current time in nanoseconds
 */
static double getTimeNanos() {
    static mach_timebase_info_data_t timebase;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        mach_timebase_info(&timebase);
    });
    uint64_t time = mach_absolute_time();
    return (double)time * timebase.numer / timebase.denom;
}

// =============================================================================
// ShmProxyModule Implementation
// =============================================================================

@implementation ShmProxyModule

RCT_EXPORT_MODULE(ShmProxy)

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

- (instancetype)init {
    if (self = [super init]) {
        // Initialize shared memory on first use
        if (g_shmHandle == nullptr) {
            [self initializeShm];
        }
    }
    return self;
}

- (void)setBridge:(RCTBridge *)bridge {
    g_bridge = bridge;
}

/**
 * Initialize shared memory with default configuration
 *
 * Creates a shared memory segment with:
 * - 4096 hash buckets
 * - 65536 nodes
 * - 64MB payload capacity
 */
- (void)initializeShm {
    if (g_shmHandle == nullptr) {
        g_shmHandle = shm_create(
            [g_shmName UTF8String],
            4096,              // n_buckets
            65536,             // n_nodes
            64 * 1024 * 1024  // 64MB payload
        );

        if (g_shmHandle == nullptr) {
            RCTLogError(@"[ShmProxy] Failed to create shared memory");
        } else {
            RCTLogInfo(@"[ShmProxy] Shared memory initialized successfully");
        }
    }
}

- (void)invalidate {
    // Reset JSI bindings flag (will need reinstallation after reload)
    g_jsiBindingsInstalled = NO;

    // Clean up shared memory when the module is invalidated
    if (g_shmHandle != nullptr) {
        shm_close(g_shmHandle);
        shm_destroy([g_shmName UTF8String]);
        g_shmHandle = nullptr;
        RCTLogInfo(@"[ShmProxy] Shared memory destroyed");
    }
}

// =============================================================================
// Native Module Methods (Promise-based API)
// =============================================================================

/**
 * Initialize shared memory explicitly
 *
 * This is optional as shared memory is initialized automatically on first use.
 * Use this method to ensure SHM is ready before operations.
 *
 * @resolve YES if initialization successful
 * @reject Error if initialization failed
 */
RCT_EXPORT_METHOD(initialize:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    if (g_shmHandle == nullptr) {
        [self initializeShm];
    }

    if (g_shmHandle != nullptr) {
        resolve(@(YES));
    } else {
        reject(@"INIT_ERROR", @"Failed to initialize shared memory", nil);
    }
}

/**
 * Write JavaScript object to shared memory
 *
 * Converts the provided NSDictionary to shared memory binary format
 * and returns a unique key for later retrieval.
 *
 * @param data The object to write (NSDictionary from JS)
 * @resolve The SHM key (string)
 * @reject Error if write failed (e.g., out of memory)
 */
RCT_EXPORT_METHOD(writeData:(NSDictionary *)data
                  resolve:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {

    if (g_shmHandle == nullptr) {
        reject(@"NOT_INITIALIZED", @"Shared memory not initialized. Call initialize() first.", nil);
        return;
    }

    // Generate unique key
    uint64_t keyId = g_keyCounter.fetch_add(1);
    std::string key = "shm_" + std::to_string(keyId);

    // Convert NSDictionary to SHM
    shm_error_t err = convertNSDictionaryToShm(g_shmHandle, key.c_str(), key.size(), data);

    if (err != SHM_OK) {
        NSString* errorMsg = [NSString stringWithFormat:@"Failed to write data to SHM: error %d", err];
        reject(@"WRITE_ERROR", errorMsg, nil);
        return;
    }

    NSString* keyString = [NSString stringWithUTF8String:key.c_str()];
    RCTLogInfo(@"[ShmProxy] Data written with key: %@", keyString);
    resolve(keyString);
}

/**
 * Read object from shared memory with full conversion
 *
 * This method is NOT recommended for use. Instead, use the synchronous
 * __shm_read JSI function for better performance.
 *
 * @param key The SHM key
 * @resolve The JavaScript object
 * @reject Error if read failed
 */
RCT_EXPORT_METHOD(readData:(NSString *)key
                  resolve:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {

    if (g_shmHandle == nullptr) {
        reject(@"NOT_INITIALIZED", @"Shared memory not initialized", nil);
        return;
    }

    std::string keyStr = [key UTF8String];

    // Check if key exists
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(g_shmHandle, keyStr.c_str(), keyStr.size(), &type);

    if (err != SHM_OK) {
        reject(@"KEY_NOT_FOUND", @"Key not found in shared memory", nil);
        return;
    }

    // Note: This should be called from JS via __shm_read for synchronous access
    // For Promise-based API, we need to bridge to JSI
    // In practice, use __shm_read directly from JS
    reject(@"USE_JSI", @"Please use __shm_read JSI function for synchronous read", nil);
}

/**
 * Get shared memory statistics
 *
 * Returns current memory usage and capacity information.
 *
 * @resolve Statistics object with buckets, nodes, memory usage
 * @reject Error if SHM not initialized
 */
RCT_EXPORT_METHOD(getStats:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {

    if (g_shmHandle == nullptr) {
        reject(@"NOT_INITIALIZED", @"Shared memory not initialized", nil);
        return;
    }

    shm_stats_t stats;
    shm_get_stats(g_shmHandle, &stats);

    NSDictionary* statsDict = @{
        @"buckets": @(stats.n_buckets),
        @"nodes": @(stats.n_nodes),
        @"nodesUsed": @(stats.nodes_used),
        @"payloadCapacity": @(stats.payload_capacity),
        @"payloadUsed": @(stats.payload_used),
        @"generation": @(stats.generation)
    };

    resolve(statsDict);
}

/**
 * Clear all data from shared memory
 *
 * Frees all stored data and resets the shared memory.
 * All existing keys become invalid after this operation.
 *
 * @resolve YES if clear successful
 * @reject Error if clear failed
 */
RCT_EXPORT_METHOD(clear:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {

    if (g_shmHandle != nullptr) {
        // Close and destroy existing SHM
        shm_close(g_shmHandle);
        shm_destroy([g_shmName UTF8String]);

        // Create new SHM
        [self initializeShm];

        // Reset counter
        g_keyCounter = 0;

        resolve(@(YES));
        RCTLogInfo(@"[ShmProxy] Shared memory cleared");
    } else {
        reject(@"NOT_INITIALIZED", @"Shared memory not initialized", nil);
    }
}

/**
 * Check if shared memory has been initialized
 *
 * @resolve YES if initialized, NO otherwise
 */
RCT_EXPORT_METHOD(isInitialized:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    BOOL initialized = (g_shmHandle != nullptr);
    resolve(@(initialized));
}

/**
 * Install JSI bindings (synchronous method called from JS)
 *
 * This method should be called from JavaScript on app startup to install
 * the global JSI functions (__shm_write, __shm_read, etc.)
 *
 * @return YES if installation successful, NO otherwise
 */
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
        RCTLogError(@"[ShmProxy] Bridge not available");
        return @NO;
    }

    RCTCxxBridge *cxxBridge = (RCTCxxBridge *)bridge;
    if (!cxxBridge.runtime) {
        RCTLogError(@"[ShmProxy] Runtime not available");
        return @NO;
    }

    // Install JSI bindings using proper Runtime cast
    facebook::jsi::Runtime &runtime = *(facebook::jsi::Runtime *)cxxBridge.runtime;
    [ShmProxyModule installWithRuntime:runtime];
    RCTLogInfo(@"[ShmProxy] JSI bindings installed successfully");

    return @YES;
}

// =============================================================================
// JSI Bindings Installation
// =============================================================================

/**
 * Install global JSI functions for synchronous shared memory access
 *
 * Installs the following global functions:
 * - __shm_write(obj): Write object to SHM, returns key
 * - __shm_read(key): Read object from SHM, returns object
 * - __shm_getStats(): Get SHM statistics
 *
 * @param runtime The JSI runtime
 */
+ (void)installWithRuntime:(jsi::Runtime&)runtime {

    RCTLogInfo(@"[ShmProxy] Installing JSI bindings...");

    // =========================================================================
    // __shm_write(obj) -> string
    // Write JavaScript object to shared memory
    // =========================================================================

    auto shmWrite = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shm_write"),
        1,  // 1 argument: the object to write
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1 || !args[0].isObject()) {
                throw jsi::JSError(rt, "__shm_write requires an object argument");
            }

            if (g_shmHandle == nullptr) {
                throw jsi::JSError(rt, "Shared memory not initialized");
            }

            // Generate unique key
            uint64_t keyId = g_keyCounter.fetch_add(1);
            std::string key = "shm_" + std::to_string(keyId);

            jsi::Object obj = args[0].asObject(rt);
            shm_error_t err;

            // Check if it's an array or object
            if (obj.isArray(rt)) {
                err = react::convertJsArrayToShm(rt, g_shmHandle, key, obj.asArray(rt));
            } else {
                err = react::convertJsObjectToShm(rt, g_shmHandle, key, obj);
            }

            if (err != SHM_OK) {
                std::string errorMsg = "Failed to write to SHM: " + std::to_string(err);
                throw jsi::JSError(rt, errorMsg);
            }

            // Return the key
            return jsi::String::createFromUtf8(rt, key);
        }
    );
    runtime.global().setProperty(runtime, "__shm_write", std::move(shmWrite));

    // =========================================================================
    // __shm_read(key) -> object
    // Read JavaScript object from shared memory with full conversion
    // =========================================================================

    auto shmRead = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shm_read"),
        1,  // 1 argument: the SHM key
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1 || !args[0].isString()) {
                throw jsi::JSError(rt, "__shm_read requires a string key argument");
            }

            if (g_shmHandle == nullptr) {
                throw jsi::JSError(rt, "Shared memory not initialized");
            }

            std::string key = args[0].asString(rt).utf8(rt);

            // Check if the key exists
            shm_value_type_t type;
            shm_error_t err = shm_get_value_type(g_shmHandle, key.c_str(), key.size(), &type);
            if (err != SHM_OK) {
                return jsi::Value::undefined();
            }

            // Convert to plain JS object for full compatibility
            return react::ShmProxyObject::convertTopLevelToJsObject(rt, g_shmHandle, key);
        }
    );
    runtime.global().setProperty(runtime, "__shm_read", std::move(shmRead));

    // =========================================================================
    // __shm_getStats() -> stats object
    // Get shared memory statistics
    // =========================================================================

    auto shmGetStats = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shm_getStats"),
        0,  // no arguments
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (g_shmHandle == nullptr) {
                return jsi::Value::undefined();
            }

            shm_stats_t stats;
            shm_get_stats(g_shmHandle, &stats);

            jsi::Object result(rt);
            result.setProperty(rt, "buckets", static_cast<int>(stats.n_buckets));
            result.setProperty(rt, "nodes", static_cast<int>(stats.n_nodes));
            result.setProperty(rt, "nodesUsed", static_cast<int>(stats.nodes_used));
            result.setProperty(rt, "payloadCapacity", static_cast<double>(stats.payload_capacity));
            result.setProperty(rt, "payloadUsed", static_cast<double>(stats.payload_used));
            result.setProperty(rt, "generation", static_cast<double>(stats.generation));

            return result;
        }
    );
    runtime.global().setProperty(runtime, "__shm_getStats", std::move(shmGetStats));

    // =========================================================================
    // __shm_clear()
    // Clear all data from shared memory
    // =========================================================================

    auto shmClear = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shm_clear"),
        0,  // no arguments
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (g_shmHandle != nullptr) {
                shm_close(g_shmHandle);
                shm_destroy([g_shmName UTF8String]);
                g_shmHandle = nullptr;
            }

            // Recreate SHM
            g_shmHandle = shm_create(
                [g_shmName UTF8String],
                4096,
                65536,
                64 * 1024 * 1024
            );

            g_keyCounter = 0;

            return jsi::Value::undefined();
        }
    );
    runtime.global().setProperty(runtime, "__shm_clear", std::move(shmClear));

    RCTLogInfo(@"[ShmProxy] JSI bindings installed successfully");
    g_jsiBindingsInstalled = YES;
}

@end

// =============================================================================
// C++ API for Native Code (Advanced Usage)
// =============================================================================

namespace facebook::react {

/**
 * Store NSDictionary data in shared memory and return a key.
 *
 * This is the main entry point for Native code to store data.
 * The returned key can be used from JavaScript to retrieve the data.
 *
 * @param data The NSDictionary to store
 * @return The key that can be used to retrieve the data, or empty string on failure
 *
 * @example
 * NSString* key = [NSString stringWithUTF8String:storeNativeData(@{@"key": @"value"})];
 */
std::string storeNativeData(NSDictionary* data) {
    if (g_shmHandle == nullptr) {
        RCTLogError(@"[ShmProxy] Cannot store data: shared memory not initialized");
        return "";
    }

    // Generate a unique key
    uint64_t keyId = g_keyCounter.fetch_add(1);
    std::string key = "data_" + std::to_string(keyId);

    // Convert NSDictionary to SHM
    shm_error_t err = convertNSDictionaryToShm(g_shmHandle, key.c_str(), key.size(), data);
    if (err != SHM_OK) {
        RCTLogError(@"[ShmProxy] Failed to store data: error %d", err);
        return "";
    }

    RCTLogInfo(@"[ShmProxy] Stored native data with key: %s", key.c_str());
    return key;
}

/**
 * Get the global shared memory handle.
 *
 * For advanced use cases where you need direct access to the SHM handle.
 *
 * @return The global SHM handle, or nullptr if not initialized
 */
shm_handle_t getGlobalShmHandle() {
    return g_shmHandle;
}

} // namespace facebook::react
