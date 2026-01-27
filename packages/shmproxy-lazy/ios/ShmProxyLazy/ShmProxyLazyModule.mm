/**
 * ShmProxyLazyModule.mm
 *
 * Implementation of Lazy Loading using ES6 Proxy pattern.
 *
 * Key differences from ShmProxyModule:
 * 1. Does NOT use HostObject (avoids limitations)
 * 2. Provides fine-grained access APIs for JS Proxy handler
 * 3. Lazy loading: only convert fields when accessed
 * 4. Independent module to avoid breaking existing code
 */

#import "ShmProxyLazyModule.h"
#import "NSDictionaryToShm.h"
#import "shm_kv_c_api.h"
#import <React/RCTBridge+Private.h>
#import <React/RCTUtils.h>
#import <jsi/jsi.h>
#include <sstream>

using namespace facebook;

// =============================================================================
// Global Shared Memory Handle
// =============================================================================

static shm_handle_t g_lazyShmHandle = nullptr;
static NSString* g_lazyShmName = @"/rn_shm_lazy";
static std::atomic<uint64_t> g_lazyKeyCounter{0};

// =============================================================================
// Get the active SHM handle
// =============================================================================

static shm_handle_t getActiveShmHandle() {
    // Return Lazy's own SHM
    return g_lazyShmHandle;
}

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * Parse field path string like "nested.deep.value" into components
 */
static std::vector<std::string> parseFieldPath(const std::string& path) {
    std::vector<std::string> components;
    std::stringstream ss(path);
    std::string component;
    while (std::getline(ss, component, '.')) {
        if (!component.empty()) {
            components.push_back(component);
        }
    }
    return components;
}

/**
 * Navigate through nested object views following a path
 */
static shm_error_t navigateObjectPath(shm_handle_t handle,
                                      const std::string& key,
                                      const std::vector<std::string>& path,
                                      shm_typed_value_view_t* outView) {
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(handle, key.c_str(), key.size(), &type);
    if (err != SHM_OK || type != SHM_TYPE_OBJECT) {
        return SHM_ERR_NOT_FOUND;
    }

    // Get top-level object view
    shm_object_view_t objView;
    err = shm_lookup_object(handle, key.c_str(), key.size(), &objView);
    if (err != SHM_OK) {
        return err;
    }

    // Start with top-level object
    shm_object_view_t* currentObjView = &objView;

    // Navigate through path
    for (size_t i = 0; i < path.size(); ++i) {
        const std::string& fieldName = path[i];

        // Get the field from current object
        shm_typed_value_view_t fieldValue;
        err = shm_object_get_field(currentObjView, fieldName.c_str(),
                                   fieldName.size(), &fieldValue);
        if (err != SHM_OK) {
            return err;
        }

        // If this is the last component, we're done
        if (i == path.size() - 1) {
            *outView = fieldValue;
            return SHM_OK;
        }

        // Otherwise, if it's not an object, we can't continue
        if (fieldValue.type != SHM_TYPE_OBJECT) {
            return SHM_ERR_TYPE_MISMATCH;
        }

        // Parse nested object view from the field payload
        const uint8_t* nestedPtr = static_cast<const uint8_t*>(fieldValue.payload);
        uint32_t nestedCount = *reinterpret_cast<const uint32_t*>(nestedPtr);

        // Create a persistent view (on heap) for the nested object
        // Note: This is a simplified approach - we're reading directly from SHM data
        // In production, you'd want to track this memory properly
        static shm_object_view_t nestedObjViews[16];  // Support up to 16 levels of nesting
        static int nestedLevel = 0;

        if (nestedLevel >= 16) {
            return SHM_ERR_NOT_FOUND;  // Too deep
        }

        shm_object_view_t* nestedView = &nestedObjViews[nestedLevel++];
        nestedView->count = nestedCount;
        nestedView->name_offsets = reinterpret_cast<const uint32_t*>(nestedPtr + 4);
        nestedView->names_data = reinterpret_cast<const char*>(nestedPtr + 4 + 4 * (nestedCount + 1));
        nestedView->field_types = reinterpret_cast<const uint8_t*>(
            nestedView->names_data + nestedView->name_offsets[nestedCount]);

        size_t nestedAfterTypesOffset = 4 + 4 * (nestedCount + 1) + nestedView->name_offsets[nestedCount] + nestedCount;
        nestedAfterTypesOffset = (nestedAfterTypesOffset + 3) & ~3;
        const uint8_t* nestedAfterTypes = nestedPtr + nestedAfterTypesOffset;

        nestedView->value_offsets = reinterpret_cast<const uint32_t*>(nestedAfterTypes);
        nestedView->values_data = reinterpret_cast<const uint8_t*>(nestedAfterTypes + 4 * (nestedCount + 1));

        // Update currentObjView for next iteration
        currentObjView = nestedView;
    }

    return SHM_OK;
}

