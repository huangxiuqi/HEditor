/**
 * 不可变修订的骨架：节点、路径复制、内容哈希。
 *
 * 判据只有一条：一次编辑之后，旧值仍然可用、且没有被改动。做到这一点之后，"改了什么"
 * 就不再需要事后推导 —— 两份修订的哈希一比，不同的子树就是损伤，而且精确到节点。
 *
 * 切片范围：文字、段落标记、内容控件、表格（表 / 行 / 格）、图片、公式、域、节的页面设置。
 * 页眉页脚的内容（HeaderFooter 子树）按容器下钻，本层不解释它。
 * 收窄不是留占位 —— 这一层表示不了的形状根本构造不出来，makeTextDoc 只产出前两类，
 * 控件只能经 insertControl 建出来。加新节点种类时 payload 的 variant 多一支，
 * 哈希用 std::visit 且不写默认分支，漏写一支直接编不过，不会静默算出一个不完整的哈希。
 *
 * 编辑入口认的是流位置，不看层级：控件内部的文字与顶层文字是同一段文字的两部分，所以把字打在
 * 控件里与打在同一段的别处走同一条路 —— 落点带一条从节往下的路径，改动沿路径换回去。
 * 读（paraTexts / paraCount）同样按流顺序下钻，与编译同一口径。
 *
 * 身份（NodeId）不进哈希：哈希表达内容，身份表达"跨修订的同一性"。撤销回到旧修订
 * 时内容哈希必须相等，而节点身份可以一路往前走。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/field_spec.h"
#include "core/doc/flow_types.h"
#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/node_id.h"
#include "core/doc/node_kind.h"
#include "core/doc/para_format.h"
#include "core/doc/section_spec.h"
#include "core/doc/subtree.h"

namespace heditor::doc {
/**
 * 一段码点文本。
 *
 * 底层是一份共享的只读码点块加一个窗口（offset、size）：段落切片只复制窗口，
 * 不复制码点。写入走写时复制的容器方法，所以按值传给排版是廉价的。
 */
class Text {
public:

    /**
     * 只读/可写都用的随机访问迭代器：只记位置，改文本走容器方法。
     */
    class const_iterator {
    public:

        using iterator_category = std::random_access_iterator_tag;
        using value_type = uint32_t;
        using difference_type = std::ptrdiff_t;
        using pointer = const uint32_t*;
        using reference = uint32_t;

        /**
         * 构造。
         * @param owner 文本
         * @param index 码点下标
         */
        const_iterator(const Text* owner, size_t index) : owner_(owner), index_(index) {}

        /**
         * 解引用。
         * @returns 当前码点
         */
        uint32_t operator*() const { return (*owner_)[index_]; }

        /**
         * 前进。
         * @returns 自身
         */
        const_iterator& operator++() {
            ++index_;
            return *this;
        }

        /**
         * 后退。
         * @returns 自身
         */
        const_iterator& operator--() {
            --index_;
            return *this;
        }

        /**
         * 前进 n 步。
         * @param n 步数
         * @returns 新位置
         */
        const_iterator operator+(difference_type n) const {
            return const_iterator(owner_,
                                  static_cast<size_t>(static_cast<difference_type>(index_) + n));
        }

        /**
         * 前进 n 步（复合赋值）。
         * @param n 步数
         * @returns 自身
         */
        const_iterator& operator+=(difference_type n) {
            index_ = static_cast<size_t>(static_cast<difference_type>(index_) + n);
            return *this;
        }

        /**
         * 后退 n 步（复合赋值）。
         * @param n 步数
         * @returns 自身
         */
        const_iterator& operator-=(difference_type n) {
            index_ = static_cast<size_t>(static_cast<difference_type>(index_) - n);
            return *this;
        }

        /**
         * 下标访问。
         * @param n 偏移
         * @returns 那个码点
         */
        uint32_t operator[](difference_type n) const {
            return (*owner_)[static_cast<size_t>(static_cast<difference_type>(index_) + n)];
        }

        /**
         * 后退 n 步。
         * @param n 步数
         * @returns 新位置
         */
        const_iterator operator-(difference_type n) const {
            return const_iterator(owner_,
                                  static_cast<size_t>(static_cast<difference_type>(index_) - n));
        }

        /**
         * 两位置之差。
         * @param o 另一位置
         * @returns 码点距离
         */
        difference_type operator-(const const_iterator& o) const {
            return static_cast<difference_type>(index_) - static_cast<difference_type>(o.index_);
        }

        /**
         * 比较。
         * @param o 另一位置
         * @returns true = 同一位置
         */
        bool operator==(const const_iterator& o) const {
            return owner_ == o.owner_ && index_ == o.index_;
        }

        /**
         * 不等。
         * @param o 另一位置
         * @returns true = 不同位置
         */
        bool operator!=(const const_iterator& o) const { return !(*this == o); }

        /**
         * 下标。
         * @returns 码点下标
         */
        size_t index() const { return index_; }

    private:

        /**
         * 所属文本。
         */
        const Text* owner_ = nullptr;

