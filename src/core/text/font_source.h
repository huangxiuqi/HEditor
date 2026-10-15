/**
 * 字体按需数据源：表目录、块缓存、骨架拼装。
 * 一份中日韩字体的体积几乎全在 glyf 与 gvar 这两张按字形可寻址的大表上（实测
 * NotoSansSC-VF：16.95 MB 里 glyf 占 57.1%、gvar 占 39.8%），而必须整表常驻的
 * （head/hhea/maxp/hmtx/cmap/GSUB/GPOS 等）合计不到 1 MB。于是把它拆成两条路：
 *   给 HarfBuzz 一张只含小表的骨架（排版要读的表一张不少），
 *   给 Skia 的 FreeType 后端一条按需流（轮廓与变体 delta 用到哪块读哪块）。
 * 两条路经过同一个块缓存，命中率与落存储的次数、字节因此可以一起统计。
 * 存储层只有一种实现：一个只读文件 —— native 上是磁盘文件，wasm 上是 MEMFS，
 * 两边都用 std::FILE，调用方不必区分。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace heditor {

/**
 * SFNT 表目录里的一项。
 */
struct SfntTable {
    uint32_t tag = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
};

/**
 * 表目录：从文件头解析一次（12 字节头加每表 16 字节目录项）。
 *
 * 只吃文件开头那一小段，不碰表数据 —— 这是按需读的第一步：有了目录之后，
 * 任意一张表的位置都能 O(log n) 查到，不必先知道字体有多大。
 * 不支持 TTC（字体集合）：本实现的每张脸都是一个独立文件。
 */
class SfntDirectory {
public:
    bool parse(const uint8_t* header, size_t n);
    const SfntTable* find(uint32_t tag) const;
    size_t count() const { return tables_.size(); }
    const std::vector<SfntTable>& tables() const { return tables_; }
    uint32_t version() const { return version_; }
    /**
     * 把四字节 tag 拼成 uint32（大端序，与文件里的表示一致）。
     */
    static uint32_t tagOf(const char* s4);

private:
    std::vector<SfntTable> tables_;
    uint32_t version_ = 0;
};

/**
 * 一次存储读的结果。
 *
 * 请求越过文件尾与读失败是两件事：流契约允许短读，越过文件尾只是正常的终止条件，
 * 不是错误；定位失败、落盘出错、文件在被打开之后被截断才是错误。两者混在一个
 * bool 里时，"字体读不到"在页面上表现为"某些字不出现"，而从统计里看不出来 ——
 * 这就是要把它们分成两个状态的理由，也是 eofReads 与 readFailures 分开计的原因。
 */
enum class FontRead {
    Ok,
    Eof,
    Error,
};

/**
 * 存储层：一个只读文件。
 *
 * readAt 是同步的，因为它服务的上游（FreeType 的 FT_Stream）就是同步契约。
 * 偏移用 uint64，但底层走 std::fseek —— 字体文件不会超过 long 能表示的范围。
 */
class FontStorage {
public:
    ~FontStorage();
    FontStorage() = default;
    FontStorage(const FontStorage&) = delete;
    FontStorage& operator=(const FontStorage&) = delete;

    bool open(const std::string& path);
    /**
     * 读一段字节。请求越过文件尾时先夹到可读范围，交出能读的那部分并返回 Eof；
     * 只有夹过之后仍然短读（文件被截断、I/O 出错）才返回 Error。
     * got 可选，写回实际读到的字节数 —— 返回 Eof 时它与 len 的差就是缺的字节。
     */
    FontRead readAt(uint64_t off, void* dst, size_t len, size_t* got = nullptr) const;
    uint64_t size() const { return size_; }
    bool valid() const { return file_ != nullptr; }

private:
    std::FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

/**
 * 块缓存：把随机小读变成块级读，并统计命中与落存储的量。
 *
 * 常驻上限就是块数乘块大小；落存储的次数与字节是判断按需是否划算的两个数，
 * 所以必须由这里数出来而不是估。替换策略是最近最少使用（线性扫描选最旧的
 * 一块）—— 块数只有几十，扫描比维护链表更省事也更快。
 */
class FontBlockCache {
public:
    /**
     * 块大小与块数决定常驻上限。默认 4 KB x 1024 块（上限 4 MB）。
     *
     * 依据（同一窗口只换配置，两种文本各测一遍；读入字节）：合成文本 4 KB 1.34 MB、
     * 16 KB 2.57 MB、64 KB 8.71 MB；真病历 4 KB 3.05 MB、16 KB 15.68 MB、64 KB 88.32 MB
     * —— 后两档都顶到常驻上限反复淘汰重读，而 IO 时间在两种文本下都是 1 ms 量级。
     * 原因是字形在 glyf 里按字形号连续、而一段文本用到的字形号是散的：块越大，
     * 一次读进来用不到的部分越多。真文本的工作集约 3 MB，上限取 4 MB 留余量。
     * 切到真磁盘后这张表要重测。
     */
    FontBlockCache(size_t block_size = 4u * 1024u, size_t max_blocks = 1024u);

