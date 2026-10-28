/**
 * 排版层内部共用的整形与断行中间产物。
 *
 * 只给排版层的 layout.cpp、block_shape.cpp、block_layout.* 与直接验证块局部排版的测试
 * 使用，不是对外契约：字段与函数随排版实现变化。
 * 这里放的是块局部的那几步 —— 整形结果、括号占位、行度量、贪心断行、横排落行；
 * 页、表格与浮动图不进来，它们由 layout.cpp 的整篇循环负责。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/layout/layout.h"
#include "core/text/shaper.h"

namespace heditor {

/**
 * 一段文本整形后的中间产物（一个块一份）。
 *
 * 单独拎出来是因为整形与落行是两件事：整形只跟文本与书写模式有关，落行还跟可用宽度
 * 与前面已排了多少行有关。
 */
struct BlockShaping {
    /**
     * 整形出的字形。
     */
    ShapedText st;

    /**
     * 与 st.glyphs 一一对应的字体实例（可能是回退链里的一环）。
     */
    std::vector<const FontInstance*> glyph_font;

    /**
     * 按 cluster 分组后的簇。
     */
    struct Cluster {
        /**
         * 簇首的段内码点下标（与文档偏移同一把尺子）。
         */
        uint32_t cp = 0;

        /**
         * 本簇第一个字形在 st.glyphs 里的下标（含）。
         */
        size_t g0 = 0;

        /**
         * 本簇最后一个字形之后的下标（不含）；g0 等于 g1 表示占位簇。
         */
        size_t g1 = 0;

        /**
         * 占位簇是开括号还是闭括号；非占位簇恒为 None。
         */
        BracketSide bracket_side = BracketSide::None;

        /**
         * 表单括号占位：没有字形，只占 advance。
         */
        bool placeholder = false;

        /**
         * 空控件的提示文字：占宽度但不占流位置，侧别数格子时跳过。
         */
        bool control_hint = false;

        /**
         * 只读字段的边界（决定括号画成灰色）。只有占位簇有意义。
         */
        bool read_only = false;

        /**
         * 画这枚括号用的字体（只有括号占位有意义）。槽宽也按它算。
         */
        const FontInstance* bracket_font = nullptr;

        /**
         * 这个占位属于哪个控件（控件集合下标）；非占位恒为 -1。
         */
        int owner_control = -1;

        /**
         * 行内原子：这是哪一种。不是原子时为 None。
         */
        InlineAtom atom = InlineAtom::None;

        /**
         * 原子的盒高（磅）；不是原子时为 0。
         */
        float atom_h_pt = 0.0f;

        /**
         * 原子的内容指纹（见 InlineObject::content_hash）。
         */
        uint64_t content_hash = 0;
    };

    /**
     * 簇表。
     */
    std::vector<Cluster> clusters;

    /**
     * 每个簇沿行内轴的推进（磅）。
     */
    std::vector<float> cluster_adv_pt;

    /**
     * 每个簇之后是否允许断行。
     */
    std::vector<bool> break_after;

    /**
     * 块主字体。
     */
    FontInstance* primary = nullptr;

    /**
     * 主字体基线以上（磅）。
     */
    float ascent_pt = 0.0f;

    /**
     * 主字体基线以下（磅）。
     */
    float descent_pt = 0.0f;

    /**
     * 主字体行间隙（磅）：单倍行高 = ascent + descent + gap。
     */
    float gap_pt = 0.0f;

    /**
     * 簇起点表（升序，末尾为段长哨兵）。纯整形产物，不含括号占位。
     */
    std::vector<uint32_t> cluster_starts;

    /**
     * 字体 run 数，随整形缓存，保证增量路径上的统计与原路径一致。
     */
    int run_count = 0;

    /**
     * 缺字数（跑完回退链仍为 .notdef）。
     */
    int missing_count = 0;
};

/**
 * 一个簇的行度量：字号与基线上下界（磅）。
 *
 * 行盒高度不能整段一套：一行里可以混排字号，行盒与基线必须由本行最高的那个决定。
 * 取的是簇所在 run 自己的字体与字号；只有一个 run 时这就是块级那套值。
 */
struct LineMetric {
    /**
     * 字号（磅）。
     */
    float size_pt = 0.0f;

    /**
     * 基线以上（磅）。
     */
    float ascent_pt = 0.0f;

    /**
     * 基线以下（磅）。
     */
    float descent_pt = 0.0f;

