#include "shm_kv_c_api.h"
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/aes.h>

// Reuse structures from shm_kv.cpp
static constexpr uint32_t EMPTY_INDEX = 0xFFFFFFFFu;
static constexpr uint32_t MAGIC = 0x4C4D4252;
static constexpr size_t DEFAULT_N_BUCKETS = 1 << 12;
static constexpr size_t DEFAULT_N_NODES = 1 << 16;

// 安全限制常量
static constexpr size_t MAX_VAL_LEN = 1 << 28;
static constexpr uint32_t MAX_CAS_RETRIES = 10000;

inline size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

// ============================================================================
// Auto-cleanup mechanism for shared memory objects
// ============================================================================

// 全局状态：记录需要清理的共享内存名称
static char g_shm_name_to_cleanup[256] = {0};
static bool g_auto_cleanup_enabled = false;
static bool g_cleanup_registered = false;

// 清理函数：在进程退出时调用
static void cleanup_shared_memory() {
    if (g_auto_cleanup_enabled && g_shm_name_to_cleanup[0] != '\0') {
        shm_unlink(g_shm_name_to_cleanup);
        // 清空名称，防止重复清理
        g_shm_name_to_cleanup[0] = '\0';
    }
}

// 信号处理器：捕获信号后清理并退出
static void signal_handler(int signum) {
    cleanup_shared_memory();
    // 使用 _exit 而不是 exit，避免触发 atexit 导致重复清理
    _exit(128 + signum);
}

// 注册清理机制（仅第一次调用时执行）
static void register_cleanup(const char* name) {
    // 检查环境变量
    const char* auto_cleanup_env = getenv("SHM_AUTO_CLEANUP");
    if (!auto_cleanup_env || strcmp(auto_cleanup_env, "1") != 0) {
        return;  // 未启用自动清理
    }

    // 只注册一次
    if (g_cleanup_registered) {
        return;
    }

    // 保存共享内存名称
    strncpy(g_shm_name_to_cleanup, name, sizeof(g_shm_name_to_cleanup) - 1);
    g_shm_name_to_cleanup[sizeof(g_shm_name_to_cleanup) - 1] = '\0';
    g_auto_cleanup_enabled = true;

    // 注册 atexit 清理（正常退出时调用）
    atexit(cleanup_shared_memory);

    // 注册信号处理器（异常退出时调用）
    signal(SIGINT, signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);  // kill 命令
    signal(SIGHUP, signal_handler);   // 终端断开
    // 注意：SIGKILL 无法捕获，SIGSEGV 捕获可能不安全，所以不处理

    g_cleanup_registered = true;
}

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t total_size;
    uint64_t bucket_area_off;
    uint64_t node_area_off;
    uint64_t payload_area_off;
    uint32_t n_buckets;
    uint32_t n_nodes;
    uint32_t next_free_node_index;  // 注意：通过__atomic_*函数操作
    uint64_t payload_alloc_off;     // 注意：通过__atomic_*函数操作
    uint64_t generation;            // 注意：通过__atomic_*函数操作
    pthread_mutex_t writer_mutex;
    uint32_t checksum;

    // 安全字段
    uid_t owner_uid;
    gid_t owner_gid;
    uint32_t auth_pid_count;        // 注意：通过__atomic_*函数操作
    pid_t auth_pids[32];
    bool is_memfd;
    uint64_t create_time;
    bool marked_for_delete;         // 注意：通过__atomic_*函数操作

    uint8_t reserved[32];
};

struct Node {
    uint32_t key_off;
    uint32_t key_len;
    uint32_t val_off;
    uint32_t val_len;
    uint32_t next_index;
    uint32_t flags;
    uint64_t version;
    uint8_t value_type;    // Type of value (shm_value_type_t)
    uint8_t reserved[7];   // Padding for 64-byte alignment
};

struct SharedShm {
    int fd;
    void* base;
    Header* hdr;

    uint8_t* buckets() const {
        return reinterpret_cast<uint8_t*>((char*)base + hdr->bucket_area_off);
    }
    Node* node_array() const {
        return reinterpret_cast<Node*>((char*)base + hdr->node_area_off);
    }
    uint8_t* payload_base() const {
        return reinterpret_cast<uint8_t*>((char*)base + hdr->payload_area_off);
    }
};

bool atomic_cas_u32(uint32_t* addr, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(addr, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

uint64_t simple_hash(const uint8_t* data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i=0;i<len;++i) h ^= data[i], h *= 1099511628211ULL;
    return h;
}

uint32_t alloc_node_index(SharedShm* s) {
    uint32_t idx = __atomic_fetch_add(&s->hdr->next_free_node_index, 1u, __ATOMIC_SEQ_CST);
    if (idx >= s->hdr->n_nodes) return EMPTY_INDEX;
    return idx;
}

uint64_t alloc_payload(SharedShm* s, size_t len) {
    if (len == 0 || len > MAX_VAL_LEN) {
        return UINT64_MAX;
    }

    uint64_t payload_capacity = s->hdr->total_size - s->hdr->payload_area_off;
    uint64_t aligned_len = align_up(len, 8);

    // CAS loop: check-then-allocate to prevent memory leak
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint64_t current_off = __atomic_load_n(&s->hdr->payload_alloc_off, __ATOMIC_SEQ_CST);

        if (current_off + aligned_len > payload_capacity) {
            return UINT64_MAX;  // No space - no leak!
        }

        uint64_t new_off = current_off + aligned_len;
        if (__atomic_compare_exchange_n(&s->hdr->payload_alloc_off,
                                        &current_off, new_off,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return current_off;
        }
    }

    return UINT64_MAX;  // Max retries exceeded
}

static bool bytes_less(const char* a, size_t a_len, const char* b, size_t b_len) {
    const size_t n = std::min(a_len, b_len);
    int cmp = 0;
    if (n > 0) {
        cmp = memcmp(a, b, n);
    }
    if (cmp != 0) return cmp < 0;
    return a_len < b_len;
}

static bool bytes_equal(const char* a, size_t a_len, const char* b, size_t b_len) {
    return a_len == b_len && (a_len == 0 || memcmp(a, b, a_len) == 0);
}

// C API Implementation

extern "C" {

// If SHM_CREATE_LEGACY=1, keep the previous behavior (size derived from args even for existing shm).
// Default is 0: reopen uses the size stored in the shared memory header.
static bool shm_create_legacy_mode() {
    const char* v = std::getenv("SHM_CREATE_LEGACY");
    return v && (std::strcmp(v, "1") == 0 || std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0);
}

static size_t clamp_or_default(size_t v, size_t def) { return v == 0 ? def : v; }

static bool read_existing_total_size(int fd, size_t* out_total_size) {
    if (!out_total_size) return false;
    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    if (st.st_size < (off_t)sizeof(Header)) return false;

    const size_t map_len = align_up(sizeof(Header), 64);
    void* base = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) return false;
    const Header* hdr = reinterpret_cast<const Header*>(base);
    bool ok = (hdr->magic == MAGIC) && (hdr->version == 1) && (hdr->total_size >= map_len);
    if (ok) {
        *out_total_size = (size_t)hdr->total_size;
    }
    munmap(base, map_len);
    return ok;
}

shm_handle_t shm_create(const char* name, size_t n_buckets, size_t n_nodes, size_t payload_size) {
    if (!name) return nullptr;

    const size_t req_buckets = clamp_or_default(n_buckets, DEFAULT_N_BUCKETS);
    const size_t req_nodes = clamp_or_default(n_nodes, DEFAULT_N_NODES);
    const size_t req_payload = clamp_or_default(payload_size, (1 << 24));

    bool need_init = false;

    // Prefer O_EXCL to reliably detect creation.
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        if (errno == EEXIST) {
            fd = shm_open(name, O_RDWR, 0666);
        }
        if (fd < 0) {
            std::cerr << "shm_open failed name=" << name << " errno=" << errno << " (" << strerror(errno) << ")\n";
            return nullptr;
        }
    }

    // Compute requested new total_size (used only when creating/initializing).
    size_t header_size = align_up(sizeof(Header), 64);
    size_t buckets_size = align_up(sizeof(uint32_t) * req_buckets, 64);
    size_t nodes_size = align_up(sizeof(Node) * req_nodes, 64);
    size_t payload_area_size = align_up(req_payload, 4096);
    size_t requested_total_size = header_size + buckets_size + nodes_size + payload_area_size;

    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        return nullptr;
    }

    size_t map_total_size = 0;
    if (shm_create_legacy_mode()) {
        map_total_size = requested_total_size;
        if ((size_t)st.st_size < map_total_size) {
            if (ftruncate(fd, map_total_size) == -1) {
                close(fd);
                return nullptr;
            }
            need_init = true;
        }
    } else {
        // New behavior:
        // - If existing shm has a valid header, map using hdr->total_size (ignoring args)
        // - Otherwise, treat as new and initialize using requested sizes.
        if ((size_t)st.st_size >= header_size) {
            size_t existing_total = 0;
            if (read_existing_total_size(fd, &existing_total)) {
                map_total_size = existing_total;
            }
        }
        if (map_total_size == 0) {
            map_total_size = requested_total_size;
            if ((size_t)st.st_size < map_total_size) {
                if (ftruncate(fd, map_total_size) == -1) {
                    close(fd);
                    return nullptr;
                }
                need_init = true;
            }
        }
    }

    void* base = mmap(nullptr, map_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    Header* hdr = reinterpret_cast<Header*>((char*)base);
    if (need_init || hdr->magic != MAGIC) {
        memset(base, 0, map_total_size);
        hdr->magic = MAGIC;
        hdr->version = 1;
        hdr->flags = 0;
        hdr->total_size = map_total_size;
        hdr->bucket_area_off = header_size;
        hdr->node_area_off = header_size + buckets_size;
        hdr->payload_area_off = header_size + buckets_size + nodes_size;
        hdr->n_buckets = (uint32_t)req_buckets;
        hdr->n_nodes = (uint32_t)req_nodes;
        hdr->next_free_node_index = 0;
        hdr->payload_alloc_off = 0;
        hdr->generation = 0;
        hdr->checksum = 0;

        uint32_t* buckets_ptr = reinterpret_cast<uint32_t*>((char*)base + hdr->bucket_area_off);
        for (size_t i = 0; i < req_buckets; i++) buckets_ptr[i] = EMPTY_INDEX;

        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
#ifdef __linux__
        pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
        pthread_mutex_init(&hdr->writer_mutex, &mattr);
        pthread_mutexattr_destroy(&mattr);

        // 安全字段初始化
        hdr->owner_uid = getuid();
        hdr->owner_gid = getgid();
        __atomic_store_n(&hdr->auth_pid_count, 1, __ATOMIC_SEQ_CST);
        hdr->auth_pids[0] = getpid();
        hdr->is_memfd = false;
        hdr->create_time = time(NULL);
        __atomic_store_n(&hdr->marked_for_delete, false, __ATOMIC_SEQ_CST);
    }

    // 注册自动清理机制（如果环境变量启用）
    register_cleanup(name);

    SharedShm* s = new SharedShm{fd, base, hdr};
    return reinterpret_cast<shm_handle_t>(s);
}