        /**
         * 码点下标。
         */
        size_t index_ = 0;
    };

    /**
     * 迭代器类型。
     */
    using iterator = const_iterator;

    /**
     * 构造空文本。
     */
    Text() = default;

    /**
     * 从连续数组构造，拷贝进共享缓冲区。
     * @param text 码点
     */
    Text(std::vector<uint32_t> text) { assignCopy(std::move(text)); }

    /**
     * 从初始化列表构造。
     * @param text 码点
     */
    Text(std::initializer_list<uint32_t> text) { assignCopy(std::vector<uint32_t>(text)); }

    /**
     * 码点数。
     * @returns 码点数
     */
    size_t size() const { return size_; }

    /**
     * 是否为空。
     * @returns true = 没有码点
     */
    bool empty() const { return size_ == 0; }

    /**
     * 按下标取码点。
     * @param index 下标
     * @returns 码点
     */
    uint32_t operator[](size_t index) const { return (*buffer_)[offset_ + index]; }

    /**
     * 起点。
     * @returns 起点
     */
    const_iterator begin() const { return const_iterator(this, 0); }

    /**
     * 终点。
     * @returns 终点
     */
    const_iterator end() const { return const_iterator(this, size_); }

    /**
     * 连续码点指针；空文本返回 nullptr。
     * @returns 码点指针
     */
    const uint32_t* data() const {
        return buffer_ == nullptr ? nullptr : buffer_->data() + offset_;
    }

    /**
     * 与另一段文本是否共享缓冲区。
     * @param o 另一段文本
     * @returns true = 共享同一缓冲区
     */
    bool sharesBufferWith(const Text& o) const { return buffer_ == o.buffer_; }

    /**
     * 从另一段文本切出一段，共享同一缓冲区。
     * @param source 源文本
     * @param offset 起点
     * @param size 码点数
     * @returns 切片
     */
    static Text slice(const Text& source, size_t offset, size_t size) {
        Text out;
        out.buffer_ = source.buffer_;
        out.offset_ = source.offset_ + offset;
        out.size_ = size;
        return out;
    } /**
     * 复制成连续数组。
     * @returns 码点副本
     */
    std::vector<uint32_t> toVector() const {
        if (buffer_ == nullptr) return {};
        return std::vector<uint32_t>(
            buffer_->begin() + static_cast<std::ptrdiff_t>(offset_),
            buffer_->begin() + static_cast<std::ptrdiff_t>(offset_ + size_));
    }

    /**
     * 插入一段码点。
     * @param pos 插入位置
     * @param first 码点起点
     * @param last 码点终点
     * @returns 插入位置
     */
    template <typename It>
    iterator insert(iterator pos, It first, It last) {
        std::vector<uint32_t> out = toVector();
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(pos.index()), first, last);
        const size_t at = pos.index();
        assignCopy(std::move(out));
        return const_iterator(this, at);
    }

    /**
     * 删除一段码点。
     * @param first 起点
     * @param last 终点
     * @returns 删除之后的位置
     */
    iterator erase(iterator first, iterator last) {
        std::vector<uint32_t> out = toVector();
        const size_t at = first.index();
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(first.index()),
                  out.begin() + static_cast<std::ptrdiff_t>(last.index()));
        assignCopy(std::move(out));
        return const_iterator(this, at);
    }

    /**
     * 换成一段码点。
     * @param first 码点起点
     * @param last 码点终点
     */
    template <typename It>
    void assign(It first, It last) {
        std::vector<uint32_t> out(first, last);
        assignCopy(std::move(out));
    }

    /**
     * 用重复码点填满。
     * @param count 码点数
     * @param value 码点
     */
    void assign(size_t count, uint32_t value) { assignCopy(std::vector<uint32_t>(count, value)); }

    /**
     * 清空。
     */
    void clear() {
        buffer_.reset();
        offset_ = 0;
        size_ = 0;
    }

    /**
     * 追加一个码点。
     * @param cp 码点
     */
    void push_back(uint32_t cp) {
        std::vector<uint32_t> out = toVector();
        out.push_back(cp);
        assignCopy(std::move(out));
    }

    /**
     * 预留容量；切片实现下不预留。
     * @param n 码点数
     */
    void reserve(size_t n) { (void)n; }

    /**
     * 从连续数组赋值。
     * @param text 码点
     * @returns 自身
     */
    Text& operator=(std::vector<uint32_t> text) {
        assignCopy(std::move(text));
        return *this;
    }

    /**
     * 从初始化列表赋值。
     * @param text 码点
     * @returns 自身
     */
    Text& operator=(std::initializer_list<uint32_t> text) {
        assignCopy(std::vector<uint32_t>(text));
        return *this;
    }

    /**
     * 隐式转成连续数组；给仍按 vector 接收的旧接口用。
     * @returns 码点副本
     */
    operator std::vector<uint32_t>() const { return toVector(); }

    /**
     * 与另一段文本比较。
     * @param o 另一段文本
     * @returns true = 内容相同
     */
    bool operator==(const Text& o) const {
        if (o.size() != size_) return false;
        for (size_t i = 0; i < size_; ++i) {
            if ((*this)[i] != o[i]) return false;
        }
        return true;
    }

    /**
     * 与连续数组比较。
     * @param o 另一段码点
     * @returns true = 内容相同
     */
    bool operator==(const std::vector<uint32_t>& o) const {
        if (o.size() != size_) return false;
        for (size_t i = 0; i < size_; ++i) {
            if ((*this)[i] != o[i]) return false;
        }
        return true;
    }

