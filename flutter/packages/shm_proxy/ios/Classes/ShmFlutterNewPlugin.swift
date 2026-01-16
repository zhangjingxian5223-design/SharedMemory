import Flutter
import UIKit

public class ShmFlutterNewPlugin: NSObject, FlutterPlugin {
  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "shm_flutter_new", binaryMessenger: registrar.messenger())
    let instance = ShmFlutterNewPlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    guard let benchmark = BenchmarkModule.sharedInstance() else {
      result(FlutterError(code: "INIT_ERROR",
                         message: "Failed to get BenchmarkModule instance",
                         details: nil))
      return
    }

    switch call.method {
    case "getPlatformVersion":
      result("iOS " + UIDevice.current.systemVersion)

    case "initializeShm":
      let success = benchmark.initializeShm()
      result(success)

    case "preloadTestData":
      let success = benchmark.preloadTestData()
      result(success)

    case "getDataTraditional":
      guard let args = call.arguments as? [String: Any],
            let dataSize = args["dataSize"] as? String else {
        result(FlutterError(code: "INVALID_ARGS",
                           message: "Missing dataSize argument",
                           details: nil))
        return
      }

      let data = benchmark.getDataTraditional(dataSize)
      result(data)

    case "getDataJsonString":
      guard let args = call.arguments as? [String: Any],
            let dataSize = args["dataSize"] as? String else {
        result(FlutterError(code: "INVALID_ARGS",
                           message: "Missing dataSize argument",
                           details: nil))
        return
      }

      let jsonString = benchmark.getDataJsonString(dataSize)
      result(jsonString)

    case "prepareShmProxy":
      guard let args = call.arguments as? [String: Any],
            let dataSize = args["dataSize"] as? String else {
        result(FlutterError(code: "INVALID_ARGS",
                           message: "Missing dataSize argument",
                           details: nil))
        return
      }

      let shmKey = benchmark.prepareShmProxy(dataSize)
      result(shmKey)

    case "getTimeNanos":
      let time = benchmark.getTimeNanos()
      result(time)

    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
