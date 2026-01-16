#ifndef SHM_KV_C_API_H
#define SHM_KV_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle for shared memory
typedef void* shm_handle_t;

// Error codes
typedef enum {
    SHM_OK = 0,
    SHM_ERR_NOT_FOUND = 1,
    SHM_ERR_NO_SPACE = 2,
    SHM_ERR_CONCURRENT_MOD = 3,
    SHM_ERR_INVALID_PARAM = 4,
    SHM_ERR_OPEN_FAILED = 5,
    SHM_ERR_PERMISSION_DENIED = 6,
    SHM_ERR_TYPE_MISMATCH = 7,
} shm_error_t;

// Value type enumeration for zero-copy optimization
typedef enum {
    SHM_TYPE_UNKNOWN = 0,
    SHM_TYPE_INT_SCALAR = 1,     // int64_t scalar
    SHM_TYPE_FLOAT_SCALAR = 2,   // double scalar
    SHM_TYPE_STRING = 3,         // string (raw bytes)
    SHM_TYPE_INT_VECTOR = 4,     // int64_t vector
    SHM_TYPE_FLOAT_VECTOR = 5,   // double vector
    SHM_TYPE_INT_MATRIX = 6,     // int64_t matrix
    SHM_TYPE_FLOAT_MATRIX = 7,   // double matrix
    SHM_TYPE_INT_SET = 8,        // set of int64_t (sorted)
    SHM_TYPE_FLOAT_SET = 9,      // set of double (sorted)
    SHM_TYPE_STRING_SET = 10,    // set of strings (sorted)
    SHM_TYPE_DICT_STR_INT = 11,  // Dict[str, int64]
    SHM_TYPE_DICT_STR_FLOAT = 12,// Dict[str, double]
    SHM_TYPE_STRING_VECTOR = 13, // list/vector of strings (preserve order)
    SHM_TYPE_BYTES = 14,         // raw bytes blob
    SHM_TYPE_DICT_STR_STRING = 15,// Dict[str, string]
    SHM_TYPE_BOOL_SCALAR = 16,    // bool scalar
    SHM_TYPE_BOOL_VECTOR = 17,    // list/vector of bool (byte-packed)
    SHM_TYPE_OBJECT = 18,         // dict/object (string-keyed), recursive typed payload
    SHM_TYPE_LIST = 19,           // heterogeneous list, recursive typed payload
    SHM_TYPE_DICT_STR_FLOAT_VECTOR = 20, // Dict[str, list/double-vector] (sorted by key)
    SHM_TYPE_DICT_STR_FLOAT_MATRIX = 21, // Dict[str, double-matrix] (sorted by key)
    SHM_TYPE_DICT_STR_STRING_VECTOR = 22, // Dict[str, list[string]] (sorted by key)
    SHM_TYPE_DICT_STR_BOOL = 23,   // Dict[str, bool] (sorted by key)
    SHM_TYPE_DICT_STR_BYTES = 24,  // Dict[str, bytes] (sorted by key)
    SHM_TYPE_NULL = 25,            // null/nil value (no payload)
    SHM_TYPE_COMPLEX = 99,       // complex type (needs structured_codec)
} shm_value_type_t;

// Create or open shared memory
// Returns NULL on failure
//
// Auto-cleanup behavior:
//   Set environment variable SHM_AUTO_CLEANUP=1 to enable automatic cleanup
//   on process exit (normal exit, Ctrl+C, SIGTERM, SIGHUP)
//   When enabled, shm_unlink will be called automatically for the FIRST
//   shared memory created by this process.
shm_handle_t shm_create(const char* name,
                        size_t n_buckets,
                        size_t n_nodes,
                        size_t payload_size);

// Insert key-value pair
// Returns SHM_OK on success
shm_error_t shm_insert(shm_handle_t handle,
                       const void* key,
                       size_t key_len,
                       const void* value,
                       size_t value_len);

// Lookup key-value pair
// out_value will point to shared memory (zero-copy, read-only!)
// out_value_len will contain the value length
// Returns SHM_OK if found, SHM_ERR_NOT_FOUND if not found
shm_error_t shm_lookup(shm_handle_t handle,
                       const void* key,
                       size_t key_len,
                       const void** out_value,
                       size_t* out_value_len);

// Lookup with copy (safer for cross-language use)
// Copies value to user-provided buffer
// Returns SHM_OK if found and copied, SHM_ERR_NO_SPACE if buffer too small
shm_error_t shm_lookup_copy(shm_handle_t handle,
                            const void* key,
                            size_t key_len,
                            void* out_buffer,
                            size_t buffer_size,
                            size_t* out_value_len);

