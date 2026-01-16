# ShmProxy 零感知实现总结

## 项目目标

实现 ShmProxy 的"零感知"特性：使 ShmProxy 返回的数据在 JavaScript 端使用时与传统方法（NSDictionary → folly → jsi）完全一致，用户代码无需任何修改即可使用。

## 实现阶段

### 阶段一：支持 null 值 ✅

**问题**：原始实现不支持 `null` 值，导致 `null` 字段无法正确传递。

**解决方案**：
- 在 `shm_kv_c_api.h` 中添加 `SHM_TYPE_NULL = 25` 类型
- 在 `NSDictionaryToShm.mm` 中处理 `NSNull` 类型，编码为 `SHM_TYPE_NULL`
- 在 `ShmProxyObject.mm` 的 `convertTypedValueToJsi` 中添加 `SHM_TYPE_NULL` case，返回 `jsi::Value::null()`

**修改文件**：
- `ios/ShmProxyBenchmark/shm_kv_c_api.h`
- `ios/ShmProxyBenchmark/NSDictionaryToShm.mm`
- `ios/ShmProxyBenchmark/ShmProxyObject.mm`

---

### 阶段二：让数组通过 Array.isArray() 检测 ✅

**问题**：使用 HostObject 实现的数组无法通过 `Array.isArray()` 检测。

**解决方案**：
- 修改 `convertTypedValueToJsi` 中的 `SHM_TYPE_LIST` 处理
- 不再返回 HostObject，而是创建真正的 `jsi::Array` 并填充元素

**修改文件**：
- `ios/ShmProxyBenchmark/ShmProxyObject.mm`

**代码变更**：
```cpp
case SHM_TYPE_LIST: {
    // 创建真正的 jsi::Array 而不是 HostObject
    jsi::Array arr = jsi::Array(rt, count);
    for (uint32_t i = 0; i < count; ++i) {
        arr.setValueAtIndex(rt, i, convertTypedValueToJsi(rt, elemView));
    }
    return arr;
}
```

---

### 阶段三：支持数组迭代器和方法 ✅

**问题**：数组方法如 `forEach`, `map`, `filter`, `reduce` 等需要正常工作。

**解决方案**：
- ��于阶段二已经返回真正的 `jsi::Array`，所有数组方法自动可用

**支持的数组方法**：
- `forEach`, `map`, `filter`, `reduce`, `find`
- `some`, `every`, `indexOf`, `includes`
- `slice`, `join`
- `for...of` 迭代器
- spread 操作符 `[...arr]`

---

### 阶段四：完善对象操作 ✅

**问题**：`Object.values()`, `Object.entries()`, spread 操作符 `{...obj}` 对嵌套对象返回空结果。

**原因**：Hermes 的 `Object.values()` 等方法对 HostObject 不能正确调用 `get()` 获取值。

**解决方案**：
- 修改 `convertTypedValueToJsi` 中的 `SHM_TYPE_OBJECT` 处理
- 将嵌套对象转换为普通 JS 对象，而不是返回 HostObject

**修改文件**：
- `ios/ShmProxyBenchmark/ShmProxyObject.mm`

**代码变更**：
```cpp
case SHM_TYPE_OBJECT: {
    // 创建普通 JS 对象而不是 HostObject
    jsi::Object obj = jsi::Object(rt);
    for (uint32_t i = 0; i < count; ++i) {
        obj.setProperty(rt, fieldName, convertTypedValueToJsi(rt, fieldView));
    }
    return obj;
}
```

**支持的对象操作**：
- `Object.keys()`, `Object.values()`, `Object.entries()`
- `for...in` 循环
- `hasOwnProperty`, `in` 操作符
- spread 操作符 `{...obj}`
- `JSON.stringify()`

---

### 阶段五：测试验证零感知 ✅

**问题**：顶层 proxy 对象的 `Object.values()` 返回空数组。

**原因**：顶层对象仍然是 HostObject，Hermes 的 `Object.values()` 对其不能正确工作。

