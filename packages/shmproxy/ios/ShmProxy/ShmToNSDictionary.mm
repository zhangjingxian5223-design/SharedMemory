/**
 * ShmToNSDictionary.mm
 *
 * Implementation of SharedMemory to NSDictionary/NSArray conversion.
 * This mirrors ShmProxyObject but outputs Objective-C objects instead of jsi::Value.
 */

#import "ShmToNSDictionary.h"
#include <cstring>
#include <string>

// ============================================================================
// Alignment helper
// ============================================================================
static inline size_t alignUp4(size_t x) {
    return (x + 3) & ~3;
}

// ============================================================================
// Forward declaration
// ============================================================================
static id convertTypedValueToNSObject(const shm_typed_value_view_t& view);

// ============================================================================
// Convert scalar types
// ============================================================================
static id convertScalarToNSObject(shm_value_type_t type,
                                   const void* payload,
                                   size_t len) {
    switch (type) {
        case SHM_TYPE_INT_SCALAR: {
            int64_t value = *static_cast<const int64_t*>(payload);
            return @(value);
        }

        case SHM_TYPE_FLOAT_SCALAR: {
            double value = *static_cast<const double*>(payload);
            return @(value);
        }

        case SHM_TYPE_BOOL_SCALAR: {
            uint8_t value = *static_cast<const uint8_t*>(payload);
            return @(value != 0);
        }

        case SHM_TYPE_STRING: {
            // Format: [length:4][data:N]
            const uint8_t* ptr = static_cast<const uint8_t*>(payload);
            uint32_t strLen = *reinterpret_cast<const uint32_t*>(ptr);
            const char* strData = reinterpret_cast<const char*>(ptr + 4);
            return [[NSString alloc] initWithBytes:strData
                                            length:strLen
                                          encoding:NSUTF8StringEncoding];
        }

        case SHM_TYPE_NULL:
            return [NSNull null];

        default:
            return nil;
    }
}

// ============================================================================
// Convert vector types
// ============================================================================
static NSArray* convertVectorToNSArray(shm_value_type_t type,
                                        const void* payload,
                                        size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(payload);

    switch (type) {
        case SHM_TYPE_FLOAT_VECTOR: {
            // Format: [count:4][data:8*count]
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const double* data = reinterpret_cast<const double*>(ptr + 4);

            NSMutableArray* arr = [NSMutableArray arrayWithCapacity:count];
            for (uint32_t i = 0; i < count; ++i) {
                [arr addObject:@(data[i])];
            }
            return arr;
        }

        case SHM_TYPE_INT_VECTOR: {
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const int64_t* data = reinterpret_cast<const int64_t*>(ptr + 4);

            NSMutableArray* arr = [NSMutableArray arrayWithCapacity:count];
            for (uint32_t i = 0; i < count; ++i) {
                [arr addObject:@(data[i])];
            }
            return arr;
        }

        case SHM_TYPE_BOOL_VECTOR: {
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const uint8_t* data = ptr + 4;

            NSMutableArray* arr = [NSMutableArray arrayWithCapacity:count];
            for (uint32_t i = 0; i < count; ++i) {
                [arr addObject:@(data[i] != 0)];
            }
            return arr;
        }

        case SHM_TYPE_STRING_VECTOR: {
            // Format: [count:4][offsets:4*(count+1)][string_data:N]
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const uint32_t* offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
            const char* stringData = reinterpret_cast<const char*>(
                ptr + 4 + 4 * (count + 1));

            NSMutableArray* arr = [NSMutableArray arrayWithCapacity:count];
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t start = offsets[i];
                uint32_t end = offsets[i + 1];
                NSString* str = [[NSString alloc] initWithBytes:stringData + start
                                                         length:end - start
                                                       encoding:NSUTF8StringEncoding];
                [arr addObject:str ?: @""];
            }
            return arr;
        }

        default:
            return @[];
    }
}

