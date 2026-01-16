# FlutterShmProxy 目录结构分析

## 📂 完整目录结构

```
shm_flutter_new/
├── lib/src/                          # Dart 源代码（核心）
│   ├── benchmark_app.dart            # ✅ 必需 - Benchmark UI
│   ├── benchmark_module.dart         # ✅ 必需 - MethodChannel API
│   ├── benchmark_runner.dart         # ✅ 必需 - 测试运行器
│   ├── deserializer.dart             # ✅ 必需 - 二进制反序列化
│   ├── serializer.dart               # ✅ 必需 - 二进制序列化
│   ├── shm_flutter_base.dart         # ✅ 必需 - ShmProxy 基类
│   └── shm_flutter_ffi.dart          # ✅ 必需 - Dart FFI 绑定
│
├── ios/Classes/                      # iOS 原生代码（核心）
│   ├── BenchmarkModule.h             # ✅ 必需 - Benchmark 模块接口
│   ├── BenchmarkModule.mm            # ✅ 必需 - Benchmark 模块实现
│   ├── NSDictionaryToShm.h          # ✅ 必需 - NSDictionary 转 SharedMemory
│   ├── NSDictionaryToShm.mm         # ✅ 必需 - NSDictionary 转 SharedMemory 实现
│   ├── ShmFlutterNewPlugin.swift     # ✅ 必需 - Flutter Plugin 入口
│   ├── shm_flutter_c_api.c           # ✅ 必需 - Flutter C API 包装
│   ├── shm_flutter_c_api.h           # ✅ 必需 - Flutter C API 头文件
│   ├── shm_kv_c_api.cpp             # ✅ 必需 - Shared Memory 核心 C++ 实现
│   └── shm_kv_c_api.h                # ✅ 必需 - Shared Memory 核心 C++ 头文件
│
├── ios/TestData/                     # ✅ 必需 - 测试数据
│   ├── test_data_128KB_205.json      # ✅ 必需
│   ├── test_data_256KB_420.json      # ✅ 必需
│   ├── test_data_512KB_845.json      # ✅ 必需
│   ├── test_data_1MB_1693.json       # ✅ 必需
│   ├── test_data_2MB_3390.json       # ✅ 必需
│   ├── test_data_5MB_8493.json       # ✅ 必需
│   ├── test_data_10MB_16986.json     # ✅ 必需
│   ├── test_data_20MB_33973.json     # ✅ 必需
│   ├── test_data_base.json           # ❌ 可删 - 基础模板（不需要）
│   ├── test_data_small_10.json       # ❌ 可删 - 旧的小数据（不需要）
│   ├── test_data_medium_100.json     # ❌ 可删 - 旧的中数据（不需要）
│   ├── test_data_large_500.json      # ❌ 可删 - 旧的大数据（不需要）
│   └── test_data_xlarge_1000.json    # ❌ 可删 - 旧的超大数据（不需要）
│
├── ios/
│   ├── shm_flutter_new.podspec       # ✅ 必需 - Podspec 配置
│   └── Assets/                       # ❌ 可删 - 空目录
│
├── test/                             # 单元测试
│   ├── serializer_test.dart          # ⚠️ 可选 - 序列化测试
│   ├── benchmark_module_test.dart    # ⚠️ 可选 - Benchmark 模块测试
│   ├── shm_flutter_new_test.dart     # ❌ 可删 - 默认生成的空测试
│   └── shm_flutter_new_method_channel_test.dart  # ❌ 可删 - 默认生成的测试
│
├── example/                          # 示例应用
│   ├── lib/
│   │   └── main.dart                 # ✅ 必需 - 示例应用入口
│   │
│   ├── ios/
│   │   ├── TestData/                 # ❌ 可删 - 重复的测试数据
│   │   ├── Runner.xcodeproj/         # ✅ 必需 - Xcode 项目
│   │   ├── Runner.xcworkspace/       # ✅ 必需 - Xcode Workspace
│   │   ├── Runner/                   # ✅ 必需 - iOS App
│   │   ├── Pods/                     # ❌ 可删 - CocoaPods 生成（可重新生成）
│   │   └── build/                    # ❌ 可删 - 构建产物
│   │
│   ├── integration_test/             # ⚠️ 可选 - 集成测试
│   ├── test/                         # ⚠️ 可选 - Widget 测试
│   ├── pubspec.yaml                  # ✅ 必需 - 依赖配置
│   └── pubspec.lock                  # ❌ 可删 - 锁定文件（可重新生成）
│
├── pubspec.yaml                      # ✅ 必需 - 包配置
├── pubspec.lock                      # ❌ 可删 - 锁定文件（可重新生成）
├── LICENSE                           # ✅ 必需 - 许可证
├── README.md                         # ✅ 必需 - 说明文档
├── CHANGELOG.md                      # ⚠️ 可选 - 变更日志
├── shm_flutter_new.iml               # ❌ 可删 - IDE 配置（可重新生成）
└── build/                            # ❌ 可删 - 构建产物（可重新生成）
```