**解决方案**：
1. 在 `ShmProxyObject` 中添加静态方法 `convertTopLevelToJsObject`
2. 修改 `__benchmark_getShmProxy` 函数，返回普通 JS 对象而不是 HostObject

**修改文件**：
- `ios/ShmProxyBenchmark/ShmProxyObject.h`
- `ios/ShmProxyBenchmark/ShmProxyObject.mm`
- `ios/ShmProxyBenchmark/BenchmarkModule.mm`

**额外修复 - 布尔数组**：
- 修改 `NSDictionaryToShm.mm` 中的 `isNumericNSNumber` 函数，排除布尔值
- 添加 `isBoolNSNumber` 函数检测 CFBoolean
- 在 `encodeArrayFast` 中添加布尔数组快速路径，使用 `SHM_TYPE_BOOL_VECTOR`

---

## 零感知测试结果

测试通过深度比较 Traditional 和 ShmProxy 返回的数据：

| 数据大小 | 字段匹配 | 状态 |
|---------|---------|------|
| SMALL   | 9/9     | PASSED |
| MEDIUM  | 9/9     | PASSED |
| LARGE   | 9/9     | PASSED |
| XLARGE  | 9/9     | PASSED |

**测试内容**：
1. 所有字段值深度比较（递归比较嵌套对象和数组）
2. `typeof` 一致性
3. `JSON.stringify` 结果长度一致
4. `Object.keys` 数量一致
5. `Object.values` 数量一致

---

## 架构说明

### 数据流

```
Native (NSDictionary)
    ↓
NSDictionaryToShm.mm (编码)
    ↓
Shared Memory (shm_kv)
    ↓
ShmProxyObject.mm (解码)
    ↓
Plain JS Object (零感知)
```

### 类型映射

| Objective-C 类型 | SHM 类型 | JavaScript 类型 |
|-----------------|----------|----------------|
| NSNull | SHM_TYPE_NULL | null |
| NSNumber (bool) | SHM_TYPE_BOOL_SCALAR | boolean |
| NSNumber (int) | SHM_TYPE_INT_SCALAR | number |
| NSNumber (float) | SHM_TYPE_FLOAT_SCALAR | number |
| NSString | SHM_TYPE_STRING | string |
| NSArray (bool) | SHM_TYPE_BOOL_VECTOR | boolean[] |
| NSArray (number) | SHM_TYPE_FLOAT_VECTOR | number[] |
| NSArray (mixed) | SHM_TYPE_LIST | any[] |
| NSDictionary | SHM_TYPE_OBJECT | object |

---

## 性能考虑

当前实现为了实现零感知，在获取数据时会将整个对象树转换为普通 JS 对象。这意味着：

1. **优点**：完全兼容所有 JS 操作，用户代码无需修改
2. **缺点**：失去了 lazy loading 的优势

**未来优化方向**：
- 可以提供两种 API：
  - `getShmProxy()` - 返回普通 JS 对象（零感知，当前实现）
  - `getShmProxyLazy()` - 返回 HostObject（lazy loading，适用于只访问部分数据的场景）

---

## 文件修改清单

1. `ios/ShmProxyBenchmark/shm_kv_c_api.h` - 添加 SHM_TYPE_NULL
2. `ios/ShmProxyBenchmark/NSDictionaryToShm.mm` - null 支持、布尔数组支持
3. `ios/ShmProxyBenchmark/ShmProxyObject.h` - 添加 convertTopLevelToJsObject 声明
4. `ios/ShmProxyBenchmark/ShmProxyObject.mm` - 核心转换逻辑
5. `ios/ShmProxyBenchmark/BenchmarkModule.mm` - JSI 绑定修改
6. `App.tsx` - 零感知测试代码

---

## 结论

ShmProxy 零感知实现已完成。通过将 shm 数据转换为普通 JS 对象，实现了与传统方法完全一致的行为。所有 JavaScript 标准操作（数组方法、对象方法、迭代器、spread 操作符等）都能正常工作，用户代码无需任何修改即可使用 ShmProxy。
