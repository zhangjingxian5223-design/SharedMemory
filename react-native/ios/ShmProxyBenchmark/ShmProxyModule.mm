/**
 * ShmProxyModule.mm
 *
 * Implementation of the TurboModule for shared memory proxy.
 * Handles:
 *   1. NSDictionary → shm conversion
 *   2. Creating ShmProxyObject handles for JS
 *   3. Lifecycle management of shared memory
 */

#import "ShmProxyModule.h"
#import "ShmProxyObject.h"
#import "NSDictionaryToShm.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTUtils.h>
#import <jsi/jsi.h>

using namespace facebook;

// Global shared memory handle (managed by the module)
static shm_handle_t g_shmHandle = nullptr;
static NSString* g_shmName = @"/rn_shm_proxy";

// Counter for generating unique keys
static std::atomic<uint64_t> g_keyCounter{0};

@implementation ShmProxyModule

RCT_EXPORT_MODULE(ShmProxy)

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

- (instancetype)init {
    if (self = [super init]) {
        // Initialize shared memory on first use
        if (g_shmHandle == nullptr) {
            g_shmHandle = shm_create(
                [g_shmName UTF8String],
                4096,      // n_buckets
                65536,     // n_nodes
                64 * 1024 * 1024  // 64MB payload
            );

            if (g_shmHandle == nullptr) {
                NSLog(@"[ShmProxyModule] Failed to create shared memory");
            } else {
                NSLog(@"[ShmProxyModule] Shared memory created successfully");
            }
        }
    }
    return self;
}

- (void)invalidate {
    // Clean up shared memory when the module is invalidated
    if (g_shmHandle != nullptr) {
        shm_close(g_shmHandle);
        shm_destroy([g_shmName UTF8String]);
        g_shmHandle = nullptr;
        NSLog(@"[ShmProxyModule] Shared memory destroyed");
    }
}

+ (void)installWithRuntime:(jsi::Runtime&)runtime
             callInvoker:(std::shared_ptr<react::CallInvoker>)callInvoker {

    // Install global function: __shmProxy_storeData(data) -> handle
    auto storeData = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxy_storeData"),
        1,  // 1 argument
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1 || !args[0].isObject()) {
                throw jsi::JSError(rt, "storeData requires an object argument");
            }

            // This function is called from JS with a plain object
            // In practice, the Native side should call storeNativeData instead
            // This is a placeholder for JS-side testing
            return jsi::Value::undefined();
        }
    );
    runtime.global().setProperty(runtime, "__shmProxy_storeData", std::move(storeData));

    // Install global function: __shmProxy_getData(key) -> proxy
    auto getData = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxy_getData"),
        1,
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1 || !args[0].isString()) {
                throw jsi::JSError(rt, "getData requires a string key argument");
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

            // Create and return a proxy object
            auto proxy = std::make_shared<react::ShmProxyObject>(g_shmHandle, key);
            return jsi::Object::createFromHostObject(rt, proxy);
        }
    );
    runtime.global().setProperty(runtime, "__shmProxy_getData", std::move(getData));

    // Install global function: __shmProxy_getStats() -> stats object
    auto getStats = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxy_getStats"),
        0,
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
    runtime.global().setProperty(runtime, "__shmProxy_getStats", std::move(getStats));

    NSLog(@"[ShmProxyModule] JSI bindings installed");
}

@end

// ============================================================================
// C++ API for Native code to store data
// ============================================================================

namespace facebook::react {

/**
 * Store NSDictionary data in shared memory and return a key.
 * This is the main entry point for Native code.
 *
 * @param data The NSDictionary to store
 * @return The key that can be used to retrieve the data, or empty string on failure
 */
std::string storeNativeData(NSDictionary* data) {
    if (g_shmHandle == nullptr) {
        NSLog(@"[ShmProxyModule] Cannot store data: shared memory not initialized");
        return "";
    }

    // Generate a unique key
    uint64_t keyId = g_keyCounter.fetch_add(1);
    std::string key = "data_" + std::to_string(keyId);

    // Convert NSDictionary to shm
    shm_error_t err = convertNSDictionaryToShm(g_shmHandle, key.c_str(), key.size(), data);
    if (err != SHM_OK) {
        NSLog(@"[ShmProxyModule] Failed to store data: error %d", err);
        return "";
    }

    return key;
}

/**
 * Get the global shm handle (for advanced use cases)
 */
shm_handle_t getGlobalShmHandle() {
    return g_shmHandle;
}

} // namespace facebook::react