// Get statistics
typedef struct {
    uint32_t n_buckets;
    uint32_t n_nodes;
    uint32_t nodes_used;
    uint64_t payload_capacity;
    uint64_t payload_used;
    uint64_t generation;
} shm_stats_t;

void shm_get_stats(shm_handle_t handle, shm_stats_t* stats);

// Close shared memory handle
void shm_close(shm_handle_t handle);

// Destroy shared memory (removes from system)
void shm_destroy(const char* name);

// ============================================================================
// Zero-Copy Typed Insert/Lookup (Performance Optimized)
// ============================================================================

// Insert int64_t scalar (zero-copy optimized)
shm_error_t shm_insert_int_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int64_t value);

// Insert double scalar (zero-copy optimized)
shm_error_t shm_insert_float_scalar(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     double value);

// Insert int64_t vector (zero-copy optimized)
shm_error_t shm_insert_int_vector(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const int64_t* values,
                                   size_t count);

// Insert double vector (zero-copy optimized)
shm_error_t shm_insert_float_vector(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const double* values,
                                     size_t count);

// Get value type for a key
shm_error_t shm_get_value_type(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                shm_value_type_t* out_type);

// Lookup int64_t scalar (zero-copy)
shm_error_t shm_lookup_int_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int64_t* out_value);

// Lookup double scalar (zero-copy)
shm_error_t shm_lookup_float_scalar(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     double* out_value);

// Insert bool scalar (zero-copy)
shm_error_t shm_insert_bool_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int value);

// Lookup bool scalar (zero-copy)
shm_error_t shm_lookup_bool_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int* out_value);

// Zero-copy view for int64 vector
typedef struct {
    const int64_t* data;
    size_t count;
} shm_int_vector_view_t;

// Zero-copy view for double vector
typedef struct {
    const double* data;
    size_t count;
} shm_float_vector_view_t;

// Lookup int64_t vector (zero-copy, returns view)
shm_error_t shm_lookup_int_vector(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_int_vector_view_t* out_view);

// Lookup double vector (zero-copy, returns view)
shm_error_t shm_lookup_float_vector(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_float_vector_view_t* out_view);

// Insert string (zero-copy optimized)
shm_error_t shm_insert_string(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               const char* value,
                               size_t value_len);

// Insert bytes blob (zero-copy optimized)
shm_error_t shm_insert_bytes(shm_handle_t handle,
                              const void* key,
                              size_t key_len,
                              const uint8_t* value,
                              size_t value_len);

// Insert int64_t matrix (zero-copy optimized)
shm_error_t shm_insert_int_matrix(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const int64_t* values,
                                   size_t rows,
                                   size_t cols);

// Insert double matrix (zero-copy optimized)
shm_error_t shm_insert_float_matrix(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const double* values,
                                     size_t rows,
                                     size_t cols);

// Zero-copy view for string
typedef struct {
    const char* data;
    size_t length;
} shm_string_view_t;

// Zero-copy view for bytes
typedef struct {
    const uint8_t* data;
    size_t length;
} shm_bytes_view_t;

// Zero-copy view for bool vector (byte-packed 0/1)
typedef struct {
    const uint8_t* data;
    size_t count;
} shm_bool_vector_view_t;

// Zero-copy view for int64 matrix
typedef struct {
    const int64_t* data;
    size_t rows;
    size_t cols;
} shm_int_matrix_view_t;

// Zero-copy view for double matrix
typedef struct {
    const double* data;
    size_t rows;
    size_t cols;
} shm_float_matrix_view_t;

// Lookup string (zero-copy, returns view)
shm_error_t shm_lookup_string(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               shm_string_view_t* out_view);

// Lookup bytes blob (zero-copy, returns view)
shm_error_t shm_lookup_bytes(shm_handle_t handle,
                              const void* key,
                              size_t key_len,
                              shm_bytes_view_t* out_view);

// Insert bool vector/list (zero-copy, byte-packed)
shm_error_t shm_insert_bool_vector(shm_handle_t handle,
                                    const void* key,
                                    size_t key_len,
                                    const uint8_t* values,
                                    size_t count);

// Lookup bool vector/list (zero-copy, returns view)
shm_error_t shm_lookup_bool_vector(shm_handle_t handle,
                                    const void* key,
                                    size_t key_len,
                                    shm_bool_vector_view_t* out_view);

