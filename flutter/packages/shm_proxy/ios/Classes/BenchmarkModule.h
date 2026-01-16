//
//  BenchmarkModule.h
//  shm_flutter_new
//
//  Created for Flutter ShmProxy Benchmark
//

#import <Foundation/Foundation.h>

@interface BenchmarkModule : NSObject

/// Shared instance
+ (instancetype)sharedInstance;

/// Initialize shared memory for benchmarking
- (BOOL)initializeShm;

/// Preload test data from bundle
- (BOOL)preloadTestData;

/// Get traditional method data (NSDictionary → JSON)
- (NSDictionary *)getDataTraditional:(NSString *)dataSize;

/// Get JSON string method data (NSDictionary → JSON String)
- (NSString *)getDataJsonString:(NSString *)dataSize;

/// Prepare ShmProxy data (NSDictionary → Shared Memory)
- (NSString *)prepareShmProxy:(NSString *)dataSize;

/// Get ShmProxy data from shared memory key
- (NSDictionary *)getShmProxyData:(NSString *)shmKey error:(NSError **)error;

/// High-precision timer (nanoseconds)
- (double)getTimeNanos;

@end