// ============================================================================
// Convert OBJECT type to NSDictionary
// ============================================================================
static NSDictionary* convertObjectToNSDictionary(const void* payload, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(payload);
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    if (count == 0) {
        return @{};
    }

    // Parse object view
    const uint32_t* nameOffsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* namesData = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
    const uint8_t* fieldTypes = reinterpret_cast<const uint8_t*>(
        namesData + nameOffsets[count]);

    // Calculate value_offsets position (aligned to 4)
    size_t afterTypesOffset = 4 + 4 * (count + 1) + nameOffsets[count] + count;
    afterTypesOffset = alignUp4(afterTypesOffset);
    const uint8_t* afterTypes = ptr + afterTypesOffset;

    const uint32_t* valueOffsets = reinterpret_cast<const uint32_t*>(afterTypes);
    const uint8_t* valuesData = afterTypes + 4 * (count + 1);

    // Build dictionary
    NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithCapacity:count];

    for (uint32_t i = 0; i < count; ++i) {
        // Get field name
        uint32_t nameStart = nameOffsets[i];
        uint32_t nameEnd = nameOffsets[i + 1];
        NSString* fieldName = [[NSString alloc] initWithBytes:namesData + nameStart
                                                       length:nameEnd - nameStart
                                                     encoding:NSUTF8StringEncoding];

        // Get field value
        shm_typed_value_view_t fieldView;
        fieldView.type = static_cast<shm_value_type_t>(fieldTypes[i]);
        uint32_t vstart = valueOffsets[i];
        uint32_t vend = valueOffsets[i + 1];
        fieldView.payload = valuesData + vstart;
        fieldView.payload_len = vend - vstart;

        // Convert and add to dictionary
        id value = convertTypedValueToNSObject(fieldView);
        if (fieldName && value) {
            dict[fieldName] = value;
        }
    }

    return dict;
}

// ============================================================================
// Convert LIST type to NSArray
// ============================================================================
static NSArray* convertListToNSArray(const void* payload, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(payload);
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    if (count == 0) {
        return @[];
    }

    // Parse list view
    const uint8_t* elemTypes = ptr + 4;

    size_t afterTypesOffset = alignUp4(4 + count);
    const uint8_t* afterTypes = ptr + afterTypesOffset;
    const uint32_t* valueOffsets = reinterpret_cast<const uint32_t*>(afterTypes);
    const uint8_t* valuesData = afterTypes + 4 * (count + 1);

    // Build array
    NSMutableArray* arr = [NSMutableArray arrayWithCapacity:count];

    for (uint32_t i = 0; i < count; ++i) {
        shm_typed_value_view_t elemView;
        elemView.type = static_cast<shm_value_type_t>(elemTypes[i]);
        uint32_t vstart = valueOffsets[i];
        uint32_t vend = valueOffsets[i + 1];
        elemView.payload = valuesData + vstart;
        elemView.payload_len = vend - vstart;

        id value = convertTypedValueToNSObject(elemView);
        [arr addObject:value ?: [NSNull null]];
    }

    return arr;
}

// ============================================================================
// Main conversion function
// ============================================================================
static id convertTypedValueToNSObject(const shm_typed_value_view_t& view) {
    switch (view.type) {
        case SHM_TYPE_NULL:
            return [NSNull null];

        case SHM_TYPE_INT_SCALAR:
        case SHM_TYPE_FLOAT_SCALAR:
        case SHM_TYPE_BOOL_SCALAR:
        case SHM_TYPE_STRING:
            return convertScalarToNSObject(view.type, view.payload, view.payload_len);

        case SHM_TYPE_INT_VECTOR:
        case SHM_TYPE_FLOAT_VECTOR:
        case SHM_TYPE_BOOL_VECTOR:
        case SHM_TYPE_STRING_VECTOR:
            return convertVectorToNSArray(view.type, view.payload, view.payload_len);

        case SHM_TYPE_OBJECT:
            return convertObjectToNSDictionary(view.payload, view.payload_len);

        case SHM_TYPE_LIST:
            return convertListToNSArray(view.payload, view.payload_len);

        default:
            return nil;
    }
}

