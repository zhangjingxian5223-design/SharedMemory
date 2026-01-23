# API 参考文档

## ShmProxy API

### Class: ShmProxy

高性能数据传输，使用共享内存优化大数据传输。

---

#### `static async write(data: Record<string, any>): Promise<string>`

将 JavaScript 对象写入共享内存。

**参数:**
- `data: Record<string, any>` - 要写入的对象，可以包含嵌套对象、数组和基本类型

**返回:**
- `Promise<string>` - 返回 SHM key，用于后续读取

**抛出:**
- `ShmError` - 如果写入失败（如内存不足）

**示例:**
```typescript
const key = await ShmProxy.write({
  user: {
    name: 'Alice',
    age: 30,
    preferences: {
      theme: 'dark'
    }
  },
  items: [1, 2, 3]
});
```

**支持的类型:**
- `string`
- `number` (int, float)
- `boolean`
- `null`
- `object` (嵌套对象)
- `array` (包括 TypedArray)

---

#### `static async read(key: string, options?: ShmReadOptions): Promise<Record<string, any>>`

从共享内存读取并完全转换为 JavaScript 对象。

**参数:**
- `key: string` - `write()` 返回的 SHM key
- `options?: ShmReadOptions` - 可选配置
  - `consume?: boolean` - 如果为 true，读取后从 SHM 删除数据

**返回:**
- `Promise<Record<string, any>>` - 完整的 JavaScript 对象

**抛出:**
- `ShmError` - 如果 key 不存在或读取失败

**示例:**
```typescript
// 读取
const data = await ShmProxy.read(key);

// 读取并删除
const data = await ShmProxy.read(key, { consume: true });
```

---

#### `static async getStats(): Promise<ShmStats>`

获取共享内存统计信息。

**返回:**
- `Promise<ShmStats>` - 统计信息对象
  - `buckets: number` - 哈希桶数量
  - `nodes: number` - 节点总数
  - `nodesUsed: number` - 已使用节点数
  - `payloadCapacity: number` - 总容量（字节）
  - `payloadUsed: number` - 已使用量（字节）
  - `generation: number` - 世代计数（每次 clear 后递增）

**示例:**
```typescript
const stats = await ShmProxy.getStats();
console.log(`Memory: ${stats.payloadUsed}/${stats.payloadCapacity} bytes`);
console.log(`Usage: ${(stats.payloadUsed / stats.payloadCapacity * 100).toFixed(2)}%`);
```

---

#### `static async clear(): Promise<void>`

清空共享内存，释放所有数据。

**注意:** 调用后，所有已存在的 key 都会失效。

**示例:**
```typescript
await ShmProxy.clear();
console.log('Shared memory cleared');
```

---

#### `static async isInitialized(): Promise<boolean>`

检查共享内存是否已初始化。

**返回:**
- `Promise<boolean>` - 如果已初始化返回 true

**示例:**
```typescript
if (!await ShmProxy.isInitialized()) {
  await ShmProxy.initialize();
}
```

---

#### `static async initialize(config?: ShmProxyConfig): Promise<void>`

初始化共享内存（通常自动调用）。

**参数:**
- `config?: ShmProxyConfig` - 可选配置
  - `shmSize?: number` - SHM 大小（字节），默认 64MB
  - `buckets?: number` - 哈希桶数量，默认 4096
  - `nodes?: number` - 节点数量，默认 65536

**示例:**
```typescript
// 自定义 128MB SHM
await ShmProxy.initialize({ shmSize: 128 * 1024 * 1024 });
```

---

### Global JSI Functions

高级用法，直接使用 JSI 同步函数。

#### `__shm_write(obj: any): string`

同步写入对象到共享内存。

**参数:**
- `obj: any` - 要写入的对象

**返回:**
- `string` - SHM key

**示例:**
```typescript
declare global {
  var __shm_write: (obj: any) => string;
}

const key = __shm_write({ data: 'test' });
```

---

#### `__shm_read(key: string): any`

同步从共享内存读取对象。

**参数:**
- `key: string` - SHM key

**返回:**
- `any` - JavaScript 对象

**示例:**
```typescript
declare global {
  var __shm_read: (key: string) => any;
}

const data = __shm_read(key);
```

---

## ShmProxyLazy API

### Class: ShmProxyLazy

使用 ES6 Proxy 实现懒加载，按需访问数据。

---

#### `static async write(data: Record<string, any>): Promise<string>`

将 JavaScript 对象写入共享内存。

**参数:**
- `data: Record<string, any>` - 要写入的对象

**返回:**
- `Promise<string>` - SHM key

**示例:**
```typescript
const key = await ShmProxyLazy.write({
  song: { title: 'Hello', year: 2024 }
});
```

---

#### `static createProxy<T = any>(key: string, options?: CreateProxyOptions): T`

创建 ES6 Proxy 实现懒加载。

**参数:**
- `key: string` - SHM key
- `options?: CreateProxyOptions` - 可选配置
  - `basePath?: string` - 嵌套对象的基础路径（通常不需要）
  - `cache?: boolean` - 是否启用字段缓存，默认 true

**返回:**
- `<T>` - Proxy 对象，类型与原始数据相同

**示例:**
```typescript
interface SongData {
  song: {
    title: string;
    year: number;
  };
}

const data = ShmProxyLazy.createProxy<SongData>(key);

// 访问字段（按需转换）
console.log(data.song.title);
```

---

#### `static async materialize<T = any>(key: string): Promise<T>`

完全转换对象（用于需要访问所有字段的场景）。

**参数:**
- `key: string` - SHM key