    /**
     * 行内原子的盒子高度（磅）；文本簇为 0。
     *
     * 不能混进 size_pt：行高是字号乘行距，而盒高不是字号推出来的。行高取两者的较大者，
     * 两条来源分开算。
     */
    float box_h_pt = 0.0f;

    /**
     * 字体自己的行间隙（磅）。只进单倍行高，不进基线。
     */
    float gap_pt = 0.0f;
};

/**
 * 断出来的一行（竖排下是一列）：簇区间与这一行自己的度量。
 *
 * 它刻意不含页号与纵向位置：那两样是分页的产物，断行只看装得下多少。
 */
struct BrokenLine {
    /**
     * 起始簇下标（含）。
     */
    size_t begin = 0;

    /**
     * 结束簇下标（不含）。
     */
    size_t end = 0;

    /**
     * 本行行高（竖排下是列宽，磅）。
     */
    float line_h = 0.0f;

    /**
     * 墨迹上方那一段空白（磅，见 lineLeadTopFor）。
     */
    float lead = 0.0f;

    /**
     * 绘制这一行时是否要裁到行盒。
     */
    bool clip = false;

    /**
     * 本行最大的基线以上高度（磅）。
     */
    float ascent = 0.0f;

    /**
     * 本行最大的基线以下深度（磅）。
     */
    float descent = 0.0f;

    /**
     * 本行最大字号（磅）。
     */
    float size_pt = 0.0f;
};

/**
 * 一行里的一段可用横向区间（页内坐标，磅）。
 *
 * 普通行只有一段；四周型浮动图把一行挖开时会有两段。区间只描述"字能落在哪"，
 * 行高与基线仍由这一行断出来的内容决定。
 */
struct LineSpan {
    /**
     * 区间左沿（页内 x，磅）。
     */
    float x = 0.0f;

    /**
     * 区间宽度（磅）。
     */
    float w = 0.0f;
};

/**
 * 一行里的一个片段：一段文字与它在行内的起点。
 *
 * 普通行只有一个片段；四周型浮动图把一行挖开时会有两个。片段共享行顶与基线，
 * 行高取各段最大值 —— 同一视觉行只有一条基线。
 */
struct LinePart {
    /**
     * 这一段断出来的行。
     */
    BrokenLine line;

    /**
     * 这一段的行内起点（页内坐标，磅）。
     */
    float x = 0.0f;
};

/**
 * 按本行的可用区间从左到右断行：每段接上一段的末尾继续断。
 *
 * 前一段放不下更多字时，剩下的字进下一段 —— 于是一行可以左边一段、右边一段
 * （四周型图在中间）。宽度不大于 1 磅的段直接跳过：连一个簇都塞不进时，文字不该在那里
 * 排成一列。整行一段都放不下时返回空片段表。
 * @param bs 整形结果
 * @param cluster_metric 每个簇的行度量
 * @param start 起始簇下标
 * @param spans 本行的可用区间（页内坐标，磅）
 * @param block_size_pt 块字号，取不到字体度量时的替代值
 * @param spacing 行距
 * @param parts 出参：本行的片段（先清空）
 * @param line_h_pt 出参：本行行高（各段最大值，磅）
 */
void fillLineParts(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric,
                   size_t start, const std::vector<LineSpan>& spans, float block_size_pt,
                   const LineSpacing& spacing, std::vector<LinePart>* parts, float* line_h_pt);

/**
 * 行所属格的行内范围（页内坐标）。
 *
 * 四个值都为 0 表示不在表格里，与 LineBox 的四个 cell 字段同一个约定。
 */
struct LineCellRange {
    /**
     * 格内容区在行内轴上的左沿（磅）。
     */
    float cell_lo_pt = 0.0f;

    /**
     * 格内容区在行内轴上的右沿（磅）。
     */
    float cell_hi_pt = 0.0f;

    /**
     * 格外框在行内轴上的左沿（磅）。
     */
    float box_lo_pt = 0.0f;

    /**
     * 格外框在行内轴上的右沿（磅）。
     */
    float box_hi_pt = 0.0f;
};

/**
 * 一次块整形的账：簇数、缺字数、字体 run 数与真正花掉的整形时间。
 */
struct BlockShapeStats {
    /**
     * 整形出的簇数。
     */
    int cluster_count = 0;

    /**
     * 跑完回退链仍为 .notdef 的字形数。
     */
    int missing_count = 0;

    /**
     * 回退切出的字体 run 数。
     */
    int font_run_count = 0;

    /**
     * 真正跑过 HarfBuzz 的毫秒数；命中缓存为 0。
     */
    double shape_ms = 0.0;
};

