/**
 * ShmProxyObject.mm
 *
 * Implementation of JSI HostObject for lazy shared memory access
 */

#include "ShmProxyObject.h"
#include <cstring>
#include <cstdio>

namespace facebook::react {

// ============================================================================
// ShmProxyObject Implementation
// ============================================================================

ShmProxyObject::ShmProxyObject(shm_handle_t handle, const std::string& key)
    : handle_(handle), key_(key) {}

ShmProxyObject::ShmProxyObject(shm_handle_t handle,
                               const shm_object_view_t& objectView,
                               size_t fieldIndex)
    : handle_(handle), isNestedObject_(true), objectView_(objectView) {
    // We store the full object view; field access happens in get()
}

ShmProxyObject::ShmProxyObject(shm_handle_t handle,
                               const shm_typed_value_view_t& typedView)
    : handle_(handle), isTypedValue_(true), typedView_(typedView) {}

jsi::Value ShmProxyObject::get(jsi::Runtime& rt, const jsi::PropNameID& name) {
    std::string propName = name.utf8(rt);

    // Handle special properties
    if (propName == "toString" || propName == "valueOf") {
        return jsi::Value::undefined();
    }

    // If this is a typed value (nested access), handle it
    if (isTypedValue_) {
        if (typedView_.type == SHM_TYPE_OBJECT) {
            // Parse the object view from the payload
            const uint8_t* ptr = static_cast<const uint8_t*>(typedView_.payload);
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

            shm_object_view_t objView;
            objView.count = count;
            objView.name_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
            objView.names_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
            objView.field_types = reinterpret_cast<const uint8_t*>(
                objView.names_data + objView.name_offsets[count]);

            // Calculate value_offsets position (aligned to 4)
            size_t after_types_offset = 4 + 4 * (count + 1) + objView.name_offsets[count] + count;
            after_types_offset = (after_types_offset + 3) & ~3;  // Align to 4
            const uint8_t* after_types = ptr + after_types_offset;

            objView.value_offsets = reinterpret_cast<const uint32_t*>(after_types);
            objView.values_data = reinterpret_cast<const uint8_t*>(after_types + 4 * (count + 1));

            // Look up the field
            shm_typed_value_view_t fieldView;
            shm_error_t err = shm_object_get_field(&objView, propName.c_str(),
                                                    propName.size(), &fieldView);
            if (err == SHM_OK) {
                return convertTypedValueToJsi(rt, fieldView);
            }
            return jsi::Value::undefined();
        }
        else if (typedView_.type == SHM_TYPE_LIST) {
            // Handle array-like access
            // Check if propName is "length"
            if (propName == "length") {
                const uint8_t* ptr = static_cast<const uint8_t*>(typedView_.payload);
                uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
                return jsi::Value(static_cast<int>(count));
            }

            // Try to parse as index
            char* endptr;
            long index = strtol(propName.c_str(), &endptr, 10);
            if (*endptr == '\0' && index >= 0) {
                // Parse list view
                const uint8_t* ptr = static_cast<const uint8_t*>(typedView_.payload);
                uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

                if (static_cast<size_t>(index) >= count) {
                    return jsi::Value::undefined();
                }

                shm_list_view_t listView;
                listView.count = count;
                listView.elem_types = ptr + 4;

                size_t after_types_offset = (4 + count + 3) & ~3;
                const uint8_t* after_types = ptr + after_types_offset;
                listView.value_offsets = reinterpret_cast<const uint32_t*>(after_types);
                listView.values_data = reinterpret_cast<const uint8_t*>(
                    after_types + 4 * (count + 1));

                shm_typed_value_view_t elemView;
                shm_error_t err = shm_list_get_element(&listView, index, &elemView);
                if (err == SHM_OK) {
                    return convertTypedValueToJsi(rt, elemView);
                }
            }
            return jsi::Value::undefined();
        }

        // For other types, return the converted value
        return convertTypedValueToJsi(rt, typedView_);
    }

    // If this is a nested object view
    if (isNestedObject_) {
        shm_typed_value_view_t fieldView;
        shm_error_t err = shm_object_get_field(&objectView_, propName.c_str(),
                                                propName.size(), &fieldView);
        if (err == SHM_OK) {
            return convertTypedValueToJsi(rt, fieldView);
        }
        return jsi::Value::undefined();
    }

    // Top-level access: look up the key in shm
    shm_value_type_t type;
    shm_error_t err = shm_get_value_type(handle_, key_.c_str(), key_.size(), &type);
    if (err != SHM_OK) {
        return jsi::Value::undefined();
    }

    if (type == SHM_TYPE_OBJECT) {
        shm_object_view_t objView;
        err = shm_lookup_object(handle_, key_.c_str(), key_.size(), &objView);
        if (err != SHM_OK) {
            return jsi::Value::undefined();
        }

        // Look up the field
        shm_typed_value_view_t fieldView;
        err = shm_object_get_field(&objView, propName.c_str(),
                                   propName.size(), &fieldView);
        if (err == SHM_OK) {
            return convertTypedValueToJsi(rt, fieldView);
        }
    }

    return jsi::Value::undefined();
}

std::vector<jsi::PropNameID> ShmProxyObject::getPropertyNames(jsi::Runtime& rt) {
    std::vector<jsi::PropNameID> names;

    if (isTypedValue_ && typedView_.type == SHM_TYPE_OBJECT) {
        // Parse object view from payload
        const uint8_t* ptr = static_cast<const uint8_t*>(typedView_.payload);
        uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
        const uint32_t* name_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
        const char* names_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t start = name_offsets[i];
            uint32_t end = name_offsets[i + 1];
            std::string fieldName(names_data + start, end - start);
            names.push_back(jsi::PropNameID::forUtf8(rt, fieldName));
        }
    }
    else if (isNestedObject_) {
        for (size_t i = 0; i < objectView_.count; ++i) {
            uint32_t start = objectView_.name_offsets[i];
            uint32_t end = objectView_.name_offsets[i + 1];
            std::string fieldName(objectView_.names_data + start, end - start);
            names.push_back(jsi::PropNameID::forUtf8(rt, fieldName));
        }
    }
    else if (!key_.empty()) {
        // Top-level: get object view
        shm_object_view_t objView;
        shm_error_t err = shm_lookup_object(handle_, key_.c_str(), key_.size(), &objView);
        if (err == SHM_OK) {
            for (size_t i = 0; i < objView.count; ++i) {
                uint32_t start = objView.name_offsets[i];
                uint32_t end = objView.name_offsets[i + 1];
                std::string fieldName(objView.names_data + start, end - start);
                names.push_back(jsi::PropNameID::forUtf8(rt, fieldName));
            }
        }
    }

