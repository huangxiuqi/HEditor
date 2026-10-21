#include "core/layout/layout.h"

#include <hb.h>

#include <algorithm>
#include <chrono>

#include "core/text/fallback.h"
#include "core/text/shaper.h"
#include "core/utils/hash.h"
#include "core/utils/utf8.h"

namespace heditor {
namespace {

using Clock = std::chrono::steady_clock;

/**
 * 方括号在某个字体下的 advance（磅）。返回 0 表示该字体没有方括号字形 ——
 * 那就不留位（宁可没有空隙，也不要留一个宽度却画不出东西）。
 * 用块的主字体算：一个块内的字号一致，而括号只是一对细笔画，
 * 不值得为"块里混了几种字体"再去逐段取值。
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
         * 只读字段的边界（决定括号画成灰色）。只有占位簇有意义。
         */
        bool read_only = false;
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
        h = fnvMix(h, static_cast<uint64_t>(fr.fallback_font_indices.size()));
        for (int fi : fr.fallback_font_indices)
            h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(fi)));
        h = fnvMixCStr(h, fr.language);
    }
    return h;
}

/**
 * 把块流的行内对象插成占位簇（括号与行内原子进文档流的全部实现）。
 * 对象从哪来、该不该显示、谁在外谁在内，全在产生端（见 BlockSpec::objects）；排版层只做
 * 一次线性归并（表已升序，同一处边界上的先后就是数组里的先后，不做第二次排序）。
 *
 * 宽度来源两种：控件边界 = 括号字形 advance + 间隙，原子 = 产生端给的盒宽。占位簇
 * g0 == g1，不产生字形只占 advance，于是断行容量、行末位置、簇的行内位置、光标与命中
 * 测试全都自动算进去。原子之后可以断（它与一个全角字同价），括号之后不断。
 *
 * shaped 可能正指着只读的段级缓存条目，需要可改副本时用它旁边的 scratch。
 */
