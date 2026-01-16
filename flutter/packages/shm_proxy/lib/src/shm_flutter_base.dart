import 'dart:ffi';
import 'dart:typed_data';
import 'shm_flutter_ffi.dart';
import 'serializer.dart';
import 'deserializer.dart';

/// ShmProxy - 高性能 Native-Dart 数据传递类
class ShmProxy {
  final ShmFlutterFFI _ffi;
  Pointer<Void>? _shmHandle;
  bool _initialized = false;

  ShmProxy() : _ffi = ShmFlutterFFI();

  /// 初始化共享内存
  Future<void> initialize() async {
    if (_initialized) return;

    _shmHandle = _ffi.createShm('flutter_shm', 64 * 1024 * 1024); // 64MB
    _initialized = true;
  }

  /// 写入数据到共享内存
  /// 返回: 数据的 key
  String write(dynamic data) {
    if (!_initialized) {
      throw StateError('ShmProxy not initialized. Call initialize() first.');
    }

    final key = 'key_${DateTime.now().millisecondsSinceEpoch}';
    final binary = ShmSerializer.serialize(data);
    _ffi.writeShm(_shmHandle!, key, binary);
    return key;
  }

  /// 从共享内存读取数据
  dynamic read(String key) {
    if (!_initialized) {
      throw StateError('ShmProxy not initialized. Call initialize() first.');
    }

    final binary = _ffi.readShm(_shmHandle!, key, 10 * 1024 * 1024);
    if (binary == null) return null;
    return ShmDeserializer.deserialize(binary);
  }

  /// 释放资源
  void dispose() {
    if (_shmHandle != null) {
      _ffi.closeShm(_shmHandle!);
      _shmHandle = null;
      _initialized = false;
    }
  }
}
