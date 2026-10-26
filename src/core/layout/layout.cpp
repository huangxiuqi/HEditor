#include "core/layout/layout.h"

#include <hb.h>

#include <algorithm>
#include <chrono>

#include "core/text/fallback.h"
#include "core/text/shaper.h"
#include "core/utils/hash.h"
#include "core/utils/utf8.h"

namespace heditor {

/**
 * 提示文字在某个字体下的排版宽度（磅）。
 *
 * 逐字取名义字形的推进再求和，不整形整串：提示文字是"这里该填什么"的一句话，不是要
 * 断行与字距优化的正文，而绘制侧（engine_paint.cpp 的 rasterControlMarks）按同一个走法
 * 取字形与推进 —— 两边必须同一套算法，否则槽宽与画出来的字对不上：不是画出槽外，
 * 就是槽里空一段。缺字按 0 宽跳过，两侧同样跳过。
 * @param f 这一侧正文 run 的字体
 * @param utf8 提示文字
 * @return 宽度（磅）；字体为空时 0
 */
float hintAdvancePt(const FontInstance* f, const std::string& utf8) {
    if (f == nullptr || utf8.empty()) return 0.0f;
    hb_font_t* hbf = f->hb();
    if (hbf == nullptr) return 0.0f;
    float sum = 0.0f;
    for (uint32_t cp : utf8ToCodepoints(utf8)) {
        hb_codepoint_t g = 0;
        if (!hb_font_get_nominal_glyph(hbf, cp, &g) || g == 0) continue;
        const float adv = static_cast<float>(hb_font_get_glyph_h_advance(hbf, g)) / kFontScalePerPt;
        if (adv > 0.0f) sum += adv;
    }
    return sum;
}

namespace {

using Clock = std::chrono::steady_clock;

/**
 * 方括号在某个字体下的 advance（磅）。返回 0 表示该字体没有方括号字形 ——
 * 那就不留位（宁可没有空隙，也不要留一个宽度却画不出东西）。
 * 字体由调用方按控件边界所在的那一侧正文 run 取（见 bracketFontFor）；块主字体只是
 * 取不到 run 字体时的退路。
 */
float bracketAdvancePt(FontInstance* f) {
    if (!f) return 0.0f;
    hb_font_t* hbf = f->hb();
    if (!hbf) return 0.0f;
    hb_codepoint_t g = 0;
    if (!hb_font_get_nominal_glyph(hbf, static_cast<hb_codepoint_t>('['), &g) || !g) return 0.0f;
    const float pt = static_cast<float>(hb_font_get_glyph_h_advance(hbf, g)) / kFontScalePerPt;
    return pt > 0.0f ? pt : 0.0f;
}

double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/**
 * 该码点之后是否允许断行。
 * 这里体现了"断行也是脚本相关的"：
 * 空格：任何脚本都可断；
 * CJK / 全角标点 / 谚文：逐字可断（中文没有词边界，按字断是常规做法）；
 * 蒙文（U+1800–U+18AF）：上下文连写文字，词内绝不能断，否则会破坏
 * 词首/词中/词末形态与连字，只在空格处断。
 */
bool canBreakAfter(uint32_t cp) {
    if (cp == 0x20 || cp == 0x09) return true;
    if (cp >= 0x2E80 && cp <= 0x9FFF) return true;   // CJK 部首扩展 ~ 统一表意文字
    if (cp >= 0x3000 && cp <= 0x303F) return true;   // CJK 标点
    if (cp >= 0xAC00 && cp <= 0xD7AF) return true;   // 谚文音节
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;   // CJK 兼容表意文字
    if (cp >= 0xFF00 && cp <= 0xFFEF) return true;   // 全角/半角形式
    if (cp >= 0x1800 && cp <= 0x18AF) return false;  // 蒙文：词内不断
    return false;
}

/**
 * 一行/一列的切分结果
 */
struct LineSlice {
    size_t begin = 0;  // cluster 下标（含）
    size_t end = 0;    // cluster 下标（不含）
    float advance_pt = 0.0f;
};

/**
 * 一个簇的行度量：字号与基线上下界（磅）。
 *
 * 为什么行盒的高度不能整段一套：一行里可以混排字号（字符格式 run），行盒与基线必须由本行
 * 最高的那个决定，否则大字会溢出到相邻行上。取的是"该簇所在 run 自己的字体与字号"——
 * 只有一个 run 时这就是块级那套值，与加 run 之前逐字段相同。
 * 回退字体不参与行度量：与加 run 之前同一条取舍（那时也只看主字体）。
 */
struct LineMetric {
    float size_pt = 0.0f;
    float ascent_pt = 0.0f;
    float descent_pt = 0.0f;
    /**
     * 行内原子的盒子高度（磅）；文本簇为 0。
     *
     * 它不能混进 size_pt：行高是"字号 × 行距"，而盒高不是字号推出来的 ——
     * 一张 200 磅高的图不该被行距撑到 320 磅，也不该按 200 磅的字号去算基线。
     * 所以行高取"字号 × 行距"与盒高的大者，两条来源分开算再取大。
     */
    float box_h_pt = 0.0f;
};

/**
 * 贪心断行：在 [start, clusters.size()) 里切出下一行，可用范围为 avail_pt。
 * 算法：边累积边推进，同时记住"最后一个可断点"。装不下时回退到该可断点；
 * 若整行内一个可断点都没有（超长的不可断词），则在该处硬断，保证不会死循环。
 */
LineSlice takeLine(const std::vector<float>& cluster_adv_pt, const std::vector<bool>& break_after,
                   size_t start, float avail_pt) {
    LineSlice s;
    s.begin = start;

    float used = 0.0f;
    size_t j = start;
    size_t last_good = start;

    while (j < cluster_adv_pt.size()) {
        const float adv = cluster_adv_pt[j];
        // 至少放一个簇，避免超宽单簇导致空行
        if (used + adv > avail_pt && j > start) break;
        used += adv;
        ++j;
        if (break_after[j - 1]) last_good = j;
    }

    size_t end;
    if (j >= cluster_adv_pt.size()) {
        end = cluster_adv_pt.size();
    } else {
        end = (last_good > start) ? last_good : j;
    }

    s.end = end;
    float sum = 0.0f;
    for (size_t i = s.begin; i < s.end; ++i) sum += cluster_adv_pt[i];
    s.advance_pt = sum;
    return s;
}

/**
 * 一段文本整形后的中间产物（一个块一份）。
 * 单独拎出来是因为"整形"和"落行"是两件事：整形只跟文本+书写模式有关，
 * 而落行还跟页宽/页高/前面已排了多少行有关。分开之后，
 * 将来做增量时就能只重算后半段。
 */
struct BlockShaping {
    ShapedText st;
    std::vector<const FontInstance*> glyph_font;  // 与 st.glyphs 一一对应
    /**
     * 按 cluster 分组
     */
    struct Cluster {
        uint32_t cp;  // 簇首的段内码点下标（= 文档偏移同一把尺子）
        size_t g0, g1;
        /**
         * 占位簇是开括号还是闭括号（非占位簇恒为 None）。见 layout.h 的 BracketSide。
         */
        BracketSide bracket_side = BracketSide::None;
        /**
         * "表单括号占位"：g0 == g1（没有字形），只占 advance。
         * 放置循环 for (g = g0; g < g1; ++g) 天然跳过它 ——
         * 所以插入占位不需要给放置代码加任何分支。
         */
        bool placeholder = false;
        /**
         * "空控件的提示文字"：g0 == g1，占宽度但不占流位置（侧别数格子时跳过它）。
         * 见 layout.h 的 LineCluster::control_hint。
         */
        bool control_hint = false;
        /**
         * 只读字段的边界（决定括号画成灰色）。只有占位簇有意义。
         */
        bool read_only = false;
        /**
         * 画这枚括号用的字体（只有括号占位有意义）。槽宽也按它算 —— 括号跟着内容
         * run 的字体走，换字体时字形与推进一起变。
         */
        const FontInstance* bracket_font = nullptr;
        /**
         * 这个占位属于哪个控件（控件集合下标）。只有占位簇有意义 ——
         * 命中测试靠它回答"点到的这个括号是谁的"（见 layout.h 的 LineCluster）。
         */
        int owner_control = -1;
        /**
         * 行内原子：这是哪一种（不是原子时为 None），以及它的盒高（磅）。
         * 盒宽不用另存 —— 它就是 cluster_adv_pt，与括号槽共用同一个累加器。
         * 盒高进行度量：行高与基线由它定，见下面的 cluster_metric。
         */
        InlineAtom atom = InlineAtom::None;
        float atom_h_pt = 0.0f;
        /**
         * 原子的内容指纹（见 InlineObject::content_hash）。原样搬到 LineCluster，
         * 再进行指纹 —— 几何看不见的那类变化（换了一张同样大的图）靠它。
         */
        uint64_t content_hash = 0;
    };
    std::vector<Cluster> clusters;
    std::vector<float> cluster_adv_pt;  // 沿行内轴的推进（磅）
    std::vector<bool> break_after;
    FontInstance* primary = nullptr;
    float ascent_pt = 0.0f;
    float descent_pt = 0.0f;

    /**
     * 簇起点表（升序，末尾为段长哨兵）。属于纯整形产物（不含括号占位），可随整形一起缓存。
     */
    std::vector<uint32_t> cluster_starts;
    /**
     * 字体 run 数，随整形缓存，保证增量路径上的统计与原路径一致。
     */
    int run_count = 0;
    /**
     * 缺字数（跑完回退链仍为 .notdef），同上。
     */
    int missing_count = 0;
};

/**
 * 计算段级整形指纹：涵盖所有影响整形结果的输入。
 * 字符格式从块级字段挪进了 run（1.8 之后），所以这里混的是实际生效的 run 序列 ——
 * 只改一个 run 的字号而文本不变，也必须判成未命中，否则会拿旧字号的字形接着用。
 *
 * 字重同理，而且更容易漏：它换的是字体实例（可变字体的 wght 轴），字形对象与推进宽度
 * 都跟着变。一段文字整段加粗时文本没动、run 边界也没动，指纹若不含字重就会命中上一轮
 * 的整形结果 —— 画面停在旧字重上，再把字号改一下才跟着变。斜体与下划线不进指纹：
 * 它们不改字形与推进，摆字形时按 run 现取（见 stampRunMarks）。
 * @note 刻意不含页宽页高、光标、括号占位计划、颜色、行高与段后距 —— 它们只影响落行
 * 或绘制。颜色不进指纹是因为它不改变字形与推进（放置时才按 run 取色），
 * 所以改颜色不该让整形缓存失效。
 * 浮点按位混入（不量化）：量化会把 12.0 与 12.001 判成同一份缓存。
 */
uint64_t shapingFingerprint(const BlockSpec& b, const std::vector<FormatRun>& runs) {
    uint64_t h = kFnvBasis;
    h = fnvMixU32s(h, b.cp_text);
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.inline_axis)));
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.block_flow)));
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.orientation)));
    h = fnvMix(h, static_cast<uint64_t>(runs.size()));
    for (const FormatRun& fr : runs) {
        h = fnvMix(h, static_cast<uint64_t>(fr.cp_begin));
        h = fnvMix(h, static_cast<uint64_t>(fr.cp_end));
        h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(fr.font_index)));
        h = fnvMixFloat(h, fr.size_pt);
        h = fnvMixFloat(h, fr.weight);
        h = fnvMix(h, static_cast<uint64_t>(fr.fallback_font_indices.size()));
        for (int fi : fr.fallback_font_indices)
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(fi)));
        h = fnvMixCStr(h, fr.language);
    }
    return h;
}

/**
 * 控件边界那一侧的括号该用哪个字体。
 *
 * 开口取边界所在的那个字（内容首字），闭口取前一个字（内容末字）：括号属于内容，
 * 换字体时它跟着变。字体没有方括号字形时退回块主字体 —— 槽宽按 0 算的话括号不会画，
 * 而边框不该因为选的字体缺一个 ASCII 括号就消失。
 *
 * @param o 行内对象（控件边界）
 * @param fruns 格式 run 表
 * @param lib 字体库
 * @param n_cp 段长（码点）
 * @param primary 块主字体（退路）
 * @return 画这枚括号用的字体
 */
