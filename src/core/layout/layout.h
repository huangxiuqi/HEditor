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
#include <string>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/text/font.h"
#include "core/types.h"
#include "core/writing_mode.h"

namespace ced {

struct PageMetrics {
    float width_pt = 595.276f;  // A4 宽（磅）
    float height_pt = 841.89f;  // A4 高（磅）
    float margin_pt = 56.7f;    // 默认 2cm

    static PageMetrics A4(float margin_pt_ = 56.7f) {
        PageMetrics p;
        p.margin_pt = margin_pt_;
        return p;
    }
    float contentWidthPt() const { return width_pt - 2.0f * margin_pt; }
    float contentHeightPt() const { return height_pt - 2.0f * margin_pt; }

    /**
     * 精确相等。引擎靠它判断"页度量动了没有"来决定要不要整篇重排：
     * 断行依赖内容宽度，任何一位浮点变化都可能让某一行多收/少收一个字。
     */
    bool operator==(const PageMetrics& o) const {
        return width_pt == o.width_pt && height_pt == o.height_pt && margin_pt == o.margin_pt;
    }
    bool operator!=(const PageMetrics& o) const { return !(*this == o); }
};

struct BlockSpec {
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

    /** 文本便捷接口：进出都是 UTF-8，内部始终是码点序列。 */
    void setText(const std::string& utf8) { cp_text = utf8ToCodepoints(utf8); }
    std::string textUtf8() const { return codepointsToUtf8(cp_text); }
    size_t codepointCount() const { return cp_text.size(); }
    bool empty() const { return cp_text.empty(); }
};

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
     * 占位括号是开括号还是闭括号（placeholder == false 时恒为 None）。
     * 只被 inlineOffsetFor 用，理由见 BracketSide 的注释。
     */
    BracketSide bracket_side = BracketSide::None;

    /**
     * 这个占位是只读字段的边界（决定括号用灰还是蓝）。
     * 只有占位簇有意义。
     * 放在这里而不是让绘制层反查控件：插槽的是排版，它知道自己在给谁插 ——
     * 反查要拿 (段, cp, 朝向) 去控件集合里匹配，遇到相邻/嵌套字段就会出现歧义。
     */
    bool read_only = false;
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
     * 给定段内码点偏移，返回相对行首的沿行内轴位置（磅）。
     * 落在簇内部（比如 e + 组合重音之间）时返回该簇的起点 ——
     * 光标天然吸附到簇边界，正是本实现要的语义。
     */
    float inlineOffsetFor(uint32_t cp) const {
        if (clusters.empty()) return 0.0f;
        // 规则："最后一个 cp <= target 的簇的起点"。对普通文本它就是
        // "target 那个字的左边缘"（＝那个字的插入点），落到簇内部时吸附到簇首 ——
        // 都对。只有占位括号会破坏它，见下面那条。
        float best = clusters.front().inline_pt;
        for (const LineCluster& c : clusters) {
            if (c.cp > cp) break;
            best = c.inline_pt;
            // 闭括号占位是这个查找的终点，立刻返回。
            //
            //   不返回的话，循环会继续走到它后面那个真实簇
            //   （就是字段外紧跟着的那个字，比如 ] 后面的。），
            //   best 被覆盖成"那个字的左边缘" —— 那已经在 ] 之外了。
            //   症状：光标停在字段最后一个字，按右方向键会直接跳到 ] 右边，
            //   中间"在 ] 之内"的位置被跳过。
            //
            //   开括号占位不能这么处理：内容在它右边，光标要落在它之后，
            //   而那正是"继续往前走、取到后面那个真实簇"的结果。
            //
            // 必须再要求 c.cp == cp（只在这个边界码点上才当终点）。
            //
            //   闭括号占位记录的 cp 就是字段的结束偏移。而"最后一个 cp <= target"
            //   的查找会路过所有更早的边界：查询字段之后的位置时，也会命中
            //   前面那个字段的闭括号 —— 于是落点被拉回到前一个字段的 ] 之内。
            //   症状：多个字段时，字段 #1 起点之后的所有位置（含 #1 自己的边界）
            //   全都映射到 #0 右括号里那一个点（实测 off12=off13=off15=off16=170.255），
            //   光标"逐字右移"于是完全不动。
            //
            //   只在 c.cp == cp 时短路，语义才对：这个偏移就是这个边界，
            //   越过它之后（cp > 边界）就继续往后走，落到下一个真实簇上。
            //   段尾字段也覆盖到了：那时占位的 cp 是段长哨兵 == cp，照样命中。
            if (c.placeholder && c.bracket_side == BracketSide::Close && c.cp == cp) return best;
        }
        // 这一句必须在循环之后：字段末尾正好是段尾时，闭括号占位的 cp
        //   就是段长哨兵（== last_cp），先短路的话会返回"行末"（＝括号右边），
        //   又跑到框外去了。
        if (cp >= last_cp) return inline_end_pt;
        return best;
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
    LayoutStats stats;
};

/**
 * 排版整篇文档：逐段整形、断行、分页，产出字形与行盒。
 *
 * bracketed 里的控件会在自己的边界插入"括号占位簇"（见 LineCluster::placeholder）：
 * 占宽度、不产生字形，于是断行、行盒长度与光标位置都把它算进去，括号把正文挤开而不是
 * 盖上去；列表为空则不插占位（与无表单时逐字节相同）。同一处边界上的多个占位按槽的
 * 归属排序（见 layout.cpp）：相邻字段为 ][，空字段为 []。
 * 显示范围由引擎的 EngineConfig::form_brackets 决定，排版层不认识"聚焦"，因此"跨字段
 * 移动要不要重排"由引擎按同一张列表判断，两边不会各记一套。
 *
 * 已知边界：只处理同一段内的边界，端点跨段的控件拿不到占位（控件矩形与命中测试已支持跨段）。
 */
LayoutResult layoutDocument(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                            const PageMetrics& page, const ControlSet* controls = nullptr,
                            const std::vector<int>* bracketed = nullptr);

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
                                const PageMetrics& page, const ControlSet* controls,
                                const std::vector<int>* bracketed, int from_para, int to_para,
                                const ReflowState& state, ShapingCache* cache, LayoutResult* out);

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

}  // namespace ced