// Lookup int64_t matrix (zero-copy, returns view)
shm_error_t shm_lookup_int_matrix(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_int_matrix_view_t* out_view);

// Lookup double matrix (zero-copy, returns view)
shm_error_t shm_lookup_float_matrix(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_float_matrix_view_t* out_view);

// Insert int64 set (zero-copy optimized, elements will be sorted)
shm_error_t shm_insert_int_set(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                const int64_t* values,
                                size_t count);

// Insert double set (zero-copy optimized, elements will be sorted)
shm_error_t shm_insert_float_set(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  const double* values,
                                  size_t count);

// Insert string set (zero-copy optimized, strings will be sorted)
shm_error_t shm_insert_string_set(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const char** strings,
                                   const size_t* string_lengths,
                                   size_t count);

// Insert string vector/list (zero-copy optimized, preserves order)
shm_error_t shm_insert_string_vector(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      const char** strings,
                                      const size_t* string_lengths,
                                      size_t count);

// Insert Dict[str, int] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_int(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const char** keys,
                                     const size_t* key_lengths,
                                     const int64_t* values,
                                     size_t count);

// Insert Dict[str, float] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_float(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       const char** keys,
                                       const size_t* key_lengths,
                                       const double* values,
                                       size_t count);

// Insert Dict[str, string] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_string(shm_handle_t handle,
                                        const void* key,
                                        size_t key_len,
                                        const char** keys,
                                        const size_t* key_lengths,
                                        const char** values,
                                        const size_t* value_lengths,
                                        size_t count);

// Insert Dict[str, bool] (zero-copy optimized, sorted by key)
// values are 0/1 bytes.
shm_error_t shm_insert_dict_str_bool(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      const char** keys,
                                      const size_t* key_lengths,
                                      const uint8_t* values,
                                      size_t count);

// Zero-copy view for int64 set
typedef struct {
    const int64_t* data;
    size_t count;
} shm_int_set_view_t;

// Zero-copy view for double set
typedef struct {
    const double* data;
    size_t count;
} shm_float_set_view_t;

// Zero-copy view for string set
typedef struct {
    const uint32_t* offsets;  // offsets[i] is start of string i
    const char* string_data;  // concatenated string data
    size_t count;             // number of strings
} shm_string_set_view_t;

// Zero-copy view for string vector/list
typedef struct {
    const uint32_t* offsets;  // offsets[i] is start of string i (count+1 entries)
    const char* string_data;  // concatenated string data
    size_t count;             // number of strings
} shm_string_vector_view_t;

// Zero-copy view for Dict[str, int]
typedef struct {
    const uint32_t* key_offsets;  // offsets for keys
    const char* keys_data;        // concatenated key strings
    const int64_t* values;        // array of values
    size_t count;                 // number of key-value pairs
} shm_dict_str_int_view_t;

// Zero-copy view for Dict[str, float]
typedef struct {
    const uint32_t* key_offsets;  // offsets for keys
    const char* keys_data;        // concatenated key strings
    const double* values;         // array of values
    size_t count;                 // number of key-value pairs
} shm_dict_str_float_view_t;

// Zero-copy view for Dict[str, string]
typedef struct {
    const uint32_t* key_offsets;    // offsets for keys (count+1)
    const char* keys_data;          // concatenated key strings
    const uint32_t* value_offsets;  // offsets for values (count+1)
    const char* values_data;        // concatenated value strings
    size_t count;                   // number of key-value pairs
} shm_dict_str_string_view_t;

// Zero-copy view for Dict[str, bool]
typedef struct {
    const uint32_t* key_offsets;  // offsets for keys (count+1)
    const char* keys_data;        // concatenated key strings
    const uint8_t* values;        // array of 0/1 bytes
    size_t count;                 // number of key-value pairs
} shm_dict_str_bool_view_t;

// Zero-copy view for Dict[str, bytes]
// Values are stored as a blob with (count+1) offsets.
typedef struct {
    const uint32_t* key_offsets;    // offsets for keys (count+1)
    const char* keys_data;          // concatenated key strings
    const uint32_t* value_offsets;  // offsets for values_data (count+1)
    const uint8_t* values_data;     // concatenated raw bytes
    size_t count;                   // number of key-value pairs
} shm_dict_str_bytes_view_t;

// ============================================================================
// Recursive Object/List (Typed Tree) Views
// ============================================================================

