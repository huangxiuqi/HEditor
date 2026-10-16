/**
 * 审计判据：增量结果与全量结果的逐字段对照。
 *
 * 判据的正身在这里，调用点在测试与浏览器探针。分开的理由是它只被验证路径用到、
 * 不参与正常编辑 —— 混在编辑代码里会让"这段到底跑不跑"变得不清楚。
 */

#include "core/engine/engine.h"

#include "core/text/writing_mode.h"
#include "core/utils/hash.h"
#include "core/utils/time.h"
#include "core/utils/units.h"

#include <hb.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sstream>
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
        !samePos(a.bracket_axis_pt, b.bracket_axis_pt) || a.clusters.size() != b.clusters.size()) {
        return false;
    }
    for (size_t i = 0; i < a.clusters.size(); ++i) {
        const LineCluster& x = a.clusters[i];
        const LineCluster& y = b.clusters[i];
        if (x.cp != y.cp || !samePos(x.inline_pt, y.inline_pt) || x.placeholder != y.placeholder ||
            x.bracket_side != y.bracket_side || x.read_only != y.read_only) {
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
    bool stats_same = true;
    int glyph_first = -1;
    int line_first = -1;
    int para_first = -1;

    int total() const {
        return glyph_diff + line_diff + para_diff + cluster_starts_diff + (stats_same ? 0 : 1);
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
            if (x.cp != y.cp || x.kind != y.kind || x.owner != y.owner ||
                x.read_only != y.read_only) {
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
 * 产物里"成对的控件边界"个数。
 *
 * 口径与 exportControlsFromBlocks 一致：按 owner 聚合两枚边界，只有成对的才算一个控件。
 * 半截边界在那里会被整对丢掉 —— 静默降级，所以审计必须独立数一遍，否则那一类差异
 * 只剩块级那点小数，看不出"控件整批消失"。
 */
int pairedControlCount(const std::vector<BlockSpec>& blocks) {
    struct Pair {
        bool has_start = false;
        bool has_end = false;
    };
    std::unordered_map<int, Pair> acc;
    for (const BlockSpec& b : blocks) {
        for (const InlineObject& o : b.objects) {
            Pair& p = acc[o.owner];
            if (o.kind == InlineObjectKind::ControlStart) {
                p.has_start = true;
            } else {
                p.has_end = true;
            }
        }
    }
    int n = 0;
    for (const auto& kv : acc) {
        if (kv.second.has_start && kv.second.has_end) ++n;
    }
    return n;
}

}  // namespace

/**
 * 帧自审：这一帧 == "整帧重画一遍" 的结果吗？
 * 为什么不满足于"画布 == 引擎帧"（selfCheck）：
 * 那个判据只能抓"上屏贴错"。如果引擎帧本身就是错的（比如某个旧标记
 * 没被擦掉），画布忠实反映了它，两边当然相等 —— 判据全绿而屏幕是错的。
 * 这正是"光标走了、旧方括号还在"那类问题一直没被抓住的原因之一。
 * 这里用的是和单测 [10]（字形缓存等价性）同一个套路：
 * 增量路径的结果，必须逐字节等于从零重来的结果。
 * "从零重来"由 frame_needs_full_refresh_ 强制：底图整块重建、
 * 整帧从底图搬、再画标记与光标 —— 那就是这一帧的"真值"。
 * 副作用（有意为之）：跑完这一帧就处于真值状态，因此它同时也是自愈手段。
 * 所以它对用户也安全：出问题时跑一次，画面立刻正确，同时拿到取证数据。
 */
std::string EditorEngine::auditFrameJson() {
    const int fw = frameWidth();
    const int fh = frameHeight();
    std::ostringstream o;
    if (fw <= 0 || fh <= 0) return "{\"diff\":-1,\"box\":null}";

    const size_t n = frameBytes();
    const std::vector<uint8_t> before(frame(), frame() + n);

    frame_needs_full_refresh_ = true;  // ← 强制"从零重来"
    repaint();

    const uint8_t* now = frame();
    long long diff = 0;
    int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
    for (int y = 0; y < fh; ++y) {
        for (int x = 0; x < fw; ++x) {
            const size_t off =
                (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u;
            if (before[off] == now[off] && before[off + 1] == now[off + 1] &&
                before[off + 2] == now[off + 2]) {
                continue;
            }
            ++diff;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    o << "{\"diff\":" << diff << ",\"frameW\":" << fw << ",\"frameH\":" << fh;
    if (diff > 0) {
        o << ",\"box\":[" << minx << "," << miny << "," << maxx << "," << maxy << "]";
    } else {
        o << ",\"box\":null";
    }
    o << "}";
    return o.str();
}

/**
 * 底图自审：把底图按"从零全建"重来一遍，与现在这一份逐字节比。
 *
 * 与 auditFrameJson 的分工：那一条比的是帧，而帧是从底图搬的 ——
 *   底图错了，帧与"整帧重贴"照样一致，两条都绿、画面却是错的。
 *   换窗时的"平移复用重叠页"只动底图，正是这一条要看的对象。
 *
 * 做法：把 base_valid_ 置假再 repaint()，就会走"底图全建"那一支。
 *   同一窗口、同一布局、同一字形缓存下，全建是确定性的，所以两边必须逐字节相同。
 * 副作用（与 auditFrameJson 同一条纪律）：跑完底图就是真值，本身也是自愈手段。
 */
std::string EditorEngine::auditBaseJson() {
    const int bw = base_.width();
    const int bh = base_.height();
    std::ostringstream o;
    if (bw <= 0 || bh <= 0 || !base_valid_) return "{\"diff\":-1,\"box\":null}";

    const size_t n = base_.byteSize();
    const std::vector<uint8_t> before(base_.data(), base_.data() + n);

    base_valid_ = false;   // ← 强制"底图全建"
    repaint();

    const uint8_t* now = base_.data();
    long long diff = 0;
    int minx = 1 << 30;
    int miny = 1 << 30;
    int maxx = -1;
    int maxy = -1;
    for (int y = 0; y < bh; ++y) {
        for (int x = 0; x < bw; ++x) {
            const size_t off =
                (static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x)) * 4u;
            if (before[off] == now[off] && before[off + 1] == now[off + 1] &&
                before[off + 2] == now[off + 2]) {
                continue;
            }
            ++diff;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    o << "{\"diff\":" << diff << ",\"frameW\":" << bw << ",\"frameH\":" << bh;
    if (diff > 0) {
        o << ",\"box\":[" << minx << "," << miny << "," << maxx << "," << maxy << "]";
    } else {
        o << ",\"box\":null";
    }
    o << "}";
    return o.str();
}

/**
 * 排版自审：拿"不用缓存的全量重排"当对照，与当前 layout_ 逐字段比。
 * 为什么要比逐字段而不是只看行数/字形数：增量最典型的错法是"少排了一段"
 * 或者"前缀多留了一段"，两者都只差几行 —— 看总数看不出来，看内容才看得见。
 */
int EditorEngine::auditLayout() {
    if (blocks_.empty()) return 0;
    const LayoutResult ref = layoutDocument(fonts_, blocks_, cfg_.page);
    return auditLayoutPair(layout_, ref).total();
}

std::string EditorEngine::auditLayoutJson() {
    std::ostringstream o;
    if (blocks_.empty()) return "{\"diff\":0,\"note\":\"empty\"}";

    const auto t0 = Clock::now();
    const LayoutResult ref = layoutDocument(fonts_, blocks_, cfg_.page);
    const double full_ms = msSince(t0);
    const LayoutAudit a = auditLayoutPair(layout_, ref);

    o << "{\"diff\":" << a.total();
    o << ",\"glyphDiff\":" << a.glyph_diff << ",\"lineDiff\":" << a.line_diff;
    o << ",\"paraDiff\":" << a.para_diff << ",\"startsDiff\":" << a.cluster_starts_diff;
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
    return o.str();
}

int EditorEngine::auditCompile() const {
    // 没装载过文档就没有"全量编译"这个对照物，没有可比的东西。
    if (!loaded_) return 0;
    const FlowVisibility vis =
        (mode_ == EditMode::Document) ? FlowVisibility::Design : FlowVisibility::Display;
    const CompileResult ref = compileDocument(tree_, vis, sheet_);
    // 段数不同就没法逐段对齐了：报一个大数（不是 1），让"哪里不符"一眼看得出来。
    if (ref.blocks.size() != blocks_full_.size()) return 1000;
    int d = 0;
    for (size_t i = 0; i < ref.blocks.size(); ++i) {
        d += diffBlockSpec(blocks_full_[i], ref.blocks[i]);
    }
    // 来源身份也要比：段的流区间是由标记位置递推出来的，这一项错了只表现为光标落偏一个字，
    //   而段的文本与格式可能全都对 —— 是最难查的一类。
    if (ref.sources.size() != block_src_.size()) {
        ++d;
    } else {
        for (size_t i = 0; i < ref.sources.size(); ++i) {
            if (ref.sources[i].first != block_src_[i].first ||
                ref.sources[i].mark != block_src_[i].mark) {
                ++d;
            }
        }
    }
    // 控件集合单独数一遍：它是产物的一次派生，而那次派生对"半截边界"是整对丢弃 ——
    //   一条静默降级。产物只错掉一枚边界时，块级差异常常小到看不出来，而界面上控件会
    //   整批消失（"字段没了"就是这么被放大出来的）。独立数一遍，让这一类差异可读。
    if (pairedControlCount(ref.blocks) != controls_.size()) ++d;
    // 来源身份的契约也要盯住：sources 的 first 是"这一段第一枚吃进内容的元素"，所以段的
    //   流起点必须落在这个元素的区间里。这一条破了，任何靠 first 判归属的判据都会跟着失效，
    //   而产物本身可能逐字段全对 —— 闭括号形态漏记时就是这样。这里独立重算段起点
    //   （与 rebuildParagraphFromTree 同一口径，刻意不共享代码），审计的价值就来自独立。
    {
        const FlowIndex idx(tree_, vis);
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
            if (!idx.intervalOf(block_src_[i].first, &lo, &hi) || seg_start < lo ||
                seg_start >= hi) {
                ++d;
            }
        }
    }
    return d;
}

std::string EditorEngine::auditCompileJson() {
    if (!loaded_) return "{\"diff\":0,\"note\":\"not-loaded\"}";
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
    return o.str();
}

}  // namespace heditor
