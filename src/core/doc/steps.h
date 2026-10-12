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
 *
 * 控件、图片、表格这类整块元素走另一组步骤（InsertNode / RemoveNode）：它们不按区间增删，
 * 而是整棵子树挂上、拿下，所以不会出现"半个控件"这种中间态。
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
#include "core/doc/subtree.h"
#include "core/doc/table_spec.h"

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

    /**
     * 插入的文本用哪个字符样式（第六节）。输入时一般是空的（沿用落点处已有的样式由命令层决定），
     * 粘贴带样式的文本时由片段带过来。逆操作要把它记回片段里，否则撤销再重做会丢样式。
     */
    std::string styleRef;
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
    ParaFormatPatch para;

    /**
     * 新段的段落样式。分段时由命令层给出（通常是沿用被切那一段的样式），
     * 从片段插回时由片段带过来。
     */
    std::string styleRef;
};

/**
 * 删除位置处的段落标记（段首退格与段末删除都是它，合并是它的结果）。
 */
struct RemoveParaMark {
    FlowPos at = 0;
    ParaFormatPatch para;
    std::string styleRef;
};

/**
 * 插入一棵子树（控件、图片、表格这类整块内容）。节点级坐标用 Pos（第四节：节点级 API 用 Pos），
 * 同时记下它对应的流位置供位置映射使用；两者对不上时 canApply 会拒掉，所以这份冗余不会被误信。
 *
 * detached 是已经摘下的那棵子树在池里的根 id。构造时（makeInsertNode）就把子树物化好，于是
 * 应用、撤销与重做都是同一棵树在挂上、拿下之间来回 —— 节点的身份不变。kInvalid 表示没有可挂的
 * 现成子树，apply 会按 subtree 现建一棵（身份不保持，序列化还原时走这条路）。
 */
struct InsertNode {
    Pos at;
    FlowPos at_flow = 0;
    NodeSpec subtree;
    NodeId detached = kInvalidNodeId;
};

/**
 * 删除一个子节点（连同它的整棵子树）。用它表达"删掉一个控件"这类结构操作。
 *
 * removed 是内容与结构的拷贝（供序列化与校验），detached 是被摘下后留在池里的根 id（供撤销
 * 原样挂回）。两者记的是同一棵子树：前者说明"删了什么"，后者说明"它现在在哪"。
 */
struct RemoveNode {
    Pos at;
    FlowPos at_flow = 0;
    NodeSpec removed;
    NodeId detached = kInvalidNodeId;
};

/**
 * 改控件的一个属性（第七节的四个：hidden / readonly / required / structureLock）。
 *
 * 一次只改一项：批量的形状由事务表达（一个事务里多条步骤），步骤本身保持最小 —— 这样逆操作
 * 与"只改 readonly 不重排"这类判断都只有一个分支要看。before 记下原值，逆操作因此自足。
 *
 * 位置映射是恒等：这一步不改编辑态的位置。hidden 会改呈现态的流结构，但那是"要重排"
 * （重建派生索引），不是"把某个位置搬到别处"，所以不在这层表达。
 */
struct SetControlProp {
    NodeId id = kInvalidNodeId;
    ControlProp prop = ControlProp::Readonly;
    bool value = false;
    bool before = false;
};

/**
 * 改控件属性侧的值（Bool / Enum / List / Resource）。
 *
 * 值分两半：文本类的值在内容里，改它走内容级的节点步骤（摘掉旧文本节点、插一个新文本节点，
 * 见 ControlApi）；这里只覆盖"值放在记录上"的那一半。与 SetControlProp 一样带原值，
 * 逆操作因此自足；同样一次只改一个控件，批量的形状由事务表达。
 *
 * 位置映射是恒等：这一步不改流位置，也不改流结构（值与属性都不占位置）。
 */
struct SetControlValue {
    NodeId id = kInvalidNodeId;
    ControlValue value;
    ControlValue before;
};

/**
 * 改一个单元格的跨度（第九节）。表格的行列操作落到步骤上就是"几个格子的跨度变了 + 插了几个
 * 新格子"—— 跨度是唯一真相，网格是派生的，所以没有"插一列"这种步骤，只有跨度的改动。
 *
 * 位置映射是恒等：跨度不改流位置，也不改流结构。
 */
struct SetTableCellSpan {
    NodeId id = kInvalidNodeId;
    CellSpan span;
    CellSpan before;
};

using Step =
    std::variant<InsertText, InsertFragment, DeleteRange, InsertParaMark, RemoveParaMark,
                 InsertNode, RemoveNode, SetControlProp, SetControlValue, SetTableCellSpan>;

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
 * 构造一个删除段落标记的步骤：标记的格式与样式引用从文档里读出来带上。
 *
 * 逆操作靠这两样还原，所以必须从文档取而不是让调用方自己填 —— 手填一份等于把"这一段长什么
 * 样"记两遍，两处不一致时撤销会还原出另一个格式。
 */
struct ParaMarkBuild {
    Step step;
    bool ok = false;
    std::string error;
};

ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, FlowPos at);

/**
 * 把一条步骤按前面几步的映射推过去，再收进批次。
 *
 * 批次的约定是"一次把所有步骤建出来"，而每步的坐标属于它自己开始的那一刻 —— 前面某一步
 * 改了文档长度，后面几步记的绝对流位置与流位置就过期了（步骤层会判"步骤过期了"）。
 * 方向键走一步不会有这个问题，批量设值与表格行列操作都会。
 */
void pushMappedStep(std::vector<Step>* out, Step step);

/**
 * 节点级步骤的构造结果。
 */
struct NodeBuild {
    Step step;
    bool ok = false;
    std::string error;
};

/**
 * 构造"插入一棵子树"的步骤：先把 spec 物化成一棵摘下的子树（身份就此定下），再把
 * "插到哪里"记进去。于是应用与撤销只是把它挂上、拿下，同一个节点的 id 在往返中不变。
 */
NodeBuild makeInsertNode(NodeTree& tree, const Pos& at, const NodeSpec& spec);

/**
 * 构造"删除一个子节点"的步骤：记下它的内容（captureSubtree）与身份（它自己就是池里的那个节点）。
 */
NodeBuild makeRemoveNode(const NodeTree& tree, const Pos& at);

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

/**
 * 把 target 里记的全部流位置按 mapped_by 这一步的映射搬一次。
 *
 * 用在"一条步骤的坐标是在含某次编辑的文档里记下的，而那次编辑后来不生效了"这种场合：
 * 把坐标穿过那次编辑的逆步骤，就回到了不含它的坐标。只覆盖内容级的五个步骤 —— 结构步骤
 * （节点级）的位置含 Pos，单靠流位置映射搬不动，返回 false 让调用方显式处理，不静默照搬。
 */
bool mapStepPositions(Step* target, const Step& mapped_by, Assoc assoc);

}  // namespace heditor
