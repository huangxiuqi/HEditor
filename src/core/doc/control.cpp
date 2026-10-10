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
 * 区间伸缩：在 (para, at) 处插入 n 个码点。
 * 两步：先定落笔点归谁，再按归属选端点规则。归属与 EditPolicy::canInsert、光标判定
 * 同一口径（最内层控件，含末尾）—— 相邻两字段共享边界时，同一个位置既是前一个字段的
 * 末尾、又是后一个字段的开头，字只能进其中一个；两边都按"在末尾追加"伸缩就会得到两段
 * 重叠的区间，表现是共享边界上的括号顺序从 ][ 变成 []。
 * 吸收者 = 落笔点的主人，以及包住主人、且新字接在主人末尾的外层控件：起点 > at 才
 * 右移（起点停在 at 处 = 在字段开头打字），终点 >= at 就右移（在末尾追加，区间变长）。
 * 其余控件：起点 >= at 右移（内容整体后移，新字不许落进来），终点 > at 才右移
 * （终点正好在 at 说明内容到此为止，新字归别人）。
 * 空控件（start == end）两端点必须同进同退，否则会写出 start > end 的非法区间；
 * 唯一例外是它就是落笔点的主人 —— 空字段里打字，新字要填进来。
 */
void adjustControlsOnInsert(ControlSet& ctrls, int para, size_t at, size_t n) {
    if (n == 0) return;
    const TextPos pos{para, at};
    const int owner = ctrls.innermostAt(pos, /*include_end=*/true);
    // 判"谁包住谁"必须用调整之前的区间：循环里前面的控件会先被改，
    //   拿改过的区间去比，结果会随控件顺序变。
    const ControlRange owner_range = (owner >= 0) ? ctrls.at(owner).range : ControlRange{};
    // 外层控件要不要跟着变长：只有当新字接在"主人的末尾"时，外层必须继续盖住主人，
    //   否则外层 [0,9) 与增长后的内层 [5,10) 会互相穿过 —— 比画错括号更糟的状态。
    //   空主人不算：它只是停在外层末尾上的一个点，新字是它的第一个字，外层到 at 为止，
    //   两者相邻不相交（这恰恰是"相邻字段"该有的形态，见单测 [18] 的 (d)）。
    const bool owner_tail_grown = (owner >= 0) && owner_range.start != owner_range.end &&
                                  owner_range.end.para == para && owner_range.end.offset == at;
    for (Control& c : ctrls.mutableAll()) {
        const bool is_owner = (owner >= 0) && (&c == &ctrls.at(owner));
        if (!is_owner && c.range.start == c.range.end && c.range.start.para == para) {
            if (c.range.start.offset >= at) {
                c.range.start.offset += n;
                c.range.end.offset += n;
            }
            continue;
        }
        const bool absorbs = is_owner || (owner_tail_grown && rangeCovers(c.range, owner_range));
        if (c.range.start.para == para &&
            (absorbs ? c.range.start.offset > at : c.range.start.offset >= at))
            c.range.start.offset += n;
        if (c.range.end.para == para &&
            (absorbs ? c.range.end.offset >= at : c.range.end.offset > at))
            c.range.end.offset += n;
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