FontInstance* bracketFontFor(const InlineObject& o, const std::vector<FormatRun>& fruns,
                             FontLibrary& lib, uint32_t n_cp, FontInstance* primary) {
    if (fruns.empty() || n_cp == 0) return primary;
    uint32_t probe = (o.kind == InlineObjectKind::ControlEnd && o.cp > 0)
                         ? static_cast<uint32_t>(o.cp - 1)
                         : static_cast<uint32_t>(o.cp);
    if (probe >= n_cp) probe = n_cp - 1;
    const FormatRun* r = runCovering(fruns, probe);
    if (r == nullptr) return primary;
    FontInstance* f = lib.instance(r->font_index, r->size_pt, r->weight);
    if (f == nullptr || !(bracketAdvancePt(f) > 0.0f)) return primary;
    return f;
}

/**
 * 把块流的行内对象插成占位簇（括号与行内原子进文档流的全部实现）。
 * 对象从哪来、该不该显示、谁在外谁在内，全在产生端（见 BlockSpec::objects）；排版层只做
 * 一次线性归并（表已升序，同一处边界上的先后就是数组里的先后，不做第二次排序）。
 *
 * 宽度来源三种：控件边界 = 括号字形 advance + 间隙，原子 = 产生端给的盒宽，空控件的
 * 提示文字 = 那句话的逐字推进。占位簇 g0 == g1，不产生字形只占 advance，于是断行容量、
 * 行末位置、簇的行内位置、光标与命中测试全都自动算进去。原子之后可以断（它与一个全角字
 * 同价），括号与提示文字之后不断；提示文字还额外"占宽度而不占位置"（侧别数格子时跳过，
 * 见 LineCluster::control_hint）。
 *
 * shaped 可能正指着只读的段级缓存条目，需要可改副本时用它旁边的 scratch。
 * @param max_hint_pt 这个容器能给提示文字的宽度上限（装不下就截断，绘制按同一上限截）
 */
const BlockShaping* withInlineObjects(const BlockSpec& b, uint32_t n_cp, const BlockShaping* shaped,
                                      BlockShaping* scratch, const std::vector<FormatRun>& fruns,
                                      FontLibrary& lib, float max_hint_pt) {
    if (b.objects.empty()) return shaped;
    const BlockShaping& src = *shaped;
    // 槽宽 = 括号字形 advance + 间隙。间隙必须计入排版，否则 字] 会挤在一起。
    //   字体按边界那一侧的 run 现取：同一段里换了字体，两侧括号的宽度可以不同。
    auto slotOf = [&](const InlineObject& o) {
        FontInstance* f = bracketFontFor(o, fruns, lib, n_cp, src.primary);
        const float bw = bracketAdvancePt(f);
        return bw > 0.0f ? bw + kControlBracketSlotGapPt : 0.0f;
    };
    // 一个都插不进去（取不到括号字形，表里又只有控件边界）就原样返回。
    //   原子不走这一条：它的宽度是自己给的，没有字体照样排得出。有提示文字的也留下：
    //   括号画不出时那句话仍要占位，否则空控件变成零宽，点击与光标都没地方落。
    bool has_atom = false;
    bool has_slot = false;
    for (const InlineObject& o : b.objects) {
        if (o.kind == InlineObjectKind::Atom) {
            has_atom = true;
        } else if (slotOf(o) > 0.0f || !o.hint.empty()) {
            has_slot = true;
        }
    }
    if (!has_slot && !has_atom) return shaped;

    // 这一段要插占位 → 簇序列会被改 → 需要一份可改的副本
    // （shaped 可能正指着只读的缓存条目）。
    if (shaped != scratch) *scratch = src;
    std::vector<BlockShaping::Cluster> cl2;
    std::vector<float> adv2;
    std::vector<bool> brk2;
    cl2.reserve(scratch->clusters.size() + b.objects.size());
    adv2.reserve(scratch->clusters.size() + b.objects.size());
    brk2.reserve(scratch->clusters.size() + b.objects.size());
    // 插一个占位槽：cp 取"它后面那个真实簇的 cp"（末尾用段长哨兵）。槽里带的是"这一格
    //   是什么"（括号朝向、原子种类与盒高），光标与命中测试不读 bracket_side ——
    //   它们按 cp 与 placeholder 定位，侧别另由位置层按对象表算。
    // idx 是对象表里的下标：空控件的提示文字要往后看一格，才判得出这一对边界是紧挨着的。
    auto emitSlot = [&](const InlineObject& o, size_t idx, uint32_t cp) {
        BlockShaping::Cluster pc;
        pc.cp = cp;
        pc.g0 = pc.g1 = 0;
        pc.placeholder = true;
        pc.owner_control = o.owner;
        // 内容指纹两族都搬：原子的是"画的是什么"（换了同尺寸的图），控件边界的是外观摘要
        //   （括号颜色、记号颜色）。少搬一族，那一类改动在几何上看不出来，页就会停在旧帧。
        pc.content_hash = o.content_hash;
        if (o.kind == InlineObjectKind::Atom) {
            pc.atom = o.atom;
            pc.atom_h_pt = o.h_pt;
            cl2.push_back(pc);
            adv2.push_back(o.w_pt);
            // 盒子之后可以断：它与一个全角字同价，断在它后面不拆开任何东西。
            brk2.push_back(true);
            return;
        }
        pc.bracket_side =
            (o.kind == InlineObjectKind::ControlStart) ? BracketSide::Open : BracketSide::Close;
        pc.read_only = o.read_only;
        pc.bracket_font = bracketFontFor(o, fruns, lib, n_cp, src.primary);
        cl2.push_back(pc);
        adv2.push_back(slotOf(o));
        brk2.push_back(false);  // 括号内部不是断点
        // 空控件的提示文字排在开括号之后、闭括号之前。
        //   两个条件缺一不可：产生端只在"这个控件一个码点都没吃进来"时给 hint，
        //   这里再确认紧跟的就是它自己的结束边界（同一锚点）—— 中间夹了内容或嵌套
        //   控件时那句提示没有意义，凭空占一截宽度只会把后面的字挤开。
        if (o.hint.empty() || idx + 1 >= b.objects.size()) return;
        const InlineObject& next = b.objects[idx + 1];
        if (next.kind != InlineObjectKind::ControlEnd || next.owner != o.owner || next.cp != o.cp) {
            return;
        }
        float w = hintAdvancePt(pc.bracket_font, o.hint);
        if (w > max_hint_pt) w = max_hint_pt;
        if (!(w > 0.0f)) return;
        BlockShaping::Cluster hc;
        hc.cp = cp;
        hc.g0 = hc.g1 = 0;
        hc.placeholder = true;
        hc.control_hint = true;
        hc.owner_control = o.owner;
        hc.read_only = o.read_only;
        hc.bracket_font = pc.bracket_font;
        // 提示文字的摘要必须进簇：改一句话而字数不变时几何一个都不动，行指纹若只看几何
        //   就会判成"这一页还是新鲜的"，画面上仍是上一句。
        hc.content_hash = fnvMixCStr(kFnvBasis, o.hint.c_str());
        cl2.push_back(hc);
        adv2.push_back(w);
        brk2.push_back(false);
    };
    size_t oi = 0;
    for (size_t i = 0; i <= scratch->clusters.size(); ++i) {
        const uint32_t at = (i < scratch->clusters.size()) ? scratch->clusters[i].cp : n_cp;
        while (oi < b.objects.size() && b.objects[oi].cp <= at) {
            emitSlot(b.objects[oi], oi, at);
            ++oi;
        }
        if (i < scratch->clusters.size()) {
            cl2.push_back(scratch->clusters[i]);
            adv2.push_back(scratch->cluster_adv_pt[i]);
            brk2.push_back(scratch->break_after[i]);
        }
    }
    // 锚点越过段长的对象夹到段尾，而不是丢掉：排版层假定表已合序且锚点不越界
    //   （见 objectsOrdered），越界说明产生端算错了端点。此时丢一枚括号 = 凭空少画
    //   一层，画面"看着还有括号"却层级不对，比排版错位更难发现；夹到段尾至少是
    //   错误的可见形态，也不会让后面的括号整串前移。
    for (; oi < b.objects.size(); ++oi) emitSlot(b.objects[oi], oi, n_cp);
    scratch->clusters.swap(cl2);
    scratch->cluster_adv_pt.swap(adv2);
    scratch->break_after.swap(brk2);
    return scratch;
}

/**
 * 断出来的一行（竖排下是一列）：簇区间与这一行自己的度量。
 *
 * 它刻意不含页号与 y —— 那两样是分页的产物，断行只看"装得下多少"。这条分工是排版拆成
 * 两件事的全部内容：断行是块局部的（给定容量与簇，结果是确定的），分页是行流上的前缀和
 * （给定每行高度与页高，页边界是确定的）。
 */
struct BrokenLine {
    size_t begin = 0;  // 簇下标（含）
    size_t end = 0;    // 簇下标（不含）
    /**
     * 本行的行高（竖排下是列宽）：行距与内容撑高取大，见 breakLine 里的说明。
     */
    float line_h = 0.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    /**
     * 本行最大字号。行盒的 size_pt 与行距都由它定；一个 run 时就是块级那套值。
     */
    float size_pt = 0.0f;
};

/**
 * 从 start 起断出下一行：容量是 avail，度量取自每个簇所在的 run。
 *
 * 行高 = 单倍行高 × 倍数，单倍行高取字体自己的 ascent + descent（不是字号：字体的纵向
 * 留白本来就属于"一行有多高"）。行里塞进比文本更高的东西（原子盒）时，撑高的是内容，
 * 行距本身要留着：max(行距, 内容高 + 行距 - 文本高)。纯文本行两项相等 ⇒ 就是行距；
 * 原子行 ⇒ 盒高 + 行距 - ascent。只取 max(行距, 盒高) 是错的：那样盒底下面一点空隙都
 * 没有，下一行的字紧贴着盒底排（实测相邻两行的行盒还重叠 3.5 磅）—— 看起来就是
 * "那一行的行距没了"。行距是段落属性，不该被行里的一枚盒子吃掉。
 *
 * 横排与竖排用的是同一个式子（那边叫 col_w，这边叫 line_h），所以它只有一份实现。
 */
BrokenLine breakLine(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric,
                     size_t start, float avail, float block_size_pt, float line_factor) {
    const LineSlice slice = takeLine(bs.cluster_adv_pt, bs.break_after, start, avail);

    BrokenLine out;
    out.begin = slice.begin;
    out.end = slice.end;

    float ascent = 0.0f;
    float descent = 0.0f;
    float max_size = 0.0f;
    float max_box = 0.0f;
    // 文本自己的度量（不含原子）：行距是"文本行之间的距离"，由文本的 ascent + descent
    // 定 —— 算行高时要把它和"内容撑出来的高度"分开，见上面那段。
    float text_ascent = 0.0f;
    float text_descent = 0.0f;
    for (size_t i = slice.begin; i < slice.end; ++i) {
        ascent = std::max(ascent, cluster_metric[i].ascent_pt);
        descent = std::max(descent, cluster_metric[i].descent_pt);
        max_size = std::max(max_size, cluster_metric[i].size_pt);
        max_box = std::max(max_box, cluster_metric[i].box_h_pt);
        if (bs.clusters[i].atom == InlineAtom::None) {
            text_ascent = std::max(text_ascent, cluster_metric[i].ascent_pt);
            text_descent = std::max(text_descent, cluster_metric[i].descent_pt);
        }
    }
    // 一个度量都取不到（取不到字体）就退回块级那套：几何必须存在。
    if (max_size <= 0.0f && max_box <= 0.0f) {
        ascent = bs.ascent_pt;
        descent = bs.descent_pt;
        max_size = block_size_pt;
    }

    // 单倍行高 = 字体自己的 ascent + descent（取不到度量时退回块级那套，再退回字号）。
    //   行距以它为基准，与字号脱钩：字体自带的纵向留白已经算在里面，1.0 就是它本身。
    //   以字号为基准时这段留白会把小倍数吃掉 —— 1.0 到 1.45 全落在同一个行高上。
    //   倍数小于 1 时被单倍行高压住（不压缩：两行字会叠在一起）。
    float natural = text_ascent + text_descent;
    if (!(natural > 0.0f)) natural = bs.ascent_pt + bs.descent_pt;
    if (!(natural > 0.0f)) natural = (max_size > 0.0f) ? max_size : block_size_pt;
    const float pitch = natural * line_factor;
    const float leading = std::max(0.0f, pitch - natural);

    out.ascent = ascent;
    out.descent = descent;
    out.size_pt = max_size;
    out.line_h = std::max(pitch, ascent + descent + leading);
    return out;
}

/**
 * 整块的断行：横排的容量是恒定的可用宽度，所以一次切完。
 *
 * 竖排不能这样切：一列的容量是"这一条带还能放多高"，而换带（分页）由已经排了几列决定 ——
 * 容量随排版推进而变，只能一列一列地断（见竖排分支里的 breakLine 调用）。
 */
