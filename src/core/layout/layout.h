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
#include <optional>
#include <string>
#include <vector>

#include "core/doc/document.h"
#include "core/text/font.h"
#include "core/text/writing_mode.h"
#include "core/utils/types.h"

namespace heditor {

struct PageMetrics {
    float width_pt = 595.276f;  // A4 宽（磅）
    float height_pt = 841.89f;  // A4 高（磅）
    float margin_pt = 56.7f;    // 默认 2cm
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

    static PageMetrics A4(float margin_pt_ = 56.7f) {
        PageMetrics p;
        p.margin_pt = margin_pt_;
        return p;
    }
    float contentWidthPt() const { return width_pt - 2.0f * margin_pt; }
    float contentHeightPt() const { return height_pt - 2.0f * margin_pt; }
    /**
     * 连页下的条带高度：给 0 或负值时退回页高（永远不能返回 0，它是除数）。
     */
    float slabHeightPt() const { return slab_pt > 0.0f ? slab_pt : height_pt; }

    /**
     * 精确相等。引擎靠它判断"页度量动了没有"来决定要不要整篇重排：
     * 断行依赖内容宽度，任何一位浮点变化都可能让某一行多收/少收一个字。
     * 连页标志与条带高度同样要进比较 —— 它们改的是断页与页号，不改的话
     *   切模式不会重排，画面上就是"分页的排版 + 连页的落点"。
     */
    bool operator==(const PageMetrics& o) const {
        return width_pt == o.width_pt && height_pt == o.height_pt && margin_pt == o.margin_pt &&
               continuous == o.continuous && slab_pt == o.slab_pt;
    }
    bool operator!=(const PageMetrics& o) const { return !(*this == o); }
};

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
    uint32_t cp_begin = 0;
    uint32_t cp_end = 0;  // 不含
    int font_index = 0;
    float size_pt = 12.0f;
    Color color = Color::rgb(24, 24, 24);
    const char* language = nullptr;
    std::vector<int> fallback_font_indices;

    uint32_t codepointCount() const { return cp_end - cp_begin; }
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
    uint32_t cp = 0;  // 锚点：段内码点下标（可等于段长，表示段尾）
    InlineObjectKind kind = InlineObjectKind::ControlStart;
    int owner = -1;          // 不透明句柄，>-1 的才画括号
    bool read_only = false;  // 只读控件的括号画灰色

    /**
     * 盒子的显示尺寸（磅）。控件边界恒为 0，原子必须为正。
     */
    float w_pt = 0.0f;
    float h_pt = 0.0f;

    /**
     * 原子种类与内容指纹。前者决定画什么；后者决定"内容变了要不要重画" ——
     * 尺寸没变而内容变了（换了一张同样大的图）在几何上看不出来，行指纹只能靠它。
     * 两者都只在 kind == Atom 时有意义。
     */
    InlineAtom atom = InlineAtom::None;
    uint64_t content_hash = 0;
};

/**
 * 一个格位：这一块内容属于哪张表的哪一格。
 *
 * table 是不透明身份（编译层填模型节点 id，排版层不解释它），与 InlineObject::owner
 * 同一个分工 —— 引擎拿它回宿主换"这是哪张表"，自己不认识节点 id 的空间。
 */
struct CellSlot {
    uint64_t table = 0;
    int row = 0;   // 逻辑行号（在该表的行序列里的下标）
    int cell = 0;  // 逻辑格号（在该行里的下标）

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
 * 单元格内边距（四边相同）。它是引擎配置常数，不来自样式表 —— 等 TableStyle 落地再接。
 * 排版按它算格内可用宽度与行高，绘制按它画内容框；两处共用这一个数，各写一遍必然分叉。
 */
inline constexpr float kTableCellPaddingPt = 4.0f;

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
    float pt = 0.0f;     // Abs：磅
    float ratio = 0.0f;  // Percent：0..1

    static WidthSpec makeAuto() { return WidthSpec{}; }
    static WidthSpec makeAbs(float v) {
        WidthSpec w;
        w.kind = Kind::Abs;
        w.pt = v;
        return w;
    }
    static WidthSpec makePercent(float r) {
        WidthSpec w;
        w.kind = Kind::Percent;
        w.ratio = r;
        return w;
    }
    bool isAuto() const { return kind == Kind::Auto; }
};