## 🗑️ 可以立即删除的文件/目录

### 1. 构建产物（可完全删除，运行时自动生成）
```bash
rm -rf build/
rm -rf example/build/
rm -rf example/ios/build/
```

### 2. Example 中的重复测试数据（不需要）
```bash
rm -rf example/ios/TestData/
```

### 3. 旧版测试数据（不需要）
```bash
cd ios/TestData/
rm test_data_base.json
rm test_data_small_10.json
rm test_data_medium_100.json
rm test_data_large_500.json
rm test_data_xlarge_1000.json
```

### 4. 依赖锁定文件（可重新生成）
```bash
rm pubspec.lock
rm example/pubspec.lock
```

### 5. IDE 生成的文件
```bash
rm shm_flutter_new.iml
rm -rf example/ios/Pods/
rm -rf example/ios/.symlinks/
```

### 6. 默认生成的空测试
```bash
rm test/shm_flutter_new_test.dart
rm test/shm_flutter_new_method_channel_test.dart
```

### 7. 空资源目录
```bash
rm -rf ios/Assets/
```

## ✅ 清理后的最小目录结构

```
shm_flutter_new/
├── lib/src/
│   ├── benchmark_app.dart
│   ├── benchmark_module.dart
│   ├── benchmark_runner.dart
│   ├── deserializer.dart
│   ├── serializer.dart
│   ├── shm_flutter_base.dart
│   └── shm_flutter_ffi.dart
│
├── ios/
│   ├── Classes/
│   │   ├── BenchmarkModule.h/mm
│   │   ├── NSDictionaryToShm.h/mm
│   │   ├── ShmFlutterNewPlugin.swift
│   │   ├── shm_flutter_c_api.c/h
│   │   └── shm_kv_c_api.cpp/h
│   ├── TestData/
│   │   ├── test_data_128KB_205.json
│   │   ├── test_data_256KB_420.json
│   │   ├── test_data_512KB_845.json
│   │   ├── test_data_1MB_1693.json
│   │   ├── test_data_2MB_3390.json
│   │   ├── test_data_5MB_8493.json
│   │   ├── test_data_10MB_16986.json
│   │   └── test_data_20MB_33973.json
│   └── shm_flutter_new.podspec
│
├── example/
│   ├── lib/main.dart
│   ├── ios/
│   │   ├── Runner.xcodeproj/
│   │   ├── Runner.xcworkspace/
│   │   └── Runner/
│   ├── pubspec.yaml
│   └── integration_test/
│
├── test/
│   ├── serializer_test.dart
│   └── benchmark_module_test.dart
│
├── pubspec.yaml
├── README.md
└── LICENSE
```

## 📊 文件统计

### 当前状态
- **总文件数**: ~150+ 文件
- **核心代码**: ~15 个文件（Dart + iOS）
- **测试数据**: 13 个 JSON 文件（5 个不需要）
- **构建产物**: ~100+ 个生成文件

### 清理后
- **总文件数**: ~35 个文件
- **核心代码**: ~15 个文件（不变）
- **测试数据**: 8 个 JSON 文件（仅保留需要的）
- **构建产物**: 0 个文件（全部删除）

## 🚀 清理脚本

保存以下脚本为 `cleanup.sh` 并运行：

```bash
#!/bin/bash

echo "🧹 Cleaning FlutterShmProxy..."

# 删除构建产物
echo "删除构建产物..."
rm -rf build/
rm -rf example/build/
rm -rf example/ios/build/

# 删除重复的测试数据
echo "删除重复的测试数据..."
rm -rf example/ios/TestData/

# 删除旧版测试数据
echo "删除旧版测试数据..."
cd ios/TestData/
rm test_data_base.json
rm test_data_small_10.json
rm test_data_medium_100.json
rm test_data_large_500.json
rm test_data_xlarge_1000.json
cd ../../..

# 删除依赖锁定文件
echo "删除依赖锁定文件..."
rm pubspec.lock
rm example/pubspec.lock

# 删除 Pods
echo "删除 CocoaPods 生成文件..."
rm -rf example/ios/Pods/
rm -rf example/ios/.symlinks/

# 删除默认测试
echo "删除默认生成的空测试..."
rm test/shm_flutter_new_test.dart
rm test/shm_flutter_new_method_channel_test.dart

# 删除空资源目录
rm -rf ios/Assets/

echo "✅ 清理完成！"
echo ""
echo "运行以下命令重新生成依赖："
echo "  cd example && flutter pub get"
echo "  cd ios && pod install"
```

