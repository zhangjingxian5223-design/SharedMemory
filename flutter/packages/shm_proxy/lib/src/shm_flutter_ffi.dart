import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

/// C function signatures for shm_flutter C API
class ShmFlutterFFI {
  late DynamicLibrary _lib;

  // Function pointers
  late _ShmCreateFunc _create;
  late _ShmCloseFunc _close;
  late _ShmWriteFunc _write;
  late _ShmReadFunc _read;
  late _ShmDeleteFunc _delete;
  late _ShmClearFunc _clear;

  ShmFlutterFFI() {
    try {
      // Try to load the dynamic library
      if (Platform.isIOS || Platform.isMacOS) {
        _lib = DynamicLibrary.process();
      } else if (Platform.isAndroid) {
        _lib = DynamicLibrary.open('libshm_flutter.so');
      } else {
        _lib = DynamicLibrary.process();
      }

      // Look up functions
      _create = _lib
          .lookup<NativeFunction<_ShmCreateNative>>('shm_flutter_create')
          .asFunction();
      _close = _lib
          .lookup<NativeFunction<_ShmCloseNative>>('shm_flutter_close')
          .asFunction();
      _write = _lib
          .lookup<NativeFunction<_ShmWriteNative>>('shm_flutter_write')
          .asFunction();
      _read = _lib
          .lookup<NativeFunction<_ShmReadNative>>('shm_flutter_read')
          .asFunction();
      _delete = _lib
          .lookup<NativeFunction<_ShmDeleteNative>>('shm_flutter_delete')
          .asFunction();
      _clear = _lib
          .lookup<NativeFunction<_ShmClearNative>>('shm_flutter_clear')
          .asFunction();
    } catch (e) {
      throw Exception('Failed to load shm_flutter library: $e');
    }
  }

  /// Create shared memory
  /// Returns pointer to shared memory handle
  Pointer<Void> createShm(String name, int size) {
    final nameC = name.toNativeUtf8();
    try {
      final handle = _create(nameC, size);
      if (handle == nullptr) {
        throw Exception('Failed to create shared memory');
      }
      return handle;
    } finally {
      calloc.free(nameC);
    }
  }

  /// Close shared memory
  void closeShm(Pointer<Void> handle) {
    if (handle != nullptr) {
      _close(handle);
    }
  }

  /// Write data to shared memory
  /// Returns true on success
  bool writeShm(Pointer<Void> handle, String key, Uint8List data) {
    final keyC = key.toNativeUtf8();
    final dataPtr = calloc.allocate<Uint8>(data.length);

    try {
      // Copy data to native memory
      for (int i = 0; i < data.length; i++) {
        dataPtr[i] = data[i];
      }

      // Call C function
      final result = _write(handle, keyC, keyC.length, dataPtr, data.length);

      // 0 = SHM_OK (success)
      return result == 0;
    } finally {
      calloc.free(keyC);
      calloc.free(dataPtr);
    }
  }

  /// Read data from shared memory
  /// Returns Uint8List or null on failure
  Uint8List? readShm(Pointer<Void> handle, String key, int maxSize) {
    final keyC = key.toNativeUtf8();
    final buffer = calloc.allocate<Uint8>(maxSize);
    final actualSize = calloc.allocate<Uint32>(1);

    try {
      // Call C function
      final result = _read(handle, keyC, keyC.length, buffer, maxSize, actualSize);

      // 0 = SHM_OK (success)
      if (result == 0 && actualSize.value > 0) {
        // Copy data to Dart Uint8List
        final data = Uint8List(actualSize.value);
        for (int i = 0; i < actualSize.value; i++) {
          data[i] = buffer[i];
        }
        return data;
      }

      return null;
    } finally {
      calloc.free(keyC);
      calloc.free(buffer);
      calloc.free(actualSize);
    }
  }

  /// Delete data from shared memory
  bool deleteShm(Pointer<Void> handle, String key) {
    final keyC = key.toNativeUtf8();
    try {
      final result = _delete(handle, keyC, keyC.length);
      return result == 0;
    } finally {
      calloc.free(keyC);
    }
  }

  /// Clear all data
  void clearShm(Pointer<Void> handle) {
    _clear(handle);
  }
}

// Native function types
typedef _ShmCreateNative = Pointer<Void> Function(
    Pointer<Utf8> name, IntPtr size);
typedef _ShmCreateFunc = Pointer<Void> Function(Pointer<Utf8> name, int size);

typedef _ShmCloseNative = Void Function(Pointer<Void> handle);
typedef _ShmCloseFunc = void Function(Pointer<Void> handle);

typedef _ShmWriteNative = Int32 Function(
    Pointer<Void> handle, Pointer<Utf8> key, IntPtr keyLen,
    Pointer<Uint8> data, IntPtr dataLen);
typedef _ShmWriteFunc = int Function(
    Pointer<Void> handle, Pointer<Utf8> key, int keyLen,
    Pointer<Uint8> data, int dataLen);

typedef _ShmReadNative = Int32 Function(
    Pointer<Void> handle, Pointer<Utf8> key, IntPtr keyLen,
    Pointer<Uint8> buffer, IntPtr bufferSize, Pointer<Uint32> actualSize);
typedef _ShmReadFunc = int Function(
    Pointer<Void> handle, Pointer<Utf8> key, int keyLen,
    Pointer<Uint8> buffer, int bufferSize, Pointer<Uint32> actualSize);

typedef _ShmDeleteNative = Int32 Function(
    Pointer<Void> handle, Pointer<Utf8> key, IntPtr keyLen);
typedef _ShmDeleteFunc = int Function(
    Pointer<Void> handle, Pointer<Utf8> key, int keyLen);

typedef _ShmClearNative = Void Function(Pointer<Void> handle);
typedef _ShmClearFunc = void Function(Pointer<Void> handle);
