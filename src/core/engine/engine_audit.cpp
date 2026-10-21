/**
 * 审计判据：增量结果与全量结果的逐字段对照。
 *
 * 判据的正身在这里，调用点在测试与浏览器探针。分开的理由是它只被验证路径用到、
 * 不参与正常编辑 —— 混在编辑代码里会让"这段到底跑不跑"变得不清楚。
 */

#include "core/engine/engine.h"
#include "core/engine/payload_schema.h"

#include "core/text/writing_mode.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"
#include "core/utils/units.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "core/text/shaper.h"

namespace heditor {

namespace {

/**
 * 位置比较：整数/标志必须逐位相同，浮点坐标允许"远小于可见阈值"的容差。
 *
 * 为什么不能要求浮点也逐位相同：平移复用的坐标是"在旧坐标上加一个偏移"算出来的，
 * 而全量重排是"从 cursor_y 重新累加"出来的 —— 同一条折线两种加法，末位必然可能不同
 * （实测最大偏差 6.1e-05 pt）。除非把复用路径的算术写成与全量逐位一致，否则这不可消除。
 *
 * 容差取 1e-3 pt 的界线是量出来的：比舍入噪声大 16 倍（不会假红），
 * 又比引擎自己的可见阈值小 30 倍 —— 页指纹量化位置用 1/16 pt（见 quantPt），
 * 即 0.03 pt 以内的差别在引擎看来本来就"不算变化"。所以容差不会掩盖任何真错位：
 * 真正的错位是"整行挪错"，量级是几十磅。
 */
constexpr float kPosTol = 1e-3f;
inline bool samePos(float a, float b) { return std::fabs(a - b) <= kPosTol; }

/**
 * 两个字形是否逐字段相同。font 比指针：同一个 FontLibrary 里同一张脸就是同一个指针。
 */
bool sameGlyph(const PlacedGlyph& a, const PlacedGlyph& b) {
    return a.glyph_id == b.glyph_id && a.font == b.font && samePos(a.x_pt, b.x_pt) &&
           samePos(a.y_pt, b.y_pt) && a.page == b.page && a.line == b.line &&
           a.vertical == b.vertical && samePos(a.rotate_deg, b.rotate_deg) &&
           a.cluster == b.cluster && a.paragraph == b.paragraph && a.color.r == b.color.r &&
           a.color.g == b.color.g && a.color.b == b.color.b && a.color.a == b.color.a;
}

bool sameLine(const LineBox& a, const LineBox& b) {
    if (a.paragraph != b.paragraph || a.line != b.line || a.page != b.page ||
        a.vertical != b.vertical || a.sideways != b.sideways || !samePos(a.size_pt, b.size_pt) ||
        !samePos(a.block_lo_pt, b.block_lo_pt) || !samePos(a.block_hi_pt, b.block_hi_pt) ||
        !samePos(a.inline_origin_pt, b.inline_origin_pt) ||
        !samePos(a.inline_end_pt, b.inline_end_pt) || a.first_cp != b.first_cp ||
        a.last_cp != b.last_cp || a.bracket_font != b.bracket_font ||
        !samePos(a.bracket_axis_pt, b.bracket_axis_pt) || a.clusters.size() != b.clusters.size() ||
        !samePos(a.cell_lo_pt, b.cell_lo_pt) || !samePos(a.cell_hi_pt, b.cell_hi_pt) ||
        !samePos(a.cell_box_lo_pt, b.cell_box_lo_pt) ||
        !samePos(a.cell_box_hi_pt, b.cell_box_hi_pt) || !samePos(a.cell_top_pt, b.cell_top_pt) ||
        !samePos(a.cell_bottom_pt, b.cell_bottom_pt)) {
        return false;
    }
    for (size_t i = 0; i < a.clusters.size(); ++i) {
        const LineCluster& x = a.clusters[i];
        const LineCluster& y = b.clusters[i];
        if (x.cp != y.cp || !samePos(x.inline_pt, y.inline_pt) || x.placeholder != y.placeholder ||
            x.bracket_side != y.bracket_side || x.read_only != y.read_only ||
            x.owner_control != y.owner_control || x.atom != y.atom ||
            !samePos(x.atom_h_pt, y.atom_h_pt) || x.content_hash != y.content_hash) {
            return false;
        }
    }
    return true;
}

/**
 * 两张已放置的表格是否逐字段相同：位置、列边界、逻辑格、每一行片段（页、y、高、续接标记）。
 *
 * 它补的是"排版自审对表格是盲的"这个洞：列的几何与行片段不进 glyphs / lines，只比那两样
 * 的话，整张表错位、行高算错、跨页片段切在错的地方，diff 都会是 0。
 */
bool sameTable(const PlacedTable& a, const PlacedTable& b) {
    if (a.handle != b.handle || a.first_block != b.first_block || !samePos(a.x, b.x) ||
        !samePos(a.w, b.w) || a.rows != b.rows || a.cols != b.cols ||
        a.col_x.size() != b.col_x.size() || a.cells.size() != b.cells.size() ||
        a.placed.size() != b.placed.size()) {
        return false;
    }
    for (size_t i = 0; i < a.col_x.size(); ++i) {
        if (!samePos(a.col_x[i], b.col_x[i])) return false;
    }
    for (size_t i = 0; i < a.cells.size(); ++i) {
        const LayoutCell& x = a.cells[i];
        const LayoutCell& y = b.cells[i];
        if (x.row != y.row || x.cell != y.cell || x.row_span != y.row_span ||
            x.col_span != y.col_span || x.v_align != y.v_align || x.first_block != y.first_block ||
            x.block_count != y.block_count) {
            return false;
        }
    }
    for (size_t i = 0; i < a.placed.size(); ++i) {
        const PlacedRow& x = a.placed[i];
        const PlacedRow& y = b.placed[i];
        if (x.row != y.row || x.page != y.page || !samePos(x.y, y.y) || !samePos(x.h, y.h) ||
            x.cont_top != y.cont_top || x.cont_bottom != y.cont_bottom) {
            return false;
        }
    }
    return true;
}

bool sameParaIndex(const ParaIndex& a, const ParaIndex& b) {
    return a.line_begin == b.line_begin && a.line_end == b.line_end &&
           a.glyph_begin == b.glyph_begin && a.glyph_end == b.glyph_end &&
           a.cluster_count == b.cluster_count && a.missing_count == b.missing_count &&
           a.font_run_count == b.font_run_count &&
           a.state_at_start.page_index == b.state_at_start.page_index &&
           a.state_at_start.line_total == b.state_at_start.line_total &&
           samePos(a.state_at_start.cursor_y, b.state_at_start.cursor_y);
}

/**
 * 一次排版自审的结果。diff 是"不符的条目数"，不是"哪里有偏差"的近似。
 */
struct LayoutAudit {
    int glyph_diff = 0;
    int line_diff = 0;
    int para_diff = 0;
    int cluster_starts_diff = 0;
    int table_diff = 0;
    bool stats_same = true;
    int glyph_first = -1;
    int line_first = -1;
    int para_first = -1;