/**
 * 一行的行高（竖排下的列宽）由行距规则定。
 *
 * @param sp 行距（规则与值）
 * @param natural 单倍行高（字体度量）
 * @param content_h 行内最高内容的高度（墨迹，含原子盒）
 * @return 这一行沿块轴推进多少（磅）
 */
float lineHeightFor(const LineSpacing& sp, float natural, float content_h);

/**
 * 一行里行槽比墨迹高的那一截，有多少落在墨迹上方。
 *
 * @param sp 行距（规则与值）
 * @param line_h 行高（lineHeightFor 的结果）
 * @param ink_h 墨迹高度（ascent 加 descent，含原子盒）
 * @return 墨迹上方那一段（磅，可为负）
 */
float lineLeadTopFor(const LineSpacing& sp, float line_h, float ink_h);

/**
 * 这一行是否需要按行盒裁字：只有固定值行距且行高比墨迹矮时成立。
 *
 * @param sp 行距
 * @param ascent 基线以上（磅）
 * @param descent 基线以下（磅）
 * @return true = 绘制这一行时要裁到行盒
 */
bool clipTextFor(const LineSpacing& sp, float ascent, float descent);

/**
 * 从 start 起断出一行：可用容量为 avail，按最后一个可断点回退。
 *
 * @param bs 整形结果
 * @param cluster_metric 每个簇的行度量
 * @param start 起始簇下标
 * @param avail 可用容量（磅）
 * @param block_size_pt 块字号，取不到字体度量时的替代值
 * @param spacing 行距
 * @return 断出来的行；至少包含一个簇
 */
BrokenLine breakLine(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric,
                     size_t start, float avail, float block_size_pt, const LineSpacing& spacing);

/**
 * 整块的断行：横排的容量恒定，一次切完。
 *
 * 竖排不能这样切：一列的容量是这一条带还能放多高，换带由已经排了几列决定，
 * 只能一列一列地断。
 *
 * @param bs 整形结果
 * @param cluster_metric 每个簇的行度量
 * @param width 可用宽度（磅）
 * @param block_size_pt 块字号，取不到字体度量时的替代值
 * @param spacing 行距
 * @param out 结果写到这里（先清空）
 */
void breakBlock(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric, float width,
                float block_size_pt, const LineSpacing& spacing, std::vector<BrokenLine>* out);

/**
 * 把块流的行内对象插成占位簇（括号与行内原子进文档流的全部实现）。
 *
 * 占位簇不产生字形、只占 advance，于是断行容量、行末位置、光标与命中测试都自动把括号
 * 算进去。哪些对象在这张表里由产生端决定，排版层不认识聚焦状态。
 *
 * @param b 块
 * @param n_cp 段长（码点）
 * @param shaped 纯整形结果；可能指向只读的缓存条目
 * @param scratch 需要可改副本时整形到这里
 * @param fruns 格式 run 表
 * @param lib 字体库
 * @param max_hint_pt 这个容器能给提示文字的宽度上限
 * @return 插好占位的整形结果；没有对象可插时原样返回 shaped
 */
const BlockShaping* withInlineObjects(const BlockSpec& b, uint32_t n_cp, const BlockShaping* shaped,
                                      BlockShaping* scratch, const std::vector<FormatRun>& fruns,
                                      FontLibrary& lib, float max_hint_pt);

/**
 * 取一个码点所在 run 的颜色。
 *
 * 颜色刻意不进整形指纹（它不改字形与推进），只能在摆字形时按 run 现取。
 *
 * @param fruns 格式 run 表
 * @param b 块（没有覆盖它的 run 时用块级颜色）
 * @param cp 段内码点下标
 * @return 字色
 */
Color runColorAt(const std::vector<FormatRun>& fruns, const BlockSpec& b, uint32_t cp);

/**
 * 把 run 的斜体、下划线、字号与字重写到已放置的字形上。
 *
 * @param fruns 格式 run 表
 * @param b 块（没有覆盖它的 run 时用块级字号）
 * @param cp 段内码点下标
 * @param pg 出参：要写标记的字形
 */
void stampRunMarks(const std::vector<FormatRun>& fruns, const BlockSpec& b, uint32_t cp,
                   PlacedGlyph* pg);

/**
 * 算出每个簇的行度量与整段的最大值。
 *
 * 相邻簇大多落在同一个 run 里，所以只在换 run 时查字体。
 *
 * @param lib 字体库
 * @param b 块（取不到字体时用它的字号）
 * @param fruns 格式 run 表
 * @param bs 整形结果
 * @param cluster_metric 出参：与 bs.clusters 等长
 * @param para_metric 出参：整段最大值（竖排列宽用它）
 */
