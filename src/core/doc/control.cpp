#include "core/doc/control.h"

#include <algorithm>
#include <cstdint>

namespace heditor {

const char* controlKindName(ControlKind k) {
    switch (k) {
        case ControlKind::NumberField:
            return "数值";
        case ControlKind::DateField:
            return "日期";
        default:
            return "文本";
    }
}

ControlKind controlKindFromCode(int code) {
    switch (code) {
        case 1:
            return ControlKind::NumberField;
        case 2:
            return ControlKind::DateField;
        default:
            return ControlKind::TextField;
    }
}

void ControlSet::clear() {
    items_.clear();
    order_.clear();
    by_end_.clear();
}

int ControlSet::add(const Control& c) {
    items_.push_back(c);
    return static_cast<int>(items_.size()) - 1;
}

void ControlSet::normalize() {
    // 清掉上一轮的树（重新算，不做增量 —— 控件数量是几十个量级）
    for (Control& c : items_) {
        c.parent = -1;
        c.depth = 0;
    }

    order_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) order_[i] = static_cast<int>(i);
    std::sort(order_.begin(), order_.end(), [this](int a, int b) {
        const ControlRange& ra = items_[static_cast<size_t>(a)].range;
        const ControlRange& rb = items_[static_cast<size_t>(b)].range;
        if (ra.start != rb.start) return ra.start < rb.start;
        if (ra.end != rb.end) return rb.end < ra.end;  // 起点相同 → 长的（外层的）在前
        return a < b;
    });

    // 父 = 所有"包含它"的控件里最内层的那个。
    // 判据用区间端点而非长度：跨段区间算不出有意义的长度，
    // 而"起点更晚 / 终点更早"在任何情况下都成立。
    // 包含的口径见 rangeContainsForNesting —— 完全重合的两段区间互为兄弟，
    //   否则它们会互相认父，深度当场失控（两段的 depth 都会涨到上限）。
    //
    // 排序后用一个栈即可：栈里只留仍可能包住后续项的区间，遇到不包含就弹。
    //   这比逐对比较少一个数量级；父链仍然按"最内层包含者"落。
    std::vector<int> stack;
    stack.reserve(order_.size());
    for (int idx : order_) {
        Control& c = items_[static_cast<size_t>(idx)];
        while (!stack.empty()) {
            const Control& top = items_[static_cast<size_t>(stack.back())];
            if (top.range.start == c.range.start && top.range.end == c.range.end) {
                c.parent = top.parent;
                stack.pop_back();
                continue;
            }
            if (top.range.end < c.range.start || !rangeContainsForNesting(top.range, c.range)) {
                stack.pop_back();
                continue;
            }
            break;
        }
        if (!stack.empty()) c.parent = stack.back();
        stack.push_back(idx);
    }

    for (int idx : order_) {
        Control& c = items_[static_cast<size_t>(idx)];
        c.depth = c.parent >= 0 ? items_[static_cast<size_t>(c.parent)].depth + 1 : 0;
    }

    by_end_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) by_end_[i] = static_cast<int>(i);
    std::sort(by_end_.begin(), by_end_.end(), [this](int a, int b) {
        const ControlRange& ra = items_[static_cast<size_t>(a)].range;
        const ControlRange& rb = items_[static_cast<size_t>(b)].range;
        if (ra.end != rb.end) return ra.end < rb.end;
        return a < b;
    });
}

int ControlSet::innermostContentAt(const TextPos& pos) const { return innermostAt(pos, false); }

int ControlSet::innermostInsertAt(const TextPos& pos) const { return innermostAt(pos, true); }

int ControlSet::innermostAt(const TextPos& pos, bool include_end) const {
    if (order_.empty()) {
        int fallback = -1;
        int fallback_depth = -1;
        for (size_t i = 0; i < items_.size(); ++i) {
            const ControlRange& r = items_[i].range;
            const bool hit = include_end ? rangeAllowsInsert(r, pos) : rangeContainsContent(r, pos);
            if (hit && items_[i].depth > fallback_depth) {
                fallback_depth = items_[i].depth;
                fallback = static_cast<int>(i);
            }
        }
        return fallback;
    }
    int best = -1;
    int best_depth = -1;
    const auto consider = [&](int ci) {
        const Control& c = items_[static_cast<size_t>(ci)];
        const bool hit =
            include_end ? rangeAllowsInsert(c.range, pos) : rangeContainsContent(c.range, pos);
        if (!hit) return;
        // 深度严格大于才换；同深度保留下标小者，和逐项比较的稳定口径一致。
        if (c.depth > best_depth || (c.depth == best_depth && (best < 0 || ci < best))) {
            best_depth = c.depth;
            best = ci;
        }
    };
    const auto it =
        std::upper_bound(order_.begin(), order_.end(), pos, [this](const TextPos& p, int idx) {
            return p < items_[static_cast<size_t>(idx)].range.start;
        });
    int oi = static_cast<int>(it - order_.begin()) - 1;
    if (oi >= 0) {
        int ci = order_[static_cast<size_t>(oi)];
        while (ci >= 0) {
            consider(ci);
            ci = items_[static_cast<size_t>(ci)].parent;
        }
    }
    if (include_end) {
        const auto by_end = std::lower_bound(
            by_end_.begin(), by_end_.end(), pos, [this](int idx, const TextPos& p) {
                return items_[static_cast<size_t>(idx)].range.end < p;
            });
        for (auto k = by_end; k != by_end_.end(); ++k) {
            const int ci = *k;
            if (!(items_[static_cast<size_t>(ci)].range.end == pos)) break;
            consider(ci);
        }
    }
    return best;
}

std::vector<int> ControlSet::overlapping(const ControlRange& r) const {
    std::vector<int> out;
    // [a,b) 与 [start,end) 有交集 ⟺ start < b 且 a < end。
    // 按起点序扫到 r.end 就停：后面的控件起点更晚，不可能再有交集。
    for (int idx : order_) {
        const ControlRange& c = items_[static_cast<size_t>(idx)].range;
        if (!(c.start < r.end)) break;
        if (r.start < c.end) out.push_back(idx);
    }
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        const int da = items_[static_cast<size_t>(a)].depth;
        const int db = items_[static_cast<size_t>(b)].depth;
        if (da != db) return da > db;
        return a < b;
    });
    return out;
}

}  // namespace heditor