void breakBlock(const BlockShaping& bs, const std::vector<LineMetric>& cluster_metric, float width,
                float block_size_pt, float line_factor, std::vector<BrokenLine>* out) {
    out->clear();
    if (bs.clusters.empty()) return;
    size_t start = 0;
    while (start < bs.clusters.size()) {
        const BrokenLine line =
            breakLine(bs, cluster_metric, start, width, block_size_pt, line_factor);
        start = line.end;
        out->push_back(line);
    }
}

}  // namespace

bool runsTileText(const BlockSpec& b, std::string* why) {
    const uint32_t n = static_cast<uint32_t>(b.cp_text.size());
    if (b.runs.empty()) return true;  // 等价情形：整段一个 run，属性取块级字段
    uint32_t expect = 0;
    for (std::size_t i = 0; i < b.runs.size(); ++i) {
        const FormatRun& r = b.runs[i];
        const std::string at = "第 " + std::to_string(i) + " 个 run";
        if (r.cp_end <= r.cp_begin) {
            if (why) *why = at + " 是空区间（cp_end 必须大于 cp_begin）";
            return false;
        }
        if (r.cp_begin != expect) {
            if (why) {
                *why = at + " 从 " + std::to_string(r.cp_begin) + " 起，但前一个结束在 " +
                       std::to_string(expect) + "（" +
                       (r.cp_begin < expect ? "与前一个重叠" : "与前一个之间有空洞") + "）";
            }
            return false;
        }
        expect = r.cp_end;
    }
    if (expect != n) {
        if (why) {
            *why = "run 序列铺到 " + std::to_string(expect) + "，段落文本有 " + std::to_string(n) +
                   " 个码点（" + (expect < n ? "尾部有空洞" : "超出了文本末尾") + "）";
        }
        return false;
    }
    return true;
}

const FormatRun* runCovering(const std::vector<FormatRun>& runs, uint32_t cp) {
    for (const FormatRun& r : runs) {
        if (cp < r.cp_end) return cp >= r.cp_begin ? &r : nullptr;
    }
    return nullptr;
}

bool objectsOrdered(const BlockSpec& b, std::string* why) {
    const uint32_t n = static_cast<uint32_t>(b.cp_text.size());
    uint32_t prev = 0;
    for (std::size_t i = 0; i < b.objects.size(); ++i) {
        const InlineObject& o = b.objects[i];
        const std::string at = "第 " + std::to_string(i) + " 个行内对象";
        if (o.cp > n) {
            if (why) {
                *why = at + "锚在 " + std::to_string(o.cp) + "，超出段落长度 " + std::to_string(n);
            }
            return false;
        }
        if (i > 0 && o.cp < prev) {
            if (why) {
                *why = at + "锚在 " + std::to_string(o.cp) + "，比前一个的 " +
                       std::to_string(prev) + " 更靠前（对象表必须按 cp 升序）";
            }
            return false;
        }
        prev = o.cp;
    }
    return true;
}

/**
 * 段级整形缓存的内部结构。
 * 按段下标存放而非哈希表：段是编辑的最小单位（当前不支持跨段编辑），下标访问即可
 * 直接复用，缓存长度也与 blocks 天然一致、有界。指纹只用于判断本段内容是否变化；
 * 指纹不符时视为未命中并原地重填。
 */
struct ShapingCache::Impl {
    struct Entry {
        uint64_t fp = 0;
        bool filled = false;
        BlockShaping bs;  // 纯整形结果，不含括号占位（占位每轮现插）
    };
    std::vector<Entry> paras;
    long long hits = 0;
    long long misses = 0;
};

ShapingCache::ShapingCache() : impl_(new Impl()) {}
ShapingCache::~ShapingCache() { delete impl_; }

void ShapingCache::clear() {
    impl_->paras.clear();
    impl_->hits = 0;
    impl_->misses = 0;
}
size_t ShapingCache::size() const { return impl_->paras.size(); }
long long ShapingCache::hits() const { return impl_->hits; }
long long ShapingCache::misses() const { return impl_->misses; }

/**
 * 存取 ShapingCache 私有实现的唯一入口（ShapingCache 的 friend）。
 * 目的是把缓存内部结构留在本文件，同时让同文件的排版函数可以使用它。
 */
struct ShapingCacheAccess {
    static ShapingCache::Impl* impl(ShapingCache* c) { return c ? c->impl_ : nullptr; }
};

/**
 * 全量排版：从第 0 段排到末尾，不使用缓存（对照路径与基准）。
 */
LayoutResult layoutDocument(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                            const PageMetrics& page, const std::vector<LayoutTable>& tables) {
    LayoutResult res;
    layoutDocumentRangeWithTables(lib, blocks, page, tables, 0, static_cast<int>(blocks.size()),
                                  freshReflowState(page), /*cache=*/nullptr, &res);
    return res;
}

ReflowState layoutDocumentRange(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                const PageMetrics& page, int from_para, int to_para,
                                const ReflowState& state, ShapingCache* cache, LayoutResult* out) {
    return layoutDocumentRangeWithTables(lib, blocks, page, {}, from_para, to_para, state, cache,
                                         out);
}

