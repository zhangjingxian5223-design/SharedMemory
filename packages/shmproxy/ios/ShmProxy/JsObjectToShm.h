/**
 * JsObjectToShm.h
 *
 * Converts jsi::Object to shared memory format.
 * This is the reverse of ShmProxyObject - it takes JS objects
 * and writes them to shared memory for Native to read.
 *
 * Data flow: JS Object -> jsi::Object -> SharedMemory -> NSDictionary
 */

#pragma once

#include <jsi/jsi.h>
#include "shm_kv_c_api.h"
#include <string>

namespace facebook::react {

/**
 * Convert a jsi::Object to shared memory OBJECT format.
 *
 * @param rt The JSI runtime
 * @param handle The shm handle
 * @param key The key to store the data under
 * @param obj The jsi::Object to convert
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertJsObjectToShm(jsi::Runtime& rt,
                                  shm_handle_t handle,
                                  const std::string& key,
                                  const jsi::Object& obj);

/**
 * Convert a jsi::Array to shared memory LIST format.
 *
 * @param rt The JSI runtime
 * @param handle The shm handle
 * @param key The key to store the data under
 * @param arr The jsi::Array to convert
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertJsArrayToShm(jsi::Runtime& rt,
                                 shm_handle_t handle,
                                 const std::string& key,
                                 const jsi::Array& arr);

} // namespace facebook::react
