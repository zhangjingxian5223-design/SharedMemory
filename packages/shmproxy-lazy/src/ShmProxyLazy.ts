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

import type {
  ShmProxyLazyResult,
  CreateProxyOptions,
  ShmProxyNestedMarker,
  MaterializeResult,
} from './types';

/**
 * Global JSI function declarations
 */
declare global {
  /**
   * Get a single field from shared memory
   * @param key - SHM key
   * @param fieldPath - Dot-separated path (e.g., "song.title")
   * @returns The field value, or a nested marker for objects/arrays
   */
  var __shmProxyLazy_getField: (key: string, fieldPath: string) => any;

  /**
   * Get all field names for an object
   * @param key - SHM key
   * @returns Array of field names
   */
  var __shmProxyLazy_getKeys: (key: string) => string[];

  /**
   * Fully materialize an object (convert all fields)
   * @param key - SHM key
   * @returns The fully converted JavaScript object
   */
  var __shmProxyLazy_materialize: (key: string) => any;

  /**
   * Get shared memory statistics
   * @returns SHM statistics object
   */
  var __shmProxyLazy_getStats: () => {
    buckets: number;
    nodes: number;
    nodesUsed: number;
    payloadCapacity: number;
    payloadUsed: number;
    generation: number;
  };
}

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
export function createShmProxyLazy<T = any>(
  key: string,
  options: CreateProxyOptions = {}
): T {
  const { basePath = '', cache = true } = options;

  // Check if JSI bindings are available
  if (typeof global.__shmProxyLazy_getField !== 'function') {
    throw new Error(
      'ShmProxyLazy JSI bindings not installed. ' +
      'Make sure you have called ShmProxyLazy.install() first.'
    );
  }

  const fieldCache: Record<string, any> = {};
  let keysCache: string[] = [];

  /**
   * Parse a field path and get its value
   */
  const getFieldValue = (prop: string | symbol): any => {
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
  const ownKeys = (): string[] => {
    if (keysCache.length === 0) {
      keysCache = global.__shmProxyLazy_getKeys(key);
    }
    return keysCache;
  };

  // Create the Proxy
  const handler: ProxyHandler<any> = {
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

  return new Proxy({}, handler) as T;
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
  static async write(data: Record<string, any>): Promise<string> {
    const ShmProxyLazyNative = (global as any).ShmProxyLazyNative;
    if (!ShmProxyLazyNative) {
      throw new Error('ShmProxyLazy native module not available');
    }

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
  static createProxy<T = any>(
    key: string,
    options?: CreateProxyOptions
  ): T {
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
  static async materialize<T = any>(key: string): Promise<T> {
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
    const ShmProxyLazyNative = (global as any).ShmProxyLazyNative;
    if (!ShmProxyLazyNative) {
      throw new Error('ShmProxyLazy native module not available');
    }

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
  static async install(): Promise<void> {
    const ShmProxyLazyNative = (global as any).ShmProxyLazyNative;
    if (!ShmProxyLazyNative) {
      throw new Error('ShmProxyLazy native module not available');
    }

    const installed = ShmProxyLazyNative.installJSIBindingsSync();
    if (!installed) {
      throw new Error('Failed to install ShmProxyLazy JSI bindings');
    }
  }

  /**
   * Check if SHM has been initialized
   */
  static async isInitialized(): Promise<boolean> {
    const ShmProxyLazyNative = (global as any).ShmProxyLazyNative;
    if (!ShmProxyLazyNative) {
      return false;
    }

    return await ShmProxyLazyNative.isInitialized();
  }
}

export default ShmProxyLazy;
