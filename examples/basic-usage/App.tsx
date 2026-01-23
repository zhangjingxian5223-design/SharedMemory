/**
 * Basic Usage Example - ShmProxy
 *
 * 这个示例展示了 ShmProxy 的基础用法
 */

import React, { useEffect, useState } from 'react';
import {
  SafeAreaView,
  Text,
  View,
  TouchableOpacity,
  StyleSheet,
  ScrollView,
  ActivityIndicator,
} from 'react-native';
import { ShmProxy } from 'react-native-shmproxy';

// 示例数据
const generateData = () => ({
  song: {
    title: 'Hello World',
    artist: 'Demo Artist',
    year: 2024,
    genre: 'Pop',
    duration: 183,
    segments: [
      {
        id: 1,
        startTime: 0.0,
        endTime: 5.2,
        pitches: [440, 523, 659],
      },
      {
        id: 2,
        startTime: 5.2,
        endTime: 10.5,
        pitches: [784, 880, 1047],
      },
    ],
  },
  metadata: {
    album: 'Demo Album',
    releaseDate: '2024-01-01',
    label: 'Demo Records',
  },
});

export default function BasicUsageExample() {
  const [shmKey, setShmKey] = useState<string | null>(null);
  const [data, setData] = useState<any>(null);
  const [stats, setStats] = useState<any>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // 1. 写入数据
  const handleWrite = async () => {
    setLoading(true);
    setError(null);

    try {
      const testData = generateData();
      console.log('[Example] Writing data to SHM...');

      const key = await ShmProxy.write(testData);
      console.log('[Example] Data written, key:', key);

      setShmKey(key);
    } catch (err: any) {
      setError(`Write failed: ${err.message}`);
      console.error('[Example] Write error:', err);
    } finally {
      setLoading(false);
    }
  };

  // 2. 读取数据
  const handleRead = async () => {
    if (!shmKey) return;

    setLoading(true);
    setError(null);

    try {
      console.log('[Example] Reading data from SHM...');

      const result = await ShmProxy.read(shmKey);
      console.log('[Example] Data read:', result);

      setData(result);
    } catch (err: any) {
      setError(`Read failed: ${err.message}`);
      console.error('[Example] Read error:', err);
    } finally {
      setLoading(false);
    }
  };

  // 3. 获取统计信息
  const handleGetStats = async () => {
    setLoading(true);
    setError(null);

    try {
      const result = await ShmProxy.getStats();
      console.log('[Example] SHM stats:', result);

      setStats(result);
    } catch (err: any) {
      setError(`Get stats failed: ${err.message}`);
      console.error('[Example] Stats error:', err);
    } finally {
      setLoading(false);
    }
  };

  // 4. 清空 SHM
  const handleClear = async () => {
    setLoading(true);
    setError(null);

    try {
      await ShmProxy.clear();
      console.log('[Example] SHM cleared');

      setShmKey(null);
      setData(null);
      setStats(null);
    } catch (err: any) {
      setError(`Clear failed: ${err.message}`);
      console.error('[Example] Clear error:', err);
    } finally {
      setLoading(false);
    }
  };

  return (
    <SafeAreaView style={styles.container}>
      <ScrollView style={styles.scrollView}>
        <Text style={styles.title}>ShmProxy 基础用法示例</Text>

        {/* 控制按钮 */}
        <View style={styles.buttonContainer}>
          <TouchableOpacity
            style={styles.button}
            onPress={handleWrite}
            disabled={loading}>
            <Text style={styles.buttonText}>1. 写入数据</Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={[styles.button, !shmKey && styles.buttonDisabled]}
            onPress={handleRead}
            disabled={!shmKey || loading}>
            <Text style={styles.buttonText}>2. 读取数据</Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={styles.button}
            onPress={handleGetStats}
            disabled={loading}>
            <Text style={styles.buttonText}>3. 获取统计</Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={[styles.button, styles.buttonDanger]}
            onPress={handleClear}
            disabled={loading}>
            <Text style={styles.buttonText}>4. 清空 SHM</Text>
          </TouchableOpacity>
        </View>

        {/* 加载指示器 */}
        {loading && (
          <View style={styles.loadingContainer}>
            <ActivityIndicator size="large" color="#007AFF" />
            <Text style={styles.loadingText}>处理中...</Text>
          </View>
        )}

        {/* 错误信息 */}
        {error && (
          <View style={styles.errorContainer}>
            <Text style={styles.errorText}>{error}</Text>
          </View>
        )}

        {/* SHM Key */}
        {shmKey && (
          <View style={styles.infoContainer}>
            <Text style={styles.infoLabel}>SHM Key:</Text>
            <Text style={styles.infoValue}>{shmKey}</Text>
          </View>
        )}

        {/* 数据展示 */}
        {data && (
          <View style={styles.dataContainer}>
            <Text style={styles.dataTitle}>读取的数据:</Text>
            <Text style={styles.dataText}>
              {JSON.stringify(data, null, 2)}
            </Text>
          </View>
        )}

        {/* 统计信息 */}
        {stats && (
          <View style={styles.statsContainer}>
            <Text style={styles.statsTitle}>SHM 统计:</Text>
            <Text style={styles.statsText}>
              节点使用: {stats.nodesUsed} / {stats.nodes}
            </Text>
            <Text style={styles.statsText}>
              内存使用: {stats.payloadUsed} / {stats.payloadCapacity} bytes
            </Text>
            <Text style={styles.statsText}>
              使用率: {((stats.payloadUsed / stats.payloadCapacity) * 100).toFixed(2)}%
            </Text>
          </View>
        )}
      </ScrollView>
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#F5F5F5',
  },
  scrollView: {
    flex: 1,
    padding: 20,
  },
  title: {
    fontSize: 24,
    fontWeight: 'bold',
    marginBottom: 20,
    textAlign: 'center',
  },
  buttonContainer: {
    gap: 10,
    marginBottom: 20,
  },
  button: {
    backgroundColor: '#007AFF',
    padding: 15,
    borderRadius: 8,
    alignItems: 'center',
  },
  buttonDisabled: {
    backgroundColor: '#CCCCCC',
  },
  buttonDanger: {
    backgroundColor: '#FF3B30',
  },
  buttonText: {
    color: '#FFFFFF',
    fontSize: 16,
    fontWeight: '600',
  },
  loadingContainer: {
    alignItems: 'center',
    padding: 20,
  },
  loadingText: {
    marginTop: 10,
    color: '#666',
  },
  errorContainer: {
    backgroundColor: '#FFEBEE',
    padding: 15,
    borderRadius: 8,
    marginBottom: 20,
  },
  errorText: {
    color: '#D32F2F',
  },
  infoContainer: {
    backgroundColor: '#E3F2FD',
    padding: 15,
    borderRadius: 8,
    marginBottom: 20,
  },
  infoLabel: {
    fontSize: 14,
    color: '#666',
    marginBottom: 5,
  },
  infoValue: {
    fontSize: 12,
    color: '#1976D2',
    fontFamily: 'monospace',
  },
  dataContainer: {
    backgroundColor: '#FFFFFF',
    padding: 15,
    borderRadius: 8,
    marginBottom: 20,
  },
  dataTitle: {
    fontSize: 18,
    fontWeight: 'bold',
    marginBottom: 10,
  },
  dataText: {
    fontSize: 12,
    fontFamily: 'monospace',
    color: '#333',
  },
  statsContainer: {
    backgroundColor: '#FFFFFF',
    padding: 15,
    borderRadius: 8,
    marginBottom: 20,
  },
  statsTitle: {
    fontSize: 18,
    fontWeight: 'bold',
    marginBottom: 10,
  },
  statsText: {
    fontSize: 14,
    color: '#333',
    marginBottom: 5,
  },
});