shm_error_t shm_insert(shm_handle_t handle, const void* key, size_t key_len,
                       const void* value, size_t value_len) {
    if (!handle || !key || !value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    const uint8_t* v = reinterpret_cast<const uint8_t*>(value);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, value_len);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);  // Rollback generation
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);
    memcpy(payloadBase + val_payload_off, v, value_len);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);  // Rollback generation
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)value_len;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_UNKNOWN;  // Default: unknown type (backward compatible)

    nodes[node_idx] = tmp;

    // Finite CAS retry loop to prevent infinite loops
    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);  // Rollback generation
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup(shm_handle_t handle, const void* key, size_t key_len,
                       const void** out_value, size_t* out_value_len) {
    if (!handle || !key || !out_value || !out_value_len) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    uint64_t h = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(h % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint32_t idx = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
    Node* nodes = s->node_array();
    uint8_t* payloadBase = s->payload_base();

    while (idx != EMPTY_INDEX) {
        Node n = nodes[idx];
        if (n.flags & 1) {
            if (n.key_len == key_len) {
                if (memcmp(payloadBase + n.key_off, k, key_len) == 0) {
                    *out_value = payloadBase + n.val_off;
                    *out_value_len = n.val_len;

                    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
                    if (g1 == g2) {
                        return SHM_OK;
                    } else {
                        return SHM_ERR_CONCURRENT_MOD;
                    }
                }
            }
        }
        idx = n.next_index;
    }

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_ERR_NOT_FOUND;
}

shm_error_t shm_lookup_copy(shm_handle_t handle, const void* key, size_t key_len,
                            void* out_buffer, size_t buffer_size, size_t* out_value_len) {
    const void* value_ptr = nullptr;
    size_t value_len = 0;

    shm_error_t err = shm_lookup(handle, key, key_len, &value_ptr, &value_len);
    if (err != SHM_OK) return err;

    *out_value_len = value_len;

    if (buffer_size < value_len) {
        return SHM_ERR_NO_SPACE;
    }

    memcpy(out_buffer, value_ptr, value_len);
    return SHM_OK;
}

void shm_get_stats(shm_handle_t handle, shm_stats_t* stats) {
    if (!handle || !stats) return;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    stats->n_buckets = s->hdr->n_buckets;
    stats->n_nodes = s->hdr->n_nodes;
    stats->nodes_used = __atomic_load_n(&s->hdr->next_free_node_index, __ATOMIC_SEQ_CST);
    stats->payload_capacity = s->hdr->total_size - s->hdr->payload_area_off;
    stats->payload_used = __atomic_load_n(&s->hdr->payload_alloc_off, __ATOMIC_SEQ_CST);
    stats->generation = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
}

void shm_close(shm_handle_t handle) {
    if (!handle) return;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    munmap(s->base, s->hdr->total_size);
    close(s->fd);
    delete s;
}

void shm_destroy(const char* name) {
    if (name) {
        shm_unlink(name);
    }
}

// ============================================================================
// Security and Permission Management
// ============================================================================

shm_error_t shm_check_owner(shm_handle_t handle, int* is_owner) {
    if (!handle || !is_owner) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    *is_owner = (getuid() == s->hdr->owner_uid) ? 1 : 0;
    return SHM_OK;
}

shm_error_t shm_check_authorized(shm_handle_t handle, int* is_authorized) {
    if (!handle || !is_authorized) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);

    // Check if owner
    if (getuid() == s->hdr->owner_uid) {
        *is_authorized = 1;
        return SHM_OK;
    }

    // Check if in authorized PID list
    pid_t current_pid = getpid();
    uint32_t auth_count = __atomic_load_n(&s->hdr->auth_pid_count, __ATOMIC_SEQ_CST);

    for (uint32_t i = 0; i < auth_count && i < 32; i++) {
        if (s->hdr->auth_pids[i] == current_pid) {
            *is_authorized = 1;
            return SHM_OK;
        }
    }

    *is_authorized = 0;
    return SHM_OK;
}

shm_error_t shm_add_authorized_pid(shm_handle_t handle, int pid) {
    if (!handle) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);

    // Check if current process is owner
    if (getuid() != s->hdr->owner_uid) {
        return SHM_ERR_PERMISSION_DENIED;
    }

    // Check if PID already authorized
    uint32_t auth_count = __atomic_load_n(&s->hdr->auth_pid_count, __ATOMIC_SEQ_CST);
    for (uint32_t i = 0; i < auth_count && i < 32; i++) {
        if (s->hdr->auth_pids[i] == pid) {
            return SHM_OK; // Already authorized
        }
    }

    // Check if list is full
    if (auth_count >= 32) {
        return SHM_ERR_NO_SPACE;
    }

    // Add new PID
    s->hdr->auth_pids[auth_count] = pid;
    __atomic_fetch_add(&s->hdr->auth_pid_count, 1, __ATOMIC_SEQ_CST);

    return SHM_OK;
}

// ============================================================================
// Encryption Support (AES-128)
// ============================================================================

