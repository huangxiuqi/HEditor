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
    /**
     * 插入位置（流位置）。
     */
    FlowPos at = 0;

    /**
     * 要插入的码点。
     */
    std::vector<uint32_t> cps;

    /**
     * 这些码点的字符格式（标记）。
     */
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
    /**
     * 插回的位置（流位置）。
     */
    FlowPos at = 0;

    /**
     * 片段内容（文本与段落标记交错）。
     */
    Fragment content;
};

/**
 * 删除一个区间。构造时就带着取出来的片段，逆操作靠它还原。
 */
struct DeleteRange {
    /**
     * 区间起点（含）。
     */
    FlowPos begin = 0;

    /**
     * 区间终点（不含）。
     */
    FlowPos end = 0;

    /**
     * 取出来的片段，逆操作靠它还原。
     */
    Fragment removed;
};

/**
 * 在位置处插入段落标记（回车分段）。
 */
struct InsertParaMark {
    /**
     * 在哪插入段落标记（流位置）。
     */
    FlowPos at = 0;

    /**
     * 新段的段落格式（补丁）。
     */
    ParaFormatPatch para;

    /**
     * 新段的段落样式。分段时由命令层给出（通常是沿用被切那一段的样式），
     * 从片段插回时由片段带过来。
     */
    std::string styleRef;

    /**
     * 新标记自己的字符格式。分段时沿用原标记那一份：空段里没有文本节点，
     * 字符格式只能挂在标记上，丢掉它等于"分段之后格式变了"。
     */
    CharFormat marks;
};

/**
 * 删除位置处的段落标记（段首退格与段末删除都是它，合并是它的结果）。
 */
struct RemoveParaMark {
    /**
     * 删哪一枚段落标记（流位置）。
     */
    FlowPos at = 0;

    /**
     * 标记上的段落格式，逆操作要还原它。
     */
    ParaFormatPatch para;

    /**
     * 标记上的字符格式，逆操作要还原它。
     */
    CharFormat marks;

    /**
     * 标记上的段落样式引用。
     */
    std::string styleRef;
};

/**
 * 改一个段落标记的格式与样式引用（段落级属性的内联覆盖，第六节）。
 *
 * 分段与合并需要它，理由不是"也想顺便改格式"：段落属性挂在结束标记上，而标记的归属与
 * "用户心里那一段"在两种情况下正好错位。回车是在光标处插一枚新标记，于是新标记结束的是
 * 前半段，"拆出来的后半段不该跟着强制分页"这条约定落不到它身上；合并是删掉前一段的
 * 结束标记，于是留下来的标记是后一段的，合并之后那一段会顶着后一段的格式。两个偏差都
 * 只改标记的数据、不改流结构，位置映射是恒等，所以它们该是独立的一步而不是挤进那两步里。
 *
 * before 与 beforeStyleRef 记下原值，逆操作因此自足；一次只改一枚标记，批量的形状由事务表达。
 */
struct SetParaMarkFormat {
    /**
     * 改哪一枚段落标记。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的段落格式（补丁）。
     */
    ParaFormatPatch para;

    /**
     * 新的段落样式引用。
     */
    std::string styleRef;

    /**
     * 改之前的段落格式（逆操作自足）。
     */
    ParaFormatPatch before;

    /**
     * 改之前的样式引用。
     */
    std::string beforeStyleRef;

    /**
     * 段落标记自己的字符格式。
     *
     * 空段落没有文本节点，字符格式无处可挂 —— 它与段落格式同挂在结束这一段的标记上。
     * 有文字的段落也带着它，"接着在段末敲字"读的是左边文本节点的标记，不看它。
     */
    CharFormat marks;

    /**
     * 改之前的字符格式（逆操作自足）。
     */
    CharFormat beforeMarks;
};

/**
 * 改一个文本节点的正文与字符标记（粗体、斜体、下划线、字体、字号、字色）。
 *
 * 与 SetParaMarkFormat 同形，记下改之前的值让逆操作自足。
 *
 * 为什么要带正文：字符标记是挂在文本节点上的，一个节点只有一套标记。选区落在节点中间时
 *   "只把这两个字加粗"没法在同一个节点上表达，只能把它切成一串节点 —— 切出来的那一段
 *   正文要写进新节点，留下的那一段则是原正文的一个前缀。切分不改总长度，所以它不影响
 *   同一批里其它步骤的流位置。
 */
struct SetRun {
    /**
     * 改哪个文本节点。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的正文（码点）。切分时这里是原正文的一个前缀。
     */
    std::vector<uint32_t> text;

    /**
     * 新的字符标记。
     */
    CharFormat marks;

    /**
     * 新的字符样式引用。
     */
    std::string styleRef;

    /**
     * 改之前的正文（逆操作自足）。
     */
    std::vector<uint32_t> beforeText;

    /**
     * 改之前的字符标记（逆操作自足）。
     */
    CharFormat before;

    /**
     * 改之前的样式引用。
     */
    std::string beforeStyleRef;
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
    /**
     * 插入位置（节点级坐标）。
     */
    Pos at;

    /**
     * 同一个位置的流坐标，位置映射用它。
     */
    FlowPos at_flow = 0;

    /**
     * 子树的内容与结构（序列化与校验用）。
     */
    NodeSpec subtree;

    /**
     * 物化好的子树在池里的根 id；kInvalid = apply 时现建一棵。
     */
    NodeId detached = kInvalidNodeId;
};

