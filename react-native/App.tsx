/**
 * ShmProxy Benchmark App
 *
 * 正确的测试方案：
 * 1. 预加载：启动时把数据加载到内存（不计时）
 * 2. Traditional：测量 NSDictionary → folly → jsi 的完整转换时间
 * 3. ShmProxy：NSDictionary → shm + 返回 HostObject（lazy loading）
 * 4. 对比：部分访问 vs 全量访问
 */

import React, {useState, useCallback, useEffect} from 'react';
import {
  SafeAreaView,
  ScrollView,
  View,
  Text,
  TouchableOpacity,
  StyleSheet,
  NativeModules,
  Alert,
} from 'react-native';
import RNFS from 'react-native-fs';

const {BenchmarkModule} = NativeModules;

// 声明全局 JSI 函数
declare global {
  var __benchmark_getShmProxy: (key: string) => any;
  var __benchmark_getTimeNanos: () => number;
  var __shm_write: (obj: any) => string;
  var __shm_read: (key: string) => any;
}

// Types
interface BenchmarkResult {
  dataSize: string;
  method: string;
  accessType: 'partial' | 'full';
  e2e_ms: number;
  convert_ms: number;
  access_ms: number;
  correctness: CorrectnessResult;
  timestamp: string;
}

interface CorrectnessResult {
  passed: boolean;
  tests: {[key: string]: boolean};
  errors: string[];
}

// Data sizes
const DATA_SIZES = ['128KB', '256KB', '512KB', '1MB', '2MB', '5MB', '10MB', '20MB'] as const;
type DataSize = (typeof DATA_SIZES)[number];

// 高精度计时（纳秒）
function getTimeNanos(): number {
  if (typeof global.__benchmark_getTimeNanos === 'function') {
    return global.__benchmark_getTimeNanos();
  }
  return performance.now() * 1000000;
}

function nanosToMs(nanos: number): number {
  return nanos / 1000000;
}

// 零感知测试：深度比较两个值是否完全相等
function deepEqual(a: any, b: any, path: string = ''): {equal: boolean; diff?: string} {
  // 处理 null 和 undefined
  if (a === null && b === null) return {equal: true};
  if (a === undefined && b === undefined) return {equal: true};
  if (a === null || b === null || a === undefined || b === undefined) {
    return {equal: false, diff: `${path}: ${a} !== ${b}`};
  }

  // 处理基本类型
  if (typeof a !== typeof b) {
    return {equal: false, diff: `${path}: type ${typeof a} !== ${typeof b}`};
  }

  if (typeof a === 'number') {
    // 浮点数比较允许小误差
    if (Math.abs(a - b) > 0.0000001) {
      return {equal: false, diff: `${path}: ${a} !== ${b}`};
    }
    return {equal: true};
  }

  if (typeof a === 'string' || typeof a === 'boolean') {
    if (a !== b) {
      return {equal: false, diff: `${path}: ${a} !== ${b}`};
    }
    return {equal: true};
  }

  // 处理数组
  if (Array.isArray(a) && Array.isArray(b)) {
    if (a.length !== b.length) {
      return {equal: false, diff: `${path}: array length ${a.length} !== ${b.length}`};
    }
    for (let i = 0; i < a.length; i++) {
      const result = deepEqual(a[i], b[i], `${path}[${i}]`);
      if (!result.equal) return result;
    }
    return {equal: true};
  }

  if (Array.isArray(a) !== Array.isArray(b)) {
    return {equal: false, diff: `${path}: one is array, other is not`};
  }

  // 处理对象
  if (typeof a === 'object') {
    const keysA = Object.keys(a).sort();
    const keysB = Object.keys(b).sort();

    if (keysA.length !== keysB.length) {
      return {equal: false, diff: `${path}: object keys count ${keysA.length} !== ${keysB.length}`};
    }

    for (let i = 0; i < keysA.length; i++) {
      if (keysA[i] !== keysB[i]) {
        return {equal: false, diff: `${path}: key ${keysA[i]} !== ${keysB[i]}`};
      }
    }

    for (const key of keysA) {
      const result = deepEqual(a[key], b[key], `${path}.${key}`);
      if (!result.equal) return result;
    }
    return {equal: true};
  }

  return {equal: a === b, diff: a === b ? undefined : `${path}: ${a} !== ${b}`};
}

// 零感知测试结果
interface ZeroAwarenessResult {
  passed: boolean;
  totalFields: number;
  matchedFields: number;
  differences: string[];
}

