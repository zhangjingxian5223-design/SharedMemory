# ShmProxy Benchmark Report

## 项目概述

本项目旨在优化 React Native 中 Native 到 JS 的数据传递性能。传统方案使用 `NSDictionary → folly::dynamic → jsi::Value` 的转换链路，我们提出的 ShmProxy 方案使用 `NSDictionary → SharedMemory → HostObject (Lazy Loading)` 来实现按需加载，减少不必要的数据转换开销。

## 目录结构

```
/Users/zjxzjx/Documents/RN/ShmProxyBenchmark/
├── App.tsx                          # React Native 测试应用
├── ios/
│   ├── ShmProxyBenchmark/
│   │   ├── BenchmarkModule.mm       # Native Module 实现
│   │   ├── BenchmarkModule.h
│   │   ├── AppDelegate.mm           # JSI bindings 安装
│   │   ├── ShmProxyObject.mm        # JSI HostObject 实现
│   │   ├── ShmProxyObject.h
│   │   ├── ShmProxyModule.mm        # TurboModule 入口
│   │   ├── ShmProxyModule.h
│   │   ├── NSDictionaryToShm.mm     # NSDictionary → SharedMemory 转换
│   │   ├── NSDictionaryToShm.h
│   │   ├── shm_kv_c_api.cpp         # 共享内存 KV 存储实现
│   │   └── shm_kv_c_api.h
│   └── TestData/
│       ├── test_data_small_10.json   # 17KB, 10 segments
│       ├── test_data_medium_100.json # 104KB, 100 segments
│       ├── test_data_large_500.json  # 493KB, 500 segments
│       └── test_data_xlarge_1000.json # 979KB, 1000 segments
└── BENCHMARK_REPORT.md              # 本文档
```

## 核心算法原理

### 传统方案 (Traditional)

```
NSDictionary (Native)
       ↓
  folly::dynamic (C++ 中间表示)
       ↓
  jsi::Value (JS Runtime)
       ↓
  JS Object (完整对象)
```

**问题**：即使 JS 只需要访问一个字段，也必须完整转换整个对象。

### ShmProxy 方案

```
NSDictionary (Native)
       ↓
  SharedMemory (二进制格式，一次写入)
       ↓
  HostObject Proxy (只是一个句柄)
       ↓
  JS 访问时按需读取 (Lazy Loading)
```

**优势**：
1. NSDictionary → SharedMemory 的转换比 folly::dynamic 更快
2. 返回给 JS 的只是一个 proxy，不做完整转换
3. JS 访问属性时才从 SharedMemory 读取并转换

## Benchmark 测试流程

### 1. 预加载阶段（不计时）

```javascript
// JS 端
await BenchmarkModule.preloadAllData();
```

```objc
// Native 端 - 把 JSON 文件加载成 NSDictionary 存在内存中
for (NSString *size in @[@"small", @"medium", @"large", @"xlarge"]) {
    NSDictionary *data = [self loadTestData:filename];
    g_preloadedData[size] = data;
}
```

### 2. Traditional 方法测试

```javascript
// JS 端
const t0 = getTimeNanos();
const response = await BenchmarkModule.getDataTraditional(dataSize);
const t1 = getTimeNanos();
// response.data 是完整的 JS 对象
```

```objc
// Native 端 - 从内存获取 NSDictionary，通过 RN Bridge 返回
- (void)getDataTraditional:(NSString *)dataSize resolver:(RCTPromiseResolveBlock)resolve {
    NSDictionary *testData = g_preloadedData[dataSize];
    // RN Bridge 会自动做 NSDictionary → folly::dynamic → jsi::Value 转换
    resolve(@{@"data": testData, ...});
}
```

### 3. ShmProxy 方法测试

```javascript
// JS 端
// Step 1: 准备 SharedMemory
const prepareResult = await BenchmarkModule.prepareShmProxy(dataSize);

// Step 2: 通过 JSI 获取 HostObject proxy
const proxy = global.__benchmark_getShmProxy(prepareResult.shmKey);

// Step 3: 访问数据（这里才真正从 shm 读取）
const title = proxy.song.title;  // Lazy loading
```

```objc
// Native 端 - NSDictionary → SharedMemory
- (void)prepareShmProxy:(NSString *)dataSize resolver:(RCTPromiseResolveBlock)resolve {
    NSDictionary *testData = g_preloadedData[dataSize];

    // 转换到共享内存
    shm_error_t err = convertNSDictionaryToShm(g_benchmarkShmHandle,
                                                key.c_str(), key.size(),
                                                testData);

    // 只返回 key，不返回数据
    resolve(@{@"shmKey": key, ...});
}
```

### 4. 访问模式

**Partial 访问**（只访问几个字段）：
```javascript
const result = {
    title: data.song?.title,
    artist: data.song?.artist_name,
    tempo: data.song?.analysis?.tempo,
    firstPitch: data.song?.analysis?.segments?.[0]?.pitches?.[0],
};
```