    int total() const {
        return glyph_diff + line_diff + para_diff + cluster_starts_diff + table_diff +
               (stats_same ? 0 : 1);
    }
};

LayoutAudit auditLayoutPair(const LayoutResult& cur, const LayoutResult& ref) {
    LayoutAudit a;
    const size_t ng = std::min(cur.glyphs.size(), ref.glyphs.size());
    for (size_t i = 0; i < ng; ++i) {
        if (!sameGlyph(cur.glyphs[i], ref.glyphs[i])) {
            if (a.glyph_first < 0) a.glyph_first = static_cast<int>(i);
            ++a.glyph_diff;
        }
    }
    a.glyph_diff += static_cast<int>(cur.glyphs.size() > ref.glyphs.size()
                                         ? cur.glyphs.size() - ref.glyphs.size()
                                         : ref.glyphs.size() - cur.glyphs.size());

    const size_t nl = std::min(cur.lines.size(), ref.lines.size());
    for (size_t i = 0; i < nl; ++i) {
        if (!sameLine(cur.lines[i], ref.lines[i])) {
            if (a.line_first < 0) a.line_first = static_cast<int>(i);
            ++a.line_diff;
        }
    }
    a.line_diff +=
        static_cast<int>(cur.lines.size() > ref.lines.size() ? cur.lines.size() - ref.lines.size()
                                                             : ref.lines.size() - cur.lines.size());

    const size_t np = std::min(cur.para_index.size(), ref.para_index.size());
    for (size_t i = 0; i < np; ++i) {
        if (!sameParaIndex(cur.para_index[i], ref.para_index[i])) {
            if (a.para_first < 0) a.para_first = static_cast<int>(i);
            ++a.para_diff;
        }
    }
    a.para_diff += static_cast<int>(cur.para_index.size() > ref.para_index.size()
                                        ? cur.para_index.size() - ref.para_index.size()
                                        : ref.para_index.size() - cur.para_index.size());

    const size_t ns = std::min(cur.para_cluster_starts.size(), ref.para_cluster_starts.size());
    for (size_t i = 0; i < ns; ++i) {
        if (cur.para_cluster_starts[i] != ref.para_cluster_starts[i]) ++a.cluster_starts_diff;
    }
    a.cluster_starts_diff +=
        static_cast<int>(cur.para_cluster_starts.size() > ref.para_cluster_starts.size()
                             ? cur.para_cluster_starts.size() - ref.para_cluster_starts.size()
                             : ref.para_cluster_starts.size() - cur.para_cluster_starts.size());

    const size_t nt = std::min(cur.placed_tables.size(), ref.placed_tables.size());
    for (size_t i = 0; i < nt; ++i) {
        if (!sameTable(cur.placed_tables[i], ref.placed_tables[i])) ++a.table_diff;
    }
    a.table_diff += static_cast<int>(cur.placed_tables.size() > ref.placed_tables.size()
                                         ? cur.placed_tables.size() - ref.placed_tables.size()
                                         : ref.placed_tables.size() - cur.placed_tables.size());

    const LayoutStats& x = cur.stats;
    const LayoutStats& y = ref.stats;
    a.stats_same =
        (x.page_count == y.page_count && x.glyph_count == y.glyph_count &&
         x.placed_line_count == y.placed_line_count && x.cluster_count == y.cluster_count &&
         x.missing_glyph_count == y.missing_glyph_count && x.font_run_count == y.font_run_count);
    return a;
}

/**
 * 一段编译产物与参照逐字段比，返回不符项数。
 *
 * 为什么逐字段而不是只比段数或文本：编译增量最典型的错法是"某一段的产物还是旧的"，
 * 段数、每段文本长度可能全都对得上，差别只在一个 run 的字号或一个边界的锚点上 ——
 * 而那正是静默错版式的来源。
 */
int diffBlockSpec(const BlockSpec& a, const BlockSpec& b) {
    int d = 0;
    if (a.cp_text != b.cp_text) ++d;
    if (a.runs.size() != b.runs.size()) {
        ++d;
    } else {
        for (size_t i = 0; i < a.runs.size(); ++i) {
            const FormatRun& x = a.runs[i];
            const FormatRun& y = b.runs[i];
            if (x.cp_begin != y.cp_begin || x.cp_end != y.cp_end || x.font_index != y.font_index ||
                x.size_pt != y.size_pt || x.color != y.color || x.language != y.language ||
                x.fallback_font_indices != y.fallback_font_indices) {
                ++d;
            }
        }
    }
    if (a.objects.size() != b.objects.size()) {
        ++d;
    } else {
        for (size_t i = 0; i < a.objects.size(); ++i) {
            const InlineObject& x = a.objects[i];
            const InlineObject& y = b.objects[i];
            // owner 不比：参照侧的节点号由模型树自己分配，与修订的节点号不同源，
            //   逐号比较恒不相等。它的一致性由控件的成对计数与下标翻译另测。
            if (x.cp != y.cp || x.kind != y.kind || x.read_only != y.read_only) {
                ++d;
            }
        }
    }
    if (a.font_index != b.font_index || a.size_pt != b.size_pt || a.line_factor != b.line_factor ||
        a.space_after_pt != b.space_after_pt || a.page_break_before != b.page_break_before ||
        a.language != b.language || a.color != b.color ||
        a.fallback_font_indices != b.fallback_font_indices) {
        ++d;
    }
    if (a.wm.inline_axis != b.wm.inline_axis || a.wm.block_flow != b.wm.block_flow ||
        a.wm.orientation != b.wm.orientation) {
        ++d;
    }
    return d;
}

/**
 * 产物里"成对的控件边界"给出的区间，按 ControlStart 的出现顺序（= 文档顺序）。
 *
 * 独立口径：控件集合本身是投影给的跨度（见 exportControls），两边是否一致不能自己证明
 * 自己 —— 所以这里从产物重算一遍。半截边界在导出那一侧会被整对丢掉，是静默降级；只数个数
 * 也看不出"区间钉错了段"或"顺序换了"，而换了序等于换了"第几个字段"这张表。
 */
std::vector<ControlRange> pairedControlRanges(const std::vector<BlockSpec>& blocks) {
    struct Pair {
        TextPos start{};
        TextPos end{};
        bool has_start = false;
        bool has_end = false;
    };
    std::unordered_map<int, Pair> acc;
    std::vector<int> order;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const int para = static_cast<int>(i);
        for (const InlineObject& o : blocks[i].objects) {
            auto it = acc.find(o.owner);
            if (it == acc.end()) {
                order.push_back(o.owner);
                it = acc.emplace(o.owner, Pair{}).first;
            }
            Pair& p = it->second;
            const TextPos at{para, static_cast<size_t>(o.cp)};
            if (o.kind == InlineObjectKind::ControlStart) {
                p.start = at;
                p.has_start = true;
            } else {
                p.end = at;
                p.has_end = true;
            }
        }
    }
    std::vector<ControlRange> out;
    for (int owner : order) {
        const Pair& p = acc[owner];
        if (!p.has_start || !p.has_end) continue;
        out.push_back(ControlRange{p.start, p.end});
    }
    return out;
}

}  // namespace