ReflowState layoutDocumentRangeWithTables(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                          const PageMetrics& page,
                                          const std::vector<LayoutTable>& tables, int from_para,
                                          int to_para, const ReflowState& state,
                                          ShapingCache* cache, LayoutResult* out) {
    const auto t_layout0 = Clock::now();
    LayoutResult& res = *out;
    ShapingCache::Impl* cache_impl = ShapingCacheAccess::impl(cache);

    const float cx = page.margins.left;
    const float cy = page.margins.top;
    const float cw = page.contentWidthPt();
    const float ch = page.contentHeightPt();
    const float usToPt = 1.0f / kFontScalePerPt;

    const int n_para = static_cast<int>(blocks.size());
    if (from_para < 0) from_para = 0;
    if (to_para > n_para) to_para = n_para;

    // 每个块一份簇起点表；空块给 {0}（只有段首一个插入点）。
    // 只补到需要的长度：前缀可能来自上一轮排版并原样复用（见追加语义）。
    if (res.para_cluster_starts.size() < blocks.size())
        res.para_cluster_starts.resize(blocks.size(), std::vector<uint32_t>{0});
    if (res.para_index.size() < blocks.size()) res.para_index.resize(blocks.size());

    int page_index = state.page_index;
    float cursor_y = state.cursor_y;  // 当前块在本页的起始 y（块流恒为自上而下）
    int line_total = state.line_total;
    double shape_ms = 0.0;  // 本轮真实整形耗时；命中缓存的段不计

    // 连页：块轴位置一路累加，页号由它现算。断页判据在下面每一处都短路。
    //   条带高度与帧的槽步距是同一个数（页高 + 页间隙），所以"这段内容落在哪一条带"
    //   在排版层就定下来了 —— 绘制、光标、选区、命中测试读的都是同一个页号。
    const bool cont = page.continuous;
    const float slab = page.slabHeightPt();
    // 块轴位置 -> 页号。分页下就是循环里那个页号（排版自己决定翻页），
    //   连页下由位置现算（没有翻页这回事）。
    auto slabOfY = [&](float y) -> int {
        if (!cont) return page_index;
        return static_cast<int>(std::floor(y / slab));
    };
    // 块轴位置 -> 页内位置。分页下是恒等（cursor_y 本来就是页内坐标），
    //   连页下要减掉条带偏移。凡是"写进 LineBox / PlacedGlyph 的 y"都必须过它 ——
    //   漏一处就是那一处按绝对坐标落进帧里（跨条带时整体错位一条带）。
    auto locY = [&](int pg, float y) -> float {
        if (!cont) return y;
        return y - static_cast<float>(pg) * slab;
    };

    // 取出一段的纯整形结果（优先命中段级缓存）。
    //
    // 命中时返回的是指向缓存条目的只读引用，不做深拷贝：一份 BlockShaping 里挂着
    // 字形、字形所属字体、簇、簇推进、断点、簇起点六条等长的表，深拷贝一段就是六次
    // 分配加搬移 —— 在增量路径上（后缀可能有几百段）这是最贵的一步，而绝大多数段的
    // 数据一个字都不会改。只有要插括号占位的那一段才需要一份可改的副本，
    // 由调用方在那一条分支上复制（见下面 scratch 的用法）。
    // pi 是出参，提前返回的路径同样要写好统计值。
    // scratch 是"未命中时整形到这里"的缓冲；返回值指向 scratch 或缓存条目。
    auto shapeParagraph = [&](const BlockSpec& b, const std::vector<FormatRun>& fruns,
                              size_t para_index, BlockShaping* scratch,
                              ParaIndex* pi) -> const BlockShaping* {
        if (b.cp_text.empty()) return nullptr;
        // 行度量（基线、行高、段后距）仍取块级字段：一段一套行高，这是本期已知边界。
        // run 上的字号只决定字形与推进，不参与行盒高度 —— 见 layout.h 的 FormatRun。
        FontInstance* font = lib.instance(b.font_index, b.size_pt);
        if (!font) return nullptr;

        const uint64_t fp = shapingFingerprint(b, fruns);
        if (cache_impl && para_index < cache_impl->paras.size()) {
            const ShapingCache::Impl::Entry& e = cache_impl->paras[para_index];
            if (e.filled && e.fp == fp) {
                ++cache_impl->hits;
                pi->cluster_count = static_cast<int>(e.bs.clusters.size());
                pi->missing_count = e.bs.missing_count;
                pi->font_run_count = e.bs.run_count;
                return e.bs.clusters.empty() ? nullptr : &e.bs;
            }
        }
        if (cache_impl) ++cache_impl->misses;
        BlockShaping& bs = *scratch;

        bs.primary = font;
        bs.ascent_pt = font->ascenderPt();
        bs.descent_pt = font->descenderPt();
        // 这两个标记只由书写模式决定，先于切段赋值，避免依赖「至少有一段整形成功」。
        bs.st.vertical = (b.wm.shapingDirection() == ShapingDirection::TopToBottom);
        bs.st.sideways = b.wm.isSideways();

        // 整形：先按字符格式 run 切，再在每段内按字体回退切 —— 两把尺子求交。
        // 每个整形段只整形一次，切段依据是 cmap 精确查询而非整形结果。
        // 字号取 run 自己的：run 是"字符格式相同"的区间，字号不同就不该是同一个 run。
        const auto t_shape0 = Clock::now();
        for (const FormatRun& fr : fruns) {
            if (fr.cp_end <= fr.cp_begin) continue;
            std::vector<int> chain;
            chain.reserve(1 + fr.fallback_font_indices.size());
            chain.push_back(fr.font_index);
            for (int fi : fr.fallback_font_indices) {
                if (fi != fr.font_index) chain.push_back(fi);
            }
            // itemizeByFont 的区间是相对于喂进去的指针的，所以传 run 的起点，
            // 拿回来的 cp_begin/cp_end 要加回 fr.cp_begin 才是段内绝对下标。
            const std::vector<FontRun> runs = itemizeByFont(lib, b.cp_text.data() + fr.cp_begin,
                                                            fr.codepointCount(), chain, fr.size_pt);
            pi->font_run_count += static_cast<int>(runs.size());
            for (const FontRun& r : runs) {
                const size_t begin = fr.cp_begin + r.cp_begin;
                const size_t end = fr.cp_begin + r.cp_end;
                if (end <= begin) continue;
                // 字重也要跟 run 走：粗体的字形与度量都来自 700 那一份实例，
                //   沿用 400 就等于"排版按细体、画出来也是细体"。
                FontInstance* rf = font;
                if (r.face_index >= 0) {
                    FontInstance* hit = lib.instance(r.face_index, fr.size_pt, fr.weight);
                    if (hit) rf = hit;
                }
                // 用 shapeTextRange 传整串指针加 [begin, end)：HarfBuzz 以区间起点为基准编号
                // cluster，故 cluster 就是段内绝对码点下标，此处不得再做任何偏移补算。
                const ShapedText part =
                    shapeTextRange(rf->hb(), b.cp_text.data(), begin, end, b.wm, fr.language);
                for (const ShapedGlyph& g : part.glyphs) {
                    if (g.glyph_id == 0) ++pi->missing_count;  // 整条链都没兜住的码点
                    bs.st.glyphs.push_back(g);
                    bs.glyph_font.push_back(rf);
                }
                bs.st.inline_advance += part.inline_advance;
            }
        }
        shape_ms += msSince(t_shape0);
        bs.run_count = pi->font_run_count;
        bs.missing_count = pi->missing_count;

        // 按 cluster 分组，并算出每个簇沿行内轴的推进。
        bs.clusters.reserve(bs.st.glyphs.size());
        for (size_t i = 0; i < bs.st.glyphs.size(); ++i) {
            if (bs.clusters.empty() || bs.clusters.back().cp != bs.st.glyphs[i].cluster) {
                bs.clusters.push_back(BlockShaping::Cluster{bs.st.glyphs[i].cluster, i, i + 1});
            } else {
                bs.clusters.back().g1 = i + 1;
            }
        }
        pi->cluster_count = static_cast<int>(bs.clusters.size());

        // 簇起点表（升序）+ 段长哨兵 —— 交给编辑层做吸附
        bs.cluster_starts.clear();
        bs.cluster_starts.reserve(bs.clusters.size() + 1);
        for (const auto& c : bs.clusters) bs.cluster_starts.push_back(c.cp);
        bs.cluster_starts.push_back(static_cast<uint32_t>(b.cp_text.size()));

        bs.cluster_adv_pt.assign(bs.clusters.size(), 0.0f);
        bs.break_after.assign(bs.clusters.size(), false);
        for (size_t i = 0; i < bs.clusters.size(); ++i) {
            float adv = 0.0f;
            for (size_t g = bs.clusters[i].g0; g < bs.clusters[i].g1; ++g) {
                adv += bs.st.vertical ? -bs.st.glyphs[g].y_advance * usToPt
                                      : bs.st.glyphs[g].x_advance * usToPt;
            }
            bs.cluster_adv_pt[i] = adv;
            // 断行判定按码点查表。
            bs.break_after[i] = canBreakAfter(b.cp_text[bs.clusters[i].cp]);
        }

        // 写入缓存（纯整形，不含占位）。
        if (cache_impl) {
            if (cache_impl->paras.size() <= para_index) cache_impl->paras.resize(para_index + 1);
            ShapingCache::Impl::Entry& e = cache_impl->paras[para_index];
            e.fp = fp;
            e.filled = true;
            e.bs = bs;  // 拷进缓存的是**纯整形**；占位阶段只会动 scratch，不会动它
        }
        return bs.clusters.empty() ? nullptr : &bs;
    };

    // 表格排版上下文。
    //
    // 表格是扁平化进块流的：格里的块照常走这个循环，只是它们的行内起点与可用宽度由格矩形
    // 给。所以这里维护一个栈（嵌套表格），在块之间切换时开/关格与行。
    //
    // 一行多格的顺序就是块流顺序（(0,0) 的块、然后 (0,1) 的块、然后 (1,0) …），于是
    // "同行的下一格"要回到行顶重排，而"这一行多高"要等所有格都排完才知道 —— 所以行高在
    // closeRow 里结算。整行放不下时把这一行已经写下的行盒与字形一起平移（行内排版不翻页，
    // 否则同一行的几个格会落到不同的页上）。
    struct TableFrame {
        const LayoutTable* t = nullptr;
        std::vector<float> col_x;  // 每列左边缘（相对表格左边）
        std::vector<float> col_w;
        float x = 0.0f;  // 表格左边缘（页内坐标）
        float w = 0.0f;
        float cell_cx = 0.0f;  // 当前格的行内起点与可用宽度（进内层表时保存，回来后恢复）
        float cell_cw = 0.0f;
        // 当前格的外框（行内轴，页内坐标）：内容区两侧各让出一层内边距。
        //   选区反白要的是这一对 —— 用内容区反白会在左右各留一条缝。
        float cell_box_lo = 0.0f;
        float cell_box_hi = 0.0f;
        int row = -1;
        int cell = -1;
        float row_top = 0.0f;
        float row_content_h = 0.0f;
        size_t line_begin = 0;    // 本行第一个行盒下标（换页时整行平移）
        size_t glyph_begin = 0;   // 同上，字形
        size_t placed_index = 0;  // 这张表在 res.placed_tables 里的下标（绘制几何）
    };
    std::vector<TableFrame> tstack;
    // 行内起点与可用宽度：正文是内容区左边界与内容区宽，格内是格左加内边距与格宽减两侧。
    //   横排分支只读这两个变量，于是"在正文里排"与"在格里排"共用同一套断行与放置代码。
    float cur_cx = cx;
    float cur_cw = cw;
    // 最后排完的那一块的段后距。格内容高要把它扣掉：格的底边由最后一段的文字底加内边距
    //   定，段后距是段与段之间的距离，最后一段后面没有下一段可让。
    float last_space_after = 0.0f;

    auto tableByHandle = [&tables](uint64_t h) -> const LayoutTable* {
        for (const LayoutTable& t : tables) {
            if (t.handle == h) return &t;
        }
        return nullptr;
    };

    // 进表：列宽与表格位置只由表宽与列定义决定，与内容无关，所以一次算好。
    //   自动列平分剩余宽度；显式列宽之和超出表宽时整体按比例缩 —— 否则表格会越出内容区。
    auto openTable = [&](const LayoutTable& t, float top_y) {
        TableFrame f;
        f.t = &t;
        float w = cw;
        switch (t.width.kind) {
            case WidthSpec::Kind::Abs:
                w = t.width.pt;
                break;
            case WidthSpec::Kind::Percent:
                w = cw * t.width.ratio;
                break;
            case WidthSpec::Kind::Auto:
                break;
        }
        w = std::min(std::max(w, 1.0f), cw);
        float shift = 0.0f;
        switch (t.align) {
            case TableHAlign::Center:
                shift = 0.5f * (cw - w);
                break;
            case TableHAlign::End:
                shift = cw - w;
                break;
            case TableHAlign::Start:
                break;
        }
        f.x = cx + shift;
        f.w = w;
        const int cols = std::max(t.cols, 0);
        f.col_w.assign(static_cast<size_t>(cols), 0.0f);
        float used = 0.0f;
        int auto_cols = 0;
        for (int c = 0; c < cols; ++c) {
            const WidthSpec spec = (static_cast<size_t>(c) < t.col_widths.size())
                                       ? t.col_widths[static_cast<size_t>(c)]
                                       : WidthSpec{};
            if (spec.kind == WidthSpec::Kind::Abs) {
                f.col_w[static_cast<size_t>(c)] = spec.pt;
            } else if (spec.kind == WidthSpec::Kind::Percent) {
                f.col_w[static_cast<size_t>(c)] = w * spec.ratio;
            } else {
                ++auto_cols;
            }
            used += f.col_w[static_cast<size_t>(c)];
        }
        if (auto_cols > 0) {
            const float each = std::max(0.0f, w - used) / static_cast<float>(auto_cols);
            for (int c = 0; c < cols; ++c) {
                const WidthSpec spec = (static_cast<size_t>(c) < t.col_widths.size())
                                           ? t.col_widths[static_cast<size_t>(c)]
                                           : WidthSpec{};
                if (spec.kind == WidthSpec::Kind::Auto) f.col_w[static_cast<size_t>(c)] = each;
            }
        }
        float total = 0.0f;
        for (float v : f.col_w) total += v;
        if (total > w && total > 0.0f) {
            const float k = w / total;
            for (float& v : f.col_w) v *= k;
        }
        f.col_x.assign(f.col_w.size(), 0.0f);
        float acc = 0.0f;
        for (size_t c = 0; c < f.col_w.size(); ++c) {
            f.col_x[c] = acc;
            acc += f.col_w[c];
        }
        f.row_top = top_y;
        f.line_begin = res.lines.size();
        f.glyph_begin = res.glyphs.size();
        // 绘制几何：列边界与表格位置在这里定下，行片段由 closeRow 逐行追加。
        {
            PlacedTable pt;
            pt.handle = t.handle;
            // 起始段 = 所有格里最小的 first_block。表的位置由列定义定，但"它从第几段开始"
            //   是格 → 段的映射给的，两者一起构成这张表的身份。
            pt.first_block = t.cells.empty() ? 0 : t.cells.front().first_block;
            for (const LayoutCell& c : t.cells) {
                pt.first_block = std::min(pt.first_block, c.first_block);
            }
            pt.x = f.x;
            pt.w = w;
            pt.rows = t.rows;
            pt.cols = cols;
            pt.col_x = f.col_x;
            pt.col_x.push_back(w);
            pt.cells = t.cells;
            res.placed_tables.push_back(std::move(pt));
            f.placed_index = res.placed_tables.size() - 1;
        }
        tstack.push_back(std::move(f));
    };

    // 出一个格：把这一格的内容高记进本行的最大值。cursor_y 此刻已经在格内容的末尾 ——
    //   收尾放在"切到下一格/下一行"时做，所以遍历途中的提前 continue 一个都不用记。
    auto closeCell = [&]() {
        TableFrame& f = tstack.back();
        if (f.cell >= 0) {
            f.row_content_h = std::max(f.row_content_h,
                                       cursor_y - f.row_top - kTableCellPadYPt - last_space_after);
        }
        f.cell = -1;
    };

    // 整行挪到下一页（不允许跨页断行的那一档）。
    auto shiftRowDown = [&](TableFrame& f) {
        const float dy = cy - f.row_top;
        for (size_t k = f.line_begin; k < res.lines.size(); ++k) {
            LineBox& lb = res.lines[k];
            lb.block_lo_pt += dy;
            lb.block_hi_pt += dy;
            lb.bracket_axis_pt += dy;
            lb.page += 1;
        }
        const int line_no = (f.line_begin < res.lines.size()) ? res.lines[f.line_begin].line : 0;
        for (PlacedGlyph& g : res.glyphs) {
            if (g.line < line_no) continue;
            g.y_pt += dy;
            g.page += 1;
        }
        ++page_index;
        f.row_top = cy;
        cursor_y = cy;
    };

    // 把这一行占的块轴范围切成若干片段（一块落在某一页上）。切点落在行与行之间，
    //   一个行盒不被切开；行盒按自己的纵向位置归属片段，不从数组后缀推 ——
    //   一行的几个格都从行顶起排，它们的行盒在数组里是"格序"而不是"纵向次序"，
    //   按后缀搬会把同行其它格的行盒一起搬走（空单元格的光标就此跑到下一页）。
    //
    // 每片的顶边是格线位置，内容从顶边下 kTableCellPadYPt 起；续排片段从页面内容区
    //   上沿起画顶边，于是跨页那一刀看起来是"接着上一页"，而不是另起一格。
    struct RowPiece {
        /**
         * 落在第几页（连页下是条带号）。
         */
        int page = 0;

        /**
         * 片段的格线上沿（页内坐标，磅）。
         */
        float box_top = 0.0f;

        /**
         * 片段内容的第一行能落到的位置（页内坐标，磅）。
         */
        float content_top = 0.0f;

        /**
         * 片段的格线下沿（页内坐标，磅）。
         */
        float box_bottom = 0.0f;

        /**
         * 这一片的上边是切出来的（接着上一片），不是这一行自己的边。
         */
        bool cont_top = false;

        /**
         * 这一片的下边是切出来的，不是这一行自己的边。
         */
        bool cont_bottom = false;
    };
    // 分页切片：返回这一行的片段（升序），并把行盒与字形挪到各自的页上。
    //   allow_split 为假时整行一片，放不下就溢出（比一整页还高的固定行高）。
    auto sliceRowByPage = [&](TableFrame& f, float row_top, float row_h,
                              bool allow_split) -> std::vector<RowPiece> {
        // 本行行盒的纵向位置（相对行顶）。同高的行盒按块流顺序（列序）稳定排。
        struct Item {
            size_t index = 0;
            float off = 0.0f;
            float h = 0.0f;
            float dy = 0.0f;
        };
        std::vector<Item> items;
        items.reserve(res.lines.size() - f.line_begin);
        for (size_t j = f.line_begin; j < res.lines.size(); ++j) {
            const LineBox& lb = res.lines[j];
            items.push_back(
                Item{j, lb.block_lo_pt - row_top, lb.block_hi_pt - lb.block_lo_pt, 0.0f});
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) { return a.off < b.off; });

        std::vector<RowPiece> pieces;
        std::vector<float> starts;
        const float page_bottom = cy + ch;
        int page = page_index;
        float start = 0.0f;
        float content_top = row_top;
        RowPiece first;
        first.page = page;
        first.box_top = row_top;
        first.content_top = row_top;
        pieces.push_back(first);
        starts.push_back(0.0f);

        bool retagged = false;
        for (Item& it : items) {
            // 这一行在本片放不下：另起一片，从页面的内容区上沿开始（续排片段的内容
            //   仍要让出一层内边距，否则第一行的字要压在补画的那条格线上）。
            if (allow_split && it.off > start + 0.01f &&
                content_top + (it.off - start) + it.h > page_bottom + 0.01f && pieces.size() < 64) {
                ++page;
                start = it.off;
                content_top = cy + kTableCellPadYPt;
                RowPiece next;
                next.page = page;
                next.box_top = cy;
                next.content_top = content_top;
                next.cont_top = true;
                pieces.back().cont_bottom = true;
                pieces.push_back(next);
                starts.push_back(start);
            }
            it.dy = content_top + (it.off - start) - res.lines[it.index].block_lo_pt;
            LineBox& lb = res.lines[it.index];
            lb.block_lo_pt += it.dy;
            lb.block_hi_pt += it.dy;
            lb.bracket_axis_pt += it.dy;
            if (lb.page != page) retagged = true;
            lb.page = page;
        }
        // 片段的上下沿：首片从行顶起，续片从页面内容区上沿起；最后一片到行底，
        //   其余各片填到页底（跨页的那一刀画在页边界上）。
        for (size_t k = 0; k < pieces.size(); ++k) {
            RowPiece& p = pieces[k];
            const bool last = (k + 1 == pieces.size());
            p.box_bottom = last ? (p.content_top + (row_h - starts[k])) : page_bottom;
            if (p.box_bottom < p.box_top) p.box_bottom = p.box_top;
        }
        // 字形跟着它那一行一起挪：行号（不是下标）是字形的归属键，行号在增量续排之后
        //   不一定等于行盒的下标。只在这一行真的被切过时才走一遍 —— 没切时每个 dy 都是 0，
        //   而这张表每出一行都要过一遍字形，大表上的代价是 O(行数 x 字形数)。
        bool moved = retagged;
        for (const Item& it : items) {
            if (it.dy != 0.0f) moved = true;
        }
        if (moved) {
            /**
             * 一条行盒的位移：字形的归属键是行号，页号也要跟着走。
             */
            struct Shift {
                /**
                 * 行号（LineBox::line）。
                 */
                int line = 0;

                /**
                 * 页内 y 的位移（磅）。
                 */
                float dy = 0.0f;

                /**
                 * 这一行落到第几页。
                 */
                int page = 0;
            };
            std::vector<Shift> shifts;
            shifts.reserve(items.size());
            for (const Item& it : items) {
                const LineBox& lb = res.lines[it.index];
                shifts.push_back(Shift{lb.line, it.dy, lb.page});
            }
            std::sort(shifts.begin(), shifts.end(),
                      [](const Shift& a, const Shift& b) { return a.line < b.line; });
            size_t si = 0;
            // 本行的字形是 glyph_begin 之后那一段（格按流顺序排，前面几行的字形不在这里）。
            //   页号必须一起改：字形是按页号区间画的，页号留在旧页 = 画到上一页去
            //   （重叠在别的字上），新页上则一个字都没有 —— 而两份自审都是"同一份错"。
            for (size_t gi = f.glyph_begin; gi < res.glyphs.size(); ++gi) {
                PlacedGlyph& g = res.glyphs[gi];
                if (g.line < shifts.front().line) continue;
                while (si + 1 < shifts.size() && shifts[si + 1].line <= g.line) ++si;
                if (shifts[si].line != g.line) continue;
                g.y_pt += shifts[si].dy;
                g.page = shifts[si].page;
            }
        }
        return pieces;
    };

    // 连页切片：行不拆（条带不是页，切一刀不该改变内容的分页），只按条带边界切出
    //   若干绘制片段。行盒的页内坐标在放置时已经按条带折过，这里只算片段几何。
    auto sliceRowByBand = [&](float row_top, float row_h) -> std::vector<RowPiece> {
        std::vector<RowPiece> pieces;
        const float slab = page.slabHeightPt();
        float off = 0.0f;
        while (pieces.size() < 64) {
            const float top_global = row_top + off;
            const int band = static_cast<int>(std::floor(top_global / slab));
            const float band_top = static_cast<float>(band) * slab;
            const float bottom_global = std::min(row_top + row_h, band_top + slab);
            RowPiece p;
            p.page = band;
            p.box_top = top_global - band_top;
            p.content_top = p.box_top;
            p.box_bottom = bottom_global - band_top;
            p.cont_top = !pieces.empty();
            pieces.push_back(p);
            if (bottom_global >= row_top + row_h - 0.01f) break;
            off = bottom_global - row_top;
        }
        for (size_t k = 0; k + 1 < pieces.size(); ++k) pieces[k].cont_bottom = true;
        return pieces;
    };

    // 出一行：结算行高。整行放不下时先看要不要整行挪走，剩下的按行盒切片 ——
    //   切片按行盒自己的纵向位置分页，同行的几个格各归各的页。
    auto closeRow = [&]() {
        TableFrame& f = tstack.back();
        closeCell();
        if (f.row < 0) return;
        LayoutRow rs;
        if (static_cast<size_t>(f.row) < f.t->row_specs.size()) {
            rs = f.t->row_specs[static_cast<size_t>(f.row)];
        }
        const float spec_h = rs.height_pt.value_or(0.0f);
        float row_h = std::max(f.row_content_h + 2.0f * kTableCellPadYPt, spec_h);
        // 上下内边距为 0 时这条下限不能跟着变成 0：零高的行会让格线、命中与翻页判定
        //   同时退化。正常路径下每格至少有一枚段落标记，内容高不会是 0。
        if (!(row_h > 0.0f)) row_h = 1.0f;
        // 整行挪到下一页的两个条件：本页剩余空间连一行都放不下（留下一条几磅宽的
        //   碎片格线毫无意义），或者这一行不允许跨页拆开。已经在页顶时不挪 ——
        //   挪过去也放不下，那就只能溢出或者拆开。
        float first_line_h = row_h;
        if (f.line_begin < res.lines.size()) {
            first_line_h =
                res.lines[f.line_begin].block_hi_pt - res.lines[f.line_begin].block_lo_pt;
        }
        const bool allow_split = rs.can_split_by_page && !rs.page_break_before;
        const bool overflow = (f.row_top + row_h > cy + ch + 0.01f);
        const bool sliver = (cy + ch - f.row_top) < first_line_h + 2.0f * kTableCellPadYPt;
        if (!cont && (rs.page_break_before || overflow) && f.row_top > cy + 0.01f &&
            (!allow_split || sliver)) {
            shiftRowDown(f);
        }
        const float top = f.row_top;
        // 连页下行不拆：条带不是页，切开它会让表格出现分页模式才有的断口。
        std::vector<RowPiece> pieces =
            cont ? sliceRowByBand(top, row_h) : sliceRowByPage(f, top, row_h, allow_split);
        page_index = pieces.back().page;
        // 下一行的起点。分页下行的进给是"最后一片的页内下沿"，连页下是"块轴上的
        //   绝对下沿"—— 两者不能混：条带切片给的是页内坐标，拿它当块轴位置，第二行起
        //   整张表就会塌到第一条带的顶上（表看上去飞走了）。
        const float next_top = cont ? (top + row_h) : pieces.back().box_bottom;
        f.row_top = next_top;
        cursor_y = next_top;
        // 记录这一行的绘制几何：一片一条。切出来的那两条边记 cont_top / cont_bottom，
        //   绘制据此决定"这一刀要不要画线"（分页要画、连页的条带切口不画）。
        {
            PlacedTable& pt = res.placed_tables[f.placed_index];
            for (const RowPiece& piece : pieces) {
                PlacedRow pr;
                pr.row = f.row;
                pr.page = piece.page;
                pr.y = piece.box_top;
                pr.h = std::max(0.0f, piece.box_bottom - piece.box_top);
                pr.cont_top = piece.cont_top;
                pr.cont_bottom = piece.cont_bottom;
                // 把这一片段的块轴范围回填给落在该页的行盒：选区高亮要铺满整格
                //   （含 AtLeast 行高多出来的那两截空白），而行盒本身只有文字那一带。
                for (size_t j = f.line_begin; j < res.lines.size(); ++j) {
                    if (res.lines[j].page != pr.page) continue;
                    res.lines[j].cell_top_pt = pr.y;
                    res.lines[j].cell_bottom_pt = pr.y + pr.h;
                }
                pt.placed.push_back(pr);
            }
        }
        f.line_begin = res.lines.size();
        f.glyph_begin = res.glyphs.size();
        f.row = -1;
        f.row_content_h = 0.0f;
    };

    // 进一个格：摆好行内起点、可用宽度与块轴起点。跨列的格把覆盖的列宽加起来。
    auto openCell = [&](const CellSlot& slot) {
        TableFrame& f = tstack.back();
        // 先结算上一格：切格时 cursor_y 还停在它的末尾，晚一步就被下面的重置覆盖了。
        //   漏这一步的症状是"格内多行不撑开行高" —— 上一格只按一行算，第二行压到下一行表格上。
        closeCell();
        if (f.row != slot.row) {
            closeRow();
            f.row = slot.row;
        }
        int grid_col = 0;
        const LayoutCell* lc = nullptr;
        for (const LayoutCell& c : f.t->cells) {
            if (c.row != slot.row) continue;
            if (c.cell == slot.cell) {
                lc = &c;
                break;
            }
            grid_col += c.col_span;
        }
        const int span = (lc != nullptr) ? std::max(lc->col_span, 1) : 1;
        float cell_x = 0.0f;
        float cell_w = 0.0f;
        for (int c = grid_col; c < grid_col + span && c < static_cast<int>(f.col_w.size()); ++c) {
            if (c == grid_col) cell_x = f.col_x[static_cast<size_t>(c)];
            cell_w += f.col_w[static_cast<size_t>(c)];
        }
        f.cell = slot.cell;
        f.cell_cx = f.x + cell_x + kTableCellPadXPt;
        f.cell_cw = std::max(1.0f, cell_w - 2.0f * kTableCellPadXPt);
        // 外框：格线本身的位置，跨列时按覆盖的列宽加起来。
        f.cell_box_lo = f.x + cell_x;
        f.cell_box_hi = f.x + cell_x + cell_w;
        cur_cx = f.cell_cx;
        cur_cw = f.cell_cw;
        cursor_y = f.row_top + kTableCellPadYPt;
    };

    // 出表：结算最后一行，退回外层上下文（正文或外层格）。
    auto closeTable = [&]() {
        closeRow();
        tstack.pop_back();
        if (tstack.empty()) {
            cur_cx = cx;
            cur_cw = cw;
            // 表底之后让出表与下一块之间的间距（见 kTableGapAfterPt）。
            cursor_y += kTableGapAfterPt;
        } else {
            TableFrame& outer = tstack.back();
            cur_cx = outer.cell_cx;
            cur_cw = outer.cell_cw;
        }
    };

    // 断行结果的缓冲区：容量跨块保留。每段新建一个 vector 在 2000 段的文档上是 2000 次
    //   分配，而这一层对"空转 present 与段数无关"是有承诺的。
    std::vector<BrokenLine> lines;

    for (int para_i = from_para; para_i < to_para; ++para_i) {
        const size_t para_index = static_cast<size_t>(para_i);
        const BlockSpec& b = blocks[para_index];
        ParaIndex& pi = res.para_index[para_index];

        // 表格上下文切换：让 tstack 与这一块的 cell_path 对齐。逐层比对前缀，不同的层
        //   从内往外关、再按需从外往内开。表结构缺失（句柄找不到）时这一块按正文排 ——
        //   宁可位置不对，也不要拿一张错表去排。
        //
        // 切换之前先记下块轴位置：表起点（最外层表刚打开的那一块）的续排锚点必须是
        //   "表顶"，而 openTable/openCell 会把 cursor_y 推进到格内第一行 —— 用它当锚点的话，
        //   从表起点续排会把整张表下移一个上内边距（与全量重排差一个偏移，[61] 抓到的
        //   就是这个）。其余块不受影响：它们的 cursor_y 在切换前后是同一个值。
        const float cursor_before_switch = cursor_y;
        bool opened_outer = false;
        {
            const std::vector<CellSlot>& path = b.cell_path;
            size_t keep = 0;
            while (keep < path.size() && keep < tstack.size() &&
                   tstack[keep].t->handle == path[keep].table) {
                ++keep;
            }
            while (tstack.size() > keep) closeTable();
            for (size_t d = keep; d < path.size(); ++d) {
                const LayoutTable* t = tableByHandle(path[d].table);
                if (t == nullptr) break;
                openTable(*t, cursor_y);
                if (d == 0) opened_outer = true;
            }
            if (!path.empty() && path.size() == tstack.size()) {
                const TableFrame& f = tstack.back();
                if (f.row != path.back().row || f.cell != path.back().cell) {
                    openCell(path.back());
                }
            }
        }
        // 切换之后才更新：上面 closeCell 要读的是"上一块"的段后距。
        last_space_after = b.space_after_pt;

        // 续排锚点必须在任何提前返回之前记录，空段也记，否则「从第 k 段续排」在空段上无定义。
        //   表起点那一段用切换之前的块轴位置（见上面 cursor_before_switch）：从表起点续排
        //   要落在表顶，不是格内第一行。
        pi.state_at_start =
            ReflowState{page_index, opened_outer ? cursor_before_switch : cursor_y, line_total};
        pi.line_begin = static_cast<int>(res.lines.size());
        pi.glyph_begin = static_cast<int>(res.glyphs.size());
        pi.cluster_count = 0;
        pi.missing_count = 0;
        pi.font_run_count = 0;

        // 空段落：占一个空行。
        //
        //   为什么不能整段跳过（跳过是它能"最省事"的做法）：跳过之后这一段既没有行盒
        //   也没有字形，于是光标画不出来（findLineFor 找不到行盒）、用户也看不见
        //   "这里有一个空段落"。而回车站上去要的正是"换行" —— 跳过就等于回车没反应，
        //   直到敲进第一个字才突然出现一行。所以空段落必须有自己的行盒：它没有真实字形，
        //   行内只有可能存在的括号占位，而这一份几何同时给光标提供了落点。
        //
        //   与有内容的段一样参与翻页判定（空段落也是一个段落）。
        if (b.cp_text.empty()) {
            FontInstance* f = lib.instance(b.font_index, b.size_pt);
            // 取不到字体就用字号折算的近似度量：空行的几何必须存在，否则光标没地方画。
            const float ascent = f ? f->ascenderPt() : b.size_pt * 0.8f;
            const float descent = f ? f->descenderPt() : b.size_pt * 0.2f;
            if (!cont && tstack.empty() && b.page_break_before && cursor_y > cy + 0.01f) {
                ++page_index;
                cursor_y = cy;
            }
            // 空段落一样要插括号占位：回车正好把字段断在两段之间、或字段所在段被删空时，
            //   这一段里就只剩字段边界了。走的是与有内容段落同一个 withInlineObjects ——
            //   对象表只有一份，不会两边不一致。
            //
            //   只有一枚图片的段落也走这一条（它的文本确实是空的），所以这里的行高与基线
            //   要把原子的盒高算进去 —— 否则"一个图片的段落"会按空行的字号占位，
            //   图片直接盖到下一段上。
            BlockShaping empty_bs;
            empty_bs.primary = f;
            empty_bs.ascent_pt = ascent;
            empty_bs.descent_pt = descent;
            empty_bs.st.vertical = b.wm.isVertical();
            empty_bs.st.sideways = b.wm.isSideways();
            BlockShaping slot_bs;
            const BlockShaping* slotted = withInlineObjects(b, /*n_cp=*/0, &empty_bs, &slot_bs,
                                                            std::vector<FormatRun>{}, lib, cur_cw);

            float box_h = 0.0f;
            for (const BlockShaping::Cluster& c : slotted->clusters) {
                if (c.atom != InlineAtom::None) box_h = std::max(box_h, c.atom_h_pt);
            }
            // 单倍行高与有内容的段同一个基准（ascent + descent），否则同一个格式下空行
            //   会比有字的行矮一截（两者只差一个"有没有字"）。
            float natural = ascent + descent;
            if (!(natural > 0.0f)) natural = b.size_pt;
            const float line_h = std::max(natural * b.line_factor, box_h);
            const float ascent_eff = std::max(ascent, box_h);

            LineBox lb;
            lb.paragraph = static_cast<int>(para_index);
            lb.line = line_total;
            lb.page = slabOfY(cursor_y);
            // 格范围（命中用）：在格内时是这一格的内容区左右边界；正文里保持 0，表示没有格。
            //   外框另记一份（选区反白铺满整格要用它，见 LineBox 的两个 box 字段）。
            if (!tstack.empty()) {
                lb.cell_lo_pt = cur_cx;
                lb.cell_hi_pt = cur_cx + cur_cw;
                lb.cell_box_lo_pt = tstack.back().cell_box_lo;
                lb.cell_box_hi_pt = tstack.back().cell_box_hi;
            }
            lb.vertical = b.wm.isVertical();
            lb.sideways = b.wm.isSideways();
            lb.size_pt = b.size_pt;
            lb.bracket_font = f;
            lb.first_cp = 0;
            lb.last_cp = 0;
            {
                // 行盒的簇 = 那些占位簇（每个的宽度都是槽宽，原子是自己给的盒宽）。
                //   空段落没有真实字形，所以行内推进只能由占位簇给出 —— 光标落在哪、
                //   括号多宽都从这一份来。
                float ax = 0.0f;
                for (size_t i = 0; i < slotted->clusters.size(); ++i) {
                    LineCluster lc;
                    lc.cp = slotted->clusters[i].cp;
                    lc.inline_pt = ax;
                    lc.placeholder = slotted->clusters[i].placeholder;
                    lc.control_hint = slotted->clusters[i].control_hint;
                    lc.bracket_side = slotted->clusters[i].bracket_side;
                    lc.bracket_font = slotted->clusters[i].bracket_font;
                    lc.read_only = slotted->clusters[i].read_only;
                    lc.owner_control = slotted->clusters[i].owner_control;
                    lc.atom = slotted->clusters[i].atom;
                    lc.atom_h_pt = slotted->clusters[i].atom_h_pt;
                    lc.content_hash = slotted->clusters[i].content_hash;
                    lb.clusters.push_back(lc);
                    ax += slotted->cluster_adv_pt[i];
                }
                lb.inline_end_pt = ax;
            }
            if (!lb.vertical) {
                if (!cont && tstack.empty() && cursor_y + line_h > cy + ch) {
                    ++page_index;
                    cursor_y = cy;
                    lb.page = slabOfY(cursor_y);
                }
                lb.page = slabOfY(cursor_y);
                lb.block_lo_pt = locY(lb.page, cursor_y);
                lb.block_hi_pt = locY(lb.page, cursor_y + (ascent_eff + descent));
                lb.ascent_pt = ascent_eff;
                lb.descent_pt = descent;
                lb.advance_pt = line_h;
                lb.inline_origin_pt = cur_cx;
                lb.bracket_axis_pt = locY(lb.page, cursor_y + ascent_eff);
                cursor_y += line_h + b.space_after_pt;
            } else {
                const float col_w = line_h;
                if (!cont && cursor_y + col_w * 2.0f > cy + ch) {
                    ++page_index;
                    cursor_y = cy;
                }
                lb.page = slabOfY(cursor_y);
                const float center =
                    b.wm.isBlockReversed() ? cx + cw - 0.5f * col_w : cx + 0.5f * col_w;
                lb.block_lo_pt = center - col_w * 0.5f;
                lb.block_hi_pt = center + col_w * 0.5f;
                lb.inline_origin_pt = locY(lb.page, cursor_y);
                lb.bracket_axis_pt = center;
                // 竖排块在分页下吃掉本页剩余高度；连页下它占的是一条高 col_w 的横带。
                cursor_y =
                    cont ? (cursor_y + col_w + b.space_after_pt) : (cy + ch + b.space_after_pt);
            }
            res.lines.push_back(std::move(lb));
            ++line_total;
            pi.line_end = static_cast<int>(res.lines.size());
            pi.glyph_end = static_cast<int>(res.glyphs.size());
            continue;
        }
        // 取不到字体（整形为空）：只翻页、不占空间 —— 与下面 !shaped 那条分支一致。
        if (!cont && tstack.empty() && b.page_break_before && cursor_y > cy + 0.01f) {
            ++page_index;
            cursor_y = cy;
        }

        const std::vector<FormatRun> fruns = b.effectiveRuns();
        BlockShaping scratch;
        const BlockShaping* shaped = shapeParagraph(b, fruns, para_index, &scratch, &pi);
        if (!shaped) {
            // 无簇（无字体或整形为空）时必须写 {0}（仅段首一个插入点）而非空表：
            // 空表会被编辑层理解为「本段无吸附边界」，导致退格删掉半个字素簇。
            res.para_cluster_starts[para_index] = std::vector<uint32_t>{0};
            pi.line_end = static_cast<int>(res.lines.size());
            pi.glyph_end = static_cast<int>(res.glyphs.size());
            continue;
        }
        // 簇起点表用于编辑吸附；它属于纯整形，不含占位簇，所以要在插占位之前取。
        res.para_cluster_starts[para_index] = shaped->cluster_starts;

        const uint32_t n_cp = static_cast<uint32_t>(b.cp_text.size());

        // 括号占位：把块流的行内对象插成占位簇（实现见 withInlineObjects）。
        //   占位簇不产生字形、只占 advance，于是下面的断行容量、行末位置、每个簇的行内
        //   位置、光标与命中测试全都自动把括号算进去 —— 这就是"括号进文档流"的全部内容。
        //   哪些对象在这张表里，由产生端决定（引擎按模式与显示开关落成对象表），
        //   排版层不认识「聚焦」：表里有就占位。默认全部控件常显，故对象表与光标无关，
        //   光标移动不引起重排。para_cluster_starts 在此之前已生成，按约定不含占位簇。
        shaped = withInlineObjects(b, n_cp, shaped, &scratch, fruns, lib, cur_cw);

        // 从这里往下全是只读：断行、放置字形都只读整形结果。
        const BlockShaping& bs = *shaped;

        // 整段的单倍行高 × 倍数（竖排的列宽上界、首列宽度都用它）：与 breakLine 同一个
        //   基准，取块的字体度量；取不到字体时按字号估。
        const float block_natural =
            (bs.ascent_pt + bs.descent_pt > 0.0f) ? (bs.ascent_pt + bs.descent_pt) : b.size_pt;
        const float block_line_h = block_natural * b.line_factor;
        // 本行/本列末尾码点：下一个簇的首码点，最后一簇用段长
        auto lastCpOf = [&](size_t slice_end) -> uint32_t {
            return (slice_end < bs.clusters.size()) ? bs.clusters[slice_end].cp : n_cp;
        };
        // 取该码点所在 run 的颜色。颜色刻意不进整形指纹（它不改字形与推进），
        // 所以只能在这里按 run 现取 —— 若存进整形缓存，改颜色就会拿到旧值。
        auto runColorAt = [&fruns, &b](uint32_t cp) -> Color {
            const FormatRun* r = runCovering(fruns, cp);
            return r != nullptr ? r->color : b.color;
        };
        // 斜体与下划线同颜色一样不进整形指纹（它们不改字形与推进），只能在摆字形时现取。
        auto stampRunMarks = [&fruns, &b](uint32_t cp, PlacedGlyph* pg) {
            const FormatRun* r = runCovering(fruns, cp);
            if (r == nullptr) {
                pg->size_pt = b.size_pt;
                return;
            }
            pg->italic = r->italic;
            pg->underline = r->underline;
            pg->size_pt = r->size_pt;
            pg->weight = r->weight;
        };

        // 每个簇的行度量：整段算一次，横排的行循环里按行取最大值。
        //   按"簇所在 run 自己的字体与字号"取 —— 只有一个 run 时这就是块级那套值，
        //   与加 run 之前逐字段相同。相邻簇大多落在同一个 run 里，所以只在换 run 时查字体。
        std::vector<LineMetric> cluster_metric(bs.clusters.size());
        LineMetric para_metric;  // 整段的最大值：竖排的列宽必须整段一致，用不了逐行的
        {
            const FormatRun* cur = nullptr;
            LineMetric m;
            for (size_t i = 0; i < bs.clusters.size(); ++i) {
                const FormatRun* r = runCovering(fruns, bs.clusters[i].cp);
                if (r != cur) {
                    cur = r;
                    const int fi = (r != nullptr) ? r->font_index : b.font_index;
                    m = LineMetric{};
                    m.size_pt = (r != nullptr) ? r->size_pt : b.size_pt;
                    if (FontInstance* f =
                            lib.instance(fi, m.size_pt, r != nullptr ? r->weight : 400.0f)) {
                        m.ascent_pt = f->ascenderPt();
                        m.descent_pt = f->descenderPt();
                    }
                }
                cluster_metric[i] = m;
                // 原子自己一套度量：盒高就是它的 ascent（盒底坐在基线上），
                //   字号留 0 —— 盒高不是字号推出来的，混进去会让行高被行距再乘一遍。
                if (bs.clusters[i].atom != InlineAtom::None) {
                    LineMetric am;
                    am.ascent_pt = bs.clusters[i].atom_h_pt;
                    am.descent_pt = 0.0f;
                    am.box_h_pt = bs.clusters[i].atom_h_pt;
                    cluster_metric[i] = am;
                }
                para_metric.size_pt = std::max(para_metric.size_pt, cluster_metric[i].size_pt);
                para_metric.ascent_pt =
                    std::max(para_metric.ascent_pt, cluster_metric[i].ascent_pt);
                para_metric.descent_pt =
                    std::max(para_metric.descent_pt, cluster_metric[i].descent_pt);
                para_metric.box_h_pt = std::max(para_metric.box_h_pt, cluster_metric[i].box_h_pt);
            }
            // 一个度量都取不到（取不到字体）：退回块级那套，几何必须存在。
            if (para_metric.size_pt <= 0.0f) {
                para_metric =
                    LineMetric{b.size_pt, bs.ascent_pt, bs.descent_pt, para_metric.box_h_pt};
            }
        }

        if (!b.wm.isVertical()) {
            // 横排。推进的是行顶（top）而不是基线：一行里可以混排字号，基线 = 行顶 +
            //   本行 ascent，而 ascent 逐行不同 —— 拿基线累加会带着上一行的 ascent 走，
            //   翻页之后落点就错了。
            // 断行一次做完：容量是这一块的可用宽度，与页无关。分页在下面按行推进 ——
            //   两个循环各管一件事，断行的结果里没有页号，分页的循环里没有断行规则。
            breakBlock(bs, cluster_metric, cur_cw, b.size_pt, b.line_factor, &lines);
            // 落位：给这一行的行顶与基线，产出它的行盒与字形。它只认 y，不决定 y ——
            //   决定 y 的是上面的翻页判据（那是分页的事）。
            auto placeLine = [&](const BrokenLine& line, float top, float baseline) {
                // 行盒（几何的正面半边）
                LineBox lb;
                lb.paragraph = static_cast<int>(para_index);
                lb.line = line_total;
                lb.page = slabOfY(top);
                if (!tstack.empty()) {
                    lb.cell_lo_pt = cur_cx;
                    lb.cell_hi_pt = cur_cx + cur_cw;
                    lb.cell_box_lo_pt = tstack.back().cell_box_lo;
                    lb.cell_box_hi_pt = tstack.back().cell_box_hi;
                }
                lb.vertical = false;
                lb.sideways = false;
                lb.size_pt = (line.size_pt > 0.0f) ? line.size_pt : b.size_pt;
                // 行顶就是 top：写成 (top + ascent) - ascent 会多一次舍入，而"行顶"是后面
                //   增量平移唯一能逐位复现的锚点（见 LineBox::ascent_pt 的说明）。
                lb.block_lo_pt = locY(lb.page, top);
                lb.block_hi_pt = locY(lb.page, baseline + line.descent);
                lb.ascent_pt = line.ascent;
                lb.descent_pt = line.descent;
                lb.advance_pt = line.line_h;
                lb.inline_origin_pt = cur_cx;
                // 画括号用：字体与基线（横排下字形原点的 y 就是基线）。
                // 字体必须是块的主字体（= 上面算占位槽宽那个 bracketAdvancePt 的来源），
                //   换个字体画括号，字形 advance 就和槽宽对不上了。
                lb.bracket_font = bs.primary;
                lb.bracket_axis_pt = locY(lb.page, baseline);
                lb.first_cp = bs.clusters[line.begin].cp;
                lb.last_cp = lastCpOf(line.end);
                {
                    float ax = 0.0f;
                    for (size_t i = line.begin; i < line.end; ++i) {
                        LineCluster lc;
                        lc.cp = bs.clusters[i].cp;
                        lc.inline_pt = ax;
                        lc.placeholder = bs.clusters[i].placeholder;
                        lc.control_hint = bs.clusters[i].control_hint;
                        lc.bracket_side = bs.clusters[i].bracket_side;
                        lc.bracket_font = bs.clusters[i].bracket_font;
                        lc.read_only = bs.clusters[i].read_only;
                        lc.owner_control = bs.clusters[i].owner_control;
                        lc.atom = bs.clusters[i].atom;
                        lc.atom_h_pt = bs.clusters[i].atom_h_pt;
                        lc.content_hash = bs.clusters[i].content_hash;
                        lb.clusters.push_back(lc);
                        ax += bs.cluster_adv_pt[i];
                    }
                    lb.inline_end_pt = ax;
                }
                res.lines.push_back(std::move(lb));

                float pen_x = cur_cx;
                float pen_y = baseline;
                // 行内位置由簇推进，不由字形连加推进 —— 这是一处结构性修正。
                //
                //   这里是 pen_x += sg.x_advance（只累加字形），而行盒那一侧
                //   用的是 ax += bs.cluster_adv_pt[i]。两者对普通簇恰好相等
                //   （cluster_adv_pt 就是该簇字形 advance 之和），所以一直没露馅。
                //   但占位簇没有字形（g0 == g1）→ 那个内层循环一次都不执行
                //   → pen_x 在它身上纹丝不动，而 cluster_adv_pt 是括号宽度。
                //   结果：光标 / 控件矩形按"文字让开了"算，画出来的字却没让开 ——
                //   表象就是方括号压在文字上。
                //
                //   改成"每个簇开头把 pen_x 重置成 cx + ax"，两者就共用一个累加器，
                //   结构上不可能再不一致（对普通簇是恒等变换，不改任何既有结果）。
                float ax = 0.0f;
                for (size_t i = line.begin; i < line.end; ++i) {
                    pen_x = cur_cx + ax;
                    for (size_t g = bs.clusters[i].g0; g < bs.clusters[i].g1; ++g) {
                        const ShapedGlyph& sg = bs.st.glyphs[g];
                        PlacedGlyph pg;
                        pg.glyph_id = sg.glyph_id;
                        // 用该字形实际整形所用的字体（可能是回退链里的一环），
                        //   不能用块的主字体 —— 否则栅格化会去错误字体里取轮廓。
                        pg.font = bs.glyph_font[g];
                        // y_offset 的符号：OpenType/GPOS 的 yPlacement 是 y 向上，
                        //   HarfBuzz 原样写进 y_offset，所以进屏幕坐标（y 向下）要取负。
                        //   x_offset 是 x 向右，无需变号。
                        pg.x_pt = pen_x + sg.x_offset * usToPt;
                        // 横排的整行共用一个基线，所以字形跟行盒同属一条带：一条带里的
                        //   内容就是从"行顶在这一带"的那一行起。按基线取页号会让跨接缝的
                        //   那一行整行落进下一带、页内 y 为负 —— 上半截被位图裁掉，
                        //   而上一带里没有它，接缝处就成了"半行字"。
                        //   越过带底的那一截由 rasterIntoBitmap 的接缝补画。
                        const int gpage = lb.page;
                        pg.y_pt = locY(gpage, pen_y - sg.y_offset * usToPt);
                        pg.color = runColorAt(sg.cluster);
                        stampRunMarks(sg.cluster, &pg);
                        pg.page = gpage;
                        pg.line = line_total;
                        pg.vertical = false;
                        pg.rotate_deg = 0.0f;
                        pg.advance_pt = sg.x_advance * usToPt;
                        pg.cluster = sg.cluster;
                        pg.paragraph = static_cast<int>(para_index);
                        res.glyphs.push_back(pg);

                        pen_x += sg.x_advance * usToPt;
                        pen_y += sg.y_advance * usToPt;
                    }
                    ax += bs.cluster_adv_pt[i];
                }
            };

            float top = cursor_y;
            float prev_line_h = block_line_h;  // 上一行的行高：本行行顶由它推进
            bool anchor = true;                // 段首或刚翻过页：本行按 cursor_y 重新定位
            for (const BrokenLine& line : lines) {
                const float line_h = line.line_h;
                const float ascent = line.ascent;
                const float descent = line.descent;
                const float max_size = line.size_pt;

                // 本行放不下就翻页。判定必须用本行自己的高度：上一行结束时还不知道
                //   本行多高，拿上一行的高度估会漏判（下一行更高时溢出页底）。
                //   连页下这一整条短路：内容一列连到底，行照样往前排（跨条带也不断）。
                if (anchor) {
                    // 段首那一行：本段从 cursor_y 起排。
                    if (!cont && tstack.empty() && cursor_y + line_h > cy + ch) {
                        ++page_index;
                        cursor_y = cy;
                    }
                    top = cursor_y;
                    anchor = false;
                } else {
                    top += prev_line_h;
                    if (!cont && tstack.empty() && top + line_h > cy + ch) {
                        ++page_index;
                        cursor_y = cy;
                        top = cursor_y;
                    }
                }
                placeLine(line, top, top + line.ascent);
                prev_line_h = line_h;
                ++line_total;
            }
            cursor_y = top + prev_line_h + b.space_after_pt;

        } else {
            // 竖排
            // 列长固定 = 内容高度（一列到底），列宽按这一列自己的最大字号算 —— 混排字号下
            //   小字列不该占大字列的宽度（与 CSS 竖排一致：列宽由该列的行高决定）。
            //   于是列位由前缀和推进而不是乘法：中日（RTL）从右边缘往左推，蒙文（LTR）
            //   从左边缘往右推，两侧各记一个"下一列的边缘"。
            //   列宽要等这一列的内容定下来才知道，而列长与列宽无关（列长就是内容高度），
            //   所以"先取内容、再算宽度、再落列位"没有循环依赖。
            //   col_w_max 是列宽的上界，只用于分页估计；基准与 breakLine 相同。
            const float para_natural = (para_metric.ascent_pt + para_metric.descent_pt > 0.0f)
                                           ? (para_metric.ascent_pt + para_metric.descent_pt)
                                           : para_metric.size_pt;
            const float col_w_max = para_natural * b.line_factor;
            float region_top = cursor_y;
            // 分页下要判"本页还放得下两条列位吗"；连页下没有页底，横带是新开的。
            if (!cont && region_top + col_w_max * 2.0f > cy + ch) {
                ++page_index;
                cursor_y = cy;
                region_top = cy;
            }

            const bool rtl = b.wm.isBlockReversed();
            int col = 0;
            float pen_y = region_top;
            float next_edge = rtl ? (cx + cw) : cx;  // 下一列的起始边缘
            float col_w = col_w_max;                 // 本列宽度，进循环后按内容改写

            // 换一条横带：列位用尽则另起一条（分页下就是翻页）。
            //   连页下横带的长度取内容高度 ch（竖排的列长本来就是固定的一列），
            //   下一条带排在它下面 —— 与分页下"另起一页"是同一个位置关系。
            auto nextBand = [&]() {
                if (cont) {
                    region_top += ch;
                } else {
                    ++page_index;
                    region_top = cy;
                }
                col = 0;
                next_edge = rtl ? (cx + cw) : cx;
                pen_y = region_top;
            };

            // 侧倒（蒙文）：横排成型 + 每个字形顺时针转 90°。
            //   局部坐标映射：横排下屏幕偏移 = (x_offset, -y_offset)；
            //   侧倒即把内容顺时针转 90°，(u, -v) ↦ (v, u)。于是
            //       屏幕偏移 = (y_offset, x_offset)
            //   笔的定位：v 轴（-descent … +ascender）现在是水平方向，
            //   要让字身在列里居中 → 基线 x = 列中线 - (ascender - descent)/2。
            const bool sideways = bs.st.sideways;

            // 落位：给这一列的列心、列宽与列顶，产出它的行盒与字形。它只认几何，不决定
            //   几何 —— 换带（分页）在上面那个判据里。
            auto placeColumn = [&](const BrokenLine& col_line, float col_center, float col_w,
                                   float pen_y) {
                const float col_asc = col_line.ascent;
                const float col_desc = col_line.descent;
                // 列内横轴的起始笔位：直立竖排 = 列心；侧倒 = 列心减去半个字身
                // （理由见上面那段"笔的定位"注释）。
                // 名字里带 0：它只是"这一列的起点"，字形放置时那个 gx 会往前推进
                //   （直立竖排下每画完一个字形 gx += x_advance）。
                const float gx0 =
                    sideways ? (col_center - (col_asc - col_desc) * 0.5f) : col_center;

                // 列盒（竖排下的行盒）
                LineBox lb;
                lb.paragraph = static_cast<int>(para_index);
                lb.line = line_total + col;
                lb.page = slabOfY(pen_y);
                if (!tstack.empty()) {
                    lb.cell_lo_pt = cur_cx;
                    lb.cell_hi_pt = cur_cx + cur_cw;
                    lb.cell_box_lo_pt = tstack.back().cell_box_lo;
                    lb.cell_box_hi_pt = tstack.back().cell_box_hi;
                }
                lb.vertical = true;
                lb.sideways = sideways;
                lb.size_pt = col_line.size_pt;
                lb.block_lo_pt = col_center - col_w * 0.5f;
                lb.block_hi_pt = col_center + col_w * 0.5f;
                lb.inline_origin_pt = locY(lb.page, pen_y);
                // 画括号用：字体 + 竖排下的列内横轴起始笔位。
                lb.bracket_font = bs.primary;
                lb.bracket_axis_pt = gx0;
                lb.first_cp = bs.clusters[col_line.begin].cp;
                lb.last_cp = lastCpOf(col_line.end);
                {
                    float ay = 0.0f;
                    for (size_t i = col_line.begin; i < col_line.end; ++i) {
                        LineCluster lc;
                        lc.cp = bs.clusters[i].cp;
                        lc.inline_pt = ay;
                        lc.placeholder = bs.clusters[i].placeholder;
                        lc.control_hint = bs.clusters[i].control_hint;
                        lc.bracket_side = bs.clusters[i].bracket_side;
                        lc.bracket_font = bs.clusters[i].bracket_font;
                        lc.read_only = bs.clusters[i].read_only;
                        lc.owner_control = bs.clusters[i].owner_control;
                        lc.atom = bs.clusters[i].atom;
                        lc.atom_h_pt = bs.clusters[i].atom_h_pt;
                        lc.content_hash = bs.clusters[i].content_hash;
                        lb.clusters.push_back(lc);
                        ay += bs.cluster_adv_pt[i];
                    }
                    lb.inline_end_pt = ay;
                }
                res.lines.push_back(std::move(lb));

                const int line_no = line_total + col;
                float gx = gx0;
                float gy = pen_y;
                // 同一个修正，见上面横排那段的长注释：行内位置由簇推进。
                //   竖排的行内轴是 y，所以这里重置的是 gy（gx 是列内横轴，不参与）。
                //   横排/竖排两条路都栽在同一处 —— 占位簇没有字形，字形连加推不动它。
                float ay = 0.0f;
                for (size_t i = col_line.begin; i < col_line.end; ++i) {
                    gy = pen_y + ay;
                    for (size_t g = bs.clusters[i].g0; g < bs.clusters[i].g1; ++g) {
                        const ShapedGlyph& sg = bs.st.glyphs[g];
                        PlacedGlyph pg;
                        pg.glyph_id = sg.glyph_id;
                        pg.font = bs.glyph_font[g];
                        if (sideways) {
                            pg.x_pt = gx + sg.y_offset * usToPt;
                            pg.y_pt = gy + sg.x_offset * usToPt;
                            pg.rotate_deg = 90.0f;
                            pg.advance_pt = sg.y_advance * usToPt;
                        } else {
                            // 非侧倒走 HarfBuzz 的 TTB：它已自行减掉竖排原点
                            // （hb-ot-shape.cc: subtract_glyph_v_origins）。
                            // y_offset 是向上为正，进屏幕坐标要取负；
                            //   验算：v_origin.y=14.08pt → y_offset=-14.08pt，
                            //   取负后基线落在列顶下方 14.08pt，墨迹正好从列顶下 1.15pt 起。
                            pg.x_pt = gx + sg.x_offset * usToPt;
                            pg.y_pt = gy - sg.y_offset * usToPt;
                            pg.rotate_deg = 0.0f;
                        }
                        const int gpage = slabOfY(gy);
                        pg.color = runColorAt(sg.cluster);
                        stampRunMarks(sg.cluster, &pg);
                        pg.page = gpage;
                        pg.line = line_no;
                        pg.vertical = true;
                        pg.cluster = sg.cluster;
                        pg.paragraph = static_cast<int>(para_index);
                        // 竖排的行内轴是 y，所以页内坐标只改 y（x 是列内横轴，不过 locY）。
                        pg.y_pt = locY(gpage, pg.y_pt);
                        res.glyphs.push_back(pg);

                        if (sideways) {
                            // 行内推进量取自 hmtx 的 x_advance（横排度量），沿列向下
                            gy += sg.x_advance * usToPt;
                        } else {
                            gx += sg.x_advance * usToPt;
                            // 竖排向下推进：y_advance 为负，取反才是"向下"的步长
                            gy += -sg.y_advance * usToPt;
                        }
                    }
                    ay += bs.cluster_adv_pt[i];
                }
            };

            size_t start = 0;
            while (start < bs.clusters.size()) {
                float avail = (cy + ch) - pen_y;
                if (avail < para_metric.size_pt) {
                    nextBand();
                    avail = (cy + ch) - pen_y;
                }

                // 一列一列地断：容量是这一条带剩下的高度（换带之后重算），所以这里只断
                //   一列。列宽（竖排下的"行高"）由 breakLine 算 —— 与横排同一个式子。
                const BrokenLine col_line =
                    breakLine(bs, cluster_metric, start, avail, b.size_pt, b.line_factor);
                col_w = col_line.line_h;

                // 本列在剩余宽度里放不下就换一条带。这一步只能放在"这一列的内容定下来
                //   之后"：列宽是逐列算出来的，事先不知道。列长与列宽无关，换带之后同一段
                //   内容仍然装得下（avail 不变），所以不必重新取。
                //   第一列例外：放不下也照排，否则永远排不进去。
                const float edge_after = rtl ? (next_edge - col_w) : (next_edge + col_w);
                const bool overflow =
                    rtl ? (edge_after < cx - 0.01f) : (edge_after > cx + cw + 0.01f);
                if (col > 0 && overflow) nextBand();

                const float col_center =
                    rtl ? (next_edge - col_w * 0.5f) : (next_edge + col_w * 0.5f);
                placeColumn(col_line, col_center, col_w, pen_y);
                start = col_line.end;
                if (start < bs.clusters.size()) {
                    next_edge = rtl ? (next_edge - col_w) : (next_edge + col_w);
                    ++col;
                    pen_y = region_top;
                }
            }
            line_total += col + 1;
            // 竖排块在分页下吃掉本页剩余高度（后续块另起一页）；连页下它占的是
            //   若干条长 ch 的横带，所以下一条从最后那条带的底下开始。
            cursor_y = cont ? (region_top + ch + b.space_after_pt) : (cy + ch + b.space_after_pt);
        }

        pi.line_end = static_cast<int>(res.lines.size());
        pi.glyph_end = static_cast<int>(res.glyphs.size());
    }

    // 收尾：文档以表格结尾时最后一行还没结算。出表的顺序与入表相反，逐层关到底。
    while (!tstack.empty()) closeTable();

    // 汇总量按 para_index 逐段求和（而非只统计本轮）：前缀可能复用自上一轮，
    // 只数本轮会让 page_count / glyph_count / placed_line_count 变成半篇，
    // 而这些数字是帧高、翻页与面板的直接判据。
    summarizeLayoutStats(&res);
    // 连页下页号是派生的：把收尾那一处的条带号写回状态，否则续排从这里开始时
    //   page_index 还停在上一段的值（ReflowState 的相等判据会因此恒不等，增量续排白跑）。
    if (cont) page_index = slabOfY(cursor_y);
    res.stats.page_count = page_index + 1;
    // shape_ms 只计本轮：命中缓存的段不产生整形耗时，本轮耗时才是关心的量。
    res.stats.shape_ms = shape_ms;
    res.stats.layout_ms = msSince(t_layout0);
    res.rebuildPageIndex();
    return ReflowState{page_index, cursor_y, line_total};
}

