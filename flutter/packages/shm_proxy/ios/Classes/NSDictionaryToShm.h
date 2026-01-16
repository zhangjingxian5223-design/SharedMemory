/**
 * NSDictionaryToShm.h
 *
 * Converts NSDictionary/NSArray to shared memory format.
 * This is the key component that replaces the traditional
 * NSDictionary → folly::dynamic → jsi::Value conversion.
 */

#pragma once

#import <Foundation/Foundation.h>
#include "shm_kv_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert an NSDictionary to shared memory OBJECT format.
 *
 * @param handle The shm handle
 * @param key The key to store the data under
 * @param keyLen Length of the key
 * @param dict The NSDictionary to convert
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertNSDictionaryToShm(shm_handle_t handle,
                                      const char* key,
                                      size_t keyLen,
                                      NSDictionary* dict);

/**
 * Convert an NSArray to shared memory LIST format.
 *
 * @param handle The shm handle
 * @param key The key to store the data under
 * @param keyLen Length of the key
 * @param array The NSArray to convert
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertNSArrayToShm(shm_handle_t handle,
                                 const char* key,
                                 size_t keyLen,
                                 NSArray* array);

#ifdef __cplusplus
}
#endif
