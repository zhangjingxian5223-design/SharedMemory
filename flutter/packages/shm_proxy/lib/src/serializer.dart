import 'dart:typed_data';
import 'dart:convert';

/// Binary serializer for Dart objects to shared memory format
/// Compatible with RN's NSDictionaryToShm implementation
class ShmSerializer {
  /// Serialize any Dart object to binary format
  static Uint8List serialize(dynamic obj) {
    final builder = BytesBuilder();

    if (obj == null) {
      _writeNull(builder);
    } else if (obj is bool) {
      _writeBool(builder, obj);
    } else if (obj is int) {
      _writeInt(builder, obj);
    } else if (obj is double) {
      _writeDouble(builder, obj);
    } else if (obj is String) {
      _writeString(builder, obj);
    } else if (obj is List) {
      _writeList(builder, obj);
    } else if (obj is Map) {
      _writeMap(builder, obj);
    } else {
      throw UnsupportedError('Unsupported type: ${obj.runtimeType}');
    }

    return builder.toBytes();
  }

  // Type markers
  static const int TYPE_NULL = 0;
  static const int TYPE_BOOL = 1;
  static const int TYPE_INT = 2;
  static const int TYPE_DOUBLE = 3;
  static const int TYPE_STRING = 4;
  static const int TYPE_LIST = 5;
  static const int TYPE_MAP = 6;

  static void _writeNull(BytesBuilder builder) {
    builder.addByte(TYPE_NULL);
  }

  static void _writeBool(BytesBuilder builder, bool value) {
    builder.addByte(TYPE_BOOL);
    builder.addByte(value ? 1 : 0);
  }

  static void _writeInt(BytesBuilder builder, int value) {
    builder.addByte(TYPE_INT);
    final bytes = ByteData(8)..setInt64(0, value, Endian.little);
    builder.add(bytes.buffer.asUint8List());
  }

  static void _writeDouble(BytesBuilder builder, double value) {
    builder.addByte(TYPE_DOUBLE);
    final bytes = ByteData(8)..setFloat64(0, value, Endian.little);
    builder.add(bytes.buffer.asUint8List());
  }

  static void _writeString(BytesBuilder builder, String value) {
    builder.addByte(TYPE_STRING);
    final utf8Bytes = utf8.encode(value);
    final length = utf8Bytes.length;

    // Write string length (4 bytes)
    final lenBytes = ByteData(4)..setUint32(0, length, Endian.little);
    builder.add(lenBytes.buffer.asUint8List());

    // Write string data
    builder.add(utf8Bytes);
  }

  static void _writeList(BytesBuilder builder, List list) {
    builder.addByte(TYPE_LIST);
    final length = list.length;

    // Write list length (4 bytes)
    final lenBytes = ByteData(4)..setUint32(0, length, Endian.little);
    builder.add(lenBytes.buffer.asUint8List());

    // Write each element
    for (final item in list) {
      final itemBytes = serialize(item);
      // Write item length (4 bytes)
      final itemLenBytes = ByteData(4)..setUint32(0, itemBytes.length, Endian.little);
      builder.add(itemLenBytes.buffer.asUint8List());
      // Write item data
      builder.add(itemBytes);
    }
  }

  static void _writeMap(BytesBuilder builder, Map map) {
    builder.addByte(TYPE_MAP);
    final length = map.length;

    // Write map length (4 bytes)
    final lenBytes = ByteData(4)..setUint32(0, length, Endian.little);
    builder.add(lenBytes.buffer.asUint8List());

    // Write each key-value pair
    map.forEach((key, value) {
      // Key must be string
      if (key is! String) {
        throw ArgumentError('Map keys must be strings');
      }

      // Write key
      final keyBytes = utf8.encode(key);
      final keyLenBytes = ByteData(4)..setUint32(0, keyBytes.length, Endian.little);
      builder.add(keyLenBytes.buffer.asUint8List());
      builder.add(keyBytes);

      // Write value
      final valueBytes = serialize(value);
      final valueLenBytes = ByteData(4)..setUint32(0, valueBytes.length, Endian.little);
      builder.add(valueLenBytes.buffer.asUint8List());
      builder.add(valueBytes);
    });
  }
}
