/**
 * 行 / 页布局。
 * 这一层的核心设计：书写轴是一等公民，不是布尔开关。
 * 横排：行内沿 +x 推进，块沿 +y 推进，可用范围 = 页宽。
 * 竖排：行内沿 +y 推进，块沿 ±x 推进，可用范围 = 页高。
 * —— 列序方向由 WritingMode::block_flow 决定：
 * vertical-rl（中日）：第 0 列在最右
 * vertical-lr（蒙文）：第 0 列在最左
 * 两种模式共用同一套贪心断行算法（先按可用范围累积推进量，记录最后一个
 * "可断点"，装不下时回退到该处），差别只在"哪个轴"和"可用范围取哪一边"。
 * 如果按横排硬编码布局核心，后面加蒙文就是重写这一层。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/layout/float_band.h"

#include "core/doc/document.h"
#include "core/doc/image_spec.h"
#include "core/doc/para_format.h"
#include "core/doc/utf8.h"
#include "core/text/font.h"
#include "core/text/writing_mode.h"
#include "core/utils/types.h"

namespace heditor {

/**
 * 四边页边距（磅）。四个方向分开给：文字处理软件的页面设置里它们本来就是四个独立的值，
 * "四边相等"只是其中最省事的一档。
 */
struct PageMargins {
    /**
     * 上边距。
     */
    float top = 56.7f;

    /**
     * 右边距。
     */
    float right = 56.7f;

    /**
     * 下边距。
     */
    float bottom = 56.7f;

    /**
     * 左边距。
     */
    float left = 56.7f;

    /**
     * 四边取同一个值。
     * @param pt 边距（磅）
     * @return 四边相等的边距
     */
    static PageMargins all(float pt) {
        PageMargins m;
        m.top = pt;
        m.right = pt;
        m.bottom = pt;
        m.left = pt;
        return m;
    }

    /**
     * 精确相等。
     * @param o 另一份
     * @return true = 四边都相同
     */
    bool operator==(const PageMargins& o) const {
        return top == o.top && right == o.right && bottom == o.bottom && left == o.left;
    }

    /**
     * 不等 = 精确相等取反。
     * @param o 另一份
     * @return true = 至少一边不同
     */
    bool operator!=(const PageMargins& o) const { return !(*this == o); }
};

struct PageMetrics {
    /**
     * 页宽（磅）。含左右页边距，正文可用宽度见 contentWidthPt()。
     */
    float width_pt = 595.276f;

    /**
     * 页高（磅）。含上下页边距。
     */
    float height_pt = 841.89f;

    /**
     * 四边页边距（磅）。默认 2cm。
     */
    PageMargins margins{};

    /**
     * 连页（不分页）：排版不在页底断开，内容一列连到底。
     *
     * 页号在连页下退化成"渲染条带号" = floor(块轴位置 / slab_pt)：它不再是排版的
     *   约束，只是"这段内容将来落进帧的哪一个槽"。条带是渲染的私事，排版不认识它。
     * 断页判据（页底溢出、显式分页符、竖排"吃掉本页剩余高度"）在连页下全部短路 ——
     *   漏掉任何一条都会留下"某一处还在翻页"的接缝。
     */
    bool continuous = false;

    /**
     * 连页下条带的高度（磅）= 页高 + 页间隙。分页下不用它（页高自己就是约束）。
     */
    float slab_pt = 841.89f;

    /**
     * A4 页度量。
     * @param margin_pt_ 四边页边距（磅），四边相等
     * @return 页宽页高取 A4、边距取给定值的度量
     */
    static PageMetrics A4(float margin_pt_ = 56.7f) {
        PageMetrics p;
        p.margins = PageMargins::all(margin_pt_);
        return p;
    }

    /**
     * 正文可用宽度（页宽减两侧边距）。断行的可用范围就是它。
     * @return 可用宽度（磅）
     */
    float contentWidthPt() const { return width_pt - margins.left - margins.right; }

    /**
     * 正文可用高度（页高减上下边距）。
     * @return 可用高度（磅）
     */
    float contentHeightPt() const { return height_pt - margins.top - margins.bottom; }

    /**
     * 连页下的条带高度：给 0 或负值时退回页高（永远不能返回 0，它是除数）。
     * @return 条带高度（磅），恒为正
     */
    float slabHeightPt() const { return slab_pt > 0.0f ? slab_pt : height_pt; }

    /**
     * 精确相等。引擎靠它判断"页度量动了没有"来决定要不要整篇重排：
     * 断行依赖内容宽度，任何一位浮点变化都可能让某一行多收/少收一个字。
     * 连页标志与条带高度同样要进比较 —— 它们改的是断页与页号，不改的话
     *   切模式不会重排，画面上就是"分页的排版 + 连页的落点"。
     */
    bool operator==(const PageMetrics& o) const {
        return width_pt == o.width_pt && height_pt == o.height_pt && margins == o.margins &&
               continuous == o.continuous && slab_pt == o.slab_pt;
    }

    /**
     * 不等 = 精确相等取反。
     */
    bool operator!=(const PageMetrics& o) const { return !(*this == o); }
};

/**
 * 把浮动图的框夹进版心（页内坐标）。
 *
 * 版心之外的那一截画不进页位图：图被页边界裁掉一半，看起来像被压扁了。
 *   比版心还宽的图没有"放进去"这回事，贴住左上角，多出来的部分照旧溢出。
 *
 * @param page 页度量（版心由它算）
 * @param w_pt 框宽（磅）
 * @param h_pt 框高（磅）
 * @param x_pt 出参：页内 x（磅）
 * @param y_pt 出参：页内 y（磅）
 */
void clampFloatBoxToTextArea(const PageMetrics& page, float w_pt, float h_pt, float* x_pt,
                             float* y_pt);

/**
 * 两个字重档：正文与加粗。可变字体的 wght 轴上有更细的档位，工具栏暂时只用到这两档。
 */
constexpr float kWeightRegular = 400.0f;
constexpr float kWeightBold = 700.0f;

/**
 * 一段"字符格式相同"的连续码点区间（升序、首尾相接，恰好铺满段落文本）。
 * 名字沿用 OOXML 的 run（w:r：a region of text with a common set of properties）。
 * 它切的是字符格式，与字体回退切出的 FontRun（切的是哪个字体画得出这个字）是两把
 * 不同的尺子，两者求交才是真正的整形边界。所以同一段文本会被切两次：先按本记录的
 * 区间，再在每段内按字形覆盖切。
 * 字段都是已解析的值：三层解析（文档默认、样式链、内联覆盖）在编译期做完，引擎不认识
 * 样式表。区间是否合法由 runsTileText 判定。
 */
struct FormatRun {
    /**
     * 区间起点（含），段内码点下标。
     */
    uint32_t cp_begin = 0;

    /**
     * 区间终点（不含）。与 cp_begin 一起构成半开区间。
     */
    uint32_t cp_end = 0;

    /**
     * 这一段用哪个字体（FontLibrary 里的下标）。
     */
    int font_index = 0;

    /**
     * 字号（磅）。
     */
    float size_pt = 12.0f;

    /**
     * 字重（可变字体的 wght 轴）。加粗就是 700，正文是 400。
     *
     * 它进实例缓存键：同一字号下 400 与 700 是两份 FontInstance，度量与字形都跟着变，
     *   排版宽度才与绘制一致。用"绘制时再加粗"的做法时字宽不变，相邻字会压在一起。
     */
    float weight = kWeightRegular;

    /**
     * 斜体：字体没有斜体面时由绘制层做合成倾斜（与字处理软件同一档做法）。
     */
    bool italic = false;

    /**
     * 下划线：由绘制层按行进画线，不参与排版度量。
     */
    bool underline = false;

    /**
     * 字色。
     */
    Color color = Color::rgb(24, 24, 24);

    /**
     * 语言标签（如 zh / mn），整形时用来选 OpenType 语言系统。按内容比，不按指针比。
     */
    const char* language = nullptr;

    /**
     * 这一段自己的回退链：font_index 之后依次尝试。空 = 关闭回退。
     */
    std::vector<int> fallback_font_indices;

    /**
     * 这一段覆盖几个码点。
     * @return 码点数（cp_end - cp_begin）
     */
    uint32_t codepointCount() const { return cp_end - cp_begin; }

    /**
     * 除区间之外的字段全相同。相邻同格式的文本段并成一个 run 时用它作判据。
     *
     * 逐字段比较：漏比一个字段会把两个不同格式的文本段并成一个，而合并是不可逆的静默丢格式。
     * 语言按内容比 —— 同一个标签在不同进程里的地址不一样，按指针比会把同一格式判成不同。
     * @param o 另一段
     * @return true = 除区间外逐字段相同
     */
    bool sameFormat(const FormatRun& o) const;

    /**
     * 区间与格式都相同。整段等价判据（投影与编译逐字段对齐）要用它。
     */
    bool operator==(const FormatRun& o) const {
        return cp_begin == o.cp_begin && cp_end == o.cp_end && sameFormat(o);
    }
};

/**
 * 行内对象的种类。
 *
 * 控件边界两种与原子一种都是"行内叶子、有宽度、参与断行"（起始边界 = 内容从此开始，
 * 结束边界 = 内容到此为止，原子 = 一个不可断的盒子）。域、链接边界按同一机制后续加成员：
 * 这一层不区分"行内 / 块级"，只认"这个位置有一项占宽度的东西"（方案一）。
 *
 * 三者的差别只有一处：宽度从哪来。控件边界的宽度由排版层按括号字形现算（只有它有
 * 字体回退结果），原子的宽高由产生端给定（它是图片或公式自己的几何）。
 */
enum class InlineObjectKind : uint8_t {
    ControlStart = 0,  // 控件起始边界（画开括号，内容在它右边）
    ControlEnd = 1,    // 控件结束边界（画闭括号，内容在它左边）
    Atom = 2,          // 行内原子（图片 / 公式）：一个不可断的盒子
};

