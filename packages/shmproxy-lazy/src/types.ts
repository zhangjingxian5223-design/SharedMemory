/**
 * ShmProxyLazy - TypeScript Type Definitions
 *
 * Lazy-loading data transfer using ES6 Proxy and Shared Memory
 */

import type { ShmStats, ShmErrorCode } from 'react-native-shmproxy';

// Re-export common types
export type { ShmStats, ShmErrorCode };

/**
 * Result of creating a lazy proxy
 */
export interface ShmProxyLazyResult<T = any> {
  /** The proxy object (lazy-loaded) */
  proxy: T;
  /** The SHM key for accessing the data */
  shmKey: string;
}

/**
 * Marker object returned by __shmProxyLazy_getField for nested objects
 */
export interface ShmProxyNestedMarker {
  /** Special marker property */
  __shmProxyNested: boolean;
  /** The path to this nested object */
  __shmProxyPath: string;
  /** Type of the nested object */
  __shmProxyType: 'object' | 'array';
}

/**
 * Options for creating a lazy proxy
 */
export interface CreateProxyOptions {
  /** Base path for nested proxies (internal use) */
  basePath?: string;
  /** Enable field caching (default: true) */
  cache?: boolean;
}

/**
 * Result of accessing a field through the proxy
 */
export interface FieldAccessResult {
  /** The value of the field */
  value: any;
  /** Whether this is a nested object/array */
  isNested: boolean;
  /** The path to this field */
  path: string;
}

/**
 * Statistics for proxy access patterns
 */
export interface ProxyAccessStats {
  /** Number of fields accessed */
  fieldsAccessed: number;
  /** Number of cache hits */
  cacheHits: number;
  /** Number of cache misses */
  cacheMisses: number;
  /** Total access time in milliseconds */
  totalAccessTime: number;
}

/**
 * Configuration for lazy proxy behavior
 */
export interface ShmProxyLazyConfig {
  /** Maximum cache size (number of fields) */
  maxCacheSize?: number;
  /** Enable access statistics */
  enableStats?: boolean;
  /** Custom field path separator (default: '.') */
  pathSeparator?: string;
}

/**
 * Materialization result
 */
export interface MaterializeResult<T = any> {
  /** The fully materialized JavaScript object */
  data: T;
  /** Time taken for materialization in milliseconds */
  time: number;
}