struct BlockSpec {
    /**
     * 这一段所在的单元格路径（外层到内层）。空 = 正文。
     *
     * 表格是扁平化进块流的：格里的内容照常编成段落，"它在哪一格"只记在这条路径上。
     * 嵌套表格就是路径更长一条。句柄不透明 —— 排版层只拿它把同一格的块聚到一起，
     * 再经结构表换出这一格的几何，不认识节点。
     */
    std::vector<CellSlot> cell_path;

    /**
     * 段落文本的唯一真源是码点（UCS-4）。UTF-8 只是 I/O 表示，
     * 用 setText()/textUtf8() 在两个表示之间往返。
     * 为什么不让 std::string 当存储：编辑会改文本，而 UTF-8 存储下
     * "第 N 个字符"的定位需要从头解码（或者维护一张字节↔码点映射表）。
     * 码点数组里 offset 就是下标，插入/删除是 O(1) 定位 + O(n) 搬移。
     */
    std::vector<uint32_t> cp_text;

    int font_index = 0;
    float size_pt = 12.0f;
    WritingMode wm = WritingMode::HorizontalTB();
    float line_factor = 1.6f;  // 行高 / 列宽 = size_pt * line_factor
    float space_after_pt = 10.0f;
    bool page_break_before = false;
    const char* language = nullptr;  // 例如 "mn" / "zh"
    Color color = Color::rgb(24, 24, 24);

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
     */
    void setText(const std::string& utf8) {
        cp_text = utf8ToCodepoints(utf8);
        runs.clear();
        objects.clear();
    }
    std::string textUtf8() const { return codepointsToUtf8(cp_text); }
    size_t codepointCount() const { return cp_text.size(); }
    bool empty() const { return cp_text.empty(); }

    /**
     * 整段一个 run，属性取块级字段。空段给一个空区间（不是空序列）。
     */
    FormatRun wholeRun() const {
        FormatRun r;
        r.cp_begin = 0;
        r.cp_end = static_cast<uint32_t>(cp_text.size());
        r.font_index = font_index;
        r.size_pt = size_pt;
        r.color = color;
        r.language = language;
        r.fallback_font_indices = fallback_font_indices;
        return r;
    }

    /**
     * 实际生效的 run 序列：runs 非空时就是它，为空时合成一个覆盖全段的 run。
     * 排版层只读这一个入口，于是"整段一个 run"与"显式一个 run"走同一条代码路径 ——
     * 这正是等价判据成立的原因。
     */
    std::vector<FormatRun> effectiveRuns() const {
        if (!runs.empty()) return runs;
        return {wholeRun()};
    }
};

/**
 * 一个逻辑格（不是网格格位）：单元格的覆盖范围由跨度表达，被合并掉的格位不存在。
 *
 * first_block / block_count 是它在 blocks 里的内容区间。空格的 count 为 0 ——
 * 排版层要按"一个空段落占一行"处理，不能当成这一格没有内容。
 */
struct LayoutCell {
    int row = 0;
    int cell = 0;
    int row_span = 1;
    int col_span = 1;
    BoxVAlign v_align = BoxVAlign::Top;
    int first_block = 0;
    int block_count = 0;
};

/**
 * 一行的排版输入。三个开关都在行上（Word 的 w:trPr），行高也在。
 */
struct LayoutRow {
    bool can_split_by_page = true;
    bool is_header = false;
    bool page_break_before = false;

    /**
     * 行高的绝对值。空表示没有绝对值（规则为 Auto，或者模型给的是编译期算不出的相对长度）。
     */
    std::optional<float> height_pt;
    RowHeightRule height_rule = RowHeightRule::Auto;
};

/**
 * 一张表的排版输入。rows / cols 是派生网格的尺寸（由跨度推出来，与模型层 buildGrid
 * 同一口径），不是"行记录条数"与"列定义条数"。
 *
 * col_widths 的长度恒为 cols：显式列宽可能比网格短，短的部分补自动。
 */
struct LayoutTable {
    uint64_t handle = 0;
    WidthSpec width;
    TableHAlign align = TableHAlign::Start;
    int rows = 0;
    int cols = 0;
    std::vector<WidthSpec> col_widths;
    std::vector<LayoutRow> row_specs;  // 长度恒为 rows
    std::vector<LayoutCell> cells;     // 逻辑格，按行列顺序
};

/**
 * 一条已放置的表格行（页片段）。跨页拆开的一行在每页各有一条。
 *
 * cont_top / cont_bottom 说明这一片段的上边或下边是"接着别处"的 —— 绘制跳过那条边，
 * 于是跨页处看起来是裁开的，而不是被分出一条线。这条区分只能在产物里表达：
 * 光看矩形分不出"行的顶边"与"上一片段被我接着的那条切口"。
 */