/**
 * 行内原子的种类。只影响绘制（画什么），不影响排版 —— 排版只看宽高。
 */
enum class InlineAtom : uint8_t {
    None = 0,     // 不是原子
    Image = 1,    // 图片：画占位框与资源标识
    Formula = 2,  // 公式：画占位框与表达式（本期不做伪渲染，见 formula_spec.h）
};

/**
 * 行内对象：块流里除文本之外的一项（元素流在引擎输入侧的落点）。
 *
 * cp 是锚点（插到第一个 cp 不小于它的簇之前），与文本共用同一个轴，所以断行、增量、
 * 横向度量都只用这一个轴；但共用轴不等于共用位置：同一个 cp 上可以压着好几项，
 * "在它之前"与"在它之后"分不出来 —— 引擎的位置因此另有一个分量。
 *
 * owner 是产生端给的不透明句柄：排版层原样搬到 LineCluster::owner_control，不解释其
 * 含义。编译层给节点 id（控件给控件节点，原子给原子节点），同一份产物里只用一种口径。
 *
 * 控件边界的宽度不在这里（排版层按字体现算）；原子的宽高必须给，它是盒子全部的几何。
 */
struct InlineObject {
    /**
     * 锚点：段内码点下标（可等于段长，表示段尾）。
     */
    uint32_t cp = 0;

    /**
     * 这是控件边界还是原子。
     */
    InlineObjectKind kind = InlineObjectKind::ControlStart;

    /**
     * 不透明句柄（编译层给节点 id）；>-1 的才画括号。
     */
    int owner = -1;

    /**
     * 只读控件的括号画灰色。
     */
    bool read_only = false;

    /**
     * 盒子的显示尺寸（磅）。控件边界恒为 0，原子必须为正。
     */
    float w_pt = 0.0f;

    /**
     * 盒高（磅）。控件边界恒为 0，原子必须为正。
     */
    float h_pt = 0.0f;

    /**
     * 原子种类与内容指纹。前者决定画什么；后者决定"内容变了要不要重画" ——
     * 尺寸没变而内容变了（换了一张同样大的图）在几何上看不出来，行指纹只能靠它。
     * 两者都只在 kind == Atom 时有意义。
     */
    InlineAtom atom = InlineAtom::None;

    /**
     * 内容指纹。尺寸没变而内容变了（换了一张同样大的图）在几何上看不出来，行指纹只能靠它。
     */
    uint64_t content_hash = 0;

    /**
     * 这一枚原子是浮动图：流位置照常占一位，版面上一寸都不占（宽度由排版层按 0 处理）。
     *
     * 不占宽而保留流位置是刻意的：流位置要能与树对齐，否则光标与选区整体错位；
     *   而浮动图的几何由 LayoutFloat 另行给出（见 PlacedFloat）。
     */
    bool floating = false;

    /**
     * 空控件（开始与结束边界之间一个码点都没有）要画的提示文字；非空串才画。
     *
     * 它挂在起始边界上而不是自成一个对象：提示文字占宽度但不是可停光标的位置 ——
     * 自成一个对象会让流位置与排版槽一一对应这条不变量破掉，控件的跨度、空判定、
     * 侧别全都要跟着另立一套。挂在这里，排版只在两枚括号之间插一枚槽，流位置不动。
     */
    std::string hint;

    bool operator==(const InlineObject& o) const = default;
};

/**
 * 一个格位：这一块内容属于哪张表的哪一格。
 *
 * table 是不透明身份（编译层填模型节点 id，排版层不解释它），与 InlineObject::owner
 * 同一个分工 —— 引擎拿它回宿主换"这是哪张表"，自己不认识节点 id 的空间。
 */
struct CellSlot {
    /**
     * 表的身份（编译层填模型节点 id，排版层不解释）。
     */
    uint64_t table = 0;

    /**
     * 逻辑行号（在该表行序列里的下标）。
     */
    int row = 0;

    /**
     * 逻辑格号（在该行里的下标）。
     */
    int cell = 0;

    bool operator==(const CellSlot& o) const = default;
};

/**
 * 行高的取值规则（引擎侧）。模型层的 HeightRule 由编译层映射过来 —— 引擎不认模型枚举，
 * 就像它不认 Length（只吃磅值）。
 */
enum class RowHeightRule : uint8_t { Auto, AtLeast, Exact };

/**
 * 单元格内垂直对齐（引擎侧）。与模型层的 CellVAlign 同值域，名字刻意不同：
 * 两个类型在同一命名空间里，同名会撞。
 */
enum class BoxVAlign : uint8_t { Top, Center, Bottom };

/**
 * 单元格左右内边距（磅）。引擎配置常数，不来自样式表 —— 等 TableStyle 落地再接。
 * 排版按它算格内可用宽度，绘制按它画内容框；两处共用这一个数，各写一遍必然分叉。
 */
inline constexpr float kTableCellPadXPt = 4.0f;

/**
 * 单元格上下内边距（磅）。
 *
 * 取 2 磅而不是 0：零内边距时格内容与格线严丝合缝，光标正好压在上下两条线上
 * （看上去是光标凸出格子），空单元格尤其明显。上下各让出 2 磅之后，行高按
 * 内容高加两层内边距结算，格线也不再贴着字顶与字底。
 */
inline constexpr float kTableCellPadYPt = 2.0f;

/**
 * 正文里表格底边之后让出的间距（磅）。
 *
 * 块与块之间的间距全靠前一块的段后距给，而表格不是段落、没有那份记录可挂 —— 不补
 * 这一段，紧跟在表格后面的图片段落或文字会贴着表格底边，与"段落之间空一段"的观感
 * 不一致。表前不需要它：前一段的段后距已经加过了。
 *
 * 只作用于最外层表格，格内嵌套表的底边不额外让出 —— 那里格高由内容决定，多让一段会
 * 把行撑高。
 */
inline constexpr float kTableGapAfterPt = 6.0f;

/**
 * 表格在内容区里的水平位置（引擎侧）。只在表宽小于可用宽度时才有意义。
 */
enum class TableHAlign : uint8_t { Start, Center, End };

/**
 * 引擎侧的宽度：自动 / 绝对磅 / 相对可用宽度。用带标签的三态而不是一个浮点数 ——
 * 自动值与"零宽"必须能分开，百分比要等到知道内容区宽度才求值。
 */
struct WidthSpec {
    enum class Kind : uint8_t { Auto, Abs, Percent } kind = Kind::Auto;

    /**
     * Kind == Abs 时的宽度（磅）；其余情况无意义。
     */
    float pt = 0.0f;

    /**
     * Kind == Percent 时的比例（0..1），相对内容区宽度。
     */
    float ratio = 0.0f;

    /**
     * 造一个自动宽度。
     * @return 自动宽（宽度由内容与格宽推）
     */
    static WidthSpec makeAuto() { return WidthSpec{}; }

    /**
     * 造一个绝对宽度。
     * @param v 宽度（磅）
     * @return 绝对宽度
     */
    static WidthSpec makeAbs(float v) {
        WidthSpec w;
        w.kind = Kind::Abs;
        w.pt = v;
        return w;
    }

    /**
     * 造一个相对宽度。
     * @param r 比例（0..1，相对内容区宽度）
     * @return 相对宽度
     */
    static WidthSpec makePercent(float r) {
        WidthSpec w;
        w.kind = Kind::Percent;
        w.ratio = r;
        return w;
    }

    /**
     * 是不是自动宽度。
     * @return true = 宽度由排版按内容推
     */
    bool isAuto() const { return kind == Kind::Auto; }
};

/**
 * 行距：三态里的哪一档，以及那一档用的值。
 *
 * 磅值只有 exact 与 atLeast 看，倍数只有 auto 看 —— 两栏并存而不是折算成一个数，因为
 * "1.5 倍"与"26 磅"是两种不同的用户意图：换个字号，倍数的结果该跟着变，磅值不该。
 *
 * 值不大于 0 时退回 auto：只有倍数的文档 line_exact 恒为 0，不该因为规则字段被写成
 * exact 就排出一叠零高的行。
 */
struct LineSpacing {
    /**
     * 规则。
     */
    LineSpacingRule rule = LineSpacingRule::Auto;

    /**
     * 倍数（auto 档）。
     */
    float factor = 1.0f;

    /**
     * 固定值（exact / atLeast 档，磅）。
     */
    float exact_pt = 0.0f;

    /**
     * 这一档是否真的按磅值排。
     * @return true = 规则是 exact / atLeast 且值有效
     */
    bool usesExact() const {
        return (rule == LineSpacingRule::Exact || rule == LineSpacingRule::AtLeast) &&
               exact_pt > 0.0f;
    }

    /**
     * 逐字段相等。投影与编译的审计比的就是它。
     * @param o 另一份行距
     * @return true = 三个字段都相等
     */
    bool operator==(const LineSpacing& o) const {
        return rule == o.rule && factor == o.factor && exact_pt == o.exact_pt;
    }
};

/**
 * 一段的排版输入。
 *
 * 块不记"自己在哪一格"：表格结构里的格用块区间声明自己有哪些块，块到格的归属由
 * CellIndex 从那份声明反推。块上再存一条路径就是同一件事的第二个副本，两者可以互相
 * 矛盾，而矛盾只会以"块在格里、表框没开"这种形式在很远的地方出现。
 */
struct BlockSpec {
    /**
     * 段落文本的唯一真源是码点（UCS-4）。UTF-8 只是 I/O 表示，
     * 用 setText()/textUtf8() 在两个表示之间往返。
     * 为什么不让 std::string 当存储：编辑会改文本，而 UTF-8 存储下
     * "第 N 个字符"的定位需要从头解码（或者维护一张字节↔码点映射表）。
     * 码点数组里 offset 就是下标，插入/删除是 O(1) 定位 + O(n) 搬移。
     */
    doc::Text cp_text;

