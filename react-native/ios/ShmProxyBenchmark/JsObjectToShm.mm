/**
 * JsObjectToShm.mm
 *
 * Implementation of JS Object to SharedMemory conversion.
 * This mirrors NSDictionaryToShm.mm but works in the opposite direction.
 */

#include "JsObjectToShm.h"
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

namespace facebook::react {

// ============================================================================
// Thread-local buffers to avoid frequent memory allocation
// ============================================================================
struct JsToShmBuffers {
    std::vector<uint8_t> payload;
    std::vector<std::string> fieldNames;
    std::vector<uint8_t> fieldTypes;
    std::vector<std::vector<uint8_t>> fieldPayloads;

    JsToShmBuffers() {
        payload.reserve(64 * 1024);
        fieldNames.reserve(64);
        fieldTypes.reserve(64);
        fieldPayloads.reserve(64);
    }

    void clear() {
        payload.clear();
        fieldNames.clear();
        fieldTypes.clear();
        fieldPayloads.clear();
    }
};

static thread_local JsToShmBuffers g_jsToShmBuffers;

// ============================================================================
// Alignment helper
// ============================================================================
static inline size_t alignUp4(size_t x) {
    return (x + 3) & ~3;
}

// ============================================================================
// Forward declaration
// ============================================================================
static shm_error_t encodeJsValue(jsi::Runtime& rt,
                                  const jsi::Value& value,
                                  std::vector<uint8_t>& outPayload,
                                  shm_value_type_t& outType);

// ============================================================================
// Encode number (int or float)
// ============================================================================
static shm_error_t encodeJsNumber(double num,
                                   std::vector<uint8_t>& outPayload,
                                   shm_value_type_t& outType) {
    // Check if it's an integer
    if (num == static_cast<double>(static_cast<int64_t>(num)) &&
        num >= static_cast<double>(INT64_MIN) &&
        num <= static_cast<double>(INT64_MAX)) {
        outType = SHM_TYPE_INT_SCALAR;
        outPayload.resize(8);
        int64_t val = static_cast<int64_t>(num);
        memcpy(outPayload.data(), &val, 8);
    } else {
        outType = SHM_TYPE_FLOAT_SCALAR;
        outPayload.resize(8);
        memcpy(outPayload.data(), &num, 8);
    }
    return SHM_OK;
}

// ============================================================================
// Encode boolean
// ============================================================================
static shm_error_t encodeJsBool(bool val,
                                 std::vector<uint8_t>& outPayload,
                                 shm_value_type_t& outType) {
    outType = SHM_TYPE_BOOL_SCALAR;
    outPayload.resize(1);
    outPayload[0] = val ? 1 : 0;
    return SHM_OK;
}

// ============================================================================
// Encode string
// ============================================================================
static shm_error_t encodeJsString(jsi::Runtime& rt,
                                   const jsi::String& str,
                                   std::vector<uint8_t>& outPayload,
                                   shm_value_type_t& outType) {
    outType = SHM_TYPE_STRING;

    std::string utf8 = str.utf8(rt);
    uint32_t len = static_cast<uint32_t>(utf8.size());

    outPayload.resize(4 + len);
    memcpy(outPayload.data(), &len, 4);
    memcpy(outPayload.data() + 4, utf8.data(), len);

    return SHM_OK;
}

// ============================================================================
// Check if array is homogeneous numeric
// ============================================================================
static bool isHomogeneousNumericArray(jsi::Runtime& rt, const jsi::Array& arr) {
    size_t len = arr.size(rt);
    if (len == 0) return false;

    // Check first element
    jsi::Value first = arr.getValueAtIndex(rt, 0);
    if (!first.isNumber()) return false;

    // Check all elements (for safety, but could optimize to check only first)
    for (size_t i = 1; i < len; ++i) {
        if (!arr.getValueAtIndex(rt, i).isNumber()) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Check if array is homogeneous boolean
// ============================================================================
static bool isHomogeneousBoolArray(jsi::Runtime& rt, const jsi::Array& arr) {
    size_t len = arr.size(rt);
    if (len == 0) return false;

    // Check first element
    jsi::Value first = arr.getValueAtIndex(rt, 0);
    if (!first.isBool()) return false;

    // Check all elements
    for (size_t i = 1; i < len; ++i) {
        if (!arr.getValueAtIndex(rt, i).isBool()) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Encode array
// ============================================================================
static shm_error_t encodeJsArray(jsi::Runtime& rt,
                                  const jsi::Array& arr,
                                  std::vector<uint8_t>& outPayload,
                                  shm_value_type_t& outType) {
    size_t count = arr.size(rt);

    if (count == 0) {
        outType = SHM_TYPE_LIST;
        outPayload.resize(4);
        uint32_t zero = 0;
        memcpy(outPayload.data(), &zero, 4);
        return SHM_OK;
    }

    // Fast path: homogeneous numeric array
    if (isHomogeneousNumericArray(rt, arr)) {
        outType = SHM_TYPE_FLOAT_VECTOR;
        outPayload.resize(4 + count * 8);

        uint32_t cnt = static_cast<uint32_t>(count);
        memcpy(outPayload.data(), &cnt, 4);

        double* dataPtr = reinterpret_cast<double*>(outPayload.data() + 4);
        for (size_t i = 0; i < count; ++i) {
            dataPtr[i] = arr.getValueAtIndex(rt, i).asNumber();
        }
        return SHM_OK;
    }

    // Fast path: homogeneous boolean array
    if (isHomogeneousBoolArray(rt, arr)) {
        outType = SHM_TYPE_BOOL_VECTOR;
        outPayload.resize(4 + count);

        uint32_t cnt = static_cast<uint32_t>(count);
        memcpy(outPayload.data(), &cnt, 4);

        uint8_t* dataPtr = outPayload.data() + 4;
        for (size_t i = 0; i < count; ++i) {
            dataPtr[i] = arr.getValueAtIndex(rt, i).getBool() ? 1 : 0;
        }
        return SHM_OK;
    }

    // General path: heterogeneous array (LIST)
    outType = SHM_TYPE_LIST;

    std::vector<std::vector<uint8_t>> elemPayloads;
    std::vector<uint8_t> elemTypes;
    elemPayloads.reserve(count);
    elemTypes.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> payload;
        payload.reserve(64);
        shm_value_type_t type;

        jsi::Value elem = arr.getValueAtIndex(rt, i);
        shm_error_t err = encodeJsValue(rt, elem, payload, type);
        if (err != SHM_OK) return err;

        elemPayloads.push_back(std::move(payload));
        elemTypes.push_back(static_cast<uint8_t>(type));
    }

    // Calculate total size
    size_t totalValues = 0;
    for (const auto& p : elemPayloads) {
        totalValues += p.size();
    }

    size_t headerSize = 4;
    size_t typesSize = count;
    size_t afterTypes = alignUp4(headerSize + typesSize);
    size_t offsetsSize = 4 * (count + 1);
    size_t totalSize = afterTypes + offsetsSize + totalValues;

    outPayload.resize(totalSize);
    uint8_t* ptr = outPayload.data();

    // Write count
    uint32_t cnt = static_cast<uint32_t>(count);
    memcpy(ptr, &cnt, 4);

    // Write elem_types
    memcpy(ptr + 4, elemTypes.data(), count);

    // Write value_offsets and values_blob
    uint32_t* offsets = reinterpret_cast<uint32_t*>(ptr + afterTypes);
    uint8_t* valuesBlob = ptr + afterTypes + offsetsSize;

    uint32_t currentOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        offsets[i] = currentOffset;
        memcpy(valuesBlob + currentOffset, elemPayloads[i].data(), elemPayloads[i].size());
        currentOffset += static_cast<uint32_t>(elemPayloads[i].size());
    }
    offsets[count] = currentOffset;

    return SHM_OK;
}

// ============================================================================
// Encode object
// ============================================================================
static shm_error_t encodeJsObject(jsi::Runtime& rt,
                                   const jsi::Object& obj,
                                   std::vector<uint8_t>& outPayload,
                                   shm_value_type_t& outType) {
    outType = SHM_TYPE_OBJECT;

    // Get property names
    jsi::Array propNames = obj.getPropertyNames(rt);
    size_t count = propNames.size(rt);

    if (count == 0) {
        outPayload.resize(4);
        uint32_t zero = 0;
        memcpy(outPayload.data(), &zero, 4);
        return SHM_OK;
    }

    // Collect and sort field names
    struct Field {
        std::string name;
        uint8_t type;
        std::vector<uint8_t> payload;
    };
    std::vector<Field> fields;
    fields.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        Field f;
        jsi::String propName = propNames.getValueAtIndex(rt, i).asString(rt);
        f.name = propName.utf8(rt);

        jsi::Value value = obj.getProperty(rt, propName);
        f.payload.reserve(64);
        shm_error_t err = encodeJsValue(rt, value, f.payload, reinterpret_cast<shm_value_type_t&>(f.type));
        if (err != SHM_OK) return err;

        fields.push_back(std::move(f));
    }

    // Sort by field name
    std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) {
        return a.name < b.name;
    });

