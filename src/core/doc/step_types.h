/**
 * 编辑步骤的值类型：只描述"改什么"，不依赖文档树。
 */
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/field_spec.h"
#include "core/doc/flow_types.h"
#include "core/doc/formula_spec.h"
#include "core/doc/fragment_value.h"
#include "core/doc/image_spec.h"
#include "core/doc/node_kind.h"
#include "core/doc/para_format.h"
#include "core/doc/range.h"
#include "core/doc/section_spec.h"
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
 * 改控件的定义（整份记录）。
 *
 * 与 SetControlProp 的分工：那个改四个运行态开关，这个改记录本身（业务编码、选项表、
 * 标签、互斥组、自由输入、日期格式与单位）。带 before 整份，逆操作自足；canApply 比
 * before 与当前记录，对不上就是这一步过期了。
 *
 * 位置映射是恒等：定义不改流位置。选项表变短只让内容变短，那是内容步骤自己带坐标的事。
 */
struct SetControlSpec {
    /**
     * 改哪个控件。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的定义。
     */
    ControlSpec spec;

    /**
     * 原定义（逆操作自足）。
     */
    ControlSpec before;
};

/**
 * 改一个单元格的记录（第九节）：跨度与格内垂直对齐。
 *
 * 表格的行列操作落到步骤上就是"几个格子的记录变了 + 插了几个新格子"—— 跨度是唯一真相，
 * 网格是派生的，所以没有"插一列"这种步骤，只有记录的改动。整份 CellSpec 一起带，是因为
 * 只写跨度会把垂直对齐静默清成顶端。
 *
 * 位置映射是恒等：这份记录不改流位置，也不改流结构。
 */
struct SetCellSpec {
    /**
     * 改哪一格。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的记录。
     */
    CellSpec spec;

    /**
     * 改之前的记录（逆操作自足）。
     */
    CellSpec before;
};

/**
 * 改一行表格的记录：标题行、允许跨页断行、行前分页、行高与行高规则。
 *
 * 与 SetCellSpec 同形：整份记录一起带、带原值。整份而非逐项，是因为调用方拿到的是
 * "改完之后的这一行"，逐项拼装必然要在命令层再实现一遍"哪些项没给"的语义。
 *
 * 位置映射是恒等：行属性只改几何与分页，不改流位置。
 */
struct SetRowSpec {
    /**
     * 改哪一行。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的记录。
     */
    RowSpec spec;

    /**
     * 改之前的记录（逆操作自足）。
     */
    RowSpec before;
};

/**
 * 改一张表格的记录：表格宽度与水平位置。
 *
 * 列定义与列宽不在这里改：列定义是"按数据落值"的形状（改它等于换一张表的用法），
 * 现在只由新建时给；列宽要等排版层的按列分配落地之后才有可写的入口。
 *
 * 位置映射是恒等：表属性只改几何。
 */
struct SetTableSpec {
    /**
     * 改哪张表。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的记录。
     */
    TableSpec spec;

    /**
     * 改之前的记录（逆操作自足）。
     */
    TableSpec before;
};

/**
 * 改一张图片的显示尺寸。
 *
 * 尺寸是作者意图（ImageSpec 的 width / height），改它只动行盒几何、不动流位置，所以位置映射
 * 是恒等。与 SetControlProp 同形：记下原值，逆操作因此自足；一次只改一张图，批量的形状由
 * 事务表达。
 */
struct SetImageSize {
    /**
     * 改哪一张图。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的显示宽度（Auto 表示按原始像素与 DPI 折算）。
     */
    Length width;

    /**
     * 新的显示高度。
     */
    Length height;

    /**
     * 改之前的宽度（逆操作自足）。
     */
    Length beforeWidth;

    /**
     * 改之前的高度。
     */
    Length beforeHeight;
};

/**
 * 改一节页面设置里引擎能理解的那部分：整份 SectionSpec 一起换。
 *
 * 与表格那三类记录同形，整份而不是逐项：节的字段多且互相牵扯（纸张名与自定义宽高、
 * 页码格式与重启标志），逐项拼装要在命令层再实现一遍"哪些项没给"的语义。
 *
 * 位置映射是恒等：节属性只改页面几何，不改流位置。
 */
struct SetSectionSpec {
    /**
     * 改哪一节。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的记录。
     */
    SectionSpec spec;

    /**
     * 改之前的记录（逆操作自足）。
     */
    SectionSpec before;
};

/**
 * 改一张图的整份记录：尺寸、定位方式与环绕方式一起换。
 *
 * 与 SetImageSize 分开：那个只改尺寸（高频、拖动时每帧一次），这个改的是"图怎么待在版面上"
 * （低频、一次成形）。两类改动的影响面不同 —— 环绕方式改的是断行，尺寸改的是行盒。
 *
 * 位置映射是恒等：图片是原子，换它的记录不改任何流位置。
 */
struct SetImageSpec {
    /**
     * 改哪一张图。
     */
    NodeId id = kInvalidNodeId;

    /**
     * 新的记录。
     */
    ImageSpec spec;

    /**
     * 改之前的记录（逆操作自足）。
     */
    ImageSpec before;
};

using Step = std::variant<InsertText, InsertFragment, DeleteRange, InsertParaMark, RemoveParaMark,
                          SetParaMarkFormat, SetRun, InsertNode, RemoveNode, UnwrapNode, WrapNode,
                          SetControlProp, SetControlValue, SetControlSpec, SetCellSpec, SetRowSpec,
                          SetTableSpec, SetImageSize, SetSectionSpec, SetImageSpec>;

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
 * 段落标记删除步骤的构造结果。
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
}  // namespace heditor