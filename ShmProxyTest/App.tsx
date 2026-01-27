/**
 * ShmProxy 测试应用
 * 验证 ShmProxy 模块是否正常工作
 */

import React, { useEffect, useState } from 'react';
import {
  SafeAreaView,
  StatusBar,
  StyleSheet,
  Text,
  View,
  Button,
  ScrollView,
} from 'react-native';
import { NativeModules } from 'react-native';

function App() {
  const [status, setStatus] = useState('正在检查...');
  const [shmProxyInstalled, setShmProxyInstalled] = useState(false);
  const [shmProxyLazyInstalled, setShmProxyLazyInstalled] = useState(false);
  const [testResult, setTestResult] = useState<string[]>([]);

  useEffect(() => {
    checkNativeModules();
  }, []);

  const checkNativeModules = () => {
    const logs: string[] = [];
    logs.push('=== Native Modules 检查 ===');

    // 检查 NativeModules
    const modules = Object.keys(NativeModules);
    logs.push(`总模块数: ${modules.length}`);
    logs.push(`所有模块: ${modules.slice(0, 10).join(', ')}${modules.length > 10 ? '...' : ''}`);

    // 检查 ShmProxy
    const hasShmProxy = 'ShmProxy' in NativeModules;
    const hasShmProxyLazy = 'ShmProxyLazy' in NativeModules;

    logs.push(`ShmProxy: ${hasShmProxy ? '✅ 已安装' : '❌ 未找到'}`);
    logs.push(`ShmProxyLazy: ${hasShmProxyLazy ? '✅ 已安装' : '❌ 未找到'}`);

    setShmProxyInstalled(hasShmProxy);
    setShmProxyLazyInstalled(hasShmProxyLazy);
    setTestResult(logs);

    if (hasShmProxy || hasShmProxyLazy) {
      setStatus('✅ ShmProxy 模块已加载');
    } else {
      setStatus('❌ ShmProxy 模块未找到');
    }

    console.log(logs.join('\n'));
  };

  const testShmProxy = async () => {
    const logs: string[] = [...testResult];
    logs.push('\n=== 测试 ShmProxy ===');

    try {
      // 导入 ShmProxy
      const { ShmProxy } = require('react-native-shmproxy');
      logs.push('✅ ShmProxy 模块导入成功');

      // 测试安装 JSI bindings (同步方法)
      const installed = ShmProxy.installJSIBindingsSync();
      logs.push(`JSI Bindings 安装: ${installed ? '✅ 成功' : '❌ 失败'}`);

      // 测试写入数据
      const testData = {
        title: '测试歌曲',
        artist: '测试艺术家',
        year: 2024,
        segments: [
          { start: 0, end: 30 },
          { start: 30, end: 60 },
        ],
      };

      const key = await ShmProxy.write(testData);
      logs.push(`✅ 数据写入成功，key: ${key}`);

      // 测试读取数据 - 使用全局 JSI 函数
      // @ts-ignore
      const hasJsiRead = typeof global.__shm_read === 'function';
      logs.push(`检查 __shm_read 函数: ${hasJsiRead ? '✅ 存在' : '❌ 不存在'}`);

      if (hasJsiRead) {
        // @ts-ignore
        const data = global.__shm_read(key);
        logs.push(`✅ 数据读取成功: ${JSON.stringify(data).substring(0, 80)}...`);
        logs.push(`  title: ${data.title}, artist: ${data.artist}`);
      } else {
        logs.push('⚠️ __shm_read 函数未安装');
      }

      // 测试获取统计信息
      const stats = await ShmProxy.getStats();
      logs.push(`✅ 内存使用: ${stats.payloadUsed} / ${stats.payloadCapacity} bytes`);

      logs.push('\n🎉 ShmProxy 基本测试通过！');
    } catch (error: any) {
      logs.push(`❌ 错误: ${error.message}`);
      logs.push(`\n⚠️ ShmProxy 测试失败`);
    }

    setTestResult(logs);
  };

  const testShmProxyLazy = async () => {
    const logs: string[] = [...testResult];
    logs.push('\n=== 测试 ShmProxyLazy ===');

    try {
      // 导入 ShmProxyLazy
      const { ShmProxyLazy } = require('react-native-shmproxy-lazy');
      logs.push('✅ ShmProxyLazy 模块导入成功');

      // 检查共享内存状态
      const isInit = await ShmProxyLazy.isInitialized();
      logs.push(`共享内存状态: ${isInit ? '✅ 已初始化' : '❌ 未初始化'}`);

      // 如果未初始化，通过 Native 模块初始化
      if (!isInit) {
        logs.push('正在初始化共享内存...');
        // @ts-ignore - 直接调用 Native 方法
        await NativeModules.ShmProxyLazy.initialize();
        logs.push('✅ 共享内存初始化成功');
      }

      // 测试安装 JSI bindings (ShmProxyLazy 使用 async install 方法)
      await ShmProxyLazy.install();
      logs.push('JSI Bindings 安装: ✅ 成功');

      // 测试写入数据
      const testData = {
        title: '测试歌曲（Lazy）',
        artist: '测试艺术家',
        metadata: {
          genre: 'Pop',
          year: 2024,
        },
      };

      const key = await ShmProxyLazy.write(testData);
      logs.push(`✅ 数据写入成功，key: ${key}`);

      // 测试懒加载访问 - 使用 Proxy
      const proxy = ShmProxyLazy.createProxy(key);
      logs.push(`✅ Proxy 创建成功`);

      // 测试访问字段 (只转换 title 字段)
      const title = proxy.title;
      logs.push(`✅ 懒加载访问 title: ${title}`);

      // 测试访问嵌套字段
      const artist = proxy.artist;
      logs.push(`✅ 懒加载访问 artist: ${artist}`);

      // 测试 materialize (全量转换)
      const fullData = await ShmProxyLazy.materialize(key);
      logs.push(`✅ 全量转换成功: ${JSON.stringify(fullData).substring(0, 60)}...`);

      logs.push('\n🎉 ShmProxyLazy 测试通过！');
    } catch (error: any) {
      logs.push(`❌ 错误: ${error.message}`);
      logs.push('\n⚠️ ShmProxyLazy 测试失败');
    }

    setTestResult(logs);
  };

  return (
    <SafeAreaView style={styles.container}>
      <StatusBar barStyle="dark-content" />
      <ScrollView contentContainerStyle={styles.scrollContent}>
        <View style={styles.header}>
          <Text style={styles.title}>🚀 ShmProxy 测试</Text>
          <Text style={styles.status}>{status}</Text>
        </View>

        <View style={styles.section}>
          <Text style={styles.sectionTitle}>模块状态</Text>
          <View style={styles.statusRow}>
            <Text style={styles.label}>ShmProxy:</Text>
            <Text style={[styles.value, shmProxyInstalled && styles.success]}>
              {shmProxyInstalled ? '✅ 已安装' : '❌ 未找到'}
            </Text>
          </View>
          <View style={styles.statusRow}>
            <Text style={styles.label}>ShmProxyLazy:</Text>
            <Text style={[styles.value, shmProxyLazyInstalled && styles.success]}>
              {shmProxyLazyInstalled ? '✅ 已安装' : '❌ 未找到'}
            </Text>
          </View>
        </View>

        <View style={styles.section}>
          <Text style={styles.sectionTitle}>测试操作</Text>
          <View style={styles.buttonContainer}>
            <Button
              title="测试 ShmProxy"
              onPress={testShmProxy}
              disabled={!shmProxyInstalled}
              color="#007AFF"
            />
          </View>
          <View style={styles.buttonContainer}>
            <Button
              title="测试 ShmProxyLazy"
              onPress={testShmProxyLazy}
              disabled={!shmProxyLazyInstalled}
              color="#5856D6"
            />
          </View>
        </View>

        <View style={styles.section}>
          <Text style={styles.sectionTitle}>日志输出</Text>
          <View style={styles.logContainer}>
            <Text style={styles.logText}>{testResult.join('\n')}</Text>
          </View>
        </View>
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#F5F5F5',
  },
  scrollContent: {
    padding: 20,
  },
  header: {
    alignItems: 'center',
    marginBottom: 30,
    paddingVertical: 20,
    backgroundColor: '#FFFFFF',
    borderRadius: 10,
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 2 },
    shadowOpacity: 0.1,
    shadowRadius: 4,
    elevation: 3,
  },
  title: {
    fontSize: 28,
    fontWeight: 'bold',
    color: '#000',
    marginBottom: 10,
  },
  status: {
    fontSize: 16,
    color: '#666',
  },
  section: {
    backgroundColor: '#FFFFFF',
    borderRadius: 10,
    padding: 15,
    marginBottom: 20,
    shadowColor: '#000',
    shadowOffset: { width: 0, height: 1 },
    shadowOpacity: 0.1,
    shadowRadius: 2,
    elevation: 2,
  },
  sectionTitle: {
    fontSize: 20,
    fontWeight: '600',
    color: '#000',
    marginBottom: 15,
  },
  statusRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 8,
    borderBottomWidth: 1,
    borderBottomColor: '#E5E5E5',
  },
  label: {
    fontSize: 16,
    color: '#333',
  },
  value: {
    fontSize: 16,
    fontWeight: '600',
    color: '#666',
  },
  success: {
    color: '#34C759',
  },
  buttonContainer: {
    marginVertical: 8,
  },
  logContainer: {
    backgroundColor: '#1C1C1E',
    borderRadius: 8,
    padding: 12,
    minHeight: 200,
  },
  logText: {
    color: '#00FF00',
    fontFamily: 'Courier',
    fontSize: 12,
    lineHeight: 18,
  },
});

export default App;
