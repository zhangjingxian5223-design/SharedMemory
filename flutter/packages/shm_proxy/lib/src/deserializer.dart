import 'dart:typed_data';
import 'dart:convert';

/// Binary deserializer from shared memory format to Dart objects
/// Compatible with RN's NSDictionaryToShm implementation
class ShmDeserializer {
  // Type markers
  static const int TYPE_NULL = 0;
  static const int TYPE_BOOL = 1;
  static const int TYPE_INT = 2;
  static const int TYPE_DOUBLE = 3;
  static const int TYPE_STRING = 4;
  static const int TYPE_LIST = 5;
  static const int TYPE_MAP = 6;

  /// Deserialize binary data to Dart object
  static dynamic deserialize(Uint8List data) {
    final reader = _ByteReader(data);
    return _readValue(reader);
  }

  static dynamic _readValue(_ByteReader reader) {
    if (reader.remainingBytes == 0) {
      throw FormatException('No data to read');
    }

    final type = reader.readByte();

    switch (type) {
      case TYPE_NULL:
        return null;
      case TYPE_BOOL:
        return reader.readByte() == 1;
      case TYPE_INT:
        return reader.readInt64();
      case TYPE_DOUBLE:
        return reader.readFloat64();
      case TYPE_STRING:
        return reader.readString();
      case TYPE_LIST:
        return _readList(reader);
      case TYPE_MAP:
        return _readMap(reader);
      default:
        throw FormatException('Unknown type marker: $type');
    }
  }

  static List _readList(_ByteReader reader) {
    final length = reader.readUint32();
    final list = [];

    for (int i = 0; i < length; i++) {
      final itemLength = reader.readUint32();
      final itemData = reader.readBytes(itemLength);
      final itemReader = _ByteReader(itemData);
      list.add(_readValue(itemReader));
    }

    return list;
  }

  static Map _readMap(_ByteReader reader) {
    final length = reader.readUint32();
    final map = <String, dynamic>{};

    for (int i = 0; i < length; i++) {
      // Read key
      final keyLength = reader.readUint32();
      final keyBytes = reader.readBytes(keyLength);
      final key = utf8.decode(keyBytes);

      // Read value
      final valueLength = reader.readUint32();
      final valueData = reader.readBytes(valueLength);
      final value = deserialize(valueData);

      map[key] = value;
    }

    return map;
  }
}

/// Helper class for reading binary data
class _ByteReader {
  final Uint8List _data;
  int _pos = 0;

  _ByteReader(this._data);

  int get position => _pos;
  int get remainingBytes => _data.length - _pos;

  bool get hasMore => _pos < _data.length;

  int readByte() {
    if (_pos >= _data.length) {
      throw FormatException('Unexpected end of data');
    }
    return _data[_pos++];
  }

  int readInt64() {
    if (_pos + 8 > _data.length) {
      throw FormatException('Unexpected end of data');
    }
    final value = ByteData.sublistView(_data, _pos, _pos + 8)
        .getInt64(0, Endian.little);
    _pos += 8;
    return value;
  }

  double readFloat64() {
    if (_pos + 8 > _data.length) {
      throw FormatException('Unexpected end of data');
    }
    final value = ByteData.sublistView(_data, _pos, _pos + 8)
        .getFloat64(0, Endian.little);
    _pos += 8;
    return value;
  }

  int readUint32() {
    if (_pos + 4 > _data.length) {
      throw FormatException('Unexpected end of data');
    }
    final value = ByteData.sublistView(_data, _pos, _pos + 4)
        .getUint32(0, Endian.little);
    _pos += 4;
    return value;
  }

  String readString() {
    final length = readUint32();
    if (_pos + length > _data.length) {
      throw FormatException('Unexpected end of data');
    }
    final str = utf8.decode(_data.sublist(_pos, _pos + length));
    _pos += length;
    return str;
  }

  Uint8List readBytes(int length) {
    if (_pos + length > _data.length) {
      throw FormatException('Unexpected end of data');
    }
    final bytes = _data.sublist(_pos, _pos + length);
    _pos += length;
    return bytes;
  }
}
