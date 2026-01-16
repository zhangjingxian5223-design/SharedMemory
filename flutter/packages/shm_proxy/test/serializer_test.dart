import 'package:flutter_test/flutter_test.dart';
import 'package:shm_flutter_new/src/serializer.dart';
import 'package:shm_flutter_new/src/deserializer.dart';

void main() {
  group('Serializer Tests', () {
    test('Serialize and deserialize null', () {
      final serialized = ShmSerializer.serialize(null);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, isNull);
    });

    test('Serialize and deserialize bool', () {
      final serialized = ShmSerializer.serialize(true);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, isTrue);

      final serialized2 = ShmSerializer.serialize(false);
      final deserialized2 = ShmDeserializer.deserialize(serialized2);
      expect(deserialized2, isFalse);
    });

    test('Serialize and deserialize int', () {
      final value = 42;
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize large int', () {
      final value = 9007199254740991; // 2^53 - 1
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize double', () {
      final value = 3.14159;
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, closeTo(value, 0.00001));
    });

    test('Serialize and deserialize string', () {
      final value = 'Hello, Flutter!';
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize Chinese string', () {
      final value = '你好，世界！';
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize emoji string', () {
      final value = '😀🎉🚀';
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize list', () {
      final value = [1, 2, 3, 4, 5];
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize mixed list', () {
      final value = [1, 'hello', 3.14, true, null];
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, orderedEquals(value));
    });

    test('Serialize and deserialize empty list', () {
      final value = <dynamic>[];
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize map', () {
      final value = {
        'name': 'Flutter',
        'version': 3.0,
        'stable': true,
        'year': 2025,
      };
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize nested map', () {
      final value = {
        'user': {
          'name': 'Alice',
          'age': 30,
          'address': {
            'city': 'Beijing',
            'country': 'China',
          },
        },
      };
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize complex structure', () {
      final value = {
        'title': 'Test Data',
        'count': 100,
        'items': [
          {'id': 1, 'name': 'Item 1', 'active': true},
          {'id': 2, 'name': 'Item 2', 'active': false},
        ],
        'metadata': {
          'created': '2025-01-15',
          'tags': ['flutter', 'shm', 'benchmark'],
        },
      };
      final serialized = ShmSerializer.serialize(value);
      final deserialized = ShmDeserializer.deserialize(serialized);
      expect(deserialized, equals(value));
    });

    test('Serialize and deserialize roundtrip preserves types', () {
      final original = {
        'nullValue': null,
        'boolValue': true,
        'intValue': 42,
        'doubleValue': 3.14,
        'stringValue': 'test',
        'listValue': [1, 2, 3],
        'mapValue': {'nested': 'data'},
      };

      final serialized = ShmSerializer.serialize(original);
      final deserialized = ShmDeserializer.deserialize(serialized);

      expect(deserialized['nullValue'], isNull);
      expect(deserialized['boolValue'], isTrue);
      expect(deserialized['intValue'], 42);
      expect(deserialized['doubleValue'], closeTo(3.14, 0.00001));
      expect(deserialized['stringValue'], 'test');
      expect(deserialized['listValue'], [1, 2, 3]);
      expect(deserialized['mapValue'], {'nested': 'data'});
    });

    test('Serialized binary size is reasonable', () {
      final value = {
        'message': 'Hello',
        'count': 100,
      };

      final serialized = ShmSerializer.serialize(value);

      // Should be less than 100 bytes for this simple structure
      expect(serialized.length, lessThan(100));
    });
  });
}