const BlockShaping* withInlineObjects(const BlockSpec& b, uint32_t n_cp, const BlockShaping* shaped,
                                      BlockShaping* scratch) {
    if (b.objects.empty()) return shaped;
    const BlockShaping& src = *shaped;
    // 槽宽 = 括号字形 advance + 间隙。间隙必须计入排版，否则 字] 会挤在一起。
    const float bw = bracketAdvancePt(src.primary);
    const float slot = bw > 0.0f ? bw + kControlBracketSlotGapPt : 0.0f;
    // 一个都插不进去（取不到括号字形，表里又只有控件边界）就原样返回。
    //   原子不走这一条：它的宽度是自己给的，没有字体照样排得出。
    bool has_atom = false;
    for (const InlineObject& o : b.objects) {
        if (o.kind == InlineObjectKind::Atom) {
            has_atom = true;
            break;
        }
    }
    if (!(slot > 0.0f) && !has_atom) return shaped;

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
    auto emitSlot = [&](const InlineObject& o, uint32_t cp) {
        BlockShaping::Cluster pc;
        pc.cp = cp;
        pc.g0 = pc.g1 = 0;
        pc.placeholder = true;
        pc.owner_control = o.owner;
        if (o.kind == InlineObjectKind::Atom) {
            pc.atom = o.atom;
            pc.atom_h_pt = o.h_pt;
            pc.content_hash = o.content_hash;
            cl2.push_back(pc);
            adv2.push_back(o.w_pt);
            // 盒子之后可以断：它与一个全角字同价，断在它后面不拆开任何东西。
            brk2.push_back(true);
            return;
        }
        pc.bracket_side =
            (o.kind == InlineObjectKind::ControlStart) ? BracketSide::Open : BracketSide::Close;
        pc.read_only = o.read_only;
        cl2.push_back(pc);
        adv2.push_back(slot);
        brk2.push_back(false);  // 括号内部不是断点
    };
    size_t oi = 0;
    for (size_t i = 0; i <= scratch->clusters.size(); ++i) {
        const uint32_t at = (i < scratch->clusters.size()) ? scratch->clusters[i].cp : n_cp;
        while (oi < b.objects.size() && b.objects[oi].cp <= at) emitSlot(b.objects[oi++], at);
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
    for (; oi < b.objects.size(); ++oi) emitSlot(b.objects[oi], n_cp);
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
 * 行高 = 行距（字号 × 倍数），但行里塞进比文本更高的东西（原子盒）时，撑高的是内容，
 * 行距本身要留着：max(行距, 内容高 + 行距 - 文本高)。纯文本行两项相等 ⇒ 就是行距；
 * 原子行 ⇒ 盒高 + 行距。只取 max(行距, 盒高) 是错的：那样盒底下面一点空隙都没有，
 * 下一行的字紧贴着盒底排（实测相邻两行的行盒还重叠 3.5 磅）—— 看起来就是"那一行的行距
 * 没了"。行距是段落属性，不该被行里的一枚盒子吃掉。
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
    // 文本自己的度量（不含原子）：行距是"文本行之间的距离"，它由字号定 ——
    // 算行高时要把它和"内容撑出来的高度"分开，见上面那段。
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

    float text_extent = text_ascent + text_descent;
    if (!(text_extent > 0.0f)) text_extent = bs.ascent_pt + bs.descent_pt;
    const float pitch = ((max_size > 0.0f) ? max_size : block_size_pt) * line_factor;
    const float leading = std::max(0.0f, pitch - text_extent);

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

    const float cx = page.margin_pt;
    const float cy = page.margin_pt;
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
                FontInstance* rf = font;
                if (r.face_index >= 0) {
                    FontInstance* hit = lib.instance(r.face_index, fr.size_pt);
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
            f.row_content_h = std::max(
                f.row_content_h, cursor_y - f.row_top - kTableCellPaddingPt - last_space_after);
        }
        f.cell = -1;
    };

    // 把 [from_line, 末尾) 的行盒与对应的字形整体挪到下一页的 dest_top。
    //   字形用 line 号筛：行号是排版顺序递增的，所以"这一行及以后"的行盒与字形是同一个后缀。
    auto moveToNextPage = [&](size_t from_line, float dest_top) {
        const float dy = dest_top - res.lines[from_line].block_lo_pt;
        const int line_no = res.lines[from_line].line;
        for (size_t j = from_line; j < res.lines.size(); ++j) {
            LineBox& lb = res.lines[j];
            lb.block_lo_pt += dy;
            lb.block_hi_pt += dy;
            lb.bracket_axis_pt += dy;
            lb.page += 1;
        }
        for (PlacedGlyph& g : res.glyphs) {
            if (g.line < line_no) continue;
            g.y_pt += dy;
            g.page += 1;
        }
        ++page_index;
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

    // 按行盒切片：本页装得下的留在本页，装不下的挪到下一页再试。切点落在行与行之间，
    //   一行文字本身不被切开。合并单元格跨页时内容重复显示是绘制层的事（这里只保证
    //   下半片段的内容真的到了下一页）。
    auto splitRow = [&](TableFrame& f) {
        size_t k = f.line_begin;
        while (k < res.lines.size()) {
            size_t bad = res.lines.size();
            for (size_t j = k; j < res.lines.size(); ++j) {
                if (res.lines[j].block_hi_pt > cy + ch + 0.01f) {
                    bad = j;
                    break;
                }
            }
            if (bad >= res.lines.size()) break;
            // 本片段第一行就放不下：整片挪走。挪完仍放不下说明这一行比整页还高，
            //   不再挪第二次（否则会一直翻页），让它溢出。
            const bool first = (bad == k);
            moveToNextPage(bad, cy + kTableCellPaddingPt);
            if (first && res.lines[k].block_hi_pt > cy + ch + 0.01f) break;
            k = bad;
        }
        // 行底只看最后一个页片段：跨页之后各页的 y 是各自的页内坐标，混在一起比没有意义。
        int last_page = 0;
        for (size_t j = f.line_begin; j < res.lines.size(); ++j) {
            last_page = std::max(last_page, res.lines[j].page);
        }
        float bottom = cy;
        for (size_t j = f.line_begin; j < res.lines.size(); ++j) {
            if (res.lines[j].page != last_page) continue;
            bottom = std::max(bottom, res.lines[j].block_hi_pt);
        }
        f.row_top = bottom + kTableCellPaddingPt;
        cursor_y = f.row_top;
    };

    // 出一行：结算行高。整行放不下时按行记录的开关分两种走法 —— 允许跨页断行的按行盒切片，
    //   不允许的整行挪到下一页。
    auto closeRow = [&]() {
        TableFrame& f = tstack.back();
        closeCell();
        if (f.row < 0) return;
        LayoutRow rs;
        if (static_cast<size_t>(f.row) < f.t->row_specs.size()) {
            rs = f.t->row_specs[static_cast<size_t>(f.row)];
        }
        const float spec_h = rs.height_pt.value_or(0.0f);
        float row_h = std::max(f.row_content_h + 2.0f * kTableCellPaddingPt, spec_h);
        if (row_h <= 0.0f) row_h = 2.0f * kTableCellPaddingPt;
        // 放置前的页号与行顶：行被整体挪走或拆开时，第一片段落在哪一页要用这两个值判。
        const int page_before = page_index;
        const float top_before = f.row_top;
        const bool overflow = (f.row_top + row_h > cy + ch + 0.01f);
        bool placed = false;
        if (!cont && (rs.page_break_before || overflow) && f.row_top > cy + 0.01f) {
            if (rs.can_split_by_page && !rs.page_break_before) {
                splitRow(f);
                placed = true;
            } else {
                shiftRowDown(f);
            }
        }
        if (!placed) {
            f.row_top += row_h;
            cursor_y = f.row_top;
        }
        // 记录这一行的绘制几何：按页给出片段，切点那两条边不画。空格没有行盒，
        //   整行按放置后的位置记一条。
        {
            PlacedTable& pt = res.placed_tables[f.placed_index];
            struct PageSpan {
                int page = 0;
                float lo = 0.0f;
                float hi = 0.0f;
            };
            std::vector<PageSpan> spans;
            for (size_t j = f.line_begin; j < res.lines.size(); ++j) {
                const LineBox& lb = res.lines[j];
                bool found = false;
                for (PageSpan& s : spans) {
                    if (s.page != lb.page) continue;
                    s.lo = std::min(s.lo, lb.block_lo_pt);
                    s.hi = std::max(s.hi, lb.block_hi_pt);
                    found = true;
                    break;
                }
                if (!found) spans.push_back(PageSpan{lb.page, lb.block_lo_pt, lb.block_hi_pt});
            }
            std::sort(spans.begin(), spans.end(),
                      [](const PageSpan& a, const PageSpan& b) { return a.page < b.page; });
            if (spans.empty()) {
                PlacedRow pr;
                pr.row = f.row;
                pr.page = page_index;
                // 空格没有行盒：整行按"放置前后"的实际范围记。
                pr.y = (page_index == page_before) ? top_before : cy;
                pr.h = std::max(0.0f, f.row_top - pr.y);
                pt.placed.push_back(pr);
            } else {
                // 行矩形按"这一行实际占的高度"给，不是按行盒的范围：显式行高（AtLeast）
                //   比内容高时，中间那段空白也要有格线，否则一行看起来断成两块。
                //   跨页拆开时按页切：首片段到页底、中间片段占满内容区、末片段到行底。
                for (size_t i = 0; i < spans.size(); ++i) {
                    PlacedRow pr;
                    pr.row = f.row;
                    pr.page = spans[i].page;
                    if (i == 0) {
                        pr.y = (spans[i].page == page_before) ? top_before : cy;
                    } else {
                        pr.y = cy;
                    }
                    const float bottom = (i + 1 == spans.size()) ? f.row_top : (cy + ch);
                    pr.h = std::max(0.0f, bottom - pr.y);
                    pr.cont_top = (i > 0);
                    pr.cont_bottom = (i + 1 < spans.size());
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
        f.cell_cx = f.x + cell_x + kTableCellPaddingPt;
        f.cell_cw = std::max(1.0f, cell_w - 2.0f * kTableCellPaddingPt);
        // 外框：格线本身的位置，跨列时按覆盖的列宽加起来。
        f.cell_box_lo = f.x + cell_x;
        f.cell_box_hi = f.x + cell_x + cell_w;
        cur_cx = f.cell_cx;
        cur_cw = f.cell_cw;
        cursor_y = f.row_top + kTableCellPaddingPt;
    };

    // 出表：结算最后一行，退回外层上下文（正文或外层格）。
    auto closeTable = [&]() {
        closeRow();
        tstack.pop_back();
        if (tstack.empty()) {
            cur_cx = cx;
            cur_cw = cw;
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
        //   从表起点续排会把整张表下移一个内边距（与全量重排差一个 padding，[61] 抓到的
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
            const BlockShaping* slotted = withInlineObjects(b, /*n_cp=*/0, &empty_bs, &slot_bs);

            float box_h = 0.0f;
            for (const BlockShaping::Cluster& c : slotted->clusters) {
                if (c.atom != InlineAtom::None) box_h = std::max(box_h, c.atom_h_pt);
            }
            const float line_h = std::max(b.size_pt * b.line_factor, box_h);
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
                    lc.bracket_side = slotted->clusters[i].bracket_side;
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
        shaped = withInlineObjects(b, n_cp, shaped, &scratch);

        // 从这里往下全是只读：断行、放置字形都只读整形结果。
        const BlockShaping& bs = *shaped;

        const float block_line_h = b.size_pt * b.line_factor;
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
                    if (FontInstance* f = lib.instance(fi, m.size_pt)) {
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
                        lc.bracket_side = bs.clusters[i].bracket_side;
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
                        pg.page = gpage;
                        pg.line = line_total;
                        pg.vertical = false;
                        pg.rotate_deg = 0.0f;
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
            //   col_w_max 是列宽的上界，只用于分页估计。
            const float col_w_max = para_metric.size_pt * b.line_factor;
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
                        lc.bracket_side = bs.clusters[i].bracket_side;
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
    return ReflowState{page_index, cursor_y, line_total};
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
    return font_index == o.font_index && size_pt == o.size_pt && color == o.color &&
           BlockSpec::sameLanguage(language, o.language) &&
           fallback_font_indices == o.fallback_font_indices;
}

}  // namespace heditor
