#pragma once
/**
 * 控件区间的唯一查询面。
 *
 * 为什么单列一份：控件在模型里不是容器节点，只是一段文字区间加流里的两枚括号占位，
 *   于是"区间在哪、这一格归谁"引擎、编辑命令与光标解析都要问。各写一套必然在边界上分家
 *   （同一个偏移压着六枚括号时，按括号排布反推的那一套会把三层内容区间缩成一个点）。
 *   区间只从流索引取（FlowIndex::intervalOf），它本来就算好了每枚括号的位置。
 *
 * 这里的函数都是纯查询、没有状态：调用方把自己 view 里的 flow 与控件节点表递进来。
 *
 * 下标约定：控件下标 = 文档顺序 = control_node_ids 的下标 = 控件集合的下标。三者由同一次
 *   投影产出，调用方要保证它们同步（引擎侧见 syncControls）。
 */
#include <algorithm>
#include <numeric>
#include <vector>

#include "core/doc/flow.h"
#include "core/doc/flow_types.h"
#include "core/doc/node_id.h"

namespace heditor {

/**
 * 控件在流里的区间（含两枚括号，半开）。
 *
 * 宽 2 = 空控件（两枚括号相邻）。不在流里（隐藏、摘除、未装载）返回假。
 *
 * @param flow 流索引
 * @param id 控件节点
 * @param lo 写回区间起点（开括号那一格）
 * @param hi 写回区间终点（闭括号之后，不含）
 * @return true = 这个控件在流里
 */
inline bool controlSpanOf(const doc::FlowIndex& flow, NodeId id, FlowPos* lo, FlowPos* hi) {
    FlowPos first = 0;
    FlowPos end = 0;
    if (!flow.intervalOf(id, &first, &end)) return false;
    // 一个控件至少有两枚括号。少于此说明索引给的不是控件（节点被摘掉了一半）。
    if (end - first < 2) return false;
    if (lo != nullptr) *lo = first;
    if (hi != nullptr) *hi = end;
    return true;
}

/**
 * 控件的内容区间（插入点范围，两端都含）。两枚括号各占一格，所以是 span 退掉两头。
 *
 * 空控件退化成一点（lo == hi）：那个点是"开括号之后、闭括号之前"，本来就是唯一的插入点。
 *
 * @param flow 流索引
 * @param id 控件节点
 * @param lo 写回内容起点（开括号之后）
 * @param hi 写回内容末尾（闭括号之前）
 * @return true = 这个控件在流里
 */
inline bool controlContentSpanOf(const doc::FlowIndex& flow, NodeId id, FlowPos* lo, FlowPos* hi) {
    FlowPos first = 0;
    FlowPos end = 0;
    if (!controlSpanOf(flow, id, &first, &end)) return false;
    if (lo != nullptr) *lo = first + 1;
    if (hi != nullptr) *hi = end - 1;
    return true;
}

/**
 * 某个流位置落在哪个控件的内容区间里。嵌套时取最内层 —— 起点最大者，同起点取跨度短者。
 *
 * 取外层会让"这一格归谁"与编辑归属分家：光标在一个字段里、编辑却按另一个字段的规则判。
 *   按遍历顺序取第一个命中的更糟：外层可编辑会替内层只读作证。
 *
 * 线性版，只给 ControlIndex 还没建（或建出来是空）的那条路用。规则与 ControlIndex::innermost
 *   是同一条，两处改一处不改就会让"索引建好前后"给出不同答案。
 *
 * @param flow 流索引
 * @param ids 控件节点表（文档顺序）
 * @param f 流位置
 * @param out_lo 写回它的内容起点（可为空）
 * @param out_hi 写回它的内容末尾（可为空）
 * @return 控件下标；-1 = 不在任何控件的内容里
 */
inline int innermostControlAtFlow(const doc::FlowIndex& flow, const std::vector<NodeId>& ids,
                                  FlowPos f, FlowPos* out_lo = nullptr, FlowPos* out_hi = nullptr) {
    int best = -1;
    FlowPos best_lo = 0;
    FlowPos best_hi = 0;
    for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
        FlowPos lo = 0;
        FlowPos hi = 0;
        if (!controlContentSpanOf(flow, ids[static_cast<size_t>(i)], &lo, &hi)) continue;
        if (f < lo || f > hi) continue;
        if (best < 0 || lo > best_lo || (lo == best_lo && (hi - lo) < (best_hi - best_lo))) {
            best = i;
            best_lo = lo;
            best_hi = hi;
        }
    }
    if (best >= 0) {
        if (out_lo != nullptr) *out_lo = best_lo;
        if (out_hi != nullptr) *out_hi = best_hi;
    }
    return best;
}