    /**
     * 主字体（FontLibrary 里的下标）。
     */
    int font_index = 0;

    /**
     * 字号（磅）。
     */
    float size_pt = 12.0f;

    /**
     * 书写模式（横排 / 竖排、列序方向）。断行沿哪个轴由它决定。
     */
    WritingMode wm = WritingMode::HorizontalTB();

    /**
     * 行距（三态：倍数 / 固定值 / 最小值）。竖排时它决定列宽。
     *
     * 单倍行高是字体自己的 ascent + descent + lineGap（Noto Sans SC 约 1.448 em），
     * 所以 1.0 就是 Word 的单倍行距，倍数与字号脱钩、加多少就松多少。以字号为基准时
     * 字体的纵向留白会把小倍数吃掉：1.0 到 1.45 都落在同一行高上。
     */
    LineSpacing line_spacing;

    /**
     * 段前距（磅）。页顶让出的那一段不加：分页之后段首的空白属于页边距。
     */
    float space_before_pt = 0.0f;

    /**
     * 段后距（磅）。
     */
    float space_after_pt = 6.0f;

    /**
     * 这一段之前强制分页。
     */
    bool page_break_before = false;

    /**
     * 语言标签（整形时选 OpenType 语言系统）。按内容比，不按指针。
     */
    const char* language = nullptr;

    /**
     * 默认字色。runs 为空或 run 没覆盖到的部分用它。
     */
    Color color = Color::rgb(24, 24, 24);

    /**
     * 段落默认字重。空段落（runs 为空）时它就是这一行"接着敲的字"的字重 ——
     * 空单元格改字重只能落在段落标记上，标记的字符格式从这里带进产物。
     */
    float weight = kWeightRegular;

    /**
     * 段落默认斜体（同 weight：空段落的字符格式只有这一份来源）。
     */
    bool italic = false;

    /**
     * 段落默认下划线（同上）。
     */
    bool underline = false;

    /**
     * 字体回退链。font_index 排在最前（不用写进来），其后依次尝试。
     * 为空 = 关闭回退，缺字直接出豆腐块。
     * 典型配置：{ 拉丁字体, 蒙文字体 } —— 主字体是中文字体时，
     * 上标数字、希腊字母、箭头这类中文子集没覆盖的码点就能被兜住。
     */
    std::vector<int> fallback_font_indices;

    /**
     * 字符格式 run（升序、首尾相接）。空表示"整段一个 run"，属性取上面那组块级字段 ——
     * 那条路径与加 run 之前逐字段相同，是迁移期的等价情形（见 effectiveRuns）。
     * 落盘判据同模型层：字符格式是作者意图，要落盘；整形切出的字体段是派生量，不落盘。
     */
    std::vector<FormatRun> runs;

    /**
     * 行内对象表（升序：先按 cp，同一个 cp 上按产生端给的次序）。
     *
     * 这是"排版只吃块流"的落点：要显示括号的控件边界以它为准，排版层不再需要一份
     * 外部的区间表 —— 两边各记一套正是"占位一次、不占位一次"那类不一致的温床。
     * 同一处边界上的多枚括号的先后次序由产生端定好（嵌套的收放顺序、空字段成对
     * 相邻都是那里的事），排版层只按数组顺序插入，不做第二次排序。
     *
     * 空表 = 一个对象都不插，排版与"没有表单"逐字节相同。
     */
    std::vector<InlineObject> objects;

    /**
     * 文本便捷接口：进出都是 UTF-8，内部始终是码点序列。
     * 换文本会清掉 runs：旧区间是按旧文本的码点下标算的，留着必然铺不满，
     * 于是静默失效。清掉是回到"整段一个 run"的等价情形，属性由块级字段给出。
     * objects 同样要清：它的 cp 锚点也是按旧文本算的，留着会把括号摆到别处去。
     * @param utf8 新的段落文本（UTF-8）
     */
    void setText(const std::string& utf8) {
        cp_text = utf8ToCodepoints(utf8);
        runs.clear();
        objects.clear();
    }

    /**
     * 段落文本的 UTF-8 表示（拷出一份）。
     * @return UTF-8 文本
     */
    std::string textUtf8() const { return codepointsToUtf8(cp_text); }

    /**
     * 这段有几个码点。
     * @return 码点数
     */
    size_t codepointCount() const { return cp_text.size(); }

    /**
     * 是不是空段。
     * @return true = 一个码点都没有
     */
    bool empty() const { return cp_text.empty(); }

    /**
     * 整段一个 run，属性取块级字段。空段给一个空区间（不是空序列）。
     * @return 覆盖整段的那个 run
     */
    FormatRun wholeRun() const {
        FormatRun r;
        r.cp_begin = 0;
        r.cp_end = static_cast<uint32_t>(cp_text.size());
        r.font_index = font_index;
        r.size_pt = size_pt;
        r.color = color;
        r.weight = weight;
        r.italic = italic;
        r.underline = underline;
        r.language = language;
        r.fallback_font_indices = fallback_font_indices;
        return r;
    }

    /**
     * 实际生效的 run 序列：runs 非空时就是它，为空时合成一个覆盖全段的 run。
     * 排版层只读这一个入口，于是"整段一个 run"与"显式一个 run"走同一条代码路径 ——
     * 这正是等价判据成立的原因。
     * @return 实际生效的 run 序列（空段也非空）
     */
    std::vector<FormatRun> effectiveRuns() const {
        if (!runs.empty()) return runs;
        return {wholeRun()};
    }

    /**
     * 逐字段相等：投影与编译是否给出同一份排版输入，判据就是它。
     *
     * 语言按内容比（理由见 FormatRun::sameFormat）。加新字段时这里不写就编不过 —— 那正是
     * 要的：判据漏比一个字段，等于允许两条路在这一点上静默分叉。
     */
    bool operator==(const BlockSpec& o) const {
        return cp_text == o.cp_text && font_index == o.font_index && size_pt == o.size_pt &&
               wm == o.wm && line_spacing == o.line_spacing &&
               space_before_pt == o.space_before_pt && space_after_pt == o.space_after_pt &&
               page_break_before == o.page_break_before && sameLanguage(language, o.language) &&
               color == o.color && weight == o.weight && italic == o.italic &&
               underline == o.underline && fallback_font_indices == o.fallback_font_indices &&
               runs == o.runs && objects == o.objects;
    }

    /**
     * 两个驻留语言标签是否同一个。空与空相等，空与非空不等。
     * @param a 语言标签
     * @param b 语言标签
     * @return true = 同为 nullptr 或内容相同
     */
    static bool sameLanguage(const char* a, const char* b) {
        if (a == b) return true;
        if (a == nullptr || b == nullptr) return false;
        return std::strcmp(a, b) == 0;
    }
};

/**
 * 一个逻辑格（不是网格格位）：单元格的覆盖范围由跨度表达，被合并掉的格位不存在。
 *
 * first_block / block_count 是它在 blocks 里的内容区间。空格的 count 为 0 ——
 * 排版层要按"一个空段落占一行"处理，不能当成这一格没有内容。
 */
struct LayoutCell {
    /**
     * 逻辑行号（在表的行序列里的下标）。
     */
    int row = 0;

    /**
     * 逻辑格号（在该行里的下标）。
     */
    int cell = 0;

    /**
     * 占几行（合并行）。1 = 不合并。
     */
    int row_span = 1;

    /**
     * 占几列（合并列）。1 = 不合并。
     */
    int col_span = 1;

    /**
     * 格内垂直对齐。
     */
    BoxVAlign v_align = BoxVAlign::Top;

    /**
     * 这一格的内容在 blocks 里的起始下标。
     */
    int first_block = 0;

    /**
     * 这一格有几个块。0 表示空段（仍占一行）。
     */
    int block_count = 0;
};

/**
 * 一行的排版输入。三个开关都在行上（Word 的 w:trPr），行高也在。
 */
struct LayoutRow {
    /**
     * 允许这一行跨页拆开（Word 的 w:cantSplit 取反）。
     */
    bool can_split_by_page = true;

    /**
     * 标题行：跨页时在每页重复。
     */
    bool is_header = false;

    /**
     * 这一行之前强制分页。
     */
    bool page_break_before = false;

    /**
     * 行高的绝对值。空表示没有绝对值（规则为 Auto，或者模型给的是编译期算不出的相对长度）。
     */
    std::optional<float> height_pt;

    /**
     * 行高规则：Auto 按内容、AtLeast 取下限、Exact 固定。
     */
    RowHeightRule height_rule = RowHeightRule::Auto;
};

/**
 * 一张表的排版输入。rows / cols 是派生网格的尺寸（由跨度推出来，与模型层 buildGrid
 * 同一口径），不是"行记录条数"与"列定义条数"。
 *
 * col_widths 的长度恒为 cols：显式列宽可能比网格短，短的部分补自动。
 */
struct LayoutTable {
    /**
     * 表的身份（编译层给，原样带回 PlacedTable::handle）。
     */
    uint64_t handle = 0;

    /**
     * 整表宽度。
     */
    WidthSpec width;

    /**
     * 表在内容区里的水平位置（只在表宽小于可用宽度时生效）。
     */
    TableHAlign align = TableHAlign::Start;

    /**
     * 派生网格的行数（由跨度推出来）。
     */
    int rows = 0;

    /**
     * 派生网格的列数。
     */
    int cols = 0;

    /**
     * 列宽，长度恒为 cols。显式列宽比网格短时，短的部分补自动。
     */
    std::vector<WidthSpec> col_widths;

    /**
     * 每行的开关与行高，长度恒为 rows。
     */
    std::vector<LayoutRow> row_specs;

    /**
     * 逻辑格（含跨度），按行列顺序。
     */
    std::vector<LayoutCell> cells;
};

/**
 * 一张浮动图的排版输入：锚在某一块的某个码点位置，尺寸与环绕方式由作者给。
 *
 * 坐标是相对的：dx / dy 相对锚点那一段所在行的内容区原点，于是"改页宽、改字号"
 * 都不会把图钉死在某个绝对坐标上。
 */
