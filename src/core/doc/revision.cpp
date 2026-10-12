#include "core/doc/revision.h"

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

const char* revisionStateName(RevisionState state) {
    switch (state) {
        case RevisionState::Pending:
            return "待定";
        case RevisionState::Accepted:
            return "已接受";
        case RevisionState::Rejected:
            return "已拒绝";
    }
    return "（未知）";
}

bool isRevisionStep(const Step& step) {
    return std::holds_alternative<InsertText>(step) ||
           std::holds_alternative<InsertFragment>(step) ||
           std::holds_alternative<DeleteRange>(step) ||
           std::holds_alternative<InsertParaMark>(step) ||
           std::holds_alternative<RemoveParaMark>(step);
}

void RevisionLog::reset(const NodeTree& tree) {
    baseline_.assignFrom(tree);
    has_baseline_ = true;
    log_.clear();
}

StepResult RevisionLog::record(const Step& step, const std::string& author,
                               const std::string& time) {
    if (!isRevisionStep(step)) {
        return failure(std::string("修订只记录内容级步骤：") +
                       "结构步骤与属性步骤的位置含槽位，单靠流位置映射搬不动");
    }
    Revision rev;
    rev.step = step;
    rev.author = author;
    rev.time = time;
    log_.push_back(std::move(rev));
    return success();
}

StepResult RevisionLog::accept(std::size_t index) {
    if (index >= log_.size()) return failure("修订下标越界");
    log_[index].state = RevisionState::Accepted;
    return success();
}

StepResult RevisionLog::reject(std::size_t index) {
    if (index >= log_.size()) return failure("修订下标越界");
    if (log_[index].state == RevisionState::Accepted) {
        return failure("这条修订已经折叠进基线，不能再拒绝");
    }
    log_[index].state = RevisionState::Rejected;
    return success();
}

int RevisionLog::count(RevisionState state) const {
    int n = 0;
    for (const Revision& rev : log_) {
        if (rev.state == state) ++n;
    }
    return n;
}

StepResult RevisionLog::expand(NodeTree& out) const {
    out.assignFrom(baseline_);

    // 被拒绝的修订的逆步骤，按记录顺序。后面每一条修订的坐标都要穿过它们搬回来：
    // 逆步骤把位置从"含那条修订的文档"搬回"不含它的文档"，正是这里要的方向。
    std::vector<Step> skipped_inverses;
    for (std::size_t i = 0; i < log_.size(); ++i) {
        const Revision& rev = log_[i];
        if (rev.state == RevisionState::Rejected) {
            skipped_inverses.push_back(invertStep(rev.step));
            continue;
        }

        Step step = rev.step;
        // 倒着搬：最后被跳过的那条先搬，于是每搬一次时文档里都还带着被搬的那条修订，
        // 映射的前提成立。正着搬会在第二次起失去这个前提。
        for (std::size_t k = skipped_inverses.size(); k-- > 0;) {
            if (!mapStepPositions(&step, skipped_inverses[k], Assoc::Before)) {
                out.assignFrom(baseline_);
                return failure("第 " + std::to_string(i) +
                               " 条修订的坐标搬不动（步骤种类不支持映射）");
            }
        }

        const StepResult r = applyStep(out, step);
        if (!r.ok) {
            // 不把展开到一半的文档交出去：调用方拿到失败回执时 out 必须还是基线。
            out.assignFrom(baseline_);
            return failure("第 " + std::to_string(i) + " 条修订展开失败：" + r.error);
        }
    }

    normalizeDocument(out);
    return success();
}

}  // namespace heditor