    return names;
}

// Static method to convert top-level shm object to plain JS object
jsi::Value ShmProxyObject::convertTopLevelToJsObject(jsi::Runtime& rt,
                                                       shm_handle_t handle,
                                                       const std::string& key) {
    // Get the object view
    shm_object_view_t objView;
    shm_error_t err = shm_lookup_object(handle, key.c_str(), key.size(), &objView);
    if (err != SHM_OK) {
        return jsi::Value::undefined();
    }

    // Create a temporary proxy to use its conversion methods
    ShmProxyObject tempProxy(handle, key);

    // Create a plain JS object
    jsi::Object obj = jsi::Object(rt);

    // Iterate through all fields and convert them
    for (size_t i = 0; i < objView.count; ++i) {
        // Get field name
        uint32_t nameStart = objView.name_offsets[i];
        uint32_t nameEnd = objView.name_offsets[i + 1];
        std::string fieldName(objView.names_data + nameStart, nameEnd - nameStart);

        // Get field value
        shm_typed_value_view_t fieldView;
        fieldView.type = (shm_value_type_t)objView.field_types[i];
        uint32_t vstart = objView.value_offsets[i];
        uint32_t vend = objView.value_offsets[i + 1];
        fieldView.payload = objView.values_data + vstart;
        fieldView.payload_len = vend - vstart;

        // Convert and set property
        obj.setProperty(rt, jsi::PropNameID::forUtf8(rt, fieldName),
                       tempProxy.convertTypedValueToJsi(rt, fieldView));
    }

    return obj;
}