// 执行零感知测试
function runZeroAwarenessTest(traditional: any, shmProxy: any): ZeroAwarenessResult {
  const differences: string[] = [];
  let totalFields = 0;
  let matchedFields = 0;

  // 测试所有顶级字段
  const allKeys = new Set([...Object.keys(traditional), ...Object.keys(shmProxy)]);

  for (const key of allKeys) {
    totalFields++;
    const result = deepEqual(traditional[key], shmProxy[key], key);
    if (result.equal) {
      matchedFields++;
    } else {
      differences.push(result.diff || `${key}: mismatch`);
    }
  }

  // 额外测试：确保行为一致
  const behaviorTests: {name: string; test: () => boolean}[] = [
    {
      name: 'typeof',
      test: () => typeof traditional === typeof shmProxy,
    },
    {
      name: 'JSON.stringify length',
      test: () => {
        const jsonT = JSON.stringify(traditional);
        const jsonS = JSON.stringify(shmProxy);
        console.log(`JSON.stringify: traditional=${jsonT.length}, shmProxy=${jsonS.length}`);
        return jsonT.length === jsonS.length;
      },
    },
    {
      name: 'Object.keys count',
      test: () => {
        const keysT = Object.keys(traditional);
        const keysS = Object.keys(shmProxy);
        console.log(`Object.keys: traditional=${keysT.length} [${keysT.join(',')}], shmProxy=${keysS.length} [${keysS.join(',')}]`);
        return keysT.length === keysS.length;
      },
    },
    {
      name: 'Object.values count',
      test: () => {
        const valuesT = Object.values(traditional);
        const valuesS = Object.values(shmProxy);
        console.log(`Object.values: traditional=${valuesT.length}, shmProxy=${valuesS.length}`);
        return valuesT.length === valuesS.length;
      },
    },
  ];

  for (const bt of behaviorTests) {
    totalFields++;
    try {
      if (bt.test()) {
        matchedFields++;
      } else {
        differences.push(`Behavior: ${bt.name} mismatch`);
      }
    } catch (e: any) {
      differences.push(`Behavior: ${bt.name} error - ${e.message}`);
    }
  }

  return {
    passed: differences.length === 0,
    totalFields,
    matchedFields,
    differences,
  };
}

// 正确性验证
function verifyCorrectness(data: any): CorrectnessResult {
  const tests: {[key: string]: boolean} = {};
  const errors: string[] = [];

  try {
    // Test basic types
    tests['string_field'] = data.basic_types?.string_field === 'Hello World';
    tests['int_field'] = data.basic_types?.int_field === 12345;
    tests['float_field'] =
      Math.abs((data.basic_types?.float_field ?? 0) - 3.14159265358979) < 0.0001;
    tests['bool_true'] = data.basic_types?.bool_true === true;
    tests['bool_false'] = data.basic_types?.bool_false === false;
    tests['null_field'] = data.basic_types?.null_field === null;
    tests['chinese_string'] =
      data.basic_types?.chinese_string === '中文测试字符串';

    // Test nested object
    tests['nested_deep'] =
      data.nested_object?.level1?.level2?.level3?.deep_string ===
      'deeply nested value';

    // 阶段四：测试对象操作
    const basicTypes = data.basic_types;
    if (basicTypes) {
      // Object.keys()
      try {
        const keys = Object.keys(basicTypes);
        console.log('Object.keys result:', keys);
        tests['object_keys'] = Array.isArray(keys) && keys.length > 0;
      } catch (e: any) {
        console.log('Object.keys error:', e.message);
        tests['object_keys'] = false;
      }

      // Object.values()
      try {
        const values = Object.values(basicTypes);
        console.log('Object.values result:', values);
        tests['object_values'] = Array.isArray(values) && values.length > 0;
      } catch (e: any) {
        console.log('Object.values error:', e.message);
        tests['object_values'] = false;
      }

      // Object.entries()
      try {
        const entries = Object.entries(basicTypes);
        console.log('Object.entries result:', entries);
        tests['object_entries'] = Array.isArray(entries) && entries.length > 0 && Array.isArray(entries[0]);
      } catch (e: any) {
        console.log('Object.entries error:', e.message);
        tests['object_entries'] = false;
      }

      // for...in 循环
      try {
        let forInCount = 0;
        for (const key in basicTypes) {
          forInCount++;
        }
        tests['object_forIn'] = forInCount > 0;
      } catch {
        tests['object_forIn'] = false;
      }

      // hasOwnProperty
      try {
        tests['object_hasOwnProperty'] = basicTypes.hasOwnProperty('string_field') ||
          Object.prototype.hasOwnProperty.call(basicTypes, 'string_field');
      } catch {
        tests['object_hasOwnProperty'] = false;
      }

      // in 操作符
      try {
        tests['object_in'] = 'string_field' in basicTypes;
      } catch {
        tests['object_in'] = false;
      }

      // spread 操作符
      try {
        const spread = {...basicTypes};
        console.log('spread result:', spread);
        tests['object_spread'] = typeof spread === 'object' && spread.string_field === 'Hello World';
      } catch (e: any) {
        console.log('spread error:', e.message);
        tests['object_spread'] = false;
      }

      // JSON.stringify (已在 full access 中测试，这里再验证一次)
      try {
        const json = JSON.stringify(basicTypes);
        tests['object_stringify'] = typeof json === 'string' && json.length > 0;
      } catch {
        tests['object_stringify'] = false;
      }
    }

    // Test arrays - 现在应该通过 Array.isArray() 检测
    const intArray = data.arrays?.int_array;
    tests['int_array_length'] = intArray?.length === 100;
    tests['int_array_first'] = intArray?.[0] === 1;
    tests['int_array_isArray'] = Array.isArray(intArray);

    // Test song data
    tests['song_title'] = data.song?.title === 'Soul Deep';
    tests['song_year'] = data.song?.year === 1969;

    // Test segments - 现在应该通过 Array.isArray() 检测
    const segments = data.song?.analysis?.segments;
    tests['segments_isArray'] = Array.isArray(segments);
    tests['segments_length'] = segments?.length > 0;
    if (segments?.length > 0) {
      const firstSegment = segments[0];
      const pitches = firstSegment?.pitches;
      tests['pitches_isArray'] = Array.isArray(pitches);
      tests['segment_pitches_length'] = pitches?.length === 12;

      // 阶段三：测试数组方法
      // forEach
      let forEachCount = 0;
      pitches?.forEach(() => forEachCount++);
      tests['array_forEach'] = forEachCount === 12;

      // map
      const mapped = pitches?.map((x: number) => x * 2);
      tests['array_map'] = Array.isArray(mapped) && mapped?.length === 12;

      // filter
      const filtered = pitches?.filter((x: number) => x > 0);
      tests['array_filter'] = Array.isArray(filtered);

      // reduce
      const sum = pitches?.reduce((acc: number, x: number) => acc + x, 0);
      tests['array_reduce'] = typeof sum === 'number';

      // find
      const found = pitches?.find((x: number) => x > 0);
      tests['array_find'] = found === undefined || typeof found === 'number';

      // some/every
      tests['array_some'] = typeof pitches?.some((x: number) => x > 0) === 'boolean';
      tests['array_every'] = typeof pitches?.every((x: number) => x >= 0) === 'boolean';

      // indexOf
      tests['array_indexOf'] = typeof pitches?.indexOf(pitches[0]) === 'number';

      // includes
      tests['array_includes'] = typeof pitches?.includes(pitches[0]) === 'boolean';

      // slice
      const sliced = pitches?.slice(0, 3);
      tests['array_slice'] = Array.isArray(sliced) && sliced?.length === 3;

      // join
      const joined = pitches?.slice(0, 3)?.join(',');
      tests['array_join'] = typeof joined === 'string';

      // for...of 迭代器
      let iterCount = 0;
      try {
        for (const _ of pitches) {
          iterCount++;
        }
        tests['array_forOf'] = iterCount === 12;
      } catch {
        tests['array_forOf'] = false;
      }

      // spread 操作符
      try {
        const spread = [...pitches];
        tests['array_spread'] = Array.isArray(spread) && spread.length === 12;
      } catch {
        tests['array_spread'] = false;
      }
    }

    // Check for failures
    for (const [testName, passed] of Object.entries(tests)) {
      if (!passed) {
        errors.push(`Failed: ${testName}`);
      }
    }
  } catch (e: any) {
    errors.push(`Exception: ${e.message}`);
  }

  return {
    passed: errors.length === 0,
    tests,
    errors,
  };
}