private:

    /**
     * 共享不可变缓冲区。
     */
    std::shared_ptr<const std::vector<uint32_t>> buffer_;

    /**
     * 本段在缓冲区里的起点。
     */
    size_t offset_ = 0;

    /**
     * 本段码点数。
     */
    size_t size_ = 0;

    /**
     * 换成一份独占缓冲区。
     * @param text 码点
     */
    void assignCopy(std::vector<uint32_t> text) {
        size_ = text.size();
        offset_ = 0;
        buffer_ = std::make_shared<const std::vector<uint32_t>>(std::move(text));
    }
};
/**
 * 文本 run 的载荷：码点、字符属性、字符样式引用。
 */
struct RunPayload {
    /**
     * 码点序列（偏移就是下标）。
     */
    Text text;

    /**
     * 字符属性（标记）。
     */
    CharFormat marks;

    /**
     * 字符样式引用；空表示不引用。
     */
    std::string style;
};

/**
 * 段落标记的载荷：段落属性的内联覆盖与段落样式引用（第三节：段落属性挂在结束标记上）。
 */
struct MarkPayload {
    /**
     * 段落属性的内联覆盖（只存与样式不同的那些）。
     */
    ParaFormatPatch para;

    /**
     * 段落样式引用；空表示不引用。
     */
    std::string style;

    /**
     * 段落标记自己的字符格式。
     *
     * 空段落里没有文本节点，字符格式无处可挂 —— 选中一个空单元格再改字重时，能落笔的
     * 只有这一枚标记。它同时是"接着敲的字用什么格式"的来源（见 marksAtFlow）。
     * 有文字的段落也带着它，但那一段的文本各用自己的标记。
     */
    CharFormat marks;
};

struct Node;
using Ref = std::shared_ptr<const Node>;

/**
 * 只读孩子表：按 64 个一份的共享块存放，整块可以跨修订复用。
 *
 * 编辑算法按孩子下标寻址，下标换算与块边界在这一层之内 —— 调用方看不到分块。
 * 路径复制只复制受影响的那条路径，未受影响的块与上一次修订共用同一份内存。
 */
class Children {
public:

    /**
     * 迭代器：只读、双向；按块跳转。
     */
    class const_iterator {
    public:

        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = Ref;
        using difference_type = std::ptrdiff_t;
        using pointer = const Ref*;
        using reference = const Ref&;

        /**
         * 构造。
         * @param owner 孩子表
         * @param index 全局下标
         */
        const_iterator(const Children* owner, size_t index) : owner_(owner), index_(index) {}

        /**
         * 解引用。
         * @returns 当前孩子
         */
        const Ref& operator*() const { return (*owner_)[index_]; }

        /**
         * 成员访问。
         * @returns 当前孩子指针
         */
        const Ref* operator->() const { return &(*owner_)[index_]; }

        /**
         * 前进一格。
         * @returns 自身
         */
        const_iterator& operator++() {
            ++index_;
            return *this;
        }

        /**
         * 后退一格。
         * @returns 自身
         */
        const_iterator& operator--() {
            --index_;
            return *this;
        }

        /**
         * 比较。
         * @param o 另一个迭代器
         * @returns true = 同一位置
         */
        bool operator==(const const_iterator& o) const {
            return owner_ == o.owner_ && index_ == o.index_;
        }

        /**
         * 不等。
         * @param o 另一个迭代器
         * @returns true = 不同位置
         */
        bool operator!=(const const_iterator& o) const { return !(*this == o); }

    private:

        /**
         * 所属孩子表。
         */
        const Children* owner_ = nullptr;

        /**
         * 全局下标。
         */
        size_t index_ = 0;
    };

    /**
     * 每个块的孩子数。
     */
    static constexpr size_t kChunkSize = 64;

    /**
     * 构造空表。
     */
    Children() = default;

    /**
     * 从连续数组构造，按固定大小切块。
     * @param items 孩子
     */
    explicit Children(std::vector<Ref> items) {
        size_ = items.size();
        chunks_.reserve((size_ + kChunkSize - 1) / kChunkSize);
        for (size_t at = 0; at < size_; at += kChunkSize) {
            const size_t n = std::min(kChunkSize, size_ - at);
            chunks_.push_back(std::make_shared<const std::vector<Ref>>(
                items.begin() + static_cast<std::ptrdiff_t>(at),
                items.begin() + static_cast<std::ptrdiff_t>(at + n)));
        }
        recomputeAll();
    }

    /**
     * 孩子数。
     * @returns 孩子数
     */
    size_t size() const { return size_; }

    /**
     * 是否为空。
     * @returns true = 没有孩子
     */
    bool empty() const { return size_ == 0; }

