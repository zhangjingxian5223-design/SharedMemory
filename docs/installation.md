# 安装指南

## 系统要求

- React Native >= 0.60.0
- iOS >= 11.0
- Xcode >= 12.0
- CocoaPods

## 安装步骤

### 1. 安装包

#### ShmProxy

```bash
npm install react-native-shmproxy
# 或
yarn add react-native-shmproxy
```

#### ShmProxyLazy

```bash
npm install react-native-shmproxy-lazy
# 或
yarn add react-native-shmproxy-lazy
```

### 2. iOS 配置

```bash
cd ios
pod install
cd ..
```

### 3. 重新构建应用

```bash
# 对于 React Native CLI
npx react-native run-ios

# 或使用 Xcode
open ios/YourProject.xcworkspace
# 然后 Cmd+R 运行
```

## 手动链接（React Native < 0.60）

如果使用 React Native < 0.60，需要手动链接：

### iOS

1. 在 Xcode 中，拖拽 `node_modules/react-native-shmproxy/ios/ShmProxy.xcodeproj` 到你的项目
2. 在 `Build Phases` → `Link Binary With Libraries` 中添加 `libShmProxy.a`
3. 添加头文件搜索路径：`$(SRCROOT)/../node_modules/react-native-shmproxy/ios`

## 验证安装

在应用启动后验证：

```typescript
import { ShmProxy } from 'react-native-shmproxy';

// 检查是否已初始化
const isInitialized = await ShmProxy.isInitialized();
console.log('ShmProxy initialized:', isInitialized);
```

## 常见问题

### Q: Pod install 失败

**A:** 确保使用最新版 CocoaPods：

```bash
sudo gem install cocoapods
cd ios
pod deintegrate
pod install
```

### Q: 构建失败，提示找不到头文件

**A:** 清理构建缓存：

```bash
# iOS
cd ios
rm -rf Pods Podfile.lock
pod install

# 清理 Xcode 缓存
# Xcode → Product → Clean Build Folder (Shift+Cmd+K)
```

### Q: 运行时报错 "JSI bindings not installed"

**A:** 确保已重新构建应用：

```bash
npx react-native run-ios
```

### Q: 性能不如预期

**A:** 检查以下几点：

1. 使用 Release 模式构建（不是 Debug）
2. 确保数据足够大（> 100KB）
3. 避免频繁的小数据传输

## 开发环境配置

### TypeScript

确保你的 `tsconfig.json` 包含：

```json
{
  "compilerOptions": {
    "esModuleInterop": true,
    "allowSyntheticDefaultImports": true
  }
}
```

### ESLint

如果使用 ESLint，确保配置正确：

```javascript
module.exports = {
  // ...
  settings: {
    'import/resolver': {
      'node': {
        'extensions': ['.ts', '.tsx', '.js', '.jsx']
      }
    }
  }
};
```

## 卸载

如果需要卸载：

```bash
# 移除包
npm uninstall react-native-shmproxy

# iOS
cd ios
pod install
```

## 下一步

- [快速开始](../README.md#快速开始)
- [API 参考](./api-reference.md)
- [示例项目](../examples/)