// 部分访问（只访问几个字段）
function accessPartialData(data: any): {time_ms: number; result: any} {
  const t0 = getTimeNanos();

  // 只访问少量字段
  const result = {
    title: data.song?.title,
    artist: data.song?.artist_name,
    tempo: data.song?.analysis?.tempo,
    firstPitch: data.song?.analysis?.segments?.[0]?.pitches?.[0],
  };

  const t1 = getTimeNanos();
  return {time_ms: nanosToMs(t1 - t0), result};
}

// 全量访问（遍历所有数据）
function accessFullData(data: any): {time_ms: number; result: any} {
  const t0 = getTimeNanos();

  // 强制遍历所有数据
  const jsonStr = JSON.stringify(data);

  const t1 = getTimeNanos();
  return {time_ms: nanosToMs(t1 - t0), result: jsonStr.length};
}

export default function App() {
  const [results, setResults] = useState<BenchmarkResult[]>([]);
  const [isRunning, setIsRunning] = useState(false);
  const [currentTest, setCurrentTest] = useState<string>('');
  const [isPreloaded, setIsPreloaded] = useState(false);
  const [preloadInfo, setPreloadInfo] = useState<string>('');
  const [zeroAwarenessResults, setZeroAwarenessResults] = useState<{[size: string]: ZeroAwarenessResult} | null>(null);

  // 预加载数据
  const preloadData = useCallback(async () => {
    try {
      setCurrentTest('Installing JSI bindings...');

      // 先安装 JSI bindings（同步方法）
      const jsiInstalled = BenchmarkModule.installJSIBindingsSync();
      if (!jsiInstalled) {
        console.warn('JSI bindings installation returned false, retrying...');
        // 重试一次
        await new Promise(resolve => setTimeout(resolve, 500));
        BenchmarkModule.installJSIBindingsSync();
      }

      setCurrentTest('Preloading data...');
      const result = await BenchmarkModule.preloadAllData();
      setIsPreloaded(true);
      const times = Object.entries(result.loadTimes)
        .map(([k, v]) => `${k}: ${(v as number).toFixed(1)}ms`)
        .join(', ');
      setPreloadInfo(`Preloaded: ${times}`);
      setCurrentTest('Data preloaded!');
    } catch (e: any) {
      Alert.alert('Preload Failed', e.message);
    }
  }, []);

  // 启动时预加载
  useEffect(() => {
    preloadData();
  }, [preloadData]);

  // Traditional 方法测试
  const runTraditionalBenchmark = useCallback(
    async (
      dataSize: DataSize,
      accessType: 'partial' | 'full',
    ): Promise<BenchmarkResult> => {
      // ========== 转换时间 ==========
      const t_convert_start = getTimeNanos();

      // 调用 Native，获取完整数据（包含 NSDictionary → JS 转换）
      const response = await BenchmarkModule.getDataTraditional(dataSize);

      const t_convert_end = getTimeNanos();
      const convert_ms = nanosToMs(t_convert_end - t_convert_start);

      // ========== 访问时间 ==========
      const t_access_start = getTimeNanos();

      const accessResult =
        accessType === 'partial'
          ? accessPartialData(response.data)
          : accessFullData(response.data);

      const t_access_end = getTimeNanos();
      const access_ms = nanosToMs(t_access_end - t_access_start);

      // 验证正确性
      const correctness = verifyCorrectness(response.data);

      return {
        dataSize,
        method: 'traditional',
        accessType,
        e2e_ms: convert_ms + access_ms,
        convert_ms,
        access_ms,
        correctness,
        timestamp: new Date().toISOString(),
      };
    },
    [],
  );

  // ShmProxy 方法测试
  const runShmProxyBenchmark = useCallback(
    async (
      dataSize: DataSize,
      accessType: 'partial' | 'full',
    ): Promise<BenchmarkResult> => {
      // ========== 转换时间 ==========
      const t_convert_start = getTimeNanos();

      // 1. 准备 shm（NSDictionary → shm）
      const prepareResult = await BenchmarkModule.prepareShmProxy(dataSize);

      // 2. 通过 JSI 获取 JS Object
      if (typeof global.__benchmark_getShmProxy !== 'function') {
        throw new Error('JSI bindings not installed');
      }

      const data = global.__benchmark_getShmProxy(prepareResult.shmKey);

      const t_convert_end = getTimeNanos();
      const convert_ms = nanosToMs(t_convert_end - t_convert_start);

      if (!data) {
        throw new Error('Failed to get data from shm');
      }

      // ========== 访问时间 ==========
      const t_access_start = getTimeNanos();

      const accessResult =
        accessType === 'partial'
          ? accessPartialData(data)
          : accessFullData(data);

      const t_access_end = getTimeNanos();
      const access_ms = nanosToMs(t_access_end - t_access_start);

      // 验证正确性
      const correctness = verifyCorrectness(data);

      return {
        dataSize,
        method: 'shmproxy',
        accessType,
        e2e_ms: convert_ms + access_ms,
        convert_ms,
        access_ms,
        correctness,
        timestamp: new Date().toISOString(),
      };
    },
    [],
  );

  // JSON String 方法测试
  const runJsonStringBenchmark = useCallback(
    async (
      dataSize: DataSize,
      accessType: 'partial' | 'full',
    ): Promise<BenchmarkResult> => {
      // ========== 转换时间 ==========
      const t_convert_start = getTimeNanos();

      // 1. 获取 JSON 字符串
      const response = await BenchmarkModule.getDataJsonString(dataSize);

      // 2. JS 端解析 JSON
      const data = JSON.parse(response.jsonString);

      const t_convert_end = getTimeNanos();
      const convert_ms = nanosToMs(t_convert_end - t_convert_start);

      // ========== 访问时间 ==========
      const t_access_start = getTimeNanos();

      const accessResult =
        accessType === 'partial'
          ? accessPartialData(data)
          : accessFullData(data);

      const t_access_end = getTimeNanos();
      const access_ms = nanosToMs(t_access_end - t_access_start);

      // 验证正确性
      const correctness = verifyCorrectness(data);

      return {
        dataSize,
        method: 'json_string',
        accessType,
        e2e_ms: convert_ms + access_ms,
        convert_ms,
        access_ms,
        correctness,
        timestamp: new Date().toISOString(),
      };
    },
    [],
  );

  // 运行所有测试
  const runAllBenchmarks = useCallback(async () => {
    if (!isPreloaded) {
      Alert.alert('Not Ready', 'Please wait for data to preload');
      return;
    }

    setIsRunning(true);
    setResults([]);

    const allResults: BenchmarkResult[] = [];

    try {
      // 清理之前的 shm 数据
      await BenchmarkModule.clearShm();

      for (const size of DATA_SIZES) {
        for (const accessType of ['partial', 'full'] as const) {
          // Traditional
          setCurrentTest(`Traditional ${accessType} - ${size}`);
          const tradResult = await runTraditionalBenchmark(size, accessType);
          allResults.push(tradResult);
          setResults([...allResults]);

          // JSON String
          setCurrentTest(`JSON String ${accessType} - ${size}`);
          const jsonResult = await runJsonStringBenchmark(size, accessType);
          allResults.push(jsonResult);
          setResults([...allResults]);

          // ShmProxy
          setCurrentTest(`ShmProxy ${accessType} - ${size}`);
          const shmResult = await runShmProxyBenchmark(size, accessType);
          allResults.push(shmResult);
          setResults([...allResults]);

          // 小延迟
          await new Promise(resolve => setTimeout(resolve, 50));
        }
      }

      setCurrentTest('Complete!');
    } catch (e: any) {
      Alert.alert('Error', e.message);
      console.error(e);
    } finally {
      setIsRunning(false);
    }
  }, [isPreloaded, runTraditionalBenchmark, runJsonStringBenchmark, runShmProxyBenchmark]);

  // 零感知测试
  const runZeroAwarenessTests = useCallback(async () => {
    if (!isPreloaded) {
      Alert.alert('Not Ready', 'Please wait for data to preload');
      return;
    }

    setIsRunning(true);
    setZeroAwarenessResults(null);
    const zaResults: {[size: string]: ZeroAwarenessResult} = {};

    try {
      await BenchmarkModule.clearShm();

      for (const size of DATA_SIZES) {
        setCurrentTest(`Zero-Awareness Test: ${size}`);

        // 获取 Traditional 数据
        const tradResponse = await BenchmarkModule.getDataTraditional(size);
        const traditionalData = tradResponse.data;

        // 获取 ShmProxy 数据
        const prepareResult = await BenchmarkModule.prepareShmProxy(size);
        const shmProxyData = global.__benchmark_getShmProxy(prepareResult.shmKey);

        // 运行零感知测试
        const result = runZeroAwarenessTest(traditionalData, shmProxyData);
        zaResults[size] = result;

        console.log(`[Zero-Awareness ${size}] Passed: ${result.passed}, Matched: ${result.matchedFields}/${result.totalFields}`);
        if (result.differences.length > 0) {
          console.log(`  Differences:`, result.differences.slice(0, 5));
        }

        await new Promise(resolve => setTimeout(resolve, 50));
      }

      setZeroAwarenessResults(zaResults);

      // 检查是否全部通过
      const allPassed = Object.values(zaResults).every(r => r.passed);
      if (allPassed) {
        setCurrentTest('Zero-Awareness: ALL PASSED!');
      } else {
        setCurrentTest('Zero-Awareness: Some tests failed');
      }
    } catch (e: any) {
      Alert.alert('Error', e.message);
      console.error(e);
    } finally {
      setIsRunning(false);
    }
  }, [isPreloaded]);

  // 导出结果
  const exportResults = useCallback(async () => {
    if (results.length === 0) {
      Alert.alert('No Results', 'Run benchmarks first');
      return;
    }

    const summary = generateSummary(results);

    const exportData = {
      device: 'iOS Simulator',
      timestamp: new Date().toISOString(),
      results,
      summary,
    };

    const filename = `benchmark_v2_${Date.now()}.json`;
    const path = `${RNFS.DocumentDirectoryPath}/${filename}`;

    try {
      await RNFS.writeFile(path, JSON.stringify(exportData, null, 2), 'utf8');
      Alert.alert('Exported', `Results saved to:\n${path}`);
      console.log('Results exported to:', path);
    } catch (e: any) {
      Alert.alert('Export Failed', e.message);
    }
  }, [results]);

  // JS → OC 测试
  const [jsToOcResult, setJsToOcResult] = useState<{
    passed: boolean;
    writeTime: number;
    readTime: number;
    roundTripTime: number;
    details: string[];
  } | null>(null);

  const runJsToOcTest = useCallback(async () => {
    if (!isPreloaded) {
      Alert.alert('Not Ready', 'Please wait for data to preload');
      return;
    }

    setIsRunning(true);
    setCurrentTest('Testing JS → OC conversion...');

    try {
      // 创建测试数据
      const testData = {
        string_field: 'Hello from JavaScript!',
        int_field: 12345,
        float_field: 3.14159265358979,
        bool_true: true,
        bool_false: false,
        null_field: null,
        chinese_string: '中文测试字符串',
        nested_object: {
          level1: {
            level2: {
              deep_string: 'deeply nested value',
            },
          },
        },
        int_array: Array.from({length: 100}, (_, i) => i + 1),
        float_array: Array.from({length: 50}, (_, i) => i * 0.1),
        string_array: ['apple', 'banana', 'cherry'],
        mixed_array: [1, 'two', true, null, {key: 'value'}],
      };

      const details: string[] = [];
      let passed = true;

      // 测试 1: JS → shm (写入)
      const t0 = getTimeNanos();
      const shmKey = global.__shm_write(testData);
      const t1 = getTimeNanos();
      const writeTime = nanosToMs(t1 - t0);
      details.push(`Write to shm: ${writeTime.toFixed(3)} ms`);
      details.push(`Key: ${shmKey}`);

      // 测试 2: shm → JS (读取回来验证)
      const t2 = getTimeNanos();
      const readBack = global.__shm_read(shmKey);
      const t3 = getTimeNanos();
      const readTime = nanosToMs(t3 - t2);
      details.push(`Read from shm: ${readTime.toFixed(3)} ms`);

      // 验证数据
      if (readBack.string_field !== testData.string_field) {
        details.push('FAIL: string_field mismatch');
        passed = false;
      }
      if (readBack.int_field !== testData.int_field) {
        details.push('FAIL: int_field mismatch');
        passed = false;
      }
      if (Math.abs(readBack.float_field - testData.float_field) > 0.0001) {
        details.push('FAIL: float_field mismatch');
        passed = false;
      }
      if (readBack.bool_true !== true || readBack.bool_false !== false) {
        details.push('FAIL: bool fields mismatch');
        passed = false;
      }
      if (readBack.null_field !== null) {
        details.push('FAIL: null_field mismatch');
        passed = false;
      }
      if (readBack.chinese_string !== testData.chinese_string) {
        details.push('FAIL: chinese_string mismatch');
        passed = false;
      }
      if (readBack.nested_object?.level1?.level2?.deep_string !== 'deeply nested value') {
        details.push('FAIL: nested_object mismatch');
        passed = false;
      }
      if (readBack.int_array?.length !== 100 || readBack.int_array[0] !== 1) {
        details.push('FAIL: int_array mismatch');
        passed = false;
      }
      if (readBack.string_array?.length !== 3 || readBack.string_array[0] !== 'apple') {
        details.push('FAIL: string_array mismatch');
        passed = false;
      }

      if (passed) {
        details.push('All data verified correctly!');
      }

      // 测试 3: Native 端读取 (通过 RN Bridge)
      const t4 = getTimeNanos();
      const nativeResult = await BenchmarkModule.readFromShm(shmKey);
      const t5 = getTimeNanos();
      const nativeReadTime = nanosToMs(t5 - t4);
      details.push(`Native read: ${nativeResult.timing.native_read_ms.toFixed(3)} ms`);
      details.push(`Bridge overhead: ${(nativeReadTime - nativeResult.timing.native_read_ms).toFixed(3)} ms`);

      // 验证 Native 读取的数据
      if (nativeResult.data.string_field !== testData.string_field) {
        details.push('FAIL: Native string_field mismatch');
        passed = false;
      }
      if (nativeResult.data.chinese_string !== testData.chinese_string) {
        details.push('FAIL: Native chinese_string mismatch');
        passed = false;
      }

      setJsToOcResult({
        passed,
        writeTime,
        readTime,
        roundTripTime: writeTime + readTime,
        details,
      });

      setCurrentTest(passed ? 'JS → OC Test: PASSED!' : 'JS → OC Test: FAILED');
    } catch (e: any) {
      Alert.alert('Error', e.message);
      console.error(e);
      setJsToOcResult({
        passed: false,
        writeTime: 0,
        readTime: 0,
        roundTripTime: 0,
        details: [`Error: ${e.message}`],
      });
    } finally {
      setIsRunning(false);
    }
  }, [isPreloaded]);

  // 生成摘要
  function generateSummary(allResults: BenchmarkResult[]): any {
    const summary: any = {};

    for (const size of DATA_SIZES) {
      summary[size] = {};

      for (const accessType of ['partial', 'full']) {
        const trad = allResults.find(
          r =>
            r.dataSize === size &&
            r.method === 'traditional' &&
            r.accessType === accessType,
        );
        const json = allResults.find(
          r =>
            r.dataSize === size &&
            r.method === 'json_string' &&
            r.accessType === accessType,
        );
        const shm = allResults.find(
          r =>
            r.dataSize === size &&
            r.method === 'shmproxy' &&
            r.accessType === accessType,
        );

        if (trad) {
          const baseline = trad.e2e_ms;
          summary[size][accessType] = {
            traditional_ms: trad.e2e_ms.toFixed(2),
            json_ms: json ? json.e2e_ms.toFixed(2) : '-',
            shmproxy_ms: shm ? shm.e2e_ms.toFixed(2) : '-',
            json_vs_trad: json ? (((baseline - json.e2e_ms) / baseline) * 100).toFixed(1) + '%' : '-',
            shm_vs_trad: shm ? (((baseline - shm.e2e_ms) / baseline) * 100).toFixed(1) + '%' : '-',
            all_correct: (trad?.correctness.passed ?? false) &&
                         (json?.correctness.passed ?? true) &&
                         (shm?.correctness.passed ?? true),
          };
        }
      }
    }

    return summary;
  }

  return (
    <SafeAreaView style={styles.container}>
      <ScrollView contentContainerStyle={styles.scrollContent}>
        <Text style={styles.title}>ShmProxy Benchmark v2</Text>
        <Text style={styles.subtitle}>Lazy Loading Test</Text>

        {/* 预加载状态 */}
        {preloadInfo && <Text style={styles.preloadInfo}>{preloadInfo}</Text>}

        {/* 控制按钮 */}
        <View style={styles.buttonRow}>
          <TouchableOpacity
            style={[
              styles.button,
              (!isPreloaded || isRunning) && styles.buttonDisabled,
            ]}
            onPress={runAllBenchmarks}
            disabled={!isPreloaded || isRunning}>
            <Text style={styles.buttonText}>
              {isRunning ? 'Running...' : 'Benchmark'}
            </Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={[
              styles.button,
              styles.zeroAwarenessButton,
              (!isPreloaded || isRunning) && styles.buttonDisabled,
            ]}
            onPress={runZeroAwarenessTests}
            disabled={!isPreloaded || isRunning}>
            <Text style={styles.buttonText}>Zero-Test</Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={[styles.button, styles.exportButton]}
            onPress={exportResults}
            disabled={results.length === 0}>
            <Text style={styles.buttonText}>Export</Text>
          </TouchableOpacity>
        </View>

        {/* JS → OC 测试按钮 */}
        <View style={styles.buttonRow}>
          <TouchableOpacity
            style={[
              styles.button,
              styles.jsToOcButton,
              (!isPreloaded || isRunning) && styles.buttonDisabled,
            ]}
            onPress={runJsToOcTest}
            disabled={!isPreloaded || isRunning}>
            <Text style={styles.buttonText}>JS → OC Test</Text>
          </TouchableOpacity>
        </View>

        {/* JS → OC 测试结果 */}
        {jsToOcResult && (
          <View style={[
            styles.jsToOcContainer,
            jsToOcResult.passed ? styles.zaResultPassed : styles.zaResultFailed
          ]}>
            <View style={styles.zaResultHeader}>
              <Text style={styles.zaResultSize}>JS → OC</Text>
              <Text style={[
                styles.zaResultStatus,
                jsToOcResult.passed ? styles.passed : styles.failed
              ]}>
                {jsToOcResult.passed ? 'PASSED' : 'FAILED'}
              </Text>
            </View>
            <Text style={styles.zaResultDetail}>
              Write: {jsToOcResult.writeTime.toFixed(3)} ms | Read: {jsToOcResult.readTime.toFixed(3)} ms
            </Text>
            <Text style={styles.zaResultDetail}>
              Round-trip: {jsToOcResult.roundTripTime.toFixed(3)} ms
            </Text>
            {jsToOcResult.details.map((detail, idx) => (
              <Text key={idx} style={styles.jsToOcDetail}>{detail}</Text>
            ))}
          </View>
        )}

        {/* 零感知测试结果 */}
        {zeroAwarenessResults && (
          <View style={styles.zeroAwarenessContainer}>
            <Text style={styles.sectionTitle}>Zero-Awareness Test</Text>
            {Object.entries(zeroAwarenessResults).map(([size, result]) => (
              <View key={size} style={[
                styles.zaResultCard,
                result.passed ? styles.zaResultPassed : styles.zaResultFailed
              ]}>
                <View style={styles.zaResultHeader}>
                  <Text style={styles.zaResultSize}>{size.toUpperCase()}</Text>
                  <Text style={[
                    styles.zaResultStatus,
                    result.passed ? styles.passed : styles.failed
                  ]}>
                    {result.passed ? 'PASSED' : 'FAILED'}
                  </Text>
                </View>
                <Text style={styles.zaResultDetail}>
                  Matched: {result.matchedFields}/{result.totalFields} fields
                </Text>
                {result.differences.length > 0 && (
                  <Text style={styles.zaResultDiff}>
                    {result.differences.slice(0, 3).join('\n')}
                  </Text>
                )}
              </View>
            ))}
          </View>
        )}

        {/* 当前测试 */}
        {currentTest && (
          <Text style={styles.currentTest}>{currentTest}</Text>
        )}

        {/* 结果 */}
        {results.length > 0 && (
          <View style={styles.resultsContainer}>
            <Text style={styles.sectionTitle}>Results</Text>

            {results.map((result, index) => (
              <View
                key={index}
                style={[
                  styles.resultCard,
                  result.method === 'shmproxy' && styles.resultCardShm,
                  result.method === 'json_string' && styles.resultCardJson,
                ]}>
                <View style={styles.resultHeader}>
                  <Text style={styles.resultMethod}>
                    {result.method.toUpperCase()}
                  </Text>
                  <Text style={styles.resultSize}>
                    {result.dataSize} / {result.accessType}
                  </Text>
                </View>

                <View style={styles.resultRow}>
                  <Text style={styles.resultLabel}>Total E2E:</Text>
                  <Text style={styles.resultValue}>
                    {result.e2e_ms.toFixed(2)} ms
                  </Text>
                </View>

                <View style={styles.resultRow}>
                  <Text style={styles.resultLabel}>Convert:</Text>
                  <Text style={styles.resultValue}>
                    {result.convert_ms.toFixed(2)} ms
                  </Text>
                </View>

                <View style={styles.resultRow}>
                  <Text style={styles.resultLabel}>Access:</Text>
                  <Text style={styles.resultValue}>
                    {result.access_ms.toFixed(3)} ms
                  </Text>
                </View>

                <View style={styles.resultRow}>
                  <Text style={styles.resultLabel}>Correct:</Text>
                  <Text
                    style={[
                      styles.resultValue,
                      result.correctness.passed ? styles.passed : styles.failed,
                    ]}>
                    {result.correctness.passed ? '✓' : '✗'}
                  </Text>
                </View>

                {!result.correctness.passed && (
                  <Text style={styles.errorText}>
                    {result.correctness.errors.slice(0, 3).join(', ')}
                  </Text>
                )}
              </View>
            ))}

            {/* 摘要 */}
            {results.length >= 8 && (
              <View style={styles.summaryContainer}>
                <Text style={styles.sectionTitle}>Summary (vs Traditional)</Text>
                {Object.entries(generateSummary(results)).map(
                  ([size, data]: [string, any]) => (
                    <View key={size} style={styles.summarySection}>
                      <Text style={styles.summarySize}>{size.toUpperCase()}</Text>
                      {data.partial && (
                        <View>
                          <Text style={styles.summarySubtitle}>Partial Access:</Text>
                          <Text style={styles.summaryLine}>
                            JSON: {data.partial.json_vs_trad}
                          </Text>
                          <Text style={[styles.summaryLine, styles.summaryHighlight]}>
                            ShmProxy: {data.partial.shm_vs_trad}
                          </Text>
                        </View>
                      )}
                      {data.full && (
                        <View>
                          <Text style={styles.summarySubtitle}>Full Access:</Text>
                          <Text style={styles.summaryLine}>
                            JSON: {data.full.json_vs_trad}
                          </Text>
                          <Text style={[styles.summaryLine, styles.summaryHighlight]}>
                            ShmProxy: {data.full.shm_vs_trad}
                          </Text>
                        </View>
                      )}
                    </View>
                  ),
                )}
              </View>
            )}
          </View>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#f5f5f5',
  },
  scrollContent: {
    padding: 16,
  },
  title: {
    fontSize: 24,
    fontWeight: 'bold',
    textAlign: 'center',
    color: '#333',
  },
  subtitle: {
    fontSize: 14,
    textAlign: 'center',
    color: '#666',
    marginBottom: 16,
  },
  preloadInfo: {
    textAlign: 'center',
    color: '#34C759',
    fontSize: 12,
    marginBottom: 12,
  },
  buttonRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    marginBottom: 16,
  },
  button: {
    flex: 1,
    backgroundColor: '#007AFF',
    padding: 14,
    borderRadius: 8,
    marginHorizontal: 4,
  },
  buttonDisabled: {
    backgroundColor: '#999',
  },
  exportButton: {
    backgroundColor: '#34C759',
    flex: 0.4,
  },
  jsToOcButton: {
    backgroundColor: '#FF6B35',
    flex: 1,
  },
  jsToOcContainer: {
    borderRadius: 8,
    padding: 12,
    marginBottom: 16,
    borderLeftWidth: 4,
  },
  jsToOcDetail: {
    fontSize: 11,
    color: '#666',
    marginTop: 2,
    fontFamily: 'monospace',
  },
  buttonText: {
    color: 'white',
    textAlign: 'center',
    fontWeight: '600',
    fontSize: 14,
  },
  currentTest: {
    textAlign: 'center',
    color: '#666',
    marginBottom: 16,
    fontStyle: 'italic',
  },
  resultsContainer: {
    marginTop: 8,
  },
  sectionTitle: {
    fontSize: 18,
    fontWeight: '600',
    marginBottom: 12,
    color: '#333',
  },
  resultCard: {
    backgroundColor: 'white',
    borderRadius: 8,
    padding: 10,
    marginBottom: 8,
    borderLeftWidth: 3,
    borderLeftColor: '#007AFF',
  },
  resultCardShm: {
    borderLeftColor: '#FF9500',
  },
  resultCardJson: {
    borderLeftColor: '#34C759',
  },
  resultHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    marginBottom: 6,
    paddingBottom: 6,
    borderBottomWidth: 1,
    borderBottomColor: '#eee',
  },
  resultMethod: {
    fontWeight: 'bold',
    fontSize: 12,
    color: '#007AFF',
  },
  resultSize: {
    color: '#666',
    fontSize: 12,
  },
  resultRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    paddingVertical: 2,
  },
  resultLabel: {
    color: '#666',
    fontSize: 12,
  },
  resultValue: {
    fontWeight: '500',
    color: '#333',
    fontSize: 12,
  },
  passed: {
    color: '#34C759',
  },
  failed: {
    color: '#FF3B30',
  },
  errorText: {
    color: '#FF3B30',
    fontSize: 10,
    marginTop: 4,
  },
  summaryContainer: {
    backgroundColor: '#E8F4FD',
    borderRadius: 8,
    padding: 12,
    marginTop: 12,
  },
  summarySection: {
    marginBottom: 8,
  },
  summarySize: {
    fontWeight: '600',
    fontSize: 14,
    color: '#333',
  },
  summaryLine: {
    fontSize: 12,
    color: '#007AFF',
    marginLeft: 8,
  },
  summarySubtitle: {
    fontSize: 11,
    color: '#666',
    marginLeft: 4,
    marginTop: 4,
    fontStyle: 'italic',
  },
  summaryHighlight: {
    color: '#FF9500',
    fontWeight: '600',
  },
  zeroAwarenessButton: {
    backgroundColor: '#5856D6',
  },
  zeroAwarenessContainer: {
    marginBottom: 16,
  },
  zaResultCard: {
    borderRadius: 8,
    padding: 10,
    marginBottom: 8,
    borderLeftWidth: 4,
  },
  zaResultPassed: {
    backgroundColor: '#E8F5E9',
    borderLeftColor: '#34C759',
  },
  zaResultFailed: {
    backgroundColor: '#FFEBEE',
    borderLeftColor: '#FF3B30',
  },
  zaResultHeader: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    marginBottom: 4,
  },
  zaResultSize: {
    fontWeight: 'bold',
    fontSize: 14,
    color: '#333',
  },
  zaResultStatus: {
    fontWeight: 'bold',
    fontSize: 12,
  },
  zaResultDetail: {
    fontSize: 12,
    color: '#666',
  },
  zaResultDiff: {
    fontSize: 10,
    color: '#FF3B30',
    marginTop: 4,
    fontFamily: 'monospace',
  },
});