namespace {

/**
 * 两块同尺寸位图逐像素比（只比 RGB —— alpha 恒为不透明，不参与判断）。
 * 返回差像素数；bbox 在 diff > 0 时给出。
 */
struct BitmapDiff {
    long long diff = 0;
    int minx = 1 << 30;
    int miny = 1 << 30;
    int maxx = -1;
    int maxy = -1;
};

BitmapDiff diffBitmaps(const Bitmap& a, const Bitmap& b) {
    BitmapDiff d;
    const int w = a.width();
    const int h = a.height();
    if (w != b.width() || h != b.height()) {
        d.diff = -1;
        return d;
    }
    const uint8_t* pa = a.data();
    const uint8_t* pb = b.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t off =
                (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u;
            if (pa[off] == pb[off] && pa[off + 1] == pb[off + 1] && pa[off + 2] == pb[off + 2]) {
                continue;
            }
            ++d.diff;
            if (x < d.minx) d.minx = x;
            if (x > d.maxx) d.maxx = x;
            if (y < d.miny) d.miny = y;
            if (y > d.maxy) d.maxy = y;
        }
    }
    return d;
}

std::string diffJson(const BitmapDiff& d, int w, int h) {
    std::ostringstream o;
    o << "{\"diff\":" << d.diff << ",\"frameW\":" << w << ",\"frameH\":" << h;
    if (d.diff > 0) {
        o << ",\"box\":[" << d.minx << "," << d.miny << "," << d.maxx << "," << d.maxy << "]";
    } else {
        o << ",\"box\":null";
    }
    o << "}";
    return pk::checked(pk::diff::kSpec, o.str());
}

}  // namespace

