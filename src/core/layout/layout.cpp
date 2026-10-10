#include "core/layout/layout.h"

#include <hb.h>

#include <algorithm>
#include <chrono>

#include "core/hash.h"
#include "core/text/fallback.h"
#include "core/text/shaper.h"
#include "core/utf8.h"

namespace ced {
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
 * @note 刻意不含页宽页高、光标、括号占位计划、颜色、行高与段后距 —— 它们只影响落行
 * 或绘制。因此改页宽、切换聚焦字段之后整形缓存依然命中。
 * 浮点按位混入（不量化）：量化会把 12.0 与 12.001 判成同一份缓存。
 */
uint64_t shapingFingerprint(const BlockSpec& b) {
    uint64_t h = kFnvBasis;
    h = fnvMixU32s(h, b.cp_text);
    h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(b.font_index)));
    h = fnvMixFloat(h, b.size_pt);
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.inline_axis)));
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.block_flow)));
    h = fnvMix(h, static_cast<uint64_t>(static_cast<int>(b.wm.orientation)));
    h = fnvMix(h, static_cast<uint64_t>(b.fallback_font_indices.size()));
    for (int fi : b.fallback_font_indices)
        h = fnvMix(h, static_cast<uint64_t>(static_cast<uint32_t>(fi)));
    h = fnvMixCStr(h, b.language);
    return h;
}

/**
 * 把"要显示括号的控件"的边界插成占位簇（括号进文档流的全部实现）。
 *
 * 三条约定，整个表单标记体系都建立在这上面：
 *   槽宽 = 括号字形 advance + 间隙，且与绘制侧共用同一个常数（见 kControlBracketSlotGapPt）；
 *   同一处边界上的多个槽按"槽属于谁"排序 —— 内容结束处的闭括号(0) → 开括号(1) →
 *     空字段自身的闭括号(2)，于是相邻字段得 ][、空字段得 []、同区间嵌套得 [[ ]]；
 *   占位簇 g0 == g1（不产生字形、只占 advance），于是断行容量、行末位置、每个簇的行内
 *     位置、光标与命中测试全都自动把括号算进去。
 *
 * @param shaped 纯整形结果（可能正指着只读的段级缓存条目）；没有槽要插时原样返回它
 * @param scratch 需要可改副本时就地用它（未命中缓存时它已持有整形结果，省一次拷贝）
 */
