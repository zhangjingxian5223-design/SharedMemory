import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'shm_flutter_new_method_channel.dart';

abstract class ShmFlutterNewPlatform extends PlatformInterface {
  /// Constructs a ShmFlutterNewPlatform.
  ShmFlutterNewPlatform() : super(token: _token);

  static final Object _token = Object();

  static ShmFlutterNewPlatform _instance = MethodChannelShmFlutterNew();

  /// The default instance of [ShmFlutterNewPlatform] to use.
  ///
  /// Defaults to [MethodChannelShmFlutterNew].
  static ShmFlutterNewPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [ShmFlutterNewPlatform] when
  /// they register themselves.
  static set instance(ShmFlutterNewPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }
}
