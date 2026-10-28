#include "core/layout/layout.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include "core/layout/block_layout.h"
#include "core/layout/block_pagination.h"
#include "core/layout/block_shape.h"
#include "core/layout/cell_index.h"
#include "core/layout/float_flow.h"
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
    /**
     * 块局部排版没有全局段下标，按整形指纹缓存；到上限整体清空（暂无淘汰策略）。
     */
    std::unordered_map<uint64_t, Entry> by_content;
    long long hits = 0;
    long long misses = 0;
};

ShapingCache::ShapingCache() : impl_(new Impl()) {}
ShapingCache::~ShapingCache() { delete impl_; }

void ShapingCache::clear() {
    impl_->paras.clear();
    impl_->by_content.clear();
    impl_->hits = 0;
    impl_->misses = 0;
}
size_t ShapingCache::size() const { return impl_->paras.size() + impl_->by_content.size(); }
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
 * 取出一段的纯整形结果（优先命中整形缓存）。
 *
 * 命中时返回指向缓存条目的只读引用，不做深拷贝；只有要插括号占位的那一段才需要一份
 * 可改副本。cache_by_content 为真时按整形指纹查找：块局部排版没有全局段下标。
 *
 * @param lib 字体库
 * @param b 块
 * @param fruns 实际生效的字符格式 run
 * @param cache 整形缓存；nullptr 表示不用缓存
 * @param cache_slot 按段下标缓存时的槽位
 * @param cache_by_content true = 按整形指纹缓存
 * @param scratch 未命中时整形到这里
 * @param stats 出参：簇数、缺字数、run 数与整形耗时
 * @return 整形结果；空文本、取不到字体或没有簇时为 nullptr
 */
const BlockShaping* shapeBlockShaping(FontLibrary& lib, const BlockSpec& b,
                                      const std::vector<FormatRun>& fruns, ShapingCache* cache,
                                      size_t cache_slot, bool cache_by_content,
                                      BlockShaping* scratch, BlockShapeStats* stats) {
    if (stats == nullptr || scratch == nullptr) return nullptr;
    *stats = BlockShapeStats{};
    if (b.cp_text.empty()) return nullptr;
    // 行度量（基线、行高、段后距）仍取块级字段：一段一套行高，这是本期已知边界。
    FontInstance* font = lib.instance(b.font_index, b.size_pt);
    if (!font) return nullptr;

    ShapingCache::Impl* cache_impl = ShapingCacheAccess::impl(cache);
    const uint64_t fp = shapingFingerprint(b, fruns);
    if (cache_impl) {
        const ShapingCache::Impl::Entry* hit = nullptr;
        if (cache_by_content) {
            const auto it = cache_impl->by_content.find(fp);
            if (it != cache_impl->by_content.end() && it->second.filled) hit = &it->second;
        } else if (cache_slot < cache_impl->paras.size()) {
            const ShapingCache::Impl::Entry& e = cache_impl->paras[cache_slot];
            if (e.filled && e.fp == fp) hit = &e;
        }
        if (hit != nullptr) {
            ++cache_impl->hits;
            stats->cluster_count = static_cast<int>(hit->bs.clusters.size());
            stats->missing_count = hit->bs.missing_count;
            stats->font_run_count = hit->bs.run_count;
            return hit->bs.clusters.empty() ? nullptr : &hit->bs;
        }
        ++cache_impl->misses;
    }

    BlockShaping& bs = *scratch;
    bs.primary = font;
    bs.ascent_pt = font->ascenderPt();
    bs.descent_pt = font->descenderPt();
    bs.gap_pt = font->lineGapPt();
    // 这两个标记只由书写模式决定，先于切段赋值。
    bs.st.vertical = (b.wm.shapingDirection() == ShapingDirection::TopToBottom);
    bs.st.sideways = b.wm.isSideways();

    const float usToPt = 1.0f / kFontScalePerPt;
    // 整形：先按字符格式 run 切，再在每段内按字体回退切 —— 两把尺子求交。字号取 run
    // 自己的：run 是字符格式相同的区间，字号不同就不该是同一个 run。
    const auto t_shape0 = Clock::now();
    for (const FormatRun& fr : fruns) {
        if (fr.cp_end <= fr.cp_begin) continue;
        std::vector<int> chain;
        chain.reserve(1 + fr.fallback_font_indices.size());
        chain.push_back(fr.font_index);
        for (int fi : fr.fallback_font_indices) {
            if (fi != fr.font_index) chain.push_back(fi);
        }
        // itemizeByFont 的区间相对于喂进去的指针，拿回来的边界要加回 fr.cp_begin。
        const std::vector<FontRun> runs = itemizeByFont(lib, b.cp_text.data() + fr.cp_begin,
                                                        fr.codepointCount(), chain, fr.size_pt);
        stats->font_run_count += static_cast<int>(runs.size());
        for (const FontRun& r : runs) {
            const size_t begin = fr.cp_begin + r.cp_begin;
            const size_t end = fr.cp_begin + r.cp_end;
            if (end <= begin) continue;
            // 字重也要跟 run 走：粗体的字形与度量都来自 700 那一份实例。
            FontInstance* rf = font;
            if (r.face_index >= 0) {
                FontInstance* hit = lib.instance(r.face_index, fr.size_pt, fr.weight);
                if (hit) rf = hit;
            }
            // 传整串指针加区间：HarfBuzz 以区间起点编号 cluster，故 cluster 就是段内
            // 绝对码点下标，此处不得再做偏移补算。
            const ShapedText part =
                shapeTextRange(rf->hb(), b.cp_text.data(), begin, end, b.wm, fr.language);
            for (const ShapedGlyph& g : part.glyphs) {
                if (g.glyph_id == 0) ++stats->missing_count;  // 整条链都没兜住的码点
                bs.st.glyphs.push_back(g);
                bs.glyph_font.push_back(rf);
            }
            bs.st.inline_advance += part.inline_advance;
        }
    }
    stats->shape_ms = msSince(t_shape0);
    bs.run_count = stats->font_run_count;
    bs.missing_count = stats->missing_count;

    // 按 cluster 分组，并算出每个簇沿行内轴的推进。
    bs.clusters.reserve(bs.st.glyphs.size());
    for (size_t i = 0; i < bs.st.glyphs.size(); ++i) {
        if (bs.clusters.empty() || bs.clusters.back().cp != bs.st.glyphs[i].cluster) {
            bs.clusters.push_back(BlockShaping::Cluster{bs.st.glyphs[i].cluster, i, i + 1});
        } else {
            bs.clusters.back().g1 = i + 1;
        }
    }
    stats->cluster_count = static_cast<int>(bs.clusters.size());

    // 簇起点表（升序）加段长哨兵，交给编辑层做吸附。
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

    // 写入缓存：拷进去的是纯整形结果，占位阶段只会动 scratch。
    if (cache_impl) {
        ShapingCache::Impl::Entry entry;
        entry.fp = fp;
        entry.filled = true;
        entry.bs = bs;
        if (cache_by_content) {
            // 块局部路径当前不是产品路径，先给一个有界集合：到上限整体清空。
            constexpr size_t kMaxContentEntries = 4096;
            if (cache_impl->by_content.size() >= kMaxContentEntries) cache_impl->by_content.clear();
            cache_impl->by_content[fp] = std::move(entry);
        } else {
            if (cache_impl->paras.size() <= cache_slot) cache_impl->paras.resize(cache_slot + 1);
            cache_impl->paras[cache_slot] = std::move(entry);
        }
    }
    return bs.clusters.empty() ? nullptr : &bs;
}