struct LayoutFloat {
    /**
     * 图片节点的身份（命中和选中靠它回宿主换下标）。编译层填节点 id。
     */
    uint64_t handle = 0;

    /**
     * 锚段（块下标）。
     */
    int para = 0;

    /**
     * 锚点（段内码点下标）。只用于排序与诊断：同一段里多张图的先后。
     */
    uint32_t cp = 0;

    /**
     * 显示尺寸（磅）。
     */
    float w_pt = 0.0f;

    /**
     * 同上，高。
     */
    float h_pt = 0.0f;

    /**
     * 环绕方式。Behind / InFront 不占位。
     */
    ImageWrap wrap = ImageWrap::None;

    /**
     * 相对锚点内容区原点的偏移（磅）。
     */
    float dx_pt = 0.0f;

    /**
     * 同上，纵向。
     */
    float dy_pt = 0.0f;

    /**
     * 内容指纹：换了一张同样大的图时几何不变，行指纹靠它发现"画出来的东西变了"。
     */
    uint64_t content_hash = 0;

    bool operator==(const LayoutFloat&) const = default;
};

/**
 * 一张已放置的浮动图：页号与页内框（磅），绘制、命中与选中都读它。
 */
struct PlacedFloat {
    /**
     * 图片节点身份，与 LayoutFloat::handle 同一个值。
     */
    uint64_t handle = 0;

    /**
     * 落在第几页。
     */
    int page = 0;

    /**
     * 页内左边。
     */
    float x = 0.0f;

    /**
     * 页内上边。
     */
    float y = 0.0f;

    /**
     * 宽。
     */
    float w = 0.0f;

    /**
     * 高。
     */
    float h = 0.0f;

    /**
     * 环绕方式（决定绘制次序与是否参与命中优先级）。
     */
    ImageWrap wrap = ImageWrap::None;

    /**
     * 内容指纹。
     */
    uint64_t content_hash = 0;
};

/**
 * 一条已放置的表格行（页片段）。跨页拆开的一行在每页各有一条。
 *
 * cont_top / cont_bottom 说明这一片段的上边或下边是"接着别处"的 —— 它不是这一行自己的
 * 边界，而是切片切出来的。分页模式下这两条边上要补画格线（看起来是表格接着长在下一页），
 * 连页模式下不画（条带的切口不是页边界，画了就变成断口）。
 */
struct PlacedRow {
    /**
     * 逻辑行号（在这张表的行序列里的下标）。
     */
    int row = 0;

    /**
     * 这一片段落在第几页。
     */
    int page = 0;

    /**
     * 行顶（页内坐标，磅）。
     */
    float y = 0.0f;

    /**
     * 这一片段的高（磅）。
     */
    float h = 0.0f;

    /**
     * 上边是切出来的（接着上一片段），不是这一行自己的边。
     */
    bool cont_top = false;

    /**
     * 下边是切出来的（接着下一片段）。
     */
    bool cont_bottom = false;
};

/**
 * 页内一点算不算落在格上的横向容差（磅）。
 *
 * 放宽到半磅是为了抓住表的左右边框：贴边那一下点仍有格可落。点击命中与悬停取栅格
 * 手柄共用这一个值，两处各取一套时同一处点两下会给出不同结果。
 */
inline constexpr float kCellHitTolX = 0.5f;

/**
 * 页内一点算不算落在格上的纵向容差（磅）。
 *
 * 比横向紧两个数量级：行与行上下相接，纵向一放宽就等于把"表上/表下那一点"也判成格内
 * —— 表格后面紧跟的那一段就点不进去了。
 */
inline constexpr float kCellHitTolY = 0.01f;

/**
 * 一张已放置的表格：位置、列边界（相对表格左边，长度 cols+1）与它的行片段。
 *
 * 它只是绘制与命中用的几何投影，不是排版输入 —— 排版输入是 LayoutTable。
 */
struct PlacedTable {
    /**
     * 表的身份，与 LayoutTable::handle 同一个值。
     */
    uint64_t handle = 0;

    /**
     * 这张表在块流里的起始段。增量排版据此判断"这一段的变更要不要连整张表一起重排"，
     * 以及"截断到第 k 段时，哪些表的几何要一起丢掉"（见 EditorEngine::truncateLayoutAt）。
     * 它是表自己的属性（列定义与格 → 段映射的产物），与位置无关。
     */
    int first_block = 0;

    /**
     * 表格左边（页内坐标，磅）。
     */
    float x = 0.0f;

    /**
     * 表格总宽（磅）。
     */
    float w = 0.0f;

    /**
     * 行数。
     */
    int rows = 0;

    /**
     * 列数。
     */
    int cols = 0;

    /**
     * 列边界，相对表格左边，长度 cols+1。
     */
    std::vector<float> col_x;

    /**
     * 逻辑格（含跨度）。绘制要靠它算每个格自己的框 —— 只画格的外框，合并格内部就不会
     * 多出几条线，而"哪些列边界被跨过"这件事也不必另存一份掩码。
     */
    std::vector<LayoutCell> cells;

    /**
     * 这一张表的行片段（跨页拆开的一行每页各一条）。
     */
    std::vector<PlacedRow> placed;

    /**
     * 找出覆盖某个网格格位的逻辑格。
     *
     * 格是按跨度存的，网格列号要按同行的跨度累加才得到 —— 这个换算只有一处（这里），
     * 命中、选区与绘制都读它，免得三处各自累加出三份结果。
     * @param row 逻辑行号
     * @param col 网格列号
     * @param col_lo 写回这一格覆盖的起始列（含），可为 nullptr
     * @param col_hi 写回这一格覆盖的结束列（不含），可为 nullptr
     * @return 覆盖这一点的格；越界或那一格被合并掉时为 nullptr
     */
    const LayoutCell* cellAt(int row, int col, int* col_lo, int* col_hi) const;

    /**
     * 每个格的起始网格列，次序与 cells 一致。
     *
     * 只能按占位推：跨行的格在它盖住的每一行里都占着列，只按本行的格累加跨度会把下面
     * 那些行的列整体左移，合并之后右边那些格就点不中了。占位从左上往右下扫一遍，
     * 每格取第一段在它盖住的每一行都空着的列。
     * @return 起始网格列，与 cells 等长；放不下的格为 -1
     */
    std::vector<int> gridColStarts() const;

    /**
     * 这一格在本行里占的起始网格列。
     *
     * 列号只能按"同一行、格号在前的那些格的跨度之和"得到，不能从格号直接算 —— 合并格
     * 之后的格号与网格列不是一回事。取格框、取栅格都读这一处。
     * @param cell 逻辑格
     * @return 起始网格列；这一格不在表里时为 -1
     */
    int gridColOf(const LayoutCell& cell) const;

    /**
     * 这一页上盖住某个纵向位置的行。
     *
     * 行片段跨页拆开时同一行每页各一条，所以查询必须带页号。容差让"点在格的下内边距里"
     * 仍然算这一行；两条行只沾到容差时取真正包含这一点的那一条。
     * @param page 页号
     * @param y_pt 页内纵向坐标（磅）
     * @param tol_pt 容差（磅）
     * @return 逻辑行号；这一页上没有行覆盖该位置时为 -1
     */
    int rowIndexAt(int page, float y_pt, float tol_pt) const;

    /**
     * 某一行在某一页上的纵向范围（页内坐标，磅）。
     * @param page 页号
     * @param row 逻辑行号
     * @param y 写回上边，可为 nullptr
     * @param h 写回高，可为 nullptr
     * @return 这一行在这一页上有片段时为 true
     */
    bool rowBandOnPage(int page, int row, float* y, float* h) const;

    /**
     * 这一页上有没有这一张表的行片段。
     * @param page 页号
     * @return 有片段时为 true
     */
    bool hasRowsOnPage(int page) const;

    /**
     * 页内一点落在哪一格。
     *
     * 「点 → 格」只有这一处实现：点击命中与悬停取栅格都读它。
     * @param page 页号
     * @param x_pt 页内横向坐标（磅）
     * @param y_pt 页内纵向坐标（磅）
     * @param tol_x_pt 横向容差（磅）
     * @param tol_y_pt 纵向容差（磅）
     * @param row_out 写回逻辑行号，可为 nullptr
     * @return 命中那一格；这一点不在这张表上或落在被合并掉的位置时为 nullptr
     */
    const LayoutCell* cellAtPt(int page, float x_pt, float y_pt, float tol_x_pt, float tol_y_pt,
                               int* row_out = nullptr) const;

    /**
     * 某一格在某一页上的框（页内坐标，磅）。
     *
     * 一个逻辑格在跨页的行上会出现多次：每一页那一段各是一个框。选区反白与命中都按它铺，
     * 而不是按行盒 —— 内容已经在上一页结束的格在续排片段上没有行盒，按行盒铺会漏掉整段。
     * @param cell 逻辑格
     * @param page 页号
     * @param x 写回左边（页内坐标）
     * @param y 写回上边（页内坐标）
     * @param w 写回宽
     * @param h 写回高
     * @return 这一格在这一页上有框时为 true
     */
    bool cellBoxOnPage(const LayoutCell& cell, int page, float* x, float* y, float* w,
                       float* h) const;
};

/**
 * runs 是否恰好铺满 [0, cp_text.size())：升序、首尾相接、无空洞、无重叠、区间非空。
 * runs 为空时恒真（等价情形，属性由块级字段给出）。
 * 返回 false 时把第一处不合法的原因写进 why；why 可以为空指针。
 * @param b 待检的块
 * @param why 写回第一处不合法的原因（可为 nullptr）
 * @return true = runs 恰好铺满文本（升序、首尾相接、无空洞、无重叠）
 */
bool runsTileText(const BlockSpec& b, std::string* why);