    /**
     * 按下标取孩子。
     * @param index 下标
     * @returns 那个孩子
     */
    const Ref& operator[](size_t index) const {
        return (*chunks_[index / kChunkSize])[index % kChunkSize];
    }

    /**
     * 第一个孩子。
     * @returns 第一个孩子
     */
    const Ref& front() const { return (*chunks_.front())[0]; }

    /**
     * 最后一个孩子。
     * @returns 最后一个孩子
     */
    const Ref& back() const {
        const std::vector<Ref>& last = *chunks_.back();
        return last.back();
    }

    /**
     * 只读起点。
     * @returns 起点迭代器
     */
    const_iterator begin() const { return const_iterator(this, 0); }

    /**
     * 只读终点。
     * @returns 终点迭代器
     */
    const_iterator end() const { return const_iterator(this, size_); }

    /**
     * 孩子的聚合哈希低位。
     * @returns 聚合哈希低位
     */
    uint64_t aggregateLo() const { return agg_lo_; }

    /**
     * 孩子的聚合哈希高位。
     * @returns 聚合哈希高位
     */
    uint64_t aggregateHi() const { return agg_hi_; }

    /**
     * 子树里的段落标记数。
     * @returns 段落标记数
     */
    int paras() const { return paras_; }

    /**
     * 从另一段文本切出一段，共享同一缓冲区。
     * @param source 源文本
     * @param offset 起点
     * @param size 码点数
     * @returns 切片
     *//**
     * 呈现态下的段落标记数。
     * @returns 呈现态段落标记数
     */
    int parasDisplay() const { return paras_display_; }

    /**
     * 复制成连续数组。
     * @returns 孩子副本
     */
    std::vector<Ref> toVector() const {
        std::vector<Ref> out;
        out.reserve(size_);
        for (const auto& chunk : chunks_) out.insert(out.end(), chunk->begin(), chunk->end());
        return out;
    }

    /**
     * 转成连续数组；给仍按 vector 接收的构造入口用。
     * @returns 孩子副本
     */
    operator std::vector<Ref>() const { return toVector(); }

    /**
     * 用新的连续数组重建，内容相同的块继续共享。
     * @param items 新孩子
     * @returns 新的孩子表
     */
    Children rebuilt(std::vector<Ref> items) const {
        Children out;
        out.size_ = items.size();
        out.chunks_.reserve((out.size_ + kChunkSize - 1) / kChunkSize);
        out.chunk_agg_lo_.reserve(out.chunks_.capacity());
        out.chunk_agg_hi_.reserve(out.chunks_.capacity());
        out.chunk_paras_.reserve(out.chunks_.capacity());
        out.chunk_paras_display_.reserve(out.chunks_.capacity());
        for (size_t at = 0; at < out.size_; at += kChunkSize) {
            const size_t n = std::min(kChunkSize, out.size_ - at);
            const size_t chunk_index = at / kChunkSize;
            if (chunk_index < chunks_.size()) {
                const std::vector<Ref>& old = *chunks_[chunk_index];
                bool same = old.size() == n;
                for (size_t i = 0; same && i < n; ++i) same = old[i] == items[at + i];
                if (same) {
                    out.chunks_.push_back(chunks_[chunk_index]);
                    out.chunk_agg_lo_.push_back(chunk_agg_lo_[chunk_index]);
                    out.chunk_agg_hi_.push_back(chunk_agg_hi_[chunk_index]);
                    out.chunk_paras_.push_back(chunk_paras_[chunk_index]);
                    out.chunk_paras_display_.push_back(chunk_paras_display_[chunk_index]);
                    continue;
                }
            }
            std::vector<Ref> chunk(items.begin() + static_cast<std::ptrdiff_t>(at),
                                   items.begin() + static_cast<std::ptrdiff_t>(at + n));
            out.chunks_.push_back(std::make_shared<const std::vector<Ref>>(std::move(chunk)));
            appendChunkSummary(out, out.chunks_.back());
        }
        out.recomputeSummary();
        return out;
    }

    /**
     * 与另一份孩子表是否共享下标所在的块。
     * @param o 另一份孩子表
     * @param index 下标
     * @returns true = 两边指向同一个块
     */
    bool sharesChunkWith(const Children& o, size_t index) const {
        if (index >= size_ || index >= o.size_) return false;
        return chunks_[index / kChunkSize] == o.chunks_[index / kChunkSize];
    }

    /**
     * 换掉一个孩子，未改动的块共享。
     * @param index 下标
     * @param child 新孩子
     * @returns 新的孩子表；下标越界时返回原表
     */
    Children withReplaced(size_t index, Ref child) const {
        if (index >= size_) return *this;
        const size_t chunk_index = index / kChunkSize;
        Children out = *this;
        std::vector<Ref> chunk = *chunks_[chunk_index];
        chunk[index % kChunkSize] = std::move(child);
        out.chunks_[chunk_index] = std::make_shared<const std::vector<Ref>>(std::move(chunk));
        out.chunk_agg_lo_[chunk_index] = 0;
        out.chunk_agg_hi_[chunk_index] = 0;
        out.chunk_paras_[chunk_index] = 0;
        out.chunk_paras_display_[chunk_index] = 0;
        appendChunkSummary(out, out.chunks_[chunk_index]);
        out.recomputeSummary();
        return out;
    }

private:

