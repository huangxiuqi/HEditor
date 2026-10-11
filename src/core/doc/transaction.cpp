#include "core/doc/transaction.h"

#include "core/doc/normalize.h"

namespace heditor {

namespace {

StepResult failure(const std::string& what) {
    StepResult r;
    r.error = what;
    return r;
}

StepResult success() {
    StepResult r;
    r.ok = true;
    return r;
}

}  // namespace

bool canMerge(const Transaction& older, const Transaction& newer) {
    if (newer.steps.size() != 1) return false;
    const auto* fresh = std::get_if<InsertText>(&newer.steps[0]);
    if (fresh == nullptr) return false;

    // 旧事务整串都必须是同标记的插入，末尾位置由最后一笔算出来。
    FlowPos tail = 0;
    bool any = false;
    for (const Step& s : older.steps) {
        const auto* hit = std::get_if<InsertText>(&s);
        if (hit == nullptr || hit->marks != fresh->marks) return false;
        const FlowPos end = hit->at + static_cast<FlowPos>(hit->cps.size());
        if (!any) {
            tail = end;
            any = true;
        } else if (end > tail) {
            tail = end;
        }
    }
    return any && fresh->at == tail;
}

void UndoStack::revert(const std::vector<Step>& applied) {
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        applyStep(tree_, invertStep(*it));
    }
    normalizeDocument(tree_);
}

StepResult UndoStack::commit(Transaction tx) {
    std::vector<Step> applied;
    for (const Step& s : tx.steps) {
        const StepResult r = applyStep(tree_, s);
        if (!r.ok) {
            revert(applied);
            return r;
        }
        applied.push_back(s);
    }
    normalizeDocument(tree_);
    undone_.clear();

    if (!done_.empty() && canMerge(done_.back(), tx)) {
        Transaction& top = done_.back();
        top.steps.insert(top.steps.end(), tx.steps.begin(), tx.steps.end());
        top.selection_after = tx.selection_after;
        return success();
    }
    done_.push_back(std::move(tx));
    return success();
}

StepResult UndoStack::undo(Range* restore) {
    if (done_.empty()) return failure("没有可撤销的事务");

    Transaction tx = std::move(done_.back());
    done_.pop_back();

    std::vector<Step> undone;
    for (auto it = tx.steps.rbegin(); it != tx.steps.rend(); ++it) {
        const StepResult r = applyStep(tree_, invertStep(*it));
        if (!r.ok) {
            // 逆操作一定可应用（它由步骤本身算出来，而规范化不改流位置）。走到这里说明步骤与
            // 文档对不上了：把已经撤掉的再应用回去（它们本身是正向步骤），事务放回栈顶，
            // 让调用方看到"撤销没生效"，而不是留下一个撤到一半的文档。
            for (auto back = undone.rbegin(); back != undone.rend(); ++back) {
                applyStep(tree_, *back);
            }
            normalizeDocument(tree_);
            done_.push_back(std::move(tx));
            return failure("逆操作不可应用：" + r.error);
        }
        undone.push_back(*it);
    }
    normalizeDocument(tree_);
    if (restore != nullptr) *restore = tx.selection_before;
    undone_.push_back(std::move(tx));
    return success();
}

StepResult UndoStack::redo(Range* restore) {
    if (undone_.empty()) return failure("没有可重做的事务");

    Transaction tx = std::move(undone_.back());
    undone_.pop_back();

    std::vector<Step> applied;
    for (const Step& s : tx.steps) {
        const StepResult r = applyStep(tree_, s);
        if (!r.ok) {
            revert(applied);
            undone_.push_back(std::move(tx));
            return failure("重做失败：" + r.error);
        }
        applied.push_back(s);
    }
    normalizeDocument(tree_);
    if (restore != nullptr) *restore = tx.selection_after;
    done_.push_back(std::move(tx));
    return success();
}

}  // namespace heditor
