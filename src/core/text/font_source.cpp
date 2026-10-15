#include "core/text/font_source.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace heditor {
namespace {

uint16_t readBE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t readBE32(const uint8_t* p, size_t at) {
    return (static_cast<uint32_t>(p[at]) << 24) | (static_cast<uint32_t>(p[at + 1]) << 16) |
           (static_cast<uint32_t>(p[at + 2]) << 8) | static_cast<uint32_t>(p[at + 3]);
}

void writeBE16(uint8_t* p, size_t at, uint16_t v) {
    p[at] = static_cast<uint8_t>(v >> 8);
    p[at + 1] = static_cast<uint8_t>(v);
}

void writeBE32(uint8_t* p, size_t at, uint32_t v) {
    p[at] = static_cast<uint8_t>(v >> 24);
    p[at + 1] = static_cast<uint8_t>(v >> 16);
    p[at + 2] = static_cast<uint8_t>(v >> 8);
    p[at + 3] = static_cast<uint8_t>(v);
}

/**
 * 表数据的校验和：按大端四字节字累加取低 32 位（OpenType 规范定义）。
 * 尾数不足四字节时右侧补零。
 */
uint32_t tableChecksum(const uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = 0;
        for (size_t k = 0; k < 4; ++k) {
            v <<= 8;
            if (i + k < n) v |= p[i + k];
        }
        sum += v;
    }
    return sum;
}

}  // namespace