    // Calculate sizes
    size_t totalNames = 0;
    size_t totalValues = 0;
    for (const auto& f : fields) {
        totalNames += f.name.size();
        totalValues += f.payload.size();
    }

    size_t headerSize = 4;
    size_t nameOffsetsSize = 4 * (count + 1);
    size_t fieldTypesSize = count;
    size_t afterTypes = alignUp4(headerSize + nameOffsetsSize + totalNames + fieldTypesSize);
    size_t valueOffsetsSize = 4 * (count + 1);
    size_t totalSize = afterTypes + valueOffsetsSize + totalValues;

    outPayload.resize(totalSize);
    uint8_t* ptr = outPayload.data();

    // Write count
    uint32_t cnt = static_cast<uint32_t>(count);
    memcpy(ptr, &cnt, 4);

    // Write name_offsets and names_blob
    uint32_t* nameOffsets = reinterpret_cast<uint32_t*>(ptr + 4);
    char* namesBlob = reinterpret_cast<char*>(ptr + 4 + nameOffsetsSize);

    uint32_t nameOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        nameOffsets[i] = nameOffset;
        memcpy(namesBlob + nameOffset, fields[i].name.data(), fields[i].name.size());
        nameOffset += static_cast<uint32_t>(fields[i].name.size());
    }
    nameOffsets[count] = nameOffset;

    // Write field_types
    uint8_t* fieldTypes = reinterpret_cast<uint8_t*>(namesBlob + totalNames);
    for (size_t i = 0; i < count; ++i) {
        fieldTypes[i] = fields[i].type;
    }

    // Write value_offsets and values_blob
    uint32_t* valueOffsets = reinterpret_cast<uint32_t*>(ptr + afterTypes);
    uint8_t* valuesBlob = ptr + afterTypes + valueOffsetsSize;

    uint32_t valueOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        valueOffsets[i] = valueOffset;
        memcpy(valuesBlob + valueOffset, fields[i].payload.data(), fields[i].payload.size());
        valueOffset += static_cast<uint32_t>(fields[i].payload.size());
    }
    valueOffsets[count] = valueOffset;

    return SHM_OK;
}