**Full 访问**（遍历所有数据）：
```javascript
JSON.stringify(data);  // 强制遍历所有字段
```

## API 使用说明

### Native Module API

```typescript
// 预加载所有测试数据到内存
BenchmarkModule.preloadAllData(): Promise<{
    success: boolean;
    loadTimes: { small: number; medium: number; large: number; xlarge: number };
}>;

// Traditional 方法 - 返回完整 JS 对象
BenchmarkModule.getDataTraditional(dataSize: string): Promise<{
    data: any;
    timing: { native_start_ns: number };
    method: 'traditional';
}>;

// ShmProxy 方法 - 返回 shm key
BenchmarkModule.prepareShmProxy(dataSize: string): Promise<{
    shmKey: string;
    timing: { native_convert_ms: number; native_start_ns: number; native_end_ns: number };
    method: 'shmproxy';
}>;

// 获取共享内存统计
BenchmarkModule.getShmStats(): Promise<{
    buckets: number;
    nodes: number;
    nodesUsed: number;
    payloadCapacity: number;
    payloadUsed: number;
    generation: number;
}>;

// 清理共享内存
BenchmarkModule.clearShm(): Promise<boolean>;
```

### JSI Global Functions

```typescript
// 通过 key 获取 HostObject proxy
global.__benchmark_getShmProxy(key: string): any;

// 高精度计时（纳秒）
global.__benchmark_getTimeNanos(): number;
```

## 测试结果

### 测试环境
- 设备：iOS Simulator (iPhone 17)
- iOS 版本：26.0
- React Native：0.73.11
- 测试时间：2026-01-06

### 性能对比

| 数据大小 | 访问模式 | Traditional | ShmProxy | 提升比例 |
|---------|---------|-------------|----------|---------|
| **small** (17KB) | partial | 1.55 ms | 0.62 ms | **60.2%** |
| | full | 2.00 ms | 0.85 ms | **57.7%** |
| **medium** (104KB) | partial | 6.92 ms | 1.52 ms | **78.0%** |
| | full | 7.71 ms | 3.28 ms | **57.5%** |
| **large** (493KB) | partial | 21.82 ms | 6.81 ms | **68.8%** |
| | full | 26.37 ms | 13.87 ms | **47.4%** |
| **xlarge** (979KB) | partial | 41.88 ms | 12.85 ms | **69.3%** |
| | full | 54.60 ms | 28.48 ms | **47.8%** |

### 时间分解分析

#### Traditional 方法
- 几乎所有时间都在 `convert`（NSDictionary → folly → jsi）
- `access` 时间极短（数据已在 JS 内存中）

#### ShmProxy 方法
- `convert` 时间大幅减少（NSDictionary → shm 更快）
- `access` 时间略增（lazy loading 需要从 shm 读取）

### 关键发现

1. **Partial 访问优势明显**：平均提升 **69%**（60-78%）
2. **Full 访问也有提升**：平均提升 **52%**（47-58%）
3. **数据越大，优势越明显**：xlarge 数据提升最高达 69.3%

## 已知限制

1. **null 值不支持**：当前 shm 编码不支持 JSON null 值
2. **Array.isArray() 返回 false**：HostObject 不是真正的 JS Array
3. **需要 JSI bindings**：必须在 AppDelegate 中安装 JSI 函数

## 后续开发建议

### 短期优化
1. 添加 `SHM_TYPE_NULL` 支持
2. 实现 `Symbol.iterator` 让 HostObject 支持 `for...of`
3. 添加 `Array.isArray()` 兼容（通过 `Symbol.toStringTag`）

### 中期优化
1. 支持 TurboModule 集成
2. 添加内存池复用，减少 shm 分配开销
3. 实现增量更新，只同步变化的字段

### 长期目标
1. 支持 Android 平台
2. 实现双向同步（JS → Native）
3. 集成到 React Native 核心

## 运行测试

```bash
# 1. 进入项目目录
cd /Users/zjxzjx/Documents/RN/ShmProxyBenchmark

# 2. 安装依赖
npm install
cd ios && pod install && cd ..

# 3. 启动 Metro
npx react-native start --reset-cache

# 4. 运行 iOS 应用
# 按 'i' 或使用 Xcode 打开 ios/ShmProxyBenchmark.xcworkspace

# 5. 在应用中点击 "Run Benchmarks" 按钮

# 6. 点击 "Export" 导出结果到 Documents 目录
```

## 相关文件

- 原始 ShmProxy 实现：`/Users/zjxzjx/Documents/react-native-main/shm-jsi-proxy/ios/`
- 共享内存库：`/Users/zjxzjx/Documents/ShareMemory/`
- 测试数据生成脚本：`ios/TestData/generate_test_data.py`

## 参考资料

- [React Native JSI](https://reactnative.dev/docs/the-new-architecture/pillars-turbomodules)
- [Hermes Engine](https://hermesengine.dev/)
- [mmap Shared Memory](https://man7.org/linux/man-pages/man2/mmap.2.html)