/**
 * 删除一个子节点（连同它的整棵子树）。用它表达"删掉一个控件"这类结构操作。
 *
 * removed 是内容与结构的拷贝（供序列化与校验），detached 是被摘下后留在池里的根 id（供撤销
 * 原样挂回）。两者记的是同一棵子树：前者说明"删了什么"，后者说明"它现在在哪"。
 */
struct RemoveNode {
    /**
     * 删除位置（节点级坐标）。
     */
    Pos at;

    /**
     * 同一个位置的流坐标。
     */
    FlowPos at_flow = 0;

    /**
     * 删掉的内容与结构（序列化与校验用）。
     */
    NodeSpec removed;

    /**
     * 摘下后留在池里的根 id，撤销靠它原样挂回。
     */
    NodeId detached = kInvalidNodeId;
};

/**
 * 拆开一个容器节点：子节点原样上提到它自己的位置，节点本身移除。
 *
 * "删除控件但保留内容"就是它（与 RemoveNode 的差别只有这一处，而那一处就是语义本身：那个
 * 连内容一起删）。子节点是同一批节点被搬了位置，身份不变 —— 撤销重做之后句柄仍然有效。
 * 逆操作是 WrapNode，两者共用一份记录、字段一一对应；span_before / span_after 是节点自身
 * 与内容各占的流长度（差就是它自己那两枚边界），位置映射要用它们。
 *
 * at 只是记录，at_flow 才是权威：规范化会在提交之后合并相邻同标记的文本节点，孩子槽位整体
 * 前移而流位置不动，所以应用与校验都按 at_flow 当场重算槽位（steps.cpp 的 slotAtFlow），
 * 必要时把被合并的文本节点切回来。
 */
struct UnwrapNode {
    /**
     * 被拆开节点的位置（节点级坐标）。
     */
    Pos at;

    /**
     * 同一个位置的流坐标（权威：槽位按它当场重算）。
     */
    FlowPos at_flow = 0;

    /**
     * 容器自身占的流长度（两枚边界）。
     */
    int span_before = 0;

    /**
     * 内容占的流长度。
     */
    int span_after = 0;

    /**
     * 被拆开那个节点离开时的快照（校验与定位用）。
     */
    NodeSpec node;

    /**
     * 节点在池里的根 id。
     */
    NodeId detached = kInvalidNodeId;
};

/**
 * 包装：把流区间 [at_flow, at_flow + span_after) 里的孩子收进一个容器节点。
 * UnwrapNode 的逆操作；记录字段与它一一对应，所以两个方向的往返是严格的。
 *
 * 注意两个方向的 at_flow 指的不是同一格：步骤的坐标一律是"这一步将要应用的那个状态"里的
 * 地址。UnwrapNode 应用在拆之前（at_flow = 容器起点），WrapNode 应用在拆之后 —— 那两枚
 * 边界已经不在，内容整体前移一格，所以它的 at_flow 是内容起点。span_before 与 span_after
 * 把这层差别带过去（mapPos 也按这个口径写）。
 *
 * 同样以流位置为准，不用槽位下标：被合并的文本节点要在这一刻切回来，区间才重新有边界。
 */
struct WrapNode {
    /**
     * 包装位置（节点级坐标）。
     */
    Pos at;

    /**
     * 内容起点的流坐标（两枚边界已不在，比 UnwrapNode 前移一格）。
     */
    FlowPos at_flow = 0;

    /**
     * 容器自身占的流长度。
     */
    int span_before = 0;

    /**
     * 内容占的流长度（被收进容器的那个区间）。
     */
    int span_after = 0;

    /**
     * 被收进容器的那些孩子与容器的形状。
     */
    NodeSpec node;

    /**
     * 逆操作要挂回的那个容器 id。
     */
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
    /**
     * 改哪个控件。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 改哪一项属性。
     */
    ControlProp prop = ControlProp::Readonly;

    /**
     * 新值。
     */
    bool value = false;

    /**
     * 原值（逆操作自足）。
     */
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
    /**
     * 改哪个控件。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的值。
     */
    ControlValue value;

    /**
     * 原值（逆操作自足）。
     */
    ControlValue before;
};

/**
 * 改一个单元格的跨度（第九节）。表格的行列操作落到步骤上就是"几个格子的跨度变了 + 插了几个
 * 新格子"—— 跨度是唯一真相，网格是派生的，所以没有"插一列"这种步骤，只有跨度的改动。
 *
 * 位置映射是恒等：跨度不改流位置，也不改流结构。
 */
struct SetTableCellSpan {
    /**
     * 改哪一格。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的跨度。
     */
    CellSpec span;

    /**
     * 改之前的跨度（逆操作自足）。
     */
    CellSpec before;
};

using Step = std::variant<InsertText, InsertFragment, DeleteRange, InsertParaMark, RemoveParaMark,
                          SetParaMarkFormat, SetRun, InsertNode, RemoveNode, UnwrapNode, WrapNode,
                          SetControlProp, SetControlValue, SetTableCellSpan>;

/**
 * 步骤的执行结果。失败时给出原因 —— 位置越界、类型不符都要显式说出来，不静默不做。
 */
struct StepResult {
    /**
     * 成了没有。
     */
    bool ok = false;

    /**
     * 失败原因（位置越界、类型不符）。
     */
    std::string error;
};

/**
 * 删除步骤的构造结果。
 */
struct DeleteBuild {
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
