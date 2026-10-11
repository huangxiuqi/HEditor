#include "core/doc/validation.h"

namespace heditor {

namespace {

/**
 * 目标是不是还挂在文档上的内容控件。
 *
 * 必须连"挂没挂"一起判：被摘下的子树留在池里（撤销要把它们原样挂回），所以按 id 查得到它，
 * 但它已经不在文档里了 —— 只看非空会把删掉的东西当成活的，裁决结果就会写到一个不存在的位置上。
 */
bool liveControl(const NodeTree& tree, NodeId id) {
    const Node* n = tree.find(id);
    return n != nullptr && n->kind == NodeKind::ContentControl && isAttached(tree, id);
}

}  // namespace

const char* validationStateName(ValidationState s) {
    switch (s) {
        case ValidationState::None:
            return "none";
        case ValidationState::Pending:
            return "pending";
        case ValidationState::Invalid:
            return "invalid";
    }
    return "（未知）";
}

ChangeGate::ChangeGate(ControlApi& api) : api_(api) {}

bool ChangeGate::take(int64_t token, Candidate* out) {
    const auto it = pending_.find(token);
    if (it == pending_.end()) return false;
    *out = it->second;
    pending_.erase(it);
    const auto by = by_target_.find(out->edit.id);
    if (by != by_target_.end() && by->second == token) by_target_.erase(by);
    return true;
}

BeginResult ChangeGate::begin(const ValueEdit& edit) {
    BeginResult out;
    if (edit.id == kInvalidNodeId) {
        out.error = "等待的目标要按 id 给出：按 key 展开成多个之后没法回答结果给谁";
        return out;
    }
    if (!liveControl(api_.tree(), edit.id)) {
        out.error = "等待的目标不是文档里的内容控件";
        return out;
    }

    // 最后交互为准：同一控件上还挂着等待时，旧的那个直接作废。
    cancel(edit.id);

    Candidate c;
    c.token = next_token_++;
    c.edit = edit;
    pending_[c.token] = c;
    by_target_[edit.id] = c.token;
    states_[edit.id] = Validation{ValidationState::Pending, ""};
    out.ok = true;
    out.token = c.token;
    return out;
}

bool ChangeGate::cancel(NodeId target) {
    const auto by = by_target_.find(target);
    if (by == by_target_.end()) return false;
    Candidate dropped;
    take(by->second, &dropped);
    states_[target] = Validation{ValidationState::None, ""};
    return true;
}

GateResult ChangeGate::allow(int64_t token) {
    Candidate c;
    if (!take(token, &c)) return GateResult{GateOutcome::Dropped, "这次等待已经不作数了"};

    // 等待期间文档可能被别的编辑改过（控件被删、段落被合并）。结果到达时先确认锚点仍指向
    // 同一个控件，失效就丢弃、不做补偿动作 —— 补偿只会把一次已经过期的决定硬套到新内容上。
    if (!liveControl(api_.tree(), c.edit.id)) {
        states_.erase(c.edit.id);
        return GateResult{GateOutcome::Dropped, "等待期间目标已失效"};
    }

    const BatchResult r = api_.setValues({c.edit});
    if (!r.ok) {
        states_[c.edit.id] = Validation{ValidationState::Invalid, r.error};
        return GateResult{GateOutcome::Failed, r.error};
    }
    states_[c.edit.id] = Validation{ValidationState::None, ""};
    return GateResult{GateOutcome::Applied, ""};
}

GateResult ChangeGate::deny(int64_t token, const std::string& reason) {
    Candidate c;
    if (!take(token, &c)) return GateResult{GateOutcome::Dropped, "这次等待已经不作数了"};
    states_[c.edit.id] = Validation{ValidationState::Invalid, reason};
    return GateResult{GateOutcome::Denied, reason};
}

GateResult ChangeGate::expire(int64_t token) { return deny(token, "校验超时"); }

void ChangeGate::setValidation(NodeId id, ValidationState state, const std::string& message) {
    states_[id] = Validation{state, message};
}

Validation ChangeGate::stateOf(NodeId id) const {
    const auto it = states_.find(id);
    return it == states_.end() ? Validation{} : it->second;
}

bool ChangeGate::isPending(NodeId id) const { return by_target_.find(id) != by_target_.end(); }

void ChangeGate::sweepStale() {
    for (auto it = states_.begin(); it != states_.end();) {
        if (!liveControl(api_.tree(), it->first)) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (!liveControl(api_.tree(), it->second.edit.id)) {
            by_target_.erase(it->second.edit.id);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace heditor