void LayoutResult::rebuildPageIndex() {
    const size_t n = static_cast<size_t>(std::max(1, stats.page_count));
    page_lines.assign(n, {});
    page_glyphs.assign(n, {});
    // 先数一遍、按页预留，再填第二遍：每页一个 vector，边推边扩容会反复搬元素，
    //   而这两张表每敲一个字都要重建一次（4000 行 / 4.5 万字形那个量级）。
    std::vector<int> glyph_count(n, 0);
    for (const PlacedGlyph& g : glyphs) {
        if (g.page < 0 || static_cast<size_t>(g.page) >= n) continue;
        ++glyph_count[static_cast<size_t>(g.page)];
    }
    for (size_t p = 0; p < n; ++p) {
        if (glyph_count[p] > 0) page_glyphs[p].reserve(static_cast<size_t>(glyph_count[p]));
    }
    for (size_t i = 0; i < glyphs.size(); ++i) {
        const int p = glyphs[i].page;
        if (p < 0 || static_cast<size_t>(p) >= n) continue;
        page_glyphs[static_cast<size_t>(p)].push_back(static_cast<int>(i));
    }
    // 行数比字形少一个量级，直接推。
    for (size_t i = 0; i < lines.size(); ++i) {
        const int p = lines[i].page;
        if (p < 0 || static_cast<size_t>(p) >= n) continue;
        page_lines[static_cast<size_t>(p)].push_back(static_cast<int>(i));
    }
}