    /**
     * 分块存储；每块不可变、可共享。
     */
    std::vector<std::shared_ptr<const std::vector<Ref>>> chunks_;

    /**
     * 孩子总数。
     */
    size_t size_ = 0;

    /**
     * 各块的聚合哈希低位。
     */
    std::vector<uint64_t> chunk_agg_lo_;

    /**
     * 各块的聚合哈希高位。
     */
    std::vector<uint64_t> chunk_agg_hi_;

    /**
     * 各块的段落标记数。
     */
    std::vector<int> chunk_paras_;

    /**
     * 各块的呈现态段落标记数。
     */
    std::vector<int> chunk_paras_display_;

    /**
     * 整张孩子表的聚合哈希低位。
     */
    uint64_t agg_lo_ = 0;

    /**
     * 整张孩子表的聚合哈希高位。
     */
    uint64_t agg_hi_ = 0;

    /**
     * 整张孩子表的段落标记数。
     */
    int paras_ = 0;

    /**
     * 整张孩子表的呈现态段落标记数。
     */
    int paras_display_ = 0;

    /**
     * 把一条哈希混进聚合值。
     * @param lo 聚合低位
     * @param hi 聚合高位
     * @param child 孩子
     */
    static void mixChild(uint64_t& lo, uint64_t& hi, const Ref& child);

    /**
     * 把一块的聚合值混进整表。
     * @param lo 聚合低位
     * @param hi 聚合高位
     * @param chunk_index 块下标
     */
    void mixChunk(uint64_t& lo, uint64_t& hi, size_t chunk_index) const {
        lo = (lo ^ chunk_agg_lo_[chunk_index]) * 1099511628211ull;
        hi = (hi ^ (chunk_agg_hi_[chunk_index] + 0x9E3779B97F4A7C15ull)) * 14029467366897019727ull;
        hi ^= hi >> 31;
    }

    /**
     * 计算一块的摘要，追加到孩子表。
     * @param out 目标孩子表
     * @param chunk 那一块
     */
    static void appendChunkSummary(Children& out,
                                   const std::shared_ptr<const std::vector<Ref>>& chunk);

    /**
     * 把一块的摘要写到指定下标。
     * @param out 目标孩子表
     * @param index 块下标
     * @param chunk 那一块
     */
    static void writeChunkSummary(Children& out, size_t index,
                                  const std::shared_ptr<const std::vector<Ref>>& chunk);

    /**
     * 从各块摘要重算整表摘要。
     */
    void recomputeSummary() {
        uint64_t lo = 1469598103934665603ull;
        uint64_t hi = 14695981039346656037ull;
        int paras = 0;
        int paras_display = 0;
        for (size_t i = 0; i < chunks_.size(); ++i) {
            mixChunk(lo, hi, i);
            paras += chunk_paras_[i];
            paras_display += chunk_paras_display_[i];
        }
        agg_lo_ = lo;
        agg_hi_ = hi;
        paras_ = paras;
        paras_display_ = paras_display;
    }

    /**
     * 从孩子表重建全部块摘要。
     */
    void recomputeAll() {
        chunk_agg_lo_.clear();
        chunk_agg_hi_.clear();
        chunk_paras_.clear();
        chunk_paras_display_.clear();
        for (size_t i = 0; i < chunks_.size(); ++i) writeChunkSummary(*this, i, chunks_[i]);
        recomputeSummary();
    }
};
/**
 * 节点载荷：种类决定哪一支（文本带字符属性、段落标记带段落覆盖、控件带控件记录、表格三种
 * 节点各带自己的记录、图片与公式各带自己的记录、域带域记录、节带页面设置，其余不带载荷）。
 *
 * 一支一种记录，不做"表格载荷里再套三种"：种类与载荷的对应是一条查表就能写出来的规则，
 * 而多一层包装会让 hashNode 与落盘都要再分一次支。
 */
using Payload = std::variant<std::monostate, RunPayload, MarkPayload, ControlSpec, TableSpec,
                             RowSpec, CellSpec, ImageSpec, FormulaSpec, FieldSpec, SectionSpec>;

/**
 * 一个不可变节点。构造之后不再改动，编辑一律走"换一条路径"（见 replaceNode）。
 *
 * hidden 是作者设定的默认可见性（只在呈现态生效）：它在哈希里，因为呈现态下它决定的
 * 不只是"画不画"，还有位置空间 —— 摘掉一棵子树，它之后的所有位置都前移。
 *
 * paras 是这棵子树里的段落标记数（顺序统计量）。它由内容唯一确定，所以不进哈希，但必须
 * 在构造时算好：比较两份修订时要靠它把"哪一棵子树变了"直接换成一端块区间，否则每层都得
 * 重新数一遍子树。这是顺序统计量树的标量版，只到"多少块"这一档，不做到"第几块"。
 *
 * 块数随可见性变，所以统计量有两个（paras / paras_display）：呈现态下隐藏子树不产生块。
 * 一个数走不到两处 —— 谁要块数，谁就得说清按哪种可见性数。
 */
