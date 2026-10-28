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

#include "core/doc/fragment.h"
#include "core/doc/range.h"
#include "core/doc/subtree.h"
#include "core/rev/char_format.h"
#include "core/rev/length.h"
#include "core/rev/para_format.h"
#include "core/rev/section_spec.h"
#include "core/rev/table_spec.h"

#include "core/rev/step_types.h"
namespace heditor {

/**
 * 构造一条删除步骤：先取区间（顺带校验可编辑集合），再带着片段构造。
 *
 * 它按结构取区间：端点落在不可半分单位（控件、表格、链接）内部时向外扩到那个单位的
 * 边界，于是取出来的片段永远良构、不会把边界切成两半。按结构取区间（选中一段、剪切、
 * 删除一个结构单元）走它；按码点编辑走下面的 makeDeleteTextSteps。
 * @param tree 文档树
 * @param range 区间
 * @return 步骤（失败时带原因）
 */
DeleteBuild makeDeleteRange(const NodeTree& tree, Range range);

/**
 * 构造"删掉这段文本"的一组步骤：把流区间 [from, to) 覆盖到的文本按所在的文本节点切开，
 * 每个文本节点一条 DeleteRange，从后往前排（删右边的不会移动左边的坐标，于是每步的坐标
 * 都是它自己那一刻的坐标，不必再做位置映射）。
 *
 * 为什么不能只用一条 makeDeleteRange：按结构取区间时，落在控件内容里的端点会被向外扩到
 * 控件边界，取出来覆盖的是控件节点本身 —— 集合里只有文本与段落标记，构造因此直接失败。
 * 编辑要删的是码点，所以这条按文本节点切分，每步都严格落在一个文本节点里。
 *
 * 覆盖到的位置若不落在任何文本上（容器边界、段落标记、原子）不产生步骤；跨越它们由切分
 * 自然处理成多条步骤。区间越界或与文档对不上时返回空序列。
 * @param tree 文档树
 * @param from 区间起点
 * @param to 区间终点（不含）
 * @return 一组删除步骤（从后往前排）
 */
std::vector<Step> makeDeleteTextSteps(const NodeTree& tree, FlowPos from, FlowPos to);

/**
 * 用一个步骤删掉整个区间 [from, to)：区间里全是文本与段落标记时可行。
 *
 * 与 makeDeleteTextSteps 的差别只有"分几步"。那边按文本节点切分，每一步严格落在一个
 *   文本节点里；这边把整段并成一步 —— Fragment 本来就支持多 piece（文本与段落标记
 *   交错），applyStep 的 eraseRange 也是按 covers 逐个处理，所以一步是合法的。
 *   整篇删除时步骤数从四千降到一，而每一步在提交时都要重建一次流索引（O(节点数)）。
 *
 * 区间里有别的东西（控件、原子、容器边界）时返回空 —— 调用方退回按节点切分那条路。
 * @param tree 文档树
 * @param idx 流索引
 * @param from 区间起点
 * @param to 区间终点（不含）
 * @return 一条删除步骤；区间里有别的东西时为空
 */
std::vector<Step> makeDeleteRangeOneStep(const NodeTree& tree, const FlowIndex& idx, FlowPos from,
                                         FlowPos to);

/**
 * 构造一个删除段落标记的步骤：标记的格式与样式引用从文档里读出来带上。
 *
 * 逆操作靠这两样还原，所以必须从文档取而不是让调用方自己填 —— 手填一份等于把"这一段长什么
 * 样"记两遍，两处不一致时撤销会还原出另一个格式。
 */
struct ParaMarkBuild {
    /**
     * 构造出来的步骤；ok 为 false 时无效。
     */
    Step step;

    /**
     * 构造成功没有。
     */
    bool ok = false;

    /**
     * 失败原因。
     */
    std::string error;
};

/**
 * 同上，只是不带流索引：这一条自己建一次索引。
 * @param tree 文档树
 * @param at 段落标记的位置
 * @return 步骤（带标记上的格式与样式，逆操作靠它）
 */
ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, FlowPos at);

/**
 * 同上，复用调用方已建好的流索引。
 *
 * 批量删除时每个段落标记都要问一次，而建一次索引是 O(节点数) —— 整篇删除就成了
 *   O(段数 x 节点数)。索引必须与 tree 同一口径，且是 Design（编辑态）。
 * @param tree 文档树
 * @param idx 流索引
 * @param at 段落标记的位置
 * @return 步骤（带标记上的格式与样式，逆操作靠它）
 * @param tree 文档树
 * @param idx 流索引
 * @param at 段落标记的位置
 * @return 步骤（带标记上的格式与样式，逆操作靠它）
 */
ParaMarkBuild makeRemoveParaMark(const NodeTree& tree, const FlowIndex& idx, FlowPos at);