// View for a recursive typed payload stored inside OBJECT/LIST.
// This is not a key-value entry by itself; it points into an existing value blob.
typedef struct {
    shm_value_type_t type;
    const void* payload;
    size_t payload_len;
} shm_typed_value_view_t;

// Zero-copy view for OBJECT (string-keyed, sorted keys, binary-search lookup).
typedef struct {
    const uint32_t* name_offsets;   // (count+1) offsets into names_data
    const char* names_data;         // concatenated field-name strings
    const uint8_t* field_types;     // (count) shm_value_type_t per field
    const uint32_t* value_offsets;  // (count+1) offsets into values_data
    const uint8_t* values_data;     // concatenated recursive typed payloads
    size_t count;
} shm_object_view_t;

// Zero-copy view for LIST (heterogeneous).
typedef struct {
    const uint8_t* elem_types;      // (count) shm_value_type_t per element
    const uint32_t* value_offsets;  // (count+1) offsets into values_data
    const uint8_t* values_data;     // concatenated recursive typed payloads
    size_t count;
} shm_list_view_t;

// Zero-copy view for Dict[str, list[float]] stored as flattened double array
typedef struct {
    const uint32_t* key_offsets;     // (count+1) offsets into keys_data
    const char* keys_data;           // concatenated key strings
    const uint32_t* value_offsets;   // (count+1) offsets into values_flat (in elements)
    const uint32_t* value_lengths;   // (count) lengths per vector (in elements)
    const double* values_flat;       // concatenated values
    size_t count;
} shm_dict_str_float_vector_view_t;

// Zero-copy view for Dict[str, matrix[float]] stored as flattened double array.
// For each key i:
//  - rows[i], cols[i]
//  - values start at values_flat[value_offsets[i]]
//  - length = rows[i] * cols[i]
typedef struct {
    const uint32_t* key_offsets;     // (count+1) offsets into keys_data
    const char* keys_data;           // concatenated key strings
    const uint32_t* value_offsets;   // (count+1) offsets into values_flat (in elements)
    const uint32_t* rows;            // (count)
    const uint32_t* cols;            // (count)
    const double* values_flat;       // concatenated values
    size_t count;
} shm_dict_str_float_matrix_view_t;

// Zero-copy view for Dict[str, list[string]] stored as 2-level offsets+blob.
// Layout provides:
//  - keys: key_offsets + keys_data
//  - values: value_list_offsets tells, for each key, the slice in string_offsets
//  - string_offsets + string_data store all strings across all lists
typedef struct {
    const uint32_t* key_offsets;          // (count+1) offsets into keys_data
    const char* keys_data;                // concatenated key strings
    const uint32_t* value_list_offsets;   // (count+1) offsets into string_offsets index space
    const uint32_t* string_offsets;       // (n_strings+1) offsets into string_data
    const char* string_data;              // concatenated string bytes
    size_t count;
    size_t n_strings;
} shm_dict_str_string_vector_view_t;

// Insert Dict[str, list[string]] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_string_vector(shm_handle_t handle,
                                               const void* key,
                                               size_t key_len,
                                               const char** keys,
                                               const size_t* key_lengths,
                                               const uint32_t* value_list_offsets,
                                               const uint32_t* string_offsets,
                                               const char* string_data,
                                               size_t n_strings,
                                               size_t count);

// Lookup Dict[str, list[string]] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_string_vector(shm_handle_t handle,
                                               const void* key,
                                               size_t key_len,
                                               shm_dict_str_string_vector_view_t* out_view);

// Insert Dict[str, list[float]] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_float_vector(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              const char** keys,
                                              const size_t* key_lengths,
                                              const uint32_t* value_offsets,
                                              const uint32_t* value_lengths,
                                              const double* values_flat,
                                              size_t count);

// Lookup Dict[str, list[float]] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_float_vector(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              shm_dict_str_float_vector_view_t* out_view);

// Insert Dict[str, matrix[float]] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_float_matrix(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              const char** keys,
                                              const size_t* key_lengths,
                                              const uint32_t* value_offsets,
                                              const uint32_t* rows,
                                              const uint32_t* cols,
                                              const double* values_flat,
                                              size_t count);

// Lookup Dict[str, matrix[float]] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_float_matrix(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              shm_dict_str_float_matrix_view_t* out_view);