// ============================================================================
// Public API
// ============================================================================
shm_error_t convertShmToNSDictionary(shm_handle_t handle,
                                      const char* key,
                                      size_t keyLen,
                                      NSDictionary** outDict) {
    if (!handle || !key || !outDict) {
        return SHM_ERR_INVALID_PARAM;
    }

    // Get value type
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(handle, key, keyLen, &type);
    if (err != SHM_OK) {
        return err;
    }

    if (type != SHM_TYPE_OBJECT) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    // Get object view
    shm_object_view_t objView;
    err = shm_lookup_object(handle, key, keyLen, &objView);
    if (err != SHM_OK) {
        return err;
    }

    // Build dictionary
    NSMutableDictionary* dict = [NSMutableDictionary dictionaryWithCapacity:objView.count];

    for (size_t i = 0; i < objView.count; ++i) {
        // Get field name
        uint32_t nameStart = objView.name_offsets[i];
        uint32_t nameEnd = objView.name_offsets[i + 1];
        NSString* fieldName = [[NSString alloc] initWithBytes:objView.names_data + nameStart
                                                       length:nameEnd - nameStart
                                                     encoding:NSUTF8StringEncoding];

        // Get field value
        shm_typed_value_view_t fieldView;
        fieldView.type = static_cast<shm_value_type_t>(objView.field_types[i]);
        uint32_t vstart = objView.value_offsets[i];
        uint32_t vend = objView.value_offsets[i + 1];
        fieldView.payload = objView.values_data + vstart;
        fieldView.payload_len = vend - vstart;

        // Convert and add to dictionary
        id value = convertTypedValueToNSObject(fieldView);
        if (fieldName && value) {
            dict[fieldName] = value;
        }
    }

    *outDict = dict;
    return SHM_OK;
}

shm_error_t convertShmToNSArray(shm_handle_t handle,
                                 const char* key,
                                 size_t keyLen,
                                 NSArray** outArray) {
    if (!handle || !key || !outArray) {
        return SHM_ERR_INVALID_PARAM;
    }

    // Get value type
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(handle, key, keyLen, &type);
    if (err != SHM_OK) {
        return err;
    }

    if (type != SHM_TYPE_LIST) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    // Get list view
    shm_list_view_t listView;
    err = shm_lookup_list(handle, key, keyLen, &listView);
    if (err != SHM_OK) {
        return err;
    }

    // Build array
    NSMutableArray* arr = [NSMutableArray arrayWithCapacity:listView.count];

    for (size_t i = 0; i < listView.count; ++i) {
        shm_typed_value_view_t elemView;
        shm_error_t elemErr = shm_list_get_element(&listView, i, &elemView);
        if (elemErr == SHM_OK) {
            id value = convertTypedValueToNSObject(elemView);
            [arr addObject:value ?: [NSNull null]];
        }
    }

    *outArray = arr;
    return SHM_OK;
}

shm_error_t convertShmToNSObject(shm_handle_t handle,
                                  const char* key,
                                  size_t keyLen,
                                  id* outValue) {
    if (!handle || !key || !outValue) {
        return SHM_ERR_INVALID_PARAM;
    }

    // Get value type
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(handle, key, keyLen, &type);
    if (err != SHM_OK) {
        return err;
    }

    switch (type) {
        case SHM_TYPE_OBJECT: {
            NSDictionary* dict;
            err = convertShmToNSDictionary(handle, key, keyLen, &dict);
            if (err == SHM_OK) {
                *outValue = dict;
            }
            return err;
        }

        case SHM_TYPE_LIST: {
            NSArray* arr;
            err = convertShmToNSArray(handle, key, keyLen, &arr);
            if (err == SHM_OK) {
                *outValue = arr;
            }
            return err;
        }

        case SHM_TYPE_INT_SCALAR: {
            int64_t val;
            err = shm_lookup_int_scalar(handle, key, keyLen, &val);
            if (err == SHM_OK) {
                *outValue = @(val);
            }
            return err;
        }

        case SHM_TYPE_FLOAT_SCALAR: {
            double val;
            err = shm_lookup_float_scalar(handle, key, keyLen, &val);
            if (err == SHM_OK) {
                *outValue = @(val);
            }
            return err;
        }

        case SHM_TYPE_BOOL_SCALAR: {
            int val;
            err = shm_lookup_bool_scalar(handle, key, keyLen, &val);
            if (err == SHM_OK) {
                *outValue = @(val != 0);
            }
            return err;
        }

        case SHM_TYPE_STRING: {
            shm_string_view_t view;
            err = shm_lookup_string(handle, key, keyLen, &view);
            if (err == SHM_OK) {
                *outValue = [[NSString alloc] initWithBytes:view.data
                                                    length:view.length
                                                  encoding:NSUTF8StringEncoding];
            }
            return err;
        }

        case SHM_TYPE_NULL: {
            *outValue = [NSNull null];
            return SHM_OK;
        }

        default:
            return SHM_ERR_TYPE_MISMATCH;
    }
}
