/**
 * ShmProxyModule.h
 *
 * React Native TurboModule that provides shared memory proxy functionality.
 * This module allows JS to request data that is lazily loaded from shared memory.
 */

#pragma once

#import <React/RCTBridgeModule.h>
#import <ReactCommon/CallInvoker.h>
#import <jsi/jsi.h>
#include "shm_kv_c_api.h"

@interface ShmProxyModule : NSObject <RCTBridgeModule>

/**
 * Install the JSI bindings into the runtime.
 * This must be called during bridge initialization.
 */
+ (void)installWithRuntime:(facebook::jsi::Runtime&)runtime
             callInvoker:(std::shared_ptr<facebook::react::CallInvoker>)callInvoker;

@end