void clampFloatBoxToTextArea(const PageMetrics& page, float w_pt, float h_pt, float* x_pt,
                             float* y_pt) {
    const float x_lo = page.margins.left;
    const float x_hi = x_lo + page.contentWidthPt() - w_pt;
    const float y_lo = page.margins.top;
    const float y_hi = y_lo + page.contentHeightPt() - h_pt;
    if (x_pt != nullptr) *x_pt = (x_hi < x_lo) ? x_lo : std::min(std::max(*x_pt, x_lo), x_hi);
    if (y_pt != nullptr) *y_pt = (y_hi < y_lo) ? y_lo : std::min(std::max(*y_pt, y_lo), y_hi);
}

/**
 * 全量排版：从第 0 段排到末尾，不使用缓存（对照路径与基准）。
 */
LayoutResult layoutDocument(FontLibrary& lib, const std::vector<BlockSpec>& blocks,
                            const PageMetrics& page, const std::vector<LayoutTable>& tables,
                            const std::vector<LayoutFloat>& floats) {
    LayoutResult res;
    layoutDocumentRangeWithTables(lib, blocks, page, tables, 0, static_cast<int>(blocks.size()),
                                  freshReflowState(page), /*cache=*/nullptr, &res, floats);
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
                                          ShapingCache* cache, LayoutResult* out,
                                          const std::vector<LayoutFloat>& floats) {
    const auto t_layout0 = Clock::now();
    LayoutResult& res = *out;

    const float cx = page.margins.left;
    const float cy = page.margins.top;
    const float cw = page.contentWidthPt();
    const float ch = page.contentHeightPt();

    const int n_para = static_cast<int>(blocks.size());
    if (from_para < 0) from_para = 0;
    if (to_para > n_para) to_para = n_para;

    // 块到格的归属由表格结构现推。逐块复用同一个缓冲：整篇几千块，每块新造一份
    //   vector 会多出几千次分配。
    CellIndex cell_index;
    cell_index.build(tables, blocks.size());
    std::vector<CellSlot> cell_path_buf;

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
    /**
     * 这一段的段首是不是正好落在版心顶上。Word 在页顶不叠加段前距 —— 页首那截空白
     * 属于页边距，不属于段落；正文第一段（第 0 页）与格内照常让出。
     * @param inside_cell 这一段是否在表格里
     * @param pg 当前页号
     * @param y 块轴位置
     * @return true = 版心顶，段前距不加
     */
    auto atPageTop = [&](bool inside_cell, int pg, float y) -> bool {
        return !cont && !inside_cell && pg > 0 && y <= cy + 0.01f;
    };

    // 取出一段的纯整形结果（优先命中段级缓存）；整形与缓存逻辑在同文件的
    // shapeBlockShaping。pi 是出参，提前返回的路径同样要写好统计值。
    auto shapeParagraph = [&](const BlockSpec& b, const std::vector<FormatRun>& fruns,
                              size_t para_index, BlockShaping* scratch,
                              ParaIndex* pi) -> const BlockShaping* {
        BlockShapeStats stats;
        const BlockShaping* shaped = shapeBlockShaping(lib, b, fruns, cache, para_index,
                                                       /*cache_by_content=*/false, scratch, &stats);
        pi->cluster_count = stats.cluster_count;
        pi->missing_count = stats.missing_count;
        pi->font_run_count = stats.font_run_count;
        shape_ms += stats.shape_ms;
        return shaped;
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

    /**
     * 交给块局部排版的宽度。
     *
     * 宽度可能退到 1 磅以下：格宽被夹到下限、页面窄到装不下一行。块局部排版要求宽度为正，
     * 所以这里取不小于 1 磅 —— 断行"至少放一个簇"，两种取值的结果都是每簇一行，逐位相同。
     * @param w 当前可用宽度（磅）
     * @returns 不小于 1 磅的宽度
     */
    auto blockWidth = [](float w) { return std::max(w, 1.0f); };
    // 最后排完的那一块的段后距。格内容高要把它扣掉：格的底边由最后一段的文字底加内边距
    //   定，段后距是段与段之间的距离，最后一段后面没有下一段可让。
    float last_space_after = 0.0f;

    /**
     * 块轴纵坐标：分页下 = 页号 x 页高 + 页内 y，连页下就是那个绝对 y。
     *
     * 浮动框必须记在这一套里而不是页内：它可能横跨两条带（连页）或跨页（分页），
     *   而"这一行落不落在框上"是一句纵向区间比较 —— 记成页内坐标就得多判页号，
     *   漏一处就是"第二页起浮动框完全不生效"。页号由它现算，不另存。
     * @param pg 页号（分页下用；连页下 y 已经是绝对的，页号不参与）
     * @param y 页内 y（连页下传绝对 y）
     * @returns 块轴纵坐标
     */
    const float page_stride = cont ? slab : page.height_pt;
    auto flowY = [&](int pg, float y) -> float {
        return cont ? y : (static_cast<float>(pg) * page_stride + y);
    };

    /**
     * 块轴纵坐标 -> 页号。
     * @param f 块轴纵坐标
     * @returns 页号（不小于 0）
     */
    auto flowPage = [&](float f) -> int {
        if (!(page_stride > 0.0f)) return page_index;
        return std::max(0, static_cast<int>(std::floor(f / page_stride)));
    };

    /**
     * 块轴纵坐标 -> 页内 y。
     * @param pg 页号
     * @param f 块轴纵坐标
     * @returns 页内 y（磅）
     */
    auto flowLocal = [&](int pg, float f) -> float {
        return f - static_cast<float>(pg) * page_stride;
    };

    // 有浮动图时排版整篇重来（引擎侧 must_full），所以"预先判页"这条规则只在这里成立。
    const bool wrap_floats = !floats.empty();
    // 续排锚点带来的排除带先铺上：更早段落的图不在本轮的输入里，但它们挖掉的版面还在。
    FloatFlow live_flow;
    live_flow.bands = state.bands;
    // floats 按 (para, cp) 升序（编译层保证），游标一路往下走。
    size_t float_cursor = 0;

    /**
     * 把一个浮动框放上版面：横向左沿 = 内容区左边 + dx，纵向上沿 = 锚段第一行行顶 + dy。
     *
     * dx 是"相对内容区左边"，不是"相对版心居中"：居中之后一张宽图两侧各留一条，
     *   文字只能走一侧，另一侧就空着（看起来像"图跑到了段中间"）。左对齐是 Word
     *   的默认（列、0 偏移），也让"图在右、文字在左"这种排法真的出现。
     * @param para 当前段号
     * @param content_x 内容区左边
     * @param anchor_flow 该段第一行行顶的块轴纵坐标
     */
    auto placeFloatsForPara = [&](int para, float content_x, float anchor_flow) {
        if (floats.empty()) return;
        for (; float_cursor < floats.size(); ++float_cursor) {
            const LayoutFloat& f = floats[float_cursor];
            if (f.para > para) break;
            if (f.para < para) continue;
            if (!(f.w_pt > 0.0f) || !(f.h_pt > 0.0f)) continue;
            const float f0 = anchor_flow + f.dy_pt;
            const int pg = flowPage(f0);
            PlacedFloat pf;
            pf.handle = f.handle;
            pf.page = pg;
            pf.x = content_x + f.dx_pt;
            pf.y = flowLocal(pg, f0);
            pf.w = f.w_pt;
            pf.h = f.h_pt;
            // 框夹进版心：出去的那一截画不进页位图，图会被页边界切掉一半。
            clampFloatBoxToTextArea(page, pf.w, pf.h, &pf.x, &pf.y);
            const float fy = flowY(pg, pf.y);
            pf.wrap = f.wrap;
            pf.content_hash = f.content_hash;
            res.placed_floats.push_back(pf);
            // 衬于文字下方 / 浮于文字上方不参与断行，只记几何。
            if (f.wrap == ImageWrap::None || f.wrap == ImageWrap::Square) {
                FloatBand band;
                band.x0 = pf.x;
                band.x1 = pf.x + pf.w;
                band.f0 = fy;
                band.f1 = fy + pf.h;
                band.square = (f.wrap == ImageWrap::Square);
                live_flow.bands.push_back(band);
            }
        }
    };

    /**
     * 清掉已经过去的浮动框：下沿已经在当前行顶之上。
     * @param at_flow 当前行顶的块轴纵坐标
     */
    auto pruneFloats = [&](float at_flow) { live_flow.prune(at_flow); };

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
        // 上限给到"内容区 + 右边距"：拖表格的右边框就是把表加宽，Word 也允许表伸进右边距。
        //   再往右就越过纸张右沿了，那一截画不进页位图。
        w = std::min(std::max(w, 1.0f), cw + page.margins.right);
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
        // 表比内容区还宽时居中/右对齐会把表推到左边距里：贴住内容区左边。
        if (shift < 0.0f) shift = 0.0f;
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

    for (int para_i = from_para; para_i < to_para; ++para_i) {
        const size_t para_index = static_cast<size_t>(para_i);
        const BlockSpec& b = blocks[para_index];
        ParaIndex& pi = res.para_index[para_index];

        // 表格上下文切换：让 tstack 与这一块的格路径对齐。逐层比对前缀，不同的层
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
            cell_index.pathAt(para_i, &cell_path_buf);
            const std::vector<CellSlot>& path = cell_path_buf;
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
        //
        //   页号在连页下必须按块轴位置现算：这条路上 page_index 一路不增（连页不翻页），
        //   记成它的话全篇每一段的锚点都是 0 号条带。那一项是 ReflowState 的相等判据之一
        //   —— 它与"脏区排完之后的真实状态"永远不等，增量排版因此从第二页起每次都落到
        //   "后缀重新分页"那一路（见 repaginateSuffix），多排一遍还有排错的风险。
        const float anchor_y = opened_outer ? cursor_before_switch : cursor_y;
        pi.state_at_start = ReflowState{cont ? slabOfY(anchor_y) : page_index, anchor_y, line_total,
                                        live_flow.bands};
        pi.line_begin = static_cast<int>(res.lines.size());
        pi.glyph_begin = static_cast<int>(res.glyphs.size());
        pi.cluster_count = 0;
        pi.missing_count = 0;
        pi.font_run_count = 0;

        // 空竖排段也要用这两个前置判断：它走块局部排版，页首位置在块布局之前就要定下来。
        bool has_floating_object = false;
        for (const InlineObject& o : b.objects) {
            if (o.floating) {
                has_floating_object = true;
                break;
            }
        }
        // 段前分页是否生效：块首不在版心顶时才翻页（在页顶再翻就是一张空页）。
        auto wantsBreakBefore = [&]() {
            return !cont && tstack.empty() && b.page_break_before && cursor_y > cy + 0.01f;
        };
        // 段前分页：简单块由分页函数翻；其余块仍在整形之前翻，位置不变。
        auto pageBreakBefore = [&]() {
            if (!wantsBreakBefore()) return;
            ++page_index;
            cursor_y = cy;
        };
        // 空竖排段的块首列顶：段前分页与段前距与空段旧路径同一判据。先算出来而不直接
        // 改游标，块局部排版万一不受理时旧路径还要用原状态。
        auto emptyVerticalTop = [&](int* page_out) -> float {
            float top = cursor_y;
            if (wantsBreakBefore()) {
                ++*page_out;
                top = cy;
            }
            if (b.space_before_pt > 0.0f && !atPageTop(!tstack.empty(), *page_out, top))
                top += b.space_before_pt;
            return top;
        };

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
            // 空竖排段：一列到底，列宽就是空行行高。列顶、块首带是否另起、分页与块尾
            //   游标都由块局部排版与分页纯函数给出。文档里的浮动图不约束空列（排除带只作用于
            //   横排），所以有浮动图也走这一条；格内与块自身带浮动原子的空段仍走下面旧路径。
            const bool simple_empty_vertical =
                b.wm.isVertical() && cell_index.nodeAt(para_i) == nullptr && tstack.empty() &&
                !has_floating_object && cur_cw > 1.0f;
            if (simple_empty_vertical) {
                int col_page = page_index;
                const float col_top = emptyVerticalTop(&col_page);
                BlockConstraints constraints;
                constraints.width_pt = cur_cw;
                constraints.writing_mode = b.wm;
                constraints.column.left_pt = cx;
                constraints.column.bottom_pt = cy + ch;
                constraints.column.first_top_pt = col_top;
                // 连页的带顶是块轴绝对坐标，第二条带起逐带下移版心高；分页的后续带都从
                //   版心上沿起排。
                constraints.column.band_top_pt = cont ? (col_top + ch) : cy;
                constraints.column.band_step_pt = cont ? ch : 0.0f;
                constraints.column.continuous = cont;
                constraints.column.slab_pt = slab;
                const BlockLayout local = layoutBlock(lib, b, constraints, cache);
                if (local.status == BlockLayoutStatus::Ok && local.lines.size() == 1 &&
                    local.columns.size() == 1) {
                    BlockPlanInput pin;
                    pin.page_index = col_page;
                    pin.line_total = line_total;
                    pin.content_bottom_pt = cy + ch;
                    pin.space_after_pt = b.space_after_pt;
                    pin.started_new_band = local.started_new_band;
                    pin.continuous = cont;
                    pin.slab_pt = slab;
                    // 空竖排只占一列：带高就是这一列的列宽，旧循环的块尾游标正是列顶加列宽。
                    pin.band_height_pt = local.columns.front().width_pt;
                    const BlockPlan plan = planBlockColumns(local.columns, pin);
                    LineBox lb = std::move(local.lines.front());
                    // 块局部不认识全局页号与行号，这两项按分页结果重打；几何已落在
                    //   页内坐标上（连页的 y 偏移在 layoutBlock 里按条带折过）。
                    lb.paragraph = static_cast<int>(para_index);
                    lb.line = plan.lines.front().line_no;
                    lb.page = plan.lines.front().page;
                    res.lines.push_back(std::move(lb));
                    line_total = plan.line_total;
                    page_index = plan.page_index;
                    cursor_y = plan.cursor_y_pt;
                    pi.line_end = static_cast<int>(res.lines.size());
                    pi.glyph_end = static_cast<int>(res.glyphs.size());
                    continue;
                }
            }
            // 格内空段：横排与竖排都走块局部的空行/空列路径。空行的几何只由字体度量、
            //   括号占位与原子盒高决定；页内锚点仍在本循环按旧路径的加法次序重算 ——
            //   块内坐标相对内容区原点，直接平移会换掉加法分组、多一次舍入。竖排的列长
            //   与列位取版心，所以竖带按页传，宽度传版心宽。
            const bool empty_cell_block =
                cell_index.nodeAt(para_i) != nullptr && !tstack.empty() && !has_floating_object;
            if (empty_cell_block) {
                if (b.space_before_pt > 0.0f) cursor_y += b.space_before_pt;
                const bool vertical = b.wm.isVertical();
                BlockConstraints constraints;
                constraints.width_pt = vertical ? cw : cur_cw;
                constraints.writing_mode = b.wm;
                if (vertical) {
                    constraints.column.left_pt = cx;
                    constraints.column.bottom_pt = cy + ch;
                    constraints.column.first_top_pt = cursor_y;
                    constraints.column.band_top_pt = cont ? (cursor_y + ch) : cy;
                    constraints.column.band_step_pt = cont ? ch : 0.0f;
                    constraints.column.continuous = cont;
                    constraints.column.slab_pt = slab;
                }
                const BlockLayout local = layoutBlock(lib, b, constraints, cache);
                const bool one_line =
                    local.status == BlockLayoutStatus::Ok && local.lines.size() == 1 &&
                    (vertical ? local.columns.size() == 1 : local.columns.empty());
                if (one_line) {
                    LineBox lb = std::move(local.lines.front());
                    lb.paragraph = static_cast<int>(para_index);
                    lb.line = line_total;
                    lb.cell_lo_pt = cur_cx;
                    lb.cell_hi_pt = cur_cx + cur_cw;
                    lb.cell_box_lo_pt = tstack.back().cell_box_lo;
                    lb.cell_box_hi_pt = tstack.back().cell_box_hi;
                    if (!vertical) {
                        lb.page = slabOfY(cursor_y);
                        lb.block_lo_pt = locY(lb.page, cursor_y + lb.lead_top_pt);
                        lb.block_hi_pt = locY(
                            lb.page, cursor_y + lb.lead_top_pt + (lb.ascent_pt + lb.descent_pt));
                        lb.bracket_axis_pt =
                            locY(lb.page, cursor_y + lb.lead_top_pt + lb.ascent_pt);
                        lb.inline_origin_pt = cur_cx;
                        cursor_y += lb.advance_pt + b.space_after_pt;
                    } else {
                        BlockPlanInput pin;
                        pin.page_index = page_index;
                        pin.line_total = line_total;
                        pin.content_bottom_pt = cy + ch;
                        pin.space_after_pt = b.space_after_pt;
                        pin.started_new_band = local.started_new_band;
                        pin.continuous = cont;
                        pin.slab_pt = slab;
                        // 空列只占一条列宽：块尾游标正是列顶加列宽（旧循环的记法）。
                        pin.band_height_pt = local.columns.front().width_pt;
                        const BlockPlan plan = planBlockColumns(local.columns, pin);
                        lb.page = plan.lines.front().page;
                        lb.line = plan.lines.front().line_no;
                        line_total = plan.line_total;
                        page_index = plan.page_index;
                        cursor_y = plan.cursor_y_pt;
                    }
                    res.lines.push_back(std::move(lb));
                    if (!vertical) ++line_total;
                    pi.line_end = static_cast<int>(res.lines.size());
                    pi.glyph_end = static_cast<int>(res.glyphs.size());
                    continue;
                }
            }
            FontInstance* f = lib.instance(b.font_index, b.size_pt);
            // 取不到字体就用字号折算的近似度量：空行的几何必须存在，否则光标没地方画。
            const float ascent = f ? f->ascenderPt() : b.size_pt * 0.8f;
            const float descent = f ? f->descenderPt() : b.size_pt * 0.2f;
            const float gap = f ? f->lineGapPt() : 0.0f;
            if (!cont && tstack.empty() && b.page_break_before && cursor_y > cy + 0.01f) {
                ++page_index;
                cursor_y = cy;
            }
            // 段前距：页顶那一段不加（页首的空白属于页边距，不属于段落），与段后距各算各的。
            if (b.space_before_pt > 0.0f && !atPageTop(!tstack.empty(), page_index, cursor_y))
                cursor_y += b.space_before_pt;
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
            float natural = ascent + descent + gap;
            if (!(natural > 0.0f)) natural = b.size_pt;
            const float ascent_eff = std::max(ascent, box_h);
            const float line_h = lineHeightFor(b.line_spacing, natural, ascent_eff + descent);
            const float lead = lineLeadTopFor(b.line_spacing, line_h, ascent_eff + descent);

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
                lb.lead_top_pt = lead;
                lb.block_lo_pt = locY(lb.page, cursor_y + lead);
                lb.block_hi_pt = locY(lb.page, cursor_y + lead + (ascent_eff + descent));
                lb.ascent_pt = ascent_eff;
                lb.descent_pt = descent;
                lb.advance_pt = line_h;
                lb.clip_text = clipTextFor(b.line_spacing, ascent_eff, descent);
                lb.inline_origin_pt = cur_cx;
                lb.bracket_axis_pt = locY(lb.page, cursor_y + lead + ascent_eff);
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
        // 简单块：横排正文、无表格上下文、无浮动图。分页整段交给 planBlockLines
        //   （core/layout/block_pagination.h），它消费块局部断行片段。
        //   条件先算出来：段前分页与段前距对简单块也由那个函数负责，这里不能重复推进。
        // 表框栈是"这一块在不在格里"的唯一依据：格路径只用来开关表框（见上面的表上下文
        //   切换），不能当成第二个判断依据 —— 两处各判一次时，不一致的输入会落到另一条
        //   落位循环里，而那条循环是同一件事的第二份实现。
        const bool simple_block =
            !b.wm.isVertical() && tstack.empty() && !wrap_floats && !has_floating_object;
        // 竖排（含格内）都由下面的竖排分支走块局部排版：列内容按竖带断好，分页只把每条带
        //   映射成页；分页与连页的差别全在约束里的竖带几何上（连页的带顶是绝对坐标）。
        //   排除带只作用于横排，块自身的浮动原子由块局部排版报告不支持 —— 这是竖排的边。
        if (!simple_block) pageBreakBefore();

        const std::vector<FormatRun> fruns = b.effectiveRuns();
        BlockShaping scratch;
        const BlockShaping* shaped = shapeParagraph(b, fruns, para_index, &scratch, &pi);
        if (!shaped) {
            // 取不到字体（整形为空）：只翻页、不占空间。这一段没有行片段可用，
            //   段前距与段后距都不进游标；空段已在上面单独处理。简单块没有行片段就
            //   走不到分页函数，段前分页在这里补上。
            if (simple_block) pageBreakBefore();
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
        const float block_natural = (bs.ascent_pt + bs.descent_pt + bs.gap_pt > 0.0f)
                                        ? (bs.ascent_pt + bs.descent_pt + bs.gap_pt)
                                        : b.size_pt;
        const float block_line_h = lineHeightFor(b.line_spacing, block_natural, block_natural);
        // 颜色、斜体、下划线与字号按 run 现取（见 block_shape.h 的 runColorAt 与
        // stampRunMarks）：它们不进整形指纹，所以不能存进整形缓存。

        // 每个簇的行度量：整段算一次，横排的行循环里按行取最大值。按簇所在 run 自己的
        // 字体与字号取；只有一个 run 时这就是块级那套值。整段最大值 para_metric 给竖排
        // 的列宽用。
        std::vector<LineMetric> cluster_metric;
        LineMetric para_metric;
        computeClusterMetrics(lib, b, fruns, bs, &cluster_metric, &para_metric);

        // 段前距：段首行之前让出这一段空白。页顶那一段不加（分页之后段首的空白属于
        //   页边距），与上一段的段后距相加、不折叠 —— 那正是 Word 的算法。
        //   简单块的段前分页与段前距一起交给 planBlockLines，这里跳过。
        if (!simple_block && b.space_before_pt > 0.0f &&
            !atPageTop(!tstack.empty(), page_index, cursor_y)) {
            cursor_y += b.space_before_pt;
        }

        if (!b.wm.isVertical()) {
            // 横排。断行由块局部排版给出（layoutBlockFragments），分页由 planBlockLines
            //   或 planBlockLinesWithFloats 给出每行的行顶与页号，本循环只负责落位。
            //   行顶是分页层递推的：这里不再按行高自己累加，两处各加一次会在末位分叉。
            // 格内文本块只依赖格宽：格内不翻页（no_page_break），行高由表格在块排完之后
            //   结算，跨页由表格的行切片处理。文档里有浮动图时断行不能一次做完 ——
            //   排除带会按行顶挖开行，那一份由 planBlockLinesWithFloats 逐行取宽。
            const bool cell_block = !tstack.empty();
            BlockLayout local_block;
            if (!wrap_floats) {
                BlockConstraints constraints;
                constraints.width_pt = blockWidth(cur_cw);
                constraints.writing_mode = b.wm;
                local_block = layoutBlockFragments(b, constraints, bs, cluster_metric);
            }
            const std::vector<BrokenLine>& block_lines = local_block.fragments;

            // 落这一行：页内坐标换算在这里，行盒与字形由 placeHorizontalLine 写。
            //   page 由分页给出：分页下是当前页，连页下是行槽顶所在的条带。
            auto placeLine = [&](const std::vector<LinePart>& parts, float top, int page,
                                 int line_no) {
                if (parts.empty()) return;
                LineCellRange cell;
                if (!tstack.empty()) {
                    cell.cell_lo_pt = cur_cx;
                    cell.cell_hi_pt = cur_cx + cur_cw;
                    cell.box_lo_pt = tstack.back().cell_box_lo;
                    cell.box_hi_pt = tstack.back().cell_box_hi;
                }
                // 连页下块轴 y 要减掉条带偏移；分页下这个位移是 0。
                const float page_y_offset = cont ? -static_cast<float>(page) * slab : 0.0f;
                placeHorizontalLine(b, bs, fruns, parts, top, page, page_y_offset,
                                    static_cast<int>(para_index), line_no, cell, &res.lines,
                                    &res.glyphs);
            };

            // 本段的浮动框在段首行顶着落；段首行若被翻页推走，它们跟着平移。
            const size_t live_base = live_flow.bands.size();
            const size_t placed_base = res.placed_floats.size();
            // 锚点用块轴纵坐标记（分页下是"页号 + 页内"，连页下就是绝对 y）：
            //   段首行可能被翻页推走，而"推走多少"必须在同一套坐标里才加得对。
            const int anchor_page = page_index;
            float anchor_flow = wrap_floats ? flowY(anchor_page, cursor_y) : cursor_y;
            placeFloatsForPara(static_cast<int>(para_index), cur_cx, anchor_flow);

            // 分页与落位：三条规则（不绕排、排除带驱动、格内）共用同一个结果类型与
            //   同一个落位循环。页号与行号都由分页给出，本函数不再自己推进。
            BlockPlanInput pin;
            pin.page_index = page_index;
            pin.line_total = line_total;
            pin.cursor_y_pt = cursor_y;
            pin.content_top_pt = cy;
            pin.content_bottom_pt = cy + ch;
            pin.content_x_pt = cur_cx;
            pin.content_width_pt = cur_cw;
            pin.continuous = cont;
            pin.slab_pt = slab;
            pin.page_stride_pt = page_stride;
            // 格内不翻页：跨页由表格的行切片负责。段前分页与段前距只有简单块交给分页层
            //   （其余块的块首已经由上面的块级处理加过），所以按键与简单块绑定，避免加两次。
            pin.no_page_break = cell_block;
            pin.page_break_before = simple_block && b.page_break_before;
            pin.space_before_pt = simple_block ? b.space_before_pt : 0.0f;
            pin.space_after_pt = b.space_after_pt;
            pin.base_line_h_pt = block_line_h;
            pin.anchor_flow_pt = anchor_flow;
            pin.live_base = live_base;
            BlockPlan plan;
            if (wrap_floats) {
                // 行宽随这一行落在哪一页变，断行与分页不能拆成两遍：纯函数按排除带
                //   驱动断行，上下型下推与四周型让宽在 float_flow 里。
                plan = planBlockLinesWithFloats(b, bs, cluster_metric, &live_flow, pin);
            } else {
                plan = planBlockLines(block_lines, pin);
            }
            // 段首行被翻页推走时，本段落下的浮动框跟着平移；更早段落的图不动。
            if (plan.first_line_shift_pt != 0.0f) {
                for (size_t k = placed_base; k < res.placed_floats.size(); ++k) {
                    PlacedFloat& pf = res.placed_floats[k];
                    const float f0 = flowY(pf.page, pf.y) + plan.first_line_shift_pt;
                    pf.page = flowPage(f0);
                    pf.y = flowLocal(pf.page, f0);
                }
            }
            for (const BlockPlanLine& line : plan.lines) {
                placeLine(line.parts, line.top_pt, line.page, line.line_no);
            }
            line_total = plan.line_total;
            cursor_y = plan.cursor_y_pt;
            page_index = plan.page_index;
            pruneFloats(wrap_floats ? flowY(page_index, cursor_y) : cursor_y);

        } else {
            // 竖排
            // 列长 = 版心高（一列到底），列宽按这一列自己的行高算 —— 混排字号下小字列
            //   不该占大字列的宽度。列位由前缀和推进：中日（RTL）从右边缘往左推，蒙文
            //   （LTR）从左边缘往右推。断列与列位由块局部排版给出（layoutBlockFragments
            //   的 columns），分页只把每条竖带映射成页。

            // 落位：给这一列的列心、列顶，产出它的列盒与字形。它只认几何，不决定
            //   几何 —— 换带在块局部排版里。列宽就是片段的 line_h（见 BlockColumn），
            //   所以不再单独传：传进来就能和片段对不上。
            auto placeColumn = [&](const LinePart& part, float pen_y, int page, int line_no) {
                LineCellRange cell;
                if (!tstack.empty()) {
                    cell.cell_lo_pt = cur_cx;
                    cell.cell_hi_pt = cur_cx + cur_cw;
                    cell.box_lo_pt = tstack.back().cell_box_lo;
                    cell.box_hi_pt = tstack.back().cell_box_hi;
                }
                placeVerticalColumn(b, bs, fruns, part.line, part.x, part.line.line_h, pen_y, page,
                                    cont, slab, static_cast<int>(para_index), line_no, cell,
                                    &res.lines, &res.glyphs);
            };

            // 竖排块（正文与格内）：块局部排版按竖带断好列，分页只回答每条带落在哪一页。
            //   约束里的竖带：first_top_pt 是块首列顶，bottom_pt 是带下沿 —— 断列直接拿
            //   它们算，不再在这里重算。分页的后续带都从版心上沿起排；连页的后续带顶是
            //   块轴绝对坐标，逐带下移版心高，容量仍是"带下沿减绝对带顶"的旧式子。
            //   块自身带浮动原子的竖排不存在：投影层把竖排里的浮动图降级成行内
            //   （见 doc::project 的 float_ok），所以这里不需要第二条实现。
            {
                BlockConstraints constraints;
                constraints.width_pt = blockWidth(cw);
                constraints.writing_mode = b.wm;
                constraints.column.left_pt = cx;
                constraints.column.bottom_pt = cy + ch;
                constraints.column.first_top_pt = cursor_y;
                constraints.column.band_top_pt = cont ? (cursor_y + ch) : cy;
                constraints.column.band_step_pt = cont ? ch : 0.0f;
                constraints.column.continuous = cont;
                constraints.column.slab_pt = slab;
                const BlockLayout local_block =
                    layoutBlockFragments(b, constraints, bs, cluster_metric);
                if (!local_block.columns.empty()) {
                    BlockPlanInput pin;
                    pin.page_index = page_index;
                    pin.line_total = line_total;
                    pin.content_bottom_pt = cy + ch;
                    pin.space_after_pt = b.space_after_pt;
                    pin.started_new_band = local_block.started_new_band;
                    pin.continuous = cont;
                    pin.slab_pt = slab;
                    pin.band_height_pt = ch;
                    const BlockPlan plan = planBlockColumns(local_block.columns, pin);
                    for (const BlockPlanLine& line : plan.lines) {
                        if (line.parts.empty()) continue;
                        placeColumn(line.parts.front(), line.top_pt, line.page, line.line_no);
                    }
                    line_total = plan.line_total;
                    page_index = plan.page_index;
                    cursor_y = plan.cursor_y_pt;
                }
            }
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
    return ReflowState{page_index, cursor_y, line_total, {}};
}

bool LayoutResult::fragmentIndexValid() const {
    return fragments.valid(lines, glyphs, para_index.size(), stats.page_count);
}

void LayoutResult::rebuildPageIndex() {
    fragments.rebuild(lines, glyphs, para_index.size(), stats.page_count);
}

const std::vector<int>& LayoutResult::linesOnPage(int page) const {
    return fragments.linesOnPage(page);
}

const std::vector<int>& LayoutResult::linesOfBlock(int para) const {
    return fragments.linesOfBlock(para);
}

const std::vector<int>& LayoutResult::glyphsOnPage(int page) const {
    return fragments.glyphsOnPage(page);
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

std::vector<int> PlacedTable::gridColStarts() const {
    std::vector<int> starts(cells.size(), -1);
    if (rows <= 0 || cols <= 0) return starts;
    const size_t width = static_cast<size_t>(cols);
    std::vector<unsigned char> taken(static_cast<size_t>(rows) * width, 0);
    for (size_t i = 0; i < cells.size(); ++i) {
        const LayoutCell& c = cells[i];
        if (c.row < 0 || c.row >= rows) continue;
        const int span = std::max(c.col_span, 1);
        const int r_last = std::min(c.row + std::max(c.row_span, 1), rows);
        int start = 0;
        for (; start + span <= cols; ++start) {
            bool free_run = true;
            for (int r = c.row; r < r_last && free_run; ++r) {
                for (int k = 0; k < span; ++k) {
                    if (taken[static_cast<size_t>(r) * width + static_cast<size_t>(start + k)]) {
                        free_run = false;
                        break;
                    }
                }
            }
            if (free_run) break;
        }
        if (start + span > cols) continue;
        starts[i] = start;
        for (int r = c.row; r < r_last; ++r) {
            for (int k = 0; k < span; ++k) {
                taken[static_cast<size_t>(r) * width + static_cast<size_t>(start + k)] = 1;
            }
        }
    }
    return starts;
}

const LayoutCell* PlacedTable::cellAt(int row, int col, int* col_lo, int* col_hi) const {
    if (col < 0 || col >= cols) return nullptr;
    const std::vector<int> starts = gridColStarts();
    for (size_t i = 0; i < cells.size(); ++i) {
        const LayoutCell& c = cells[i];
        const int lo = starts[i];
        if (lo < 0) continue;
        const int hi = lo + std::max(c.col_span, 1);
        if (col < lo || col >= hi) continue;
        // 跨行的格：它属于起点那一行，但覆盖到的每一行都该命中它 —— 否则合并格的下半截
        //   点不中，而那里并没有别的格可以接。
        if (row < c.row || row >= c.row + std::max(c.row_span, 1)) continue;
        if (col_lo) *col_lo = lo;
        if (col_hi) *col_hi = hi;
        return &c;
    }
    return nullptr;
}

int PlacedTable::gridColOf(const LayoutCell& cell) const {
    const std::vector<int> starts = gridColStarts();
    for (size_t i = 0; i < cells.size(); ++i) {
        const LayoutCell& c = cells[i];
        if (&c == &cell) return starts[i];
        // 同一个格也可能以副本传进来（调用方持有的是拷贝）：按行、格号与跨度认。
        if (c.row == cell.row && c.cell == cell.cell && c.row_span == cell.row_span &&
            c.col_span == cell.col_span) {
            return starts[i];
        }
    }
    return -1;
}

int PlacedTable::rowIndexAt(int page, float y_pt, float tol_pt) const {
    int hit = -1;
    float best = 0.0f;
    for (const PlacedRow& pr : placed) {
        if (pr.page != page) continue;
        if (y_pt < pr.y - tol_pt || y_pt > pr.y + pr.h + tol_pt) continue;
        const float d =
            (y_pt < pr.y) ? (pr.y - y_pt) : (y_pt > pr.y + pr.h ? y_pt - (pr.y + pr.h) : 0.0f);
        // 只沾到容差的两条行都有可能接住这一点（点在行边界上）：取更近的那一条，
        //   一样近时取先遇到的（行片段自上而下排，先遇到的在上）。
        if (hit < 0 || d < best) {
            hit = pr.row;
            best = d;
        }
    }
    return hit;
}

bool PlacedTable::rowBandOnPage(int page, int row, float* y, float* h) const {
    float top = 0.0f;
    float bottom = 0.0f;
    bool seen = false;
    for (const PlacedRow& pr : placed) {
        if (pr.row != row || pr.page != page) continue;
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
    if (y) *y = top;
    if (h) *h = bottom - top;
    return true;
}

bool PlacedTable::hasRowsOnPage(int page) const {
    for (const PlacedRow& pr : placed) {
        if (pr.page == page) return true;
    }
    return false;
}

const LayoutCell* PlacedTable::cellAtPt(int page, float x_pt, float y_pt, float tol_x_pt,
                                        float tol_y_pt, int* row_out) const {
    if (x_pt < x - tol_x_pt || x_pt > x + w + tol_x_pt) return nullptr;
    const int row = rowIndexAt(page, y_pt, tol_y_pt);
    if (row < 0) return nullptr;
    // 列按"第一个把这一点连同容差一起盖住的段"取，从左往右找：点在边界上时归左那一格，
    //   与行边界上归上那一格同一个取向。
    int col = -1;
    for (int c = 0; c < cols && c + 1 < static_cast<int>(col_x.size()); ++c) {
        const float lo = x + col_x[static_cast<size_t>(c)];
        const float hi = x + col_x[static_cast<size_t>(c) + 1];
        if (x_pt >= lo - tol_x_pt && x_pt <= hi + tol_x_pt) {
            col = c;
            break;
        }
    }
    if (col < 0) return nullptr;
    const LayoutCell* cell = cellAt(row, col, nullptr, nullptr);
    if (cell == nullptr) return nullptr;
    if (row_out) *row_out = row;
    return cell;
}

bool PlacedTable::cellBoxOnPage(const LayoutCell& cell, int page, float* x, float* y, float* w,
                                float* h) const {
    const int col_lo = gridColOf(cell);
    if (col_lo < 0) return false;
    const int col_hi = col_lo + std::max(cell.col_span, 1);
    if (col_hi >= static_cast<int>(col_x.size())) return false;
    float top = 0.0f;
    float band = 0.0f;
    if (!rowBandOnPage(page, cell.row, &top, &band)) return false;
    if (x) *x = this->x + col_x[static_cast<size_t>(col_lo)];
    if (w) *w = col_x[static_cast<size_t>(col_hi)] - col_x[static_cast<size_t>(col_lo)];
    if (y) *y = top;
    if (h) *h = band;
    return true;
}

}  // namespace heditor