/**
 * 在已解析出的 run 序列里找覆盖该码点的那个；越界或序列为空返回 nullptr。
 * 线性扫描：run 的数量就是"一段里格式变化的次数"，真实文档里是个位数。
 * 入参是调用方自己持有的序列（effectiveRuns 的返回值是按值给的，不能取它的引用）。
 * @param runs run 序列（调用方自己持有的那份）
 * @param cp 段内码点下标
 * @return 覆盖这个码点的 run；越界或序列为空为 nullptr
 */
const FormatRun* runCovering(const std::vector<FormatRun>& runs, uint32_t cp);

/**
 * objects 是否合序：cp 非降、且每个 cp 都不越过段长（等于段长表示锚在段尾）。
 * 为什么必须判而不是"约定一下"：排版层按数组顺序线性插槽，顺序一坏，括号的层级
 * 就画错（][ 变 [] ），而画面看上去仍然"有括号"，不容易发现。
 * 返回 false 时把第一处不合法的原因写进 why；why 可以为空指针。
 * @param b 待检的块
 * @param why 写回第一处不合法的原因（可为 nullptr）
 * @return true = objects 按 cp 非降且不越过段长
 */
bool objectsOrdered(const BlockSpec& b, std::string* why);

/**
 * 已定位的字形。x/y 是"字形原点"在页面坐标系里的位置（页面左上为原点，单位磅）。
 */
struct PlacedGlyph {
    /**
     * 字形下标（字体里的 glyph id）。
     */
    uint32_t glyph_id = 0;

    /**
     * 画出这个字形的字体实例。
     */
    const FontInstance* font = nullptr;

    /**
     * 字形原点的 x（页面坐标，磅）。
     */
    float x_pt = 0.0f;

    /**
     * 字形原点的 y（页面坐标，磅）。
     */
    float y_pt = 0.0f;

    /**
     * 这个字形的颜色（来自覆盖它的那个 run）。
     */
    Color color = Color::rgb(0, 0, 0);

    /**
     * 斜体：绘制层做合成倾斜。字宽不变，与字处理软件的合成斜体同一档。
     */
    bool italic = false;

    /**
     * 下划线：绘制层按行进画线，不参与排版。
     */
    bool underline = false;

    /**
     * 这个字形自己的字号（磅）：下划线的位置与粗细按它算。
     */
    float size_pt = 12.0f;

    /**
     * 这个字形的推进（磅，沿行内轴）：下划线要靠它量到"这个字的右端"。
     *
     * 不给的话绘制只能按 0.6 em 估，汉字 1 em、拉丁半角 0.5 em —— 一行末尾那一截总是
     *   短掉一大块。它是排版的产物，不是派生出的估算量。
     */
    float advance_pt = 0.0f;

    /**
     * 这个字形用的字重（可变字体的 wght 轴）。
     *
     * 它同时决定"画出来是哪一份字形"与"进位移是多少"，所以在字形上留一份 —— 页指纹
     *   按字形算，而它看不见字体实例本身（见 engine_layout.cpp 的 mixGlyph）。
     */
    float weight = 400.0f;

    /**
     * 落在第几页。
     */
    int page = 0;

    /**
     * 横排 = 行号；竖排 = 列号。与 LineBox::line 对齐。
     */
    int line = 0;

    /**
     * 是不是竖排。绘制按它选轴。
     */
    bool vertical = false;

    /**
     * 渲染时要绕 (x_pt, y_pt) 顺时针旋转的角度。蒙文竖排 = 90，
     * 表示"横排成型 + 整段侧倒"，详见 WritingMode::isSideways()。
     */
    float rotate_deg = 0.0f;

    /**
     * 段内码点下标（与文档偏移同一把尺子）。
     */
    uint32_t cluster = 0;

    /**
     * 所属块下标 —— 会话期定位时要靠它回到文档。
     */
    int paragraph = 0;
};

/**
 * 表单括号的占位槽宽度 = 括号字形的 advance ＋ 这一点间隙（磅）。
 * 为什么间隙也要进排版，而不是画的时候往外挪一点：
 * 画的时候挪，就把"槽"和"字形"分成了两套账 —— 括号会盖到相邻的字上，
 * 或者槽里空一条、字却已经走了。这和"括号必须进文档流"是同一条道理：
 * 凡是占画面的宽度，就必须是排版算出来的那一个。
 * 所以这里只留一个常数，排版和绘制共用它（见 engine_paint.cpp 的 rasterControlMarks）。
 */
inline constexpr float kControlBracketSlotGapPt = 1.5f;

/**
 * 提示文字在某个字体下的排版宽度（磅）。
 *
 * 排版（槽宽）与绘制（画灰字）共用它：各算一套的话，槽宽与画出来的字对不上 ——
 * 要么画出槽外，要么槽里空一段。逐字取名义字形的推进，缺字按 0 宽跳过。
 * @param f 控件边界那一侧正文 run 的字体
 * @param utf8 提示文字
 * @return 宽度（磅）
 */
float hintAdvancePt(const FontInstance* f, const std::string& utf8);

/**
 * 表单括号占位是开括号还是闭括号。
 * 这个区分只服务一件事：同一个 cp 上会出现两个视觉位置
 * （[ 的前/后、] 的前/后），而光标必须落在朝向内容的那一侧：
 * 开括号：内容在它右边 → 光标落在它之后；
 * 闭括号：内容在它左边 → 光标落在它之前。
 * 光靠 cp 区分不出来（两者的 cp 都等于边界偏移），所以必须显式记下来。
 * 漏掉这一条的症状：光标停在字段最后一个字上，
 * 再按一次右方向键，光标会直接跳到 ] 的右边 —— 中间的
 * "在 ] 之内"那个位置被跳过了，看起来像光标从字里蹿出了框外。
 */
enum class BracketSide : uint8_t { None = 0, Open = 1, Close = 2 };

/**
 * 行内的一个"簇"在行内轴上的位置。
 * 这是光标/命中测试所依赖的几何的正面半边：
 * 簇首码点 --(这一层)--> 沿行内轴的磅坐标。
 * 反向查询（磅坐标 → 码点偏移）由引擎层在 LineBox 数组上做。
 */
struct LineCluster {
    /**
     * 簇首的段内码点下标。
     */
    uint32_t cp = 0;

    /**
     * 相对行首的沿行内轴偏移（磅）。
     */
    float inline_pt = 0.0f;

    /**
     * 这个簇是"表单括号的占位"：只占 advance，没有字形。
     * 它存在的意义就是让括号进文档流 —— 括号的宽度被算进断行、
     * 行盒长度和每个簇的位置里，于是它把后面的字挤开，
     * 而不是盖在字上（overlay）。
     * controlRects 靠这个标记把括号排除在"控件矩形"之外：
     * 括号在内容之外（[内容]），控件矩形只该框住内容。
     */
    bool placeholder = false;

    /**
     * 占位括号是开括号还是闭括号。
     *
     * 非括号的占位（行内原子）恒为 None，所以 None 有两种来源：不是占位，或者是占位但
     * 不是括号。反过来读（"占位就一定是括号"）会错：绘制按它分派画法，把 None 当成开
     * 括号会在每枚原子左边画出一枚开括号。它只被绘制与行指纹用 —— 光标与命中测试按
     * cp 与 placeholder 定位，不看这个字段。
     */
    BracketSide bracket_side = BracketSide::None;

    /**
     * 这个占位是只读字段的边界（决定括号用灰还是蓝）。
     * 只有占位簇有意义。
     * 放在这里而不是让绘制层反查控件：插槽的是排版，它知道自己在给谁插 ——
     * 反查要拿 (段, cp, 朝向) 去控件集合里匹配，遇到相邻/嵌套字段就会出现歧义。
     */
    bool read_only = false;

    /**
     * 画这枚括号用的字体；只有括号占位有意义，原子与普通簇为空。
     *
     * 取控件边界那一侧正文 run 的字体，而不是块主字体：括号是内容的边框，换字体时
     * 字形与槽宽都要跟着换。为空时退回行盒的 bracket_font（块主字体）。
     */
    const FontInstance* bracket_font = nullptr;

    /**
     * 这个占位属于哪个控件（控件集合里的下标）；非占位恒为 -1。
     * 与 read_only 同一个理由：只有插槽那一步知道答案。命中测试要用它来回答
     * "用户点的这个括号是谁的" —— 共享边界上两个字段的括号挨在一起（][），
     * 光凭 (cp, 朝向) 反查，空字段与相邻字段会给出同一个答案。
     *
     * 原子占位放的是原子的节点 id（它没有控件下标，而节点 id 同样是稳定身份）——
     * 绘制要靠它回树里取"框里写什么"。两者都是"不透明句柄"的下游形态，见
     * InlineObject::owner。
     */
    int owner_control = -1;

    /**
     * 行内原子：这是哪一种（不是原子时为 None）。绘制按它分派画法。
     */
    InlineAtom atom = InlineAtom::None;

    /**
     * 原子的盒高（磅）；不是原子时为 0。盒宽不在这里 —— 它是"到下一个簇的距离"，
     * 也就是 inline_pt 的差（最后一个簇用 LineBox::inline_end_pt）。
     *
     * 绘制要它：框的高度是原子自己的，不是行盒的 —— 一行里混排大字号文字时行盒会更高。
     */
    float atom_h_pt = 0.0f;

    /**
     * 这个簇是"空控件的提示文字"：占宽度、不产生字形，也不是一个可以停光标的位置。
     *
     * 它夹在一对括号之间（[提示]），所以同一偏移上的簇序是 括号、提示、括号。侧别是
     * "跨过了几个位置"，数格子时必须跳过它 —— 不跳的话"控件之后"会落在闭括号上（差
     * 一个括号宽），而控件内部的插入点会多出一个。
     */
    bool control_hint = false;

    /**
     * 原子的内容指纹（见 InlineObject::content_hash）。行指纹要它 ——
     * 几何一样而内容变了（换了一张同样大的图、改了公式表达式）在行盒上看不出来。
     */
    uint64_t content_hash = 0;
};

