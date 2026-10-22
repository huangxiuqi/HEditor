/**
 * 表单控件：文档流里的区间标注。
 * 控件是覆盖在码点区间上的标注对象，不是矩形。它的内容就是文档里那段普通文字，
 * 于是断行、分页、竖排、可变字体、字素簇吸附与增量重绘全部自动继承，不需要第二套
 * 布局系统；表单模式只负责显示边界，阅读与打印时看不出区别（同类设计见 OOXML 的
 * 内容控件 w:sdt）。若改成"固定矩形加独立排版"，整套排版管线都要重写。
 *
 * 区间不是独立状态：它是投影产物的派生结果 —— 引擎按产物给的跨度建控件集合
 * （见 EditorEngine::exportControls），而跨度由投影走树时记下。于是"控件内
 * 打字就变长、控件前插入就整体右移、删掉区间内的字就缩短"这件事不用谁来维护：内容在
 * 树上变，边界跟着变，导出一遍自然就对上了。手工维护区间那一套（编辑之后逐个伸缩端点）
 * 是会脱节的 —— 它要求"每一次内容改动"与"每一次端点适配"永远配对，漏一处就会让控件
 * 与它标注的内容错位，而错位之后界面看着仍然正常。
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/doc/document.h"

namespace heditor {
/**
 * 控件的业务类型。决定的是"值怎么校验/怎么显示"，
 * 而不是"怎么画" —— 画法由引擎统一决定（底纹 + 边框）。
 */
enum class ControlKind : int {
    TextField = 0,    // 文本输入（默认）
    NumberField = 1,  // 数值（将来接 range/精度）
    DateField = 2,    // 日期（将来接日历浮层）
};

/**
 * 种类名，用于诊断与报告。
 * @param k 种类
 * @return 名字
 */
const char* controlKindName(ControlKind k);

/**
 * 代码转种类（落盘用的是代码）。
 * @param code 代码
 * @return 种类；不认识的代码按文本输入处理
 */
ControlKind controlKindFromCode(int code);

/**
 * 区间：半开 [start, end)
 * 端点是 (段, 段内码点偏移)，与文档、整形 cluster、光标同一把尺子。
 *
 * 允许跨段（start.para != end.para）：控件是容器，里面可以放文字、嵌套控件、甚至表格
 * （见 node.h 的 ContentControl 与 isContainer），所以它跨段落是正常形态而不是特例。
 * 端点的伸缩由各编辑入口按"内容归属"处理，不能拿段号做假设 —— 在控件内容里分段时，
 * 起止端点本来就该分落两段。
 */
struct ControlRange {
    /**
     * 区间起点（含）。
     */
    TextPos start{};

    /**
     * 区间终点：内容范围是半开，插入点判定按闭区间看。
     */
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

/**
 * 区间树判父子关系用的包含：rangeCovers 再排除掉"完全重合"这一种。
 *
 * 完全重合的两段区间谁也不是谁的父（互为兄弟）—— 不排除的话它们会互相认父，
 * 深度当场失控（两段的 depth 都会涨到上限，括号也跟着画成嵌套的）。
 *   三连可编辑字段把后两个都删空之后就是这个形状：两段都成了零宽、且落在同一个偏移上。
 *
 * 零宽区间落在邻居端点上（[5,7) 与 [7,7)）仍然算包含，这一条必须留着：
 *   它是"刚删空的字段在自己那个位置上是最内层"的来源。少了它，位置 7 会被左邻居
 *   抢走，那个字段就再也接不住输入 —— 正是"中间控件删空后光标进不去"那个缺陷。
 */
inline bool rangeContainsForNesting(const ControlRange& outer, const ControlRange& inner) {
    if (outer.start == inner.start && outer.end == inner.end) return false;
    return rangeCovers(outer, inner);
}

struct Control {
    /**
     * 它标注的码点区间。
     */
    ControlRange range;

    /**
     * 业务类型（决定校验与显示，不决定画法）。
     */
    ControlKind kind = ControlKind::TextField;

    /**
     * 业务标识（如 chief_complaint）。
     */
    std::string id;

    /**
     * 显示标签（如 主诉）。
     */
    std::string label;

    /**
     * 内容不可编辑。
     */
    bool read_only = false;

    /**
     * 嵌套深度（0 = 顶层）。这一项与 parent / children 都由 ControlSet::normalize()
     * 算出来，外部不要手填。
     */
    int depth = 0;

    /**
     * 最小包围它的控件下标；-1 = 顶层。
     */
    int parent = -1;

    /**
     * 直接子控件的下标。
     */
    std::vector<int> children;
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
     * @param c 控件
     * @return 它在集合里的下标
     */
    int add(const Control& c);

    /**
     * 重建区间树（排序 + 父子 + depth）。加/改控件后必须调一次。
     */
    void normalize();

    /**
     * 控件个数。
     * @return 个数
     */
    int size() const { return static_cast<int>(items_.size()); }

    /**
     * 一个控件都没有。
     * @return true = 空
     */
    bool empty() const { return items_.empty(); }

    /**
     * 按下标取控件（只读）。
     * @param i 下标
     * @return 控件
     */
    const Control& at(int i) const { return items_[static_cast<size_t>(i)]; }

    /**
     * 按下标取控件（可写）。
     * @param i 下标
     * @return 控件
     */
    Control& at(int i) { return items_[static_cast<size_t>(i)]; }

    /**
     * 全部控件（按 add 的顺序，不是区间顺序）。
     * @return 控件数组
     */
    const std::vector<Control>& all() const { return items_; }

    /**
     * 覆盖 pos 的最内层控件下标。
     * include_end = true  → 区间按闭区间看（用于"插入点/光标能不能停在这儿"）
     * include_end = false → 半开（用于"这个位置是不是控件的内容"）
     * 没有任何控件覆盖时返回 -1。
     * @param pos 位置
     * @param include_end true = 按闭区间看（插入点判定）
     * @return 最内层控件的下标；没有覆盖时为 -1
     */
    int innermostAt(const TextPos& pos, bool include_end = true) const;

    /**
     * 与 [a, b) 有交集的控件（按 depth 降序 = 由内到外）。
     * @param r 区间（半开）
     * @return 有交集的控件下标，按深度降序（由内到外）
     */
    std::vector<int> overlapping(const ControlRange& r) const;

private:
    std::vector<Control> items_;

    /**
     * 按区间起点排序后的下标序（外层在前）。
     */
    std::vector<int> order_;
};

}  // namespace heditor
