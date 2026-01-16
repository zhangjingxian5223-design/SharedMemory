/**
 * NSDictionaryToShm.mm - 优化版本 v2
 *
 * 优化点：
 * 1. 单次遍历 - 避免重复遍历 NSDictionary
 * 2. 预分配内存 - 使用 reserve 减少 realloc
 * 3. 快速类型检查 - 只检查第一个元素判断数组类型
 * 4. 避免临时对象 - 减少 NSData 创建
 * 5. 线程局部存储 - 复用 buffer
 */

#import "NSDictionaryToShm.h"
#import <objc/runtime.h>
#include <vector>
#include <string>
#include <cstring>

// ============================================================================
// 线程局部 Buffer - 避免频繁内存分配
// ============================================================================
struct ThreadLocalBuffers {
    std::vector<uint8_t> payload;
    std::vector<const char*> fieldNames;
    std::vector<size_t> fieldNameLengths;
    std::vector<uint8_t> fieldTypes;
    std::vector<const void*> fieldPayloads;
    std::vector<size_t> fieldPayloadLengths;
    std::vector<std::vector<uint8_t>> payloadStorage;
    std::vector<std::string> nameStorage;

    ThreadLocalBuffers() {
        payload.reserve(64 * 1024);
        fieldNames.reserve(64);
        fieldNameLengths.reserve(64);
        fieldTypes.reserve(64);
        fieldPayloads.reserve(64);
        fieldPayloadLengths.reserve(64);
        payloadStorage.reserve(64);
        nameStorage.reserve(64);
    }

    void clear() {
        payload.clear();
        fieldNames.clear();
        fieldNameLengths.clear();
        fieldTypes.clear();
        fieldPayloads.clear();
        fieldPayloadLengths.clear();
        payloadStorage.clear();
        nameStorage.clear();
    }
};

static thread_local ThreadLocalBuffers g_buffers;

// ============================================================================
// 快速类型检查
// ============================================================================
static inline bool isBoolNSNumber(NSNumber* num) {
    // 检查是否是 CFBoolean
    return num == (id)kCFBooleanTrue || num == (id)kCFBooleanFalse;
}

static inline bool isNumericNSNumber(NSNumber* num) {
    // 排除布尔类型
    if (isBoolNSNumber(num)) {
        return false;
    }
    const char* type = [num objCType];
    char c = type[0];
    return c == _C_FLT || c == _C_DBL ||
           c == _C_INT || c == _C_UINT ||
           c == _C_LNG || c == _C_ULNG ||
           c == _C_LNG_LNG || c == _C_ULNG_LNG ||
           c == _C_SHT || c == _C_USHT ||
           c == _C_CHR || c == _C_UCHR;
}

// ============================================================================
// 对齐辅助
// ============================================================================
static inline size_t alignUp4(size_t x) {
    return (x + 3) & ~3;
}

// ============================================================================
// 前向声明
// ============================================================================
static shm_error_t encodeValueFast(id value,
                                    std::vector<uint8_t>& outPayload,
                                    shm_value_type_t& outType);

// ============================================================================
// 编码 NSNumber - 优化版
// ============================================================================
static inline shm_error_t encodeNumberFast(NSNumber* num,
                                            std::vector<uint8_t>& outPayload,
                                            shm_value_type_t& outType) {
    const char* objCType = [num objCType];

    switch (objCType[0]) {
        case _C_BOOL: {
            outType = SHM_TYPE_BOOL_SCALAR;
            outPayload.resize(1);
            outPayload[0] = [num boolValue] ? 1 : 0;
            return SHM_OK;
        }

        case _C_CHR: {
            // 检查是否是 boolean
            if (num == (id)kCFBooleanTrue || num == (id)kCFBooleanFalse) {
                outType = SHM_TYPE_BOOL_SCALAR;
                outPayload.resize(1);
                outPayload[0] = [num boolValue] ? 1 : 0;
            } else {
                outType = SHM_TYPE_INT_SCALAR;
                outPayload.resize(8);
                int64_t val = [num longLongValue];
                memcpy(outPayload.data(), &val, 8);
            }
            return SHM_OK;
        }

        case _C_UCHR:
        case _C_SHT:
        case _C_USHT:
        case _C_INT:
        case _C_UINT:
        case _C_LNG:
        case _C_ULNG:
        case _C_LNG_LNG:
        case _C_ULNG_LNG: {
            outType = SHM_TYPE_INT_SCALAR;
            outPayload.resize(8);
            int64_t val = [num longLongValue];
            memcpy(outPayload.data(), &val, 8);
            return SHM_OK;
        }

        case _C_FLT:
        case _C_DBL:
        default: {
            outType = SHM_TYPE_FLOAT_SCALAR;
            outPayload.resize(8);
            double val = [num doubleValue];
            memcpy(outPayload.data(), &val, 8);
            return SHM_OK;
        }
    }
}