/**
 * 一个行盒（竖排时是"列盒"）。
 * 为什么必须有这一层：
 * "画光标"需要 (段, 码点偏移) → 像素矩形；
 * "鼠标点选"需要 像素点 → (段, 码点偏移)；
 * "选区高亮"需要 两个偏移 → 一串矩形。
 * 三个能力的本质是同一份几何的正反查。没有行盒，这三个都得各写一遍，
 * 而且三种书写轴各写一遍 = 六份互相不一致的实现。
 */
struct LineBox {
    /**
     * 所属块下标。
     */
    int paragraph = 0;

    /**
     * 全局行/列号，与 PlacedGlyph::line 对齐。
     */
    int line = 0;

    /**
     * 落在第几页。
     */
    int page = 0;

    /**
     * 是不是竖排。
     */
    bool vertical = false;

    /**
     * 是不是侧倒（蒙文竖排：横排成型后整段转 90 度）。
     */
    bool sideways = false;

    /**
     * 这一行的主字号（磅）。
     */
    float size_pt = 12.0f;

    /**
     * 行盒在"块轴"上的范围（横排 = y 上下；竖排 = x 左右），页面坐标（磅）。
     * 横排：block_lo = 基线 - ascent，block_hi = 基线 + descent —— 这是字面盒，
     * 字形、脏区、图片锚点都用它；反白与插入点用的是整条行槽（见 slotLoPt）。
     */
    float block_lo_pt = 0.0f;

    /**
     * 字面盒之上、行槽之内那一段空白（磅，页面坐标方向）。多倍行距占一半、另一半在字面
     * 盒之下（见 lineLeadTopFor）；固定值与最小值是 0，竖排也是 0。
     *
     * 有它，'行槽顶' 才是 block_lo - lead_top_pt，而不是 block_lo。
     */
    float lead_top_pt = 0.0f;

    /**
     * 块轴范围的上界（页面坐标，磅）。
     */
    float block_hi_pt = 0.0f;

    /**
     * 横排行盒自己的纵向度量：基线以上、以下各多少，以及沿块轴推进多少（行高）。
     *
     * 排版时由 breakLine 定下（ascent / descent / line_h），增量平移必须拿它们原样重算，
     * 不能用 block_hi - block_lo 或"单倍行高 × 倍数"反推：反推出来的是另一个数
     * （差一个末位），而判据是"增量与从零重排逐位相同"。行高也不等于单倍行高乘倍数 ——
     * 一行里有比文字更高的行内原子时它更大（见 breakLine 的 line_h）。
     *
     * 竖排的列盒不维护这三个值：列沿块轴的推进是列宽，由换带规则算，不是这三样。
     */
    float ascent_pt = 0.0f;

    /**
     * 基线以下多少（磅）。
     */
    float descent_pt = 0.0f;

    /**
     * 沿块轴推进多少（行高，磅）。
     */
    float advance_pt = 0.0f;

    /**
     * 绘制这一行时是否要裁到行盒。
     *
     * 只有"固定值行距且行高比墨迹矮"时成立：Word 在这种情形下裁掉超出的字，而按倍数
     *   压行时不裁（相邻行重叠，字压在一起但看得见）。绘制侧只在它为真时进裁剪栈，
     *   所以普通文档一个字节的额外开销都没有。
     */
    bool clip_text = false;

    /**
     * 行内轴的起点（横排 = 基线起始 x；竖排 = 列顶 y），页面坐标（磅）。
     */
    float inline_origin_pt = 0.0f;

    /**
     * 行末位置，相对行首（磅）。
     */
    float inline_end_pt = 0.0f;

    /**
     * 本行覆盖的段内码点区间 [first_cp, last_cp)
     */
    uint32_t first_cp = 0;

    /**
     * 本行的结束偏移（不含）。
     */
    uint32_t last_cp = 0;

    /**
     * 这一行所属格在行内轴上的范围（页内坐标）。两者相等表示不在表格里。
     *
     * 命中测试靠它：格里的文字只占开头一小段，按"行内范围"判势力范围，点格子的后半段
     * 就会跳到下一格。格范围是这一格的宽度，与文字多长无关。
     */
    float cell_lo_pt = 0.0f;

    /**
     * 格内容区在行内轴上的上界（页内坐标）。
     */
    float cell_hi_pt = 0.0f;

    /**
     * 这一行所属格的外框在行内轴上的范围（页内坐标）。两者相等表示不在表格里。
     *
     * 与 cell_lo_pt / cell_hi_pt 差两层内边距：那一对是内容区，这一对是格子的边框。
     * 命中测试用内容区（它要的正是"文字左沿之前那一条"），选区高亮用外框 —— 反白要铺满
     * 整格，留一条内边距的空隙就是"选区的左右还有缝"。
     */
    float cell_box_lo_pt = 0.0f;

    /**
     * 格外框在行内轴上的上界（页内坐标）。
     */
    float cell_box_hi_pt = 0.0f;

    /**
     * 这一行所属格的块轴范围（页内坐标）。两者相等表示不在表格里。
     *
     * 选区高亮靠它：一格被整段选中时要整格反白，而行盒只盖住文字那一带 —— 显式行高
     * （AtLeast）比内容高时，上下那两截空白也得算进高亮。排版在行高结算之后回填。
     */
    float cell_top_pt = 0.0f;

    /**
     * 格的块轴下界（页内坐标）。
     */
    float cell_bottom_pt = 0.0f;

    /**
     * 一行可用的横向区间（相对 inline_origin_pt，磅）。
     */
    struct Span {
        /**
         * 区间左沿（相对行首）。
         */
        float lo_pt = 0.0f;

        /**
         * 区间右沿（相对行首）。
         */
        float hi_pt = 0.0f;
    };

    /**
     * 这一行被四周型浮动图挖开后剩下的横向区间。
     *
     * 单段行留空：它的可用区间就是 [0, inline_end_pt] 一段，填上只是重复。
     * 两段以上才填 —— 簇的 inline_pt 在空隙处直接跳过去，绘制、命中与光标都按簇走，
     * 不必认识这里；只有选区要按它切矩形，否则一条横跨图的选区会把图也涂上。
     */
    std::vector<Span> spans;

    /**
     * 行内簇（cp 升序）
     */
    std::vector<LineCluster> clusters;

    /**
     * 画表单括号用的"标点参考"
     * 为什么必须放在行盒里、而不是绘制层自己去找一个参考字形：
     * 占位槽的位置和宽度是排版算出来的（见 layout.cpp 里插占位簇那一段），
     * 画括号就必须用排版的同一份坐标。绘制层如果自己去扫字形、自己算基线，
     * 就又变成"排版算占位、画字另算一套" —— 那是"括号压在文字上"
     * "槽里空一条而字已经走了"这一整个 bug 家族的根源。
     * 更要紧的是空字段（删光字段内容后括号不画了）：
     * 一整行里可能一个字形都没有，任何"拿一个已画字形当参考"的做法都会
     * 直接放弃绘制 —— 而占位槽明明已经把宽度占掉了（"位置还占着"）。
     * 行盒里存着字体与基线，这种情况才画得出来。
     */
    const FontInstance* bracket_font = nullptr;

    /**
     * 横排：字形原点的 y（= 基线）；竖排：字形原点的 x（列心 / 侧倒笔位），
     * 也就是与 PlacedGlyph::x_pt 同一把尺子。
     * 括号沿行内轴的位置来自占位簇自己的 inline_pt，不在这里。
     */
    float bracket_axis_pt = 0.0f;

    /**
     * 落点画在括号【之外】时的哨兵值。
     *
     * 宿主与探针只知道"控件下标"，分不出共享边界的两侧，于是用这个负数说"贴外侧"。
     * 引擎收到它就换成侧别（见 offsetAtSide）：本偏移上的闭括号数。
     */
    static constexpr int kInlineOffsetOuter = -2;

    /**
     * 本偏移上第一枚簇在 clusters 里的下标；本行没有这个偏移的簇时返回 clusters.size()。
     * 同一偏移上的簇必然连续：占位槽（括号、提示文字、原子）在前、真实簇在后
     * （见 layout.cpp 的 emitSlot）。
     * @param cp 段内码点下标
     * @return 第一枚簇的下标；本行没有这个偏移时为 clusters.size()
     */
    size_t firstClusterAt(uint32_t cp) const {
        for (size_t i = 0; i < clusters.size(); ++i)
            if (clusters[i].cp == cp) return i;
        return clusters.size();
    }

    /**
     * 本偏移上占位槽的枚数 —— 也就是这个偏移的侧别上限。
     *
     * 提示文字那一枚槽不算：它占宽度但不占位置，侧别数的是"跨过了几个位置"。
     * @param cp 段内码点下标
     * @return 这个偏移上占位槽的枚数（侧别上限）
     */
    int bracketCountAt(uint32_t cp) const {
        int n = 0;
        for (size_t i = firstClusterAt(cp); i < clusters.size() && clusters[i].cp == cp; ++i)
            if (clusters[i].placeholder && !clusters[i].control_hint) ++n;
        return n;
    }

