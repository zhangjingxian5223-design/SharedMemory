# ShmProxy

> 使用共享内存（Shared Memory）优化 React Native 数据传输

[![npm version](https://badge.fury.io/js/react-native-shmproxy.svg)](https://www.npmjs.com/package/react-native-shmproxy)

## 📖 简介

ShmProxy 通过共享内存（Shared Memory）实现了 Native 到 JavaScript 的高性能数据传输，避免了传统 Bridge 方法的序列化开销。

### 适用场景

✅ **大数据传输** (>1MB)
✅ **需要完整对象访问**
✅ **对性能敏感的应用**
✅ **需要同步访问数据**

### 性能数据

- **1MB 数据**: 比传统方法快 **56.8%**
- **20MB 数据**: 比传统方法快 **57.2%**
- **访问速度**: 数据已在内存中，无额外开销

## 🚀 快速开始

### 安装

```bash
npm install react-native-shmproxy
cd ios && pod install
```

### 基础用法

```typescript
import { ShmProxy } from 'react-native-shmproxy';

// 1. 写入数据
const shmKey = await ShmProxy.write({
  user: {
    name: 'Alice',
    age: 30,
    preferences: {
      theme: 'dark',
      notifications: true
    }
  },
  items: [...thousands of items]
});

// 2. 读取数据（全量转换）
const data = await ShmProxy.read(shmKey);

// 3. 使用数据
console.log(data.user.name);
console.log(data.items.length);
```

## 📚 API 文档

### `ShmProxy.write(data)`

将 JavaScript 对象写入共享内存。

**参数:**
- `data: Record<string, any>` - 要写入的对象

**返回:**
- `Promise<string>` - SHM key

**示例:**
```typescript
const key = await ShmProxy.write({
  song: { title: 'Hello', year: 2024 }
});
```

### `ShmProxy.read(key, options?)`

从共享内存读取并完全转换为 JavaScript 对象。

**参数:**
- `key: string` - SHM key
- `options?: ShmReadOptions`
  - `consume?: boolean` - 读取后删除数据

**返回:**
- `Promise<Record<string, any>>` - JavaScript 对象

**示例:**
```typescript
const data = await ShmProxy.read(key);

// 读取并删除
const data = await ShmProxy.read(key, { consume: true });
```

### `ShmProxy.getStats()`

获取共享内存统计信息。

**返回:**
- `Promise<ShmStats>`
  - `buckets: number` - 哈希桶数量
  - `nodes: number` - 节点总数
  - `nodesUsed: number` - 已使用节点数
  - `payloadCapacity: number` - 容量（字节）
  - `payloadUsed: number` - 已使用量（字节）

**示例:**
```typescript
const stats = await ShmProxy.getStats();
console.log(`Memory: ${stats.payloadUsed}/${stats.payloadCapacity} bytes`);
```

### `ShmProxy.clear()`

清空共享内存。

**示例:**
```typescript
await ShmProxy.clear();
```

## 🔧 高级用法

### JSI 同步函数

对于需要同步访问的场景，可以使用 JSI 函数：

```typescript
declare global {
  var __shm_write: (obj: any) => string;
  var __shm_read: (key: string) => any;
}

// 同步写入（极快）
const key = __shm_write(largeObject);

// 同步读取
const data = __shm_read(key);
```

### 内存管理

```typescript
// 检查内存使用
const stats = await ShmProxy.getStats();
const usagePercent = (stats.payloadUsed / stats.payloadCapacity) * 100;

if (usagePercent > 80) {
  // 内存使用超过 80%，清理旧数据
  await ShmProxy.clear();
}
```

## 🏗️ 实现原理

### 数据流

```
NSDictionary (Native)
    ↓ convertNSDictionaryToShm()
Shared Memory (Binary Format)
    ↓ ShmProxyObject::convertTopLevelToJsObject()
jsi::Object (JavaScript)
```

### 关键优化

1. **零拷贝视图**: `shm_object_view_t` 直接指向 SHM 内存
2. **跳过 folly::dynamic**: 直接创建 jsi::Value
3. **二进制格式**: 比 JSON 更紧凑
4. **避免序列化**: 无需 JSON 字符串中间层

## 📊 性能对比

### Full Access 模式

| 数据大小 | Traditional | ShmProxy | 提升 |
|---------|-------------|----------|------|
| 128KB   | 11.13ms     | 4.75ms   | ▲57% |
| 1MB     | 64.50ms     | 35.05ms  | ▲46% |
| 20MB    | 1293.30ms   | 715.94ms | ▲45% |

### 何时选择 ShmProxy

**选择 ShmProxy:**
- ✅ 需要访问 >50% 的字段
- ✅ 需要完整对象的语义（JSON.stringify, Object.keys 等）
- ✅ 代码简洁性优先

**考虑 ShmProxyLazy:**
- ✅ 只访问 <20% 的字段
- ✅ 需要极致性能
- 见 [react-native-shmproxy-lazy](../shmproxy-lazy/)

## 🔗 相关链接

- [完整 API 文档](../../docs/api-reference.md#shmproxy)
- [架构设计](../../docs/architecture.md)
- [性能测试](../../docs/performance.md)

## 📄 许可证

MIT