const std::vector<int>& LayoutResult::linesOnPage(int page) const {
    static const std::vector<int> kNone;
    if (page < 0 || static_cast<size_t>(page) >= page_lines.size()) return kNone;
    return page_lines[static_cast<size_t>(page)];
}

const std::vector<int>& LayoutResult::glyphsOnPage(int page) const {
    static const std::vector<int> kNone;
    if (page < 0 || static_cast<size_t>(page) >= page_glyphs.size()) return kNone;
    return page_glyphs[static_cast<size_t>(page)];
}

void summarizeLayoutStats(LayoutResult* out) {
    if (!out) return;
    int glyphs = 0, lines = 0, clusters = 0, missing = 0, runs = 0;
    for (const ParaIndex& p : out->para_index) {
        glyphs += p.glyph_end - p.glyph_begin;
        lines += p.line_end - p.line_begin;
        clusters += p.cluster_count;
        missing += p.missing_count;
        runs += p.font_run_count;
    }
    out->stats.glyph_count = glyphs;
    out->stats.placed_line_count = lines;
    out->stats.cluster_count = clusters;
    out->stats.missing_glyph_count = missing;
    out->stats.font_run_count = runs;
}

bool FormatRun::sameFormat(const FormatRun& o) const {
    return font_index == o.font_index && size_pt == o.size_pt && weight == o.weight &&
           italic == o.italic && underline == o.underline && color == o.color &&
           BlockSpec::sameLanguage(language, o.language) &&
           fallback_font_indices == o.fallback_font_indices;
}