struct PlacedRow {
    int row = 0;
    int page = 0;
    float y = 0.0f;  // 行顶（页内坐标）
    float h = 0.0f;
    bool cont_top = false;
    bool cont_bottom = false;
};

/**
 * 一张已放置的表格：位置、列边界（相对表格左边，长度 cols+1）与它的行片段。
 *
 * 它只是绘制与命中用的几何投影，不是排版输入 —— 排版输入是 LayoutTable。
 */
struct PlacedTable {
    uint64_t handle = 0;
    float x = 0.0f;  // 表格左边（页内坐标）
    float w = 0.0f;
    int rows = 0;
    int cols = 0;
    std::vector<float> col_x;
    /**
     * 逻辑格（含跨度）。绘制要靠它算每个格自己的框 —— 只画格的外框，合并格内部就不会
     * 多出几条线，而"哪些列边界被跨过"这件事也不必另存一份掩码。
     */
    std::vector<LayoutCell> cells;
    std::vector<PlacedRow> placed;
};

/**
 * runs 是否恰好铺满 [0, cp_text.size())：升序、首尾相接、无空洞、无重叠、区间非空。
 * runs 为空时恒真（等价情形，属性由块级字段给出）。
 * 返回 false 时把第一处不合法的原因写进 why；why 可以为空指针。
 */
bool runsTileText(const BlockSpec& b, std::string* why);

/**
 * 在已解析出的 run 序列里找覆盖该码点的那个；越界或序列为空返回 nullptr。
 * 线性扫描：run 的数量就是"一段里格式变化的次数"，真实文档里是个位数。
 * 入参是调用方自己持有的序列（effectiveRuns 的返回值是按值给的，不能取它的引用）。
 */
const FormatRun* runCovering(const std::vector<FormatRun>& runs, uint32_t cp);

/**
 * objects 是否合序：cp 非降、且每个 cp 都不越过段长（等于段长表示锚在段尾）。
 * 为什么必须判而不是"约定一下"：排版层按数组顺序线性插槽，顺序一坏，括号的层级
 * 就画错（][ 变 [] ），而画面看上去仍然"有括号"，不容易发现。
 * 返回 false 时把第一处不合法的原因写进 why；why 可以为空指针。
 */
bool objectsOrdered(const BlockSpec& b, std::string* why);

/**
 * 已定位的字形。x/y 是"字形原点"在页面坐标系里的位置（页面左上为原点，单位磅）。
 */
struct PlacedGlyph {
    uint32_t glyph_id = 0;
    const FontInstance* font = nullptr;
    float x_pt = 0.0f;
    float y_pt = 0.0f;
    Color color = Color::rgb(0, 0, 0);
    int page = 0;
    int line = 0;  // 横排=行号；竖排=列号
    bool vertical = false;
    /**
     * 渲染时要绕 (x_pt, y_pt) 顺时针旋转的角度。蒙文竖排 = 90，
     * 表示"横排成型 + 整段侧倒"，详见 WritingMode::isSideways()。
     */
    float rotate_deg = 0.0f;
    uint32_t cluster = 0;  // 段内码点下标（与文档偏移同一把尺子）
    int paragraph = 0;     // 所属块下标 —— 会话期定位时要靠它回到文档
};

/**
 * 表单括号的占位槽宽度 = 括号字形的 advance ＋ 这一点间隙（磅）。
 * 为什么间隙也要进排版，而不是画的时候往外挪一点：
 * 画的时候挪，就把"槽"和"字形"分成了两套账 —— 括号会盖到相邻的字上，
 * 或者槽里空一条、字却已经走了。这和"括号必须进文档流"是同一条道理：
 * 凡是占画面的宽度，就必须是排版算出来的那一个。
 * 所以这里只留一个常数，排版和绘制共用它（见 engine.cpp 的 drawControlMarks）。
 */