struct Node {
    /**
     * 节点身份。不进哈希 —— 它表达跨修订的同一性，不表达内容。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 结构种类，决定载荷是哪一支。
     */
    NodeKind kind = NodeKind::TextRun;

    /**
     * 作者设定的默认可见性（只在呈现态生效）。它在哈希里。
     */
    bool hidden = false;

    /**
     * 这棵子树里的段落标记数（编辑态）。派生量，不进哈希。
     */
    int paras = 0;

    /**
     * 呈现态（Display）下这棵子树里有几个段落标记 —— 与 paras 是两个数，不是一个。
     *
     * 隐藏的子树整棵不在呈现态的流里，它里面的段落标记也就不产生块。损伤判定按"哪种可见性"
     * 数块，必须读对应的那一个：两者混用会让 after_blocks 与投影的块数不等，按区间搬块时
     * shift 算错，表现是段落重复（见 TRAPS 124）。它同样不进哈希 —— 由 hidden 与内容唯一
     * 确定，不是新的作者意图。
     */
    int paras_display = 0;

    /**
     * 这一支种类自己的记录。
     */
    Payload payload;

    /**
     * 子节点（共享引用，不可变）。
     */
    Children children;

    /**
     * 自身哈希的低 64 位 —— 种类、隐藏、载荷。
     */
    uint64_t hash_lo = 0;

    /**
     * 自身哈希的高 64 位。
     */
    uint64_t hash_hi = 0;
};

/**
 * 一份修订：根节点、全部内容的哈希、下一个可用的节点号。
 *
 * 根是 Document，其下是一个 Section（本切片不分节），Section 的子序列是 run、段落标记与
 * 控件交替排列的流 —— 与模型层的节点树同一个形状，将来换到真正的树上时不用再翻译一次。
 */
inline void Children::mixChild(uint64_t& lo, uint64_t& hi, const Ref& child) {
    lo = (lo ^ child->hash_lo) * 1099511628211ull;
    hi = (hi ^ (child->hash_hi + 0x9E3779B97F4A7C15ull)) * 14029467366897019727ull;
    hi ^= hi >> 31;
}

inline void Children::writeChunkSummary(Children& out, size_t index,
                                        const std::shared_ptr<const std::vector<Ref>>& chunk) {
    uint64_t lo = 1469598103934665603ull;
    uint64_t hi = 14695981039346656037ull;
    int paras = 0;
    int paras_display = 0;
    for (const Ref& child : *chunk) {
        mixChild(lo, hi, child);
        paras += child->paras;
        paras_display += child->paras_display;
    }
    if (out.chunk_agg_lo_.size() <= index) {
        out.chunk_agg_lo_.resize(index + 1);
        out.chunk_agg_hi_.resize(index + 1);
        out.chunk_paras_.resize(index + 1);
        out.chunk_paras_display_.resize(index + 1);
    }
    out.chunk_agg_lo_[index] = lo;
    out.chunk_agg_hi_[index] = hi;
    out.chunk_paras_[index] = paras;
    out.chunk_paras_display_[index] = paras_display;
}

inline void Children::appendChunkSummary(Children& out,
                                         const std::shared_ptr<const std::vector<Ref>>& chunk) {
    writeChunkSummary(out, out.chunks_.size() - 1, chunk);
}
struct Doc {
    /**
     * 根节点（Document）。
     */
    Ref root;

    /**
     * 全文内容哈希的低 64 位。
     */
    uint64_t lo = 0;

    /**
     * 全文内容哈希的高 64 位。
     */
    uint64_t hi = 0;

    /**
     * 下一个可用的节点号。
     */
    NodeId next_id = 1;

    /**
     * 这份修订有效吗（根非空）。
     * @return true = 有根
     */
    explicit operator bool() const { return root != nullptr; }
};

/**
 * 一次编辑的结果。失败时带原因，不返回一个"看起来没变"的文档 —— 调用方分不清
 * "编辑被拒绝"与"编辑没产生变化"。
 */
struct EditResult {
    /**
     * 成了没有。
     */
    bool ok = false;

    /**
     * 新的修订；失败时是原样返回的旧值（不要用）。
     */
    Doc doc;

    /**
     * 失败原因。
     */
    std::string error;
};

/**
 * 128 位内容哈希的累加器。
 *
 * 节点哈希与块哈希共用它 —— 两处必须用同一套混合，否则"块哈希变了"与"节点哈希变了"
 * 不可比，按哈希做的复用也就无从判断。两条通道真的不同源（乘子与混合方式都不同），
 * 否则 128 位只是同一个 64 位写了两遍。
 */
class Hasher {
public:

    void u64(uint64_t v) {
        lo_ = (lo_ ^ v) * 1099511628211ull;
        hi_ = (hi_ ^ (v + 0x9E3779B97F4A7C15ull)) * 14029467366897019727ull;
        hi_ ^= hi_ >> 31;
    }

