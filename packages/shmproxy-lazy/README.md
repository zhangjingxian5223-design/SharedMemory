# ShmProxyLazy

> 基于 ES6 Proxy 的懒加载实现，按需访问数据

[![npm version](https://badge.fury.io/js/react-native-shmproxy-lazy.svg)](https://www.npmjs.com/package/react-native-shmproxy-lazy)

## 📖 简介

ShmProxyLazy 使用 ES6 Proxy 拦截属性访问，只在真正访问字段时才从共享内存读取数据。

### 适用场景

✅ **只访问少量字段** (< 20%)
✅ **需要极致性能**
✅ **可以接受异步访问**
✅ **大数据传输**

### 性能数据

- **1MB 数据（部分访问）**: 比传统方法快 **65.3%**
- **20MB 数据（部分访问）**: 比传统方法快 **67.4%**
- **Proxy 创建**: 极快（< 3ms），只创建 wrapper

## 🚀 快速开始

### 安装

```bash
npm install react-native-shmproxy-lazy
cd ios && pod install
```

### 基础用法

```typescript
import { ShmProxyLazy } from 'react-native-shmproxy-lazy';

// 1. 写入数据
const shmKey = await ShmProxyLazy.write({
  song: {
    title: 'Hello World',
    year: 2024,
    artist: 'Artist Name',
    album: 'Album Name',
    genres: ['pop', 'rock'],
    segments: [...thousands of segments]
  }
});

// 2. 创建 Proxy（极快，不转换数据）
const data = ShmProxyLazy.createProxy(shmKey);

// 3. 访问字段（只转换访问的字段）
console.log(data.song.title);  // 只转换 song.title
console.log(data.song.year);   // 只转换 song.year

// segments 未被访问，不转换（节省大量时间）
```

## 📚 API 文档

### `ShmProxyLazy.write(data)`

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

### `ShmProxyLazy.createProxy(key, options?)`

创建 ES6 Proxy 实现懒加载。

**参数:**
- `key: string` - SHM key
- `options?: CreateProxyOptions`
  - `basePath?: string` - 嵌套对象的基础路径（内部使用）
  - `cache?: boolean` - 是否启用字段缓存（默认: true）

**返回:**
- `<T>` - Proxy 对象

**示例:**
```typescript
const proxy = ShmProxyLazy.createProxy(key);

// 带选项
const proxy = ShmProxyLazy.createProxy(key, {
  cache: true  // 启用缓存（推荐）
});
```

### `ShmProxyLazy.materialize(key)`

完全转换对象（用于需要访问所有字段的场景）。

**参数:**
- `key: string` - SHM key

**返回:**
- `Promise<T>` - 完整的 JavaScript 对象

**示例:**
```typescript
// 对于完整访问，使用 materialize 更快
const data = await ShmProxyLazy.materialize(key);
console.log(Object.keys(data));  // 所有字段已转换
```

### `ShmProxyLazy.getStats()`

获取共享内存统计信息。

**示例:**
```typescript
const stats = await ShmProxyLazy.getStats();
console.log(`Memory: ${stats.payloadUsed}/${stats.payloadCapacity}`);
```

### `ShmProxyLazy.clear()`

清空共享内存。

**示例:**
```typescript
await ShmProxyLazy.clear();
```

## 🔧 高级用法

### 嵌套对象访问

```typescript
const data = ShmProxyLazy.createProxy(key);

// 嵌套访问也只转换访问的路径
console.log(data.song.segments[0].pitches[0]);
// 只转换: song → segments → [0] → pitches → [0]
```

### 与 Object 方法配合

```typescript
const data = ShmProxyLazy.createProxy(key);

// Object.keys 会触发获取所有字段名（但不转换值）
const keys = Object.keys(data);

// Object.values 会触发转换所有值
const values = Object.values(data);

// 推荐：使用 materialize 代替
const fullData = await ShmProxyLazy.materialize(key);
```

### 字段缓存

```typescript
// 默认启用缓存，重复访问同一字段无额外开销
const data = ShmProxyLazy.createProxy(key);

console.log(data.song.title);  // 第一次：从 SHM 读取
console.log(data.song.title);  // 第二次：从缓存读取
console.log(data.song.title);  // 第三次：从缓存读取
```

### 禁用缓存

```typescript
// 如果数据会被修改，可以禁用缓存
const data = ShmProxyLazy.createProxy(key, { cache: false });

// 每次访问都从 SHM 读取（确保数据最新）
console.log(data.song.title);
```

## 🏗️ 实现原理

### 数据流

**转换阶段（极快）:**
```
NSDictionary (Native, 已在 SHM)
    ↓
Shared Memory (Binary Format)
    ↓
ES6 Proxy (JS, 只创建 wrapper)
```

**访问阶段（按需）:**
```
data.song.title (JS)
    ↓
ES6 Proxy get trap
    ↓
__shmProxyLazy_getField(key, "song.title")
    ↓
navigateObjectPath() (零拷贝导航)
    ↓
shm_object_get_field() (只读取 title)
    ↓
convertTypedValueToJsi() (单字段转换)
    ↓
jsi::Value (JS, 带缓存)
```

### 关键优化

1. **ES6 Proxy**: JavaScript 标准特性，引擎有优化
2. **路径导航**: `"song.title"` 一次导航到位
3. **字段缓存**: 已访问字段直接返回
4. **零拷贝**: `navigateObjectPath` 直接在 SHM 上操作

## 📊 性能对比

### Partial Access（访问 4 个字段）

| 数据大小 | Traditional | ShmProxy | ShmProxyLazy | 提升 |
|---------|-------------|----------|--------------|------|
| 128KB   | 10.12ms     | 4.01ms   | 3.23ms       | ▲68% |
| 1MB     | 49.97ms     | 21.57ms  | 17.32ms      | ▲65% |
| 20MB    | 1068.98ms   | 457.71ms | 348.69ms     | ▲67% |

### Full Access（访问所有字段）

| 数据大小 | Traditional | ShmProxy | ShmProxyLazy | 提升 |
|---------|-------------|----------|--------------|------|
| 1MB     | 64.50ms     | 35.05ms  | 36.26ms      | ▲44% |
| 20MB    | 1293.30ms   | 715.94ms | 714.30ms     | ▲45% |

### 性能分析

**Partial Access 优势明显:**
- Proxy 创建: ~2.7ms（极快）
- 只转换访问的字段
- 未访问字段不转换

**Full Access 也更快:**
- 即使触发所有字段转换
- 转换时间节省 > JSI 调用累积开销

### 何时选择 ShmProxyLazy

**选择 ShmProxyLazy:**
- ✅ 只访问 <20% 的字段
- ✅ 需要极致性能
- ✅ 大数据传输（>1MB）

**考虑 ShmProxy:**
- ✅ 需要访问 >50% 的字段
- ✅ 需要完整对象语义
- 见 [react-native-shmproxy](../shmproxy/)

## ⚠️ 注意事项

### JSON.stringify

Proxy 对象直接 stringify 会有限制：

```typescript
const data = ShmProxyLazy.createProxy(key);

// ❌ 不推荐：Proxy stringify 可能不完整
const json = JSON.stringify(data);

// ✅ 推荐：先 materialize
const fullData = await ShmProxyLazy.materialize(key);
const json = JSON.stringify(fullData);
```

### React Native 组件

在 React 组件中使用：

```typescript
function MyComponent() {
  const [data, setData] = useState(null);

  useEffect(() => {
    async function loadData() {
      const key = await ShmProxyLazy.write(largeObject);
      const proxy = ShmProxyLazy.createProxy(key);

      // 只在需要时访问
      console.log(proxy.title);

      // 或完全转换
      const fullData = await ShmProxyLazy.materialize(key);
      setData(fullData);
    }

    loadData();
  }, []);

  return <Text>{data?.title}</Text>;
}
```

## 🔗 相关链接

- [完整 API 文档](../../docs/api-reference.md#shmproxy-lazy)
- [架构设计](../../docs/architecture.md#lazy-proxy-design)
- [性能测试](../../docs/performance.md#partial-access)

## 📄 许可证

MIT