inline constexpr float kControlBracketSlotGapPt = 1.5f;

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
    uint32_t cp = 0;         // 簇首的段内码点下标
    float inline_pt = 0.0f;  // 相对行首的沿行内轴偏移（磅）

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
    int paragraph = 0;
    int line = 0;  // 全局行/列号，与 PlacedGlyph::line 对齐
    int page = 0;
    bool vertical = false;
    bool sideways = false;
    float size_pt = 12.0f;

    /**
     * 行盒在"块轴"上的范围（横排 = y 上下；竖排 = x 左右），页面坐标（磅）。
     * 横排：block_lo = 基线 - ascent，block_hi = 基线 + descent。
     */
    float block_lo_pt = 0.0f;
    float block_hi_pt = 0.0f;

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
    uint32_t last_cp = 0;

    /**
     * 这一行所属格在行内轴上的范围（页内坐标）。两者相等表示不在表格里。
     *
     * 命中测试靠它：格里的文字只占开头一小段，按"行内范围"判势力范围，点格子的后半段
     * 就会跳到下一格。格范围是这一格的宽度，与文字多长无关。
     */
    float cell_lo_pt = 0.0f;
    float cell_hi_pt = 0.0f;

    /**
     * 这一行所属格的块轴范围（页内坐标）。两者相等表示不在表格里。
     *
     * 选区高亮靠它：一格被整段选中时要整格反白，而行盒只盖住文字那一带 —— 显式行高
     * （AtLeast）比内容高时，上下那两截空白也得算进高亮。排版在行高结算之后回填。
     */
    float cell_top_pt = 0.0f;
    float cell_bottom_pt = 0.0f;

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
     * 同一偏移上的簇必然连续：占位括号在前、真实簇在后（见 layout.cpp 的 emitSlot）。
     */
    size_t firstClusterAt(uint32_t cp) const {
        for (size_t i = 0; i < clusters.size(); ++i)
            if (clusters[i].cp == cp) return i;
        return clusters.size();
    }

    /**
     * 本偏移上占位括号的枚数 —— 也就是这个偏移的侧别上限。
     */
    int bracketCountAt(uint32_t cp) const {
        int n = 0;
        for (size_t i = firstClusterAt(cp); i < clusters.size() && clusters[i].cp == cp; ++i)
            if (clusters[i].placeholder) ++n;
        return n;
    }

    /**
     * (码点偏移, 侧别) → 相对行首的沿行内轴位置（磅）。侧别 = 在这个偏移上跨过了几枚簇。
     *
     * 为什么偏移本身不够：同一个偏移上可能压着好几枚括号，于是"控件之前 / 进入控件 /
     * 内容末尾 / 控件之后"是好几个互不相同的插入点（flow.h 定的四个流位置）。偏移把这个
     * 分量丢掉了，只剩一个数 —— "字与开括号之间""两个相邻控件之间"这些位置于是根本表达
     * 不出来：点不进去，方向键也整段跳过去。
     *
     * 补回来的那个分量排版侧本来就记着：括号占位各占一枚簇，所以侧别就是"数格子"。
     * 取值 0 = 所有括号之前；逐枚数下去；到 bracketCountAt(cp) 就是本偏移上所有括号之后，
     * 也就是紧随其后那个真实簇的位置（本偏移没有真实簇时是行末）。
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
        const size_t k = k0 + static_cast<size_t>(s);
        return (k < clusters.size()) ? clusters[k].inline_pt : inline_end_pt;
    }
};

// 增量排版（档 2）：续排状态 / 段切片 / 段级整形缓存

/**
 * 段落之间传递的全部排版状态。
 * 断行是段内局部决策（只依赖本段的簇推进与内容宽度），跨段传播的只有这三个标量：
 * 页号、当前块轴起点、已排行数。竖排无需额外字段 —— 每个块在块首自行从
 * cursor_y 推出列状态。
 * 因此「保留前缀 + 从脏段续排」与「整篇重排」的结果逐字节相同；
 * 该等价性由 EditorEngine::auditLayout() 用一次全量重排当场校验。
 */
struct ReflowState {
    int page_index = 0;     // 本段的起始页号
    float cursor_y = 0.0f;  // 本段首行的块轴起点（磅）
    int line_total = 0;     // 本段之前的累计行数（全局行号从此续起）

    bool operator==(const ReflowState& o) const {
        // 精确比较：1ulp 的差异也可能改变后续落行，必须重排后缀。
        return page_index == o.page_index && line_total == o.line_total && cursor_y == o.cursor_y;
    }
    bool operator!=(const ReflowState& o) const { return !(*this == o); }
};

/**
 * 整篇排版的起始状态（第 0 段之前）。
 */
inline ReflowState freshReflowState(const PageMetrics& page) {
    return ReflowState{0, page.margin_pt, 0};
}

/**
 * 单段在 LayoutResult 中的切片，下标与 blocks 一致。
 * 引擎据此截断前缀、搬回后缀；逐段记账之和即为整篇统计（前缀可能是复用的，
 * 只有逐段记的账拼得回来）。
 * @note 空段（无文本或无字体）同样占一项，切片为空。下标必须与 blocks 一一对应，
 * 否则「第 k 段」在两个模块里会指向不同的东西。
 */