    /**
     * 混进一个有符号整数（按位当无符号）。
     * @param v 要混进去的值
     */
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }

    /**
     * 混进一个字节（种类、枚举这类）。
     * @param v 要混进去的值
     */
    void u8(uint8_t v) { u64(v); }

    /**
     * 混进一个浮点数。按位混，避免 -0.0 与 0.0 混成同一个。
     * @param v 要混进去的值
     */
    void f32(float v);

    /**
     * 混进一个字符串（长度与字节一起混）。
     * @param s 要混进去的串
     */
    void str(const std::string& s);

    /**
     * 低 64 位。
     * @return 累计值
     */
    uint64_t lo() const { return lo_; }

    /**
     * 高 64 位。
     * @return 累计值
     */
    uint64_t hi() const { return hi_; }

private:

    uint64_t lo_ = 1469598103934665603ull;

    /**
     * 高位的累加器（初始值与 lo_ 不同，两条通道不同源）。
     */
    uint64_t hi_ = 14695981039346656037ull;
};

/**
 * 一个节点自身的哈希：种类、隐藏标记、载荷，不含孩子。
 *
 * 完整节点哈希是"自身哈希 + 孩子哈希"，所以两者之间不需要另一份定义。损伤判定要区分
 * "容器自己的属性变了"与"只是里面某处变了"：前者要把它覆盖的块整段算进损伤（列宽变了，
 * 同一张表每一行的行高都可能变），后者只算变的那一块。
 * @param h 累加器
 * @param node 节点
 */
void hashSelf(Hasher& h, const Node& node);

/**
 * 正文那一节的子序列（本切片不分节）。没有正文时返回 nullptr。
 * @param doc 修订
 * @return 正文那一节的孩子表；没有正文时为 nullptr
 */
const Children* bodyOf(const Doc& doc);

/**
 * 按节点号找节点（只走正文，线性扫描）。本切片没有索引，用途是读属性。
 * @param doc 修订
 * @param id 节点号
 * @return 节点；找不到为 nullptr
 */
const Node* findNode(const Doc& doc, NodeId id);

/**
 * 父节点。修订没有向上的指针（它是不可变树，加一条反向链就要在每次路径复制时维护两处），
 * 所以按节点号现找。命令层用它把"某个节点"换成"某个孩子槽位"。
 * @param doc 修订
 * @param id 节点号
 * @return 父节点；找不到或它是根时为 nullptr
 */
const Node* parentOf(const Doc& doc, NodeId id);

/**
 * 修订节点 → 节点描述：内容、记录与孩子，不含身份。
 *
 * 结构步骤要记"被删掉的是什么"（流位置映射要用它的长度），剪贴板要一份可移植的拷贝 ——
 * 两处都要的是这份值。身份不进描述：修订层按流位置定位，"是哪一号节点"在换一条路径之后
 * 已经没有意义。
 * @param node 修订节点
 * @return 节点描述
 */
NodeSpec toSpec(const Node& node);

/**
 * 建一个节点：哈希与段落数由孩子算出来，调用方给的是内容与身份，不是派生量。
 *
 * 派生量不从外面填，是不给"第二个可写真相"留口子：节点若能带着别人算好的哈希建出来，
 * 就多了一处能对不上的地方。反序列化与"从模型导出修订"用它。
 * @param id 节点号
 * @param kind 节点种类
 * @param payload 这一支种类自己的记录
 * @param children 孩子（共享引用）
 * @param hidden 作者设定的默认可见性
 * @return 新节点（哈希与段落数已由孩子算好）
 */
Ref makeNode(NodeId id, NodeKind kind, Payload payload, std::vector<Ref> children,
             bool hidden = false);

/**
 * 从只读孩子表构造节点；未改动的块可以共享。
 * @param id 节点号
 * @param kind 种类
 * @param payload 载荷
 * @param children 只读孩子表
 * @param hidden 作者设定的默认可见性
 * @returns 节点
 */
Ref makeNodeFromChildren(NodeId id, NodeKind kind, Payload payload, Children children,
                         bool hidden = false);

/**
 * 用一棵树与分配器水位装出一份修订；全文哈希取自根。
 *
 * 水位决定后续分配的起点。反序列化按文件里的值恢复它，否则新建的节点会与文件里引用过的
 * 身份撞上 —— 那种撞车是静默的：改一个节点会连带改到另一个同号节点。
 * @param root 根节点
 * @param next_id 下一个可用的节点号（反序列化按文件里的水位给）
 * @return 一份修订
 */
Doc adopt(Ref root, NodeId next_id);

/**
 * 用若干段纯文本建一份修订。空段落被保留：它由一个段落标记单独表示。
 * @param paragraphs 每段一份码点序列
 * @return 一份修订
 */
Doc makeTextDoc(const std::vector<std::u32string>& paragraphs);

