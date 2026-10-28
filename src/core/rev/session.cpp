/**
 * 会话层的实现。
 *
 * 每个 kind 一行分派，全部落到 rev 的原语上 —— 这里不重算任何落点，也不复制原语的规则。
 * 会增删原子的 kind 在此处定：没写进 Edit 的那些字段保持默认值，正是"按这种编辑，它不适用"。
 */
#include "core/rev/session.h"

#include "core/rev/steps.h"

namespace heditor::rev {

namespace {

/**
 * 把一步编辑落成新修订。任何 kind 都返回 EditResult，失败原因原样带出去。
 */
EditResult run(const Doc& doc, const Edit& e, NodeId* created) {
    switch (e.kind) {
        case Edit::Kind::InsertText:
            return insertText(doc, e.para, e.cp, e.text);
        case Edit::Kind::EraseText:
            return eraseText(doc, e.para, e.cp, e.count);
        case Edit::Kind::SplitPara:
            return splitPara(doc, e.para, e.cp);
        case Edit::Kind::MergePara:
            return mergePara(doc, e.para);
        case Edit::Kind::InsertControl:
            return insertControl(doc, e.para, e.cp, e.control, e.text, created);
        case Edit::Kind::SetHidden:
            return setHidden(doc, e.node, e.hidden);
    }
    EditResult bad;
    bad.error = "不认识的操作";
    return bad;
}

}  // namespace

Session::Session(Doc doc, int limit) : doc_(std::move(doc)), limit_(limit < 0 ? 0 : limit) {}

void Session::setLimit(int limit) {
    limit_ = limit < 0 ? 0 : limit;
    if (limit_ <= 0) return;
    while (static_cast<int>(undo_.size()) > limit_) undo_.erase(undo_.begin());
    while (static_cast<int>(selection_undo_.size()) > limit_) {
        selection_undo_.erase(selection_undo_.begin());
    }
}

void Session::pushUndo(Doc doc) {
    undo_.push_back(std::move(doc));
    // 超出上限时丢最旧的一份。丢的是"更早的状态"，不是"某一步的逆操作"：
    // 撤销仍然一步步往回走，只是走不到最开头了。
    if (static_cast<int>(undo_.size()) > limit_) {
        undo_.erase(undo_.begin());
        if (!selection_undo_.empty()) selection_undo_.erase(selection_undo_.begin());
    }
}

void Session::recordSelection(SelectionSnapshot before, SelectionSnapshot after, bool merged) {
    if (merged && !selection_undo_.empty()) {
        selection_undo_.back().after = after;
        return;
    }
    selection_undo_.push_back(SelectionStep{before, after});
    selection_redo_.clear();
    if (static_cast<int>(selection_undo_.size()) > limit_) {
        selection_undo_.erase(selection_undo_.begin());
    }
}

bool Session::undoSelection(SelectionSnapshot* out) {
    if (selection_undo_.empty()) return false;
    const SelectionStep step = selection_undo_.back();
    selection_undo_.pop_back();
    selection_redo_.push_back(step);
    if (out != nullptr) *out = step.before;
    return true;
}

bool Session::redoSelection(SelectionSnapshot* out) {
    if (selection_redo_.empty()) return false;
    const SelectionStep step = selection_redo_.back();
    selection_redo_.pop_back();
    selection_undo_.push_back(step);
    if (out != nullptr) *out = step.after;
    return true;
}

Applied Session::apply(const Edit& edit) {
    Applied out;
    NodeId created = kInvalidNodeId;
    const EditResult r = run(doc_, edit, &created);
    if (!r.ok) {
        out.error = r.error.empty() ? std::string("编辑被拒绝") : r.error;
        return out;
    }
    // 内容没变（比如在第 0 个码点删 0 个）就不该占一条撤销：撤销栈里留一份与当前相同的
    // 修订，按一次撤销看起来"什么都没发生"，用户会以为撤销坏了。
    const bool changed = (r.doc.lo != doc_.lo || r.doc.hi != doc_.hi);
    if (!changed) {
        out.ok = true;
        out.created = created;
        out.changed = false;
        return out;
    }
    pushUndo(std::move(doc_));
    last_merged_ = false;
    doc_ = r.doc;
    redo_.clear();
    ++generation_;
    out.ok = true;
    out.created = created;
    out.changed = true;
    return out;
}

Applied Session::applySteps(const std::vector<Step>& steps, FlowVisibility vis, MergeRun merge) {
    Applied out;
    if (steps.empty()) {
        out.ok = true;
        return out;
    }
    // 限定到命名空间：Session 自己的 applySteps 会把同名函数藏起来。
    const EditResult r = ::heditor::rev::applySteps(doc_, steps, vis);
    if (!r.ok) {
        out.error = r.error;
        return out;
    }
    out.ok = true;
    if (r.doc.lo == doc_.lo && r.doc.hi == doc_.hi) return out;
    // 连续拖拽只压一份旧修订：后续帧不写撤销栈，撤销一下直接回到拖拽开始之前 ——
    //   否则一次拖拽会留下几十步，用户要按几十次撤销。
    const bool merge_in = (merge == MergeRun::Continue) && last_merged_;
    if (!merge_in) pushUndo(doc_);
    last_merged_ = (merge != MergeRun::None);
    doc_ = r.doc;
    redo_.clear();
    ++generation_;
    out.changed = true;
    out.merged = merge_in;
    return out;
}

bool Session::undo() {
    if (undo_.empty()) return false;
    // 撤销之后"上一次提交"已经不在栈顶了，再合并会把两步并成一步错的历史。
    last_merged_ = false;
    redo_.push_back(std::move(doc_));
    doc_ = undo_.back();
    undo_.pop_back();
    ++generation_;
    return true;
}

bool Session::redo() {
    if (redo_.empty()) return false;
    last_merged_ = false;
    pushUndo(doc_);
    doc_ = redo_.back();
    redo_.pop_back();
    ++generation_;
    return true;
}

}  // namespace heditor::rev