    /**
     * (码点偏移, 侧别) → 相对行首的沿行内轴位置（磅）。侧别 = 在这个偏移上跨过了几枚簇。
     *
     * 偏移本身不够：同一个偏移上可能压着好几枚括号，"控件之前 / 进入控件 / 内容末尾 /
     *   控件之后"是好几个互不相同的插入点（flow.h 的四个流位置），偏移把这个分量丢了。
     * 补回来的那个分量排版侧本来就记着：括号占位各占一枚簇，侧别就是"数格子"。取值 0 =
     *   所有括号之前；逐枚数下去；到 bracketCountAt(cp) 就是本偏移上所有括号之后。
     * @param cp 段内码点下标
     * @param side 跨过几枚括号（0 = 全部之前）
     * @return 相对行首的沿行内轴位置（磅）
     */
    float offsetAtSide(uint32_t cp, int side) const {
        if (clusters.empty()) return 0.0f;
        const size_t k0 = firstClusterAt(cp);
        if (k0 >= clusters.size()) {
            // 本行里没有这个偏移的簇：退化成"最后一个 cp <= target 的簇起点"。
            if (cp >= last_cp) return inline_end_pt;
            float best = clusters.front().inline_pt;
            for (const LineCluster& c : clusters) {
                if (c.cp > cp) break;
                best = c.inline_pt;
            }
            return best;
        }
        const int n = bracketCountAt(cp);
        const int s = (side < 0) ? 0 : (side > n ? n : side);
        // 侧别 0 = 所有槽之前。
        if (s == 0) return clusters[k0].inline_pt;
        // 数格子时跳过提示文字：它不占位置，所以"跨过 s 个位置"不能按簇下标直接加 s。
        //   位置取第 s 枚槽的右缘（紧跟它的那一枚簇的起点），不是下一枚槽的起点 ——
        //   提示文字夹在两枚括号之间时，两者差着一句话的宽度。
        int seen = 0;
        for (size_t k = k0; k < clusters.size() && clusters[k].cp == cp; ++k) {
            if (clusters[k].control_hint) continue;
            ++seen;
            if (seen != s) continue;
            const size_t next = k + 1;
            return (next < clusters.size()) ? clusters[next].inline_pt : inline_end_pt;
        }
        // 跨过了本偏移上全部槽：位置在最后一个槽的右缘，也就是这一偏移之后第一枚簇的起点。
        size_t k = k0;
        while (k < clusters.size() && clusters[k].cp == cp) ++k;
        return (k < clusters.size()) ? clusters[k].inline_pt : inline_end_pt;
    }

    /**
     * 行槽顶（块轴上的页面坐标）：字面盒顶再往上让出 lead_top_pt。
     *
     * 插入点与反白都从这里起、盖满 slotSpanPt()，于是多倍行距下字落在反白中间。
     * 竖排的 lead_top_pt 恒为 0（列盒本来就是行槽），这里就退回 block_lo_pt。
     * @return 行槽起点（磅）
     */
    float slotLoPt() const {
        return (advance_pt > 0.0f) ? (block_lo_pt - lead_top_pt) : block_lo_pt;
    }

    /**
     * 光标与选区沿块轴的高度：整条行槽，不是字身盒。
     *
     * 字面盒（block_hi - block_lo）负责字形与命中；插入点与反白要盖住这一行占的位置 ——
     * 不并进去就会出现"1.5 倍行距下反白一条一条断开、光标比行矮一截"。
     * 浏览器按行盒画、Word 按行距画，这里取同一个口径。竖排的列盒不维护 advance_pt，
     * 退回列宽。
     * @return 块轴跨度（磅）
     */
    float slotSpanPt() const {
        return (advance_pt > 0.0f) ? advance_pt : (block_hi_pt - block_lo_pt);
    }
};

// 增量排版（档 2）：续排状态 / 段切片 / 段级整形缓存

/**
 * 段落之间传递的排版状态。
 * 断行是段内局部决策（只依赖本段的簇推进与内容宽度），跨段传播的只有这三个标量：
 * 页号、当前块轴起点、已排行数。行数不能由行盒下标现数：竖排块跨多条带时，旧循环的
 * 行号只按最后一条带的列数推进，前几条带不累加。竖排无需额外字段 —— 每个块在块首
 * 自行从 cursor_y 推出列状态。
 * 因此「保留前缀 + 从脏段续排」与「整篇重排」的结果逐字节相同；
 * 该等价性由 EditorEngine::auditLayout() 用一次全量重排当场校验。
 */
struct ReflowState {
    /**
     * 本段的起始页号。
     */
    int page_index = 0;

    /**
     * 本段首行的块轴起点（磅）。
     */
    float cursor_y = 0.0f;

    /**
     * 本段之前的累计行数（全局行号从此续起）。
     */
    int line_total = 0;

    /**
     * 此刻仍然生效的浮动图排除带（块轴坐标）。
     *
     * 从中间某段续排时，前面那些段落的图已经画在纸上了，但它们挖掉的版面还要继续算 ——
     * 少了这份快照，续排只看得到"本段及之后"的图，行宽会与整篇排版不同。
     */
    std::vector<FloatBand> bands;

    /**
     * 精确相等。1ulp 的差异也算不同 —— 它会改变后续落行。
     */
    bool operator==(const ReflowState& o) const {
        // 精确比较：1ulp 的差异也可能改变后续落行，必须重排后缀。
        if (page_index != o.page_index || line_total != o.line_total || cursor_y != o.cursor_y) {
            return false;
        }
        if (bands.size() != o.bands.size()) return false;
        for (size_t i = 0; i < bands.size(); ++i) {
            const FloatBand& a = bands[i];
            const FloatBand& b = o.bands[i];
            if (a.x0 != b.x0 || a.x1 != b.x1 || a.f0 != b.f0 || a.f1 != b.f1 ||
                a.square != b.square) {
                return false;
            }
        }
        return true;
    }

    /**
     * 不等 = 精确相等取反。
     */
    bool operator!=(const ReflowState& o) const { return !(*this == o); }
};

/**
 * 整篇排版的起始状态（第 0 段之前）。
 * @param page 页度量
 * @return 第 0 段开始时的续排状态
 */
inline ReflowState freshReflowState(const PageMetrics& page) {
    return ReflowState{0, page.margins.top, 0, {}};
}

/**
 * 单段在 LayoutResult 中的切片，下标与 blocks 一致。
 * 引擎据此截断前缀、搬回后缀；逐段记账之和即为整篇统计（前缀可能是复用的，
 * 只有逐段记的账拼得回来）。
 * @note 空段（无文本或无字体）同样占一项，切片为空。下标必须与 blocks 一一对应，
 * 否则「第 k 段」在两个模块里会指向不同的东西。
 */
struct ParaIndex {
    /**
     * lines 中的起始下标。
     */
    int line_begin = 0;

    /**
     * lines 中的结束下标（不含）。
     */
    int line_end = 0;

    /**
     * glyphs 中的起始下标。
     */
    int glyph_begin = 0;

    /**
     * glyphs 中的结束下标（不含）。
     */
    int glyph_end = 0;

    /**
     * 本段整形出的簇数（仅统计，不参与几何）。
     */
    int cluster_count = 0;

    /**
     * 本段跑完回退链仍为 .notdef 的字形数。
     */
    int missing_count = 0;

    /**
     * 本段切出的字体 run 数。
     */
    int font_run_count = 0;

    /**
     * 本段开始时的续排状态，增量从这里续排。
     */
    ReflowState state_at_start;
};

/**
 * 段级整形缓存（不透明句柄）。
 * 整形是排版中唯一必须访问字体的一步，也是最贵的一步，而它的输入只有
 * 「文本 + 字体链 + 字号 + 书写模式 + 语言」；命中缓存即完全跳过 HarfBuzz。
 * 内部结构定义在 layout.cpp，避免 layout.h 的使用者被拖入 shaper.h。
 */
class ShapingCache {
public:

    ShapingCache();

    /**
     * 释放实现（缓存里那份 HarfBuzz 结果）。
     */
    ~ShapingCache();
    ShapingCache(const ShapingCache&) = delete;
    ShapingCache& operator=(const ShapingCache&) = delete;

    /**
     * 清空缓存（换字体或改配置之后）。
     */
    void clear();

    /**
     * 已缓存的条目数：按段下标的条目与块局部排版按内容指纹的条目合起来。
     * @return 已缓存的条目数
     */
    size_t size() const;

    /**
     * 累计命中次数（增量跳过了多少次整形）。
     * @return 命中次数
     */
    long long hits() const;

    /**
     * 累计未命中次数（真正跑过 HarfBuzz 的次数）。
     * @return 未命中次数
     */
    long long misses() const;

    /**
     * 实现（定义在 layout.cpp，避免使用者被拖进 shaper.h）。
     */
    struct Impl;

private:

    friend struct ShapingCacheAccess;

    /**
     * 实现（定义在 layout.cpp）。
     */
    Impl* impl_ = nullptr;
};

struct LayoutStats {
    /**
     * 排出来的总页数。
     */
    int page_count = 0;

    /**
     * 字形总数。
     */
    int glyph_count = 0;

    /**
     * 落位过的行数（竖排是列数）。
     */
    int placed_line_count = 0;

    /**
     * 字素簇总数。
     */
    int cluster_count = 0;

    /**
     * 跑完整条回退链之后仍然是 .notdef 的字形数。
     * 非 0 就说明文档里有字体链覆盖不到的码点 —— 这是可证伪的"缺字"指标。
     */
    int missing_glyph_count = 0;

    /**
     * 回退切出的 run 总数（=1 表示整块单字体）。
     */
    int font_run_count = 0;

    /**
     * 整形耗时（毫秒）。
     */
    double shape_ms = 0.0;

    /**
     * 排版耗时（毫秒）。
     */
    double layout_ms = 0.0;
};

/**
 * 片段索引：页 / 块 / 字形三张反查表，也是增量后缀重新分页的块行权威。
 *
 * 三张表都从同一次排版结果推出来，放在一个对象里重建，避免"页表已换、块表还是旧值"。
 * 增量排版拼接之后只刷新后缀的块行表（见 rebuildBlockLinesFrom），后缀重新分页从这里
 * 取每块的行切片，不再自己扫行盒。
 */
struct FragmentIndex {
    /**
     * 一段连续的行：同一块、同一页。
     */
    struct Fragment {
        /**
         * 块号。
         */
        int block = 0;

        /**
         * 页号。
         */
        int page = 0;

        /**
         * 首行下标。
         */
        int first_line = 0;

        /**
         * 末行下标（含）。
         */
        int last_line = 0;
    };