// ============================================================================
// 编码 NSString - 优化版
// ============================================================================
static inline shm_error_t encodeStringFast(NSString* str,
                                            std::vector<uint8_t>& outPayload,
                                            shm_value_type_t& outType) {
    outType = SHM_TYPE_STRING;

    // 直接获取 UTF8，避免创建 NSData
    const char* utf8 = [str UTF8String];
    uint32_t len = (uint32_t)strlen(utf8);

    outPayload.resize(4 + len);
    memcpy(outPayload.data(), &len, 4);
    memcpy(outPayload.data() + 4, utf8, len);

    return SHM_OK;
}

// ============================================================================
// 编码 NSArray - 优化版
// ============================================================================
static shm_error_t encodeArrayFast(NSArray* arr,
                                    std::vector<uint8_t>& outPayload,
                                    shm_value_type_t& outType) {
    NSUInteger count = [arr count];

    if (count == 0) {
        outType = SHM_TYPE_LIST;
        outPayload.resize(4);
        uint32_t zero = 0;
        memcpy(outPayload.data(), &zero, 4);
        return SHM_OK;
    }

    // 快速路径：数值数组（只检查第一个元素）
    id first = arr[0];
    if ([first isKindOfClass:[NSNumber class]] && isNumericNSNumber((NSNumber*)first)) {
        outType = SHM_TYPE_FLOAT_VECTOR;

        outPayload.resize(4 + count * 8);
        uint32_t cnt = (uint32_t)count;
        memcpy(outPayload.data(), &cnt, 4);

        double* dataPtr = reinterpret_cast<double*>(outPayload.data() + 4);
        NSUInteger i = 0;
        for (NSNumber* num in arr) {
            dataPtr[i++] = [num doubleValue];
        }

        return SHM_OK;
    }

    // 快速路径：布尔数组（只检查第一个元素）
    if ([first isKindOfClass:[NSNumber class]] && isBoolNSNumber((NSNumber*)first)) {
        outType = SHM_TYPE_BOOL_VECTOR;

        outPayload.resize(4 + count);
        uint32_t cnt = (uint32_t)count;
        memcpy(outPayload.data(), &cnt, 4);

        uint8_t* dataPtr = outPayload.data() + 4;
        NSUInteger i = 0;
        for (NSNumber* num in arr) {
            dataPtr[i++] = [num boolValue] ? 1 : 0;
        }

        return SHM_OK;
    }

    // 通用路径：异构数组
    outType = SHM_TYPE_LIST;

    // 预分配
    std::vector<std::vector<uint8_t>> elemPayloads;
    std::vector<uint8_t> elemTypes;
    elemPayloads.reserve(count);
    elemTypes.reserve(count);

    for (id elem in arr) {
        std::vector<uint8_t> payload;
        payload.reserve(64);  // 预分配小 buffer
        shm_value_type_t type;
        shm_error_t err = encodeValueFast(elem, payload, type);
        if (err != SHM_OK) return err;
        elemPayloads.push_back(std::move(payload));
        elemTypes.push_back((uint8_t)type);
    }

    // 计算总大小
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

    // 写入 count
    uint32_t cnt = (uint32_t)count;
    memcpy(ptr, &cnt, 4);

    // 写入 elem_types
    memcpy(ptr + 4, elemTypes.data(), count);

    // 写入 value_offsets 和 values_blob
    uint32_t* offsets = reinterpret_cast<uint32_t*>(ptr + afterTypes);
    uint8_t* valuesBlob = ptr + afterTypes + offsetsSize;

    uint32_t currentOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        offsets[i] = currentOffset;
        memcpy(valuesBlob + currentOffset, elemPayloads[i].data(), elemPayloads[i].size());
        currentOffset += elemPayloads[i].size();
    }
    offsets[count] = currentOffset;

    return SHM_OK;
}

