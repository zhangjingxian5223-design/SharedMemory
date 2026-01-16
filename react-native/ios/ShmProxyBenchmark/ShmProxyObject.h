/**
 * ShmProxyObject.h
 *
 * JSI HostObject that provides lazy access to shared memory data.
 * When JS accesses properties, this object intercepts the access and
 * reads data from shared memory on-demand.
 */

#pragma once

#include <jsi/jsi.h>
#include <memory>
#include <string>
#include <vector>
#include "shm_kv_c_api.h"

namespace facebook::react {

/**
 * ShmProxyObject - A JSI HostObject that lazily reads from shared memory
 *
 * Usage:
 *   1. Native writes data to shm using shm_insert_object()
 *   2. JS receives a ShmProxyObject handle
 *   3. When JS accesses song.segments[0].pitches, the proxy intercepts
 *      and reads only that specific data from shm
 */
class ShmProxyObject : public jsi::HostObject {
public:
    /**
     * Create a proxy for a top-level shm key
     * @param handle The shm handle
     * @param key The key in shm that this proxy represents
     */
    ShmProxyObject(shm_handle_t handle, const std::string& key);

    /**
     * Create a proxy for a nested object field
     * @param handle The shm handle
     * @param objectView The parent object view
     * @param fieldIndex The index of this field in the parent
     */
    ShmProxyObject(shm_handle_t handle,
                   const shm_object_view_t& objectView,
                   size_t fieldIndex);

    /**
     * Create a proxy from a typed value view (for nested access)
     */
    ShmProxyObject(shm_handle_t handle,
                   const shm_typed_value_view_t& typedView);

    ~ShmProxyObject() override = default;

    // JSI HostObject interface
    jsi::Value get(jsi::Runtime& rt, const jsi::PropNameID& name) override;
    std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& rt) override;

    // Static method to convert top-level shm object to plain JS object
    // This is needed because Object.values() doesn't work correctly with HostObjects
    static jsi::Value convertTopLevelToJsObject(jsi::Runtime& rt,
                                                  shm_handle_t handle,
                                                  const std::string& key);

private:
    shm_handle_t handle_;
    std::string key_;  // For top-level access

    // For nested object access
    bool isNestedObject_ = false;
    shm_object_view_t objectView_;

    // For typed value access
    bool isTypedValue_ = false;
    shm_typed_value_view_t typedView_;

    // Convert shm typed value to jsi::Value
    jsi::Value convertTypedValueToJsi(jsi::Runtime& rt,
                                       const shm_typed_value_view_t& view);

    // Convert primitive types
    jsi::Value convertScalarToJsi(jsi::Runtime& rt,
                                   shm_value_type_t type,
                                   const void* payload,
                                   size_t len);

    // Convert array types
    jsi::Value convertVectorToJsi(jsi::Runtime& rt,
                                   shm_value_type_t type,
                                   const void* payload,
                                   size_t len);
};

/**
 * ShmProxyArray - A JSI HostObject for array/list access
 *
 * Handles indexed access like segments[0]
 */
class ShmProxyArray : public jsi::HostObject {
public:
    /**
     * Create a proxy for a LIST type
     */
    ShmProxyArray(shm_handle_t handle, const shm_list_view_t& listView);

    /**
     * Create a proxy for a float vector (returns actual values)
     */
    ShmProxyArray(shm_handle_t handle,
                  const double* data,
                  size_t count);

    /**
     * Create a proxy for an int vector
     */
    ShmProxyArray(shm_handle_t handle,
                  const int64_t* data,
                  size_t count);

    ~ShmProxyArray() override = default;

    jsi::Value get(jsi::Runtime& rt, const jsi::PropNameID& name) override;
    std::vector<jsi::PropNameID> getPropertyNames(jsi::Runtime& rt) override;

private:
    shm_handle_t handle_;

    // For LIST type
    bool isList_ = false;
    shm_list_view_t listView_;

    // For float vector
    bool isFloatVector_ = false;
    const double* floatData_ = nullptr;
    size_t floatCount_ = 0;

    // For int vector
    bool isIntVector_ = false;
    const int64_t* intData_ = nullptr;
    size_t intCount_ = 0;
};

} // namespace facebook::react
