#include "core/engine/document_session.h"

namespace heditor {

DocumentSession::CommitOutcome DocumentSession::commitSteps(const std::vector<Step>& steps,
                                                            FlowVisibility vis, rev::MergeRun merge,
                                                            rev::SelectionSnapshot before,
                                                            rev::SelectionSnapshot after) {
    CommitOutcome out;
    if (!impl_) {
        out.applied.error = "没有装载文档";
        return out;
    }
    const rev::Doc old = impl_->doc();
    out.applied = impl_->applySteps(steps, vis, merge);
    if (!out.applied.ok || !out.applied.changed) return out;
    out.damage = rev::damage(old, impl_->doc(), vis);
    impl_->recordSelection(before, after, out.applied.merged);
    return out;
}

bool DocumentSession::undo(rev::SelectionSnapshot* selection, rev::Damage* damage,
                           FlowVisibility vis) {
    if (!impl_) return false;
    const rev::Doc old = impl_->doc();
    if (!impl_->undo()) return false;
    if (damage != nullptr) *damage = rev::damage(old, impl_->doc(), vis);
    if (selection != nullptr) impl_->undoSelection(selection);
    return true;
}

bool DocumentSession::redo(rev::SelectionSnapshot* selection, rev::Damage* damage,
                           FlowVisibility vis) {
    if (!impl_) return false;
    const rev::Doc old = impl_->doc();
    if (!impl_->redo()) return false;
    if (damage != nullptr) *damage = rev::damage(old, impl_->doc(), vis);
    if (selection != nullptr) impl_->redoSelection(selection);
    return true;
}

}  // namespace heditor
