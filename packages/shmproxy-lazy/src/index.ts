/**
 * ShmProxyLazy - Main Module Entry Point
 *
 * Lazy-loading data transfer using ES6 Proxy and Shared Memory.
 *
 * @example
 * ```typescript
 * import { ShmProxyLazy } from 'react-native-shmproxy-lazy';
 *
 * // Write data to shared memory
 * const key = await ShmProxyLazy.write({ song: { title: 'Hello' } });
 *
 * // Create lazy proxy (instant, no data conversion)
 * const data = ShmProxyLazy.createProxy(key);
 *
 * // Access fields (only converts accessed fields)
 * console.log(data.song.title);
 * ```
 */

export { ShmProxyLazy, createShmProxyLazy } from './ShmProxyLazy';
export * from './types';

// Re-export for convenience
export { ShmProxyLazy as default } from './ShmProxyLazy';