/**
 * 帧自审：现在这份画面 == "从零重画一遍"的结果吗？
 *
 * 为什么不满足于"画布 == 引擎画面"（selfCheck）：那个判据只能抓"上屏贴错"。如果引擎
 *   画出来的本身就错了（旧光标没擦、某页的带重画错位），画布忠实反映了它，两边当然
 *   相等 —— 判据全绿而屏幕是错的。这里用的是和单测 [10]（字形缓存等价性）同一个套路：
 *   增量路径的结果，必须逐字节等于从零重来的结果。
 *
 * "从零重来"= 整份页缓存丢掉 + 重光栅 + 重拼。页缓存是唯一的像素缓存，所以这一条
 *   同时覆盖"光栅确定性"与"拼装 / 瞬态确定性"。
 * 副作用（有意为之）：跑完画面就处于真值状态，因此它同时也是自愈手段 ——
 *   出问题时跑一次，画面立刻正确，同时拿到取证数据。
 */
std::string EditorEngine::auditFrameJson() {
    const int fw = frameWidth();
    const int fh = frameHeight();
    if (fw <= 0 || fh <= 0) return pk::checked(pk::diff::kSpec, "{\"diff\":-1,\"box\":null}");

    // 参照物与现况都现场拼：现况那一份不能复用缓存的导出图 ——
    //   那正是要审的东西（它可能与页位图对不上）。
    Bitmap before;
    assembleWindow(true, &before);

    page_cache_.clear();
    page_dirty_rects_.clear();
    repaint();

    Bitmap after;
    assembleWindow(true, &after);
    return pk::checked(pk::diff::kSpec, diffJson(diffBitmaps(before, after), fw, fh));
}

/**
 * 内容自审：不含瞬态的拼装 == 从零重来吗？
 *
 * 与帧自审分工：两条都差 ⇒ 内容画错了；这一条绿而帧自审差 ⇒ 瞬态叠错了。
 *   它的对象是"页位图按窗口拼起来"的那一层：清缓存、重画、逐字节比。
 */
