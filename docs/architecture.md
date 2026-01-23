# Architecture Design

## Overview

ShmProxy 项目使用共享内存（Shared Memory）来实现高性能的 Native 到 JavaScript 数据传输。

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     React Native App                          │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌──────────────────┐         ┌──────────────────┐         │
│  │  ShmProxy        │         │  ShmProxyLazy    │         │
│  │  (Full Convert)  │         │  (ES6 Proxy)     │         │
│  └────────┬─────────┘         └────────┬─────────┘         │
│           │                            │                     │
│           └──────────┬─────────────────┘                     │
│                      ▼                                       │
│           ┌─────────────────────┐                            │
│           │   Shared Memory     │                            │
│           │   (Zero Copy)       │                            │
│           └─────────────────────┘                            │
└─────────────────────────────────────────────────────────────┘
                      ▲
                      │
┌─────────────────────────────────────────────────────────────┐
│                    Native (iOS)                              │
├─────────────────────────────────────────────────────────────┤
│           ┌─────────────────────┐                            │
│           │  NSDictionary       │                            │
│           └──────────┬──────────┘                            │
│                      ▼                                       │
│           ┌─────────────────────┐                            │
│           │  SHM Binary Format  │                            │
│           └─────────────────────┘                            │
└─────────────────────────────────────────────────────────────┘
```

## Components

### 1. Shared Memory (SHM)

共享内存是实现零拷贝数据传输的核心。

**特性**:
- 进程间共享
- 零拷贝访问
- 二进制格式存储

**配置**:
```objc
shm_handle_t g_shmHandle = shm_create(
    "/rn_shm_proxy",    // name
    4096,               // n_buckets
    65536,              // n_nodes
    64 * 1024 * 1024    // payload capacity (64MB)
);
```

### 2. ShmProxy Module

全量转换方案，使用 JSI 直接创建 JavaScript 对象。

**数据流**:
```
NSDictionary (Native)
    ↓ convertNSDictionaryToShm()
SHM Binary Format
    ↓ ShmProxyObject::convertTopLevelToJsObject()
jsi::Object (JavaScript)
```

**关键类**:
- `ShmProxyModule`: Native Module 桥接
- `ShmProxyObject`: HostObject 实现（未直接使用）
- `NSDictionaryToShm`: NSDictionary → SHM 转换

### 3. ShmProxyLazy Module

懒加载方案，使用 ES6 Proxy 实现按需访问。

**数据流**:
```
NSDictionary (Native)
    ↓ convertNSDictionaryToShm()
SHM Binary Format
    ↓ createShmProxyLazy()
ES6 Proxy (JavaScript)
    ↓ get() trap
__shmProxyLazy_getField(key, fieldPath)
    ↓ navigateObjectPath()
Single Field Conversion
    ↓