uint32_t SfntDirectory::tagOf(const char* s4) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(s4[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s4[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s4[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(s4[3]));
}

bool SfntDirectory::parse(const uint8_t* header, size_t n) {
    if (header == nullptr || n < 12) return false;
    const uint32_t version = readBE32(header, 0);
    // 0x00010000（TrueType）、OTTO（CFF 轮廓）、true（旧 Apple）都接受。
    //   ttcf（字体集合）不支持：本实现的每张脸都是一个独立文件。
    if (version != 0x00010000u && version != tagOf("OTTO") && version != tagOf("true")) {
        return false;
    }
    const uint16_t num = readBE16(header + 4);
    if (num == 0 || n < 12u + static_cast<size_t>(num) * 16u) return false;

    tables_.clear();
    tables_.reserve(num);
    for (uint16_t i = 0; i < num; ++i) {
        const uint8_t* e = header + 12u + static_cast<size_t>(i) * 16u;
        SfntTable t;
        t.tag = readBE32(e, 0);
        t.offset = readBE32(e, 8);
        t.length = readBE32(e, 12);
        tables_.push_back(t);
    }
    version_ = version;
    return true;
}

const SfntTable* SfntDirectory::find(uint32_t tag) const {
    // 线性扫描即可：一份字体只有二十几张表。做成有序数组加二分是过早优化。
    for (const SfntTable& t : tables_) {
        if (t.tag == tag) return &t;
    }
    return nullptr;
}

FontStorage::~FontStorage() {
    if (file_ != nullptr) std::fclose(file_);
}

bool FontStorage::open(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    const long end = std::ftell(f);
    if (end <= 0) {
        std::fclose(f);
        return false;
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    file_ = f;
    size_ = static_cast<uint64_t>(end);
    return true;
}

FontRead FontStorage::readAt(uint64_t off, void* dst, size_t len, size_t* got) const {
    if (got != nullptr) *got = 0;
    if (file_ == nullptr || dst == nullptr) return FontRead::Error;
    if (len == 0) return FontRead::Ok;
    if (off >= size_) return FontRead::Eof;  // 起点已在文件尾：零字节可读，不是错误
    // 先把请求夹到可读范围里。夹过之后还短读，就只能是文件被截断或 I/O 出错 ——
    //   这就是"文件尾触发的 fread 读不满"不会被当成失败的原因。
    const bool clamped = len > size_ - off;
    const size_t want = clamped ? static_cast<size_t>(size_ - off) : len;
    if (std::fseek(file_, static_cast<long>(off), SEEK_SET) != 0) return FontRead::Error;
    const size_t n = std::fread(dst, 1, want, file_);
    if (got != nullptr) *got = n;
    if (n != want) return FontRead::Error;
    return clamped ? FontRead::Eof : FontRead::Ok;
}

FontBlockCache::FontBlockCache(size_t block_size, size_t max_blocks)
    : blocks_(max_blocks > 0 ? max_blocks : 1u),  // 零块的缓存会让每次读都失败
      block_size_(block_size > 0 ? block_size : 1u) {}

FontRead FontBlockCache::read(const FontStorage& src, uint64_t off, void* dst, size_t len,
                              size_t* got) {
    if (got != nullptr) *got = 0;
    if (dst == nullptr) {
        ++read_failures_;
        return FontRead::Error;
    }
    if (len == 0) return FontRead::Ok;
    const uint64_t total = src.size();
    if (off >= total) {
        ++eof_reads_;
        return FontRead::Eof;
    }
    // 与存储层同一套口径：先夹到可读范围，夹过之后还读不满才算失败。
    const bool clamped = len > total - off;
    const size_t want_total = clamped ? static_cast<size_t>(total - off) : len;

    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < want_total) {
        const uint64_t cur = off + done;
        const uint64_t id = cur / block_size_;
        const size_t within = static_cast<size_t>(cur % block_size_);
        const size_t want = std::min(want_total - done, block_size_ - within);
        const Block* b = fetch(src, id);
        // 块取不到、或块比请求的区间短（文件在装入之后被截断），都是真失败。
        if (b == nullptr || within >= b->data.size() || b->data.size() - within < want) {
            ++read_failures_;
            if (got != nullptr) *got = done;
            return FontRead::Error;
        }
        std::memcpy(out + done, b->data.data() + within, want);
        done += want;
    }
    if (got != nullptr) *got = done;
    if (clamped) {
        ++eof_reads_;
        return FontRead::Eof;
    }
    return FontRead::Ok;
}

FontBlockCache::Block* FontBlockCache::fetch(const FontStorage& src, uint64_t id) {
    for (Block& b : blocks_) {
        if (b.used && b.id == id) {
            b.stamp = ++clock_;
            ++hits_;
            return &b;
        }
    }
    ++misses_;
    Block* victim = nullptr;
    for (Block& b : blocks_) {
        if (!b.used) {
            victim = &b;
            break;
        }
        if (victim == nullptr || b.stamp < victim->stamp) victim = &b;
    }
    if (victim == nullptr) return nullptr;  // 块数为零

    const uint64_t begin = id * block_size_;
    const uint64_t total = src.size();
    if (begin >= total) return nullptr;
    const size_t n =
        static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(block_size_), total - begin));
    victim->data.resize(n);
    const auto t0 = std::chrono::steady_clock::now();
    if (src.readAt(begin, victim->data.data(), n, nullptr) != FontRead::Ok) {
        victim->used = false;
        return nullptr;
    }
    const auto t1 = std::chrono::steady_clock::now();
    io_ms_ += std::chrono::duration<double, std::milli>(t1 - t0).count();
    ++storage_reads_;
    storage_bytes_ += n;
    victim->id = id;
    victim->used = true;
    victim->stamp = ++clock_;
    return victim;
}

void FontBlockCache::clear() {
    for (Block& b : blocks_) b.used = false;
}

void FontBlockCache::configure(size_t block_size, size_t max_blocks) {
    blocks_.assign(max_blocks > 0 ? max_blocks : 1u, Block{});
    block_size_ = block_size > 0 ? block_size : 1u;
    clock_ = 0;
    resetStats();
}

void FontBlockCache::resetStats() {
    hits_ = 0;
    misses_ = 0;
    storage_reads_ = 0;
    storage_bytes_ = 0;
    read_failures_ = 0;
    eof_reads_ = 0;
    io_ms_ = 0.0;
}

size_t FontBlockCache::residentBytes() const {
    size_t n = 0;
    for (const Block& b : blocks_) {
        if (b.used) n += b.data.size();
    }
    return n;
}