void computeClusterMetrics(FontLibrary& lib, const BlockSpec& b,
                           const std::vector<FormatRun>& fruns, const BlockShaping& bs,
                           std::vector<LineMetric>* cluster_metric, LineMetric* para_metric);

/**
 * 取出一段的纯整形结果（优先命中整形缓存）。
 *
 * 命中时返回指向缓存条目的只读引用，不做深拷贝；只有要插括号占位的那一段才复制一份
 * 可改副本。实现在 layout.cpp，因为整形缓存的内部结构在那里。
 *
 * @param lib 字体库
 * @param b 块
 * @param fruns 实际生效的字符格式 run
 * @param cache 整形缓存；nullptr 表示不用缓存
 * @param cache_slot 按段下标缓存时的槽位；cache_by_content 为 true 时忽略
 * @param cache_by_content true = 按整形指纹缓存（块局部排版没有全局段下标）
 * @param scratch 未命中时整形到这里；返回的指针可能指向它
 * @param stats 出参：这一块的簇数、缺字数、run 数与整形耗时
 * @return 整形结果；空文本、取不到字体或没有簇时为 nullptr
 */
const BlockShaping* shapeBlockShaping(FontLibrary& lib, const BlockSpec& b,
                                      const std::vector<FormatRun>& fruns, ShapingCache* cache,
                                      size_t cache_slot, bool cache_by_content,
                                      BlockShaping* scratch, BlockShapeStats* stats);

/**
 * 落一行横排：写一个行盒与它的字形。
 *
 * 只认坐标，不决定分页：top 是行槽顶（块轴坐标），page 与 page_y_offset 把块轴 y 折算成
 * 页内 y（连页下相差页号乘条带高，分页下位移为 0）。行内位置由簇推进决定，不由字形连加
 * 推进 —— 占位簇没有字形，字形连加会在它身上原地不动。
 *
 * @param b 块
 * @param bs 整形结果
 * @param fruns 格式 run 表
 * @param parts 本行的片段（从左到右）
 * @param top 行槽顶（块轴坐标，磅）
 * @param page 这一行落在第几页
 * @param page_y_offset 块轴 y 到页内 y 的位移（磅）
 * @param para_index 行盒与字形里记的块下标
 * @param line_no 行盒与字形里记的行号
 * @param cell 行所属格的行内范围；不在表格里时四个值都为 0
 * @param lines 出参：追加行盒
 * @param glyphs 出参：追加字形
 */
void placeHorizontalLine(const BlockSpec& b, const BlockShaping& bs,
                         const std::vector<FormatRun>& fruns, const std::vector<LinePart>& parts,
                         float top, int page, float page_y_offset, int para_index, int line_no,
                         const LineCellRange& cell, std::vector<LineBox>* lines,
                         std::vector<PlacedGlyph>* glyphs);

/**
 * 落一列竖排：写一个列盒与它的字形。
 *
 * 与 placeHorizontalLine 同一分工：只认坐标，不决定列位。侧倒（蒙文）的笔位与旋转在
 * 这里定：局部坐标映射下屏幕偏移等于 (y_offset, x_offset)，基线取列中线减去半个字身。
 *
 * @param b 块
 * @param bs 整形结果
 * @param fruns 格式 run 表
 * @param col_line 这一列的断行片段（竖排下 line_h 是列宽）
 * @param col_center 列心（页内 x，磅）
 * @param col_w 列宽（磅）
 * @param pen_y 列顶（分页下是页内 y，连页下是块轴 y，磅）
 * @param page 分页下的页号；continuous 为 true 时忽略
 * @param continuous 连页：页号由列顶与字形 y 现算
 * @param slab_pt 连页条带高（磅）；continuous 为 true 时必须为正
 * @param para_index 列盒与字形里记的块下标
 * @param line_no 列盒与字形里记的行号
 * @param cell 列所属格的行内范围；不在表格里时四个值都为 0
 * @param lines 出参：追加列盒
 * @param glyphs 出参：追加字形
 */
void placeVerticalColumn(const BlockSpec& b, const BlockShaping& bs,
                         const std::vector<FormatRun>& fruns, const BrokenLine& col_line,
                         float col_center, float col_w, float pen_y, int page, bool continuous,
                         float slab_pt, int para_index, int line_no, const LineCellRange& cell,
                         std::vector<LineBox>* lines, std::vector<PlacedGlyph>* glyphs);

}  // namespace heditor