## 📝 核心文件说明

### Dart 源代码（7 个文件）

1. **benchmark_app.dart** (9KB)
   - UI 界面
   - 测试结果显示
   - 结果导出功能

2. **benchmark_module.dart** (2.5KB)
   - MethodChannel API
   - Native ↔ Dart 通信

3. **benchmark_runner.dart** (7KB)
   - 测试运行器
   - 48 个自动化测试
   - 性能对比计算

4. **serializer.dart** (3.5KB)
   - Dart 对象 → 二进制序列化
   - 支持基础类型和嵌套结构

5. **deserializer.dart** (3.5KB)
   - 二进制 → Dart 对象反序列化
   - 零拷贝读取

6. **shm_flutter_base.dart** (1.5KB)
   - ShmProxy 公共接口
   - 数据读写封装

7. **shm_flutter_ffi.dart** (5KB)
   - Dart FFI 绑定
   - C 函数调用

### iOS 原生代码（9 个文件）

1. **BenchmarkModule.h/mm** (7KB)
   - Benchmark 模块
   - 测试数据管理
   - 高精度计时器

2. **NSDictionaryToShm.h/mm** (18KB)
   - NSDictionary → Shared Memory
   - 数据序列化核心

3. **ShmFlutterNewPlugin.swift** (2.5KB)
   - Flutter Plugin 入口
   - MethodChannel 处理

4. **shm_flutter_c_api.c/h** (4KB)
   - Flutter C API 包装
   - C 接口适配

5. **shm_kv_c_api.cpp/h** (177KB)
   - 共享内存核心实现
   - KV 存储引擎
   - 支持复杂数据类型

### 测试数据（8 个 JSON 文件，总计 ~40MB）

```
test_data_128KB_205.json      128KB
test_data_256KB_420.json      256KB
test_data_512KB_845.json      512KB
test_data_1MB_1693.json       1MB
test_data_2MB_3390.json       2MB
test_data_5MB_8493.json       5MB
test_data_10MB_16986.json     10MB
test_data_20MB_33973.json     20MB
```

## 🎯 最终建议

### 必需保留（生产环境）
- ✅ 所有 `lib/src/` 文件（7 个）
- ✅ 所有 `ios/Classes/` 文件（9 个）
- ✅ `ios/TestData/` 中的 8 个测试数据
- ✅ `ios/shm_flutter_new.podspec`
- ✅ `pubspec.yaml`
- ✅ `example/lib/main.dart`
- ✅ `example/ios/Runner.xcodeproj/`
- ✅ `example/ios/Runner.xcworkspace/`
- ✅ `example/ios/Runner/`
- ✅ `example/pubspec.yaml`

### 可选保留（开发环境）
- ⚠️ `test/serializer_test.dart`（序列化单元测试）
- ⚠️ `test/benchmark_module_test.dart`（模块测试）
- ⚠️ `README.md`（文档）
- ⚠️ `CHANGELOG.md`（变更日志）

### 可以删除
- ❌ 所有 `build/` 目录
- ❌ 所有 `Pods/` 目录
- ❌ 所有 `.lock` 文件
- ❌ `example/ios/TestData/`（重复）
- ❌ 旧版测试数据（5 个）
- ❌ 默认生成的空测试

## 📦 发布到 pub.dev 前的清理

如果要发布到 pub.dev，还需要：

1. **删除 example 中的 Pods**
   ```bash
   cd example/ios
   rm -rf Pods/
   rm Podfile.lock
   ```

2. **添加到 .gitignore**
   ```
   build/
   .dart_tool/
   .packages
   pubspec.lock
   example/ios/Pods/
   example/ios/Podfile.lock
   example/build/
   ```

3. **更新 README.md**，添加使用说明

4. **运行测试**
   ```bash
   flutter test
   flutter pub publish --dry-run
   ```
