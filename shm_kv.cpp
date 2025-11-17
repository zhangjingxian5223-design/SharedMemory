#include <unistd.h> // POSIX 基本系统调用与工具（close、ftruncate、sleep、getpid 等）
#include <sys/mman.h> // 内存映射与共享内存接口（mmap/munmap、PROT_/MAP_、shm_open 等）
#include <sys/stat.h> // 文件状态与权限常量/结构（stat、S_IRUSR 等）
#include <fcntl.h> // 文件描述符控制与打开标志（open、O_CREAT/O_RDWR、fcntl）
#include <cerrno> // errno 和错误码（EOWNERDEAD 等）
#include <cstring> // C 字符/内存函数（memcpy/memset/strcmp/strlen/strerror）
#include <cstdint> // 固定宽度整数类型（uint32_t、uint64_t 等）
#include <cstdio> // C 标准 I/O（printf、FILE*、perror）
#include <cstdlib> // 通用工具（malloc/free、exit、atoi、size_t 等）
#include <string> // C++ 字符串 std::string
#include <stdexcept> // 标准异常（std::runtime_error 等）
#include <atomic> // C++ 原子类型与内存序（std::atomic 等；本代码主要用 GCC 原子内建）
#include <pthread.h> // POSIX 线程/互斥量/属性（pthread_mutex_t 等，支持进程间共享）
#include <iostream> // C++ 流式 I/O（std::cout、std::endl）
#include <cassert> // 断言宏 assert，用于开发期条件检查

// Simple relocatable shared memory KV layout
// HEADER | BUCKETS | NODES | PAYLOAD
// 定义共享内存的规范化布局，头部记录元信息，桶区存每个哈希桶的链表头索引，节点区存键值元数据与链，负载区存实际 key/val 字节。

static constexpr uint32_t EMPTY_INDEX = 0xFFFFFFFFu; // 表示"空/无"索引（如桶为空、链尾））；选最大值避免与合法索引冲突
static constexpr uint32_t MAGIC = 0x4C4D4252; // 'LMBR' 魔数签名 用于在打开共享内存时校验格式是否正确、数据是否被污染或未初始化
static constexpr size_t DEFAULT_N_BUCKETS = 1 << 12; // 4096
static constexpr size_t DEFAULT_N_NODES = 1 << 16;   // 65536 默认桶数量与节点容量（取2的幂便于哈希分布与潜在按位与取模优化）

// 安全限制常量，防止恶意或错误输入导致资源耗尽
static constexpr size_t MAX_KEY_LEN = 1 << 16;      // 64KB - key 最大长度
static constexpr size_t MAX_VAL_LEN = 1 << 28;      // 256MB - value 最大长度
static constexpr size_t MAX_TOTAL_SIZE = 1ULL << 32; // 4GB - 共享内存最大尺寸
static constexpr size_t MAX_BUCKETS = 1 << 24;      // 16M - 最大桶数
static constexpr size_t MAX_NODES = 1 << 24;        // 16M - 最大节点数

// CAS 循环最大重试次数，防止高竞争下的死循环
static constexpr uint32_t MAX_CAS_RETRIES = 10000;  // 最大重试次数

// alignment helper
// align_up(x, a)将数值 x 向上对齐到 a 的倍数：若已对齐返回x，否则返回下一个倍数。
// 其位运算公式 (x + a - 1) & ~(a - 1)要求 a为2的幂，通过先“加上a-1”越过边界，再用掩码清零低位实现对齐（例如 align_up(13, 8)=16）。
// 该函数常用于对齐内存/偏移到字节、缓存行或页边界，确保结构体和区域满足平台对齐要求。
inline size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }

struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t total_size; // 整个共享内存映射的总字节数，便于边界与校验

    uint64_t bucket_area_off; // offset from base 三段区域的相对偏移（相对于映射基址），保证可重定位
    uint64_t node_area_off;
    uint64_t payload_area_off;

    uint32_t n_buckets; // 哈希桶数量与节点数组容量，用于取模与容量管理
    uint32_t n_nodes;

    // allocation cursors (relative offsets / indices)
    uint32_t next_free_node_index; // index allocator for nodes 节点数组的索引分配游标
    uint64_t payload_alloc_off;    // bump pointer offset inside payload area 负载区的线性“bump”分配指针

    uint64_t generation; // epoch/version to detect concurrent writes 实现无锁一致性校验
    // pthread mutex for writers (process-shared)
    pthread_mutex_t writer_mutex; // 进程间共享的 pthread 互斥量，序列化写入以保证更新原子性

    uint32_t checksum; // 头部或关键元数据的校验值，检测损坏
    uint8_t reserved[32]; // pad to make header reasonably large / aligned 预留与对齐空间，便于未来扩展字段而不破坏布局。
};

// Node 表示哈希表中的一条记录（链表结点），不存放实际字节，只存放元信息与链接
// offset 为 uint32_t，单个 payload 区最大可寻址约 4GiB；结构体定长、无指针，保证可重定位与跨语言解析。 4GiB = 4 × 2^30 字节
struct Node {
    uint32_t key_off;   // offset into payload area (base + payload_area_off + key_off) 在payload段中的相对偏移和长度
    uint32_t key_len;
    uint32_t val_off;   // offset into payload area
    uint32_t val_len;
    uint32_t next_index; // index into node array, EMPTY_INDEX if none， 为 EMPTY_INDEX 表示链尾
    uint32_t flags;      // bit flags (active/deleted) 状态位，比如 1=active，置墓碑时可标记 deleted 供后台回收
    uint64_t version;    // per-node version for fine-grained validation 每结点版本号，便于细粒度并发校验/读一致性检测
};

struct SharedShm {
    int fd; // 共享内存文件描述符
    void* base; //  mmap 映射后的起始虚拟地址
    Header* hdr; // 指向映射中的Header 头部（base 开头）

