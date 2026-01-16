import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'shm_flutter_new_platform_interface.dart';

/// An implementation of [ShmFlutterNewPlatform] that uses method channels.
class MethodChannelShmFlutterNew extends ShmFlutterNewPlatform {
  /// The method channel used to interact with the native platform.
  @visibleForTesting
  final methodChannel = const MethodChannel('shm_flutter_new');

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>('getPlatformVersion');
    return version;
  }
}
