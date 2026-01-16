#ifndef SHM_FLUTTER_C_API_H
#define SHM_FLUTTER_C_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "shm_kv_c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Flutter-specific wrapper for shared memory
// Uses the existing shm_kv_c_api implementation

// Create a shared memory region for Flutter
// Returns shared memory handle or NULL on failure
shm_handle_t shm_flutter_create(const char* name, size_t size);

// Close Flutter shared memory
void shm_flutter_close(shm_handle_t handle);

// Write data to shared memory (wrapper around shm_insert)
// Returns SHM_OK on success
shm_error_t shm_flutter_write(shm_handle_t handle,
                              const char* key,
                              size_t key_len,
                              const uint8_t* data,
                              size_t data_len);

// Read data from shared memory (wrapper around shm_lookup_copy)
// Caller must provide buffer
// Returns SHM_OK on success
shm_error_t shm_flutter_read(shm_handle_t handle,
                             const char* key,
                             size_t key_len,
                             uint8_t* buffer,
                             size_t buffer_size,
                             size_t* actual_size);

// Delete data from shared memory
// Note: This marks the key as deleted (lazy deletion)
// Returns SHM_OK on success
shm_error_t shm_flutter_delete(shm_handle_t handle,
                               const char* key,
                               size_t key_len);

// Clear all data in shared memory
void shm_flutter_clear(shm_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif // SHM_FLUTTER_C_API_H