std::string EditorEngine::auditBaseJson() {
    const int fw = frameWidth();
    const int fh = frameHeight();
    if (fw <= 0 || fh <= 0) return pk::checked(pk::diff::kSpec, "{\"diff\":-1,\"box\":null}");

    Bitmap before;
    assembleWindow(false, &before);

    page_cache_.clear();
    page_dirty_rects_.clear();
    repaint();

    Bitmap after;
    assembleWindow(false, &after);
    return pk::checked(pk::diff::kSpec, diffJson(diffBitmaps(before, after), fw, fh));
}

/**
 * 页自审：缓存里的第 p 页 == 从零光栅同一页的结果吗？
 *
 * 参照物刻意绕开缓存。auditFrameJson 与 auditBaseJson 读的都是缓存里的像素，
 *   缓存本身错了它们照样全绿；而"两条路径都错得一样"正是最难查的一种（与
 *   auditGlyphPagesJson 同一条理由）。这里让同一份 rasterIntoBitmap 画到一块
 *   临时位图上：写进缓存是更新，写进临时位图是取证。
 *
 * 副作用：只读，不动缓存、不动计时账（光栅计时与字形计数先存后还）。
 * 页不在缓存里（还没画过，或被淘汰）时返回 diff = -1，不做假绿。
 */
std::string EditorEngine::auditPageJson(int page) {
    const int pw = pagePxWidth();
    const int ph = pagePxHeight();
    const auto it = page_cache_.find(page);
    if (it == page_cache_.end() || !it->second.valid || it->second.px.width() != pw ||
        it->second.px.height() != ph) {
        return pk::checked(pk::audit_page::kSpec, "{\"diff\":-1,\"cached\":0}");
    }
    Bitmap ref(pw, ph);
    ref.shareFonts(font_owner_);
    const int saved_glyphs = raster_glyphs_;
    const double saved_bounds = raster_glyph_bounds_ms_;
    const double saved_fill = raster_fill_ms_;
    const double saved_glyph = raster_glyph_ms_;
    rasterIntoBitmap(ref, page, nullptr, nullptr);
    raster_glyphs_ = saved_glyphs;
    raster_glyph_bounds_ms_ = saved_bounds;
    raster_fill_ms_ = saved_fill;
    raster_glyph_ms_ = saved_glyph;

    const uint8_t* a = it->second.px.data();
    const uint8_t* b = ref.data();
    long long diff = 0;
    int minx = 1 << 30;
    int miny = 1 << 30;
    int maxx = -1;
    int maxy = -1;
    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            const size_t off =
                (static_cast<size_t>(y) * static_cast<size_t>(pw) + static_cast<size_t>(x)) * 4u;
            if (a[off] == b[off] && a[off + 1] == b[off + 1] && a[off + 2] == b[off + 2]) continue;
            ++diff;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    std::ostringstream o;
    o << "{\"diff\":" << diff << ",\"w\":" << pw << ",\"h\":" << ph << ",\"page\":" << page
      << ",\"cached\":1";
    if (diff > 0) {
        o << ",\"box\":[" << minx << "," << miny << "," << maxx << "," << maxy << "]";
    } else {
        o << ",\"box\":null";
    }
    o << "}";
    return pk::checked(pk::audit_page::kSpec, o.str());
}

/**
 * 表格结构自审：块与格这两份记录互为对照（见头文件）。
 *
 * 两份记录说的是同一件事：块用 cell_path 说自己属于哪一格，格用块区间说自己有哪些块。
 *   两边都对才站得住 —— 只看一边，遇到"格还在、块没了"就查不出来。
 */
int EditorEngine::auditTables() const { return auditTablesDetail().total(); }

std::string EditorEngine::auditTablesJson() const {
    const TableAudit a = auditTablesDetail();
    std::ostringstream o;
    o << "{\"diff\":" << a.total() << ",\"cells\":" << a.cells << ",\"emptyCell\":" << a.empty_cell
      << ",\"badRange\":" << a.bad_range << ",\"overlap\":" << a.overlap
      << ",\"noMark\":" << a.block_without_mark << ",\"pathMismatch\":" << a.path_mismatch
      << ",\"badSpan\":" << a.bad_span << ",\"stray\":" << a.blocks_in_no_cell
      << ",\"firstCell\":" << a.first_cell << "}";
    return pk::checked(pk::audit_tables::kSpec, o.str());
}

/**
 * 表格结构自审的实现（判据见头文件的 auditTables）。
 *
 * 块与格两份记录逐块对照：格说"我有哪些块"，块说"我在哪一格"。嵌套表的两层会同时
 *   认领内层那些块，那是合法重叠 —— 只有同一张表的格互相重叠才是错，跨表则要求一边
 *   严格包含另一边（否则就是"一半重叠"，两份记录都没法用）。
 */