// ============================================================================
// Main encode function
// ============================================================================
static shm_error_t encodeJsValue(jsi::Runtime& rt,
                                  const jsi::Value& value,
                                  std::vector<uint8_t>& outPayload,
                                  shm_value_type_t& outType) {
    // Handle null/undefined
    if (value.isNull() || value.isUndefined()) {
        outType = SHM_TYPE_NULL;
        outPayload.clear();
        return SHM_OK;
    }

    // Handle boolean
    if (value.isBool()) {
        return encodeJsBool(value.getBool(), outPayload, outType);
    }

    // Handle number
    if (value.isNumber()) {
        return encodeJsNumber(value.asNumber(), outPayload, outType);
    }

    // Handle string
    if (value.isString()) {
        return encodeJsString(rt, value.asString(rt), outPayload, outType);
    }

    // Handle object (including array)
    if (value.isObject()) {
        jsi::Object obj = value.asObject(rt);

        // Check if it's an array
        if (obj.isArray(rt)) {
            return encodeJsArray(rt, obj.asArray(rt), outPayload, outType);
        }

        // Regular object
        return encodeJsObject(rt, obj, outPayload, outType);
    }

    return SHM_ERR_INVALID_PARAM;
}

// ============================================================================
// Public API
// ============================================================================
shm_error_t convertJsObjectToShm(jsi::Runtime& rt,
                                  shm_handle_t handle,
                                  const std::string& key,
                                  const jsi::Object& obj) {
    if (!handle || key.empty()) {
        return SHM_ERR_INVALID_PARAM;
    }

    // Clear thread-local buffers
    g_jsToShmBuffers.clear();

    // Get property names
    jsi::Array propNames = obj.getPropertyNames(rt);
    size_t count = propNames.size(rt);

    // Reserve space
    g_jsToShmBuffers.fieldNames.reserve(count);
    g_jsToShmBuffers.fieldTypes.reserve(count);
    g_jsToShmBuffers.fieldPayloads.reserve(count);

    // Collect fields
    for (size_t i = 0; i < count; ++i) {
        jsi::String propName = propNames.getValueAtIndex(rt, i).asString(rt);
        std::string name = propName.utf8(rt);

        jsi::Value value = obj.getProperty(rt, propName);

        std::vector<uint8_t> payload;
        payload.reserve(128);
        shm_value_type_t type;
        shm_error_t err = encodeJsValue(rt, value, payload, type);
        if (err != SHM_OK) return err;

        g_jsToShmBuffers.fieldNames.push_back(std::move(name));
        g_jsToShmBuffers.fieldTypes.push_back(static_cast<uint8_t>(type));
        g_jsToShmBuffers.fieldPayloads.push_back(std::move(payload));
    }

    // Sort by field name (create sorted indices)
    std::vector<size_t> sortedIndices(count);
    for (size_t i = 0; i < count; ++i) {
        sortedIndices[i] = i;
    }
    std::sort(sortedIndices.begin(), sortedIndices.end(), [](size_t a, size_t b) {
        return g_jsToShmBuffers.fieldNames[a] < g_jsToShmBuffers.fieldNames[b];
    });

    // Build sorted arrays for shm_insert_object
    std::vector<const char*> sortedNames;
    std::vector<size_t> sortedNameLengths;
    std::vector<uint8_t> sortedTypes;
    std::vector<const void*> sortedPayloads;
    std::vector<size_t> sortedPayloadLengths;

    sortedNames.reserve(count);
    sortedNameLengths.reserve(count);
    sortedTypes.reserve(count);
    sortedPayloads.reserve(count);
    sortedPayloadLengths.reserve(count);

    for (size_t idx : sortedIndices) {
        sortedNames.push_back(g_jsToShmBuffers.fieldNames[idx].c_str());
        sortedNameLengths.push_back(g_jsToShmBuffers.fieldNames[idx].size());
        sortedTypes.push_back(g_jsToShmBuffers.fieldTypes[idx]);
        sortedPayloads.push_back(g_jsToShmBuffers.fieldPayloads[idx].data());
        sortedPayloadLengths.push_back(g_jsToShmBuffers.fieldPayloads[idx].size());
    }

    return shm_insert_object(
        handle,
        key.c_str(),
        key.size(),
        sortedNames.data(),
        sortedNameLengths.data(),
        sortedTypes.data(),
        sortedPayloads.data(),
        sortedPayloadLengths.data(),
        count
    );
}

