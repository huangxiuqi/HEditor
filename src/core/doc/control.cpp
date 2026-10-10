#include "core/doc/control.h"

#include <algorithm>
#include <cstdint>

namespace ced {

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

    // 父 = 所有"严格包含它"的控件里最内层的那个。
    // 判据用区间端点而非长度：跨段区间算不出有意义的长度，
    // 而"起点更晚 / 终点更早"在任何情况下都成立。
    for (size_t i = 0; i < items_.size(); ++i) {
        int best = -1;
        for (size_t j = 0; j < items_.size(); ++j) {
            if (i == j) continue;
            if (!rangeCovers(items_[j].range, items_[i].range)) continue;
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
        // 深度上限防御：万一有人造出循环引用，也不能把这里挂死。
        while (p >= 0 && d < 64) {
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

/**
 * 区间伸缩
 * 两个端点用不同的比较（这是唯一容易写错的地方，所以单独说明）：
 * 起点：start.offset > at 才右移。
 * 在 start 处插入 → 起点不动，新字符成为控件的一部分（区间变长）。
 * 若写成 >=，在控件开头打字会让区间整体右移、新字落在控件外 ——
 * 症状是"在字段开头打的字立刻变成不可编辑"。
 * 终点：end.offset >= at 就右移。
 * 在 end 处插入 = 在末尾追加，区间必须变长。
 * 若写成 >，在字段末尾追加的内容会掉到控件外。
 */
void adjustControlsOnInsert(ControlSet& ctrls, int para, size_t at, size_t n) {
    if (n == 0) return;
    for (Control& c : ctrls.mutableAll()) {
        if (c.range.start.para == para && c.range.start.offset > at) c.range.start.offset += n;
        if (c.range.end.para == para && c.range.end.offset >= at) c.range.end.offset += n;
    }
}

void adjustControlsOnSplitPara(ControlSet& ctrls, const TextPos& pos) {
    for (Control& c : ctrls.mutableAll()) {
        auto fix = [&](TextPos& e) {
            if (e.para > pos.para) {
                e.para += 1;  // 整体后移一段
                return;
            }
            if (e.para < pos.para) return;
            if (e.offset >= pos.offset) {  // 内容被搬进了新段
                e.para = pos.para + 1;
                e.offset -= pos.offset;
            }
        };
        fix(c.range.start);
        fix(c.range.end);
    }
}

void adjustControlsOnMergePara(ControlSet& ctrls, int para, size_t prev_len) {
    for (Control& c : ctrls.mutableAll()) {
        auto fix = [&](TextPos& e) {
            if (e.para == para + 1) {
                e.para = para;
                e.offset += prev_len;
            } else if (e.para > para + 1) {
                e.para -= 1;
            }
        };
        fix(c.range.start);
        fix(c.range.end);
    }
}

void adjustControlsOnErase(ControlSet& ctrls, int para, size_t a, size_t b) {
    if (b <= a) return;
    const size_t len = b - a;
    for (Control& c : ctrls.mutableAll()) {
        // 两个端点规则相同：删掉的位置在端点之前就左移，被删区间覆盖就夹到 a。
        auto fix = [&](TextPos& e) {
            if (e.para != para) return;
            if (e.offset <= a) return;
            if (e.offset >= b) {
                e.offset -= len;
            } else {
                e.offset = a;  // 端点落在被删区间里 → 夹到删除起点
            }
        };
        fix(c.range.start);
        fix(c.range.end);
    }
}

}  // namespace ced