bool FontSource::open(const std::string& path) {
    if (!storage_.open(path)) return false;
    path_ = path;

    // 表目录在文件最前面：先读 12 字节头拿到表数，再把目录整段读进来。
    //   这两次读刻意不走块缓存 —— 它们是装载期的元数据，不该算进按需读的账。
    uint8_t header[12];
    if (storage_.readAt(0, header, sizeof(header)) != FontRead::Ok) return false;
    const uint16_t num = readBE16(header + 4);
    if (num == 0) return false;
    const size_t need = 12u + static_cast<size_t>(num) * 16u;
    std::vector<uint8_t> raw(need);
    // 表目录声明了 num 张表，文件里就得真有那么多目录项 —— 少一个字节都是坏文件。
    if (storage_.readAt(0, raw.data(), need) != FontRead::Ok) return false;
    return dir_.parse(raw.data(), raw.size());
}

bool FontSource::inSkeleton(uint32_t tag) {
    // 排除的是两类：轮廓与变体数据（按字形可寻址，正是按需要省的那部分），
    //   以及位图与彩色字形表（本实现不用）。其余一张不落地收进骨架 ——
    //   这个白名单式的判据故意写成黑名单，宁可多收一张小表，也不冒险漏掉
    //   HarfBuzz 排版要读的东西。
    switch (tag) {
        case 0x676C7966u:  // glyf
        case 0x67766172u:  // gvar
        case 0x6C6F6361u:  // loca
        case 0x43464620u:  // CFF
        case 0x43464632u:  // CFF2
        case 0x73626978u:  // sbix
        case 0x43424454u:  // CBDT
        case 0x43424C43u:  // CBLC
        case 0x45424454u:  // EBDT
        case 0x45424C43u:  // EBLC
        case 0x45425343u:  // EBSC
        case 0x434F4C52u:  // COLR
        case 0x4350414Cu:  // CPAL
        case 0x53564720u:  // SVG
        case 0x564F5247u:  // VORG
            return false;
        default:
            return true;
    }
}

bool FontSource::buildSkeleton(std::vector<uint8_t>* out) {
    if (out == nullptr || dir_.count() == 0) return false;

    std::vector<const SfntTable*> keep;
    for (const SfntTable& t : dir_.tables()) {
        if (inSkeleton(t.tag) && t.length > 0) keep.push_back(&t);
    }
    if (keep.empty()) return false;
    // 目录按 tag 升序：规范如此建议，也让骨架文件肉眼可比对。
    std::sort(keep.begin(), keep.end(),
              [](const SfntTable* a, const SfntTable* b) { return a->tag < b->tag; });

    const size_t n = keep.size();
    std::vector<uint32_t> new_off(n, 0);
    size_t cursor = 12u + n * 16u;
    for (size_t i = 0; i < n; ++i) {
        cursor = (cursor + 3u) & ~static_cast<size_t>(3u);  // 每张表按四字节对齐
        new_off[i] = static_cast<uint32_t>(cursor);
        cursor += keep[i]->length;
    }
    out->assign(cursor, 0);

    uint8_t* base = out->data();
    writeBE32(base, 0, dir_.version());
    writeBE16(base, 4, static_cast<uint16_t>(n));
    uint16_t selector = 0;
    while ((static_cast<size_t>(1) << (selector + 1)) <= n) ++selector;
    const uint16_t search_range = static_cast<uint16_t>((static_cast<size_t>(1) << selector) * 16u);
    writeBE16(base, 6, search_range);
    writeBE16(base, 8, selector);
    writeBE16(base, 10, static_cast<uint16_t>(n * 16u - search_range));

    std::vector<uint8_t> raw;
    for (size_t i = 0; i < n; ++i) {
        const SfntTable& t = *keep[i];
        raw.resize(t.length);
        // 目录里的表长必须落在文件里；缺了就当这份骨架拼不成（别把半张表拼进去）。
        if (readAt(t.offset, raw.data(), t.length) != FontRead::Ok) return false;
        std::memcpy(base + new_off[i], raw.data(), t.length);

        uint8_t* e = base + 12u + i * 16u;
        writeBE32(e, 0, t.tag);
        writeBE32(e, 4, tableChecksum(base + new_off[i], t.length));
        writeBE32(e, 8, new_off[i]);
        writeBE32(e, 12, t.length);
    }
    // head 表里的 checkSumAdjustment 保持原值：它本是"整个文件的校验和修正量"，
    //   重排之后严格说不再成立。HarfBuzz 与 FreeType 都不校验它，实测也无差异；
    //   将来若要产出可被字体工具校验的骨架，再补这一步。
    return true;
}

}  // namespace heditor