/**
 * 同上，但每段的段落标记挂一份内联格式。
 *
 * 宿主"打开纯文本"要的不是一串裸段落：字体、字号、行距、语言都决定怎么排。这些属性只能
 * 挂在段落标记上（段落格式的来源就是它），所以建文档时一次给全 —— 建完再逐段
 * applyParaFormat 等于把同一件事做两遍，而中间那一拍是没有任何格式的文档。
 *
 * @param paragraphs 每段一份码点序列
 * @param para 每一段的段落格式（内联覆盖）
 * @return 一份修订
 */
Doc makeTextDoc(const std::vector<std::u32string>& paragraphs, const ParaFormatPatch& para);

/**
 * 段落数（等于流里段落标记的个数，含控件内部的标记）。
 *
 * 直接读根的 paras。现数一遍的写法在 locate 的循环里就是 O(段数 × 每段节点数)——
 * "定位一段"与整篇规模挂钩，而它每敲一个字都要跑一次。
 * @param doc 修订
 * @return 段落数（含控件内部的标记）
 */
int paraCount(const Doc& doc);

/**
 * 每段的文本。只拼 run 的码点，不还原字符属性与段落格式；控件里的文字按流顺序并入。
 * @param doc 修订
 * @return 每段的文本（只拼 run 的码点）
 */
std::vector<std::u32string> paraTexts(const Doc& doc);

/**
 * 当前排版是否支持这份文档的节形状。
 *
 * 排版只消费正文流、分节分页与第一节水印；按节页度量与页眉页脚还没有实现。这里把
 * 这两类形状显式判成不支持，避免它们被当成普通内容静默排出错误结果。
 * @param doc 修订
 * @param why 失败原因；可以为空指针
 * @return true = 当前排版能正确表达
 */
bool checkSupportedLayout(const Doc& doc, std::string* why);

/**
 * 在第 para 段的第 cp 个码点之前插入文本。
 *
 * cp 的取址空间是这一段自己的码点序列（含控件内部的文字），[0, 段长] 都合法。插入的文本
 * 继承插入点的字符属性（打在加粗的 run 里就该是加粗的），所以并进那个 run、不新起一个。
 * 位置在控件内部时，改的是控件那一层的孩子表，容器之外的孩子一个都不复制。
 * @param doc 修订
 * @param para 段号
 * @param cp 段内码点位置
 * @param text 要插入的文本
 * @return 结果（失败时带原因，修订不动）
 */
EditResult insertText(const Doc& doc, int para, int cp, const std::u32string& text);

/**
 * 删掉第 para 段从第 cp 个码点起的 count 个码点。
 *
 * 只在本段内删：够不到就删到段末为止，不跨过段落标记。跨段删除是"删一段"的另一种语义，
 * 由调用方拆成"删本段余下"加"删下一段整段"，不该由这里替它猜。
 * @param doc 修订
 * @param para 段号
 * @param cp 起始码点位置
 * @param count 删几个码点
 * @return 结果（失败时带原因，修订不动）
 */
EditResult eraseText(const Doc& doc, int para, int cp, int count);

/**
 * 在第 para 段的第 cp 个码点处断开，插入一个段落标记。
 *
 * cp 取 0 或段长都合法，结果是多出一个空段。新段落标记默认没有内联覆盖与样式引用。
 * 位置在控件内部时那枚标记也落在控件里 —— 控件因此跨段（投影里它的起止在不同块）。
 * @param doc 修订
 * @param para 段号
 * @param cp 断开处的段内码点位置
 * @return 结果（失败时带原因，修订不动）
 */
EditResult splitPara(const Doc& doc, int para, int cp);

/**
 * 把第 para 段与下一段合并：删掉它们之间的段落标记。
 *
 * 两段各自的 run 保持原样（不合并同属性的相邻 run），文本拼接的结果与逐段读出再相加相同。
 * @param doc 修订
 * @param para 要并入下一段的段号
 * @return 结果（失败时带原因，修订不动）
 */
EditResult mergePara(const Doc& doc, int para);

/**
 * 在第 para 段的第 cp 个码点之前插入一个内容控件，内容是一段文本。
 *
 * 括号由投影按边界产生，不占文本位置（见 compile.cpp 里控件那一段的次序说明）。
 * label 为空时控件里没有内容，投影出来是一对相邻的边界。created 非空时写回新控件的节点号 ——
 * 紧接着要设隐藏或回填值的调用方不该再去树里按形状把它找一遍。
 * @param doc 修订
 * @param para 段号
 * @param cp 插入处的段内码点位置
 * @param spec 控件记录
 * @param label 控件里的内容
 * @param created 写回新控件的节点号（可为 nullptr）
 * @return 结果（失败时带原因，修订不动）
 */
EditResult insertControl(const Doc& doc, int para, int cp, const ControlSpec& spec,
                         const std::u32string& label, NodeId* created = nullptr);

/**
 * 设置一个节点的隐藏标记（作者意图，只在呈现态生效）。节点不存在时返回失败。
 * @param doc 修订
 * @param id 节点号
 * @param hidden true = 隐藏
 * @return 结果（节点不存在时失败）
 */
EditResult setHidden(const Doc& doc, NodeId id, bool hidden);

}  // namespace heditor::doc
