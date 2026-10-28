#include "core/engine/document_session.h"

namespace heditor {

DocumentSession::CommitOutcome DocumentSession::commitSteps(const std::vector<Step>& steps,
                                                            FlowVisibility vis, doc::MergeRun merge,
                                                            doc::SelectionSnapshot before,
                                                            doc::SelectionSnapshot after) {
    CommitOutcome out;
    if (!impl_) {
        out.applied.error = "没有装载文档";
        return out;
    }
    const doc::Doc old = impl_->doc();
    out.applied = impl_->applySteps(steps, vis, merge);
    if (!out.applied.ok || !out.applied.changed) return out;
    out.damage = doc::damage(old, impl_->doc(), vis);
    impl_->recordSelection(before, after, out.applied.merged);
    return out;
}

bool DocumentSession::undo(doc::SelectionSnapshot* selection, doc::Damage* damage,
                           FlowVisibility vis) {
    if (!impl_) return false;
    const doc::Doc old = impl_->doc();
    if (!impl_->undo()) return false;
    if (damage != nullptr) *damage = doc::damage(old, impl_->doc(), vis);
    if (selection != nullptr) impl_->undoSelection(selection);
    return true;
}

bool DocumentSession::redo(doc::SelectionSnapshot* selection, doc::Damage* damage,
                           FlowVisibility vis) {
    if (!impl_) return false;
    const doc::Doc old = impl_->doc();
    if (!impl_->redo()) return false;
    if (damage != nullptr) *damage = doc::damage(old, impl_->doc(), vis);
    if (selection != nullptr) impl_->redoSelection(selection);
    return true;
}

}  // namespace heditor