/**
 * 把一条步骤按前面几步的映射推过去，再收进批次。
 *
 * 批次的约定是"一次把所有步骤建出来"，而每步的坐标属于它自己开始的那一刻 —— 前面某一步
 * 改了文档长度，后面几步记的绝对流位置与流位置就过期了（步骤层会判"步骤过期了"）。
 * 方向键走一步不会有这个问题，批量设值与表格行列操作都会。
 * @param out 收步骤的容器
 * @param step 要收的步骤（坐标会被映射到当下）
 */
void pushMappedStep(std::vector<Step>* out, Step step);

/**
 * 节点级步骤的构造结果。
 */
struct NodeBuild {
    /**
     * 构造出来的步骤；ok 为 false 时无效。
     */
    Step step;

    /**
     * 构造成功没有。
     */
    bool ok = false;

    /**
     * 失败原因。
     */
    std::string error;
};

/**
 * 构造"插入一棵子树"的步骤：先把 spec 物化成一棵摘下的子树（身份就此定下），再把
 * "插到哪里"记进去。于是应用与撤销只是把它挂上、拿下，同一个节点的 id 在往返中不变。
 * @param tree 文档树
 * @param at 插入位置
 * @param spec 子树
 * @return 步骤（子树已物化，身份就此定下）
 */
NodeBuild makeInsertNode(NodeTree& tree, const Pos& at, const NodeSpec& spec);

/**
 * 构造"删除一个子节点"的步骤：记下它的内容（captureSubtree）与身份（它自己就是池里的那个节点）。
 * @param tree 文档树
 * @param at 删除位置
 * @return 步骤（带内容与身份）
 */
NodeBuild makeRemoveNode(const NodeTree& tree, const Pos& at);

/**
 * 构造"删掉某一枚原子"的步骤（按流位置寻址）。
 *
 * 与 makeRemoveNode 的分工：那个按孩子槽位寻址、不挑种类，给"删控件"这类结构操作用；这个按
 * 流位置寻址、只认原子 —— 原子在流里整枚占一格，删它只有整枚移除这一种做法，而它的槽位活不过
 * 一次规范化（相邻同标记的文本节点会被合并、槽位整体前移，见 slotForFlow），所以寻址只能落在
 * 流位置上。段落标记另有一种步骤（RemoveParaMark），文本按码点删 —— 三类各占一条路。
 * @param tree 文档树
 * @param idx 流索引
 * @param at 原子的流位置
 * @return 步骤（整枚移除）
 */
NodeBuild makeRemoveAtom(const NodeTree& tree, const FlowIndex& idx, FlowPos at);

/**
 * 这个步骤现在能应用吗。applyStep 先跑它，所以失败时文档一定没被动过。
 * @param tree 文档树
 * @param step 步骤
 * @return 能不能应用；不能时带原因
 */
StepResult canApply(const NodeTree& tree, const Step& step);

/**
 * 同上，复用调用方已建好的流索引。applyStep 每一步都要跑一次判据，两边各建一次索引
 *   等于把每步的成本翻倍 —— 一批四千步的删除里那就是几千次全树遍历。
 * @param tree 文档树
 * @param step 步骤
 * @return 能不能应用；不能时带原因
 * @param idx 调用方建好的流索引
 */
StepResult canApply(const NodeTree& tree, const FlowIndex& idx, const Step& step);

/**
 * 应用步骤。
 * @param tree 文档树
 * @param step 步骤
 * @return 结果；失败时文档没被动过
 */
StepResult applyStep(NodeTree& tree, const Step& step);

/**
 * 逆步骤。由步骤本身算出来，不必再读文档。
 * @param step 步骤
 * @return 逆步骤
 */
Step invertStep(const Step& step);

/**
 * 把编辑前的位置映射到编辑后。
 *
 * 位于被删区间内部的位置没有原像（那段内容已经不在），一律塌到区间起点；这一条是单向的。
 * 其余位置都能与逆步骤的映射精确往返，前提是两个方向用同一个 assoc。
 * @param step 那一步
 * @param pos 编辑前的位置
 * @param assoc 落在边界上时贴哪一边
 * @return 编辑后的位置
 */
FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc);

/**
 * 把 target 里记的全部流位置按 mapped_by 这一步的映射搬一次。
 *
 * 用在"一条步骤的坐标是在含某次编辑的文档里记下的，而那次编辑后来不生效了"这种场合：
 * 把坐标穿过那次编辑的逆步骤，就回到了不含它的坐标。只覆盖内容级的五个步骤 —— 结构步骤
 * （节点级）的位置含 Pos，单靠流位置映射搬不动，返回 false 让调用方显式处理，不静默照搬。
 * @param target 要搬坐标的步骤（就地改写）
 * @param mapped_by 坐标要穿过的那一步
 * @param assoc 贴哪一边
 * @return true = 搬好了；false = 结构步骤，搬不动
 */
bool mapStepPositions(Step* target, const Step& mapped_by, Assoc assoc);

}  // namespace heditor
