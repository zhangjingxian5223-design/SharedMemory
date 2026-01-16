/**
 * ShmToNSDictionary.h
 *
 * Converts shared memory data back to NSDictionary/NSArray.
 * This is the reverse of NSDictionaryToShm - it reads from shared memory
 * and creates Objective-C objects for Native code to use.
 *
 * Data flow: SharedMemory -> NSDictionary/NSArray
 */

#pragma once

#import <Foundation/Foundation.h>
#include "shm_kv_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert shared memory OBJECT to NSDictionary.
 *
 * @param handle The shm handle
 * @param key The key to read from
 * @param keyLen Length of the key
 * @param outDict Output pointer for the resulting NSDictionary (autoreleased)
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertShmToNSDictionary(shm_handle_t handle,
                                      const char* key,
                                      size_t keyLen,
                                      NSDictionary** outDict);

/**
 * Convert shared memory LIST to NSArray.
 *
 * @param handle The shm handle
 * @param key The key to read from
 * @param keyLen Length of the key
 * @param outArray Output pointer for the resulting NSArray (autoreleased)
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertShmToNSArray(shm_handle_t handle,
                                 const char* key,
                                 size_t keyLen,
                                 NSArray** outArray);

/**
 * Convert any shared memory value to the appropriate Objective-C object.
 * Automatically detects the type and returns NSDictionary, NSArray, NSNumber,
 * NSString, or NSNull as appropriate.
 *
 * @param handle The shm handle
 * @param key The key to read from
 * @param keyLen Length of the key
 * @param outValue Output pointer for the resulting object (autoreleased)
 * @return SHM_OK on success, error code on failure
 */
shm_error_t convertShmToNSObject(shm_handle_t handle,
                                  const char* key,
                                  size_t keyLen,
                                  id* outValue);

#ifdef __cplusplus
}
#endif
