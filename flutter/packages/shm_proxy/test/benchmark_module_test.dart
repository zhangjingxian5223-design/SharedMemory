import 'package:flutter_test/flutter_test.dart';
import 'package:shm_flutter_new/src/benchmark_module.dart';

void main() {
  group('BenchmarkModule Tests', () {
    late BenchmarkModule benchmark;

    setUpAll(() async {
      benchmark = BenchmarkModule();
      await benchmark.initializeShm();
    });

    test('Initialize shared memory', () async {
      final initialized = await benchmark.initializeShm();
      expect(initialized, isTrue);
    });

    test('Get time in nanoseconds', () async {
      final time1 = await benchmark.getTimeNanos();
      expect(time1, greaterThan(0));

      // Wait a bit
      await Future.delayed(const Duration(milliseconds: 10));

      final time2 = await benchmark.getTimeNanos();
      expect(time2, greaterThan(time1));
    });

    test('Preload test data', () async {
      final success = await benchmark.preloadTestData();
      expect(success, isTrue);
    });

    test('Get data using Traditional method', () async {
      await benchmark.preloadTestData();

      final data = await benchmark.getDataTraditional('128KB');
      expect(data, isNotNull);
      expect(data, isMap);

      // Check basic structure
      expect(data.containsKey('song'), isTrue);
      expect(data['song'], isMap);
    }, skip: 'Requires test data files in iOS bundle');

    test('Get data using JSON String method', () async {
      await benchmark.preloadTestData();

      final jsonString = await benchmark.getDataJsonString('128KB');
      expect(jsonString, isNotNull);
      expect(jsonString, isNotEmpty);

      // Should be valid JSON
      final decoded = jsonDecode(jsonString) as Map;
      expect(decoded, isMap);
    }, skip: 'Requires test data files in iOS bundle');

    test('Prepare ShmProxy data', () async {
      await benchmark.preloadTestData();

      final shmKey = await benchmark.prepareShmProxy('128KB');
      expect(shmKey, isNotNull);
      expect(shmKey, isNotEmpty);
      expect(shmKey.startsWith('shm_'), isTrue);
    }, skip: 'Requires test data files in iOS bundle');

    test('Performance: Traditional vs JSON String', () async {
      await benchmark.preloadTestData();

      // Measure Traditional
      final start1 = await benchmark.getTimeNanos();
      await benchmark.getDataTraditional('128KB');
      final end1 = await benchmark.getTimeNanos();
      final traditionalTime = (end1 - start1) / 1000000; // Convert to ms

      print('Traditional method: ${traditionalTime.toStringAsFixed(2)} ms');

      // Measure JSON String
      final start2 = await benchmark.getTimeNanos();
      await benchmark.getDataJsonString('128KB');
      final end2 = await benchmark.getTimeNanos();
      final jsonStringTime = (end2 - start2) / 1000000; // Convert to ms

      print('JSON String method: ${jsonStringTime.toStringAsFixed(2)} ms');

      expect(traditionalTime, greaterThan(0));
      expect(jsonStringTime, greaterThan(0));
    }, skip: 'Requires test data files in iOS bundle');
  });
}