jsi::Value (JavaScript)
```

**关键组件**:
- `ShmProxyLazyModule`: Native Module + JSI 绑定
- `ES6 Proxy Handler`: JavaScript 拦截器
- `navigateObjectPath()`: 零拷贝路径导航

## SHM Data Format

### Object Structure

```
┌────────────────────────────────────────────────────────┐
│ Object View                                            │
├────────────────────────────────────────────────────────┤
│ count (uint32_t)                                       │
├────────────────────────────────────────────────────────┤
│ name_offsets[] (uint32_t[count + 1])                   │
│  [offset_0, offset_1, ..., offset_n, END]              │
├────────────────────────────────────────────────────────┤
│ names_data (char[])                                    │
│  "name1\0name2\0..."                                  │
├────────────────────────────────────────────────────────┤
│ field_types[] (uint8_t[count])                         │
│  [type_0, type_1, ..., type_n]                        │
├────────────────────────────────────────────────────────┤
│ value_offsets[] (uint32_t[count + 1])                  │
│  [offset_0, offset_1, ..., offset_n, END]              │
├────────────────────────────────────────────────────────┤
│ values_data (uint8_t[])                                │
│  [field_0_data, field_1_data, ...]                    │
└────────────────────────────────────────────────────────┘
```

### Value Types

```cpp
enum shm_value_type_t {
    SHM_TYPE_NULL = 0,
    SHM_TYPE_INT_SCALAR,    // int64_t
    SHM_TYPE_FLOAT_SCALAR,  // double
    SHM_TYPE_BOOL_SCALAR,   // uint8_t
    SHM_TYPE_STRING,        // uint32_t length + char[]
    SHM_TYPE_INT_VECTOR,    // uint32_t count + int64_t[]
    SHM_TYPE_FLOAT_VECTOR,  // uint32_t count + double[]
    SHM_TYPE_BOOL_VECTOR,   // uint32_t count + uint8_t[]
    SHM_TYPE_STRING_VECTOR, // uint32_t count + (length + char[])[]
    SHM_TYPE_OBJECT,        // ObjectView
    SHM_TYPE_LIST           // ListView
};
```

## JSI Bindings

### ShmProxy JSI Functions

```cpp
// 全局函数
__shm_write(obj: object): string
__shm_read(key: string): object
__shm_getStats(): object
__shm_clear(): void
```

### ShmProxyLazy JSI Functions

```cpp
// 全局函数
__shmProxyLazy_getField(key: string, fieldPath: string): any
__shmProxyLazy_getKeys(key: string): string[]
__shmProxyLazy_materialize(key: string): object
__shmProxyLazy_getStats(): object
```

## Performance Optimization

### 1. Zero-Copy

`shm_object_view_t` 直接指向 SHM 内存，无需复制：

```cpp
typedef struct {
    uint32_t count;
    const uint32_t* name_offsets;      // Points to SHM
    const char* names_data;            // Points to SHM
    const uint8_t* field_types;        // Points to SHM
    const uint32_t* value_offsets;     // Points to SHM
    const uint8_t* values_data;        // Points to SHM
} shm_object_view_t;
```

### 2. Lazy Loading (ShmProxyLazy)

- **Proxy 创建**: ~2.7ms（只创建 wrapper）
- **字段访问**: ~0.2ms per field（第一次访问）
- **缓存**: 后续访问无 SHM 调用

### 3. Path Navigation

直接路径导航，避免逐层 HostObject 创建：

```cpp
// ❌ ShmProxy: 逐层 HostObject
proxy.get("song") → HostObject
  .get("segments") → HostObject
    .get(0) → HostObject
      .get("pitches") → conversion

// ✅ ShmProxyLazy: 直接路径
proxy.get("song.segments[0].pitches")
  → __shmProxyLazy_getField(key, "song.segments.0.pitches")
    → navigateObjectPath()
    → single conversion
```

## Memory Management

### Initialization

```objc
- (instancetype)init {
    if (self = [super init]) {
        [self initializeShm];
    }
    return self;
}

- (void)initializeShm {
    g_shmHandle = shm_create(
        "/rn_shm_proxy",
        4096,              // buckets
        65536,             // nodes
        64 * 1024 * 1024  // 64MB
    );
}
```

### Cleanup

```objc
- (void)invalidate {
    if (g_shmHandle != nullptr) {
        shm_close(g_shmHandle);
        shm_destroy("/rn_shm_proxy");
        g_shmHandle = nullptr;
    }
}
```

## Thread Safety

### Current Implementation

- **读操作**: 完全线程安全
- **写操作**: 需要外部同步（SHM 支持并发读，但写需要锁）
- **建议**: 单线程写入，多线程读取

### Future Improvements

- [ ] 添加读写锁
- [ ] 支持并发写入
- [ ] 添加原子操作支持

## Android Support (Planned)

目前仅支持 iOS。Android 支持计划在未来的版本中实现。

---

## Related Documents

- [API Reference](./api-reference.md)
- [Performance Analysis](./performance.md)
- [Installation Guide](./installation.md)
