# Basic Usage Example

这是一个展示 ShmProxy 基础用法的示例项目。

## 功能演示

1. **写入数据**: 将 JavaScript 对象写入共享内存
2. **读取数据**: 从共享内存读取并转换为 JavaScript 对象
3. **获取统计**: 查看共享内存使用情况
4. **清空 SHM**: 释放共享内存

## 运行示例

```bash
# 安装依赖
npm install

# iOS
cd ios && pod install && cd ..
npm run ios

# Android
npm run android
```

## 代码结构

```
basic-usage/
├── App.tsx           # 主应用组件
├── package.json
└── README.md
```

## 使用说明

1. 点击"写入数据"按钮，将示例数据写入共享内存
2. 点击"读取数据"按钮，从共享内存读取数据
3. 点击"获取统计"按钮，查看共享内存使用情况
4. 点击"清空 SHM"按钮，释放所有数据

## 关键代码

```typescript
import { ShmProxy } from 'react-native-shmproxy';

// 写入数据
const key = await ShmProxy.write({
  song: {
    title: 'Hello World',
    artist: 'Demo Artist',
    year: 2024,
  }
});

// 读取数据
const data = await ShmProxy.read(key);
console.log(data.song.title);

// 获取统计
const stats = await ShmProxy.getStats();
console.log(`Memory: ${stats.payloadUsed}/${stats.payloadCapacity}`);

// 清空
await ShmProxy.clear();
```

## 预期输出

```
[Example] Writing data to SHM...
[Example] Data written, key: bench_0
[Example] Reading data from SHM...
[Example] Data read: {
  song: {
    title: 'Hello World',
    artist: 'Demo Artist',
    year: 2024
  }
}
```

## 下一步

- [Large Data Example](../large-data/) - 大数据传输示例
- [Performance Comparison](../performance-comparison/) - 性能对比示例