// =============================================================================
// ShmProxyLazyModule Implementation
// =============================================================================

@implementation ShmProxyLazyModule

RCT_EXPORT_MODULE(ShmProxyLazy)

+ (BOOL)requiresMainQueueSetup {
    return YES;
}

// Global bridge reference (like BenchmarkModule)
static RCTBridge *g_lazyBridge = nullptr;

// Synchronous method to install JSI bindings (called from JS)
RCT_EXPORT_BLOCKING_SYNCHRONOUS_METHOD(installJSIBindingsSync) {
    if (g_lazyBridge == nullptr) {
        g_lazyBridge = [RCTBridge currentBridge];
    }

    if (g_lazyBridge == nullptr) {
        NSLog(@"[ShmProxyLazy] Bridge not available");
        return @NO;
    }

    RCTCxxBridge *cxxBridge = (RCTCxxBridge *)g_lazyBridge;
    if (!cxxBridge.runtime) {
        NSLog(@"[ShmProxyLazy] Runtime not available");
        return @NO;
    }

    // Install JSI bindings - use proper Runtime cast
    facebook::jsi::Runtime &runtime = *(facebook::jsi::Runtime *)cxxBridge.runtime;
    [ShmProxyLazyModule installWithRuntime:runtime];
    NSLog(@"[ShmProxyLazy] JSI bindings installed successfully");
    return @YES;
}

// =============================================================================
// Native Module Methods
// =============================================================================

RCT_EXPORT_METHOD(initialize:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    if (g_lazyShmHandle != nullptr) {
        resolve(@(YES));
        return;
    }

    // Try to create shared memory
    g_lazyShmHandle = shm_create(
        [g_lazyShmName UTF8String],
        4096,
        65536,
        64 * 1024 * 1024
    );

    if (g_lazyShmHandle != nullptr) {
        NSLog(@"[ShmProxyLazy] Shared memory initialized");
        resolve(@(YES));
    } else {
        NSLog(@"[ShmProxyLazy] Failed to initialize shared memory");
        reject(@"INIT_ERROR", @"Failed to initialize shared memory", nil);
    }
}

RCT_EXPORT_METHOD(writeData:(NSDictionary *)data
                  resolve:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    if (g_lazyShmHandle == nullptr) {
        reject(@"NOT_INITIALIZED", @"Shared memory not initialized", nil);
        return;
    }

    // Generate unique key
    uint64_t keyId = g_lazyKeyCounter.fetch_add(1);
    std::string key = "lazy_data_" + std::to_string(keyId);

    // Convert NSDictionary to SHM
    shm_handle_t activeHandle = getActiveShmHandle();
    shm_error_t err = convertNSDictionaryToShm(activeHandle, key.c_str(), key.size(), data);
    if (err != SHM_OK) {
        NSLog(@"[ShmProxyLazy] Failed to write data: error %d", err);
        reject(@"WRITE_ERROR", @"Failed to write data to shared memory", nil);
        return;
    }

    NSString* keyString = [NSString stringWithUTF8String:key.c_str()];
    NSLog(@"[ShmProxyLazy] Data written with key: %@", keyString);
    resolve(keyString);
}

RCT_EXPORT_METHOD(isInitialized:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    BOOL initialized = (g_lazyShmHandle != nullptr);
    resolve(@(initialized));
}

RCT_EXPORT_METHOD(close:(RCTPromiseResolveBlock)resolve
                  reject:(RCTPromiseRejectBlock)reject) {
    if (g_lazyShmHandle != nullptr) {
        shm_close(g_lazyShmHandle);
        shm_destroy([g_lazyShmName UTF8String]);
        g_lazyShmHandle = nullptr;
        NSLog(@"[ShmProxyLazy] Shared memory closed");
    }
    resolve(@(YES));
}

// =============================================================================
// JSI Bindings Installation
// =============================================================================