     // 根据头部记录的相对偏移，将 base + offset转为对应区域的指针：
     // buckets() 返回桶区起始地址（uint32_t 数组所在处，返回为字节指针便于偏移计算）。
     // node_array() 返回节点数组首地址（Node 定长数组）。
     // payload_base() 返回负载区起始地址（存放实际 key/val 字节）。
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

// atomic CAS on bucket entry (uint32_t stored in mapped memory)
// 对共享内存中的 32 位值做一次原子“比较并交换”（CAS）的封装：当且仅当 *addr 等于
// expected 时，将其更新为 desired 并返回 true，否则不改动并返回 false。
// 用于你代码里桶头插入的 CAS 自旋，需保证 addr 自然对齐（uint32_t 按 4 字节对齐）。
bool atomic_cas_u32(uint32_t* addr, uint32_t expected, uint32_t desired) {
    // use GCC builtin (works on shared memory)
    return __atomic_compare_exchange_n(addr, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Create or open a shared memory mapping with a canonical layout
SharedShm create_or_open_shm(const char* name, size_t n_buckets = DEFAULT_N_BUCKETS, size_t n_nodes = DEFAULT_N_NODES, size_t payload_size = (1<<24)) {
    // 输入验证：检查共享内存名称
    if (!name || name[0] == '\0') {
        throw std::runtime_error("Invalid shared memory name: nullptr or empty");
    }

    // 输入验证：检查参数范围，防止资源耗尽
    if (n_buckets == 0 || n_buckets > MAX_BUCKETS) {
        throw std::runtime_error("Invalid n_buckets: must be in range [1, " + std::to_string(MAX_BUCKETS) + "]");
    }
    if (n_nodes == 0 || n_nodes > MAX_NODES) {
        throw std::runtime_error("Invalid n_nodes: must be in range [1, " + std::to_string(MAX_NODES) + "]");
    }
    if (payload_size == 0 || payload_size > MAX_TOTAL_SIZE) {
        throw std::runtime_error("Invalid payload_size: must be in range [1, " + std::to_string(MAX_TOTAL_SIZE) + "]");
    }

    bool need_init = false;
    int fd = shm_open(name, O_RDWR | O_CREAT, 0666); // 获取fd
    if (fd < 0) {
        throw std::runtime_error(std::string("shm_open failed for '") + name + "': " + strerror(errno));
    }

    // compute sizes 计算各段对齐后的大小
    size_t header_size = align_up(sizeof(Header), 64);
    size_t buckets_size = align_up(sizeof(uint32_t) * n_buckets, 64);
    size_t nodes_size = align_up(sizeof(Node) * n_nodes, 64);
    size_t payload_area_size = align_up(payload_size, 4096);

    // 求出total_size，并检查整数溢出
    size_t total_size = header_size + buckets_size + nodes_size + payload_area_size;

    // 整数溢出检查：每次加法后检查是否变小（溢出回绕）
    if (total_size < header_size || total_size < buckets_size ||
        total_size < nodes_size || total_size < payload_area_size) {
        close(fd);
        throw std::runtime_error("Integer overflow when calculating total_size");
    }

    // 检查总大小是否超过限制
    if (total_size > MAX_TOTAL_SIZE) {
        close(fd);
        throw std::runtime_error("Total size " + std::to_string(total_size) +
                                 " exceeds maximum " + std::to_string(MAX_TOTAL_SIZE));
    }

    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        throw std::runtime_error(std::string("fstat failed: ") + strerror(errno));
    }
    if ((size_t)st.st_size < total_size) {
        // need to expand
        if (ftruncate(fd, total_size) == -1) {
            close(fd);
            throw std::runtime_error(std::string("ftruncate failed: ") + strerror(errno));
        }
        need_init = true;
    }

    // 随后 mmap映射，若需要初始化或魔数不匹配 则整体清零、填充头部字段（区域偏移/容量/游标/generation等）、
    // 将所有桶置 EMPTY_INDEX，并初始化"进程共享"的 pthread 互斥量（Linux 下可设为robust）。
    void* base = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        throw std::runtime_error(std::string("mmap failed: ") + strerror(errno));
    }

    Header* hdr = reinterpret_cast<Header*>((char*)base);
    if (need_init || hdr->magic != MAGIC) {
        // initialize header and zero region
        memset(base, 0, total_size);
        hdr->magic = MAGIC;
        hdr->version = 1;
        hdr->flags = 0;
        hdr->total_size = total_size;
        hdr->bucket_area_off = header_size;
        hdr->node_area_off = header_size + buckets_size;
        hdr->payload_area_off = header_size + buckets_size + nodes_size;
        hdr->n_buckets = (uint32_t)n_buckets;
        hdr->n_nodes = (uint32_t)n_nodes;
        hdr->next_free_node_index = 0;
        hdr->payload_alloc_off = 0;
        hdr->generation = 0;
        hdr->checksum = 0;

        // initialize buckets to empty
        uint32_t* buckets_ptr = reinterpret_cast<uint32_t*>((char*)base + hdr->bucket_area_off);
        for (size_t i=0;i<n_buckets;i++) buckets_ptr[i] = EMPTY_INDEX;

        // init pthread mutex attribute for process-shared mutex
        pthread_mutexattr_t mattr;
        pthread_mutexattr_init(&mattr);
        pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
#ifdef __linux__
        // optional: robust mutex so that if writer crashes, other can recover
        pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
#endif
        pthread_mutex_init(&hdr->writer_mutex, &mattr);
        pthread_mutexattr_destroy(&mattr);
    }

    // 函数最终返回封装了 fd/base/hdr 的 SharedShm，调用方后续用它访问buckets/node_array/payload；
    // 配套需调用 close_shm 做 munmap/close（是否 shm_unlink由上层决定）。
    SharedShm s{fd, base, hdr};
    return s;
}

//用 hdr->total_size 调用 munmap 解除整块映射，再 close fd；调用后s.base/s.hdr 都失效；不会删除命名共享内存（未 shm_unlink）。
void close_shm(SharedShm& s) {
    // 防止 double free：检查资源是否有效
    if (s.base && s.base != MAP_FAILED) {
        size_t total_size = s.hdr ? s.hdr->total_size : 0;
        if (total_size > 0) {
            if (munmap(s.base, total_size) == -1) {
                // munmap 失败，记录错误但继续清理
                perror("munmap failed");
            }
        }
        s.base = nullptr;  // 标记为已释放
        s.hdr = nullptr;
    }

    // 关闭文件描述符
    if (s.fd >= 0) {
        if (close(s.fd) == -1) {
            perror("close fd failed");
        }
        s.fd = -1;  // 标记为已关闭
    }
}

// --- simple helpers ---

// hash simple 64 位 FNV-1a 哈希（先异或再乘常数），实现简单、分布较好，适合对 key做桶索引
uint64_t simple_hash(const uint8_t* data, size_t len) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (size_t i=0;i<len;++i) h ^= data[i], h *= 1099511628211ULL;
    return h;
}

