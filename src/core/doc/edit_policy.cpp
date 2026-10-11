#include "core/doc/edit_policy.h"

namespace heditor {
namespace {

/**
 * 文档里有没有任何可编辑控件。
 * 没有的话，"表单模式"退化为文档模式，而不是"什么都不让改"。
 * 理由：一个还没配控件的文档切到表单模式，用户期望的是"跟平时一样"，
 * 而不是突然一个字都打不进去 —— 后者会被当成"功能坏了"，没人会想到
 * 是策略在生效。把这条写进注释，是因为它是个刻意的安全阀，
 * 不是遗漏（将来若要做"严格表单"，应该另加一个 EditMode，不要改这里）。
 */
bool hasEditableControl(const ControlSet& ctrls) {
    for (const Control& c : ctrls.all()) {
        if (!c.read_only) return true;
    }
    return false;
}

/**
 * b 的前一个码点位置。用于"删除区间里最后一个字符在哪"。
 * 段首（b.offset == 0）返回 false —— 档 1 的编辑不跨段，这属于非法输入。
 */
bool prevPos(const TextPos& b, TextPos* out) {
    if (b.offset == 0) return false;
    *out = TextPos{b.para, b.offset - 1};
    return true;
}

/**
 * "编辑已经明确属于 preferred 这个控件"时的归属判定。
 * 带回 true 表示用它的结论；false 表示它不适用，调用方回落到最内层判定。
 * 只在 preferred 真的覆盖 pos 时才生效 —— 防御性地挡掉"提示与位置不一致"
 * （光标被改到别处之后，旧提示不该继续指挥编辑）。
 */
bool preferredDecides(const ControlSet& ctrls, const TextPos& pos, int preferred, bool* editable) {
    if (preferred < 0 || preferred >= ctrls.size()) return false;
    const Control& c = ctrls.at(preferred);
    if (!rangeAllowsInsert(c.range, pos)) return false;
    *editable = EditPolicy::isEditable(c);
    return true;
}

}  // namespace

const char* editModeName(EditMode m) {
    switch (m) {
        case EditMode::Form:
            return "表单模式";
        case EditMode::ReadOnly:
            return "只读模式";
        default:
            return "文档模式";
    }
}

EditMode editModeFromCode(int code) {
    switch (code) {
        case 1:
            return EditMode::Form;
        case 2:
            return EditMode::ReadOnly;
        default:
            return EditMode::Document;
    }
}

bool EditPolicy::canInsert(const ControlSet& ctrls, EditMode mode, const TextPos& pos,
                           int preferred) {
    if (mode == EditMode::Document) return true;
    if (mode == EditMode::ReadOnly) return false;
    if (!hasEditableControl(ctrls)) return true;  // 未配控件 → 退化（见上面的说明）

    bool editable = false;
    if (preferredDecides(ctrls, pos, preferred, &editable)) return editable;

    // 用 include_end = true：在控件末尾追加是合法编辑。
    //   取最内层控件，于是"外层可编辑、内层只读"时内层优先 —— 这就是嵌套语义。
    const int inner = ctrls.innermostAt(pos, /*include_end=*/true);
    if (inner < 0) return false;  // 落在所有控件之外
    return isEditable(ctrls.at(inner));
}

bool EditPolicy::canErase(const ControlSet& ctrls, EditMode mode, const TextPos& a,
                          const TextPos& b) {
    if (mode == EditMode::Document) return true;
    if (mode == EditMode::ReadOnly) return false;
    if (!(a < b)) return false;                   // 空区间：没有东西可删
    if (!hasEditableControl(ctrls)) return true;  // 未配控件 → 退化

    // 跨段删除在档 1 本来就不支持；保守拒绝，不要给出"允许"的错误信号。
    if (a.para != b.para) return false;

    TextPos last{};
    if (!prevPos(b, &last)) return false;

    // 删除区间两端都必须落在同一个控件的内容里，且该控件可编辑。
    // 只查一端是不够的：区间可能从一个控件跨到另一个（或跨出控件外）。
    const int ia = ctrls.innermostAt(a, /*include_end=*/false);
    const int ib = ctrls.innermostAt(last, /*include_end=*/false);
    if (ia < 0 || ia != ib) return false;
    return isEditable(ctrls.at(ia));
}

bool EditPolicy::canSplit(const ControlSet& ctrls, EditMode mode, const TextPos& pos,
                          int preferred) {
    // 分段 = 在断点处插入一个段落分隔符。所以判据与"在这里插字"完全相同 ——
    // 刻意不另立一套：两者都是"这个位置能不能被写入"，分开写迟早会不一致。
    return canInsert(ctrls, mode, pos, preferred);
}

bool EditPolicy::canMerge(const ControlSet& ctrls, EditMode mode, const TextPos& join,
                          int preferred) {
    // 合并 = 删掉 join 处的段落分隔符（后一段的文字向前搬）。
    return canInsert(ctrls, mode, join, preferred);
}

}  // namespace heditor