    /**
     * 带缓存的读。语义与 FontStorage::readAt 一致：越过文件尾返回 Eof（不是错误），
     * 并且把 eofReads 与 readFailures 分开累计。
     */
    FontRead read(const FontStorage& src, uint64_t off, void* dst, size_t len,
                  size_t* got = nullptr);
    void clear();
    void resetStats();
    /**
     * 换一套缓存配置（块大小与块数）并清空已有块与统计。
     *
     * 存在的理由是一个一直没有依据的参数：块大小按"减少读的次数"这一个目标取，
     * 没有量过它的代价 —— 一次读进来的字节里有多少是用不到的。
     * 块越大，落存储的次数越少，但一次读进来的字节里用不到的部分越多 —— 字形在 glyf 里
     * 按字形号连续，而一段文本用到的字形号是散的，所以块读的空间效率取决于块有多大。
     * 要在这两者之间挑，就得能改它、并且量出来。
     */
    void configure(size_t block_size, size_t max_blocks);
    /**
     * 当前常驻的字节数（已装入的块之和），不是上限。
     */
    size_t residentBytes() const;

    size_t hits() const { return hits_; }
    size_t misses() const { return misses_; }
    size_t storageReads() const { return storage_reads_; }
    size_t storageBytes() const { return storage_bytes_; }
    double ioMs() const { return io_ms_; }
    size_t blockSize() const { return block_size_; }
    /**
     * 常驻上限（块大小乘块数）。residentBytes 是当前实际装入的量，不会超过它。
     */
    size_t capacityBytes() const { return block_size_ * blocks_.size(); }
    /**
     * 真失败的次数。文件尾的短读不计在这里，见 eofReads。
     */
    size_t readFailures() const { return read_failures_; }
    /**
     * 因请求越过文件尾而短读的次数。这是正常终止，不是失败。
     */
    size_t eofReads() const { return eof_reads_; }

private:
    struct Block {
        std::vector<uint8_t> data;
        uint64_t id = 0;
        uint64_t stamp = 0;
        bool used = false;
    };
    Block* fetch(const FontStorage& src, uint64_t block_id);

    std::vector<Block> blocks_;
    size_t block_size_ = 0;
    uint64_t clock_ = 0;
    size_t hits_ = 0;
    size_t misses_ = 0;
    size_t storage_reads_ = 0;
    size_t storage_bytes_ = 0;
    size_t read_failures_ = 0;
    size_t eof_reads_ = 0;
    double io_ms_ = 0.0;
};

/**
 * 一个字体的按需数据源：表目录加块缓存，外加一张给 HarfBuzz 的骨架。
 *
 * 骨架是重新拼出来的一份合法 SFNT，只收小表（大表与位图表被排除，见
 * inSkeleton）。它让 HarfBuzz 不必持有整份字体字节 —— 排版要用的表一张不少，
 * 而体积从十几 MB 降到不足 1 MB。Skia 那条路则完全不经过骨架，直接用
 * readAt 按需取它自己要的表与字形。
 */
class FontSource {
public:
    /**
     * 打开文件并解析表目录（只读开头一小段）。
     */
    bool open(const std::string& path);

    const std::string& path() const { return path_; }
    uint64_t size() const { return storage_.size(); }
    const SfntDirectory& dir() const { return dir_; }
    const FontBlockCache& cache() const { return cache_; }

    /**
     * 按需读（经过块缓存）。上游要同步契约，所以这里也是同步的。
     */
    FontRead readAt(uint64_t off, void* dst, size_t len, size_t* got = nullptr) {
        return cache_.read(storage_, off, dst, len, got);
    }

    /**
     * 转发给块缓存的配置入口（给"块大小该取多少"的实验用）。
     */
    void configureCache(size_t block_size, size_t max_blocks) {
        cache_.configure(block_size, max_blocks);
    }

    /**
     * 拼一张只含小表的骨架字体。会经过块缓存，因此会改动统计。
     */
    bool buildSkeleton(std::vector<uint8_t>* out);

    /**
     * 某张表是否收进骨架。排除的是大表与位图/彩色字形表。
     */
    static bool inSkeleton(uint32_t tag);

private:
    std::string path_;
    FontStorage storage_;
    SfntDirectory dir_;
    FontBlockCache cache_;
};

}  // namespace heditor