const BlockShaping* withBracketSlots(const ControlSet* controls, const std::vector<int>* bracketed,
                                     size_t para_index, uint32_t n_cp, const BlockShaping* shaped,
                                     BlockShaping* scratch) {
    if (!controls || !bracketed || bracketed->empty()) return shaped;
    const BlockShaping& src = *shaped;
    // 槽宽 = 括号字形 advance + 间隙。间隙必须计入排版，否则 字] 会挤在一起。
    const float bw = bracketAdvancePt(src.primary);
    const float slot = bw > 0.0f ? bw + kControlBracketSlotGapPt : 0.0f;
    if (!(slot > 0.0f)) return shaped;

    // 同一处边界（簇 i 之前）可能同时挂着多个槽，来自不同字段：
    // 某字段在此结束（闭括号）、某字段在此开始（开括号）、空字段两者皆有。
    struct Slot {
        BracketSide side;
        bool ro;  // 只读字段的括号画灰色（颜色只有插槽这一层知道）
        int prio;
    };
    std::vector<std::vector<Slot>> at(src.clusters.size() + 1);
    // 段内偏移 → 插入点（第一个 cp >= offset 的簇；没有就插在末尾）。
    auto slotIndex = [&](size_t offset) {
        for (size_t i = 0; i < src.clusters.size(); ++i) {
            if (src.clusters[i].cp >= offset) return i;
        }
        return src.clusters.size();
    };
    for (int ci : *bracketed) {
        if (ci < 0 || ci >= controls->size()) continue;
        const Control& fc = controls->at(ci);
        // 空字段：开、闭落在同一处，且要画成 []（不是 ][）。
        const bool empty_field = (fc.range.start == fc.range.end);
        if (fc.range.end.para == static_cast<int>(para_index)) {
            at[slotIndex(fc.range.end.offset)].push_back(
                Slot{BracketSide::Close, fc.read_only, empty_field ? 2 : 0});
        }
        if (fc.range.start.para == static_cast<int>(para_index)) {
            at[slotIndex(fc.range.start.offset)].push_back(
                Slot{BracketSide::Open, fc.read_only, 1});
        }
    }
    bool any = false;
    for (const std::vector<Slot>& v : at) {
        if (!v.empty()) {
            any = true;
            break;
        }
    }
    if (!any) return shaped;

    // 这一段要插占位 → 簇序列会被改 → 需要一份可改的副本
    // （shaped 可能正指着只读的缓存条目）。
    if (shaped != scratch) *scratch = src;
    std::vector<BlockShaping::Cluster> cl2;
    std::vector<float> adv2;
    std::vector<bool> brk2;
    cl2.reserve(scratch->clusters.size() + 4);
    adv2.reserve(scratch->clusters.size() + 4);
    brk2.reserve(scratch->clusters.size() + 4);
    // 在簇 i 前面插一个占位槽（side = 开/闭，ro = 只读）。
    auto emitSlot = [&](size_t i, BracketSide side, bool ro) {
        BlockShaping::Cluster pc;
        // 占位簇的 cp = 它后面那个真实簇的 cp（末尾用段长哨兵）。这让"光标该落在槽的
        // 哪一侧"归 inlineOffsetFor 判定，判据是 bracket_side —— 光靠 cp 是分不开的。
        pc.cp = (i < scratch->clusters.size()) ? scratch->clusters[i].cp : n_cp;
        pc.g0 = pc.g1 = 0;
        pc.placeholder = true;
        pc.bracket_side = side;
        pc.read_only = ro;
        cl2.push_back(pc);
        adv2.push_back(slot);
        brk2.push_back(false);  // 括号内部不是断点
    };
    for (size_t i = 0; i <= scratch->clusters.size(); ++i) {
        // 按 prio 排序（见上）。stable_sort 保证同优先级内保持控件列表顺序，结果确定可复现。
        std::stable_sort(at[i].begin(), at[i].end(),
                         [](const Slot& a, const Slot& b) { return a.prio < b.prio; });
        for (const Slot& s : at[i]) emitSlot(i, s.side, s.ro);
        if (i < scratch->clusters.size()) {
            cl2.push_back(scratch->clusters[i]);
            adv2.push_back(scratch->cluster_adv_pt[i]);
            brk2.push_back(scratch->break_after[i]);
        }
    }
    scratch->clusters.swap(cl2);
    scratch->cluster_adv_pt.swap(adv2);
    scratch->break_after.swap(brk2);
    return scratch;
}

}  // namespace

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
                            const PageMetrics& page, const ControlSet* controls,
                            const std::vector<int>* bracketed) {
    LayoutResult res;
    layoutDocumentRange(lib, blocks, page, controls, bracketed, 0, static_cast<int>(blocks.size()),
                        freshReflowState(page), /*cache=*/nullptr, &res);
    return res;
}

