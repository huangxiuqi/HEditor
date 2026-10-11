/**
 * 编辑步骤：可应用、可逆、可映射位置（第五节）。
 *
 * 步骤只做最小改动：插入时若紧邻的文本节点标记相同就并入它，删除时把两端所在的文本节点截断
 * 而不是切成两段。这样应用与逆操作能精确相消 —— 这是往返判据成立的前提。规范化不参与步骤：
 * 它在事务提交前统一跑一次（见 1.1），所以"提交后的形状"才是合法形状，中间态可以暂时不合法。
 *
 * 逆操作由步骤本身算出来：每个步骤都带着还原所需的全部信息（删除步骤带着取出来的片段）。
 * 位置映射与逆映射是一对 —— 映射把编辑前的位置搬到编辑后，逆步骤的映射把它搬回来。
 *
 * 可编辑集合是文本与段落标记（1.2 的范围）：区间里出现其它元素时，构造步骤就失败并给原因，
 * 不会应用到一个半截的区间上。
 */
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/fragment.h"
#include "core/doc/para_format.h"
#include "core/doc/range.h"

namespace heditor {

/**
 * 位置映射"贴哪一边"：位置正好落在插入或删除的边界上时归左边还是右边。
 *
 * 同一位置有两个合法归宿时，这是唯一能表态的地方。参照项目用一个 FixIndex() 做同样的修正。
 */
enum class Assoc : uint8_t { Before, After };

/**
 * 在文本位置插入码点序列（普通输入）。
 */
struct InsertText {
    FlowPos at = 0;
    std::vector<uint32_t> cps;
    CharFormat marks;
};

/**
 * 把一段片段插回。它既是删除的逆操作，也是粘贴。
 */
struct InsertFragment {
    FlowPos at = 0;
    Fragment content;
};

/**
 * 删除一个区间。构造时就带着取出来的片段，逆操作靠它还原。
 */
struct DeleteRange {
    FlowPos begin = 0;
    FlowPos end = 0;
    Fragment removed;
};

/**
 * 在位置处插入段落标记（回车分段）。
 */
struct InsertParaMark {
    FlowPos at = 0;
    ParaFormat para;
};

/**
 * 删除位置处的段落标记（段首退格与段末删除都是它，合并是它的结果）。
 */
struct RemoveParaMark {
    FlowPos at = 0;
    ParaFormat para;
};

using Step = std::variant<InsertText, InsertFragment, DeleteRange, InsertParaMark, RemoveParaMark>;

/**
 * 步骤的执行结果。失败时给出原因 —— 位置越界、类型不符都要显式说出来，不静默不做。
 */
struct StepResult {
    bool ok = false;
    std::string error;
};

/**
 * 删除步骤的构造结果。
 */
struct DeleteBuild {
    Step step;
    bool ok = false;
    std::string error;
};

/**
 * 构造一个删除步骤：先取区间（顺带校验可编辑集合），再带着片段构造。
 */
DeleteBuild makeDeleteRange(const NodeTree& tree, Range range);

/**
 * 这个步骤现在能应用吗。applyStep 先跑它，所以失败时文档一定没被动过。
 */
StepResult canApply(const NodeTree& tree, const Step& step);

/**
 * 应用步骤。
 */
StepResult applyStep(NodeTree& tree, const Step& step);

/**
 * 逆步骤。由步骤本身算出来，不必再读文档。
 */
Step invertStep(const Step& step);

/**
 * 把编辑前的位置映射到编辑后。
 *
 * 位于被删区间内部的位置没有原像（那段内容已经不在），一律塌到区间起点；这一条是单向的。
 * 其余位置都能与逆步骤的映射精确往返，前提是两个方向用同一个 assoc。
 */
FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc);

}  // namespace heditor