    /**
     * 每页上的行下标（下标 = 页号，值 = lines 里那一行的下标，按流顺序）。
     */
    std::vector<std::vector<int>> page_lines;

    /**
     * 按行顺序的连续片段。
     */
    std::vector<Fragment> fragments;

    /**
     * 每页上的片段下标。
     */
    std::vector<std::vector<int>> page_fragments;

    /**
     * 每块的行下标（下标 = 块号，值 = lines 里那一行的下标，按行顺序）。
     */
    std::vector<std::vector<int>> block_lines;

    /**
     * 每页上的字形下标（同上，值 = glyphs 里的下标，按流顺序）。
     */
    std::vector<std::vector<int>> page_glyphs;

    /**
     * 重建三张表。
     * @param lines 行盒
     * @param glyphs 字形
     * @param para_count 块数
     * @param page_count 页数
     */
    void rebuild(const std::vector<LineBox>& lines, const std::vector<PlacedGlyph>& glyphs,
                 size_t para_count, int page_count);

    /**
     * 只重建 [first_block, para_count) 的块行表，页表保持原样。
     * 增量排版在拼接之后、后缀重新分页之前用它刷新块行切片；收尾的 rebuild 会把页表一起换掉。
     * @param lines 行盒
     * @param para_count 段数
     * @param first_block 从这里起的块（含）
     * @param first_line 从这一行起扫（含）：之前的行属于没改动的块
     */
    void rebuildBlockLinesFrom(const std::vector<LineBox>& lines, size_t para_count,
                               int first_block, int first_line);

    /**
     * 第 page 页上的行下标；越界时是空表。
     * @param page 页号
     * @returns 行下标表
     */
    const std::vector<int>& linesOnPage(int page) const;

    /**
     * 第 page 页上的片段下标；越界时是空表。
     * @param page 页号
     * @returns 片段下标表
     */
    const std::vector<int>& fragmentsOnPage(int page) const;

    /**
     * 第 para 块的行下标；越界时是空表。
     * @param para 块号
     * @returns 行下标表
     */
    const std::vector<int>& linesOfBlock(int para) const;

    /**
     * 第 page 页上的字形下标；越界时是空表。
     * @param page 页号
     * @returns 字形下标表
     */
    const std::vector<int>& glyphsOnPage(int page) const;

    /**
     * 三张反查表是否自洽：下标在范围内、页码/块号对得上、覆盖不重不漏。
     * @param lines 行盒
     * @param glyphs 字形
     * @param para_count 块数
     * @param page_count 页数
     * @returns true = 索引自洽
     */
    bool valid(const std::vector<LineBox>& lines, const std::vector<PlacedGlyph>& glyphs,
               size_t para_count, int page_count) const;
};

struct LayoutResult {
    /**
     * 页 / 块 / 字形反查表。
     */
    FragmentIndex fragments;

    /**
     * 字形，按 (块, 簇) 顺序。
     */
    std::vector<PlacedGlyph> glyphs;

    /**
     * 行盒，按 (块, 行) 顺序。
     */
    std::vector<LineBox> lines;

    /**
     * 每段在 glyphs / lines 里的切片 + 该段的续排锚点（见 ParaIndex）。
     * 增量排版靠它做"截断前缀 / 搬回后缀"；也靠它把统计从每段拼回来。
     */
    std::vector<ParaIndex> para_index;

    /**
     * 每个块一份：簇起点的段内码点下标（升序），末尾追加段长作为哨兵。
     * 编辑操作靠它把偏移吸附到"用户感知字符"的边界（退格删一个簇，
     * 而不是删一个码点——否则 e+U+0301 会被删成孤立的组合重音）。
     * 下标即块下标；空块给一个 {0}。
     */
    std::vector<std::vector<uint32_t>> para_cluster_starts;

    /**
     * 表格的绘制几何（每张表一条，嵌套时内层也各有一条）。没有表格时为空 ——
     * 空表与"这段代码不存在"逐字节相同。
     */
    std::vector<PlacedTable> placed_tables;

    /**
     * 浮动图的几何（每张图一条）。没有浮动图时为空 —— 空表与"这段代码不存在"逐字节相同。
     */
    std::vector<PlacedFloat> placed_floats;

    /**
     * 按 lines / glyphs 重建片段索引（每次排版收尾，以及"后缀整体平移"之后）。
     */
    void rebuildPageIndex();

    /**
     * 第 page 页上的行下标；页号越界时是空表。
     * @param page 页号
     * @return 行下标表
     */
    const std::vector<int>& linesOnPage(int page) const;

    /**
     * 第 para 块的行下标；块号越界时是空表。
     * @param para 块号
     * @return 行下标表
     */
    const std::vector<int>& linesOfBlock(int para) const;

    /**
     * 第 page 页上的字形下标；页号越界时是空表。
     * @param page 页号
     * @return 字形下标表
     */
    const std::vector<int>& glyphsOnPage(int page) const;

    /**
     * 片段索引自检。
     * @returns true = 页/块/字形三张反查表自洽
     */
    bool fragmentIndexValid() const;

    /**
     * 这一轮排版的账。
     */
    LayoutStats stats;
};

/**
 * 排版整篇文档：逐段整形、断行、分页，产出字形与行盒。
 *
 * 输入只有块流与前一段的页度量：行内对象（控件边界、将来的图片与域）已经在
 * BlockSpec::objects 里，排版层据此在对应位置插入"占位簇"（见 LineCluster::placeholder）。
 * 占宽度、不产生字形，于是断行、行盒长度与光标位置都把它算进去，括号把正文挤开而不是
 * 盖上去；objects 为空则不插占位（与无表单时逐字节相同）。
 *
 * 这一条契约是有意的：呈现策略（哪些字段显示括号、焦点在谁身上）不是排版的输入，
 * 它由产生块流的那一侧落成对象表。于是"要画"和"要占位"在结构上不可能不一致。
 *
 * 已知边界：只处理同一段内的边界，端点跨段的控件拿不到占位（控件矩形与命中测试已支持跨段）。
 * @param lib 字体库
 * @param blocks 块流（排版输入）
 * @param page 页度量
 * @param tables 表格结构（空 = 没有表格）
 * @param floats 浮动图（空 = 文档里没有浮动图，与加这个参数之前逐字节相同）
 * @return 字形、行盒、每段切片与这一轮的账
 */
LayoutResult layoutDocument(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                            const PageMetrics& page, const std::vector<LayoutTable>& tables = {},
                            const std::vector<LayoutFloat>& floats = {});

/**
 * 续排入口：把 [from_para, to_para) 排入 @p out，返回结束时的续排状态。
 * 三条硬契约（引擎的增量拼接建立在其上）：
 * 追加语义：out 中 [0, from_para) 段的行盒、字形与 para_cluster_starts 原样保留；
 * state 必须是第 from_para 段开始时的续排状态（见 ParaIndex::state_at_start），
 * 传错不崩溃但与全量排版不一致，这正是 auditLayout() 要抓的错误；返回值是第
 * to_para 段开始时的续排状态，可直接作为下一次续排的入参。
 * @param lib 字体库
 * @param blocks 块流
 * @param page 页度量
 * @param from_para 起始段（含）
 * @param to_para 结束段（不含）
 * @param state 第 from_para 段开始时的续排状态
 * @param cache 段级整形缓存；nullptr = 不用缓存（对照路径与单测的全量基准）
 * @param out 结果写到这里（前缀原样保留）
 * @return 第 to_para 段开始时的续排状态
 * @note 返回时 out.stats 按 para_index 逐段重算，复用前缀也不会得到半篇的数字；
 *       shape_ms 例外，只计本轮真正跑过的整形。
 */
ReflowState layoutDocumentRange(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                const PageMetrics& page, int from_para, int to_para,
                                const ReflowState& state, ShapingCache* cache, LayoutResult* out);

/**
 * 带表格结构的续排入口。落在格里的块按格矩形排（格内断行、行高按内容结算），
 * 其余与上面那条完全相同。块在不在格里由表格结构的格区间反推，块自身不声明。
 *
 * 表格只支持横排：竖排文档里的表格块仍按整页宽排（明确的边界，不做静默的错误布局）。
 * tables 为空时与不带表格的那条入口逐字节相同（所有现有调用点走的就是这条路）。
 * @param lib 字体库
 * @param blocks 块流
 * @param page 页度量
 * @param tables 表格结构（空 = 没有表格）
 * @param from_para 起始段（含）
 * @param to_para 结束段（不含）
 * @param state 起始续排状态
 * @param cache 段级整形缓存；nullptr = 不用缓存
 * @param out 结果写到这里
 * @param floats 浮动图。非空时调用方必须从第 0 段整篇排（浮动框的纵向位置取决于它前面的
 *        内容，续排拿不到那个上下文）；引擎在有浮动图时禁止走增量。
 * @return 第 to_para 段开始时的续排状态
 */
ReflowState layoutDocumentRangeWithTables(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                          const PageMetrics& page,
                                          const std::vector<LayoutTable>& tables, int from_para,
                                          int to_para, const ReflowState& state,
                                          ShapingCache* cache, LayoutResult* out,
                                          const std::vector<LayoutFloat>& floats = {});

/**
 * 按 para_index 逐段求和，写回 stats 里的字面量汇总（字形数、行数、簇数、缺字、run 数）。
 *
 * 为什么单独抽出来：增量排版有两条产生结果的路 —— "续排一段后缀"与"只重排脏段、
 * 后缀原样搬回"。两条路都必须用同一套汇总口径，否则同一个文档会因为走了哪条路而
 * 报出不同的字形数/行数（数字是真的，归属却错了）。page_count / shape_ms / layout_ms
 * 不在这里：它们要么由调用方按自己的语义填（拼接路径的页数来自后缀的旧值），
 * 要么是耗时。
 * @param out 要汇总的排版结果（就地写 stats）
 */
void summarizeLayoutStats(LayoutResult* out);

}  // namespace heditor
