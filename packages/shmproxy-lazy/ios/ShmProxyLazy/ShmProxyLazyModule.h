/**
 * ShmProxyLazyModule.h
 *
 * React Native TurboModule for Lazy Loading implementation using ES6 Proxy.
 *
 * This module provides:
 * 1. Fine-grained access APIs for lazy loading
 * 2. JSI bindings for Proxy handler to call
 * 3. Separate from ShmProxyModule to avoid breaking existing implementation
 */

#ifndef ShmProxyLazyModule_h
#define ShmProxyLazyModule_h

#import <React/RCTBridgeModule.h>

@interface ShmProxyLazyModule : NSObject <RCTBridgeModule>

@end

#endif /* ShmProxyLazyModule_h */
