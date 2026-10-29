/**
 * 文档会话服务：拥有当前修订与撤销/重做历史。
 *
 * 阶段五位的第一步：先把会话的所有权从 EditorEngine 里移出来，引擎只通过这一个门面
 * 访问修订。历史与选区快照都在 doc::Session 里，这一层不另存一份。
 */
#pragma once

#include <optional>

#include "core/compile/project.h"
#include "core/doc/session.h"

namespace heditor {

/**
 * 文档会话：装载、修订、撤销/重做与选区快照的唯一所有者。
 */
class DocumentSession {
public:

    /**
     * 有没有装载过文档。
     * @returns true = 会话里有一份修订
     */
    bool has_value() const { return impl_.has_value(); }

    /**
     * 有没有装载过文档（布尔上下文）。
     * @returns true = 会话有效
     */
    explicit operator bool() const { return impl_.has_value(); }

    /**
     * 访问会话。
     * @returns 会话指针；未装载时为 nullptr
     */
    doc::Session* operator->() { return impl_ ? &*impl_ : nullptr; }

    /**
     * 访问会话。
     * @returns 会话指针；未装载时为 nullptr
     */
    const doc::Session* operator->() const { return impl_ ? &*impl_ : nullptr; }

    /**
     * 装入一份修订并建立历史。
     * @param doc 修订
     * @param limit 撤销栈上限
     */
    void emplace(doc::Doc doc, int limit) { impl_.emplace(std::move(doc), limit); }

    /**
     * 丢掉当前会话。
     */
    void reset() { impl_.reset(); }

    /**
     * 一次提交的结果：修订变化与损伤区间。
     */
    struct CommitOutcome {
        /**
         * 会话提交结果。
         */
        doc::Applied applied;

        /**
         * 新旧修订的损伤区间；applied.changed 为 false 时无意义。
         */
        doc::Damage damage;
    };

    /**
     * 提交一批步骤，并把两端选区记进会话历史。
     * @param steps 步骤
     * @param vis 可见性口径
     * @param merge 与上一步的关系
     * @param before 编辑前选区
     * @param after 编辑后选区
     * @returns 提交结果与损伤区间
     */
    CommitOutcome commitSteps(const std::vector<Step>& steps, FlowVisibility vis,
                              doc::MergeRun merge, doc::SelectionSnapshot before,
                              doc::SelectionSnapshot after);

    /**
     * 撤销一步，返回损伤区间与要恢复的选区。
     * @param selection 出参：撤销后的选区
     * @param damage 出参：损伤区间
     * @param vis 可见性口径
     * @returns true = 撤销成功
     */
    bool undo(doc::SelectionSnapshot* selection, doc::Damage* damage, FlowVisibility vis);

    /**
     * 重做一步，返回损伤区间与要恢复的选区。
     * @param selection 出参：重做后的选区
     * @param damage 出参：损伤区间
     * @param vis 可见性口径
     * @returns true = 重做成功
     */
    bool redo(doc::SelectionSnapshot* selection, doc::Damage* damage, FlowVisibility vis);

private:

    /**
     * 会话本体。
     */
    std::optional<doc::Session> impl_;
};

}  // namespace heditor