jsi::Value ShmProxyObject::convertTypedValueToJsi(jsi::Runtime& rt,
                                                   const shm_typed_value_view_t& view) {
    switch (view.type) {
        // Null type - return JS null
        case SHM_TYPE_NULL:
            return jsi::Value::null();

        // Scalar types - convert directly
        case SHM_TYPE_INT_SCALAR:
        case SHM_TYPE_FLOAT_SCALAR:
        case SHM_TYPE_BOOL_SCALAR:
        case SHM_TYPE_STRING:
            return convertScalarToJsi(rt, view.type, view.payload, view.payload_len);

        // Vector types - convert to JS Array
        case SHM_TYPE_INT_VECTOR:
        case SHM_TYPE_FLOAT_VECTOR:
        case SHM_TYPE_BOOL_VECTOR:
        case SHM_TYPE_STRING_VECTOR:
            return convertVectorToJsi(rt, view.type, view.payload, view.payload_len);

        // Object type - convert to plain JS object for Object.values/entries compatibility
        case SHM_TYPE_OBJECT: {
            // Parse the object view from the payload
            const uint8_t* ptr = static_cast<const uint8_t*>(view.payload);
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

            shm_object_view_t objView;
            objView.count = count;
            objView.name_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
            objView.names_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
            objView.field_types = reinterpret_cast<const uint8_t*>(
                objView.names_data + objView.name_offsets[count]);

            // Calculate value_offsets position (aligned to 4)
            size_t after_types_offset = 4 + 4 * (count + 1) + objView.name_offsets[count] + count;
            after_types_offset = (after_types_offset + 3) & ~3;  // Align to 4
            const uint8_t* after_types = ptr + after_types_offset;

            objView.value_offsets = reinterpret_cast<const uint32_t*>(after_types);
            objView.values_data = reinterpret_cast<const uint8_t*>(after_types + 4 * (count + 1));

            // Create a plain JS object and populate it
            jsi::Object obj = jsi::Object(rt);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t nameStart = objView.name_offsets[i];
                uint32_t nameEnd = objView.name_offsets[i + 1];
                std::string fieldName(objView.names_data + nameStart, nameEnd - nameStart);

                // Get the field value
                shm_typed_value_view_t fieldView;
                fieldView.type = (shm_value_type_t)objView.field_types[i];
                uint32_t vstart = objView.value_offsets[i];
                uint32_t vend = objView.value_offsets[i + 1];
                fieldView.payload = objView.values_data + vstart;
                fieldView.payload_len = vend - vstart;

                // Recursively convert the field value
                obj.setProperty(rt, jsi::PropNameID::forUtf8(rt, fieldName),
                               convertTypedValueToJsi(rt, fieldView));
            }
            return obj;
        }

        // List type - return a real jsi::Array for Array.isArray() compatibility
        case SHM_TYPE_LIST: {
            // Parse list view
            const uint8_t* ptr = static_cast<const uint8_t*>(view.payload);
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

            shm_list_view_t listView;
            listView.count = count;
            listView.elem_types = ptr + 4;

            size_t after_types_offset = (4 + count + 3) & ~3;
            const uint8_t* after_types = ptr + after_types_offset;
            listView.value_offsets = reinterpret_cast<const uint32_t*>(after_types);
            listView.values_data = reinterpret_cast<const uint8_t*>(
                after_types + 4 * (count + 1));

            // Create a real jsi::Array and populate it
            jsi::Array arr = jsi::Array(rt, count);
            for (uint32_t i = 0; i < count; ++i) {
                shm_typed_value_view_t elemView;
                shm_error_t err = shm_list_get_element(&listView, i, &elemView);
                if (err == SHM_OK) {
                    arr.setValueAtIndex(rt, i, convertTypedValueToJsi(rt, elemView));
                }
            }
            return arr;
        }

        default:
            return jsi::Value::undefined();
    }
}

jsi::Value ShmProxyObject::convertScalarToJsi(jsi::Runtime& rt,
                                               shm_value_type_t type,
                                               const void* payload,
                                               size_t len) {
    switch (type) {
        case SHM_TYPE_INT_SCALAR: {
            int64_t value = *static_cast<const int64_t*>(payload);
            return jsi::Value(static_cast<double>(value));
        }

        case SHM_TYPE_FLOAT_SCALAR: {
            double value = *static_cast<const double*>(payload);
            return jsi::Value(value);
        }

        case SHM_TYPE_BOOL_SCALAR: {
            uint8_t value = *static_cast<const uint8_t*>(payload);
            return jsi::Value(value != 0);
        }

        case SHM_TYPE_STRING: {
            // Format: [length:4][data:N]
            const uint8_t* ptr = static_cast<const uint8_t*>(payload);
            uint32_t strLen = *reinterpret_cast<const uint32_t*>(ptr);
            const uint8_t* strData = ptr + 4;
            return jsi::String::createFromUtf8(rt, strData, strLen);
        }

        default:
            return jsi::Value::undefined();
    }
}

jsi::Value ShmProxyObject::convertVectorToJsi(jsi::Runtime& rt,
                                               shm_value_type_t type,
                                               const void* payload,
                                               size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(payload);

    switch (type) {
        case SHM_TYPE_FLOAT_VECTOR: {
            // Format: [count:4][data:8*count]
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const double* data = reinterpret_cast<const double*>(ptr + 4);

            jsi::Array arr = jsi::Array(rt, count);
            for (uint32_t i = 0; i < count; ++i) {
                arr.setValueAtIndex(rt, i, jsi::Value(data[i]));
            }
            return arr;
        }

        case SHM_TYPE_INT_VECTOR: {
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const int64_t* data = reinterpret_cast<const int64_t*>(ptr + 4);

            jsi::Array arr = jsi::Array(rt, count);
            for (uint32_t i = 0; i < count; ++i) {
                arr.setValueAtIndex(rt, i, jsi::Value(static_cast<double>(data[i])));
            }
            return arr;
        }

        case SHM_TYPE_BOOL_VECTOR: {
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const uint8_t* data = ptr + 4;

            jsi::Array arr = jsi::Array(rt, count);
            for (uint32_t i = 0; i < count; ++i) {
                arr.setValueAtIndex(rt, i, jsi::Value(data[i] != 0));
            }
            return arr;
        }

        case SHM_TYPE_STRING_VECTOR: {
            // Format: [count:4][offsets:4*(count+1)][string_data:N]
            uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
            const uint32_t* offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
            const char* stringData = reinterpret_cast<const char*>(
                ptr + 4 + 4 * (count + 1));

            jsi::Array arr = jsi::Array(rt, count);
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t start = offsets[i];
                uint32_t end = offsets[i + 1];
                std::string str(stringData + start, end - start);
                arr.setValueAtIndex(rt, i, jsi::String::createFromUtf8(rt, str));
            }
            return arr;
        }

        default:
            return jsi::Value::undefined();
    }
}

