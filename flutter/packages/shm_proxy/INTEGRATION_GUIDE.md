# FlutterShmProxy 快速集成指南

## 🚀 3 分钟快速集成

### 步骤 1：添加依赖

在项目的 `pubspec.yaml` 中添加：

```yaml
dependencies:
  shm_flutter_new:
    path: ../shm_flutter  # 或者使用 git/url
```

### 步骤 2：安装依赖

```bash
flutter pub get
cd ios && pod install
```

### 步骤 3：开始使用

```dart
import 'package:shm_flutter_new/shm_flutter_new.dart';

// 使用 ShmProxy
final shmProxy = ShmProxy();
await shmProxy.initialize();

// 写入数据
final key = shmProxy.write({'message': 'Hello from Native!'});

// 读取数据（零拷贝）
final data = shmProxy.read(key);
print(data); // {message: Hello from Native!}
```

---

## 📖 详细集成步骤

### iOS 集成

#### 1. 确认 iOS 配置

检查 `ios/Podfile` 包含：

```ruby
platform :ios, '13.0'  # iOS 13.0+

target 'Runner' do
  use_frameworks!
  pod 'Flutter'
end
```

#### 2. 安装 CocoaPods 依赖

```bash
cd ios
pod install
```

#### 3. 验证安装

```bash
# 检查 ShmFlutterNewPlugin 是否注册成功
open Runner.xcworkspace
```

在 Xcode 中：
1. 选择 `Runner` 项目
2. 查看 `Linked Frameworks and Libraries`
3. 确认包含 `ShmFlutterNewPlugin.framework`

---

## 🔧 常见集成问题

### 问题 1：编译错误 "Undefined symbols"

**症状**：
```
Undefined symbol: _shm_create
Undefined symbol: _shm_insert
```

**解决方案**：
```bash
cd ios
rm -rf Pods/
rm Podfile.lock
pod install
```

### 问题 2：共享内存初始化失败

**症状**：
```
Error: Failed to initialize shared memory
```

**解决方案**：
检查 `ios/shm_flutter_new.podspec` 包含：
```ruby
s.resources = ['TestData/*.json']  # 如果使用测试功能
s.dependency 'OpenSSL-Universal'  # OpenSSL 依赖
```

### 问题 3：运行时崩溃

**症状**：应用启动后立即崩溃

**解决方案**：
确认已运行 `pod install` 并重新编译：
```bash
cd ios
pod install
cd ..
flutter clean
flutter run
```

---

## ✅ 验证集成成功

### 测试代码

```dart
import 'package:flutter/material.dart';
import 'package:shm_flutter_new/shm_flutter_new.dart';

void main() async {
  runApp(MyApp());
}

class MyApp extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: ShmProxyTest(),
    );
  }
}

class ShmProxyTest extends StatefulWidget {
  @override
  _ShmProxyTestState createState() => _ShmProxyTestState();
}

class _ShmProxyTestState extends State<ShmProxyTest> {
  final _shmProxy = ShmProxy();
  String _status = 'Not initialized';

  @override
  void initState() {
    super.initState();
    _testShmProxy();
  }

  Future<void> _testShmProxy() async {
    try {
      // 1. 初始化
      await _shmProxy.initialize();
      setState(() => _status = '✅ Initialized');

      // 2. 写入测试数据
      final testData = {
        'id': 123,
        'title': 'Test Message',
        'timestamp': DateTime.now().toIso8601String(),
      };
      final key = _shmProxy.write(testData);
      setState(() => _status = '✅ Written: $key');

      // 3. 读取测试数据
      final retrievedData = _shmProxy.read(key);
      setState(() => _status = '✅ Success: $retrievedData');

    } catch (e) {
      setState(() => _status = '❌ Error: $e');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('ShmProxy Test')),
      body: Center(
        child: Text(_status),
      ),
    );
  }
}
```

### 预期输出

```
✅ Initialized
✅ Written: shm_0
✅ Success: {id: 123, title: Test Message, timestamp: 2026-01-15T...}
```

---

## 📊 性能验证

### 简单性能测试

```dart
import 'package:shm_flutter_new/shm_flutter_new.dart';

class PerformanceTest {
  Future<void> run() async {
    final shmProxy = ShmProxy();
    await shmProxy.initialize();

    // 生成 1MB 测试数据
    final testData = _generateLargeData(1024 * 1024);

    // 测试 100 次写入
    final writeStart = DateTime.now();
    for (int i = 0; i < 100; i++) {
      shmProxy.write(testData);
    }
    final writeEnd = DateTime.now();
    final writeAvg = writeEnd.difference(writeStart).inMilliseconds / 100;

    // 测试 100 次读取
    final readStart = DateTime.now();
    for (int i = 0; i < 100; i++) {
      shmProxy.read('shm_$i');
    }
    final readEnd = DateTime.now();
    final readAvg = readEnd.difference(readStart).inMilliseconds / 100;

    print('写入平均: ${writeAvg.toStringAsFixed(2)} ms');
    print('读取平均: ${readAvg.toStringAsFixed(2)} ms');
    print('预期性能提升: 50-60%');
  }
}
```

---

## 🎯 下一步

集成成功后，你可以：

1. **替换现有 MethodChannel 调用**
   ```dart
   // 之前
   final data = await platform.invokeMethod('getData');

   // 现在
   final data = shmProxy.read(key);
   ```

2. **优化大数据传输**
   - 识别传输数据 > 100KB 的场景
   - 使用 ShmProxy 替换

3. **运行完整性能测试**
   - 参见 `example/lib/main.dart`
   - 获取完整的性能报告

---

## 💡 最佳实践

### DO ✅

- ✅ 用于大数据传输（> 100KB）
- ✅ 用于高频调用场景
- ✅ 测量性能提升
- ✅ 及时关闭共享内存

### DON'T ❌

- ❌ 用于小数据（< 10KB）
- ❌ 用于一次性操作
- ❌ 忘记初始化
- ❌ 忘记错误处理

---

## 📞 获取帮助

遇到问题？

1. 查看 [README.md](README.md) 完整文档
2. 查看 [DIRECTORY_STRUCTURE.md](DIRECTORY_STRUCTURE.md) 目录说明
3. 查看 [benchmark_results.json](benchmark_results.json) 性能数据
4. 提交 Issue 到 GitHub

---

**祝集成顺利！🚀**
