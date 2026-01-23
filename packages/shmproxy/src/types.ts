/**
 * ShmProxy - TypeScript Type Definitions
 *
 * High-performance data transfer using Shared Memory for React Native
 */

/**
 * Shared memory statistics
 */
export interface ShmStats {
  /** Number of hash buckets in SHM */
  buckets: number;
  /** Total number of nodes available */
  nodes: number;
  /** Number of nodes currently in use */
  nodesUsed: number;
  /** Total payload capacity in bytes */
  payloadCapacity: number;
  /** Payload bytes currently used */
  payloadUsed: number;
  /** Generation number (incremented on clear) */
  generation: number;
}

/**
 * Result of writing data to shared memory
 */
export interface ShmWriteResult {
  /** Unique key identifying the data in SHM */
  shmKey: string;
  /** Time taken for conversion in milliseconds */
  convertTime: number;
}

/**
 * Options for reading from shared memory
 */
export interface ShmReadOptions {
  /** Whether to delete the data after reading */
  consume?: boolean;
}

/**
 * ShmProxy configuration options
 */
export interface ShmProxyConfig {
  /** Size of shared memory payload in bytes (default: 64MB) */
  shmSize?: number;
  /** Number of hash buckets (default: 4096) */
  buckets?: number;
  /** Number of nodes (default: 65536) */
  nodes?: number;
}

/**
 * Data type in shared memory
 */
export enum ShmValueType {
  NULL = 'null',
  INT_SCALAR = 'int_scalar',
  FLOAT_SCALAR = 'float_scalar',
  BOOL_SCALAR = 'bool_scalar',
  STRING = 'string',
  INT_VECTOR = 'int_vector',
  FLOAT_VECTOR = 'float_vector',
  BOOL_VECTOR = 'bool_vector',
  STRING_VECTOR = 'string_vector',
  OBJECT = 'object',
  LIST = 'list',
}

/**
 * Typed value view in shared memory
 */
export interface ShmTypedValueView {
  /** Type of the value */
  type: ShmValueType;
  /** Pointer to the payload data */
  payload: Uint8Array;
  /** Length of the payload in bytes */
  payloadLen: number;
}

/**
 * Object view in shared memory
 */
export interface ShmObjectView {
  /** Number of fields in the object */
  count: number;
  /** Offsets for field names */
  nameOffsets: Uint32Array;
  /** Field names data */
  namesData: string;
  /** Field types */
  fieldTypes: Uint8Array;
  /** Value offsets */
  valueOffsets: Uint32Array;
  /** Values data */
  valuesData: Uint8Array;
}

/**
 * Error codes for shared memory operations
 */
export enum ShmErrorCode {
  OK = 0,
  ERR_NO_SPACE = 2,
  ERR_NOT_FOUND = 3,
  ERR_TYPE_MISMATCH = 4,
  ERR_KEY_EXISTS = 5,
  ERR_INVALID_HANDLE = 1,
}

/**
 * Error thrown by shared memory operations
 */
export class ShmError extends Error {
  constructor(
    public code: ShmErrorCode,
    message: string
  ) {
    super(message);
    this.name = 'ShmError';
  }
}