// allocate node index (atomic)
// 对 header 的 next_free_node_index做原子自增（fetch_add）分配节点下标，
// 超出 n_nodes 时返回 EMPTY_INDEX表示容量耗尽；多写者安全，但不支持回收/复用已释放索引。
uint32_t alloc_node_index(SharedShm& s) {
    // fetch-and-add
    uint32_t idx = __atomic_fetch_add(&s.hdr->next_free_node_index, 1u, __ATOMIC_SEQ_CST);
    if (idx >= s.hdr->n_nodes) {
        // out of nodes
        return EMPTY_INDEX;
    }
    return idx;
}

// allocate payload (bump pointer)
// PAYLOAD 段的原子"bump 指针"分配器：
// 使用 CAS 循环实现先检查后分配，避免在失败时泄漏空间
// payload_capacity 通过total_size - payload_area_off 计算，偏移均为相对地址，进程/线程安全。
uint64_t alloc_payload(SharedShm& s, size_t len) {
    // 输入验证：检查长度合理性
    if (len == 0 || len > MAX_VAL_LEN) {
        return UINT64_MAX;
    }

    uint64_t payload_capacity = s.hdr->total_size - s.hdr->payload_area_off;
    uint64_t aligned_len = align_up(len, 8);

    // 使用 CAS 循环实现先检查后分配，防止空间泄漏
    // 添加重试计数，防止高竞争下的死循环
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint64_t current_off = __atomic_load_n(&s.hdr->payload_alloc_off, __ATOMIC_SEQ_CST);

        // 先检查：是否有足够空间
        if (current_off + aligned_len > payload_capacity) {
            return UINT64_MAX; // 空间不足，失败但不泄漏
        }

        // 再分配：尝试 CAS 更新
        uint64_t new_off = current_off + aligned_len;
        if (__atomic_compare_exchange_n(&s.hdr->payload_alloc_off,
                                        &current_off, new_off,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            // CAS 成功，返回分配的起始偏移
            return current_off;
        }
        // CAS 失败，其他线程抢先了，重试
    }

    // 达到最大重试次数，竞争过于激烈
    return UINT64_MAX;
}

