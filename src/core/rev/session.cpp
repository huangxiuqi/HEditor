/**
 * 会话层的实现。
 *
 * 每个 kind 一行分派，全部落到 rev 的原语上 —— 这里不重算任何落点，也不复制原语的规则。
 * 会增删原子的 kind 在此处定：没写进 Edit 的那些字段保持默认值，正是"按这种编辑，它不适用"。
 */
#include "core/rev/session.h"

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

void Session::pushUndo(Doc doc) {
    undo_.push_back(std::move(doc));
    // 超出上限时丢最旧的一份。丢的是"更早的状态"，不是"某一步的逆操作"：
    // 撤销仍然一步步往回走，只是走不到最开头了。
    if (static_cast<int>(undo_.size()) > limit_) {
        undo_.erase(undo_.begin());
    }
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
    doc_ = r.doc;
    redo_.clear();
    ++generation_;
    out.ok = true;
    out.created = created;
    out.changed = true;
    return out;
}

bool Session::undo() {
    if (undo_.empty()) return false;
    redo_.push_back(std::move(doc_));
    doc_ = undo_.back();
    undo_.pop_back();
    ++generation_;
    return true;
}

bool Session::redo() {
    if (redo_.empty()) return false;
    pushUndo(doc_);
    doc_ = redo_.back();
    redo_.pop_back();
    ++generation_;
    return true;
}

}  // namespace heditor::rev