EditorEngine::TableAudit EditorEngine::auditTablesDetail() const {
    TableAudit a;
    const int n_blocks = static_cast<int>(blocks().size());
    std::vector<int> owner(static_cast<size_t>(n_blocks), -1);
    struct Ref {
        int table = -1;
        int lo = 0;
        int hi = -1;  // 闭区间
    };
    std::vector<Ref> refs;

    for (size_t ti = 0; ti < tables_.size(); ++ti) {
        const LayoutTable& t = tables_[ti];
        for (const LayoutCell& c : t.cells) {
            const int ci = static_cast<int>(refs.size());
            ++a.cells;
            if (c.block_count <= 0) {
                ++a.empty_cell;
                if (a.first_cell < 0) a.first_cell = ci;
                refs.push_back(Ref{static_cast<int>(ti), 0, -1});
                continue;
            }
            const int last_b = c.first_block + c.block_count - 1;
            if (c.first_block < 0 || last_b >= n_blocks) {
                ++a.bad_range;
                if (a.first_cell < 0) a.first_cell = ci;
                refs.push_back(Ref{static_cast<int>(ti), c.first_block, last_b});
                continue;
            }
            if (c.row < 0 || c.cell < 0 || c.row + c.row_span > t.rows ||
                c.cell + c.col_span > t.cols) {
                ++a.bad_span;
                if (a.first_cell < 0) a.first_cell = ci;
            }
            refs.push_back(Ref{static_cast<int>(ti), c.first_block, last_b});
            for (int b = c.first_block; b <= last_b; ++b) {
                const size_t k = static_cast<size_t>(b);
                const int prev = owner[k];
                if (prev < 0) {
                    owner[k] = ci;
                    continue;
                }
                const Ref& p = refs[static_cast<size_t>(prev)];
                const Ref& n = refs[static_cast<size_t>(ci)];
                const bool nested =
                    (p.lo <= n.lo && p.hi >= n.hi) || (n.lo <= p.lo && n.hi >= p.hi);
                if (p.table == n.table || !nested) ++a.overlap;
                // 记最内层那一格（块数少的），后面的对照要用它。
                if (n.hi - n.lo < p.hi - p.lo) owner[k] = ci;
            }
        }
    }

    // 块那一侧：cell_path 的最内层一段应当指向认领它的那一格。
    std::map<std::tuple<uint64_t, int, int>, int> by_slot;
    {
        size_t ci = 0;
        for (const LayoutTable& t : tables_) {
            for (const LayoutCell& c : t.cells) {
                by_slot[std::make_tuple(t.handle, c.row, c.cell)] = static_cast<int>(ci);
                ++ci;
            }
        }
    }
    const bool marks_known = block_src_.size() == blocks().size();
    for (int i = 0; i < n_blocks; ++i) {
        const size_t k = static_cast<size_t>(i);
        const std::vector<CellSlot>& path = blocks()[k].cell_path;
        if (path.empty()) {
            if (owner[k] >= 0) ++a.blocks_in_no_cell;
        } else {
            const CellSlot& in = path.back();
            const auto it = by_slot.find(std::make_tuple(in.table, in.row, in.cell));
            const int want = (it == by_slot.end()) ? -1 : it->second;
            if (want < 0 || want != owner[k]) ++a.path_mismatch;
        }
        // 文档最后一块可以没有结束标记（末尾未结束），格里的块不行。
        if (owner[k] >= 0 && i + 1 < n_blocks && marks_known &&
            block_src_[k].mark == kInvalidNodeId) {
            ++a.block_without_mark;
        }
    }
    return a;
}

/**
 * 排版自审：拿"不用缓存的全量重排"当对照，与当前 layout_ 逐字段比。
 * 为什么要比逐字段而不是只看行数/字形数：增量最典型的错法是"少排了一段"
 * 或者"前缀多留了一段"，两者都只差几行 —— 看总数看不出来，看内容才看得见。
 */
int EditorEngine::auditLayout() {
    if (blocks().empty()) return 0;
    const LayoutResult ref = layoutDocument(fonts_, blocks(), cfg_.page, tables_);
    return auditLayoutPair(layout_, ref).total();
}

