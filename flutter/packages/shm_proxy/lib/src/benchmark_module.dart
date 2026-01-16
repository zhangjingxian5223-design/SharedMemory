import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// BenchmarkModule - Dart API for benchmarking Traditional vs ShmProxy
///
/// This class provides methods to:
/// 1. Preload test data (JSON files from bundle)
/// 2. Get data using Traditional method (NSDictionary → Dart)
/// 3. Get data using JSON String method (NSDictionary → JSON String → Dart)
/// 4. Get data using ShmProxy (NSDictionary → Shared Memory → Dart FFI)
class BenchmarkModule {
  static const MethodChannel _channel = MethodChannel('shm_flutter_new');

  /// Initialize shared memory
  Future<bool> initializeShm() async {
    final result = await _channel.invokeMethod('initializeShm');
    return result as bool? ?? false;
  }

  /// Preload all test data from bundle
  /// Returns true if successful
  Future<bool> preloadTestData() async {
    final result = await _channel.invokeMethod('preloadTestData');
    if (result is bool) return result;
    if (result is FlutterError) throw Exception(result.message);
    return false;
  }

  /// Get data using Traditional method (NSDictionary → Dart)
  /// This is the baseline for comparison
  Future<Map<String, dynamic>> getDataTraditional(String dataSize) async {
    final result = await _channel.invokeMethod('getDataTraditional', {
      'dataSize': dataSize,
    });
    if (result is Map) {
      return Map<String, dynamic>.from(result);
    }
    if (result is FlutterError) throw Exception(result.message);
    throw Exception('Unknown error');
  }

  /// Get data using JSON String method (NSDictionary → JSON String → Dart)
  Future<String> getDataJsonString(String dataSize) async {
    final result = await _channel.invokeMethod('getDataJsonString', {
      'dataSize': dataSize,
    });
    if (result is String) return result as String;
    if (result is FlutterError) throw Exception(result.message);
    throw Exception('Unknown error');
  }

  /// Prepare ShmProxy data (NSDictionary → Shared Memory)
  /// Returns a unique key that can be used to access the data
  Future<String> prepareShmProxy(String dataSize) async {
    final result = await _channel.invokeMethod('prepareShmProxy', {
      'dataSize': dataSize,
    });
    if (result is String) return result as String;
    if (result is FlutterError) throw Exception(result.message);
    throw Exception('Unknown error');
  }

  /// Get high-precision time in nanoseconds
  Future<double> getTimeNanos() async {
    final result = await _channel.invokeMethod('getTimeNanos');
    return result as double? ?? 0.0;
  }
}
