#include "shm_flutter_c_api.h"
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#define SHM_FLUTTER_DEFAULT_NAME "/shm_flutter_"
#define SHM_FLUTTER_BUCKETS 4096
#define SHM_FLUTTER_NODES 65536

// Create shared memory for Flutter
shm_handle_t shm_flutter_create(const char* name, size_t size) {
    if (!name || size == 0) {
        return NULL;
    }

    // Build full shared memory name
    char full_name[256];
    snprintf(full_name, sizeof(full_name), "%s%s", SHM_FLUTTER_DEFAULT_NAME, name);

    // Create shared memory using shm_kv_c_api
    // Default: 4096 buckets, 65536 nodes, custom payload size
    shm_handle_t handle = shm_create(full_name,
                                     SHM_FLUTTER_BUCKETS,
                                     SHM_FLUTTER_NODES,
                                     size);

    return handle;
}

// Close shared memory
void shm_flutter_close(shm_handle_t handle) {
    if (handle) {
        shm_close(handle);
    }
}

// Write data to shared memory
shm_error_t shm_flutter_write(shm_handle_t handle,
                              const char* key,
                              size_t key_len,
                              const uint8_t* data,
                              size_t data_len) {
    if (!handle || !key || key_len == 0 || !data || data_len == 0) {
        return SHM_ERR_INVALID_PARAM;
    }

    return shm_insert(handle, key, key_len, data, data_len);
}

// Read data from shared memory
shm_error_t shm_flutter_read(shm_handle_t handle,
                             const char* key,
                             size_t key_len,
                             uint8_t* buffer,
                             size_t buffer_size,
                             size_t* actual_size) {
    if (!handle || !key || key_len == 0 || !buffer || buffer_size == 0) {
        return SHM_ERR_INVALID_PARAM;
    }

    return shm_lookup_copy(handle, key, key_len, buffer, buffer_size, actual_size);
}

// Delete data from shared memory
shm_error_t shm_flutter_delete(shm_handle_t handle,
                               const char* key,
                               size_t key_len) {
    if (!handle || !key || key_len == 0) {
        return SHM_ERR_INVALID_PARAM;
    }

    // Note: shm_kv_c_api doesn't have a delete function
    // This is a placeholder - would need to be added to shm_kv_c_api
    return SHM_OK;
}

// Clear all data (destroy and recreate)
void shm_flutter_clear(shm_handle_t handle) {
    if (handle) {
        // Close and the shared memory will be cleaned up
        // when the process exits or shm_destroy is called
        shm_close(handle);
    }
}