+ (void)installWithRuntime:(jsi::Runtime&)runtime {

    NSLog(@"[ShmProxyLazy] Installing JSI bindings...");

    // =========================================================================
    // __shmProxyLazy_getField(key, fieldPath) -> value
    // Get a single field from SHM (supports nested paths like "nested.value")
    // =========================================================================

    auto getField = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxyLazy_getField"),
        2,  // key, fieldPath
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 2) {
                throw jsi::JSError(rt, "getField requires key and fieldPath arguments");
            }

            std::string key = args[0].asString(rt).utf8(rt);
            std::string fieldPath = args[1].asString(rt).utf8(rt);

            NSLog(@"[__shmProxyLazy_getField] Called with key='%s' fieldPath='%s'",
                  key.c_str(), fieldPath.c_str());

            shm_handle_t activeHandle = getActiveShmHandle();
            if (activeHandle == nullptr) {
                NSLog(@"[__shmProxyLazy_getField] ERROR: Shared memory not initialized");
                throw jsi::JSError(rt, "Shared memory not initialized");
            }

            // Parse field path
            std::vector<std::string> pathComponents = parseFieldPath(fieldPath);
            NSLog(@"[__shmProxyLazy_getField] Path components: %zu", pathComponents.size());

            if (pathComponents.empty()) {
                NSLog(@"[__shmProxyLazy_getField] ERROR: Empty path");
                return jsi::Value::undefined();
            }

            // Navigate to the field
            shm_typed_value_view_t fieldView;
            shm_error_t err = navigateObjectPath(activeHandle, key, pathComponents, &fieldView);
            NSLog(@"[__shmProxyLazy_getField] navigateObjectPath result: %d", err);

            if (err != SHM_OK) {
                NSLog(@"[__shmProxyLazy_getField] ERROR: navigateObjectPath failed for path='%s'", fieldPath.c_str());
                return jsi::Value::undefined();
            }

            NSLog(@"[__shmProxyLazy_getField] Field type: %d", fieldView.type);

            // Convert to JS value based on type
            switch (fieldView.type) {
                case SHM_TYPE_NULL:
                    return jsi::Value::null();

                case SHM_TYPE_INT_SCALAR: {
                    int64_t value = *static_cast<const int64_t*>(fieldView.payload);
                    return jsi::Value(static_cast<double>(value));
                }

                case SHM_TYPE_FLOAT_SCALAR: {
                    double value = *static_cast<const double*>(fieldView.payload);
                    return jsi::Value(value);
                }

                case SHM_TYPE_BOOL_SCALAR: {
                    uint8_t value = *static_cast<const uint8_t*>(fieldView.payload);
                    return jsi::Value(value != 0);
                }

                case SHM_TYPE_STRING: {
                    const uint8_t* ptr = static_cast<const uint8_t*>(fieldView.payload);
                    uint32_t strLen = *reinterpret_cast<const uint32_t*>(ptr);
                    const uint8_t* strData = ptr + 4;
                    return jsi::String::createFromUtf8(rt, strData, strLen);
                }

                // For nested objects and arrays, return a marker object
                // The JS Proxy handler will create nested Proxies
                case SHM_TYPE_OBJECT:
                case SHM_TYPE_LIST: {
                    jsi::Object marker(rt);
                    marker.setProperty(rt, "__shmProxyNested",
                        jsi::String::createFromUtf8(rt, fieldPath));
                    marker.setProperty(rt, "__shmProxyType",
                        jsi::String::createFromUtf8(rt,
                            fieldView.type == SHM_TYPE_OBJECT ? "object" : "array"));
                    return marker;
                }

                default:
                    return jsi::Value::undefined();
            }
        }
    );
    runtime.global().setProperty(runtime, "__shmProxyLazy_getField", std::move(getField));

    // =========================================================================
    // __shmProxyLazy_getKeys(key) -> string[]
    // Get all field names for an object
    // =========================================================================

    auto getKeys = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxyLazy_getKeys"),
        1,  // key
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1) {
                throw jsi::JSError(rt, "getKeys requires key argument");
            }

            std::string key = args[0].asString(rt).utf8(rt);

            NSLog(@"[__shmProxyLazy_getKeys] Called with key='%s'", key.c_str());

            shm_handle_t activeHandle = getActiveShmHandle();
            if (activeHandle == nullptr) {
                NSLog(@"[__shmProxyLazy_getKeys] ERROR: Shared memory not initialized");
                throw jsi::JSError(rt, "Shared memory not initialized");
            }

            // Get object view
            shm_object_view_t objView;
            shm_error_t err = shm_lookup_object(activeHandle, key.c_str(), key.size(), &objView);

            NSLog(@"[__shmProxyLazy_getKeys] shm_lookup_object result: %d", err);

            if (err != SHM_OK) {
                NSLog(@"[__shmProxyLazy_getKeys] ERROR: shm_lookup_object failed");
                return jsi::Value::undefined();
            }

            NSLog(@"[__shmProxyLazy_getKeys] Found %u fields", objView.count);

            // Create array of field names
            jsi::Array keysArray = jsi::Array(rt, objView.count);
            for (uint32_t i = 0; i < objView.count; ++i) {
                uint32_t start = objView.name_offsets[i];
                uint32_t end = objView.name_offsets[i + 1];
                std::string fieldName(objView.names_data + start, end - start);
                keysArray.setValueAtIndex(rt, i, jsi::String::createFromUtf8(rt, fieldName));
            }

            return keysArray;
        }
    );
    runtime.global().setProperty(runtime, "__shmProxyLazy_getKeys", std::move(getKeys));

    // =========================================================================
    // __shmProxyLazy_materialize(key) -> object
    // Fully materialize an object (convert all fields to JS)
    // =========================================================================

    auto materialize = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxyLazy_materialize"),
        1,  // key
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            if (count < 1) {
                throw jsi::JSError(rt, "materialize requires key argument");
            }

            std::string key = args[0].asString(rt).utf8(rt);

            shm_handle_t activeHandle = getActiveShmHandle();
            if (activeHandle == nullptr) {
                throw jsi::JSError(rt, "Shared memory not initialized");
            }

            // Get object view
            shm_object_view_t objView;
            shm_error_t err = shm_lookup_object(activeHandle, key.c_str(), key.size(), &objView);
            if (err != SHM_OK) {
                return jsi::Value::undefined();
            }

            NSLog(@"[__shmProxyLazy_materialize] Called with key='%s', fields=%u", key.c_str(), objView.count);

            // TODO: Implement full materialization
            // For now, return empty object as placeholder
            // In production, you would implement recursive SHM to JS object conversion
            jsi::Object result(rt);
            NSLog(@"[__shmProxyLazy_materialize] Conversion complete (placeholder)");

            return result;
        }
    );
    runtime.global().setProperty(runtime, "__shmProxyLazy_materialize", std::move(materialize));

    // =========================================================================
    // __shmProxyLazy_getStats() -> stats object
    // Get shared memory statistics
    // =========================================================================

    auto getStats = jsi::Function::createFromHostFunction(
        runtime,
        jsi::PropNameID::forAscii(runtime, "__shmProxyLazy_getStats"),
        0,
        [](jsi::Runtime& rt,
           const jsi::Value& thisVal,
           const jsi::Value* args,
           size_t count) -> jsi::Value {

            shm_handle_t activeHandle = getActiveShmHandle();
            if (activeHandle == nullptr) {
                return jsi::Value::undefined();
            }

            shm_stats_t stats;
            shm_get_stats(activeHandle, &stats);

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
    runtime.global().setProperty(runtime, "__shmProxyLazy_getStats", std::move(getStats));

    NSLog(@"[ShmProxyLazy] JSI bindings installed successfully");
}

@end

// =============================================================================
// C++ API for Native Code (if needed)
// =============================================================================

namespace facebook::react {

/**
 * Store NSDictionary in shared memory and return key
 */
std::string storeLazyData(NSDictionary* data) {
    shm_handle_t activeHandle = getActiveShmHandle();
    if (activeHandle == nullptr) {
        NSLog(@"[ShmProxyLazy] Cannot store data: shared memory not initialized");
        return "";
    }

    uint64_t keyId = g_lazyKeyCounter.fetch_add(1);
    std::string key = "lazy_data_" + std::to_string(keyId);

    shm_error_t err = convertNSDictionaryToShm(g_lazyShmHandle, key.c_str(), key.size(), data);
    if (err != SHM_OK) {
        NSLog(@"[ShmProxyLazy] Failed to store data: error %d", err);
        return "";
    }

    return key;
}

/**
 * Get global SHM handle
 */
shm_handle_t getLazyShmHandle() {
    return g_lazyShmHandle;
}

} // namespace facebook::react