ReflowState layoutDocumentRange(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                                const PageMetrics& page, const ControlSet* controls,
                                const std::vector<int>* bracketed, int from_para, int to_para,
                                const ReflowState& state, ShapingCache* cache, LayoutResult* out) {
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

    // 取出一段的纯整形结果（优先命中段级缓存）。
    //
    // 命中时返回的是指向缓存条目的只读引用，不做深拷贝：一份 BlockShaping 里挂着
    // 字形、字形所属字体、簇、簇推进、断点、簇起点六条等长的表，深拷贝一段就是六次
    // 分配加搬移 —— 在增量路径上（后缀可能有几百段）这是最贵的一步，而绝大多数段的
    // 数据一个字都不会改。只有要插括号占位的那一段才需要一份可改的副本，
    // 由调用方在那一条分支上复制（见下面 scratch 的用法）。
    // pi 是出参，提前返回的路径同样要写好统计值。
    // scratch 是"未命中时整形到这里"的缓冲；返回值指向 scratch 或缓存条目。
    auto shapeParagraph = [&](const BlockSpec& b, size_t para_index, BlockShaping* scratch,
                              ParaIndex* pi) -> const BlockShaping* {
        if (b.cp_text.empty()) return nullptr;
        FontInstance* font = lib.instance(b.font_index, b.size_pt);
        if (!font) return nullptr;

        const uint64_t fp = shapingFingerprint(b);
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

        // 整形：字体回退切段 → 逐段整形 → 合并。
        // 每个 run 只整形一次，切段依据是 cmap 精确查询而非整形结果。
        std::vector<int> chain;
        chain.reserve(1 + b.fallback_font_indices.size());
        chain.push_back(b.font_index);
        for (int fi : b.fallback_font_indices) {
            if (fi != b.font_index) chain.push_back(fi);
        }

        const auto t_shape0 = Clock::now();
        const std::vector<FontRun> runs =
            itemizeByFont(lib, b.cp_text.data(), b.cp_text.size(), chain, b.size_pt);
        pi->font_run_count = static_cast<int>(runs.size());

        bs.primary = font;
        bs.ascent_pt = font->ascenderPt();
        bs.descent_pt = font->descenderPt();
        // 这两个标记只由书写模式决定，先于切段赋值，避免依赖「至少有一段整形成功」。
        bs.st.vertical = (b.wm.shapingDirection() == ShapingDirection::TopToBottom);
        bs.st.sideways = b.wm.isSideways();

        for (const FontRun& r : runs) {
            if (r.cp_end <= r.cp_begin) continue;
            FontInstance* rf = font;
            if (r.face_index >= 0) {
                FontInstance* hit = lib.instance(r.face_index, b.size_pt);
                if (hit) rf = hit;
            }
            // 用 shapeTextRange 传整串指针加 [begin, end)：HarfBuzz 以区间起点为基准编号
            // cluster，故 cluster 就是段内绝对码点下标，此处不得再做任何偏移补算。
            const ShapedText part =
                shapeTextRange(rf->hb(), b.cp_text.data(), r.cp_begin, r.cp_end, b.wm, b.language);
            for (const ShapedGlyph& g : part.glyphs) {
                if (g.glyph_id == 0) ++pi->missing_count;  // 整条链都没兜住的码点
                bs.st.glyphs.push_back(g);
                bs.glyph_font.push_back(rf);
            }
            bs.st.inline_advance += part.inline_advance;
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

    for (int para_i = from_para; para_i < to_para; ++para_i) {
        const size_t para_index = static_cast<size_t>(para_i);
        const BlockSpec& b = blocks[para_index];
        ParaIndex& pi = res.para_index[para_index];

        // 续排锚点必须在任何提前返回之前记录，空段也记，否则「从第 k 段续排」在空段上无定义。
        pi.state_at_start = ReflowState{page_index, cursor_y, line_total};
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
            const float line_h = b.size_pt * b.line_factor;
            // 取不到字体就用字号折算的近似度量：空行的几何必须存在，否则光标没地方画。
            const float ascent = f ? f->ascenderPt() : b.size_pt * 0.8f;
            const float descent = f ? f->descenderPt() : b.size_pt * 0.2f;
            if (b.page_break_before && cursor_y > cy + 0.01f) {
                ++page_index;
                cursor_y = cy;
            }
            // 空段落一样要插括号占位：回车正好把字段断在两段之间、或字段所在段被删空时，
            //   这一段里就只剩字段边界了。走的是与有内容段落同一个 withBracketSlots ——
            //   顺序规则（][ / [] / [[ ]]）只有一份，不会两边不一致。
            BlockShaping empty_bs;
            empty_bs.primary = f;
            empty_bs.ascent_pt = ascent;
            empty_bs.descent_pt = descent;
            empty_bs.st.vertical = b.wm.isVertical();
            empty_bs.st.sideways = b.wm.isSideways();
            BlockShaping slot_bs;
            const BlockShaping* slotted =
                withBracketSlots(controls, bracketed, para_index, /*n_cp=*/0, &empty_bs, &slot_bs);

            LineBox lb;
            lb.paragraph = static_cast<int>(para_index);
            lb.line = line_total;
            lb.page = page_index;
            lb.vertical = b.wm.isVertical();
            lb.sideways = b.wm.isSideways();
            lb.size_pt = b.size_pt;
            lb.bracket_font = f;
            lb.first_cp = 0;
            lb.last_cp = 0;
            {
                // 行盒的簇 = 那些占位簇（每个的宽度都是槽宽）。空段落没有真实字形，
                //   所以行内推进只能由占位簇给出 —— 光标落在哪、括号多宽都从这一份来。
                float ax = 0.0f;
                for (size_t i = 0; i < slotted->clusters.size(); ++i) {
                    lb.clusters.push_back(LineCluster{
                        slotted->clusters[i].cp, ax, slotted->clusters[i].placeholder,
                        slotted->clusters[i].bracket_side, slotted->clusters[i].read_only});
                    ax += slotted->cluster_adv_pt[i];
                }
                lb.inline_end_pt = ax;
            }
            if (!lb.vertical) {
                if (cursor_y + line_h > cy + ch) {
                    ++page_index;
                    cursor_y = cy;
                    lb.page = page_index;
                }
                lb.block_lo_pt = cursor_y;
                lb.block_hi_pt = cursor_y + (ascent + descent);
                lb.inline_origin_pt = cx;
                lb.bracket_axis_pt = cursor_y + ascent;
                cursor_y += line_h + b.space_after_pt;
            } else {
                const float col_w = line_h;
                if (cursor_y + col_w * 2.0f > cy + ch) {
                    ++page_index;
                    cursor_y = cy;
                    lb.page = page_index;
                }
                const float center =
                    b.wm.isBlockReversed() ? cx + cw - 0.5f * col_w : cx + 0.5f * col_w;
                lb.block_lo_pt = center - col_w * 0.5f;
                lb.block_hi_pt = center + col_w * 0.5f;
                lb.inline_origin_pt = cursor_y;
                lb.bracket_axis_pt = center;
                // 竖排块吃掉本页剩余高度（与有内容的竖排段一致）
                cursor_y = cy + ch + b.space_after_pt;
            }
            res.lines.push_back(std::move(lb));
            ++line_total;
            pi.line_end = static_cast<int>(res.lines.size());
            pi.glyph_end = static_cast<int>(res.glyphs.size());
            continue;
        }
        // 取不到字体（整形为空）：只翻页、不占空间 —— 与下面 !shaped 那条分支一致。
        if (b.page_break_before && cursor_y > cy + 0.01f) {
            ++page_index;
            cursor_y = cy;
        }

        BlockShaping scratch;
        const BlockShaping* shaped = shapeParagraph(b, para_index, &scratch, &pi);
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

        // 括号占位：把"要显示括号的控件"的边界插成占位簇（实现见 withBracketSlots）。
        //   占位簇不产生字形、只占 advance，于是下面的断行容量、行末位置、每个簇的行内
        //   位置、光标与命中测试全都自动把括号算进去 —— 这就是"括号进文档流"的全部内容。
        //   显示范围由引擎给出（EngineConfig::form_brackets），排版层不认识「聚焦」：
        //   谁显示就占谁的位。默认全部控件常显，故占位集合与光标无关，光标移动不引起重排。
        //   para_cluster_starts 在此之前已生成，按约定不含占位簇。
        shaped = withBracketSlots(controls, bracketed, para_index, n_cp, shaped, &scratch);

        // 从这里往下全是只读：断行、放置字形都只读整形结果。
        const BlockShaping& bs = *shaped;

        const float line_h = b.size_pt * b.line_factor;
        // 本行/本列末尾码点：下一个簇的首码点，最后一簇用段长
        auto lastCpOf = [&](size_t slice_end) -> uint32_t {
            return (slice_end < bs.clusters.size()) ? bs.clusters[slice_end].cp : n_cp;
        };

        if (!b.wm.isVertical()) {
            // 横排
            const float ascent = bs.ascent_pt;
            const float descent = bs.descent_pt;
            if (cursor_y + line_h > cy + ch) {
                ++page_index;
                cursor_y = cy;
            }
            float baseline = cursor_y + ascent;

            size_t start = 0;
            while (start < bs.clusters.size()) {
                const LineSlice slice = takeLine(bs.cluster_adv_pt, bs.break_after, start, cw);

                // 行盒（几何的正面半边）
                LineBox lb;
                lb.paragraph = static_cast<int>(para_index);
                lb.line = line_total;
                lb.page = page_index;
                lb.vertical = false;
                lb.sideways = false;
                lb.size_pt = b.size_pt;
                lb.block_lo_pt = baseline - ascent;
                lb.block_hi_pt = baseline + descent;
                lb.inline_origin_pt = cx;
                // 画括号用：字体与基线（横排下字形原点的 y 就是基线）。
                // 字体必须是块的主字体（= 上面算占位槽宽那个 bracketAdvancePt 的来源），
                //   换个字体画括号，字形 advance 就和槽宽对不上了。
                lb.bracket_font = bs.primary;
                lb.bracket_axis_pt = baseline;
                lb.first_cp = bs.clusters[slice.begin].cp;
                lb.last_cp = lastCpOf(slice.end);
                {
                    float ax = 0.0f;
                    for (size_t i = slice.begin; i < slice.end; ++i) {
                        lb.clusters.push_back(
                            LineCluster{bs.clusters[i].cp, ax, bs.clusters[i].placeholder,
                                        bs.clusters[i].bracket_side, bs.clusters[i].read_only});
                        ax += bs.cluster_adv_pt[i];
                    }
                    lb.inline_end_pt = ax;
                }
                res.lines.push_back(std::move(lb));

                float pen_x = cx;
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
                for (size_t i = slice.begin; i < slice.end; ++i) {
                    pen_x = cx + ax;
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
                        pg.y_pt = pen_y - sg.y_offset * usToPt;
                        pg.color = b.color;
                        pg.page = page_index;
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

                start = slice.end;
                if (start < bs.clusters.size()) {
                    baseline += line_h;
                    ++line_total;
                    // 本行底部越过页底 → 翻页
                    if (baseline + (line_h - ascent) > cy + ch) {
                        ++page_index;
                        cursor_y = cy;
                        baseline = cursor_y + ascent;
                    }
                }
            }
            ++line_total;
            cursor_y = baseline + (line_h - ascent) + b.space_after_pt;

        } else {
            // 竖排
            // 竖排块占满本页剩余高度；列宽由 line_factor 决定。
            const float col_w = line_h;
            float region_top = cursor_y;
            if (region_top + col_w * 2.0f > cy + ch) {
                ++page_index;
                cursor_y = cy;
                region_top = cy;
            }

            int col = 0;
            float pen_y = region_top;

            // 列的中线 x。蒙文（LTR 列序）从左边开始排；中日（RTL 列序）从右边开始排。
            auto colCenterX = [&](int c) -> float {
                if (b.wm.isBlockReversed()) return cx + cw - (static_cast<float>(c) + 0.5f) * col_w;
                return cx + (static_cast<float>(c) + 0.5f) * col_w;
            };
            // 进入下一列；本页列位用尽则翻页
            auto nextColumn = [&]() {
                ++col;
                if ((static_cast<float>(col) + 1.0f) * col_w > cw + 0.01f) {
                    ++page_index;
                    col = 0;
                    region_top = cy;
                }
                pen_y = region_top;
            };

            // 侧倒（蒙文）：横排成型 + 每个字形顺时针转 90°。
            //   局部坐标映射：横排下屏幕偏移 = (x_offset, -y_offset)；
            //   侧倒即把内容顺时针转 90°，(u, -v) ↦ (v, u)。于是
            //       屏幕偏移 = (y_offset, x_offset)
            //   笔的定位：v 轴（-descent … +ascender）现在是水平方向，
            //   要让字身在列里居中 → 基线 x = 列中线 - (ascender - descent)/2。
            const bool sideways = bs.st.sideways;
            const float side_asc = bs.ascent_pt;
            const float side_desc = bs.descent_pt;

            size_t start = 0;
            while (start < bs.clusters.size()) {
                float avail = (cy + ch) - pen_y;
                if (avail < b.size_pt) {
                    nextColumn();
                    avail = (cy + ch) - pen_y;
                }

                const LineSlice slice = takeLine(bs.cluster_adv_pt, bs.break_after, start, avail);

                // 列内横轴的起始笔位：直立竖排 = 列心；侧倒 = 列心减去半个字身
                // （理由见下面那段"笔的定位"注释）。
                // 名字里带 0：它只是"这一列的起点"，字形放置时那个 gx 会往前推进
                //   （直立竖排下每画完一个字形 gx += x_advance）。
                const float gx0 =
                    sideways ? (colCenterX(col) - (side_asc - side_desc) * 0.5f) : colCenterX(col);

                // 列盒（竖排下的行盒）
                LineBox lb;
                lb.paragraph = static_cast<int>(para_index);
                lb.line = line_total + col;
                lb.page = page_index;
                lb.vertical = true;
                lb.sideways = sideways;
                lb.size_pt = b.size_pt;
                lb.block_lo_pt = colCenterX(col) - col_w * 0.5f;
                lb.block_hi_pt = colCenterX(col) + col_w * 0.5f;
                lb.inline_origin_pt = pen_y;
                // 画括号用：字体 + 竖排下的列内横轴起始笔位。
                lb.bracket_font = bs.primary;
                lb.bracket_axis_pt = gx0;
                lb.first_cp = bs.clusters[slice.begin].cp;
                lb.last_cp = lastCpOf(slice.end);
                {
                    float ay = 0.0f;
                    for (size_t i = slice.begin; i < slice.end; ++i) {
                        lb.clusters.push_back(
                            LineCluster{bs.clusters[i].cp, ay, bs.clusters[i].placeholder,
                                        bs.clusters[i].bracket_side, bs.clusters[i].read_only});
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
                for (size_t i = slice.begin; i < slice.end; ++i) {
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
                        pg.color = b.color;
                        pg.page = page_index;
                        pg.line = line_no;
                        pg.vertical = true;
                        pg.cluster = sg.cluster;
                        pg.paragraph = static_cast<int>(para_index);
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

                start = slice.end;
                if (start < bs.clusters.size()) nextColumn();
            }
            line_total += col + 1;
            // 竖排块吃掉本页剩余高度，后续块另起一页
            cursor_y = cy + ch + b.space_after_pt;
        }

        pi.line_end = static_cast<int>(res.lines.size());
        pi.glyph_end = static_cast<int>(res.glyphs.size());
    }

    // 汇总量按 para_index 逐段求和（而非只统计本轮）：前缀可能复用自上一轮，
    // 只数本轮会让 page_count / glyph_count / placed_line_count 变成半篇，
    // 而这些数字是帧高、翻页与面板的直接判据。
    summarizeLayoutStats(&res);
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

}  // namespace ced