// ============================================================================
// ShmProxyArray Implementation
// ============================================================================

ShmProxyArray::ShmProxyArray(shm_handle_t handle, const shm_list_view_t& listView)
    : handle_(handle), isList_(true), listView_(listView) {}

ShmProxyArray::ShmProxyArray(shm_handle_t handle, const double* data, size_t count)
    : handle_(handle), isFloatVector_(true), floatData_(data), floatCount_(count) {}

ShmProxyArray::ShmProxyArray(shm_handle_t handle, const int64_t* data, size_t count)
    : handle_(handle), isIntVector_(true), intData_(data), intCount_(count) {}

jsi::Value ShmProxyArray::get(jsi::Runtime& rt, const jsi::PropNameID& name) {
    std::string propName = name.utf8(rt);

    // Handle "length" property
    if (propName == "length") {
        if (isList_) {
            return jsi::Value(static_cast<int>(listView_.count));
        } else if (isFloatVector_) {
            return jsi::Value(static_cast<int>(floatCount_));
        } else if (isIntVector_) {
            return jsi::Value(static_cast<int>(intCount_));
        }
        return jsi::Value(0);
    }

    // Try to parse as index
    char* endptr;
    long index = strtol(propName.c_str(), &endptr, 10);
    if (*endptr != '\0' || index < 0) {
        return jsi::Value::undefined();
    }

    size_t idx = static_cast<size_t>(index);

    if (isList_) {
        if (idx >= listView_.count) {
            return jsi::Value::undefined();
        }

        shm_typed_value_view_t elemView;
        shm_error_t err = shm_list_get_element(&listView_, idx, &elemView);
        if (err != SHM_OK) {
            return jsi::Value::undefined();
        }

        // Convert based on type
        switch (elemView.type) {
            case SHM_TYPE_NULL:
                return jsi::Value::null();
            case SHM_TYPE_INT_SCALAR: {
                int64_t value = *static_cast<const int64_t*>(elemView.payload);
                return jsi::Value(static_cast<double>(value));
            }
            case SHM_TYPE_FLOAT_SCALAR: {
                double value = *static_cast<const double*>(elemView.payload);
                return jsi::Value(value);
            }
            case SHM_TYPE_BOOL_SCALAR: {
                uint8_t value = *static_cast<const uint8_t*>(elemView.payload);
                return jsi::Value(value != 0);
            }
            case SHM_TYPE_STRING: {
                const uint8_t* ptr = static_cast<const uint8_t*>(elemView.payload);
                uint32_t strLen = *reinterpret_cast<const uint32_t*>(ptr);
                const uint8_t* strData = ptr + 4;
                return jsi::String::createFromUtf8(rt, strData, strLen);
            }
            case SHM_TYPE_OBJECT:
            case SHM_TYPE_LIST: {
                // Return a proxy for nested access
                auto proxy = std::make_shared<ShmProxyObject>(handle_, elemView);
                return jsi::Object::createFromHostObject(rt, proxy);
            }
            default:
                return jsi::Value::undefined();
        }
    }
    else if (isFloatVector_) {
        if (idx >= floatCount_) {
            return jsi::Value::undefined();
        }
        return jsi::Value(floatData_[idx]);
    }
    else if (isIntVector_) {
        if (idx >= intCount_) {
            return jsi::Value::undefined();
        }
        return jsi::Value(static_cast<double>(intData_[idx]));
    }

    return jsi::Value::undefined();
}

std::vector<jsi::PropNameID> ShmProxyArray::getPropertyNames(jsi::Runtime& rt) {
    std::vector<jsi::PropNameID> names;
    names.push_back(jsi::PropNameID::forUtf8(rt, "length"));

    size_t count = 0;
    if (isList_) {
        count = listView_.count;
    } else if (isFloatVector_) {
        count = floatCount_;
    } else if (isIntVector_) {
        count = intCount_;
    }

    for (size_t i = 0; i < count; ++i) {
        names.push_back(jsi::PropNameID::forUtf8(rt, std::to_string(i)));
    }

    return names;
}

} // namespace facebook::react
