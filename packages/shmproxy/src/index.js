/**
 * ShmProxy - Main Module Interface
 *
 * High-performance data transfer using Shared Memory for React Native.
 *
 * @example
 * ```typescript
 * import { ShmProxy } from 'react-native-shmproxy';
 *
 * // Write data to shared memory
 * const key = await ShmProxy.write({ song: { title: 'Hello' } });
 *
 * // Read data (fully converted)
 * const data = await ShmProxy.read(key);
 *
 * // Get statistics
 * const stats = await ShmProxy.getStats();
 * ```
 */

import { NativeModules } from 'react-native';

// Link to native module
const LINKING_ERROR =
  `The package 'react-native-shmproxy' doesn't seem to be linked. Make sure: \n\n` +
  `You've run 'pod install'\n` +
  `You rebuilt the app after installing the package\n` +
  'You are not using Expo Go\n';

const ShmProxyNative = NativeModules.ShmProxy
  ? NativeModules.ShmProxy
  : new Proxy(
      {},
      {
        get() {
          throw new Error(LINKING_ERROR);
        },
      }
    );

/**
 * ShmProxy - High-performance data transfer using Shared Memory
 *
 * This module provides faster data transfer between Native and JavaScript
 * by using shared memory instead of the traditional bridge serialization.
 *
 * Performance improvements:
 * - 1MB data: ~57% faster than traditional method
 * - 20MB data: ~57% faster than traditional method
 */
export class ShmProxy {
  /**
   * Write JavaScript object to shared memory
   *
   * @param data - The object to write. Can contain nested objects, arrays, and primitives.
   * @returns A promise that resolves with the SHM key
   *
   * @example
   * ```typescript
   * const key = await ShmProxy.write({
   *   song: {
   *     title: 'Hello',
   *     year: 2024,
   *     segments: [{ pitches: [440, 880] }]
   *   }
   * });
   * ```
   */
  static async write(data) {
    return await ShmProxyNative.writeData(data);
  }

  /**
   * Read object from shared memory with full conversion
   *
   * This method reads all data from shared memory and converts it to a
   * plain JavaScript object. Suitable for cases where you need to access
   * most or all fields.
   *
   * @param key - The SHM key returned by write()
   * @param options - Optional reading options
   * @returns A promise that resolves with the JavaScript object
   *
   * @example
   * ```typescript
   * const key = await ShmProxy.write(largeObject);
   * const data = await ShmProxy.read(key);
   * console.log(data.song.title);
   * ```
   */
  static async read(key, options) {
    if (options?.consume) {
      return await ShmProxyNative.readAndConsume(key);
    }
    return await ShmProxyNative.readData(key);
  }

  /**
   * Get shared memory statistics
   *
   * Useful for monitoring memory usage and debugging.
   *
   * @returns A promise that resolves with SHM statistics
   *
   * @example
   * ```typescript
   * const stats = await ShmProxy.getStats();
   * console.log(`Memory used: ${stats.payloadUsed}/${stats.payloadCapacity} bytes`);
   * ```
   */
  static async getStats() {
    return await ShmProxyNative.getStats();
  }

  /**
   * Clear all data from shared memory
   *
   * This frees up memory and increments the generation counter.
   * Existing keys become invalid after calling this method.
   *
   * @returns A promise that resolves when cleared
   *
   * @example
   * ```typescript
   * await ShmProxy.clear();
   * console.log('Shared memory cleared');
   * ```
   */
  static async clear() {
    await ShmProxyNative.clearShm();
  }

  /**
   * Check if SHM has been initialized
   *
   * @returns A promise that resolves with true if initialized
   */
  static async isInitialized() {
    return await ShmProxyNative.isInitialized();
  }

  /**
   * Initialize shared memory with custom configuration
   *
   * Note: This is typically called automatically on first use.
   * Only call this if you need custom SHM size.
   *
   * @param config - SHM configuration options
   * @returns A promise that resolves when initialized
   *
   * @example
   * ```typescript
   * await ShmProxy.initialize({ shmSize: 128 * 1024 * 1024 }); // 128MB
   * ```
   */
  static async initialize(config) {
    await ShmProxyNative.initialize(config);
  }

  /**
   * Install JSI bindings synchronously
   *
   * This method installs the synchronous JSI functions (__shm_write, __shm_read)
   * which can be used for maximum performance.
   *
   * Should be called on app startup.
   *
   * @example
   * ```typescript
   * import { ShmProxy } from 'react-native-shmproxy';
   *
   * // In your App.tsx
   * useEffect(() => {
   *   ShmProxy.installJSIBindingsSync();
   * }, []);
   * ```
   */
  static installJSIBindingsSync() {
    if (typeof ShmProxyNative.installJSIBindingsSync !== 'function') {
      throw new Error(LINKING_ERROR);
    }
    return ShmProxyNative.installJSIBindingsSync();
  }
}