const LayoutCell* PlacedTable::cellAt(int row, int col, int* col_lo, int* col_hi) const {
    if (col < 0 || col >= cols) return nullptr;
    int grid = 0;
    int cur_row = -1;
    for (const LayoutCell& c : cells) {
        // 列号按"同一行的格依次累加跨度"得到的，换行就归零。它与排版进格时的累加
        // 是同一套（见 layout.cpp 的 openCell）：两处口径分叉会让命中落在隔壁列上。
        if (c.row != cur_row) {
            cur_row = c.row;
            grid = 0;
        }
        const int lo = grid;
        grid += std::max(c.col_span, 1);
        if (col < lo || col >= grid) continue;
        // 跨行的格：它属于起点那一行，但覆盖到的每一行都该命中它 —— 否则合并格的下半截
        // 点不中，而那里并没有别的格可以接。
        if (row < c.row || row >= c.row + std::max(c.row_span, 1)) continue;
        if (col_lo) *col_lo = lo;
        if (col_hi) *col_hi = grid;
        return &c;
    }
    return nullptr;
}

bool PlacedTable::cellBoxOnPage(const LayoutCell& cell, int page, float* x, float* y, float* w,
                                float* h) const {
    // 列号要在"这一格所在的那一行"里累加跨度才得到；格号相同但行不同的格是另一个格。
    int grid = 0;
    int col_lo = -1;
    for (const LayoutCell& c : cells) {
        if (c.row != cell.row) continue;
        if (c.cell == cell.cell) {
            col_lo = grid;
            break;
        }
        grid += std::max(c.col_span, 1);
    }
    if (col_lo < 0) return false;
    const int col_hi = col_lo + std::max(cell.col_span, 1);
    if (col_hi >= static_cast<int>(col_x.size())) return false;
    float top = 0.0f;
    float bottom = 0.0f;
    bool seen = false;
    for (const PlacedRow& pr : placed) {
        if (pr.row != cell.row || pr.page != page) continue;
        if (!seen) {
            top = pr.y;
            bottom = pr.y + pr.h;
            seen = true;
        } else {
            top = std::min(top, pr.y);
            bottom = std::max(bottom, pr.y + pr.h);
        }
    }
    if (!seen) return false;
    if (x) *x = this->x + col_x[static_cast<size_t>(col_lo)];
    if (w) *w = col_x[static_cast<size_t>(col_hi)] - col_x[static_cast<size_t>(col_lo)];
    if (y) *y = top;
    if (h) *h = bottom - top;
    return true;
}

}  // namespace heditor
