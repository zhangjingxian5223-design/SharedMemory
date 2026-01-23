# .podspec Files Creation Complete

## ✅ 已完成

两个 `.podspec` 文件已成功创建：

1. **ShmProxy.podspec** → `packages/shmproxy/ShmProxy.podspec`
2. **ShmProxyLazy.podspec** → `packages/shmproxy-lazy/ShmProxyLazy.podspec`

---

## 📋 .podspec 文件详情

### ShmProxy.podspec

```ruby
name:         react-native-shmproxy
version:      1.0.0
summary:      High-performance data transfer using Shared Memory
description:  Full description with features
homepage:     https://github.com/yourusername/react-native-shmproxy
license:      MIT
platforms:    iOS 11.0+

Dependencies:
  - React-Core
  - React-RCTBridge
  - React-jsi
  - React-RCTAPI

Source files: ios/ShmProxy/**/*.{h,mm,cpp}
Frameworks:   UIKit, Foundation
```

### ShmProxyLazy.podspec

```ruby
name:         react-native-shmproxy-lazy
version:      1.0.0
summary:      Lazy-loading data transfer using ES6 Proxy
description:  Full description with features
homepage:     https://github.com/yourusername/react-native-shmproxy
license:      MIT
platforms:    iOS 11.0+

Dependencies:
  - React-Core
  - React-RCTBridge
  - React-jsi
  - React-RCTAPI
  - react-native-shmproxy (depends on ShmProxy)

Source files: ios/ShmProxyLazy/**/*.{h,mm,cpp}
Frameworks:   UIKit, Foundation
```

---

## 🔑 关键配置

### C++ Standard

两个模块都使用 **C++17**：

```ruby
CLANG_CXX_LANGUAGE_STANDARD = c++17
CLANG_CXX_LIBRARY = libc++
```

### Header Search Paths

ShmProxy 需要的搜索路径：

```ruby
HEADER_SEARCH_PATHS = [
  "$(PODS_TARGET_SRCROOT)",
  "$(PODS_ROOT)/React-Core/ReactCommon/jsi",
]
```

ShmProxyLazy 额外需要 ShmProxy 的头文件：

```ruby
HEADER_SEARCH_PATHS = [
  "$(PODS_TARGET_SRCROOT)",
  "$(PODS_ROOT)/React-Core/ReactCommon/jsi",
  "$(PODS_ROOT)/react-native-shmproxy/ios/ShmProxy",  # 额外添加
]
```

### Compiler Flags

```ruby
COMPILER_FLAGS = -DFOLLY_NO_CONFIG
```

---

## 📖 使用方法

### 方法 1: React Native >= 0.60 (自动链接)

如果你使用的是 React Native 0.60 或更高版本，只需：

```bash
# 安装包
npm install react-native-shmproxy
npm install react-native-shmproxy-lazy

# 安装 pods
cd ios && pod install
```

### 方法 2: 手动集成

如果你的项目不支持自动链接，在 `ios/Podfile` 中添加：

```ruby
target 'YourProject' do
  config = use_native_modules!

  # 添加这两行
  pod 'react-native-shmproxy', :path => '../node_modules/react-native-shmproxy'
  pod 'react-native-shmproxy-lazy', :path => '../node_modules/react-native-shmproxy-lazy'

  # ... other pods ...
end
```

### 方法 3: Monorepo 开发

如果你在本地开发（packages 目录），在 `Podfile` 中使用相对路径：

```ruby
target 'YourProject' do
  config = use_native_modules!

  # 指向本地 packages 目录
  pod 'react-native-shmproxy', :path => '../../packages/shmproxy'
  pod 'react-native-shmproxy-lazy', :path => '../../packages/shmproxy-lazy'

  # ... other pods ...
end
```

---

## 🚀 快速开始

### 使用设置脚本（推荐）

我创建了一个自动化设置脚本：

```bash
# 在项目根目录运行
./setup_ios_pods.sh
```

这个脚本会：
1. ✅ 检查项目结构
2. ✅ 安装 npm 包
3. ✅ 更新 Podfile（如果需要）
4. ✅ 运行 `pod install`
5. ✅ 验证安装

### 手动设置

如果你想手动设置：

```bash
# 1. 安装包
npm install react-native-shmproxy react-native-shmproxy-lazy

# 2. 进入 iOS 目录
cd ios

# 3. 安装 pods
pod install

# 4. 打开 workspace
open *.xcworkspace
```

---

## ✅ 验证安装

### 检查 Pods 目录

```bash
cd ios/Pods
ls -la | grep shmproxy
```

你应该看到：
```
react-native-shmproxy
react-native-shmproxy-lazy
```

### 在 Xcode 中验证

1. 打开 `YourProject.xcworkspace`
2. 展开 `Pods` → `Development Pods`
3. 你应该看到：
   - `react-native-shmproxy`
   - `react-native-shmproxy-lazy`

### 编译测试

在 Xcode 中：
1. 选择目标设备（模拟器或真机）
2. 点击 `Product` → `Build` (Cmd+B)
3. 确保没有编译错误

---

## 🔧 常见问题

### Q: pod install 失败

**A**: 尝试清理并重新安装：

```bash
cd ios
rm -rf Pods Podfile.lock
pod deintegrate
pod install
```

### Q: 编译错误 "找不到头文件"

**A**: 确保 Header Search Paths 配置正确。在 Xcode 中：
1. 选择你的项目
2. 选择 Target → Build Settings
3. 搜索 "Header Search Paths"
4. 确保包含：
   - `$(PODS_TARGET_SRCROOT)`
   - `$(PODS_ROOT)/React-Core/ReactCommon/jsi`

### Q: 链接错误

**A**: 确保所有必需的 frameworks 都已链接：
1. 在 Xcode 中选择 Target
2. "Build Phases" → "Link Binary With Libraries"
3. 确保包含：
   - UIKit.framework
   - Foundation.framework

### Q: 找不到 .podspec 文件

**A**: 确保文件在正确的位置：
- `node_modules/react-native-shmproxy/ShmProxy.podspec`
- `node_modules/react-native-shmproxy-lazy/ShmProxyLazy.podspec`

---

## 📚 相关文档

- [Podfile Integration Guide](./podfile-integration.md)
- [Installation Guide](./installation.md)
- [Basic Usage Example](../examples/basic-usage/)
- [API Reference](./api-reference.md)

---

## 📞 需要帮助？

如果遇到问题：

1. 查看 [Podfile Integration Guide](./podfile-integration.md)
2. 查看 [Installation Guide](./installation.md)
3. 检查 [Troubleshooting](./installation.md#常见问题)
4. 提交 Issue: https://github.com/yourusername/react-native-shmproxy/issues

---

**创建时间**: 2026-01-21
**版本**: 1.0.0
**状态**: ✅ 完成，可以使用