// insert key/value (writer holds writer_mutex)
// 串行化写入（进程共享 pthread 互斥）向哈希桶链表头插入一条新 KV，并用代数generation 实现读侧无锁一致性检测。
bool insert_kv(SharedShm& s, const uint8_t* key, size_t key_len, const uint8_t* val, size_t val_len) {
    // 输入验证：检查空指针和长度
    if (!key || !val) {
        return false; // 空指针，拒绝
    }
    if (key_len == 0 || key_len > MAX_KEY_LEN) {
        return false; // key 长度无效
    }
    if (val_len == 0 || val_len > MAX_VAL_LEN) {
        return false; // value 长度无效
    }

    // trivial lock for writers 加锁
    int lockRes = pthread_mutex_lock(&s.hdr->writer_mutex);
    if (lockRes == EOWNERDEAD) {
#ifdef __linux__
        // recover mutex state if writer died holding it - mark consistent
        pthread_mutex_consistent(&s.hdr->writer_mutex);
        // 记录恢复事件（可选：生产环境应记录日志）
#endif
    } else if (lockRes != 0) {
        // 锁获取失败，记录错误码
        // fprintf(stderr, "pthread_mutex_lock failed: %d\n", lockRes);
        return false;
    }

    // bump generation before modification (optional) generation++（标记写入开始）
    __atomic_fetch_add(&s.hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    uint64_t hash = simple_hash(key, key_len); // hash定位桶
    uint32_t bucket = (uint32_t)(hash % s.hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s.base + s.hdr->bucket_area_off) + bucket;

    // allocate payload 在 payload 段按"bump 指针"各分配key/val 空间并拷贝字节
    uint64_t key_payload_off = alloc_payload(s, key_len);
    uint64_t val_payload_off = alloc_payload(s, val_len);
    if (key_payload_off == UINT64_MAX || val_payload_off == UINT64_MAX) {
        // 分配失败，需要回滚 generation 避免读者看到不一致状态
        __atomic_fetch_add(&s.hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s.hdr->writer_mutex);
        return false;
    }

    // copy bytes
    uint8_t* payloadBase = s.payload_base();
    memcpy(payloadBase + key_payload_off, key, key_len);
    memcpy(payloadBase + val_payload_off, val, val_len);

    // allocate node 分配节点索引
    uint32_t node_idx = alloc_node_index(s);
    if (node_idx == EMPTY_INDEX) {
        // 节点容量耗尽，回滚 generation
        __atomic_fetch_add(&s.hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s.hdr->writer_mutex);
        return false;
    }

    Node* nodes = s.node_array();
    Node tmp{};
    tmp.key_off = (uint32_t)key_payload_off;
    tmp.key_len = (uint32_t)key_len;
    tmp.val_off = (uint32_t)val_payload_off;
    tmp.val_len = (uint32_t)val_len;
    tmp.next_index = EMPTY_INDEX;
    tmp.flags = 1; // active
    tmp.version = 1;

    // write node into node_array (we write entire struct) 写入Node
    nodes[node_idx] = tmp;

    // now insert node at bucket head (CAS loop) CAS将节点挂到对应桶头
    // 添加重试计数，防止死循环
    bool cas_success = false;
    for (uint32_t retries = 0; retries < MAX_CAS_RETRIES; ++retries) {
        uint32_t old_head = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST);
        nodes[node_idx].next_index = old_head;
        // perform CAS on bucket entry
        if (atomic_cas_u32(bucket_ptr, old_head, node_idx)) {
            cas_success = true;
            break;
        }
    }

    // CAS 失败检查
    if (!cas_success) {
        // 达到最大重试次数，放弃插入，回滚 generation
        __atomic_fetch_add(&s.hdr->generation, 1ULL, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&s.hdr->writer_mutex);
        return false;
    }

    // bump generation after modification (optional) generation++（发布新版本）
    __atomic_fetch_add(&s.hdr->generation, 1ULL, __ATOMIC_SEQ_CST);

    pthread_mutex_unlock(&s.hdr->writer_mutex); // 解锁
    return true;
}

