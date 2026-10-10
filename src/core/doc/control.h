/**
 * 表单控件：文档流里的区间标注。
 * 控件是覆盖在码点区间上的标注对象，不是矩形。它的内容就是文档里那段普通文字，
 * 于是断行、分页、竖排、可变字体、字素簇吸附与增量重绘全部自动继承，不需要第二套
 * 布局系统；表单模式只负责显示边界，阅读与打印时看不出区别（同类设计见 OOXML 的
 * 内容控件 w:sdt）。若改成"固定矩形加独立排版"，整套排版管线都要重写。
 *
 * 区间是活的：控件内打字则变长，控件前插入则整体右移，删掉区间内的字则缩短。
 * 由 EditorEngine 在每次编辑后维护（adjustControlsOnInsert / adjustControlsOnErase）；
 * 漏掉这一步，控件会与它标注的内容脱节，既拒绝该允许的输入又允许该拒绝的输入。
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/doc/document.h"

namespace ced {

/**
 * 控件的业务类型。决定的是"值怎么校验/怎么显示"，
 * 而不是"怎么画" —— 画法由引擎统一决定（底纹 + 边框）。
 */
enum class ControlKind : int {
    TextField = 0,    // 文本输入（默认）
    NumberField = 1,  // 数值（将来接 range/精度）
    DateField = 2,    // 日期（将来接日历浮层）
};

const char* controlKindName(ControlKind k);
ControlKind controlKindFromCode(int code);

/**
 * 区间：半开 [start, end)
 * 端点是 (段, 段内码点偏移)，与文档、整形 cluster、光标同一把尺子。
 * 允许跨段（start.para != end.para），但编辑时的伸缩只处理同段端点 ——
 * 病历表单里"一个字段 = 一段"是绝大多数情况，跨段控件先只做标注与判定。
 */
struct ControlRange {
    TextPos start{};
    TextPos end{};
};

/**
 * start <= pos <= end（闭区间语义）。
 * 为什么插入点判定要用闭区间：在区间末尾追加是合法编辑
 * （"主诉"栏打到最后一个字之后继续打），插进去的内容仍然属于这个控件。
 * 而 [start, end) 是内容的范围，两者差一个位置，不能混用。
 */
inline bool rangeAllowsInsert(const ControlRange& r, const TextPos& pos) {
    if (pos < r.start) return false;
    if (r.end < pos) return false;
    return true;
}

/**
 * start <= pos < end（半开语义）：pos 落在控件内容里。
 */
inline bool rangeContainsContent(const ControlRange& r, const TextPos& pos) {
    if (pos < r.start) return false;
    if (!(pos < r.end)) return false;  // 用 !(pos < end) 表达 pos >= end
    return true;
}

/**
 * outer 是否覆盖 inner（闭区间语义，允许相等）。
 */
inline bool rangeCovers(const ControlRange& outer, const ControlRange& inner) {
    if (inner.start < outer.start) return false;
    if (outer.end < inner.end) return false;
    return true;
}

struct Control {
    ControlRange range;
    ControlKind kind = ControlKind::TextField;
    std::string id;     // 业务标识，如 "chief_complaint"
    std::string label;  // 显示标签，如 "主诉"
    bool read_only = false;

    /**
     * 以下由 ControlSet::normalize() 计算，外部不要手填
     */
    int depth = 0;              // 嵌套深度（0 = 顶层）
    int parent = -1;            // 最小包围它的控件下标；-1 = 顶层
    std::vector<int> children;  // 直接子控件下标
};

/**
 * 控件集合：维护区间树，并提供"某个位置属于哪个控件"的查询。
 * 查询的口径统一走 innermostAt：嵌套时内层优先，
 * 这是嵌套语义的全部内容（"这个区域只有这一小块是锁死的"）。
 */
class ControlSet {
public:
    void clear();
    /**
     * 追加一个控件，返回下标（不自动 normalize，批量加完调一次）。
     */
    int add(const Control& c);
    /**
     * 重建区间树（排序 + 父子 + depth）。加/改控件后必须调一次。
     */
    void normalize();

    int size() const { return static_cast<int>(items_.size()); }
    bool empty() const { return items_.empty(); }
    const Control& at(int i) const { return items_[static_cast<size_t>(i)]; }
    Control& at(int i) { return items_[static_cast<size_t>(i)]; }
    const std::vector<Control>& all() const { return items_; }

    /**
     * 覆盖 pos 的最内层控件下标。
     * include_end = true  → 区间按闭区间看（用于"插入点/光标能不能停在这儿"）
     * include_end = false → 半开（用于"这个位置是不是控件的内容"）
     * 没有任何控件覆盖时返回 -1。
     */
    int innermostAt(const TextPos& pos, bool include_end = true) const;

    /**
     * 与 [a, b) 有交集的控件（按 depth 降序 = 由内到外）。
     */
    std::vector<int> overlapping(const ControlRange& r) const;

    std::vector<Control>& mutableAll() { return items_; }

private:
    std::vector<Control> items_;
    std::vector<int> order_;  // 按区间起点排序后的下标序（外层在前）
};

/**
 * 区间伸缩：编辑之后必须调这两个，否则控件会脱离它标注的内容
 * 规则只有两条，但对两种情况（控件内编辑 / 控件外编辑）同时成立：
 * 插入 (para, at, n)：端点在 at 之后（含 at）的 → 右移 n
 * ⟹ 在控件内插入 → end 右移 = 区间变长（对）
 * ⟹ 在控件前插入 → start/end 都右移 = 区间平移（对）
 * 删除 (para, [a, b))：端点在 a 之前 → 不动；在 b 之后 → 左移 len；
 * 落在 [a, b) 内 → 夹到 a
 */
void adjustControlsOnInsert(ControlSet& ctrls, int para, size_t at, size_t n);
void adjustControlsOnErase(ControlSet& ctrls, int para, size_t a, size_t b);

/**
 * 分段：在 pos 处把一段断成两段（原段保持段号，其后半段成为 pos.para + 1 段）。
 *
 * 端点规则唯一依据是"内容跟着谁走"：端点 >= pos 的内容被搬进了新段，于是
 * 段号 +1、偏移 -= pos.offset；端点 < pos 的留在原段不动；段号大于 pos.para
 * 的端点整体后移一段。两端点同时按这一条走，所以不可能出现 start > end 的
 * 不合法区间（"恰好落在断点上的空字段"会整对搬到新段段首 —— 与"回车之后
 * 光标落在新段段首"同一口径）。
 */
void adjustControlsOnSplitPara(ControlSet& ctrls, const TextPos& pos);

/**
 * 合并：把 para + 1 段并入 para 段（para 段在合并前的长度为 prev_len），
 * 调用方随后会删掉 para + 1 段。
 *
 * 同理：para + 1 段的端点重基到 para 段（段号 -1、偏移 += prev_len）；
 * 更靠后段的端点段号 -1；para 段内的端点不动 —— 它前面的内容没变，
 * 追加进来的内容在它之后。
 * 端点恰好等于 (para, prev_len) 的控件不会包住追加进来的内容，这是对的：
 * 它标注的那串文字在合并前后是同一串。
 */
void adjustControlsOnMergePara(ControlSet& ctrls, int para, size_t prev_len);

}  // namespace ced