// ============================================================================
// 编码 NSDictionary - 优化版
// ============================================================================
static shm_error_t encodeDictionaryFast(NSDictionary* dict,
                                         std::vector<uint8_t>& outPayload,
                                         shm_value_type_t& outType) {
    outType = SHM_TYPE_OBJECT;

    NSArray* allKeys = [dict allKeys];
    NSUInteger count = [allKeys count];

    if (count == 0) {
        outPayload.resize(4);
        uint32_t zero = 0;
        memcpy(outPayload.data(), &zero, 4);
        return SHM_OK;
    }

    // 排序 keys
    NSArray* sortedKeys = [allKeys sortedArrayUsingSelector:@selector(compare:)];

    // 一次遍历：收集 key 名称和编码值
    struct Field {
        const char* name;
        size_t nameLen;
        uint8_t type;
        std::vector<uint8_t> payload;
    };
    std::vector<Field> fields;
    fields.reserve(count);

    for (NSString* key in sortedKeys) {
        Field f;
        f.name = [key UTF8String];
        f.nameLen = strlen(f.name);

        id value = dict[key];
        f.payload.reserve(64);
        shm_error_t err = encodeValueFast(value, f.payload, (shm_value_type_t&)f.type);
        if (err != SHM_OK) return err;

        fields.push_back(std::move(f));
    }

    // 计算大小
    size_t totalNames = 0;
    size_t totalValues = 0;
    for (const auto& f : fields) {
        totalNames += f.nameLen;
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

    // 写入 count
    uint32_t cnt = (uint32_t)count;
    memcpy(ptr, &cnt, 4);

    // 写入 name_offsets 和 names_blob
    uint32_t* nameOffsets = reinterpret_cast<uint32_t*>(ptr + 4);
    char* namesBlob = reinterpret_cast<char*>(ptr + 4 + nameOffsetsSize);

    uint32_t nameOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        nameOffsets[i] = nameOffset;
        memcpy(namesBlob + nameOffset, fields[i].name, fields[i].nameLen);
        nameOffset += fields[i].nameLen;
    }
    nameOffsets[count] = nameOffset;

    // 写入 field_types
    uint8_t* fieldTypes = reinterpret_cast<uint8_t*>(namesBlob + totalNames);
    for (size_t i = 0; i < count; ++i) {
        fieldTypes[i] = fields[i].type;
    }

    // 写入 value_offsets 和 values_blob
    uint32_t* valueOffsets = reinterpret_cast<uint32_t*>(ptr + afterTypes);
    uint8_t* valuesBlob = ptr + afterTypes + valueOffsetsSize;

    uint32_t valueOffset = 0;
    for (size_t i = 0; i < count; ++i) {
        valueOffsets[i] = valueOffset;
        memcpy(valuesBlob + valueOffset, fields[i].payload.data(), fields[i].payload.size());
        valueOffset += fields[i].payload.size();
    }
    valueOffsets[count] = valueOffset;

    return SHM_OK;
}