shm_error_t convertJsArrayToShm(jsi::Runtime& rt,
                                 shm_handle_t handle,
                                 const std::string& key,
                                 const jsi::Array& arr) {
    if (!handle || key.empty()) {
        return SHM_ERR_INVALID_PARAM;
    }

    size_t count = arr.size(rt);

    std::vector<uint8_t> elemTypes;
    std::vector<std::vector<uint8_t>> elemPayloads;
    elemTypes.reserve(count);
    elemPayloads.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> payload;
        payload.reserve(64);
        shm_value_type_t type;

        jsi::Value elem = arr.getValueAtIndex(rt, i);
        shm_error_t err = encodeJsValue(rt, elem, payload, type);
        if (err != SHM_OK) return err;

        elemTypes.push_back(static_cast<uint8_t>(type));
        elemPayloads.push_back(std::move(payload));
    }

    std::vector<const void*> payloadPtrs;
    std::vector<size_t> payloadLengths;
    payloadPtrs.reserve(count);
    payloadLengths.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        payloadPtrs.push_back(elemPayloads[i].data());
        payloadLengths.push_back(elemPayloads[i].size());
    }

    return shm_insert_list(
        handle,
        key.c_str(),
        key.size(),
        elemTypes.data(),
        payloadPtrs.data(),
        payloadLengths.data(),
        count
    );
}

} // namespace facebook::react
