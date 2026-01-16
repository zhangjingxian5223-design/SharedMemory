
import 'shm_flutter_new_platform_interface.dart';

class ShmFlutterNew {
  Future<String?> getPlatformVersion() {
    return ShmFlutterNewPlatform.instance.getPlatformVersion();
  }
}