std::string EditorEngine::auditLayoutJson() {
    std::ostringstream o;
    if (blocks().empty())
        return pk::checked(pk::audit_layout::kSpec, "{\"diff\":0,\"note\":\"empty\"}");

    const auto t0 = Clock::now();
    const LayoutResult ref = layoutDocument(fonts_, blocks(), cfg_.page, tables_);
    const double full_ms = msSince(t0);
    const LayoutAudit a = auditLayoutPair(layout_, ref);

    o << "{\"diff\":" << a.total();
    o << ",\"glyphDiff\":" << a.glyph_diff << ",\"lineDiff\":" << a.line_diff;
    o << ",\"paraDiff\":" << a.para_diff << ",\"startsDiff\":" << a.cluster_starts_diff;
    o << ",\"tableDiff\":" << a.table_diff;
    o << ",\"statsSame\":" << (a.stats_same ? "true" : "false");
    o << ",\"first\":{\"glyph\":" << a.glyph_first << ",\"line\":" << a.line_first
      << ",\"para\":" << a.para_first << "}";
    o << ",\"glyphs\":" << layout_.glyphs.size() << ",\"lines\":" << layout_.lines.size();
    o << ",\"paras\":" << layout_.para_index.size();
    o << ",\"relaidParagraphs\":" << last_relaid_paragraphs_;
    o << ",\"relaidLines\":" << last_relaid_lines_;
    o << ",\"layoutWasFull\":" << (last_layout_full_ ? "true" : "false");
    // 两个耗时都来自本次调用：curMs 是刚才那次增量续排，fullMs 是审计顺便做的
    // 全量重排 —— 分母与分子同源，不掺历史数字。
    o << ",\"curMs\":" << layout_.stats.layout_ms << ",\"fullMs\":" << full_ms;
    o << "}";
    return pk::checked(pk::audit_layout::kSpec, o.str());
}

std::string EditorEngine::auditGlyphPagesJson() const {
    const std::vector<PlacedGlyph>& gs = layout_.glyphs;
    const int pages = std::max(0, layout_.stats.page_count);
    std::ostringstream o;

    // 参照物：一次全篇扫描，把每个字形登记到它自己声明的页上。
    //   不用"逐页再全篇筛"（那是 O(页数 x 全篇)，160 页 x 8 万字形 = 一千多万次），
    //   一次扫描 O(全篇) 就够，成本从分钟级降到毫秒级。
    std::vector<int> ref(static_cast<size_t>(pages), 0);
    int stray = 0;  // 页号落在 [0, pages) 之外的字形：那是排版自己的错，单独计数
    for (const PlacedGlyph& g : gs) {
        if (g.page < 0 || g.page >= pages) {
            ++stray;
            continue;
        }
        ++ref[static_cast<size_t>(g.page)];
    }

    // 逐页验两条：区间长度等于该页的字形总数（不漏不多），区间内没有别页的字形（不错位）。
    int bad_pages = 0;
    int first_bad = -1;
    int missing = 0;
    int extra = 0;
    for (int p = 0; p < pages; ++p) {
        int b = 0;
        int e = 0;
        glyphRangeOnPage(p, &b, &e);
        const int want = ref[static_cast<size_t>(p)];
        int wrong = 0;
        for (int k = b; k < e; ++k) {
            if (gs[static_cast<size_t>(k)].page != p) ++wrong;
        }
        const int got = (e - b) - wrong;
        if (got != want || wrong != 0) {
            ++bad_pages;
            missing += (want > got) ? (want - got) : 0;
            extra += (got > want) ? (got - want) : 0;
            if (first_bad < 0) first_bad = p;
        }
    }

    o << "{\"diff\":" << (bad_pages + stray);
    o << ",\"pages\":" << pages << ",\"glyphs\":" << gs.size();
    o << ",\"badPages\":" << bad_pages << ",\"firstBadPage\":" << first_bad;
    o << ",\"missing\":" << missing << ",\"extra\":" << extra << ",\"stray\":" << stray;
    o << "}";
    return pk::checked(pk::audit_glyph_pages::kSpec, o.str());
}

