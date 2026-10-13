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
        c.children.clear();
    }

    order_.resize(items_.size());
    for (size_t i = 0; i < items_.size(); ++i) order_[i] = static_cast<int>(i);
    std::sort(order_.begin(), order_.end(), [this](int a, int b) {
        const ControlRange& ra = items_[static_cast<size_t>(a)].range;
        const ControlRange& rb = items_[static_cast<size_t>(b)].range;
        if (ra.start != rb.start) return ra.start < rb.start;
        return rb.end < ra.end;  // 起点相同 → 长的（外层的）在前
    });

    // 父 = 所有"包含它"的控件里最内层的那个。
    // 判据用区间端点而非长度：跨段区间算不出有意义的长度，
    // 而"起点更晚 / 终点更早"在任何情况下都成立。
    // 包含的口径见 rangeContainsForNesting —— 完全重合的两段区间互为兄弟，
    //   否则它们会互相认父，深度当场失控（两段的 depth 都会涨到上限）。
    for (size_t i = 0; i < items_.size(); ++i) {
        int best = -1;
        for (size_t j = 0; j < items_.size(); ++j) {
            if (i == j) continue;
            if (!rangeContainsForNesting(items_[j].range, items_[i].range)) continue;
            if (best < 0) {
                best = static_cast<int>(j);
                continue;
            }
            const ControlRange& cur = items_[static_cast<size_t>(best)].range;
            const ControlRange& cand = items_[j].range;
            const bool cand_more_inner =
                (cur.start < cand.start) || (!(cand.start < cur.start) && cand.end < cur.end);
            if (cand_more_inner) best = static_cast<int>(j);
        }
        items_[i].parent = best;
    }

    for (size_t i = 0; i < items_.size(); ++i) {
        int d = 0;
        int p = items_[i].parent;
        // 严格包含是偏序，父链必然有限（每上一级外层都严格更大），上限取控件数即可。
        //   写成固定的 64 会把"真的出了环"也压成 64 —— 那就成了静默的错误答案。
        while (p >= 0 && d <= static_cast<int>(items_.size())) {
            ++d;
            p = items_[static_cast<size_t>(p)].parent;
        }
        items_[i].depth = d;
        if (items_[i].parent >= 0)
            items_[static_cast<size_t>(items_[i].parent)].children.push_back(static_cast<int>(i));
    }
}

int ControlSet::innermostAt(const TextPos& pos, bool include_end) const {
    int best = -1;
    int best_depth = -1;
    for (size_t i = 0; i < items_.size(); ++i) {
        const ControlRange& r = items_[i].range;
        const bool hit = include_end ? rangeAllowsInsert(r, pos) : rangeContainsContent(r, pos);
        if (!hit) continue;
        // 严格大于：两个区间完全相同时（不该有人这么配）取下标小的那个，
        // 结果稳定且可复现 —— 比"随遍历顺序变"好。
        if (items_[i].depth > best_depth) {
            best_depth = items_[i].depth;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::vector<int> ControlSet::overlapping(const ControlRange& r) const {
    std::vector<int> out;
    for (size_t i = 0; i < items_.size(); ++i) {
        const ControlRange& c = items_[i].range;
        // [a,b) 与 [start,end) 有交集 ⟺ start < b 且 a < end
        if (c.start < r.end && r.start < c.end) out.push_back(static_cast<int>(i));
    }
    std::sort(out.begin(), out.end(), [this](int a, int b) {
        return items_[static_cast<size_t>(a)].depth > items_[static_cast<size_t>(b)].depth;
    });
    return out;
}

}  // namespace heditor