**返回:**
- `Promise<T>` - 完整的 JavaScript 对象

**示例:**
```typescript
// 对于完整访问，materialize 比 Proxy 逐个访问更快
const data = await ShmProxyLazy.materialize(key);
console.log(Object.keys(data)); // 所有字段已转换
```

---

#### `static async getStats(): Promise<ShmStats>`

获取共享内存统计信息。

**返回:**
- `Promise<ShmStats>` - 统计信息

**示例:**
```typescript
const stats = await ShmProxyLazy.getStats();
console.log(`Usage: ${stats.payloadUsed}/${stats.payloadCapacity}`);
```

---

#### `static async clear(): Promise<void>`

清空共享内存。

**示例:**
```typescript
await ShmProxyLazy.clear();
```

---

#### `static async install(): Promise<void>`

安装 JSI 绑定（通常自动调用）。

**示例:**
```typescript
await ShmProxyLazy.install();
```

---

#### `static async isInitialized(): Promise<boolean>`

检查共享内存是否已初始化。

**返回:**
- `Promise<boolean>` - 如果已初始化返回 true

**示例:**
```typescript
if (!await ShmProxyLazy.isInitialized()) {
  await ShmProxyLazy.initialize();
}
```

---

### Global JSI Functions

#### `__shmProxyLazy_getField(key: string, fieldPath: string): any`

获取单个字段（支持嵌套路径）。

**参数:**
- `key: string` - SHM key
- `fieldPath: string` - 点号分隔的字段路径，如 `"song.title"`

**返回:**
- `any` - 字段值，或嵌套对象标记

**示例:**
```typescript
declare global {
  var __shmProxyLazy_getField: (key: string, fieldPath: string) => any;
}

const title = __shmProxyLazy_getField(key, 'song.title');
```

---

#### `__shmProxyLazy_getKeys(key: string): string[]`

获取对象的所有字段名。

**参数:**
- `key: string` - SHM key

**返回:**
- `string[]` - 字段名数组

**示例:**
```typescript
declare global {
  var __shmProxyLazy_getKeys: (key: string) => string[];
}

const keys = __shmProxyLazy_getKeys(key);
console.log(keys); // ['song', 'artist', 'year']
```

---

#### `__shmProxyLazy_materialize(key: string): any`

完全转换对象（同步版本）。

**参数:**
- `key: string` - SHM key

**返回:**
- `any` - 完整的 JavaScript 对象

**示例:**
```typescript
declare global {
  var __shmProxyLazy_materialize: (key: string) => any;
}

const data = __shmProxyLazy_materialize(key);
```

---

## Type Definitions

### ShmStats

共享内存统计信息。

```typescript
interface ShmStats {
  buckets: number;           // 哈希桶数量
  nodes: number;             // 节点总数
  nodesUsed: number;         // 已使用节点数
  payloadCapacity: number;   // 总容量（字节）
  payloadUsed: number;       // 已使用量（字节）
  generation: number;        // 世代计数
}
```

### ShmReadOptions

读取选项。

```typescript
interface ShmReadOptions {
  consume?: boolean;  // 读取后删除数据
}
```

### CreateProxyOptions

Proxy 创建选项。

```typescript
interface CreateProxyOptions {
  basePath?: string;  // 嵌套对象的基础路径
  cache?: boolean;    // 是否启用字段缓存
}
```

### ShmProxyConfig

ShmProxy 配置选项。

```typescript
interface ShmProxyConfig {
  shmSize?: number;   // SHM 大小（字节）
  buckets?: number;   // 哈希桶数量
  nodes?: number;     // 节点数量
}
```

### ShmErrorCode

错误码枚举。

```typescript
enum ShmErrorCode {
  OK = 0,                    // 成功
  ERR_INVALID_HANDLE = 1,    // 无效的句柄
  ERR_NO_SPACE = 2,          // 内存不足
  ERR_NOT_FOUND = 3,         // key 不存在
  ERR_TYPE_MISMATCH = 4,     // 类型不匹配
  ERR_KEY_EXISTS = 5,        // key 已存在
}
```

### ShmError

共享内存错误。

```typescript
class ShmError extends Error {
  constructor(
    public code: ShmErrorCode,
    message: string
  );
}
```

---

## 错误处理

### ShmError

所有 SHM 操作可能抛出 `ShmError`。

```typescript
import { ShmProxy, ShmError, ShmErrorCode } from 'react-native-shmproxy';

try {
  const key = await ShmProxy.write(hugeData);
} catch (error) {
  if (error instanceof ShmError) {
    if (error.code === ShmErrorCode.ERR_NO_SPACE) {
      console.error('Shared memory full!');
      await ShmProxy.clear();
    }
  }
}
```

---

## 最佳实践

### 1. 内存管理

```typescript
// 定期检查内存使用
const stats = await ShmProxy.getStats();
if (stats.payloadUsed / stats.payloadCapacity > 0.8) {
  await ShmProxy.clear();
}
```

### 2. 错误处理

```typescript
try {
  const key = await ShmProxy.write(data);
  const result = await ShmProxy.read(key);
} catch (error) {
  console.error('SHM operation failed:', error.message);
}
```

### 3. 选择合适的方法

```typescript
// 只访问少量字段 → ShmProxyLazy
const proxy = ShmProxyLazy.createProxy(key);
console.log(proxy.title);

// 访问大部分字段 → ShmProxy 或 materialize
const data = await ShmProxy.read(key);
// 或
const data = await ShmProxyLazy.materialize(key);
```

---

## 下一步

- [安装指南](./installation.md)
- [示例项目](../examples/)
- [性能分析](./performance.md)
