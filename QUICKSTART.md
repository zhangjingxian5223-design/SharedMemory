# ShmProxy 快速开始指南

本指南将帮助你在 5 分钟内上手 ShmProxy 和 ShmProxyLazy。

---

## ShmProxy - 5 分钟上手

### 步骤 1: 安装

```bash
npm install react-native-shmproxy
cd ios
pod install
cd ..
```

### 步骤 2: 基本使用

```typescript
import { ShmProxy } from 'react-native-shmproxy';
import { useEffect } from 'react';

function App() {
  useEffect(() => {
    // 1. 安装 JSI bindings（应用启动时调用一次）
    ShmProxy.installJSIBindingsSync();
  }, []);

  const handleWrite = async () => {
    // 2. 写入数据到共享内存
    const key = await ShmProxy.write({
      song: {
        title: 'Hello ShmProxy',
        artist: 'Test Artist',
        year: 2024,
        segments: [
          { start: 0, end: 30 },
          { start: 30, end: 60 }
        ]
      }
    });
    console.log('数据已写入，key:', key);

    // 3. 读取数据（使用 JSI 函数，推荐）
    const data = global.__shm_read(key);
    console.log('歌曲标题:', data.song.title);
    console.log('艺术家:', data.song.artist);
    console.log('片段数:', data.song.segments.length);
  };

  const getStats = async () => {
    // 4. 获取共享内存统计信息
    const stats = await ShmProxy.getStats();
    console.log('内存使用:', stats.payloadUsed, '/', stats.payloadCapacity);
  };

  return (
    <>
      <Button title="写入数据" onPress={handleWrite} />
      <Button title="查看统计" onPress={getStats} />
    </>
  );
}
```

### 关键点

✅ **自动初始化**: ShmProxy 首次使用时自动初始化共享内存
✅ **同步 JSI 函数**: 使用 `global.__shm_read()` 获得最佳性能
✅ **完整对象**: 一次性获取所有数据，适合访问大部分字段的场景

---

## ShmProxyLazy - 5 分钟上手

### 步骤 1: 安装

```bash
npm install react-native-shmproxy-lazy
cd ios
pod install
cd ..
```

### 步骤 2: 基本使用

```typescript
import { ShmProxyLazy } from 'react-native-shmproxy-lazy';
import { useEffect } from 'react';

function App() {
  useEffect(() => {
    // 1. 初始化共享内存（必须！）
    ShmProxyLazy.initialize();

    // 2. 安装 JSI bindings
    ShmProxyLazy.install();
  }, []);

  const handleLazy = async () => {
    // 3. 写入数据
    const key = await ShmProxyLazy.write({
      song: {
        title: 'Hello ShmProxyLazy',
        artist: 'Test Artist',
        metadata: {
          genre: 'Pop',
          year: 2024,
          tags: ['happy', 'energetic']
        }
      }
    });
    console.log('数据已写入，key:', key);

    // 4. 创建 Proxy（极快，不转换数据）
    const data = ShmProxyLazy.createProxy(key);

    // 5. 懒加载访问（只转换访问的字段）
    console.log('歌曲标题:', data.song.title); // 只转换 song.title
    console.log('艺术家:', data.song.artist);  // 只转换 song.artist
    // metadata 完全不会转换，除非访问它！

    // 6. 如果需要全量转换
    const fullData = await ShmProxyLazy.materialize(key);
    console.log('所有键:', Object.keys(fullData));
  };

  return (
    <Button title="测试懒加载" onPress={handleLazy} />
  );
}
```

### 关键点

⚠️ **手动初始化**: 必须先调用 `await ShmProxyLazy.initialize()`
✅ **极快创建**: `createProxy()` 立即返回，不转换任何数据
✅ **按需转换**: 只转换访问的字段，性能最优
✅ **适合大数据**: 当只访问少量字段时，性能提升显著

---

## 常见问题

### Q1: 为什么要使用 JSI 函数（`global.__shm_read`）？

**A**: JSI 函数提供**同步访问**，避免了 Promise 的异步开销，性能更好。

```typescript
// ❌ 不推荐（Promise 开销）
const data = await ShmProxy.read(key);

// ✅ 推荐（同步，更快）
const data = global.__shm_read(key);
```

### Q2: ShmProxy 和 ShmProxyLazy 如何选择？

**A**: 看你的数据访问比例：

| 访问字段比例 | 推荐方案 | 原因 |
|------------|---------|------|
| > 50% | ShmProxy | 全量转换更简单 |
| < 20% | ShmProxyLazy | 按需转换更快 |
| 20-50% | 看场景 | 追求性能用 Lazy，追求简单用 ShmProxy |

### Q3: 可以混合使用吗？

**A**: 可以，但它们使用**独立的共享内存**：

```typescript
// ShmProxy 的内存
await ShmProxy.write(data1);
const data1 = global.__shm_read(key1);

// ShmProxyLazy 的内存（不同的内存区域）
await ShmProxyLazy.initialize();
await ShmProxyLazy.write(data2);
const data2 = ShmProxyLazy.createProxy(key2);
```

### Q4: 如何处理初始化错误？

**A**: 检查是否初始化，如果未初始化则先初始化：

```typescript
// ShmProxy
const isInit = await ShmProxy.isInitialized();
if (!isInit) {
  await ShmProxy.initialize();
}

// ShmProxyLazy
const isInit = await ShmProxyLazy.isInitialized();
if (!isInit) {
  await ShmProxyLazy.initialize(); // 必须！
}
```

### Q5: 本地开发遇到 `Cannot find module` 错误？

**A**: Metro bundler 不支持符号链接，使用实际复制：

```bash
# ❌ 不要用 npm install（会创建符号链接）
npm install ../path/to/shmproxy

# ✅ 使用实际复制
cp -r ../path/to/shmproxy node_modules/react-native-shmproxy
```

或使用提供的 setup 脚本：
```bash
./setup-test-project.sh
```

### Q6: 性能对比到底如何？

**A**: 实测数据（RN 0.73.11，iOS Simulator）：

| 场景 | 传统方法 | ShmProxy | ShmProxyLazy | 提升 |
|------|---------|----------|--------------|------|
| 1MB，访问 4 字段 | 49.97ms | 21.57ms | 17.32ms | **▲65%** |
| 20MB，访问 4 字段 | 1068.98ms | 457.71ms | 348.69ms | **▲67%** |
| 20MB，访问全部 | 1293.30ms | 715.94ms | 714.30ms | **▲45%** |

**结论**: ShmProxyLazy 在部分访问场景下性能提升最大（65-67%）。

---

## 下一步

- 📖 阅读 [API 参考](./docs/api-reference.md)
- 🏗️ 了解 [架构设计](./docs/architecture.md)
- 📊 查看 [性能分析](./docs/performance.md)
- 💡 查看 [完整示例](./examples/basic-usage/)

---

## 需要帮助？

- 📋 查看 [常见问题](./docs/troubleshooting.md)
- 🐛 [报告问题](https://github.com/yourusername/react-native-shmproxy/issues)
- 💬 [讨论](https://github.com/yourusername/react-native-shmproxy/discussions)
