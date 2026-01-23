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
import type {
  ShmStats,
  ShmWriteResult,
  ShmReadOptions,
  ShmProxyConfig,
  ShmErrorCode,
} from './types';

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
  static async write(data: Record<string, any>): Promise<string> {
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
  static async read(
    key: string,
    options?: ShmReadOptions
  ): Promise<Record<string, any>> {
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
  static async getStats(): Promise<ShmStats> {
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
  static async clear(): Promise<void> {
    await ShmProxyNative.clearShm();
  }

  /**
   * Check if SHM has been initialized
   *
   * @returns A promise that resolves with true if initialized
   */
  static async isInitialized(): Promise<boolean> {
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
  static async initialize(config?: ShmProxyConfig): Promise<void> {
    await ShmProxyNative.initialize(config);
  }
}

/**
 * JSI-exposed synchronous functions (advanced usage)
 *
 * These functions are available globally after the native module is loaded.
 * They provide synchronous access to shared memory for maximum performance.
 *
 * @example
 * ```typescript
 * declare global {
 *   var __shm_write: (obj: any) => string;
 *   var __shm_read: (key: string) => any;
 * }
 *
 * // Synchronous write
 * const key = __shm_write({ data: 'test' });
 *
 * // Synchronous read
 * const data = __shm_read(key);
 * ```
 */
export interface ShmProxyJSI {
  /**
   * Synchronously write object to shared memory
   * @param obj - The object to write
   * @returns The SHM key
   */
  __shm_write(obj: any): string;

  /**
   * Synchronously read object from shared memory
   * @param key - The SHM key
   * @returns The JavaScript object
   */
  __shm_read(key: string): any;
}

// Export types
export * from './types';
export type { ShmStats, ShmWriteResult, ShmReadOptions, ShmProxyConfig, ShmErrorCode };
