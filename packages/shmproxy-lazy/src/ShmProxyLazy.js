/**
 * ShmProxyLazy - ES6 Proxy Implementation for Lazy Loading
 *
 * This module provides lazy-loading data transfer using ES6 Proxy.
 * Data is only converted from shared memory when fields are actually accessed.
 *
 * @example
 * ```typescript
 * import { ShmProxyLazy } from 'react-native-shmproxy-lazy';
 *
 * // Write data
 * const key = await ShmProxyLazy.write({ song: { title: 'Hello' } });
 *
 * // Create lazy proxy (instant, no data conversion)
 * const data = await ShmProxyLazy.createProxy(key);
 *
 * // Access field (triggers conversion of only this field)
 * console.log(data.song.title);
 * ```
 */

import { NativeModules } from 'react-native';

// Link to native module
const LINKING_ERROR =
  `The package 'react-native-shmproxy-lazy' doesn't seem to be linked. Make sure: \n\n` +
  `You've run 'pod install'\n` +
  `You rebuilt the app after installing the package\n` +
  'You are not using Expo Go\n';

const ShmProxyLazyNative = NativeModules.ShmProxyLazy
  ? NativeModules.ShmProxyLazy
  : new Proxy(
      {},
      {
        get() {
          throw new Error(LINKING_ERROR);
        },
      }
    );

/**
 * Create an ES6 Proxy for lazy-loading data from shared memory
 *
 * @param key - The SHM key
 * @param options - Proxy creation options
 * @returns The proxy object
 *
 * @example
 * ```typescript
 * const proxy = createShmProxyLazy('shm_key_123');
 * console.log(proxy.song.title); // Only converts song.title
 * ```
 */
export function createShmProxyLazy(key, options = {}) {
  const { basePath = '', cache = true } = options;

  // Check if JSI bindings are available
  if (typeof global.__shmProxyLazy_getField !== 'function') {
    throw new Error(
      'ShmProxyLazy JSI bindings not installed. ' +
      'Make sure you have called ShmProxy.installJSIBindingsSync() first.'
    );
  }

  const fieldCache = {};
  let keysCache = [];

  /**
   * Parse a field path and get its value
   */
  const getFieldValue = (prop) => {
    const propStr = String(prop);
    const fullPath = basePath ? `${basePath}.${propStr}` : propStr;

    // Check cache first
    if (cache && propStr in fieldCache) {
      return fieldCache[propStr];
    }

    // Get value from SHM
    const value = global.__shmProxyLazy_getField(key, fullPath);

    // Handle nested objects/arrays
    if (value && typeof value === 'object' && value.__shmProxyNested) {
      const nestedProxy = createShmProxyLazy(key, {
        basePath: value.__shmProxyPath,
        cache,
      });

      if (cache) {
        fieldCache[propStr] = nestedProxy;
      }

      return nestedProxy;
    }

    // Cache and return primitive values
    if (cache) {
      fieldCache[propStr] = value;
    }

    return value;
  };

  /**
   * Get all property keys
   */
  const ownKeys = () => {
    if (keysCache.length === 0) {
      keysCache = global.__shmProxyLazy_getKeys(key);
    }
    return keysCache;
  };

  // Create the Proxy
  const handler = {
    get(target, prop, receiver) {
      // Handle special properties
      if (prop === 'then') {
        // Make the proxy thenable to avoid Promise coercion issues
        return undefined;
      }

      if (prop === 'toString') {
        return () => '[ShmProxyLazy]';
      }

      if (prop === Symbol.iterator || prop === Symbol.toPrimitive) {
        return undefined;
      }

      // Get the field value
      return getFieldValue(prop);
    },

    has(target, prop) {
      const keys = ownKeys();
      const propStr = String(prop);
      return keys.includes(propStr);
    },

    ownKeys(target) {
      return ownKeys();
    },

    getOwnPropertyDescriptor(target, prop) {
      const keys = ownKeys();
      const propStr = String(prop);

      if (keys.includes(propStr)) {
        return {
          enumerable: true,
          configurable: true,
        };
      }

      return undefined;
    },
  };

  return new Proxy({}, handler);
}

/**
 * ShmProxyLazy - Main API class
 */
export class ShmProxyLazy {
  /**
   * Write data to shared memory
   *
   * @param data - The object to write
   * @returns The SHM key
   */
  static async write(data) {
    return await ShmProxyLazyNative.writeData(data);
  }

  /**
   * Create a lazy proxy for accessing data
   *
   * This is extremely fast as it only creates a Proxy wrapper.
   * Data is not converted until fields are accessed.
   *
   * @param key - The SHM key
   * @param options - Proxy options
   * @returns The lazy proxy
   *
   * @example
   * ```typescript
   * const key = await ShmProxyLazy.write({ song: { title: 'Hello' } });
   * const proxy = ShmProxyLazy.createProxy(key);
   *
   * // At this point, no data has been converted yet
   * console.log(proxy.song.title); // Now it converts song.title
   * ```
   */
  static createProxy(key, options) {
    return createShmProxyLazy(key, options);
  }

  /**
   * Fully materialize (convert) an object from shared memory
   *
   * Use this when you need to access most or all fields.
   * This is faster than accessing fields individually via proxy.
   *
   * @param key - The SHM key
   * @returns The fully converted JavaScript object
   *
   * @example
   * ```typescript
   * const key = await ShmProxyLazy.write(largeObject);
   *
   * // For full access, materialize once
   * const data = await ShmProxyLazy.materialize(key);
   * console.log(Object.keys(data)); // All keys available
   * ```
   */
  static async materialize(key) {
    if (typeof global.__shmProxyLazy_materialize !== 'function') {
      throw new Error('ShmProxyLazy JSI bindings not installed');
    }

    return global.__shmProxyLazy_materialize(key);
  }

  /**
   * Get shared memory statistics
   *
   * @returns SHM statistics
   */
  static async getStats() {
    if (typeof global.__shmProxyLazy_getStats !== 'function') {
      throw new Error('ShmProxyLazy JSI bindings not installed');
    }

    return global.__shmProxyLazy_getStats();
  }

  /**
   * Clear all data from shared memory
   */
  static async clear() {
    await ShmProxyLazyNative.clear();
  }

  /**
   * Initialize the native module and install JSI bindings
   *
   * This is typically called automatically on first use.
   *
   * @example
   * ```typescript
   * await ShmProxyLazy.install();
   * ```
   */
  static async install() {
    const installed = ShmProxyLazyNative.installJSIBindingsSync();
    if (!installed) {
      throw new Error('Failed to install ShmProxyLazy JSI bindings');
    }
  }

  /**
   * Check if SHM has been initialized
   */
  static async isInitialized() {
    return await ShmProxyLazyNative.isInitialized();
  }

  /**
   * Initialize shared memory
   *
   * This must be called before any write operations.
   * Typically called automatically on first use, but can be called explicitly.
   *
   * @returns A promise that resolves when initialized
   *
   * @example
   * ```typescript
   * // Check if initialized
   * const isInit = await ShmProxyLazy.isInitialized();
   * if (!isInit) {
   *   await ShmProxyLazy.initialize();
   * }
   * ```
   */
  static async initialize() {
    return await ShmProxyLazyNative.initialize();
  }
}

export default ShmProxyLazy;
