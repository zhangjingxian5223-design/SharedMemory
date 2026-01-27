# React Native ShmProxy

> 高性能 React Native 数据传输方案，使用共享内存（Shared Memory）优化大数据传输

[![npm version](https://badge.fury.io/js/react-native-shmproxy.svg)](https://www.npmjs.com/package/react-native-shmproxy)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-iOS-lightgrey.svg)](https://github.com/yourusername/react-native-shmproxy)

## ✨ 特性

- 🚀 **性能提升**: 比传统 Bridge 方法快 **55-68%**
- 💾 **零拷贝**: 使用共享内存避免数据复制
- 🔄 **懒加载**: ShmProxyLazy 支持按需加载，只在访问时转换数据
- 📱 **React Native**: 完整的 TypeScript 支持
- 🔒 **类型安全**: 完整的类型定义

## 📦 包含模块

本项目包含两个独立但互补的模块：

### ShmProxy

全量转换方案，适合需要完整对象的场景。

- **性能**: 比传统方法快 **55-60%**
- **使用场景**: 需要访问大部分字段
- **实现**: Shared Memory + JSI 全量转换

### ShmProxyLazy

懒加载方案，使用 ES6 Proxy 实现按需访问。

- **性能**: 比传统方法快 **64-68%** (部分访问场景)
- **使用场景**: 只访问少量字段，需要极致性能
- **实现**: ES6 Proxy + Shared Memory + 按需转换

## 📊 性能对比

### Partial Access (访问 4 个字段)

| 数据大小 | Traditional | ShmProxy | ShmProxyLazy | 提升 |
|---------|-------------|----------|--------------|------|
| 1MB     | 49.97ms     | 21.57ms  | 17.32ms      | ▲65% |
| 5MB     | 256.14ms    | 111.35ms | 91.77ms      | ▲64% |
| 20MB    | 1068.98ms   | 457.71ms | 348.69ms     | ▲67% |

### Full Access (访问所有字段)

| 数据大小 | Traditional | ShmProxy | ShmProxyLazy | 提升 |
|---------|-------------|----------|--------------|------|
| 1MB     | 64.50ms     | 35.05ms  | 36.26ms      | ▲44% |
| 5MB     | 324.92ms    | 183.67ms | 181.99ms     | ▲44% |
| 20MB    | 1293.30ms   | 715.94ms | 714.30ms     | ▲45% |

*测试环境: iOS Simulator, React Native 0.73.11*

## ⚠️ 重要提示

### React Native 版本要求
- ✅ **React Native 0.73.x 及以下**：已验证，完全兼容（旧架构）
- ⚠️ **React Native 0.84+**：新架构（Fabric/TurboModules）适配中

### Metro Bundler 兼容性

**重要**: Metro bundler 不支持符号链接。本地开发时有三种安装方式：

#### 方式 1: 使用 setup 脚本（推荐）
```bash
./setup-test-project.sh
```
脚本会自动处理符号链接问题并创建测试项目。

#### 方式 2: 手动安装（注意符号链接）
```bash
# ❌ 错误：npm install 会创建符号链接，Metro 无法解析
npm install ../SharedMemory/packages/shmproxy

# ✅ 正确：使用实际复制（Metro bundler 要求）
cp -r ../SharedMemory/packages/shmproxy node_modules/react-native-shmproxy
cp -r ../SharedMemory/packages/shmproxy-lazy node_modules/react-native-shmproxy-lazy
```

#### 方式 3: 从 npm 安装（生产环境推荐）
```bash
npm install react-native-shmproxy react-native-shmproxy-lazy
```
从 npm 安装不会创建符号链接，可以正常使用。

### 共享内存说明

**重要**: ShmProxy 和 ShmProxyLazy 使用**独立的共享内存**：

```typescript
import { ShmProxy } from 'react-native-shmproxy';
import { ShmProxyLazy } from 'react-native-shmproxy-lazy';

// ShmProxy - 自动初始化
await ShmProxy.write(data); // 首次使用时自动初始化

// ShmProxyLazy - 需要手动初始化
await ShmProxyLazy.initialize(); // 必须先初始化！
await ShmProxyLazy.write(data);
```

## 📚 API 快速参考

### ShmProxy vs ShmProxyLazy 对照表

| 功能 | ShmProxy | ShmProxyLazy | 说明 |
|------|----------|--------------|------|
| **初始化** | 自动 | `await initialize()` | Lazy 需要手动初始化 |
| **安装 JSI** | `installJSIBindingsSync()` | `await install()` | API 不同 |
| **写入数据** | `await write(data)` | `await write(data)` | 相同 |
| **读取数据** | `global.__shm_read(key)` | `createProxy(key)` | 不同 |
| **全量转换** | N/A (全量) | `await materialize(key)` | Lazy 专用 |
| **获取统计** | `await getStats()` | `await getStats()` | 相同 |
| **清空数据** | `await clear()` | `await clear()` | 相同 |

### 使用场景选择

**使用 ShmProxy 当**:
- ✅ 需要访问大部分字段（> 50%）
- ✅ 需要完整对象的语义
- ✅ 代码简洁性优先
- ✅ 不需要手动初始化

**使用 ShmProxyLazy 当**:
- ✅ 只访问少量字段（< 20%）
- ✅ 需要极致性能
- ✅ 大数据场景
- ✅ 可以接受手动初始化

## 🚀 快速开始

### 安装

```bash
# 使用 npm
npm install react-native-shmproxy

# 或使用 yarn
yarn add react-native-shmproxy

# 如果需要懒加载功能
npm install react-native-shmproxy-lazy
```

### iOS 配置

```bash
cd ios
pod install
```

### 基础使用

#### ShmProxy

```typescript
import { ShmProxy } from 'react-native-shmproxy';

// 1. 写入数据到共享内存
const shmKey = await ShmProxy.write({
  song: {
    title: 'Hello World',
    year: 2024,
    segments: [
      { pitches: [440, 880, 1320] },
      { pitches: [523, 1047, 1568] }
    ]
  }
});

// 2. 读取数据（全量转换）
const data = await ShmProxy.read(shmKey);

// 3. 使用数据
console.log(data.song.title); // 'Hello World'
console.log(data.song.segments[0].pitches); // [440, 880, 1320]
```

#### ShmProxyLazy

```typescript
import { ShmProxyLazy } from 'react-native-shmproxy-lazy';

// 1. 写入数据
const shmKey = await ShmProxyLazy.write({
  song: {
    title: 'Hello World',
    year: 2024,
    segments: [...]
  }
});

// 2. 创建 Proxy（极快，不转换数据）
const data = ShmProxyLazy.createProxy(shmKey);

// 3. 访问字段（只转换访问的字段）
console.log(data.song.title); // 只转换 song.title

// 4. 完整访问（如果需要）
const fullData = await ShmProxyLazy.materialize(shmKey);
```

## 📖 文档

- [安装指南](./docs/installation.md)
- [API 参考](./docs/api-reference.md)
- [ShmProxy 文档](./packages/shmproxy/README.md)
- [ShmProxyLazy 文档](./packages/shmproxy-lazy/README.md)
- [架构设计](./docs/architecture.md)
- [性能分析](./docs/performance.md)

## 🎯 使用场景

### 使用 ShmProxy 当:

✅ 需要访问大部分字段
✅ 需要完整对象的语义
✅ 代码简洁性优先

### 使用 ShmProxyLazy 当:

✅ 只访问少量字段（< 20%）
✅ 需要极致性能
✅ 可以接受异步访问

## 🏗️ 架构设计

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

## 🤝 贡献

欢迎贡献代码！请查看 [CONTRIBUTING.md](./CONTRIBUTING.md) 了解详情。

## 📄 许可证

MIT License - 详见 [LICENSE](./LICENSE) 文件

## 🔗 相关链接

- [Shared Memory 原理](./docs/architecture.md#shared-memory-format)
- [性能测试方法](./docs/performance.md)
- [常见问题](./docs/troubleshooting.md)

## 🙏 致谢

本项目受以下项目启发：
- [React Native JSI](https://github.com/facebook/react-native/tree/main/packages/react-native/ReactCommon/jsi)
- [shm](https://github.com/waymondrang/shm)

---

**Made with ❤️ by [Your Name]**
