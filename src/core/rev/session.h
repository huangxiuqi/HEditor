/**
 * 会话层：一份当前修订，加一条撤销栈。
 *
 * 这里只有两件事，边界按这条划：
 *   进 —— 把一步编辑变成新修订、历史（撤销/重做）与"现在用的是哪一版"；
 *   不进 —— 落点策略（选区怎么删、控件边界怎么落、退格往前还是往后）、排版、绘制。
 *
 * 为什么历史是"一串旧修订"而不是"一串逆操作"：修订不可变，旧值一直在，撤销就是把它拿
 * 回来。逆操作要写一份"删的逆是插、插的逆是删、合并的逆是分段"的对照表，而每一对都得
 * 与正向实现严格对称 —— 那是这个项目过去反复出错的那类地方（两份实现慢慢分叉）。
 * 代价是内存，而结构共享让一份旧修订只多出被改的那条路径。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/flow.h"
#include "core/doc/steps.h"
#include "core/rev/rev.h"

namespace heditor::rev {
/**
 * 一步已经算好的编辑：与 rev 的原语一一对应，不带任何落点判断。
 *
 * 它刻意不是"命令"：命令层的输入是光标与选区，要决定删哪几个字、插在哪个 run；
 * 这里收到的是决定之后的结果（第几段第几个码点）。两者的分界就是"要不要看策略"——
 * 策略还在模型层（引擎的命令层），这一层只负责把结果变成新修订并记住旧值。
 *
 * 一个结构体带 kind，而不是 std::variant：它会成为宿主那边一条 JSON 的形状
 * （步 6 的 IDL），扁平结构映射起来只有一份字段表。
 */
struct Edit {
    enum class Kind : uint8_t {
        InsertText,
        EraseText,
        SplitPara,
        MergePara,
        InsertControl,
        SetHidden,
    };

    /**
     * 这是哪一种编辑。
     */
    Kind kind = Kind::InsertText;

    /**
     * 段号（编辑态）。
     */
    int para = 0;

    /**
     * 段内码点位置。
     */
    int cp = 0;

    /**
     * 删掉几个码点。
     */
    int count = 0;

    /**
     * 插入的文本，或控件的内容。
     */
    std::u32string text;

    /**
     * 控件记录（InsertControl 用）。
     */
    ControlSpec control;

    /**
     * 被隐藏的节点（SetHidden 用）。
     */
    NodeId node = kInvalidNodeId;

    /**
     * 隐藏还是取消隐藏。
     */
    bool hidden = false;
};

/**
 * 一次提交的结果。失败时带原因，且会话状态一个字节都不动 —— "编辑被拒绝"与"编辑没产生
 * 变化"必须分得开，否则调用方会把失败当成成功继续往下走。
 */
struct Applied {
    /**
     * 成了没有。
     */
    bool ok = false;

    /**
     * 失败原因（会话状态没动）。
     */
    std::string error;

    /**
     * InsertControl 造出来的控件节点号。
     */
    NodeId created = kInvalidNodeId;

    /**
     * 修订换了没有（哈希不同）；失败时为 false。
     */
    bool changed = false;
};

/**
 * 会话：当前修订 + 撤销栈 + 重做栈。
 *
 * limit 是撤销栈最多保留几份旧修订（0 表示不留历史，只能前进）。超出时丢最旧的一份 ——
 * 丢的是"更早的状态"，不是"某一步的逆操作"，所以丢完之后撤销仍然一步步往回走，只是走不到
 * 最开头了。这与"逆操作栈丢弃最旧条目"不同，后者会留下一个无法回到的起点。
 */
class Session {
public:

    explicit Session(Doc doc, int limit = 100);

    /**
     * 当前修订（不可变）。
     * @return 当前修订
     */
    const Doc& doc() const { return doc_; }

    /**
     * 提交一步编辑。成功时当前修订换成新的、旧值进撤销栈、重做栈清空。
     *
     * 重做栈清空是必须的：分支之后原先那条"未来"已经不成立了，留着它意味着一次重做会跳回
     * 一条与当前状态无关的修订上 —— 画面看着正常，内容却是别的分支。
     * @param edit 已经算好的那一步
     * @return 提交结果（失败时会话状态没动）
     */
    Applied apply(const Edit& edit);

    /**
     * 提交一批步骤。一次编辑可以有多条步骤（分段后再改段落格式、删选区与插入合并），
     * 用户按下的是一下，撤销也该是一下 —— 所以整批算一步，旧修订只压一份。
     *
     * vis 是步骤坐标的可见性口径（见 rev::applySteps）：会话不替调用方猜模式。
     * @param steps 一批步骤（整批算一步撤销）
     * @param vis 步骤坐标的可见性口径
     * @return 提交结果
     */
    Applied applySteps(const std::vector<Step>& steps, FlowVisibility vis);

    /**
     * 回到上一份修订。没有历史时返回 false（不动状态）。
     * @return true = 退回到上一份修订；没有历史时为 false
     */
    bool undo();

    /**
     * 回到撤销掉的那一份。与 undo 对称。
     * @return true = 回到撤销掉的那一份
     */
    bool redo();

    /**
     * 调小上限时当场裁掉多余的（丢最旧的），不必等下一次提交。
     *
     * 0 表示不留历史；这与"不限制"不是一回事，调用方若把 0 当"不限制"用，
     * 要在它自己那一层换成一个足够大的数。
     * @param limit 最多保留几份旧修订（0 = 不留历史）
     */
    void setLimit(int limit);

    /**
     * 有没有可撤销的历史。
     * @return true = 撤销栈非空
     */
    bool canUndo() const { return !undo_.empty(); }

    /**
     * 有没有可重做的。
     * @return true = 重做栈非空
     */
    bool canRedo() const { return !redo_.empty(); }

    /**
     * 撤销栈里有几份旧修订。
     * @return 份数
     */
    int undoDepth() const { return static_cast<int>(undo_.size()); }

    /**
     * 重做栈里有几份。
     * @return 份数
     */
    int redoDepth() const { return static_cast<int>(redo_.size()); }

    /**
     * 撤销栈的上限。
     * @return 最多保留几份
     */
    int limit() const { return limit_; }

    /**
     * 状态代号：每次修订真的换了就 +1（提交、撤销、重做都算）。
     *
     * 宿主靠它判"要不要重画"，而不是靠哈希 —— 一次撤销回到旧修订时哈希本来就该回到旧值，
     * 拿哈希当代号会让"撤销之后画面没更新"。它只保证单调，不保证连续到某一步。
     * @return 状态代号（只保证单调）
     */
    uint64_t generation() const { return generation_; }

private:

    void pushUndo(Doc doc);

    /**
     * 当前修订。
     */
    Doc doc_;

    /**
     * 旧修订（越靠后越新），撤销就是把它取回来。
     */
    std::vector<Doc> undo_;

    /**
     * 撤销掉的修订，重做按它回去。
     */
    std::vector<Doc> redo_;

    /**
     * 撤销栈的上限（0 = 不留历史）。
     */
    int limit_ = 100;

    /**
     * 状态代号，每次修订真的换了就 +1。
     */
    uint64_t generation_ = 0;
};

}  // namespace heditor::rev