/**
 * 控件区间索引：把“按流位置找最内层控件”从线性扫描降成 O(log n + 深度)。
 *
 * 控件是良构嵌套/并列的区间；构建时按内容起点排序并用栈连出父链，查询时先二分到
 * 起点不大于 f 的最后一条，再沿父链找第一个包住 f 的项。控件数量大时这条路径每次
 * 按键都会走，线性扫描的代价会随字段数增长。
 */
class ControlIndex {
public:

    /**
     * 按一份流索引与控件节点表重建。
     * @param flow 流索引
     * @param ids 控件节点表（文档顺序）
     */
    void build(const doc::FlowIndex& flow, const std::vector<NodeId>& ids) {
        entries_.clear();
        by_control_.assign(ids.size(), -1);
        for (int i = 0; i < static_cast<int>(ids.size()); ++i) {
            FlowPos b_lo = 0;
            FlowPos b_hi = 0;
            if (!controlSpanOf(flow, ids[static_cast<size_t>(i)], &b_lo, &b_hi)) continue;
            Entry e;
            e.control = i;
            e.bracket_lo = b_lo;
            e.bracket_hi = b_hi;
            e.content_lo = b_lo + 1;
            e.content_hi = b_hi - 1;
            entries_.push_back(e);
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
            if (a.content_lo != b.content_lo) return a.content_lo < b.content_lo;
            if (a.content_hi != b.content_hi) return a.content_hi > b.content_hi;
            return a.control < b.control;
        });
        std::vector<int> stack;
        stack.reserve(entries_.size());
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            Entry& e = entries_[static_cast<size_t>(i)];
            while (!stack.empty()) {
                const Entry& top = entries_[static_cast<size_t>(stack.back())];
                if (top.content_lo == e.content_lo && top.content_hi == e.content_hi) {
                    e.parent = top.parent;
                    stack.pop_back();
                    continue;
                }
                if (top.content_hi < e.content_lo) {
                    stack.pop_back();
                    continue;
                }
                break;
            }
            if (!stack.empty()) e.parent = stack.back();
            by_control_[static_cast<size_t>(e.control)] = i;
            stack.push_back(i);
        }
        by_start_.resize(entries_.size());
        std::iota(by_start_.begin(), by_start_.end(), 0);
        std::sort(by_start_.begin(), by_start_.end(), [this](int a, int b) {
            const Entry& ea = entries_[static_cast<size_t>(a)];
            const Entry& eb = entries_[static_cast<size_t>(b)];
            if (ea.bracket_lo != eb.bracket_lo) return ea.bracket_lo < eb.bracket_lo;
            return ea.control < eb.control;
        });
        by_end_.resize(entries_.size());
        std::iota(by_end_.begin(), by_end_.end(), 0);
        std::sort(by_end_.begin(), by_end_.end(), [this](int a, int b) {
            const Entry& ea = entries_[static_cast<size_t>(a)];
            const Entry& eb = entries_[static_cast<size_t>(b)];
            if (ea.bracket_hi != eb.bracket_hi) return ea.bracket_hi < eb.bracket_hi;
            return ea.control < eb.control;
        });
    }

    /**
     * 有没有可查询的控件。
     * @returns 是否为空
     */
    bool empty() const { return entries_.empty(); }

    /**
     * 控件个数（含不在流里、因而不进索引的那些，仍按原表长度算）。
     * @returns 原表长度
     */
    int size() const { return static_cast<int>(by_control_.size()); }

    /**
     * 内容区间。
     * @param control 控件下标
     * @param lo 写回内容起点（可为空）
     * @param hi 写回内容末尾（可为空）
     * @returns 这个控件在索引里吗
     */
    bool contentSpan(int control, FlowPos* lo, FlowPos* hi) const {
        if (control < 0 || control >= static_cast<int>(by_control_.size())) return false;
        const int i = by_control_[static_cast<size_t>(control)];
        if (i < 0) return false;
        const Entry& e = entries_[static_cast<size_t>(i)];
        if (lo != nullptr) *lo = e.content_lo;
        if (hi != nullptr) *hi = e.content_hi;
        return true;
    }

    /**
     * 括号区间（含两枚括号）。
     * @param control 控件下标
     * @param lo 写回区间起点（可为空）
     * @param hi 写回区间终点（可为空）
     * @returns 这个控件在索引里吗
     */
    bool bracketSpan(int control, FlowPos* lo, FlowPos* hi) const {
        if (control < 0 || control >= static_cast<int>(by_control_.size())) return false;
        const int i = by_control_[static_cast<size_t>(control)];
        if (i < 0) return false;
        const Entry& e = entries_[static_cast<size_t>(i)];
        if (lo != nullptr) *lo = e.bracket_lo;
        if (hi != nullptr) *hi = e.bracket_hi;
        return true;
    }

    /**
     * 最内层控件。规则与 innermostControlAtFlow 同一条：起点最大者，同起点取跨度短者。
     * 这一份是索引路径（生产走它），那一份是索引未建时的线性路径。
     * @param f 流位置
     * @param lo 写回内容起点（可为空）
     * @param hi 写回内容末尾（可为空）
     * @returns 控件下标；-1 = 不在任何控件内容里
     */
    int innermost(FlowPos f, FlowPos* lo = nullptr, FlowPos* hi = nullptr) const {
        if (entries_.empty()) return -1;
        const auto it =
            std::upper_bound(entries_.begin(), entries_.end(), f,
                             [](FlowPos v, const Entry& e) { return v < e.content_lo; });
        int i = static_cast<int>(it - entries_.begin()) - 1;
        while (i >= 0) {
            const Entry& e = entries_[static_cast<size_t>(i)];
            if (e.content_lo <= f && f <= e.content_hi) {
                if (lo != nullptr) *lo = e.content_lo;
                if (hi != nullptr) *hi = e.content_hi;
                return e.control;
            }
            i = e.parent;
        }
        return -1;
    }

    /**
     * 父控件下标。
     * @param control 控件下标
     * @returns 父下标；-1 = 顶层或不在索引里
     */
    int parentOf(int control) const {
        if (control < 0 || control >= static_cast<int>(by_control_.size())) return -1;
        const int i = by_control_[static_cast<size_t>(control)];
        if (i < 0) return -1;
        const int p = entries_[static_cast<size_t>(i)].parent;
        return p < 0 ? -1 : entries_[static_cast<size_t>(p)].control;
    }

    /**
     * 起点正好在 p 的控件；多个时取控件下标最小者。
     * @param p 流位置
     * @returns 控件下标；-1 = 没有
     */
    int startingAt(FlowPos p) const {
        const auto it =
            std::lower_bound(by_start_.begin(), by_start_.end(), p, [this](int idx, FlowPos v) {
                return entries_[static_cast<size_t>(idx)].bracket_lo < v;
            });
        for (auto k = it; k != by_start_.end(); ++k) {
            const Entry& e = entries_[static_cast<size_t>(*k)];
            if (e.bracket_lo != p) break;
            return e.control;
        }
        return -1;
    }

    /**
     * 终点正好在 p 的控件；多个时取控件下标最小者。
     * @param p 流位置
     * @returns 控件下标；-1 = 没有
     */
    int endingAt(FlowPos p) const {
        const auto it =
            std::lower_bound(by_end_.begin(), by_end_.end(), p, [this](int idx, FlowPos v) {
                return entries_[static_cast<size_t>(idx)].bracket_hi < v;
            });
        for (auto k = it; k != by_end_.end(); ++k) {
            const Entry& e = entries_[static_cast<size_t>(*k)];
            if (e.bracket_hi != p) break;
            return e.control;
        }
        return -1;
    }

