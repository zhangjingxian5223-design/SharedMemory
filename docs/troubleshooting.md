# Troubleshooting

## Common Issues and Solutions

### Installation Issues

#### Issue: "No bundle URL present"

**Error Message**:
```
Error: No bundle URL present
```

**Solution**:
1. 确保 Metro bundler 正在运行
2. 在项目根目录运行：
   ```bash
   npx react-native start
   ```

#### Issue: "Could not find ShmProxy module"

**Error Message**:
```
Error: Unable to resolve module 'react-native-shmproxy'
```

**Solution**:
1. 确认已安装包：
   ```bash
   npm list react-native-shmproxy
   ```

2. 重新链接：
   ```bash
   cd ios
   pod install
   ```

3. 重新编译：
   ```bash
   npx react-native run-ios
   ```

#### Issue: "JSI bindings not installed"

**Error Message**:
```
Error: ShmProxy JSI bindings not installed
```

**Solution**:
1. 在 App 启动时安装 JSI 绑定：
   ```typescript
   import { ShmProxy } from 'react-native-shmproxy';

   // 在 App.tsx 的 useEffect 中
   useEffect(() => {
     ShmProxy.installJSIBindingsSync();
   }, []);
   ```

2. 确保在 Native 模块加载后调用

---

### Build Issues

#### Issue: "Compilation error: 'ShmProxyModule.h' file not found"

**Error Message**:
```
❌ /path/to/ShmProxyModule.mm:10:9: 'ShmProxyModule.h' file not found
```

**Solution**:
1. 检查 `.podspec` 文件中的 `source_files` 路径
2. 确保 `ShmProxyModule.h` 在正确的位置
3. Clean 并重新 install pods：
   ```bash
   cd ios
   rm -rf Pods Podfile.lock
   pod install
   ```

#### Issue: "Linker error: undefined symbols"

**Error Message**:
```
Undefined symbols for architecture x86_64:
  "_shm_create", referenced from...
```

**Solution**:
1. 确保所有 `.mm` 文件都被编译
2. 检查 `Podfile` 中的配置：
   ```ruby
   s.source_files = 'ios/ShmProxy/**/*.{h,mm,cpp}'
   ```
3. 确保 `shm_kv_c_api.cpp` 被包含（ShmProxyLazy）

#### Issue: "SIGABRT when accessing shared memory"

**Error Message**:
```
Thread 1: signal SIGABRT
```

**Solution**:
1. 检查 SHM 是否已初始化：
   ```typescript
   const isInit = await ShmProxy.isInitialized();
   console.log('Initialized:', isInit);
   ```

2. 尝试手动初始化：
   ```typescript
   await ShmProxy.initialize();
   ```

---

### Runtime Issues

#### Issue: "Shared memory not initialized"

**Error Message**:
```
Error: Shared memory not initialized
```

**Solution**:
1. 确保调用了 `initialize()` 或 `installJSIBindingsSync()`
2. 检查 Bridge 是否可用：
   ```typescript
   const bridge = NativeModules.ShmProxy;
   console.log('Bridge available:', !!bridge);
   ```

#### Issue: "Key not found in shared memory"

**Error Message**:
```
Error: Key not found in shared memory
```

**Solution**:
1. 确认 key 正确（检查拼写）
2. 确认数据已写入：
   ```typescript
   const key = await ShmProxy.write(data);
   console.log('Written key:', key);
   ```
3. 确认没有调用 `clear()` 清除了数据

#### Issue: "Failed to write: SHM_ERR_NO_SPACE"

**Error Message**:
```
Error: Failed to write data to SHM: error 2
```

**Solution**:
1. 检查内存使用情况：
   ```typescript
   const stats = await ShmProxy.getStats();
   console.log('Memory:', stats.payloadUsed, '/', stats.payloadCapacity);
   ```

2. 清理旧数据：
   ```typescript
   await ShmProxy.clear();
   ```

3. 增加共享内存大小：
   ```objc
   // In ShmProxyModule.mm
   g_shmHandle = shm_create(
       [g_shmName UTF8String],
       4096,              // buckets
       65536,             // nodes
       128 * 1024 * 1024  // 增加到 128MB
   );
   ```

---

### Performance Issues

#### Issue: "Slower than expected"

**Possible Causes**:
1. **Debug mode**: 确保使用 Release 模式
2. **Small data**: 对于小数据（<100KB），优化效果不明显
3. **Full access**: 如果访问所有字段，ShmProxyLazy 优势不大

**Solutions**:
1. 在 Release 模式下测试
2. 使用更大的数据集（>1MB）
3. 使用 Partial Access 模式

#### Issue: "Memory usage keeps growing"

**Symptoms**:
- SHM 内存使用率持续增长
- 应用变慢

**Solution**:
1. 定期清理：
   ```typescript
   setInterval(async () => {
     const stats = await ShmProxy.getStats();
     if (stats.payloadUsed / stats.payloadCapacity > 0.8) {
       await ShmProxy.clear();
     }
   }, 60000); // 每分钟检查
   ```

2. 使用后立即删除数据：
   ```typescript
   const key = await ShmProxy.write(data);
   const value = __shm_read(key);
   // 使用完后
   __shm_clear();
   ```

---

### ShmProxyLazy Specific Issues

