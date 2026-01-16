//
//  BenchmarkModule.m
//  shm_flutter_new
//
//  Created for Flutter ShmProxy Benchmark
//

#import "BenchmarkModule.h"
#import "NSDictionaryToShm.h"
#import "shm_flutter_c_api.h"
#import <mach/mach_time.h>
#import <mutex>

// Global shared memory handle
static shm_handle_t g_benchmarkShmHandle = nullptr;
static std::atomic<uint64_t> g_benchmarkKeyCounter{0};

// Preloaded test data
static NSMutableDictionary<NSString*, NSDictionary*> *g_preloadedData = nil;
static std::mutex g_preloadMutex;

@interface BenchmarkModule ()
@property (nonatomic, assign) BOOL shmInitialized;
@property (nonatomic, assign) BOOL dataPreloaded;
@end

@implementation BenchmarkModule

+ (instancetype)sharedInstance {
    static BenchmarkModule *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[BenchmarkModule alloc] init];
    });
    return instance;
}

- (instancetype)init {
    if (self = [super init]) {
        _shmInitialized = NO;
        _dataPreloaded = NO;
        [self initializeShm];
    }
    return self;
}

- (BOOL)initializeShm {
    if (g_benchmarkShmHandle == nullptr) {
        g_benchmarkShmHandle = shm_create(
            "/benchmark_shm_flutter",
            4096,              // n_buckets
            65536,             // n_nodes
            64 * 1024 * 1024   // 64MB payload
        );

        if (g_benchmarkShmHandle != nullptr) {
            _shmInitialized = YES;
            NSLog(@"[BenchmarkModule] Shared memory initialized");
            return YES;
        } else {
            NSLog(@"[BenchmarkModule] Failed to initialize shared memory");
            return NO;
        }
    }
    _shmInitialized = YES;
    return YES;
}

- (NSString *)filenameForSize:(NSString *)dataSize {
    NSDictionary *mapping = @{
        @"128KB": @"test_data_128KB_205",
        @"256KB": @"test_data_256KB_420",
        @"512KB": @"test_data_512KB_845",
        @"1MB": @"test_data_1MB_1693",
        @"2MB": @"test_data_2MB_3390",
        @"5MB": @"test_data_5MB_8493",
        @"10MB": @"test_data_10MB_16986",
        @"20MB": @"test_data_20MB_33973"
    };
    return mapping[dataSize];
}

- (NSDictionary *)loadTestData:(NSString *)filename error:(NSError **)error {
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *path = [bundle pathForResource:filename ofType:@"json"];

    if (!path) {
        if (error) {
            *error = [NSError errorWithDomain:@"BenchmarkModule"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"File not found: %@", filename]}];
        }
        return nil;
    }

    NSData *data = [NSData dataWithContentsOfFile:path options:0 error:error];
    if (!data) {
        return nil;
    }

    return [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
}

- (BOOL)preloadTestData {
    std::lock_guard<std::mutex> lock(g_preloadMutex);

    if (g_preloadedData == nil) {
        g_preloadedData = [[NSMutableDictionary alloc] init];

        NSArray *sizes = @[@"128KB", @"256KB", @"512KB", @"1MB", @"2MB", @"5MB", @"10MB", @"20MB"];

        for (NSString *size in sizes) {
            NSString *filename = [self filenameForSize:size];
            NSError *error = nil;
            NSDictionary *data = [self loadTestData:filename error:&error];

            if (data) {
                g_preloadedData[size] = data;
                NSLog(@"[BenchmarkModule] Preloaded %@: %@", size, filename);
            } else {
                NSLog(@"[BenchmarkModule] Failed to load %@: %@", filename, error.localizedDescription);
                return NO;
            }
        }

        _dataPreloaded = YES;
    }

    return YES;
}

- (NSDictionary *)getDataTraditional:(NSString *)dataSize {
    NSDictionary *data = g_preloadedData[dataSize];
    if (!data) {
        NSLog(@"[BenchmarkModule] Data not preloaded for size: %@", dataSize);
        return @{};
    }

    // Return the NSDictionary directly (will be converted by Flutter)
    return data;
}

- (NSString *)getDataJsonString:(NSString *)dataSize {
    NSDictionary *data = g_preloadedData[dataSize];
    if (!data) {
        NSLog(@"[BenchmarkModule] Data not preloaded for size: %@", dataSize);
        return @"";
    }

    // Convert to JSON string
    NSError *jsonError = nil;
    NSData *jsonData = [NSJSONSerialization dataWithJSONObject:data
                                                       options:0
                                                         error:&jsonError];
    if (jsonError) {
        NSLog(@"[BenchmarkModule] JSON conversion error: %@", jsonError.localizedDescription);
        return @"";
    }

    return [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
}

- (NSString *)prepareShmProxy:(NSString *)dataSize {
    NSDictionary *data = g_preloadedData[dataSize];
    if (!data) {
        NSLog(@"[BenchmarkModule] Data not preloaded for size: %@", dataSize);
        return @"";
    }

    // Generate unique key
    uint64_t keyCounter = g_benchmarkKeyCounter.fetch_add(1);
    NSString *shmKey = [NSString stringWithFormat:@"shm_%llu", keyCounter];

    // Convert NSDictionary to shared memory
    const char *keyStr = [shmKey UTF8String];
    size_t keyLen = strlen(keyStr);

    shm_error_t result = convertNSDictionaryToShm(g_benchmarkShmHandle, keyStr, keyLen, data);

    if (result != SHM_OK) {
        NSLog(@"[BenchmarkModule] Failed to convert to shm for %@: error=%d", dataSize, result);
        return @"";
    }

    NSLog(@"[BenchmarkModule] Prepared ShmProxy for %@: key=%@", dataSize, shmKey);
    return shmKey;
}

- (NSDictionary *)getShmProxyData:(NSString *)shmKey error:(NSError **)error {
    // This will be implemented using Dart FFI
    // For now, return nil
    if (error) {
        *error = [NSError errorWithDomain:@"BenchmarkModule"
                                     code:4
                                 userInfo:@{NSLocalizedDescriptionKey: @"Not implemented yet"}];
    }
    return nil;
}

- (double)getTimeNanos {
    static mach_timebase_info_data_t info;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        mach_timebase_info(&info);
    });
    uint64_t time = mach_absolute_time();
    return (double)time * info.numer / info.denom;
}

- (void)dealloc {
    if (g_benchmarkShmHandle != nullptr) {
        shm_close(g_benchmarkShmHandle);
        g_benchmarkShmHandle = nullptr;
    }
}

@end