private:

    /**
     * 一条控件区间记录。
     */
    struct Entry {
        /**
         * 括号区间起点。
         */
        FlowPos bracket_lo = 0;

        /**
         * 括号区间终点（不含）。
         */
        FlowPos bracket_hi = 0;

        /**
         * 内容起点。
         */
        FlowPos content_lo = 0;

        /**
         * 内容末尾（含）。
         */
        FlowPos content_hi = 0;

        /**
         * 控件下标。
         */
        int control = -1;

        /**
         * 父项在 entries_ 里的下标；-1 = 顶层。
         */
        int parent = -1;
    };

    /**
     * 按内容起点排序的区间。
     */
    std::vector<Entry> entries_;

    /**
     * 控件下标 → entries_ 下标；不在流里为 -1。
     */
    std::vector<int> by_control_;

    /**
     * 按括号起点排序的 entries_ 下标。
     */
    std::vector<int> by_start_;

    /**
     * 按括号终点排序的 entries_ 下标。
     */
    std::vector<int> by_end_;
};

/**
 * 某个流位置落在哪个控件的内容区间里：索引与线性两版由这一处挑。
 *
 * 索引没建（或建出来是空）时退回线性扫。两条规则必须给出同一个答案，所以"用哪一版"
 *   只判一次 —— 调用点各写一遍的话改一处就会漏，症状是"索引建好前后答案不同"。
 *
 * @param index 控件区间索引；nullptr 表示还没建
 * @param flow 流索引
 * @param ids 控件节点表（文档顺序）
 * @param f 流位置
 * @param out_lo 写回它的内容起点（可为空）
 * @param out_hi 写回它的内容末尾（可为空）
 * @return 控件下标；-1 = 不在任何控件的内容里
 */
inline int resolveInnermostControl(const ControlIndex* index, const doc::FlowIndex& flow,
                                   const std::vector<NodeId>& ids, FlowPos f,
                                   FlowPos* out_lo = nullptr, FlowPos* out_hi = nullptr) {
    if (index != nullptr && !index->empty()) return index->innermost(f, out_lo, out_hi);
    return innermostControlAtFlow(flow, ids, f, out_lo, out_hi);
}

}  // namespace heditor