// Insert OBJECT (recursive typed payload). Fields are provided as parallel arrays.
// - field_names/field_name_lengths: UTF-8 bytes
// - field_types: shm_value_type_t per field
// - field_payloads/field_payload_lengths: typed payload bytes for each field (already encoded)
// The implementation sorts by field name and rejects duplicates.
shm_error_t shm_insert_object(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               const char** field_names,
                               const size_t* field_name_lengths,
                               const uint8_t* field_types,
                               const void* const* field_payloads,
                               const size_t* field_payload_lengths,
                               size_t field_count);

// Insert LIST (heterogeneous, recursive typed payload).
shm_error_t shm_insert_list(shm_handle_t handle,
                             const void* key,
                             size_t key_len,
                             const uint8_t* elem_types,
                             const void* const* elem_payloads,
                             const size_t* elem_payload_lengths,
                             size_t count);

// Lookup OBJECT (zero-copy, returns view)
shm_error_t shm_lookup_object(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               shm_object_view_t* out_view);

// Lookup LIST (zero-copy, returns view)
shm_error_t shm_lookup_list(shm_handle_t handle,
                             const void* key,
                             size_t key_len,
                             shm_list_view_t* out_view);

// OBJECT field lookup (binary-search by name; returns typed view into values_data)
shm_error_t shm_object_get_field(const shm_object_view_t* object_view,
                                  const char* field_name,
                                  size_t field_name_len,
                                  shm_typed_value_view_t* out_value);

// LIST element lookup (by index; returns typed view into values_data)
shm_error_t shm_list_get_element(const shm_list_view_t* list_view,
                                  size_t index,
                                  shm_typed_value_view_t* out_value);

// Lookup int64 set (zero-copy, returns view)
shm_error_t shm_lookup_int_set(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                shm_int_set_view_t* out_view);

// Lookup double set (zero-copy, returns view)
shm_error_t shm_lookup_float_set(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  shm_float_set_view_t* out_view);

// Lookup string set (zero-copy, returns view)
shm_error_t shm_lookup_string_set(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_string_set_view_t* out_view);

// Lookup string vector/list (zero-copy, returns view)
shm_error_t shm_lookup_string_vector(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      shm_string_vector_view_t* out_view);

// Lookup Dict[str, int] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_int(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_dict_str_int_view_t* out_view);

// Lookup Dict[str, float] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_float(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       shm_dict_str_float_view_t* out_view);

// Lookup Dict[str, string] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_string(shm_handle_t handle,
                                        const void* key,
                                        size_t key_len,
                                        shm_dict_str_string_view_t* out_view);

// Lookup Dict[str, bool] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_bool(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      shm_dict_str_bool_view_t* out_view);

// Insert Dict[str, bytes] (zero-copy optimized, sorted by key)
shm_error_t shm_insert_dict_str_bytes(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       const char** keys,
                                       const size_t* key_lengths,
                                       const uint8_t* const* values,
                                       const size_t* value_lengths,
                                       size_t count);

// Lookup Dict[str, bytes] (zero-copy, returns view)
shm_error_t shm_lookup_dict_str_bytes(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       shm_dict_str_bytes_view_t* out_view);

// ============================================================================
// Security and Permission Management
// ============================================================================

// Check if current process is the owner of shared memory
// Returns SHM_OK and sets *is_owner to true/false
shm_error_t shm_check_owner(shm_handle_t handle, int* is_owner);

// Check if current process is authorized to access shared memory
// (either owner or in authorized PID list)
// Returns SHM_OK and sets *is_authorized to true/false
shm_error_t shm_check_authorized(shm_handle_t handle, int* is_authorized);

// Add a PID to the authorized list (owner only)
// Returns SHM_OK on success, SHM_ERR_PERMISSION_DENIED if not owner
shm_error_t shm_add_authorized_pid(shm_handle_t handle, int pid);

// ============================================================================
// Encryption Support (AES-128)
// ============================================================================

// Insert encrypted key-value pair (owner only)
// aes_key must be 16 bytes
// Returns SHM_OK on success, SHM_ERR_PERMISSION_DENIED if not owner
shm_error_t shm_insert_encrypted(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  const void* value,
                                  size_t value_len,
                                  const uint8_t* aes_key);

// Lookup and decrypt key-value pair (authorized processes only)
// aes_key must be 16 bytes
// Returns SHM_OK on success, SHM_ERR_PERMISSION_DENIED if not authorized
shm_error_t shm_lookup_decrypted(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  void* out_buffer,
                                  size_t buffer_size,
                                  size_t* out_value_len,
                                  const uint8_t* aes_key);

#ifdef __cplusplus
}
#endif

#endif // SHM_KV_C_API_H