int EditorEngine::auditCompile() const {
    // 没装载过文档就没有"全量编译"这个对照物，没有可比的东西。
    if (!loaded_) return 0;
    const FlowVisibility vis =
        (mode_ == EditMode::Document) ? FlowVisibility::Design : FlowVisibility::Display;
    // 参照物走另一条路：修订转回模型树，再让模型层编译一遍。两边都用投影的话，
    //   自审就退化成"自己跟自己比"，那是没有判据价值的。
    NodeTree mirror;
    rev::toTree(document(), &mirror);
    const CompileResult ref = compileDocument(mirror, vis, sheet_);
    // 段数不同就没法逐段对齐了：报一个大数（不是 1），让"哪里不符"一眼看得出来。
    if (ref.blocks.size() != blocks_full_.size()) return 1000;
    int d = 0;
    for (size_t i = 0; i < ref.blocks.size(); ++i) {
        d += diffBlockSpec(blocks_full_[i], ref.blocks[i]);
    }
    // 来源身份只比长度：参照侧的节点号是模型树自己分配的，与修订的节点号不同源，
    //   逐号比较恒不相等。身份契约由下面那段按修订节点号的检查负责。
    if (ref.sources.size() != block_src_.size()) ++d;
    // 控件集合单独数一遍：它是产物的一次派生，而那次派生对"半截边界"是整对丢弃 ——
    //   一条静默降级。产物只错掉一枚边界时，块级差异常常小到看不出来，而界面上控件会
    //   整批消失（"字段没了"就是这么被放大出来的）。独立数一遍，让这一类差异可读。
    {
        const std::vector<ControlRange> want = pairedControlRanges(ref.blocks);
        if (want.size() != static_cast<size_t>(controls_.size())) {
            ++d;
        } else {
            for (size_t i = 0; i < want.size(); ++i) {
                const Control& got = controls_.at(static_cast<int>(i));
                if (got.range.start != want[i].start || got.range.end != want[i].end) ++d;
            }
        }
    }
    // 来源身份的契约也要盯住：sources 的 first 是"这一段第一枚吃进内容的元素"，所以段的
    //   流起点必须落在这个元素的区间里。这一条破了，任何靠 first 判归属的判据都会跟着失效，
    //   而产物本身可能逐字段全对 —— 闭括号形态漏记时就是这样。这里独立重算段起点
    //   （与 rebuildParagraphFromTree 同一口径，刻意不共享代码），审计的价值就来自独立。
    {
        const rev::FlowIndex idx(document(), vis);
        for (size_t i = 0; i < block_src_.size(); ++i) {
            int seg_start = -1;
            if (i == 0) {
                if (!idx.intervalOf(block_src_[0].first, &seg_start, nullptr)) {
                    ++d;
                    continue;
                }
            } else if (!idx.intervalOf(block_src_[i - 1].mark, nullptr, &seg_start)) {
                ++d;
                continue;
            }
            int lo = 0;
            int hi = 0;
            // seg_start 落在本块第一个元素之前是合法的 —— 表格的格边界（离开上一格、
            //   进入这一格）就夹在两者之间。所以只要求它不越过这个元素的末尾：越过了
            //   说明段起点跑进了下一块的内容里。
            (void)lo;
            if (!idx.intervalOf(block_src_[i].first, &lo, &hi) || seg_start >= hi) {
                ++d;
            }
        }
        // 块区间与流索引对拍：带标记的块，内容末尾必须正好落在自己那枚标记的起点上。
        //   这一条是独立参照物 —— block_flow_ 由投影的游标累加而来，标记位置由流索引数出，
        //   两条路算同一个数。格边界少算一位、上游变长没跟上，都会在这里现形，而只看
        //   "段起点不越过首元素末尾"那一条是看不出来的。
        if (block_flow_.size() != block_src_.size() ||
            block_flow_span_.size() != block_src_.size()) {
            ++d;
        } else {
            for (size_t i = 0; i < block_src_.size(); ++i) {
                if (block_src_[i].mark == kInvalidNodeId) continue;
                int mark_first = -1;
                if (!idx.intervalOf(block_src_[i].mark, &mark_first, nullptr)) {
                    ++d;
                    continue;
                }
                if (block_flow_[i] + block_flow_span_[i] != mark_first) ++d;
            }
        }
    }
    return d;
}

std::string EditorEngine::auditCompileJson() {
    if (!loaded_)
        return pk::checked(pk::audit_compile::kSpec, "{\"diff\":0,\"note\":\"not-loaded\"}");
    const auto t0 = Clock::now();
    const int d = auditCompile();
    const double full_ms = msSince(t0);
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    // curMs 是本轮提交实际花在编译上的时间（rebuildFromTree 记的），fullMs 是这一次
    //   整树全量编译 —— 两者同源，所以"增量省了多少"是一个可当场证伪的比值。
    o << "{\"diff\":" << d << ",\"paras\":" << blocks_full_.size()
      << ",\"curMs\":" << edit_stats_.rebuild_compile_ms << ",\"fullMs\":" << full_ms << "}";
    return pk::checked(pk::audit_compile::kSpec, o.str());
}

}  // namespace heditor