// lookup (reader, without locking; uses generation to detect races)
bool lookup_kv(SharedShm& s, const uint8_t* key, size_t key_len, uint8_t* out_buf, size_t& out_len) {
    // 输入验证：检查空指针和长度
    if (!key || key_len == 0 || key_len > MAX_KEY_LEN) {
        return false;
    }

    uint64_t g1 = __atomic_load_n(&s.hdr->generation, __ATOMIC_SEQ_CST); // 读取generation记为g1

    uint64_t h = simple_hash(key, key_len);// 计算桶
    uint32_t bucket = (uint32_t)(h % s.hdr->n_buckets);
    uint32_t* bucket_ptr = reinterpret_cast<uint32_t*>((char*)s.base + s.hdr->bucket_area_off) + bucket;

    uint32_t idx = __atomic_load_n(bucket_ptr, __ATOMIC_SEQ_CST); // 原子读桶头索引
    Node* nodes = s.node_array();
    uint8_t* payloadBase = s.payload_base();
    uint64_t payload_capacity = s.hdr->total_size - s.hdr->payload_area_off;

    // 遍历列表：将节点拷贝到本地快照n，比较 key_len 与 payload 中的 key
    while (idx != EMPTY_INDEX) {
        // 边界检查：索引是否合法
        if (idx >= s.hdr->n_nodes) {
            return false; // 索引越界，数据损坏
        }

        Node n = nodes[idx]; // copy snapshot
        if (n.flags & 1) { // active
            if (n.key_len == key_len) {
                // 边界检查：key 偏移和长度是否在 payload 范围内
                if (n.key_off + n.key_len > payload_capacity) {
                    return false; // key 数据越界
                }

                if (memcmp(payloadBase + n.key_off, key, key_len) == 0) {
                    // 边界检查：value 偏移和长度是否在 payload 范围内
                    if (n.val_off + n.val_len > payload_capacity) {
                        return false; // value 数据越界
                    }

                    // found 找到即根据 out_buf/out_len 决定是否拷贝value
                    if (out_buf && out_len >= n.val_len) {
                        memcpy(out_buf, payloadBase + n.val_off, n.val_len);
                        out_len = n.val_len;
                    } else {
                        out_len = n.val_len;
                    }
                    uint64_t g2 = __atomic_load_n(&s.hdr->generation, __ATOMIC_SEQ_CST);
                    if (g1 == g2) {  // 若 g1==g2 则认为读取期间无并发写入，返回结果有效，否则返回false 让调用方重试。
                        return true;
                    } else {
                        // concurrent modification -> caller may retry 并发写入
                        return false;
                    }
                }
            }
        }
        idx = n.next_index;
    }
    uint64_t g2 = __atomic_load_n(&s.hdr->generation, __ATOMIC_SEQ_CST);// 再读一次generation记为g2
    if (g1 == g2) return false;  // 未找到
    // else indicates concurrent change - caller should retry 并发修改，建议重试
    return false;
}

// simple example usage
int main(int argc, char** argv) {
    const char* name = "/my_shm_test_1234"; // 程序固定使用共享内存名
    SharedShm s = create_or_open_shm(name); // 映射或初始化共享区

    if (argc >= 2 && strcmp(argv[1], "writer") == 0) {
        // a sample writer 若为writer，插入键 "hello" 值 "world"，打印是否成功
        const char* k = "hello";
        const char* v = "world";
        if (insert_kv(s, (const uint8_t*)k, strlen(k), (const uint8_t*)v, strlen(v))) {
            std::cout << "writer: inserted\n";
        } else {
            std::cout << "writer: insert failed\n";
        }
    } else {
        // reader 作为 reader查询 "hello"，若命中打印长度与内容，否则提示未找到或发生并发写入
        uint8_t buf[256];
        size_t buflen = sizeof(buf);
        const char* k = "hello";
        bool ok = lookup_kv(s, (const uint8_t*)k, strlen(k), buf, buflen);
        if (ok && buflen>0) {
            std::cout << "reader: found val len=" << buflen << " val=" << std::string((char*)buf, buflen) << "\n";
        } else {
            std::cout << "reader: not found or concurrent modification\n";
        }
    }

    close_shm(s); // 释放映射与文件描述符
    return 0;
}