struct ParaIndex {
    int line_begin = 0;          // lines 中的起始下标
    int line_end = 0;            // lines 中的结束下标（不含）
    int glyph_begin = 0;         // glyphs 中的起始下标
    int glyph_end = 0;           // glyphs 中的结束下标（不含）
    int cluster_count = 0;       // 本段整形出的簇数（仅统计，不参与几何）
    int missing_count = 0;       // 本段跑完回退链仍为 .notdef 的字形数
    int font_run_count = 0;      // 本段切出的字体 run 数
    ReflowState state_at_start;  // 本段开始时的续排状态，增量从这里续排
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
    ~ShapingCache();
    ShapingCache(const ShapingCache&) = delete;
    ShapingCache& operator=(const ShapingCache&) = delete;

    void clear();

    /**
     * 已缓存的段落数。
     */
    size_t size() const;
    /**
     * 累计命中次数（增量跳过了多少次整形）。
     */
    long long hits() const;
    /**
     * 累计未命中次数（真正跑过 HarfBuzz 的次数）。
     */
    long long misses() const;

    struct Impl;  // 定义在 layout.cpp

private:
    friend struct ShapingCacheAccess;
    Impl* impl_ = nullptr;
};

struct LayoutStats {
    int page_count = 0;
    int glyph_count = 0;
    int placed_line_count = 0;  // 横排=行数，竖排=列数
    int cluster_count = 0;
    /**
     * 跑完整条回退链之后仍然是 .notdef 的字形数。
     * 非 0 就说明文档里有字体链覆盖不到的码点 —— 这是可证伪的"缺字"指标。
     */
    int missing_glyph_count = 0;
    int font_run_count = 0;  // 回退切出的 run 总数（=1 表示整块单字体）
    double shape_ms = 0.0;
    double layout_ms = 0.0;
};

struct LayoutResult {
    std::vector<PlacedGlyph> glyphs;
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
 */
LayoutResult layoutDocument(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                            const PageMetrics& page,
                            const std::vector<LayoutTable>& tables = {});

/**
 * 续排入口：把 [from_para, to_para) 排入 @p out，返回结束时的续排状态。
 * 三条硬契约（引擎的增量拼接建立在其上）：
 * 追加语义：out 中 [0, from_para) 段的行盒、字形与 para_cluster_starts 原样保留；
 * state 必须是第 from_para 段开始时的续排状态（见 ParaIndex::state_at_start），
 * 传错不崩溃但与全量排版不一致，这正是 auditLayout() 要抓的错误；返回值是第
 * to_para 段开始时的续排状态，可直接作为下一次续排的入参。
 *
 * @param cache 段级整形缓存；传 nullptr 表示不使用缓存（每次都真整形），用于对照路径与
 *              单测中的全量基准。
 * @note 返回时 out.stats 的汇总量按 para_index 逐段重算，复用前缀也不会得到半篇的数字；
 *       shape_ms 例外，只计本轮真正跑过的整形。
 */
ReflowState layoutDocumentRange(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                const PageMetrics& page, int from_para, int to_para,
                                const ReflowState& state, ShapingCache* cache, LayoutResult* out);

/**
 * 带表格结构的续排入口。带 cell_path 的块按格矩形排（格内断行、行高按内容结算），
 * 其余与上面那条完全相同。
 *
 * 表格只支持横排：竖排文档里的表格块仍按整页宽排（明确的边界，不做静默的错误布局）。
 * tables 为空时与不带表格的那条入口逐字节相同（所有现有调用点走的就是这条路）。
 */
ReflowState layoutDocumentRangeWithTables(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                          const PageMetrics& page,
                                          const std::vector<LayoutTable>& tables, int from_para,
                                          int to_para, const ReflowState& state,
                                          ShapingCache* cache, LayoutResult* out);

/**
 * 按 para_index 逐段求和，写回 stats 里的字面量汇总（字形数、行数、簇数、缺字、run 数）。
 *
 * 为什么单独抽出来：增量排版有两条产生结果的路 —— "续排一段后缀"与"只重排脏段、
 * 后缀原样搬回"。两条路都必须用同一套汇总口径，否则同一个文档会因为走了哪条路而
 * 报出不同的字形数/行数（数字是真的，归属却错了）。page_count / shape_ms / layout_ms
 * 不在这里：它们要么由调用方按自己的语义填（拼接路径的页数来自后缀的旧值），
 * 要么是耗时。
 */
void summarizeLayoutStats(LayoutResult* out);

}  // namespace heditor