#### Issue: "Proxy returns undefined for nested fields"

**Problem**:
```typescript
const data = createShmProxyLazy(key);
console.log(data.song.title);  // undefined
```

**Diagnosis**:
1. 检查字段路径是否正确
2. 检查 JSI 绑定是否安装：
   ```typescript
   typeof global.__shmProxyLazy_getField === 'function'
   ```

**Solution**:
1. 确保 JSI 绑定已安装
2. 检查字段名是否正确
3. 添加调试日志：
   ```typescript
   console.log('Keys:', Object.keys(data));
   console.log('Type:', typeof data);
   ```

#### Issue: "Circular reference when using JSON.stringify"

**Problem**:
```typescript
const data = createShmProxyLazy(key);
JSON.stringify(data);  // Error: Maximum nesting level exceeded
```

**Solution**:
使用 `materialize` 方法：
```typescript
const data = createShmProxyLazy(key);
// ❌ 不要直接 stringify Proxy
// const json = JSON.stringify(data);

// ✅ 先 materialize
const fullData = await ShmProxyLazy.materialize(key);
const json = JSON.stringify(fullData);
```

#### Issue: "Object.keys() returns empty array"

**Problem**:
```typescript
const data = createShmProxyLazy(key);
console.log(Object.keys(data));  // []
```

**Solution**:
确保在 Proxy handler 中实现了 `ownKeys` trap：
```typescript
const handler = {
  ownKeys(target) {
    return global.__shmProxyLazy_getKeys(key);
  }
};
```

---

### iOS Specific Issues

#### Issue: "App crashes on launch (iOS 11)"

**Cause**: iOS 11 不支持某些 C++17 特性

**Solution**:
1. 提高最低 iOS 版本要求：
   ```ruby
   # In .podspec
   s.platforms = { :ios => '12.0' }
   ```

2. 或者检查 Xcode 的 Deployment Target 设置

#### Issue: "Works in simulator but not on device"

**Possible Causes**:
1. 架构不匹配（x86_64 vs arm64）
2. 内存限制不同

**Solution**:
1. 确保真机架构设置正确：
   ```bash
   # 检查架构
   lipo -info libShmProxy.a
   ```

2. 在真机上测试内存使用

---

### Debug Tips

### Enable Logging

```typescript
// Native logging (check Xcode console)
NSLog(@"[ShmProxy] Data written with key: %@", key);

// JavaScript logging
console.log('[ShmProxy] Written key:', key);
```

### Check SHM Status

```typescript
const stats = await ShmProxy.getStats();
console.log('SHM Status:', {
  nodesUsed: stats.nodesUsed,
  nodesTotal: stats.nodes,
  memoryUsed: (stats.payloadUsed / 1024 / 1024).toFixed(2) + 'MB',
  memoryTotal: (stats.payloadCapacity / 1024 / 1024).toFixed(2) + 'MB'
});
```

### Verify JSI Bindings

```typescript
// 在 App.tsx 中
useEffect(() => {
  async function checkJSI() {
    console.log('__shm_write:', typeof global.__shm_write);
    console.log('__shm_read:', typeof global.__shm_read);
    console.log('__shmProxyLazy_getField:', typeof global.__shmProxyLazy_getField);
  }
  checkJSI();
}, []);
```

---

## Getting Help

If you're still stuck:

1. **Check existing issues**: https://github.com/yourusername/react-native-shmproxy/issues
2. **Create a new issue**: Use the Bug Report template
3. **Provide details**:
   - React Native version
   - iOS version
   - Device/simulator info
   - Error messages
   - Steps to reproduce
   - Relevant code snippets

---

## Known Limitations

### Current Limitations

- **iOS only**: Android support is planned
- **React Native 0.60+**: Requires JSI support
- **iOS 11.0+**: Requires JSI availability
- **Single writer**: Concurrent writes not yet supported

### Workarounds

For Android:
- Use Traditional Bridge or AsyncStorage
- Wait for Android support

For older React Native:
- Upgrade to React Native 0.60+
- Use polyfills if needed

---

## Common Mistakes

### ❌ Don't

```typescript
// 1. 忘记安装 JSI 绑定
const data = await ShmProxy.read(key);  // Error!

// 2. 使用错误的 key
const data = await ShmProxy.read("wrong_key");  // Error!

// 3. 直接 stringify Proxy
const proxy = createShmProxyLazy(key);
const json = JSON.stringify(proxy);  // Error!

// 4. 重复初始化
await ShmProxy.initialize();
await ShmProxy.initialize();  // Wasteful
```

### ✅ Do

```typescript
// 1. 安装 JSI 绑定
useEffect(() => {
  ShmProxy.installJSIBindingsSync();
}, []);

// 2. 使用正确的 key
const key = await ShmProxy.write(data);
const data = await ShmProxy.read(key);

// 3. Materialize 后再 stringify
const fullData = await ShmProxyLazy.materialize(key);
const json = JSON.stringify(fullData);

// 4. 检查初始化状态
if (!await ShmProxy.isInitialized()) {
  await ShmProxy.initialize();
}
```

---

## Related Documents

- [Installation Guide](./installation.md)
- [API Reference](./api-reference.md)
- [Architecture Design](./architecture.md)
- [Performance Analysis](./performance.md)