// AES-128 encryption helper
static bool aes_encrypt_data(const uint8_t* key, const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t* out_len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int len;
    if (EVP_EncryptUpdate(ctx, out, &len, in, in_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    *out_len = len;

    int final_len;
    if (EVP_EncryptFinal_ex(ctx, out + len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    *out_len += final_len;

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

// AES-128 decryption helper
static bool aes_decrypt_data(const uint8_t* key, const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t* out_len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key, nullptr) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }

    int len;
    if (EVP_DecryptUpdate(ctx, out, &len, in, in_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    *out_len = len;

    int final_len;
    if (EVP_DecryptFinal_ex(ctx, out + len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return false;
    }
    *out_len += final_len;

    EVP_CIPHER_CTX_free(ctx);
    return true;
}

shm_error_t shm_insert_encrypted(shm_handle_t handle,
                                  const void* key, size_t key_len,
                                  const void* value, size_t value_len,
                                  const uint8_t* aes_key) {
    if (!handle || !key || !value || !aes_key) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);

    // Check if current process is owner
    if (getuid() != s->hdr->owner_uid) {
        return SHM_ERR_PERMISSION_DENIED;
    }

    // Calculate encrypted length (16-byte aligned for AES)
    size_t encrypted_val_len = ((value_len + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;
    size_t total_payload_len = sizeof(uint32_t) + encrypted_val_len;

    uint8_t* total_payload = new uint8_t[total_payload_len];
    if (!total_payload) return SHM_ERR_NO_SPACE;

    // Write original length
    *reinterpret_cast<uint32_t*>(total_payload) = static_cast<uint32_t>(value_len);

    // Encrypt value
    size_t actual_encrypted_len = 0;
    if (!aes_encrypt_data(aes_key, reinterpret_cast<const uint8_t*>(value), value_len,
                          total_payload + sizeof(uint32_t), &actual_encrypted_len)) {
        delete[] total_payload;
        return SHM_ERR_OPEN_FAILED;
    }

    // Insert encrypted data
    shm_error_t result = shm_insert(handle, key, key_len, total_payload,
                                    sizeof(uint32_t) + actual_encrypted_len);

    delete[] total_payload;
    return result;
}

shm_error_t shm_lookup_decrypted(shm_handle_t handle,
                                  const void* key, size_t key_len,
                                  void* out_buffer, size_t buffer_size,
                                  size_t* out_value_len,
                                  const uint8_t* aes_key) {
    if (!handle || !key || !out_buffer || !out_value_len || !aes_key) {
        return SHM_ERR_INVALID_PARAM;
    }

    (void)buffer_size; // Unused parameter - reserved for future bounds checking

    // Check if current process is authorized
    int is_auth = 0;
    shm_error_t auth_result = shm_check_authorized(handle, &is_auth);
    if (auth_result != SHM_OK || !is_auth) {
        return SHM_ERR_PERMISSION_DENIED;
    }

    // Lookup encrypted data (use reasonable buffer size)
    const size_t TEMP_BUFFER_SIZE = 1024 * 1024;  // 1MB should be enough for most cases
    uint8_t* temp_buffer = new uint8_t[TEMP_BUFFER_SIZE];
    if (!temp_buffer) {
        return SHM_ERR_NO_SPACE;
    }
    size_t temp_len = TEMP_BUFFER_SIZE;

    shm_error_t lookup_result = shm_lookup_copy(handle, key, key_len,
                                                 temp_buffer, TEMP_BUFFER_SIZE, &temp_len);
    if (lookup_result != SHM_OK) {
        delete[] temp_buffer;
        return lookup_result;
    }

    // Validate data length
    if (temp_len < sizeof(uint32_t)) {
        delete[] temp_buffer;
        return SHM_ERR_INVALID_PARAM;
    }

    // Parse original length
    uint32_t original_val_len = *reinterpret_cast<uint32_t*>(temp_buffer);
    size_t encrypted_data_len = temp_len - sizeof(uint32_t);
    uint8_t* encrypted_data_ptr = temp_buffer + sizeof(uint32_t);

    // Decrypt
    size_t decrypted_len = 0;
    if (!aes_decrypt_data(aes_key, encrypted_data_ptr, encrypted_data_len,
                          reinterpret_cast<uint8_t*>(out_buffer), &decrypted_len)) {
        delete[] temp_buffer;
        return SHM_ERR_OPEN_FAILED;
    }

    // Adjust output length to original length
    *out_value_len = original_val_len;

    delete[] temp_buffer;
    return SHM_OK;
}

// ============================================================================
// Zero-Copy Typed Insert/Lookup (Performance Optimized)
// ============================================================================

shm_error_t shm_insert_int_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int64_t value) {
    if (!handle || !key) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload for key and value (8 bytes for int64_t)
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, 8);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    // Write int64_t directly (no encoding!)
    *reinterpret_cast<int64_t*>(payloadBase + val_payload_off) = value;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = 8;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_INT_SCALAR;  // Mark as int scalar

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_float_scalar(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     double value) {
    if (!handle || !key) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload for key and value (8 bytes for double)
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, 8);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    // Write double directly (no encoding!)
    *reinterpret_cast<double*>(payloadBase + val_payload_off) = value;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = 8;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_FLOAT_SCALAR;  // Mark as float scalar

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_bool_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int value) {
    if (!handle || !key) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    const uint8_t v = value ? 1u : 0u;

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload for key and value (1 byte)
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, 1);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);
    *(payloadBase + val_payload_off) = v;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = 1;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_BOOL_SCALAR;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_int_vector(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const int64_t* values,
                                   size_t count) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes for count + 8 * count bytes for data
    size_t val_size = 4 + count * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Write count (no type tag!)
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;
    // Write raw data (no encoding, direct memcpy!)
    memcpy(val_ptr + 4, values, count * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_INT_VECTOR;  // Mark as int vector

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_float_vector(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const double* values,
                                     size_t count) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes for count + 8 * count bytes for data
    size_t val_size = 4 + count * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Write count (no type tag!)
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;
    // Write raw data (no encoding, direct memcpy!)
    memcpy(val_ptr + 4, values, count * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_FLOAT_VECTOR;  // Mark as float vector

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

// Helper function to find node by key
static Node* find_node_by_key(SharedShm* s, const uint8_t* key, size_t key_len) {
    uint64_t h = simple_hash(key, key_len);
    uint32_t bucket = (uint32_t)(h % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint32_t idx = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
    Node* nodes = s->node_array();
    uint8_t* payloadBase = s->payload_base();

    while (idx != EMPTY_INDEX) {
        Node* n = &nodes[idx];
        if (n->flags & 1) {
            if (n->key_len == key_len) {
                if (memcmp(payloadBase + n->key_off, key, key_len) == 0) {
                    return n;
                }
            }
        }
        idx = n->next_index;
    }
    return nullptr;
}

shm_error_t shm_get_value_type(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                shm_value_type_t* out_type) {
    if (!handle || !key || !out_type) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    *out_type = static_cast<shm_value_type_t>(node->value_type);

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_int_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int64_t* out_value) {
    if (!handle || !key || !out_value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_INT_SCALAR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    // Zero-copy read: direct pointer dereference!
    const int64_t* ptr = reinterpret_cast<const int64_t*>(s->payload_base() + node->val_off);
    *out_value = *ptr;  // Just 8-byte read, no decode!

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_float_scalar(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     double* out_value) {
    if (!handle || !key || !out_value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_FLOAT_SCALAR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    // Zero-copy read: direct pointer dereference!
    const double* ptr = reinterpret_cast<const double*>(s->payload_base() + node->val_off);
    *out_value = *ptr;  // Just 8-byte read, no decode!

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_bool_scalar(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   int* out_value) {
    if (!handle || !key || !out_value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_BOOL_SCALAR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(s->payload_base() + node->val_off);
    *out_value = (*ptr != 0) ? 1 : 0;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_int_vector(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_int_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_INT_VECTOR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointer to data!
    const int64_t* data = reinterpret_cast<const int64_t*>(ptr + 4);

    out_view->data = data;   // Just pointer assignment!
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;  // Return time < 0.001ms!
}

shm_error_t shm_lookup_float_vector(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_float_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_FLOAT_VECTOR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointer to data!
    const double* data = reinterpret_cast<const double*>(ptr + 4);

    out_view->data = data;   // Just pointer assignment!
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;  // Return time < 0.001ms!
}

// ============================================================================
// String and Matrix Zero-Copy Functions
// ============================================================================

shm_error_t shm_insert_string(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               const char* value,
                               size_t value_len) {
    if (!handle || !key || !value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes for length + N bytes for data
    size_t val_size = 4 + value_len;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Write length (no type tag!)
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)value_len;
    // Write raw string data (no encoding!)
    memcpy(val_ptr + 4, value, value_len);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_STRING;  // Mark as string

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_bytes(shm_handle_t handle,
                              const void* key,
                              size_t key_len,
                              const uint8_t* value,
                              size_t value_len) {
    if (!handle || !key || !value) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes for length + N bytes for data
    size_t val_size = 4 + value_len;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)value_len;
    memcpy(val_ptr + 4, value, value_len);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_BYTES;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_int_matrix(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const int64_t* values,
                                   size_t rows,
                                   size_t cols) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes rows + 4 bytes cols + 8*rows*cols bytes for data
    size_t val_size = 8 + rows * cols * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Write dimensions (no type tag!)
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)rows;
    *reinterpret_cast<uint32_t*>(val_ptr + 4) = (uint32_t)cols;
    // Write raw matrix data (no encoding, direct memcpy!)
    memcpy(val_ptr + 8, values, rows * cols * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_INT_MATRIX;  // Mark as int matrix

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_float_matrix(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const double* values,
                                     size_t rows,
                                     size_t cols) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload: 4 bytes rows + 4 bytes cols + 8*rows*cols bytes for data
    size_t val_size = 8 + rows * cols * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Write dimensions (no type tag!)
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)rows;
    *reinterpret_cast<uint32_t*>(val_ptr + 4) = (uint32_t)cols;
    // Write raw matrix data (no encoding, direct memcpy!)
    memcpy(val_ptr + 8, values, rows * cols * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_FLOAT_MATRIX;  // Mark as float matrix

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_string(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               shm_string_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_STRING) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read length
    uint32_t length = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointer to data!
    const char* data = reinterpret_cast<const char*>(ptr + 4);

    out_view->data = data;     // Just pointer assignment!
    out_view->length = length;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_bytes(shm_handle_t handle,
                              const void* key,
                              size_t key_len,
                              shm_bytes_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_BYTES) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t length = *reinterpret_cast<const uint32_t*>(ptr);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ptr + 4);

    out_view->data = data;
    out_view->length = length;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_bool_vector(shm_handle_t handle,
                                    const void* key,
                                    size_t key_len,
                                    const uint8_t* values,
                                    size_t count) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Format: [count:4][values:count bytes]
    size_t val_size = 4 + count;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;
    memcpy(val_ptr + 4, values, count);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_BOOL_VECTOR;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_bool_vector(shm_handle_t handle,
                                    const void* key,
                                    size_t key_len,
                                    shm_bool_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_BOOL_VECTOR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint8_t* data = reinterpret_cast<const uint8_t*>(ptr + 4);

    out_view->data = data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_int_matrix(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_int_matrix_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_INT_MATRIX) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read dimensions
    uint32_t rows = *reinterpret_cast<const uint32_t*>(ptr);
    uint32_t cols = *reinterpret_cast<const uint32_t*>(ptr + 4);

    // Zero-copy: return pointer to data!
    const int64_t* data = reinterpret_cast<const int64_t*>(ptr + 8);

    out_view->data = data;  // Just pointer assignment!
    out_view->rows = rows;
    out_view->cols = cols;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_float_matrix(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_float_matrix_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_FLOAT_MATRIX) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read dimensions
    uint32_t rows = *reinterpret_cast<const uint32_t*>(ptr);
    uint32_t cols = *reinterpret_cast<const uint32_t*>(ptr + 4);

    // Zero-copy: return pointer to data!
    const double* data = reinterpret_cast<const double*>(ptr + 8);

    out_view->data = data;  // Just pointer assignment!
    out_view->rows = rows;
    out_view->cols = cols;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

// ============================================================================
// SET and DICT Zero-Copy Implementation
// ============================================================================

shm_error_t shm_insert_int_set(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                const int64_t* values,
                                size_t count) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Step 1: Sort and deduplicate
    std::vector<int64_t> sorted_unique(values, values + count);
    std::sort(sorted_unique.begin(), sorted_unique.end());
    auto last = std::unique(sorted_unique.begin(), sorted_unique.end());
    sorted_unique.erase(last, sorted_unique.end());

    size_t unique_count = sorted_unique.size();

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Step 2: Allocate payload: 4 bytes count + 8*unique_count bytes data
    size_t val_size = 4 + unique_count * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Step 3: Write format: [count][elem0][elem1]...[elemN-1]
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)unique_count;
    memcpy(val_ptr + 4, sorted_unique.data(), unique_count * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_INT_SET;  // Mark as int set

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_int_set(shm_handle_t handle,
                                const void* key,
                                size_t key_len,
                                shm_int_set_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_INT_SET) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointer to data
    const int64_t* data = reinterpret_cast<const int64_t*>(ptr + 4);

    out_view->data = data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_float_set(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  const double* values,
                                  size_t count) {
    if (!handle || !key || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Step 1: Sort and deduplicate
    std::vector<double> sorted_unique(values, values + count);
    std::sort(sorted_unique.begin(), sorted_unique.end());
    auto last = std::unique(sorted_unique.begin(), sorted_unique.end());
    sorted_unique.erase(last, sorted_unique.end());

    size_t unique_count = sorted_unique.size();

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Step 2: Allocate payload: 4 bytes count + 8*unique_count bytes data
    size_t val_size = 4 + unique_count * 8;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;
    // Step 3: Write format: [count][elem0][elem1]...[elemN-1]
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)unique_count;
    memcpy(val_ptr + 4, sorted_unique.data(), unique_count * 8);

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_FLOAT_SET;  // Mark as float set

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_float_set(shm_handle_t handle,
                                  const void* key,
                                  size_t key_len,
                                  shm_float_set_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_FLOAT_SET) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointer to data
    const double* data = reinterpret_cast<const double*>(ptr + 4);

    out_view->data = data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_string_set(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   const char** strings,
                                   const size_t* string_lengths,
                                   size_t count) {
    if (!handle || !key || !strings || !string_lengths) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Step 1: Create string references for sorting
    struct StringRef {
        const char* data;
        size_t len;
        bool operator<(const StringRef& other) const {
            int cmp = strncmp(data, other.data, std::min(len, other.len));
            if (cmp != 0) return cmp < 0;
            return len < other.len;
        }
        bool operator==(const StringRef& other) const {
            if (len != other.len) return false;
            return strncmp(data, other.data, len) == 0;
        }
    };

    std::vector<StringRef> refs;
    refs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        refs.push_back({strings[i], string_lengths[i]});
    }

    // Sort and deduplicate
    std::sort(refs.begin(), refs.end());
    auto last = std::unique(refs.begin(), refs.end());
    refs.erase(last, refs.end());

    size_t unique_count = refs.size();

    // Calculate total size needed
    size_t total_string_data = 0;
    for (const auto& ref : refs) {
        total_string_data += ref.len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Step 2: Allocate payload
    // Format: [count:4][offsets:4*(count+1)][string_data:N]
    size_t val_size = 4 + 4 * (unique_count + 1) + total_string_data;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;

    // Step 3: Write data
    // Write count
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)unique_count;

    // Write offsets array
    uint32_t* offsets = reinterpret_cast<uint32_t*>(val_ptr + 4);
    char* string_data = reinterpret_cast<char*>(val_ptr + 4 + 4 * (unique_count + 1));

    uint32_t current_offset = 0;
    for (size_t i = 0; i < unique_count; ++i) {
        offsets[i] = current_offset;
        memcpy(string_data + current_offset, refs[i].data, refs[i].len);
        current_offset += refs[i].len;
    }
    offsets[unique_count] = current_offset;  // Total length

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_STRING_SET;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_string_set(shm_handle_t handle,
                                   const void* key,
                                   size_t key_len,
                                   shm_string_set_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_STRING_SET) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointers
    const uint32_t* offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* string_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

    out_view->offsets = offsets;
    out_view->string_data = string_data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_string_vector(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      const char** strings,
                                      const size_t* string_lengths,
                                      size_t count) {
    if (!handle || !key || !strings || !string_lengths) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Calculate total size needed
    size_t total_string_data = 0;
    for (size_t i = 0; i < count; ++i) {
        total_string_data += string_lengths[i];
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Allocate payload
    // Format: [count:4][offsets:4*(count+1)][string_data:N]
    size_t val_size = 4 + 4 * (count + 1) + total_string_data;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;

    // Write count
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;

    // Write offsets and string data
    uint32_t* offsets = reinterpret_cast<uint32_t*>(val_ptr + 4);
    char* string_data = reinterpret_cast<char*>(val_ptr + 4 + 4 * (count + 1));

    uint32_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        offsets[i] = current_offset;
        const size_t len = string_lengths[i];
        if (len > 0) {
            memcpy(string_data + current_offset, strings[i], len);
            current_offset += (uint32_t)len;
        }
    }
    offsets[count] = current_offset;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_STRING_VECTOR;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_string_vector(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      shm_string_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_STRING_VECTOR) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* string_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

    out_view->offsets = offsets;
    out_view->string_data = string_data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_dict_str_int(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     const char** keys,
                                     const size_t* key_lengths,
                                     const int64_t* values,
                                     size_t count) {
    if (!handle || !key || !keys || !key_lengths || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Step 1: Create key-value pairs for sorting
    struct KVPair {
        const char* key_data;
        size_t key_len;
        int64_t value;
        bool operator<(const KVPair& other) const {
            int cmp = strncmp(key_data, other.key_data, std::min(key_len, other.key_len));
            if (cmp != 0) return cmp < 0;
            return key_len < other.key_len;
        }
    };

    std::vector<KVPair> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.push_back({keys[i], key_lengths[i], values[i]});
    }

    // Sort by key
    std::sort(pairs.begin(), pairs.end());

    // Calculate total size needed
    size_t total_key_data = 0;
    for (const auto& pair : pairs) {
        total_key_data += pair.key_len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Step 2: Allocate payload
    // Format: [count:4][key_offsets:4*(count+1)][keys_data:M][values:8*count]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 8 * count;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;

    // Step 3: Write data
    // Write count
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;

    // Write key_offsets array
    uint32_t* offsets = reinterpret_cast<uint32_t*>(val_ptr + 4);
    char* keys_data = reinterpret_cast<char*>(val_ptr + 4 + 4 * (count + 1));

    uint32_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        offsets[i] = current_offset;
        memcpy(keys_data + current_offset, pairs[i].key_data, pairs[i].key_len);
        current_offset += pairs[i].key_len;
    }
    offsets[count] = current_offset;  // Total key data length

    // Write values array
    int64_t* vals = reinterpret_cast<int64_t*>(keys_data + current_offset);
    for (size_t i = 0; i < count; ++i) {
        vals[i] = pairs[i].value;
    }

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_INT;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_int(shm_handle_t handle,
                                     const void* key,
                                     size_t key_len,
                                     shm_dict_str_int_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_DICT_STR_INT) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointers
    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* keys_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

    // Values start after keys_data
    uint32_t keys_data_length = key_offsets[count];
    const int64_t* vals = reinterpret_cast<const int64_t*>(keys_data + keys_data_length);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->values = vals;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_insert_dict_str_float(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       const char** keys,
                                       const size_t* key_lengths,
                                       const double* values,
                                       size_t count) {
    if (!handle || !key || !keys || !key_lengths || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Step 1: Create key-value pairs for sorting
    struct KVPair {
        const char* key_data;
        size_t key_len;
        double value;
        bool operator<(const KVPair& other) const {
            int cmp = strncmp(key_data, other.key_data, std::min(key_len, other.key_len));
            if (cmp != 0) return cmp < 0;
            return key_len < other.key_len;
        }
    };

    std::vector<KVPair> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.push_back({keys[i], key_lengths[i], values[i]});
    }

    // Sort by key
    std::sort(pairs.begin(), pairs.end());

    // Calculate total size needed
    size_t total_key_data = 0;
    for (const auto& pair : pairs) {
        total_key_data += pair.key_len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Step 2: Allocate payload
    // Format: [count:4][key_offsets:4*(count+1)][keys_data:M][values:8*count]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 8 * count;
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* val_ptr = payloadBase + val_payload_off;

    // Step 3: Write data
    // Write count
    *reinterpret_cast<uint32_t*>(val_ptr) = (uint32_t)count;

    // Write key_offsets array
    uint32_t* offsets = reinterpret_cast<uint32_t*>(val_ptr + 4);
    char* keys_data = reinterpret_cast<char*>(val_ptr + 4 + 4 * (count + 1));

    uint32_t current_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        offsets[i] = current_offset;
        memcpy(keys_data + current_offset, pairs[i].key_data, pairs[i].key_len);
        current_offset += pairs[i].key_len;
    }
    offsets[count] = current_offset;  // Total key data length

    // Write values array
    double* vals = reinterpret_cast<double*>(keys_data + current_offset);
    for (size_t i = 0; i < count; ++i) {
        vals[i] = pairs[i].value;
    }

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_FLOAT;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_string(shm_handle_t handle,
                                        const void* key,
                                        size_t key_len,
                                        const char** keys,
                                        const size_t* key_lengths,
                                        const char** values,
                                        const size_t* value_lengths,
                                        size_t count) {
    if (!handle || !key || !keys || !key_lengths || !values || !value_lengths) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct KVPair {
        const char* key_data;
        size_t key_len;
        const char* value_data;
        size_t value_len;
        bool operator<(const KVPair& other) const {
            int cmp = strncmp(key_data, other.key_data, std::min(key_len, other.key_len));
            if (cmp != 0) return cmp < 0;
            return key_len < other.key_len;
        }
    };

    std::vector<KVPair> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.push_back({keys[i], key_lengths[i], values[i], value_lengths[i]});
    }

    std::sort(pairs.begin(), pairs.end());

    size_t total_key_data = 0;
    size_t total_value_data = 0;
    for (const auto& p : pairs) {
        total_key_data += p.key_len;
        total_value_data += p.value_len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Format:
    // [count:4]
    // [key_offsets:4*(count+1)] [keys_data:K]
    // [value_offsets:4*(count+1)] [values_data:V]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 4 * (count + 1) + total_value_data;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;

    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* values_data_out = reinterpret_cast<char*>(ptr);

    uint32_t key_off = 0;
    uint32_t val_off = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = key_off;
        if (pairs[i].key_len > 0) {
            memcpy(keys_data + key_off, pairs[i].key_data, pairs[i].key_len);
            key_off += (uint32_t)pairs[i].key_len;
        }

        value_offsets[i] = val_off;
        if (pairs[i].value_len > 0) {
            memcpy(values_data_out + val_off, pairs[i].value_data, pairs[i].value_len);
            val_off += (uint32_t)pairs[i].value_len;
        }
    }
    key_offsets[count] = key_off;
    value_offsets[count] = val_off;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_STRING;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_bool(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      const char** keys,
                                      const size_t* key_lengths,
                                      const uint8_t* values,
                                      size_t count) {
    if (!handle || !key || !keys || !key_lengths || !values) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct KVPair {
        const char* key_data;
        size_t key_len;
        uint8_t value;
    };

    std::vector<KVPair> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.push_back({keys[i], key_lengths[i], (uint8_t)(values[i] ? 1 : 0)});
    }

    std::sort(pairs.begin(), pairs.end(), [](const KVPair& a, const KVPair& b) {
        return bytes_less(a.key_data, a.key_len, b.key_data, b.key_len);
    });
    for (size_t i = 1; i < pairs.size(); ++i) {
        if (bytes_equal(pairs[i - 1].key_data, pairs[i - 1].key_len, pairs[i].key_data, pairs[i].key_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    size_t total_key_data = 0;
    for (const auto& p : pairs) total_key_data += p.key_len;

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Format: [count:4][key_offsets:4*(count+1)][keys_data:K][values:1*count]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 1 * count;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data_out = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;
    uint8_t* values_out = reinterpret_cast<uint8_t*>(ptr);

    uint32_t kcur = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = kcur;
        if (pairs[i].key_len) {
            memcpy(keys_data_out + kcur, pairs[i].key_data, pairs[i].key_len);
            kcur += (uint32_t)pairs[i].key_len;
        }
        values_out[i] = pairs[i].value;
    }
    key_offsets[count] = kcur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_BOOL;

    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_bytes(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       const char** keys,
                                       const size_t* key_lengths,
                                       const uint8_t* const* values,
                                       const size_t* value_lengths,
                                       size_t count) {
    if (!handle || !key || !keys || !key_lengths || !values || !value_lengths) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct KVPair {
        const char* key_data;
        size_t key_len;
        const uint8_t* value_data;
        size_t value_len;
    };

    std::vector<KVPair> pairs;
    pairs.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        pairs.push_back({keys[i], key_lengths[i], values[i], value_lengths[i]});
    }

    std::sort(pairs.begin(), pairs.end(), [](const KVPair& a, const KVPair& b) {
        return bytes_less(a.key_data, a.key_len, b.key_data, b.key_len);
    });
    for (size_t i = 1; i < pairs.size(); ++i) {
        if (bytes_equal(pairs[i - 1].key_data, pairs[i - 1].key_len, pairs[i].key_data, pairs[i].key_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    size_t total_key_data = 0;
    size_t total_value_data = 0;
    for (const auto& p : pairs) {
        total_key_data += p.key_len;
        total_value_data += p.value_len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Format:
    // [count:4]
    // [key_offsets:4*(count+1)] [keys_data:K]
    // [value_offsets:4*(count+1)] [values_data:V]
    if (total_key_data > UINT32_MAX || total_value_data > UINT32_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_INVALID_PARAM;
    }

    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 4 * (count + 1) + total_value_data;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data_out = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;

    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    uint8_t* values_data_out = reinterpret_cast<uint8_t*>(ptr);

    uint32_t kcur = 0;
    uint32_t vcur = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = kcur;
        if (pairs[i].key_len) {
            memcpy(keys_data_out + kcur, pairs[i].key_data, pairs[i].key_len);
            kcur += (uint32_t)pairs[i].key_len;
        }

        value_offsets[i] = vcur;
        if (pairs[i].value_len) {
            memcpy(values_data_out + vcur, pairs[i].value_data, pairs[i].value_len);
            vcur += (uint32_t)pairs[i].value_len;
        }
    }
    key_offsets[count] = kcur;
    value_offsets[count] = vcur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_BYTES;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_float_vector(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              const char** keys,
                                              const size_t* key_lengths,
                                              const uint32_t* in_value_offsets,
                                              const uint32_t* in_value_lengths,
                                              const double* values_flat,
                                              size_t count) {
    if (!handle || !key || !keys || !key_lengths || !in_value_offsets || !in_value_lengths || !values_flat) {
        return SHM_ERR_INVALID_PARAM;
    }

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct KV {
        const char* key_data;
        size_t key_len;
        uint32_t value_off;
        uint32_t value_len;
    };

    std::vector<KV> items;
    items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        items.push_back(KV{keys[i], key_lengths[i], in_value_offsets[i], in_value_lengths[i]});
    }

    std::sort(items.begin(), items.end(), [](const KV& a, const KV& b) {
        return bytes_less(a.key_data, a.key_len, b.key_data, b.key_len);
    });
    for (size_t i = 1; i < items.size(); ++i) {
        if (bytes_equal(items[i - 1].key_data, items[i - 1].key_len, items[i].key_data, items[i].key_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    size_t total_key_data = 0;
    uint64_t total_values = 0;
    for (const auto& it : items) {
        total_key_data += it.key_len;
        total_values += it.value_len;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Layout:
    // [count:4]
    // [key_offsets:4*(count+1)] [keys_data:K]
    // [value_offsets:4*(count+1)] [value_lengths:4*count]
    // [values_flat:8*sum(value_lengths)]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 4 * (count + 1) + 4 * count + 8 * (size_t)total_values;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data_out = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;

    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    uint32_t* value_lengths = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * count;
    double* values_out = reinterpret_cast<double*>(ptr);

    // write keys
    uint32_t kcur = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = kcur;
        memcpy(keys_data_out + kcur, items[i].key_data, items[i].key_len);
        kcur += (uint32_t)items[i].key_len;
    }
    key_offsets[count] = kcur;

    // write values: flatten by iterating items in sorted order
    uint32_t vcur = 0;
    for (size_t i = 0; i < count; ++i) {
        value_offsets[i] = vcur;
        value_lengths[i] = items[i].value_len;
        if (items[i].value_len) {
            const double* src = values_flat + items[i].value_off;
            memcpy(values_out + vcur, src, (size_t)items[i].value_len * sizeof(double));
        }
        vcur += items[i].value_len;
    }
    value_offsets[count] = vcur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_FLOAT_VECTOR;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }
    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_float_vector(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              shm_dict_str_float_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_DICT_STR_FLOAT_VECTOR) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* keys_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

    const uint32_t keys_len = key_offsets[count];
    const uint8_t* after_keys = reinterpret_cast<const uint8_t*>(keys_data + keys_len);
    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(after_keys);
    const uint32_t* value_lengths = reinterpret_cast<const uint32_t*>(after_keys + 4 * (count + 1));
    const double* values_flat_out = reinterpret_cast<const double*>(after_keys + 4 * (count + 1) + 4 * count);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->value_offsets = value_offsets;
    out_view->value_lengths = value_lengths;
    out_view->values_flat = values_flat_out;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_float_matrix(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              const char** keys,
                                              const size_t* key_lengths,
                                              const uint32_t* in_value_offsets,
                                              const uint32_t* in_rows,
                                              const uint32_t* in_cols,
                                              const double* values_flat,
                                              size_t count) {
    if (!handle || !key || !keys || !key_lengths || !in_value_offsets || !in_rows || !in_cols || !values_flat) {
        return SHM_ERR_INVALID_PARAM;
    }

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct KV {
        const char* key_data;
        size_t key_len;
        uint32_t value_off;
        uint32_t rows;
        uint32_t cols;
    };

    std::vector<KV> items;
    items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        items.push_back(KV{keys[i], key_lengths[i], in_value_offsets[i], in_rows[i], in_cols[i]});
    }

    std::sort(items.begin(), items.end(), [](const KV& a, const KV& b) {
        return bytes_less(a.key_data, a.key_len, b.key_data, b.key_len);
    });
    for (size_t i = 1; i < items.size(); ++i) {
        if (bytes_equal(items[i - 1].key_data, items[i - 1].key_len, items[i].key_data, items[i].key_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    size_t total_key_data = 0;
    uint64_t total_values = 0;
    for (const auto& it : items) {
        total_key_data += it.key_len;
        total_values += (uint64_t)it.rows * (uint64_t)it.cols;
    }

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    // Layout:
    // [count:4]
    // [key_offsets:4*(count+1)] [keys_data:K]
    // [value_offsets:4*(count+1)] [rows:4*count] [cols:4*count]
    // [values_flat:8*sum(rows*cols)]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 4 * (count + 1) + 4 * count + 4 * count + 8 * (size_t)total_values;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data_out = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;

    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    uint32_t* rows_out = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * count;
    uint32_t* cols_out = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * count;
    double* values_out = reinterpret_cast<double*>(ptr);

    uint32_t kcur = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = kcur;
        memcpy(keys_data_out + kcur, items[i].key_data, items[i].key_len);
        kcur += (uint32_t)items[i].key_len;
    }
    key_offsets[count] = kcur;

    uint32_t vcur = 0;
    for (size_t i = 0; i < count; ++i) {
        value_offsets[i] = vcur;
        rows_out[i] = items[i].rows;
        cols_out[i] = items[i].cols;
        const uint64_t len = (uint64_t)items[i].rows * (uint64_t)items[i].cols;
        if (len) {
            const double* src = values_flat + items[i].value_off;
            memcpy(values_out + vcur, src, (size_t)len * sizeof(double));
        }
        vcur += (uint32_t)len;
    }
    value_offsets[count] = vcur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_FLOAT_MATRIX;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }
    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_float_matrix(shm_handle_t handle,
                                              const void* key,
                                              size_t key_len,
                                              shm_dict_str_float_matrix_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_DICT_STR_FLOAT_MATRIX) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* keys_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
    const uint32_t keys_len = key_offsets[count];

    const uint8_t* after_keys = reinterpret_cast<const uint8_t*>(keys_data + keys_len);
    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(after_keys);
    const uint32_t* rows = reinterpret_cast<const uint32_t*>(after_keys + 4 * (count + 1));
    const uint32_t* cols = reinterpret_cast<const uint32_t*>(after_keys + 4 * (count + 1) + 4 * count);
    const double* values_flat_out = reinterpret_cast<const double*>(after_keys + 4 * (count + 1) + 4 * count + 4 * count);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->value_offsets = value_offsets;
    out_view->rows = rows;
    out_view->cols = cols;
    out_view->values_flat = values_flat_out;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_insert_dict_str_string_vector(shm_handle_t handle,
                                               const void* key,
                                               size_t key_len,
                                               const char** keys,
                                               const size_t* key_lengths,
                                               const uint32_t* in_value_list_offsets,
                                               const uint32_t* in_string_offsets,
                                               const char* in_string_data,
                                               size_t n_strings,
                                               size_t count) {
    if (!handle || !key || !keys || !key_lengths || !in_value_list_offsets || !in_string_offsets || (!in_string_data && n_strings)) {
        return SHM_ERR_INVALID_PARAM;
    }

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    // Each key i corresponds to a list of strings in the global string table.
    // We reorder keys by sorting them; we must reorder value_list_offsets accordingly.
    struct KV {
        const char* key_data;
        size_t key_len;
        uint32_t list_start;
        uint32_t list_end;
    };

    std::vector<KV> items;
    items.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        items.push_back(KV{keys[i], key_lengths[i], in_value_list_offsets[i], in_value_list_offsets[i + 1]});
    }

    std::sort(items.begin(), items.end(), [](const KV& a, const KV& b) {
        return bytes_less(a.key_data, a.key_len, b.key_data, b.key_len);
    });
    for (size_t i = 1; i < items.size(); ++i) {
        if (bytes_equal(items[i - 1].key_data, items[i - 1].key_len, items[i].key_data, items[i].key_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    // Build a compacted string table in the new key order.
    // We do NOT deduplicate strings; we preserve original order per list.
    std::vector<uint32_t> out_value_list_offsets;
    out_value_list_offsets.reserve(count + 1);
    std::vector<uint32_t> out_string_offsets;
    out_string_offsets.reserve(n_strings + 1);
    std::string out_string_blob;
    out_string_blob.reserve((size_t)(in_string_offsets[n_strings]));

    out_value_list_offsets.push_back(0);
    out_string_offsets.push_back(0);
    uint32_t string_index_cur = 0;

    for (const auto& it : items) {
        for (uint32_t j = it.list_start; j < it.list_end; ++j) {
            const uint32_t s_start = in_string_offsets[j];
            const uint32_t s_end = in_string_offsets[j + 1];
            const uint32_t s_len = s_end - s_start;
            if (s_len) {
                out_string_blob.append(in_string_data + s_start, in_string_data + s_end);
            }
            const uint32_t new_off = (uint32_t)out_string_blob.size();
            out_string_offsets.push_back(new_off);
            ++string_index_cur;
        }
        out_value_list_offsets.push_back(string_index_cur);
    }

    const size_t out_n_strings = (size_t)string_index_cur;

    size_t total_key_data = 0;
    for (const auto& it : items) total_key_data += it.key_len;

    // Layout:
    // [count:4]
    // [key_offsets:4*(count+1)] [keys_data:K]
    // [value_list_offsets:4*(count+1)]
    // [n_strings:4]
    // [string_offsets:4*(n_strings+1)] [string_data:V]
    size_t val_size = 4 + 4 * (count + 1) + total_key_data + 4 * (count + 1) + 4 + 4 * (out_n_strings + 1) + out_string_blob.size();

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    ptr += 4;

    uint32_t* key_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    char* keys_data_out = reinterpret_cast<char*>(ptr);
    ptr += total_key_data;

    uint32_t* value_list_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (count + 1);

    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)out_n_strings;
    ptr += 4;

    uint32_t* string_offsets = reinterpret_cast<uint32_t*>(ptr);
    ptr += 4 * (out_n_strings + 1);
    char* string_data_out = reinterpret_cast<char*>(ptr);

    // keys
    uint32_t kcur = 0;
    for (size_t i = 0; i < count; ++i) {
        key_offsets[i] = kcur;
        memcpy(keys_data_out + kcur, items[i].key_data, items[i].key_len);
        kcur += (uint32_t)items[i].key_len;
    }
    key_offsets[count] = kcur;

    // value_list_offsets
    for (size_t i = 0; i < out_value_list_offsets.size(); ++i) {
        value_list_offsets[i] = out_value_list_offsets[i];
    }

    // string table
    for (size_t i = 0; i < out_string_offsets.size(); ++i) {
        string_offsets[i] = out_string_offsets[i];
    }
    if (!out_string_blob.empty()) {
        memcpy(string_data_out, out_string_blob.data(), out_string_blob.size());
    }

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_DICT_STR_STRING_VECTOR;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }
    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_string_vector(shm_handle_t handle,
                                               const void* key,
                                               size_t key_len,
                                               shm_dict_str_string_vector_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_DICT_STR_STRING_VECTOR) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* keys_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
    const uint32_t keys_len = key_offsets[count];

    const uint8_t* after_keys = reinterpret_cast<const uint8_t*>(keys_data + keys_len);
    const uint32_t* value_list_offsets = reinterpret_cast<const uint32_t*>(after_keys);
    const uint32_t n_strings = *reinterpret_cast<const uint32_t*>(after_keys + 4 * (count + 1));
    const uint32_t* string_offsets = reinterpret_cast<const uint32_t*>(after_keys + 4 * (count + 1) + 4);
    const char* string_data = reinterpret_cast<const char*>(after_keys + 4 * (count + 1) + 4 + 4 * (n_strings + 1));

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->value_list_offsets = value_list_offsets;
    out_view->string_offsets = string_offsets;
    out_view->string_data = string_data;
    out_view->count = count;
    out_view->n_strings = n_strings;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_string(shm_handle_t handle,
                                        const void* key,
                                        size_t key_len,
                                        shm_dict_str_string_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_DICT_STR_STRING) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4;

    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    const char* keys_data = reinterpret_cast<const char*>(ptr);
    const uint32_t key_total = key_offsets[count];
    ptr += key_total;

    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    const char* values_data = reinterpret_cast<const char*>(ptr);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->value_offsets = value_offsets;
    out_view->values_data = values_data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_bool(shm_handle_t handle,
                                      const void* key,
                                      size_t key_len,
                                      shm_dict_str_bool_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_DICT_STR_BOOL) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4;

    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    const char* keys_data = reinterpret_cast<const char*>(ptr);
    const uint32_t key_total = key_offsets[count];
    ptr += key_total;

    const uint8_t* values = reinterpret_cast<const uint8_t*>(ptr);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->values = values;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_bytes(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       shm_dict_str_bytes_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_DICT_STR_BYTES) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4;

    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    const char* keys_data = reinterpret_cast<const char*>(ptr);
    const uint32_t key_total = key_offsets[count];
    ptr += key_total;

    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(ptr);
    ptr += 4 * (count + 1);
    const uint8_t* values_data = reinterpret_cast<const uint8_t*>(ptr);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->value_offsets = value_offsets;
    out_view->values_data = values_data;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_lookup_dict_str_float(shm_handle_t handle,
                                       const void* key,
                                       size_t key_len,
                                       shm_dict_str_float_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) {
        return SHM_ERR_NOT_FOUND;
    }

    if (node->value_type != SHM_TYPE_DICT_STR_FLOAT) {
        return SHM_ERR_TYPE_MISMATCH;
    }

    const uint8_t* ptr = s->payload_base() + node->val_off;

    // Read count
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);

    // Zero-copy: return pointers
    const uint32_t* key_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* keys_data = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));

    // Values start after keys_data
    uint32_t keys_data_length = key_offsets[count];
    const double* vals = reinterpret_cast<const double*>(keys_data + keys_data_length);

    out_view->key_offsets = key_offsets;
    out_view->keys_data = keys_data;
    out_view->values = vals;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;

    return SHM_OK;
}

// ============================================================================
// OBJECT/LIST (Recursive Typed Tree)
// ============================================================================

shm_error_t shm_insert_object(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               const char** field_names,
                               const size_t* field_name_lengths,
                               const uint8_t* field_types,
                               const void* const* field_payloads,
                               const size_t* field_payload_lengths,
                               size_t field_count) {
    if (!handle || !key || !field_names || !field_name_lengths || !field_types || !field_payloads || !field_payload_lengths) {
        return SHM_ERR_INVALID_PARAM;
    }

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    struct Field {
        const char* name;
        size_t name_len;
        uint8_t type;
        const void* payload;
        size_t payload_len;
    };

    std::vector<Field> fields;
    fields.reserve(field_count);
    for (size_t i = 0; i < field_count; ++i) {
        fields.push_back(Field{field_names[i], field_name_lengths[i], field_types[i], field_payloads[i], field_payload_lengths[i]});
    }

    std::sort(fields.begin(), fields.end(), [](const Field& a, const Field& b) {
        return bytes_less(a.name, a.name_len, b.name, b.name_len);
    });
    for (size_t i = 1; i < fields.size(); ++i) {
        if (bytes_equal(fields[i - 1].name, fields[i - 1].name_len, fields[i].name, fields[i].name_len)) {
            return SHM_ERR_INVALID_PARAM;
        }
    }

    size_t total_names = 0;
    size_t total_values = 0;
    for (const auto& f : fields) {
        total_names += f.name_len;
        total_values += f.payload_len;
    }

    // Layout:
    // [count:4]
    // [name_offsets:4*(count+1)]
    // [names_blob:total_names]
    // [field_types:count]
    // [pad to 4]
    // [value_offsets:4*(count+1)]
    // [values_blob:total_values]
    const size_t count = field_count;
    size_t header_size = 4;
    size_t name_offsets_size = 4 * (count + 1);
    size_t field_types_size = count;
    size_t value_offsets_size = 4 * (count + 1);
    size_t val_size = header_size + name_offsets_size + total_names + field_types_size;
    val_size = align_up(val_size, 4);
    val_size += value_offsets_size + total_values;

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;
    uint32_t* name_offsets = reinterpret_cast<uint32_t*>(ptr + 4);
    char* names_blob = reinterpret_cast<char*>(ptr + 4 + name_offsets_size);

    uint32_t cur = 0;
    for (size_t i = 0; i < count; ++i) {
        name_offsets[i] = cur;
        memcpy(names_blob + cur, fields[i].name, fields[i].name_len);
        cur += (uint32_t)fields[i].name_len;
    }
    name_offsets[count] = cur;

    uint8_t* field_types_out = reinterpret_cast<uint8_t*>(names_blob + total_names);
    for (size_t i = 0; i < count; ++i) {
        field_types_out[i] = fields[i].type;
    }

    uint8_t* after_types = field_types_out + field_types_size;
    after_types = reinterpret_cast<uint8_t*>(payloadBase + val_payload_off + align_up((size_t)(after_types - ptr), 4));

    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(after_types);
    uint8_t* values_blob = reinterpret_cast<uint8_t*>(after_types + value_offsets_size);
    uint32_t vcur = 0;
    for (size_t i = 0; i < count; ++i) {
        value_offsets[i] = vcur;
        if (fields[i].payload_len) {
            memcpy(values_blob + vcur, fields[i].payload, fields[i].payload_len);
        }
        vcur += (uint32_t)fields[i].payload_len;
    }
    value_offsets[count] = vcur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_OBJECT;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_insert_list(shm_handle_t handle,
                             const void* key,
                             size_t key_len,
                             const uint8_t* elem_types,
                             const void* const* elem_payloads,
                             const size_t* elem_payload_lengths,
                             size_t count) {
    if (!handle || !key || !elem_types || !elem_payloads || !elem_payload_lengths) {
        return SHM_ERR_INVALID_PARAM;
    }

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);

    size_t total_values = 0;
    for (size_t i = 0; i < count; ++i) total_values += elem_payload_lengths[i];

    // Layout:
    // [count:4]
    // [elem_types:count]
    // [pad to 4]
    // [value_offsets:4*(count+1)]
    // [values_blob:total_values]
    size_t val_size = 4 + count;
    val_size = align_up(val_size, 4);
    val_size += 4 * (count + 1) + total_values;

    int lockRes = pthread_mutex_lock(&s->hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        pthread_mutex_consistent(&s->hdr->writer_mutex);
#endif
    } else if (lockRes != 0) {
        return SHM_ERR_OPEN_FAILED;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(k, key_len);
    uint32_t bucket = (uint32_t)(hash % s->hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s->base + s->hdr->bucket_area_off) + bucket;

    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_size);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    uint8_t* payloadBase = s->payload_base();
    memcpy(payloadBase + key_payload_off, k, key_len);

    uint8_t* ptr = payloadBase + val_payload_off;
    *reinterpret_cast<uint32_t*>(ptr) = (uint32_t)count;

    uint8_t* types_out = ptr + 4;
    memcpy(types_out, elem_types, count);

    uint8_t* after_types = reinterpret_cast<uint8_t*>(payloadBase + val_payload_off + align_up((size_t)(4 + count), 4));
    uint32_t* value_offsets = reinterpret_cast<uint32_t*>(after_types);
    uint8_t* values_blob = reinterpret_cast<uint8_t*>(after_types + 4 * (count + 1));

    uint32_t cur = 0;
    for (size_t i = 0; i < count; ++i) {
        value_offsets[i] = cur;
        if (elem_payload_lengths[i]) {
            memcpy(values_blob + cur, elem_payloads[i], elem_payload_lengths[i]);
        }
        cur += (uint32_t)elem_payload_lengths[i];
    }
    value_offsets[count] = cur;

    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    Node* nodes = s->node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_size;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1;
    tmp.version = 1;
    tmp.value_type = SHM_TYPE_LIST;
    nodes[node_idx] = tmp;

    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    if (!cas_success) {
        __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s->hdr->writer_mutex);
        return SHM_ERR_NO_SPACE;
    }

    __atomic_fetch_add(&s->hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&s->hdr->writer_mutex);
    return SHM_OK;
}

shm_error_t shm_lookup_object(shm_handle_t handle,
                               const void* key,
                               size_t key_len,
                               shm_object_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_OBJECT) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint32_t* name_offsets = reinterpret_cast<const uint32_t*>(ptr + 4);
    const char* names_blob = reinterpret_cast<const char*>(ptr + 4 + 4 * (count + 1));
    const uint8_t* field_types = reinterpret_cast<const uint8_t*>(names_blob + name_offsets[count]);

    const uint8_t* after_types = reinterpret_cast<const uint8_t*>(ptr + align_up(4 + 4 * (count + 1) + name_offsets[count] + count, 4));
    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(after_types);
    const uint8_t* values_blob = reinterpret_cast<const uint8_t*>(after_types + 4 * (count + 1));

    out_view->name_offsets = name_offsets;
    out_view->names_data = names_blob;
    out_view->field_types = field_types;
    out_view->value_offsets = value_offsets;
    out_view->values_data = values_blob;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_lookup_list(shm_handle_t handle,
                             const void* key,
                             size_t key_len,
                             shm_list_view_t* out_view) {
    if (!handle || !key || !out_view) return SHM_ERR_INVALID_PARAM;

    SharedShm* s = reinterpret_cast<SharedShm*>(handle);
    const uint8_t* k = reinterpret_cast<const uint8_t*>(key);
    uint64_t g1 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);

    Node* node = find_node_by_key(s, k, key_len);
    if (!node) return SHM_ERR_NOT_FOUND;
    if (node->value_type != SHM_TYPE_LIST) return SHM_ERR_TYPE_MISMATCH;

    const uint8_t* ptr = s->payload_base() + node->val_off;
    uint32_t count = *reinterpret_cast<const uint32_t*>(ptr);
    const uint8_t* elem_types = ptr + 4;
    const uint8_t* after_types = reinterpret_cast<const uint8_t*>(ptr + align_up(4 + count, 4));
    const uint32_t* value_offsets = reinterpret_cast<const uint32_t*>(after_types);
    const uint8_t* values_blob = reinterpret_cast<const uint8_t*>(after_types + 4 * (count + 1));

    out_view->elem_types = elem_types;
    out_view->value_offsets = value_offsets;
    out_view->values_data = values_blob;
    out_view->count = count;

    uint64_t g2 = __atomic_load_n(&s->hdr->generation, __ATOMIC_SEQ_CST);
    if (g1 != g2) return SHM_ERR_CONCURRENT_MOD;
    return SHM_OK;
}

shm_error_t shm_object_get_field(const shm_object_view_t* object_view,
                                  const char* field_name,
                                  size_t field_name_len,
                                  shm_typed_value_view_t* out_value) {
    if (!object_view || !field_name || !out_value) return SHM_ERR_INVALID_PARAM;
    const size_t n = object_view->count;
    size_t lo = 0;
    size_t hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint32_t start = object_view->name_offsets[mid];
        const uint32_t end = object_view->name_offsets[mid + 1];
        const char* name_ptr = object_view->names_data + start;
        const size_t name_len = end - start;

        if (bytes_equal(name_ptr, name_len, field_name, field_name_len)) {
            const uint32_t vstart = object_view->value_offsets[mid];
            const uint32_t vend = object_view->value_offsets[mid + 1];
            out_value->type = (shm_value_type_t)object_view->field_types[mid];
            out_value->payload = object_view->values_data + vstart;
            out_value->payload_len = (size_t)(vend - vstart);
            return SHM_OK;
        }
        if (bytes_less(name_ptr, name_len, field_name, field_name_len)) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return SHM_ERR_NOT_FOUND;
}

shm_error_t shm_list_get_element(const shm_list_view_t* list_view,
                                  size_t index,
                                  shm_typed_value_view_t* out_value) {
    if (!list_view || !out_value) return SHM_ERR_INVALID_PARAM;
    if (index >= list_view->count) return SHM_ERR_NOT_FOUND;
    const uint32_t start = list_view->value_offsets[index];
    const uint32_t end = list_view->value_offsets[index + 1];
    out_value->type = (shm_value_type_t)list_view->elem_types[index];
    out_value->payload = list_view->values_data + start;
    out_value->payload_len = (size_t)(end - start);
    return SHM_OK;
}

} // extern "C"
