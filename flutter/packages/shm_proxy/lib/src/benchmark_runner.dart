import 'dart:async';
import 'dart:convert';
import 'benchmark_module.dart';
import 'shm_flutter_base.dart';

/// Benchmark runner for comparing Traditional vs JSON String vs ShmProxy
class BenchmarkRunner {
  final BenchmarkModule _benchmark = BenchmarkModule();
  final ShmProxy _shmProxy = ShmProxy();

  /// Data sizes to test
  static const List<String> dataSizes = [
    '128KB',
    '256KB',
    '512KB',
    '1MB',
    '2MB',
    '5MB',
    '10MB',
    '20MB',
  ];

  /// Access types
  static const List<String> accessTypes = ['partial', 'full'];

  /// Initialize benchmark
  Future<void> initialize() async {
    await _benchmark.initializeShm();
    await _benchmark.preloadTestData();
    await _shmProxy.initialize();
  }

  /// Run Traditional benchmark
  Future<BenchmarkResult> runTraditional({
    required String dataSize,
    required String accessType,
  }) async {
    // Measure conversion time
    final startConvert = await _benchmark.getTimeNanos();
    final data = await _benchmark.getDataTraditional(dataSize);
    final endConvert = await _benchmark.getTimeNanos();

    final convertTimeMs = (endConvert - startConvert) / 1000000;

    // Measure access time
    final startAccess = await _benchmark.getTimeNanos();
    final accessData = _accessData(data, accessType);
    final endAccess = await _benchmark.getTimeNanos();

    final accessTimeMs = (endAccess - startAccess) / 1000000;

    return BenchmarkResult(
      dataSize: dataSize,
      method: 'Traditional',
      accessType: accessType,
      convertMs: convertTimeMs,
      accessMs: accessTimeMs,
      e2eMs: convertTimeMs + accessTimeMs,
    );
  }

  /// Run JSON String benchmark
  Future<BenchmarkResult> runJsonString({
    required String dataSize,
    required String accessType,
  }) async {
    // Measure conversion time
    final startConvert = await _benchmark.getTimeNanos();
    final jsonString = await _benchmark.getDataJsonString(dataSize);
    final endConvert = await _benchmark.getTimeNanos();

    final convertTimeMs = (endConvert - startConvert) / 1000000;

    // Measure access time
    final startAccess = await _benchmark.getTimeNanos();
    final accessData = _accessJsonString(jsonString, accessType);
    final endAccess = await _benchmark.getTimeNanos();

    final accessTimeMs = (endAccess - startAccess) / 1000000;

    return BenchmarkResult(
      dataSize: dataSize,
      method: 'JSON String',
      accessType: accessType,
      convertMs: convertTimeMs,
      accessMs: accessTimeMs,
      e2eMs: convertTimeMs + accessTimeMs,
    );
  }

  /// Run ShmProxy benchmark
  Future<BenchmarkResult> runShmProxy({
    required String dataSize,
    required String accessType,
  }) async {
    // Prepare ShmProxy
    final startConvert = await _benchmark.getTimeNanos();
    final shmKey = await _benchmark.prepareShmProxy(dataSize);
    final endConvert = await _benchmark.getTimeNanos();

    final convertTimeMs = (endConvert - startConvert) / 1000000;

    // Measure access time
    final startAccess = await _benchmark.getTimeNanos();
    // TODO: Implement actual FFI-based access
    // final accessData = await _shmProxy.readFromShm(shmKey);
    final endAccess = await _benchmark.getTimeNanos();

    final accessTimeMs = (endAccess - startAccess) / 1000000;

    return BenchmarkResult(
      dataSize: dataSize,
      method: 'ShmProxy',
      accessType: accessType,
      convertMs: convertTimeMs,
      accessMs: accessTimeMs,
      e2eMs: convertTimeMs + accessTimeMs,
    );
  }

  /// Run all benchmarks
  Future<List<BenchmarkResult>> runAllBenchmarks() async {
    final results = <BenchmarkResult>[];

    for (final dataSize in dataSizes) {
      for (final accessType in accessTypes) {
        print('Running: $dataSize - $accessType');

        // Traditional
        results.add(await runTraditional(
          dataSize: dataSize,
          accessType: accessType,
        ));

        // JSON String
        results.add(await runJsonString(
          dataSize: dataSize,
          accessType: accessType,
        ));

        // ShmProxy
        results.add(await runShmProxy(
          dataSize: dataSize,
          accessType: accessType,
        ));

        // Small delay between tests
        await Future.delayed(const Duration(milliseconds: 100));
      }
    }

    return results;
  }

  /// Access data (partial)
  dynamic _accessData(Map<String, dynamic> data, String accessType) {
    if (accessType == 'partial') {
      // Access only 4 fields from actual data structure
      try {
        final basicTypes = data['basic_types'] as Map<String, dynamic>?;
        final nested = data['nested_object'] as Map<String, dynamic>?;

        return {
          'string_field': basicTypes?['string_field'],
          'int_field': basicTypes?['int_field'],
          'float_field': basicTypes?['float_field'],
          'deep_string': nested?['level1']?['level2']?['level3']?['deep_string'],
        };
      } catch (e) {
        // If data structure is unexpected, return simple access
        return {
          'metadata': data['metadata'],
          'has_basic_types': data['basic_types'] != null,
          'has_nested': data['nested_object'] != null,
        };
      }
    } else {
      // Full access - simulate JSON.stringify
      return data.toString();
    }
  }

  /// Access JSON string data
  dynamic _accessJsonString(String jsonString, String accessType) {
    if (accessType == 'partial') {
      // Parse and access 4 fields from actual data structure
      try {
        final data = jsonDecode(jsonString) as Map<String, dynamic>;
        final basicTypes = data['basic_types'] as Map<String, dynamic>?;
        final nested = data['nested_object'] as Map<String, dynamic>?;

        return {
          'string_field': basicTypes?['string_field'],
          'int_field': basicTypes?['int_field'],
          'float_field': basicTypes?['float_field'],
          'deep_string': nested?['level1']?['level2']?['level3']?['deep_string'],
        };
      } catch (e) {
        // If parsing fails, return simple access
        return {
          'parse_error': e.toString(),
          'json_length': jsonString.length,
        };
      }
    } else {
      // Full access
      return jsonString; // Already a string
    }
  }
}

/// Benchmark result data model
class BenchmarkResult {
  final String dataSize;
  final String method;
  final String accessType;
  final double convertMs;
  final double accessMs;
  final double e2eMs;

  BenchmarkResult({
    required this.dataSize,
    required this.method,
    required this.accessType,
    required this.convertMs,
    required this.accessMs,
    required this.e2eMs,
  });

  Map<String, dynamic> toJson() {
    return {
      'dataSize': dataSize,
      'method': method,
      'accessType': accessType,
      'convertMs': convertMs,
      'accessMs': accessMs,
      'e2eMs': e2eMs,
      'timestamp': DateTime.now().toIso8601String(),
    };
  }

  @override
  String toString() {
    return 'BenchmarkResult($dataSize, $method, $accessType, '
        'convert: ${convertMs.toStringAsFixed(2)}ms, '
        'access: ${accessMs.toStringAsFixed(2)}ms, '
        'e2e: ${e2eMs.toStringAsFixed(2)}ms)';
  }
}