// ============================================================================
// 主编码函数 - 优化版
// ============================================================================
static shm_error_t encodeValueFast(id value,
                                    std::vector<uint8_t>& outPayload,
                                    shm_value_type_t& outType) {
    // 处理 null 值
    if (value == nil || value == (id)kCFNull || [value isKindOfClass:[NSNull class]]) {
        outType = SHM_TYPE_NULL;
        outPayload.clear();  // null 类型不需要 payload
        return SHM_OK;
    }

    if ([value isKindOfClass:[NSNumber class]]) {
        return encodeNumberFast((NSNumber*)value, outPayload, outType);
    }

    if ([value isKindOfClass:[NSString class]]) {
        return encodeStringFast((NSString*)value, outPayload, outType);
    }

    if ([value isKindOfClass:[NSArray class]]) {
        return encodeArrayFast((NSArray*)value, outPayload, outType);
    }

    if ([value isKindOfClass:[NSDictionary class]]) {
        return encodeDictionaryFast((NSDictionary*)value, outPayload, outType);
    }

    return SHM_ERR_INVALID_PARAM;
}

// ============================================================================
// Public API - 优化版
// ============================================================================
shm_error_t convertNSDictionaryToShm(shm_handle_t handle,
                                      const char* key,
                                      size_t keyLen,
                                      NSDictionary* dict) {
    if (!handle || !key || !dict) {
        return SHM_ERR_INVALID_PARAM;
    }

    // 清空线程局部 buffer
    g_buffers.clear();

    NSArray* allKeys = [dict allKeys];
    NSArray* sortedKeys = [allKeys sortedArrayUsingSelector:@selector(compare:)];
    NSUInteger count = [sortedKeys count];

    // 预分配
    g_buffers.fieldNames.reserve(count);
    g_buffers.fieldNameLengths.reserve(count);
    g_buffers.fieldTypes.reserve(count);
    g_buffers.fieldPayloads.reserve(count);
    g_buffers.fieldPayloadLengths.reserve(count);
    g_buffers.payloadStorage.reserve(count);
    g_buffers.nameStorage.reserve(count);

    // 一次遍历：编码所有字段
    for (NSString* fieldKey in sortedKeys) {
        // 存储 key 名称
        g_buffers.nameStorage.emplace_back([fieldKey UTF8String]);

        // 编码值
        std::vector<uint8_t> fieldPayload;
        fieldPayload.reserve(128);
        shm_value_type_t fieldType;
        shm_error_t err = encodeValueFast(dict[fieldKey], fieldPayload, fieldType);
        if (err != SHM_OK) return err;

        g_buffers.fieldTypes.push_back((uint8_t)fieldType);
        g_buffers.payloadStorage.push_back(std::move(fieldPayload));
    }

    // 设置指针（必须在所有数据存储后）
    for (size_t i = 0; i < count; ++i) {
        g_buffers.fieldNames.push_back(g_buffers.nameStorage[i].c_str());
        g_buffers.fieldNameLengths.push_back(g_buffers.nameStorage[i].size());
        g_buffers.fieldPayloads.push_back(g_buffers.payloadStorage[i].data());
        g_buffers.fieldPayloadLengths.push_back(g_buffers.payloadStorage[i].size());
    }

    return shm_insert_object(
        handle,
        key,
        keyLen,
        g_buffers.fieldNames.data(),
        g_buffers.fieldNameLengths.data(),
        g_buffers.fieldTypes.data(),
        g_buffers.fieldPayloads.data(),
        g_buffers.fieldPayloadLengths.data(),
        count
    );
}

shm_error_t convertNSArrayToShm(shm_handle_t handle,
                                 const char* key,
                                 size_t keyLen,
                                 NSArray* array) {
    if (!handle || !key || !array) {
        return SHM_ERR_INVALID_PARAM;
    }

    NSUInteger count = [array count];

    std::vector<uint8_t> elemTypes;
    std::vector<std::vector<uint8_t>> elemPayloads;
    elemTypes.reserve(count);
    elemPayloads.reserve(count);

    for (id elem in array) {
        std::vector<uint8_t> payload;
        payload.reserve(64);
        shm_value_type_t type;
        shm_error_t err = encodeValueFast(elem, payload, type);
        if (err != SHM_OK) return err;
        elemTypes.push_back((uint8_t)type);
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
        key,
        keyLen,
        elemTypes.data(),
        payloadPtrs.data(),
        payloadLengths.data(),
        count
    );
}
