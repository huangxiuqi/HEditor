/**
 * 可证伪的整形与布局单测。
 * 目的不是"跑通"，而是把关键结论变成断言，让"整形真的发生了""竖排度量真的取到了"
 * 这类事情不靠肉眼、也不靠"能编译"来判定。
 * 失败时退出码非 0，并打印期望值/实际值。
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "core/layout/block_layout.h"
#include "core/layout/block_pagination.h"
#include "core/layout/fragment.h"
#include "core/layout/layout.h"
#include "core/text/fallback.h"
#include "core/text/font_source.h"
#include "core/text/shaper.h"
#include "core/utils/hash.h"
#include "core/utils/units.h"
#include "core/utils/utf8.h"
#include "fixtures/doc_fixtures.h"

#include <hb.h>

#ifndef HEDITOR_NATIVE_ASSET_DIR
#define HEDITOR_NATIVE_ASSET_DIR "."
#endif

using namespace heditor;

namespace {

int g_failures = 0;
int g_checks = 0;

void expectTrue(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

template <typename A, typename B>
void expectNear(A a, B b, double tol, const std::string& what) {
    ++g_checks;
    const double diff = std::fabs(static_cast<double>(a) - static_cast<double>(b));
    if (diff <= tol) {
        std::printf("  [PASS] %s   (%.4f ≈ %.4f)\n", what.c_str(), static_cast<double>(a),
                    static_cast<double>(b));
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s   期望 %.4f，实际 %.4f（偏差 %.4f > %.4f）\n", what.c_str(),
                    static_cast<double>(b), static_cast<double>(a), diff, tol);
    }
}

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

/**
 * 把 BlockSpec 的段落格式折成 doc 的段落覆盖。
 * @param b 块
 * @returns 段落覆盖
 */
ParaFormatPatch paraPatchOf(const BlockSpec& b) {
    ParaFormatPatch p;
    p.font_index = b.font_index;
    p.size = Length::abs(ptToEmu(b.size_pt));
    p.wm = b.wm;
    p.line_rule = b.line_spacing.rule;
    p.line_factor = b.line_spacing.factor;
    if (b.line_spacing.exact_pt > 0.0f)
        p.line_exact = Length::abs(ptToEmu(b.line_spacing.exact_pt));
    p.space_before = Length::abs(ptToEmu(b.space_before_pt));
    p.space_after = Length::abs(ptToEmu(b.space_after_pt));
    p.page_break_before = b.page_break_before;
    p.language = b.language;
    p.color = b.color;
    if (!b.fallback_font_indices.empty()) p.fallback_font_indices = b.fallback_font_indices;
    return p;
}

/**
 * 把 BlockSpec 的默认字符格式折成 doc 的字符标记。
 * @param b 块
 * @returns 字符标记
 */
CharFormat marksOf(const BlockSpec& b) {
    CharFormat m;
    m.font_index = b.font_index;
    m.size = Length::abs(ptToEmu(b.size_pt));
    m.color = b.color;
    m.bold = b.weight >= 700.0f;
    m.italic = b.italic;
    m.underline = b.underline;
    return m;
}

/**
 * 用 BlockSpec 造修订：文本按 FormatRun 切，段落格式与字符格式来自块。
 * @param blocks 块流
 * @returns 修订
 */
doc::Doc docFromBlocks(const std::vector<BlockSpec>& blocks) {
    NodeId next = 1;
    std::vector<doc::Ref> section_kids;
    for (const BlockSpec& b : blocks) {
        const CharFormat base = marksOf(b);
        if (b.runs.empty()) {
            if (!b.cp_text.empty()) {
                doc::RunPayload run;
                run.text = b.cp_text;
                run.marks = base;
                section_kids.push_back(
                    doc::makeNode(next++, NodeKind::TextRun, std::move(run), {}));
            }
        } else {
            for (const FormatRun& fr : b.runs) {
                const size_t lo = std::min<size_t>(fr.cp_begin, b.cp_text.size());
                const size_t hi = std::min<size_t>(fr.cp_end, b.cp_text.size());
                if (hi <= lo) continue;
                CharFormat m;
                m.font_index = fr.font_index;
                m.size = Length::abs(ptToEmu(fr.size_pt));
                m.color = fr.color;
                m.bold = fr.weight >= 700.0f;
                m.italic = fr.italic;
                m.underline = fr.underline;
                doc::RunPayload run;
                run.text.assign(b.cp_text.begin() + static_cast<long>(lo),
                                b.cp_text.begin() + static_cast<long>(hi));
                run.marks = m;
                section_kids.push_back(
                    doc::makeNode(next++, NodeKind::TextRun, std::move(run), {}));
            }
        }
        doc::MarkPayload mark;
        mark.para = paraPatchOf(b);
        mark.marks = base;
        section_kids.push_back(doc::makeNode(next++, NodeKind::ParagraphMark, std::move(mark), {}));
    }
    doc::Ref section = doc::makeNode(next++, NodeKind::Section, {}, std::move(section_kids));
    doc::Ref root = doc::makeNode(next++, NodeKind::Document, {}, {std::move(section)});
    return doc::adopt(std::move(root), next);
}

/**
 * 浮动图场景的排版指纹：行盒、字形与浮动几何逐项混入。
 *
 * 字体指针不进指纹（跨进程地址不同），字形按内容与坐标比。它锁的是"绕排路径切换前后
 * 逐位相同"，不是"绕排是对的"。
 * @param r 一次排版的结果
 * @return 64 位指纹
 */
uint64_t floatCaseDigest(const LayoutResult& r) {
    uint64_t h = kFnvBasis;
    auto u = [&](uint64_t v) { h = fnvMix(h, v); };
    auto f = [&](float v) { h = fnvMixFloat(h, v); };
    u(r.lines.size());
    for (const LineBox& lb : r.lines) {
        u(static_cast<uint64_t>(lb.paragraph));
        u(static_cast<uint64_t>(lb.line));
        u(static_cast<uint64_t>(lb.page));
        u(lb.vertical ? 1u : 0u);
        f(lb.size_pt);
        f(lb.block_lo_pt);
        f(lb.block_hi_pt);
        f(lb.lead_top_pt);
        f(lb.ascent_pt);
        f(lb.descent_pt);
        f(lb.advance_pt);
        f(lb.inline_origin_pt);
        f(lb.inline_end_pt);
        f(lb.bracket_axis_pt);
        u(lb.first_cp);
        u(lb.last_cp);
        u(lb.spans.size());
        for (const LineBox::Span& s : lb.spans) {
            f(s.lo_pt);
            f(s.hi_pt);
        }
        u(lb.clusters.size());
        for (const LineCluster& c : lb.clusters) {
            u(c.cp);
            f(c.inline_pt);
            u(c.placeholder ? 1u : 0u);
            u(static_cast<uint64_t>(c.bracket_side));
        }
    }
    u(r.glyphs.size());
    for (const PlacedGlyph& g : r.glyphs) {
        u(g.glyph_id);
        f(g.x_pt);
        f(g.y_pt);
        u(static_cast<uint64_t>(g.page));
        u(static_cast<uint64_t>(g.line));
        u(g.cluster);
        u(static_cast<uint64_t>(g.paragraph));
        f(g.advance_pt);
    }
    u(r.placed_floats.size());
    for (const PlacedFloat& pf : r.placed_floats) {
        u(pf.handle);
        u(static_cast<uint64_t>(pf.page));
        f(pf.x);
        f(pf.y);
        f(pf.w);
        f(pf.h);
        u(static_cast<uint64_t>(pf.wrap));
        u(pf.content_hash);
    }
    u(static_cast<uint64_t>(r.stats.page_count));
    return h;
}

/**
 * UTF-8 字面量（避免源文件编码在不同工具链上漂移）
 */
const char* kCJK_bingli = "\xE7\x97\x85\xE5\x8E\x86";  // 病历
const char* kCJK_sentence =
    "\xE6\x82\xA3\xE8\x80\x85\xE4\xB8\xBB\xE8\xAF\x89"  // 患者主诉
    "\xEF\xBC\x9A";                                     // ：
const char* kMongol_word =
    "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF";  // mongol
const char* kMongol_fvs = "\xE1\xA0\xA0\xE1\xA0\x8B";                            // ᠠ + FVS1

/**
 * 上标 9（U+2079）。检验报告里的 "10⁹/L" 就靠它 —— 而 NotoSansSC 恰好没有这个码点。
 */
const char* kSup9 = "\xE2\x81\xB9";
const char* kMixedProbe = "10\xE2\x81\xB9/L";  // 10⁹/L

constexpr uint32_t kSup9Cp = 0x2079;
constexpr uint32_t kMongolFvs1 = 0x180B;
constexpr uint32_t kCombiningAcute = 0x0301;
constexpr uint32_t kZwj = 0x200D;

}  // namespace

int main() {
    // 必须无缓冲。
    //   重定向到文件时 MSVC CRT 用整块缓冲（4KB），一旦测试中途崩溃
    //   （段错误/断言弹窗），缓冲区不会被刷新 —— 于是"进程崩溃"和"什么都没输出"
    //   看起来一模一样，最关键的现场（崩溃前跑到哪一节）正好丢掉。
    //   实测代价：一次段错误让本实现完全看不到已经跑过的 5 个小节。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    FontLibrary lib;
    const std::string cjkPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string mnPath =
        joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

    const int cjk = lib.addFace(cjkPath);
    const int mn = lib.addFace(mnPath);
    const int latin = lib.addFace(latinPath);
    if (cjk < 0 || mn < 0 || latin < 0) {
        std::fprintf(stderr, "字体加载失败：%s / %s / %s\n", cjkPath.c_str(), mnPath.c_str(),
                     latinPath.c_str());
        return 2;
    }
    std::printf(
        "字体加载：CJK=index %d (%u glyphs)，蒙文=index %d (%u glyphs)，拉丁=index %d (%u glyphs)\n\n",
        cjk, lib.face(cjk)->glyphCount(), mn, lib.face(mn)->glyphCount(), latin,
        lib.face(latin)->glyphCount());

    std::printf("[0] 字体身份：三种名字查到同一张脸\n");
    {
        const std::string cjkFamily = lib.face(cjk)->familyName();
        const std::string cjkPs = lib.face(cjk)->postScriptName();
        std::printf("     族名 \"%s\"，PostScript 名 \"%s\"\n", cjkFamily.c_str(), cjkPs.c_str());
        expectTrue(!cjkFamily.empty() && !cjkPs.empty(), "字体文件自己报得出两种名字");
        expectTrue(lib.indexOfFamily(cjkFamily) == cjk, "族名查到第一张脸");
        expectTrue(lib.indexOfPostScript(cjkPs) == cjk, "PostScript 名查到同一张脸");
        expectTrue(lib.indexOfFile("NotoSansSC-VF.ttf") == cjk, "文件名查到同一张脸");
        expectTrue(lib.indexOfFile(cjkPath) == -1, "完整路径不算文件名，按名字查不到");
        expectTrue(lib.indexOfName(cjkFamily) == cjk && lib.indexOfName(cjkPs) == cjk &&
                       lib.indexOfName("NotoSansSC-VF.ttf") == cjk,
                   "indexOfName 三种名字都认");
        expectTrue(lib.indexOfName("") == -1 && lib.indexOfName("没有这份") == -1,
                   "空名与没注册的名字都回 -1，不挑一张凑数");
        // 同一份文件装两次会得到两张同族的脸；按族名查必须回先装的那一张，
        //   否则"按名字选字体"的结果会随注册顺序漂移。
        FontLibrary twins;
        const int t0 = twins.addFace(latinPath);
        const int t1 = twins.addFace(latinPath);
        expectTrue(t0 == 0 && t1 == 1, "同一份文件装两次是两张脸");
        expectTrue(twins.indexOfFamily(lib.face(latin)->familyName()) == t0, "同族取先装的那一张");
        expectTrue(
            twins.addFaceFromMemory("bad.ttf", reinterpret_cast<const uint8_t*>("x"), 1) == -1,
            "坏字节装不上脸");
        expectTrue(twins.faceCount() == 2, "装失败的脸不进清单");
    }

    const float kSize = 12.0f;

    std::printf("[1] 中文横排整形\n");
    {
        FontInstance* f = lib.instance(cjk, kSize);
        const ShapedText st = shapeText(f->hb(), kCJK_bingli, WritingMode::HorizontalTB(), "zh");
        std::printf("      glyphs=%zu inline_advance=%.3f pt\n", st.glyphs.size(),
                    st.inline_advance / kFontScalePerPt);

        expectTrue(st.glyphs.size() >= 2, "两个汉字至少产出 2 个字形");
        expectTrue(!st.vertical, "横排：vertical 标记为 false");

        bool allPositive = true;
        for (const auto& g : st.glyphs) allPositive = allPositive && (g.x_advance > 0);
        expectTrue(allPositive, "所有字形的 x_advance > 0");

        // CJK 是等宽全角：两个汉字的推进量应当≈ 2 个字身 = 2 × 字号
        const double totalPt = st.inline_advance / kFontScalePerPt;
        expectNear(totalPt, 2.0 * kSize, 0.6, "两个汉字的推进量 ≈ 2 × 字号（全角等宽）");
    }

    std::printf("\n[2] 竖排两条路线：TTB 竖排变体 vs 侧倒（蒙文）\n");
    {
        FontInstance* f = lib.instance(mn, 16.0f);

        // (a) vertical-rl（orientation=Upright，中日那一套）→ 走 TTB，真取 vmtx
        const ShapedText v = shapeText(f->hb(), kMongol_word, WritingMode::VerticalRL(), "mn");
        std::printf("     TTB (vertical-rl) : glyphs=%zu, y_advance=", v.glyphs.size());
        for (const auto& g : v.glyphs) std::printf("%d ", g.y_advance);
        std::printf("\n");

        expectTrue(!v.glyphs.empty(), "TTB 整形产出字形");
        expectTrue(v.vertical, "TTB：vertical 标记为 true");
        expectTrue(!v.sideways, "TTB：sideways 标记为 false");

        // 从 HarfBuzz 源码推出的强断言：
        //   hb-ot-font.cc 的 hb_ot_get_glyph_v_advances() 对 vmtx 显式取负，
        //   所以 TTB 方向下 y_advance 必须为负。若字体缺 vmtx/vhea，
        //   HarfBuzz 会走 fallback（descender - ascender，同样为负），断言依然成立。
        bool allNeg = true;
        for (const auto& g : v.glyphs) allNeg = allNeg && (g.y_advance < 0);
        expectTrue(allNeg, "TTB：所有字形 y_advance < 0（HarfBuzz 竖排符号约定）");

        // (b) vertical-lr（orientation=SidewaysRight，蒙文）→ 刻意不走 TTB
        const ShapedText s = shapeText(f->hb(), kMongol_word, WritingMode::VerticalLR(), "mn");
        const ShapedText h = shapeText(f->hb(), kMongol_word, WritingMode::HorizontalTB(), "mn");

        std::printf("     Sideways (vertical-lr) : glyphs=%zu, x_advance=", s.glyphs.size());
        for (const auto& g : s.glyphs) std::printf("%d ", g.x_advance);
        std::printf("\n     LTR (horizontal-tb)    : glyphs=%zu, x_advance=", h.glyphs.size());
        for (const auto& g : h.glyphs) std::printf("%d ", g.x_advance);
        std::printf("\n");

        expectTrue(s.sideways, "蒙文 vertical-lr 标记为 sideways（需渲染层顺时针转 90°）");
        expectTrue(!s.vertical, "蒙文 vertical-lr 不走 TTB（否则会换成断开的竖排变体）");

        bool allPos = true;
        for (const auto& g : s.glyphs) allPos = allPos && (g.x_advance > 0);
        expectTrue(allPos, "侧倒：字形用 hmtx 的 x_advance（全为正值）");

        // 侧倒 == 横排整形，字形序列必须逐一相同（证明它确实走的是横排那条路）
        bool sameAsLtr = (s.glyphs.size() == h.glyphs.size());
        if (sameAsLtr)
            for (size_t i = 0; i < s.glyphs.size(); ++i)
                sameAsLtr = sameAsLtr && (s.glyphs[i].glyph_id == h.glyphs[i].glyph_id) &&
                            (s.glyphs[i].x_advance == h.glyphs[i].x_advance);
        expectTrue(sameAsLtr, "侧倒与横排的字形序列逐一相同（走的是同一条整形路径）");

        // 且必须与 TTB 的序列不同 —— 否则说明"侧倒"其实还是走了 TTB
        bool differsFromTtb =
            (s.glyphs.size() != v.glyphs.size()) || (s.glyphs[0].glyph_id != v.glyphs[0].glyph_id);
        if (!differsFromTtb)
            for (size_t i = 0; i < s.glyphs.size(); ++i)
                if (s.glyphs[i].glyph_id != v.glyphs[i].glyph_id) differsFromTtb = true;
        expectTrue(differsFromTtb, "侧倒的字形序列与 TTB 不同（两条路线真的分开了）");

        // 横排总长就是竖排时这个词在列里占的高度
        const double sTotal = s.inline_advance / kFontScalePerPt;
        const double hTotal = h.inline_advance / kFontScalePerPt;
        std::printf("     侧倒总长=%.3f pt，横排总长=%.3f pt\n", sTotal, hTotal);
        expectNear(sTotal, hTotal, 0.001, "侧倒与横排的总推进量相同");

        // 上下文字形：蒙文是连写文字，字形数通常少于码点数
        expectTrue(s.glyphs.size() <= 6, "蒙文 6 个码点产出的字形数不超过 6（存在连字/合形）");

        std::printf("     TTB      glyph ids: ");
        for (const auto& g : v.glyphs) std::printf("%u ", g.glyph_id);
        std::printf("\n     Sideways glyph ids: ");
        for (const auto& g : s.glyphs) std::printf("%u ", g.glyph_id);
        std::printf("\n");
    }

    std::printf("\n[3] 蒙文 FVS（自由变体选择符 U+180B）\n");
    {
        FontInstance* f = lib.instance(mn, 16.0f);
        const ShapedText st = shapeText(f->hb(), kMongol_fvs, WritingMode::VerticalLR(), "mn");
        std::printf("     glyphs=%zu\n", st.glyphs.size());
        expectTrue(!st.glyphs.empty(), "带 FVS 的序列能整形出字形（不会崩）");
    }

    std::printf("\n[4] layoutDocument：三种书写模式混合分页\n");
    {
        EditorEngine engine;
        expectTrue(engine.init({cjkPath, mnPath, latinPath}), "引擎初始化（加载三个字体）");
        engine.setDocument(docFromBlocks(fixtures::sampleDocument(0, 1, 2)));
        engine.relayout();
        const LayoutResult& r = engine.layout();
        std::printf("     pages=%d glyphs=%d lines/cols=%d clusters=%d shape=%.2fms\n",
                    r.stats.page_count, r.stats.glyph_count, r.stats.placed_line_count,
                    r.stats.cluster_count, r.stats.shape_ms);

        expectTrue(r.stats.page_count >= 3, "三个块各自另起一页 → 页数 ≥ 3");
        expectTrue(r.stats.glyph_count > 0, "有字形被定位");
        expectTrue(r.stats.glyph_count == static_cast<int>(r.glyphs.size()),
                   "stats.glyph_count 与 glyphs 数组长度一致");
        expectTrue(r.stats.missing_glyph_count == 0, "整份样例文书跑完回退链后零缺字");

        // 竖排的列序：vertical-lr 的第一列必须在 vertical-rl 的第一列左边
        float lrMinX = 1e9f, rlMinX = 1e9f;
        float lrCluster0X = 0.0f, rlCluster0X = 0.0f;
        bool gotLR = false, gotRL = false;
        for (const auto& pg : r.glyphs) {
            if (!pg.vertical) continue;
            if (pg.page == 1 && pg.cluster == 0 && !gotLR) {
                lrCluster0X = pg.x_pt;
                gotLR = true;
            }
            if (pg.page == 2 && pg.cluster == 0 && !gotRL) {
                rlCluster0X = pg.x_pt;
                gotRL = true;
            }
            if (pg.page == 1) lrMinX = std::min(lrMinX, pg.x_pt);
            if (pg.page == 2) rlMinX = std::min(rlMinX, pg.x_pt);
        }
        std::printf("     vertical-lr 第 0 簇 x=%.1f pt（最小 x=%.1f）\n", lrCluster0X, lrMinX);
        std::printf("     vertical-rl 第 0 簇 x=%.1f pt（最小 x=%.1f）\n", rlCluster0X, rlMinX);

        expectTrue(gotLR && gotRL, "两页竖排正文都能找到 cluster 0 的字形");
        expectTrue(lrCluster0X < rlCluster0X,
                   "vertical-lr 的首列在 vertical-rl 的首列之左（列序方向相反）");

        // 竖排字形必须在页面内容框内
        expectTrue(lrMinX >= engine.config().page.margins.left - 1.0f, "竖排字形没有越出左边距");

        // 竖排所有字形的 x 应该只有少数几个离散值（每列一个）
        std::vector<float> xs;
        for (const auto& pg : r.glyphs)
            if (pg.vertical && pg.page == 1) xs.push_back(std::round(pg.x_pt * 10.0f) / 10.0f);
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::printf("     vertical-lr 出现的列中线 x 值个数（含字形偏移）: %zu\n", xs.size());
        expectTrue(!xs.empty(), "竖排字形有 x 坐标");
    }

    std::printf("\n[5] 字体回退：同一份文本，只换回退链 → 缺字数从 >0 变 0\n");
    {
        // (a) 先证明"缺口真实存在"，否则下面所有断言都可能是测试写错
        const float sz = 12.5f;
        hb_font_t* fCjk = lib.instance(cjk, sz)->hb();
        hb_font_t* fLatin = lib.instance(latin, sz)->hb();
        hb_codepoint_t g1 = 0, g2 = 0;
        const bool cjkHasSup9 = hb_font_get_nominal_glyph(fCjk, kSup9Cp, &g1) && g1 != 0;
        const bool latinHasSup9 = hb_font_get_nominal_glyph(fLatin, kSup9Cp, &g2) && g2 != 0;
        std::printf("     U+2079 在 CJK=%s(gid=%u)，在拉丁=%s(gid=%u)\n", cjkHasSup9 ? "有" : "无",
                    g1, latinHasSup9 ? "有" : "无", g2);
        expectTrue(!cjkHasSup9, "前提：中文主字体确实没有 U+2079（缺口是真的）");
        expectTrue(latinHasSup9, "前提：拉丁回退字体有 U+2079（能兜住）");

        // (b) 哪些码点"必须跟随基字"、不参与缺字判定
        expectTrue(needsOwnGlyph(kSup9Cp), "U+2079 需要自己的字形");
        expectTrue(!needsOwnGlyph(kMongolFvs1), "蒙文 FVS U+180B 不需要独立字形（跟随基字）");
        expectTrue(!needsOwnGlyph(kCombiningAcute), "组合重音 U+0301 不需要独立字形（跟随基字）");
        expectTrue(!needsOwnGlyph(kZwj), "ZWJ U+200D 不需要独立字形（跟随基字）");

        // (c) 切段：chain = {CJK, 拉丁}
        const std::vector<int> chain{cjk, latin};
        const std::vector<FontRun> runs = itemizeByFont(lib, kMixedProbe, chain, sz);
        std::printf("     \"10⁹/L\" 切成 %zu 段：", runs.size());
        for (const auto& r : runs)
            std::printf("[%zu,%zu)→face%d ", r.cp_begin, r.cp_end, r.face_index);
        std::printf("\n");

        expectTrue(runs.size() >= 3, "10⁹/L 至少切出 3 段（10 | ⁹ | /L）");
        bool hasLatinRun = false;
        for (const auto& r : runs) hasLatinRun = hasLatinRun || (r.face_index == latin);
        expectTrue(hasLatinRun, "存在归属于拉丁字体的段（上标 9 被切出来了）");

        // 段的覆盖必须无缝且无重叠，且完整覆盖整串 —— 这是最容易写错的地方。
        // 单位是码点（不再字节）：与 ShapedGlyph::cluster、与文档偏移同一把尺子。
        const size_t probeCps = utf8ToCodepoints(std::string(kMixedProbe)).size();
        bool tiled = !runs.empty() && runs.front().cp_begin == 0 && runs.back().cp_end == probeCps;
        for (size_t i = 1; i < runs.size(); ++i)
            tiled = tiled && (runs[i].cp_begin == runs[i - 1].cp_end);
        expectTrue(tiled, "切段无缝无重叠且完整覆盖整串（按码点计数）");

        // (d) 蒙文侧不误伤：带 FVS 的序列应当整段归蒙文字体（1 段）
        const std::vector<int> mnChain{mn};
        const std::vector<FontRun> mnRuns = itemizeByFont(lib, kMongol_fvs, mnChain, 16.0f);
        std::printf("     蒙文 ᠠ+FVS1 切成 %zu 段\n", mnRuns.size());
        expectTrue(mnRuns.size() == 1, "蒙文 FVS 跟随基字：整串只剩 1 段（未被拆开）");

        // (e) 端到端：关闭回退 vs 开启回退
        EditorEngine eOff, eOn;
        expectTrue(eOff.init({cjkPath, mnPath, latinPath}), "引擎初始化（三个字体）");
        expectTrue(eOn.init({cjkPath, mnPath, latinPath}), "引擎初始化（三个字体）");

        eOff.setDocument(docFromBlocks({fixtures::mixedParagraphBlock(cjk)}));        // 不回退
        eOn.setDocument(docFromBlocks({fixtures::mixedParagraphBlock(cjk, latin)}));  // 回退到拉丁
        eOff.relayout();
        eOn.relayout();

        const int missOff = eOff.stats().missing_glyph_count;
        const int missOn = eOn.stats().missing_glyph_count;
        std::printf("     缺字数：关闭回退=%d，开启回退=%d\n", missOff, missOn);
        std::printf("     字体 run 数：关闭回退=%d，开启回退=%d\n", eOff.stats().font_run_count,
                    eOn.stats().font_run_count);

        expectTrue(missOff > 0, "关闭回退时确实出现 .notdef（豆腐块）");
        expectTrue(missOn == 0, "开启回退后缺字数为 0（豆腐块消失）");
        expectTrue(missOn < missOff, "同一文本、只换回退链 → 缺字数严格下降");
        expectTrue(eOn.stats().font_run_count > eOff.stats().font_run_count,
                   "开启回退后切段数增加（回退真的生效，不是碰巧）");

        // (f) 那个上标 9 的字形，其 font 必须指向拉丁字体实例，而不是中文主字体
        //
        // 这里的写法变了：cluster 现在是段内码点下标（整形已改用
        //   hb_buffer_add_utf32），所以要在码点数组里定位，不能再拿字节位置去比。
        //   这正是"文档偏移 == 整形 cluster == 切段单位"三把尺子合一带来的直接好处：
        //   这里找到的下标，就是 layout 内部用的下标，中间不需要任何换算。
        const std::vector<uint32_t>& cps = eOn.blocks().front().cp_text;
        size_t supPos = cps.size();
        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == kSup9Cp) {
                supPos = i;
                break;
            }
        }
        expectTrue(supPos < cps.size(), "样本文本里能找到 U+2079（按码点定位）");
        const FontInstance* primInst = eOn.fonts().instance(cjk, 12.5f);
        const FontInstance* latinInst = eOn.fonts().instance(latin, 12.5f);
        const PlacedGlyph* supGlyph = nullptr;
        for (const auto& pg : eOn.layout().glyphs)
            if (pg.cluster == supPos) supGlyph = &pg;
        expectTrue(supGlyph != nullptr, "上标 9 对应的字形被定位了");
        if (supGlyph) {
            std::printf("     上标 9 → gid=%u，font=%s\n", supGlyph->glyph_id,
                        supGlyph->font == latinInst ? "拉丁(回退)" : "其它");
            expectTrue(supGlyph->font == latinInst,
                       "上标 9 的字形来自拉丁回退字体，不是中文主字体");
            expectTrue(supGlyph->font != primInst, "上标 9 的字形不来自主字体");
        }

        // (g) 每个 cluster 都必须落在段内码点范围里。
        //
        // 这条断言换过一次含义，值得记一笔：
        //   验的是"cluster 是全串字节偏移，且能被 decodeUtf8 解出码点"；
        //   单位换成码点之后，再验那个就是自欺 —— 所以断言也换成了
        //   "cluster < 码点总数"。单位换了，断言必须跟着换，
        //   否则测试会一直显示通过，却什么都没在验。
        int clusterOk = 0, clusterBad = 0;
        for (const auto& pg : eOn.layout().glyphs) {
            if (pg.cluster < cps.size())
                ++clusterOk;
            else
                ++clusterBad;
        }
        std::printf("     cluster 落在码点范围内：ok=%d bad=%d（段长 %zu 码点）\n", clusterOk,
                    clusterBad, cps.size());
        expectTrue(clusterBad == 0, "回退合并后的 cluster 是段内码点下标，逐字形都落在范围内");
        expectTrue(clusterOk > 0, "确有字形被计入");
    }

    std::printf("\n[6] 侧倒端到端：蒙文竖排的旋转与阅读顺序\n");
    {
        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（三个字体）");

        BlockSpec b;
        b.font_index = mn;
        b.size_pt = 16.0f;
        b.wm = WritingMode::VerticalLR();
        b.line_spacing.factor = 1.8f;
        b.language = "mn";
        b.setText(kMongol_word);
        const std::string word = b.textUtf8();
        e.setDocument(docFromBlocks({b}));
        e.relayout();

        const LayoutResult& r = e.layout();
        expectTrue(!r.glyphs.empty(), "侧倒块产出了字形");

        bool allRot90 = true;
        for (const auto& pg : r.glyphs) allRot90 = allRot90 && (pg.rotate_deg == 90.0f);
        expectTrue(allRot90, "侧倒块里每个字形都带 rotate_deg=90（渲染层据此顺时针转）");

        bool allVertical = true;
        for (const auto& pg : r.glyphs) allVertical = allVertical && pg.vertical;
        expectTrue(allVertical, "侧倒块的字形都标记为 vertical");

        // 阅读顺序：同一列内按 y 升序排列后，cluster 必须非递减。
        // 这条正是"旋转方向取反"的探测器 —— 转错方向会得到倒序的 cluster。
        std::vector<const PlacedGlyph*> col;
        for (const auto& pg : r.glyphs)
            if (pg.line == r.glyphs.front().line) col.push_back(&pg);
        std::sort(col.begin(), col.end(),
                  [](const PlacedGlyph* a, const PlacedGlyph* b) { return a->y_pt < b->y_pt; });

        bool monotonic = true;
        float dyMin = 1e9f;
        for (size_t i = 1; i < col.size(); ++i) {
            if (col[i]->cluster < col[i - 1]->cluster) monotonic = false;
            dyMin = std::min(dyMin, col[i]->y_pt - col[i - 1]->y_pt);
        }
        std::printf("     第 0 列 %zu 个字形，y 升序后 cluster:", col.size());
        for (const auto* p : col) std::printf(" %u", p->cluster);
        std::printf("   最小相邻 Δy=%.2f pt\n", dyMin);
        expectTrue(monotonic, "列内自上而下 cluster 非递减（阅读顺序没被旋转搞反）");
        expectTrue(col.size() >= 2 && dyMin > 0.0f, "列内字形严格向下推进（Δy > 0）");

        // 同一列的基线 x 应当一致（侧倒时基线是一条竖线）
        float xmin = 1e9f, xmax = -1e9f;
        for (const auto* p : col) {
            xmin = std::min(xmin, p->x_pt);
            xmax = std::max(xmax, p->x_pt);
        }
        std::printf("     第 0 列基线 x ∈ [%.2f, %.2f]（列宽 %.2f）\n", xmin, xmax,
                    b.size_pt * b.line_spacing.factor);
        expectTrue(xmax - xmin < 0.5f, "同列字形共用一条基线（x 基本一致 = 确实侧倒在列里）");

        // 整列高度 ≈ 该词横排时的推进量
        const FontInstance* fi = e.fonts().instance(mn, 16.0f);
        const ShapedText hw = shapeText(fi->hb(), word, WritingMode::HorizontalTB(), "mn");
        const double expectH = hw.inline_advance / kFontScalePerPt;
        const double actualH = col.back()->y_pt - col.front()->y_pt;
        std::printf("     列内跨度=%.3f pt，横排推进=%.3f pt\n", actualH, expectH);
        expectTrue(actualH > 0.0 && actualH < expectH + 1.0,
                   "列内跨度与横排推进量一致（竖排高度 = 横排宽度）");
    }

    std::printf("\n[7] 竖排列宽逐列算：混排字号时小字列不占大字列的宽度\n");
    {
        // 列宽 = 这一列自己的最大字号 × 行距，不是整段最大字号 —— 与 CSS 竖排同一条
        //   （列宽就是该列的行高）。判据必须两半都有：一半钉"每列宽 = 该列字号 × 行距"，
        //   一半钉"同一段里大字列真的比小字列宽"。少了后一半，列宽取整段最大照样全绿。
        // 列长（一列能装多少）与列宽无关，所以把内容高度压到装不下两个字，就得到
        //   "一个字符一列"，列的归属不再依赖字号 —— 判据因此不脆。
        auto mkVertical = [&](WritingMode wm, int big_n, int small_n, float big_pt,
                              float small_pt) {
            BlockSpec b;
            b.wm = wm;
            b.line_spacing.factor = 1.6f;
            std::string txt;
            for (int i = 0; i < big_n; ++i) txt += "汉";
            for (int i = 0; i < small_n; ++i) txt += "汉";
            b.setText(txt);
            FormatRun a;
            a.cp_begin = 0;
            a.cp_end = static_cast<uint32_t>(big_n);
            a.font_index = cjk;
            a.size_pt = big_pt;
            a.language = "zh";
            b.runs.push_back(a);
            if (small_n > 0) {
                FormatRun c;
                c.cp_begin = static_cast<uint32_t>(big_n);
                c.cp_end = static_cast<uint32_t>(big_n + small_n);
                c.font_index = cjk;
                c.size_pt = small_pt;
                c.language = "zh";
                b.runs.push_back(c);
            }
            return b;
        };

        PageMetrics page;
        page.width_pt = 110.0f;
        page.height_pt = 20.0f;
        page.margins = PageMargins::all(5.0f);
        const float cx = page.margins.left;
        const float cw = page.contentWidthPt();
        const float big_pt = 24.0f;
        const float small_pt = 12.0f;
        const float factor = 1.6f;
        // 列宽 = 单倍行高 × 倍数，单倍行高是字体自己的 ascent + descent。
        auto colWidthOf = [&](float size_pt) {
            const FontInstance* f = lib.instance(cjk, size_pt);
            const float natural = f ? (f->ascenderPt() + f->descenderPt()) : size_pt;
            return natural * factor;
        };

        const LayoutResult r = layoutDocument(
            lib, {mkVertical(WritingMode::VerticalRL(), 4, 4, big_pt, small_pt)}, page);
        std::vector<const LineBox*> cols;
        for (const LineBox& lb : r.lines)
            if (lb.vertical) cols.push_back(&lb);
        expectTrue(cols.size() == 8, "8 个字符各占一列（列长只有 10 磅，一列装不下第二个字）");

        int bad_size = 0;
        int bad_w = 0;
        int bad_box = 0;
        float big_w = 0.0f;
        float small_w = 0.0f;
        for (const LineBox* lb : cols) {
            const bool big = lb->first_cp < 4;
            const float want = big ? big_pt : small_pt;
            const float want_w = colWidthOf(want);
            const float w = lb->block_hi_pt - lb->block_lo_pt;
            if (std::fabs(lb->size_pt - want) > 0.01f) ++bad_size;
            if (std::fabs(w - want_w) > 0.05f) ++bad_w;
            if (lb->block_lo_pt < cx - 0.01f || lb->block_hi_pt > cx + cw + 0.01f) ++bad_box;
            if (big) {
                big_w = w;
            } else {
                small_w = w;
            }
        }
        expectTrue(bad_size == 0, "每一列的字号都等于本列内容的最大字号（8 列）");
        expectTrue(bad_w == 0, "每一列的列宽都等于本列字号 × 行距（逐列算，不是整段最大）");
        expectTrue(bad_box == 0, "所有列都落在内容框里（列位按前缀和推进，没有越界）");
        std::printf("     大字列宽 %.2f pt，小字列宽 %.2f pt\n", big_w, small_w);
        expectTrue(big_w > small_w + 1.0f, "大字列真的比小字列宽（取整段最大字号时两者会相等）");

        int overlap = 0;
        int order_bad = 0;
        for (const LineBox* a : cols) {
            for (const LineBox* b : cols) {
                if (a == b || a->page != b->page) continue;
                if (a->block_lo_pt < b->block_hi_pt - 0.01f &&
                    b->block_lo_pt < a->block_hi_pt - 0.01f) {
                    ++overlap;
                }
                // 中日竖排：列序自右向左 ⇒ 列号小的那一列更靠右。
                if (a->line < b->line && a->block_hi_pt < b->block_hi_pt - 0.01f) ++order_bad;
            }
        }
        expectTrue(overlap == 0, "同一页上的列两两不重叠");
        expectTrue(order_bad == 0, "中日竖排：列号小的更靠右（列序方向没被前缀和带反）");

        // 反向一半：整段一个字号 ⇒ 所有列宽相等，且等于单倍行高 × 行距。
        const LayoutResult r1 = layoutDocument(
            lib, {mkVertical(WritingMode::VerticalRL(), 8, 0, small_pt, 0.0f)}, page);
        std::vector<float> widths;
        for (const LineBox& lb : r1.lines) {
            if (!lb.vertical) continue;
            widths.push_back(std::round((lb.block_hi_pt - lb.block_lo_pt) * 100.0f) / 100.0f);
        }
        std::sort(widths.begin(), widths.end());
        widths.erase(std::unique(widths.begin(), widths.end()), widths.end());
        expectTrue(widths.size() == 1 && std::fabs(widths.front() - colWidthOf(small_pt)) < 0.05f,
                   "整段一个字号：所有列宽相等且等于单倍行高 × 行距（与逐列算之前逐字段相同）");

        // 列序的正面判据也要有一份：蒙文（LTR）的第一列在最左，中日（RTL）在最右。
        const LayoutResult rl = layoutDocument(
            lib, {mkVertical(WritingMode::VerticalLR(), 4, 4, big_pt, small_pt)}, page);
        float lr_first_lo = 1e9f;
        float rl_first_hi = -1e9f;
        for (const LineBox& lb : rl.lines) {
            if (lb.vertical && lb.line == 0) lr_first_lo = lb.block_lo_pt;
        }
        for (const LineBox* lb : cols) {
            if (lb->line == 0) rl_first_hi = lb->block_hi_pt;
        }
        expectTrue(lr_first_lo < cx + big_pt * factor + 0.05f,
                   "蒙文竖排的第一列贴着内容区左边（列自左向右）");
        expectTrue(rl_first_hi > cx + cw - big_pt * factor - 0.05f,
                   "中日竖排的第一列贴着内容区右边（列自右向左）");
    }
    std::printf("\n[8] 行内原子：不可断的盒子参与断行\n");
    {
        // 原子是"有宽高、不可断"的行内盒子（迁移路线阶段 4）。三条判据各有反向一半：
        //   宽度真的进了行宽（不只看对象表里有没有这一项）、高度真的进了行盒（不是跟着
        //   字号走）、超宽时整枚换行而不是被拆开。少了反向那一半，判据会在"原子被完全
        //   忽略"时照样全绿。
        auto mkAtom = [](uint32_t cp, float w_pt, float h_pt) {
            InlineObject o;
            o.cp = cp;
            o.kind = InlineObjectKind::Atom;
            o.owner = 7;
            o.atom = InlineAtom::Image;
            o.w_pt = w_pt;
            o.h_pt = h_pt;
            o.content_hash = 99;
            return o;
        };
        auto mkPara = [&](const std::string& txt, std::vector<InlineObject> objs, float size_pt) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = size_pt;
            b.line_spacing.factor = 1.6f;
            b.space_after_pt = 0.0f;
            b.setText(txt);
            b.objects = std::move(objs);
            return b;
        };
        // 一行里那个原子的推进（磅）；行里没有原子时返回 0。
        auto atomAdvance = [](const LineBox& lb, int* count) -> float {
            float adv = 0.0f;
            for (size_t i = 0; i < lb.clusters.size(); ++i) {
                if (lb.clusters[i].atom == InlineAtom::None) continue;
                ++(*count);
                adv = (i + 1 < lb.clusters.size())
                          ? (lb.clusters[i + 1].inline_pt - lb.clusters[i].inline_pt)
                          : (lb.inline_end_pt - lb.clusters[i].inline_pt);
            }
            return adv;
        };
        auto textAdvance = [](const LineBox& lb) -> float {
            float adv = 0.0f;
            for (size_t i = 0; i < lb.clusters.size(); ++i) {
                if (lb.clusters[i].atom != InlineAtom::None) continue;
                adv += (i + 1 < lb.clusters.size())
                           ? (lb.clusters[i + 1].inline_pt - lb.clusters[i].inline_pt)
                           : (lb.inline_end_pt - lb.clusters[i].inline_pt);
            }
            return adv;
        };

        PageMetrics page;
        page.width_pt = 200.0f;
        page.height_pt = 400.0f;
        page.margins = PageMargins::all(10.0f);
        const float box_w = 60.0f;
        const float box_h = 40.0f;

        // 一段 "汉字汉字"（四个 12 磅的汉字）+ 一枚 60x40 的原子，锚在第三个字之前。
        const LayoutResult r =
            layoutDocument(lib, {mkPara("汉字汉字", {mkAtom(2, box_w, box_h)}, 12.0f)}, page);
        expectTrue(r.lines.size() == 1, "原子段排出一行（内容 48 + 60 = 108 磅 < 180）");
        if (!r.lines.empty()) {
            const LineBox& lb = r.lines[0];
            int atom_count = 0;
            const float adv = atomAdvance(lb, &atom_count);
            expectTrue(atom_count == 1, "原子在行里是一个簇（不可分的最小单位）");
            expectNear(adv, box_w, 0.05, "原子占的宽度就是它自己的盒宽");
            expectNear(lb.inline_end_pt, textAdvance(lb) + box_w, 0.05,
                       "行末位置把原子的宽度算了进去（不只是表里有这一项）");

            // 高度进的是行盒，不是字号：盒高 40 > 字号 x 行距 19.2。
            const float line_h = lb.block_hi_pt - lb.block_lo_pt;
            expectTrue(line_h >= box_h - 0.05f,
                       "行盒高度不小于盒高（40 磅的图把行撑到 40 以上，实际 " +
                           std::to_string(line_h) + "）");

            // 反向一半：把盒高压到 6 磅（比一行 19.2 矮）⇒ 行盒回落到字号 x 行距。
            const LayoutResult r_small =
                layoutDocument(lib, {mkPara("汉字汉字", {mkAtom(2, box_w, 6.0f)}, 12.0f)}, page);
            const float small_h =
                r_small.lines.empty()
                    ? 0.0f
                    : (r_small.lines[0].block_hi_pt - r_small.lines[0].block_lo_pt);
            expectTrue(small_h > 0.0f && small_h < line_h - 1.0f,
                       "盒高变小之后行盒跟着变矮（高度不是从字号来的，实际 " +
                           std::to_string(small_h) + "）");
        }

        // 断行处不可分割：内容宽度压到只放得下两个字（30 磅）⇒ 原子整枚换到下一行，
        //   宽度一分不少。判据只看"原子整枚落在某一行里"，不看它落在第几行 ——
        //   那由断行算法决定，钉死它会让判据在无关的改动上红。
        PageMetrics narrow = page;
        narrow.width_pt = 50.0f;  // 内容宽 30 磅
        const LayoutResult r_narrow =
            layoutDocument(lib, {mkPara("汉字汉字", {mkAtom(2, box_w, box_h)}, 12.0f)}, narrow);
        int lines_with_atom = 0;
        int bad_whole = 0;
        for (const LineBox& l : r_narrow.lines) {
            int n = 0;
            const float adv = atomAdvance(l, &n);
            if (n == 0) continue;
            ++lines_with_atom;
            if (n != 1 || std::fabs(adv - box_w) > 0.05f) ++bad_whole;
        }
        expectTrue(lines_with_atom == 1 && bad_whole == 0,
                   "窄行里原子整枚落在某一行、宽度一分不少（断行处不可分割，跨 " +
                       std::to_string(lines_with_atom) + " 行）");

        // 超宽原子：比整行还宽（60 > 50）也独占一行，不产生空行、不丢内容。
        PageMetrics tiny = page;
        tiny.width_pt = 60.0f;  // 内容宽 50 磅，比盒宽窄
        tiny.margins = PageMargins::all(5.0f);
        const LayoutResult r_wide =
            layoutDocument(lib, {mkPara("汉字汉字", {mkAtom(2, box_w, box_h)}, 12.0f)}, tiny);
        int wide_lines = 0;
        int wide_empty = 0;
        bool saw_atom = false;
        for (const LineBox& l : r_wide.lines) {
            ++wide_lines;
            if (l.clusters.empty()) ++wide_empty;
            for (const LineCluster& c : l.clusters) {
                if (c.atom != InlineAtom::None) saw_atom = true;
            }
        }
        expectTrue(saw_atom && wide_empty == 0 && wide_lines >= 3,
                   "超宽原子独占一行，且不是空行（行数 " + std::to_string(wide_lines) + "，空行 " +
                       std::to_string(wide_empty) + "）");

        // 反向一半：没有原子时这一段的排版与加原子之前逐字段相同（空表 = 一个都不插）。
        const LayoutResult r_plain = layoutDocument(lib, {mkPara("汉字汉字", {}, 12.0f)}, page);
        expectTrue(r_plain.lines.size() == 1 && r_plain.lines[0].clusters.size() == 4,
                   "对象表为空时行里只有四个字（没有凭空多出占位簇）");

        // 行距不能被盒高吃掉：原子那一行的下一行要落在同样的空隙之后。
        //   量的是"上一行的底边到下一行的顶边"这段空隙 —— 纯文本行是行距减文本高，
        //   原子行必须一样：盒高撑高的是内容，而行距是段落属性。
        //   行高只取 max(行距, 盒高) 时这段空隙与纯文本行对不上（实测差 20.8 磅），
        //   症状就是"带盒子的那一行的行距没了"。
        {
            // 二十个字（240 磅 > 内容宽 180）⇒ 一定折成两行；原子锚在第 2 个字之前，
            //   于是它落在第一行上、第二行纯文本 —— 正是"盒子的下一行"那个位置关系。
            std::string twenty;
            for (int i = 0; i < 10; ++i) twenty += "汉字";
            const LayoutResult rp = layoutDocument(lib, {mkPara(twenty, {}, 12.0f)}, page);
            const LayoutResult ra =
                layoutDocument(lib, {mkPara(twenty, {mkAtom(2, box_w, box_h)}, 12.0f)}, page);
            expectTrue(rp.lines.size() == 2 && ra.lines.size() == 2,
                       "对照段与原子段都排成两行（实际 " + std::to_string(rp.lines.size()) + " / " +
                           std::to_string(ra.lines.size()) + "）");
            if (rp.lines.size() == 2 && ra.lines.size() == 2) {
                const float plain_gap = rp.lines[1].block_lo_pt - rp.lines[0].block_hi_pt;
                const float atom_gap = ra.lines[1].block_lo_pt - ra.lines[0].block_hi_pt;
                expectTrue(plain_gap > 0.0f, "纯文本两行之间有空隙（行距减文本高）");
                expectNear(atom_gap, plain_gap, 0.05,
                           "★ 带原子那一行的下一行落在同样的空隙之后（行距没被盒高吃掉）");
                // 反向一半：原子那一行的行盒要装得下它自己的盒子（盒顶不越出行顶）。
                const LineBox& al = ra.lines[0];
                expectTrue(al.bracket_axis_pt - box_h >= al.block_lo_pt - 0.05f,
                           "原子盒顶不越出它自己那一行的行顶");
            }
        }
    }
    std::printf("\n[按需] 数据源：目录、骨架、按需读\n");
    {
        FontSource src;
        expectTrue(src.open(cjkPath), "数据源能打开文件并解析表目录");
        expectTrue(src.dir().count() > 10,
                   "表目录里不止十张表（实际 " + std::to_string(src.dir().count()) + " 张）");

        const uint32_t glyf = SfntDirectory::tagOf("glyf");
        const uint32_t gvar = SfntDirectory::tagOf("gvar");
        const uint32_t cmap = SfntDirectory::tagOf("cmap");
        const uint32_t hmtx = SfntDirectory::tagOf("hmtx");
        expectTrue(src.dir().find(glyf) != nullptr && src.dir().find(cmap) != nullptr,
                   "目录里能查到 glyf 与 cmap");
        expectTrue(!FontSource::inSkeleton(glyf) && !FontSource::inSkeleton(gvar),
                   "轮廓与变体数据不进骨架（按需要省的就是它们）");
        expectTrue(FontSource::inSkeleton(cmap) && FontSource::inSkeleton(hmtx),
                   "排版要读的小表全进骨架");

        std::vector<uint8_t> skeleton;
        expectTrue(src.buildSkeleton(&skeleton), "骨架能拼出来");
        const double skel_mb = static_cast<double>(skeleton.size()) / 1048576.0;
        const double full_mb = static_cast<double>(src.size()) / 1048576.0;
        std::printf("     骨架 %.3f MB（原字体 %.2f MB）\n", skel_mb, full_mb);
        expectTrue(skel_mb > 0.05 && skel_mb < 2.0, "骨架远小于原字体（上限取 2 MB）");

        // 骨架自己得是一份合法 SFNT：能被 HarfBuzz 打开，且字形数与原字体一致。
        //   这一条是"骨架没拼坏"的直接判据 —— 表目录偏移算错就会在这里露出来。
        hb_blob_t* skel_blob = hb_blob_create(reinterpret_cast<const char*>(skeleton.data()),
                                              static_cast<unsigned>(skeleton.size()),
                                              HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t* skel_face = hb_face_create(skel_blob, 0);
        const unsigned skel_glyphs = skel_face ? hb_face_get_glyph_count(skel_face) : 0;
        const unsigned full_glyphs = lib.face(cjk)->glyphCount();
        if (skel_face) hb_face_destroy(skel_face);
        hb_blob_destroy(skel_blob);
        expectTrue(skel_glyphs == full_glyphs && full_glyphs > 0,
                   "骨架是合法 SFNT 且字形数与原字体一致（" + std::to_string(skel_glyphs) + " vs " +
                       std::to_string(full_glyphs) + "）");

        // 按需读与整份读逐字节相同：偏移算术与块缓存的正确性都在这一条上。
        std::ifstream in(cjkPath, std::ios::binary);
        const std::vector<uint8_t> whole((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        expectTrue(whole.size() == src.size(), "数据源报的大小与文件实际大小一致");

        bool same_bytes = true;
        const uint64_t offsets[] = {0, 1, 12, 63, 64, 4095, 4096, 65535, 65536, 65537};
        for (uint64_t off : offsets) {
            if (off >= whole.size()) continue;
            const size_t n = std::min<size_t>(96, whole.size() - static_cast<size_t>(off));
            std::vector<uint8_t> got(n, 0);
            if (src.readAt(off, got.data(), n) != FontRead::Ok ||
                !std::equal(got.begin(), got.end(), whole.begin() + static_cast<size_t>(off))) {
                same_bytes = false;
            }
        }
        // 文件尾单独取一段 —— 这是最容易把偏移算错的地方。
        const size_t tail = 64;
        const uint64_t tail_off = src.size() - tail;
        std::vector<uint8_t> got_tail(tail, 0);
        if (src.readAt(tail_off, got_tail.data(), tail) != FontRead::Ok ||
            !std::equal(got_tail.begin(), got_tail.end(),
                        whole.begin() + static_cast<size_t>(tail_off))) {
            same_bytes = false;
        }
        expectTrue(same_bytes, "十个位置（含块边界前后与文件尾）的按需读逐字节等于整份读");

        std::printf("\n[按需] 存储读：失败与文件尾是两种状态\n");
        {
            const size_t eof_before = src.cache().eofReads();
            const size_t fail_before = src.cache().readFailures();
            const uint64_t tail_start = src.size() - 4;

            // 请求越过文件尾：交出能读的那 4 字节，尾后一个字节都不许写（不返回垃圾）。
            std::vector<uint8_t> probe(16, 0xAB);
            size_t got_n = 0;
            const FontRead r = src.readAt(tail_start, probe.data(), probe.size(), &got_n);
            expectTrue(r == FontRead::Eof, "越过文件尾的读是文件尾，不是失败");
            expectTrue(got_n == 4,
                       "越过文件尾的读只交出可读的那 4 字节（实得 " + std::to_string(got_n) + "）");
            bool untouched = true;
            for (size_t i = 4; i < probe.size(); ++i) {
                if (probe[i] != 0xAB) untouched = false;
            }
            expectTrue(untouched && std::equal(probe.begin(), probe.begin() + 4, whole.end() - 4),
                       "可读那 4 字节等于文件尾，其后的缓冲一个字节都没被写");

            // 起点直接落在文件尾之后：零字节可读，同样是文件尾而不是失败。
            size_t got_zero = 99;
            expectTrue(src.readAt(src.size(), probe.data(), 4, &got_zero) == FontRead::Eof &&
                           got_zero == 0,
                       "起点在文件尾之后的读也是文件尾，交出零字节");

            // 这两条是分开计数的核心判据：文件尾只进 eofReads，不进 readFailures。
            expectTrue(src.cache().eofReads() == eof_before + 2, "两次越尾读都记进了 eofReads");
            expectTrue(src.cache().readFailures() == fail_before,
                       "文件尾的短读一次都没记进 readFailures");

            // 真失败长什么样：文件在打开之后被截断（存储记的尺寸大于实际可读的部分）。
            //   只断言"存储层没打开会失败"是不够的 —— 那不进块缓存的计数，
            //   证明不了这个计数器在按需那条路上真的会动。
            const std::string cut_path = "_font_truncate_probe.bin";
            const size_t keep = 1u << 20;  // 留 1 MB：够放下头与表目录
            {
                std::ofstream cut(cut_path, std::ios::binary | std::ios::trunc);
                cut.write(reinterpret_cast<const char*>(whole.data()),
                          static_cast<std::streamsize>(std::min(keep, whole.size())));
            }
            // 作用域不是随手加的：Windows 上不能删除一个还开着句柄的文件，而 FontSource
            //   直到析构才关句柄。把它留在外层，"删掉临时文件"那一步会失败且返回值没人看 ——
            //   每跑一次就留一个 4 KB 的残骸（本目录下曾有 _font_truncate_probe.bin）。
            bool cut_removed = false;
            {
                FontSource truncated;
                const bool cut_open = truncated.open(cut_path);
                expectTrue(cut_open, "截断成 1 MB 的副本还能解析表目录（所以后面读到的是真失败）");
                if (cut_open) {
                    const size_t cut_fail_before = truncated.cache().readFailures();
                    const size_t cut_eof_before = truncated.cache().eofReads();
                    // 把文件再砍到 4 KB。存储记的尺寸仍是 1 MB，于是 512 KB 处的读
                    //   落在记录范围内、却读不到 —— 这正是"失败"而不是"文件尾"。
                    {
                        std::ofstream shrink(cut_path, std::ios::binary | std::ios::trunc);
                        shrink.write(reinterpret_cast<const char*>(whole.data()), 4096);
                    }
                    std::vector<uint8_t> lost(4096, 0);
                    size_t lost_got = 0;
                    const FontRead lr =
                        truncated.readAt(512u * 1024u, lost.data(), lost.size(), &lost_got);
                    expectTrue(lr == FontRead::Error, "文件被截断后的读是失败（不是文件尾）");
                    expectTrue(truncated.cache().readFailures() == cut_fail_before + 1,
                               "真失败会让 readFailures 加一 —— 这个计数不是死代码");
                    expectTrue(truncated.cache().eofReads() == cut_eof_before,
                               "真失败一次都不进 eofReads（两个计数互不串台）");
                }
                // 句柄在这里关闭（truncated 析构）。
            }
            cut_removed = std::remove(cut_path.c_str()) == 0;
            expectTrue(cut_removed, "临时文件删掉了（删不掉就是每跑一次留一份残骸）");

            // 与它对照：没打开的存储同样是失败，且返回的字节数是零。
            FontStorage unopened;
            std::vector<uint8_t> buf(4, 0);
            size_t unopened_got = 7;
            expectTrue(
                unopened.readAt(0, buf.data(), buf.size(), &unopened_got) == FontRead::Error &&
                    unopened_got == 0,
                "没打开的存储读是失败，交出零字节");

            std::printf("     存储读：文件尾 %zu 次，失败 %zu 次\n", src.cache().eofReads(),
                        src.cache().readFailures());
        }

        // 块缓存自己也会把请求夹到文件尾，所以上面那几条走不到存储层的夹取逻辑 ——
        //   这里直接对存储层再测一遍，否则"存储层的越尾读是文件尾"没人管。
        {
            FontStorage raw;
            expectTrue(raw.open(cjkPath), "存储层能单独打开字体文件");
            if (raw.valid()) {
                std::vector<uint8_t> probe2(32, 0x5A);
                size_t got2 = 0;
                const FontRead r2 = raw.readAt(raw.size() - 3, probe2.data(), probe2.size(), &got2);
                expectTrue(r2 == FontRead::Eof, "存储层的越尾读也是文件尾，不是失败");
                expectTrue(got2 == 3, "存储层的越尾读只交出可读的那 3 字节（实得 " +
                                          std::to_string(got2) + "）");
                bool clean = true;
                for (size_t i = 3; i < probe2.size(); ++i) {
                    if (probe2[i] != 0x5A) clean = false;
                }
                expectTrue(clean && std::equal(probe2.begin(), probe2.begin() + 3, whole.end() - 3),
                           "存储层越尾读之后，尾后缓冲一个字节都没被写");
                // 正好读到文件尾不算越尾：请求区间完整落在文件里，那就是 Ok。
                std::vector<uint8_t> exact(64, 0);
                expectTrue(raw.readAt(raw.size() - 64, exact.data(), exact.size()) == FontRead::Ok,
                           "正好读到文件尾的那一次是成功的读（不是文件尾）");

                // 块数配成零时不该造出一个"每次读都失败"的缓存。
                FontBlockCache zero_blocks(4096, 0);
                std::vector<uint8_t> zb(64, 0);
                expectTrue(zero_blocks.read(raw, 0, zb.data(), zb.size()) == FontRead::Ok &&
                               zero_blocks.readFailures() == 0,
                           "块数传零时缓存仍读得出内容，且不记成失败");
            }
        }

        // 缓存生效的直接判据：同一区间连读两次，第二次一次存储都不落。
        //   只数总命中是测不出来的 —— 拼骨架时每张表只读一次，本来就全是不命中。
        const uint64_t probe_off = 1u << 20;
        std::vector<uint8_t> p1(4096, 0);
        std::vector<uint8_t> p2(4096, 0);
        const size_t reads_before = src.cache().storageReads();
        const FontRead r1 = src.readAt(probe_off, p1.data(), p1.size());
        const size_t reads_first = src.cache().storageReads();
        const FontRead r2 = src.readAt(probe_off, p2.data(), p2.size());
        const size_t reads_second = src.cache().storageReads();
        expectTrue(r1 == FontRead::Ok && r2 == FontRead::Ok && p1 == p2,
                   "同一区间两次读到相同内容");
        expectTrue(reads_first > reads_before, "第一次读落了存储");
        expectTrue(reads_second == reads_first, "第二次读全部命中，一次存储都没落");

        const FontBlockCache& c = src.cache();
        std::printf("     块缓存：命中 %zu，未命中 %zu，落存储 %zu 次 / %.1f KB，IO %.3f ms\n",
                    c.hits(), c.misses(), c.storageReads(),
                    static_cast<double>(c.storageBytes()) / 1024.0, c.ioMs());
        expectTrue(c.misses() > 0 && c.storageReads() == c.misses(),
                   "拼骨架确实落了存储，且落存储次数等于未命中次数");
        expectTrue(c.storageBytes() >= skeleton.size(), "从存储搬进来的字节不少于骨架本身");
        // 常驻不可能超过累计读进来的字节，也不可能超过块数乘块大小。
        expectTrue(c.residentBytes() <= c.storageBytes(), "常驻不超过累计从存储读入的字节");
        expectTrue(
            c.residentBytes() <= c.capacityBytes(),
            "常驻不超过块数乘块大小（上限 " + std::to_string(c.capacityBytes() / 1024) + " KB）");
    }

    std::printf("\n[按需] 按需脸与全量脸的整形结果逐字段相同\n");
    {
        FontLibrary lazy;
        const int lazy_cjk = lazy.addFaceLazy(cjkPath);
        const int lazy_latin = lazy.addFace(latinPath);
        expectTrue(lazy_cjk >= 0, "按需模式能装上一张脸");
        if (lazy_cjk >= 0) {
            FontFace* lf = lazy.face(lazy_cjk);
            expectTrue(lf->source() != nullptr, "按需脸挂着数据源");
            expectTrue(lf->glyphCount() == lib.face(cjk)->glyphCount(),
                       "按需脸的字形数与全量脸一致");

            const FontInstance* full = lib.instance(cjk, kSize);
            const FontInstance* part = lazy.instance(lazy_cjk, kSize);
            const ShapedText a =
                shapeText(full->hb(), kCJK_sentence, WritingMode::HorizontalTB(), "zh");
            const ShapedText b =
                shapeText(part->hb(), kCJK_sentence, WritingMode::HorizontalTB(), "zh");

            bool identical =
                a.glyphs.size() == b.glyphs.size() && a.inline_advance == b.inline_advance;
            for (size_t i = 0; identical && i < a.glyphs.size(); ++i) {
                const ShapedGlyph& x = a.glyphs[i];
                const ShapedGlyph& y = b.glyphs[i];
                identical = x.glyph_id == y.glyph_id && x.cluster == y.cluster &&
                            x.x_advance == y.x_advance && x.y_advance == y.y_advance &&
                            x.x_offset == y.x_offset && x.y_offset == y.y_offset;
            }
            std::printf("     字形数 %zu vs %zu，推进 %d vs %d\n", a.glyphs.size(), b.glyphs.size(),
                        a.inline_advance, b.inline_advance);
            expectTrue(identical, "同一段文本：两种装载方式的整形结果逐字段相同");

            // 回退链也要一致：这是"骨架的 cmap 覆盖与原字体相同"的间接判据。
            //   探针用的是 10 上标 9 /L —— NotoSansSC 恰好没有 U+2079，回退真的会被走到。
            const std::vector<int> chain_full{cjk, latin};
            const std::vector<int> chain_lazy{lazy_cjk, lazy_latin};
            const std::vector<FontRun> rf = itemizeByFont(lib, kMixedProbe, chain_full, kSize);
            const std::vector<FontRun> rl = itemizeByFont(lazy, kMixedProbe, chain_lazy, kSize);
            // 比"命中的是哪张脸"要用文件路径，不能比 face_index —— 后者是库内下标，
            //   两个库的装载顺序不同就不一样，拿它比会把"结果其实相同"误判成不同。
            bool runs_same = rf.size() == rl.size();
            for (size_t i = 0; runs_same && i < rf.size(); ++i) {
                runs_same = rf[i].cp_begin == rl[i].cp_begin && rf[i].cp_end == rl[i].cp_end &&
                            lib.facePath(rf[i].face_index) == lazy.facePath(rl[i].face_index);
            }
            std::printf("     回退段 %zu vs %zu\n", rf.size(), rl.size());
            expectTrue(runs_same, "字体回退的切段与命中字体也一致（cmap 覆盖没变）");
        }
    }

    std::printf("\n[9] 表格排版：格内断行、列宽、行高与整行换页\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 300.0f;
        page.margins = PageMargins::all(10.0f);
        const float cx = page.margins.left;
        const float cw = page.contentWidthPt();  // 280
        const float pad = kTableCellPadXPt;      // 只有左右有内边距，上下为 0

        // 块在 grid 里的位置。格用块区间声明归属，块流按格序排，两边说的是同一件事，
        //   所以位置在造块时记下来，由 cellsOf 变成格区间。
        std::vector<std::pair<int, int>> where;
        auto mkCellBlock = [&](const std::string& text, int row, int cell) {
            where.push_back({row, cell});
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = kSize;
            b.cp_text = utf8ToCodepoints(text);
            return b;
        };
        auto mkTable = [&](uint64_t handle, int rows, int cols) {
            LayoutTable t;
            t.handle = handle;
            t.rows = rows;
            t.cols = cols;
            t.col_widths.assign(static_cast<size_t>(cols), WidthSpec{});
            t.row_specs.assign(static_cast<size_t>(rows), LayoutRow{});
            return t;
        };
        // 每格的在 grid 里的原点列（无合并时就是格号），块区间按 where 推出来。
        auto cellsOf = [&](int rows, int cols) {
            std::vector<LayoutCell> out;
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    LayoutCell lc;
                    lc.row = r;
                    lc.cell = c;
                    int first = -1;
                    int count = 0;
                    for (size_t i = 0; i < where.size(); ++i) {
                        if (where[i].first != r || where[i].second != c) continue;
                        if (first < 0) first = static_cast<int>(i);
                        ++count;
                    }
                    if (first >= 0) {
                        lc.first_block = first;
                        lc.block_count = count;
                    }
                    out.push_back(lc);
                }
            }
            where.clear();
            return out;
        };

        // 一：2x2 表，每格一个字 —— 四个行盒，同行的 y 相同、不同列的 x 差一个列宽。
        {
            constexpr uint64_t kT = 7;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0), mkCellBlock("乙", 0, 1),
                                             mkCellBlock("丙", 1, 0), mkCellBlock("丁", 1, 1)};
            LayoutTable t = mkTable(kT, 2, 2);
            t.width = WidthSpec::makeAbs(cw);  // 撑满 280 → 每列 140
            t.cells = cellsOf(2, 2);
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() == 4, "2x2 每格一行，共四个行盒");
            if (r.lines.size() == 4) {
                const float col_w = cw / 2.0f;
                expectNear(r.lines[0].inline_origin_pt, cx + pad, 0.01,
                           "第一格的行内起点 = 表左 + 内边距");
                expectNear(r.lines[1].inline_origin_pt, cx + col_w + pad, 0.01,
                           "第二格的行内起点 = 表左 + 一个列宽 + 内边距");
                expectNear(r.lines[1].block_lo_pt, r.lines[0].block_lo_pt, 0.01,
                           "同一行的两格在同一基线上");
                expectTrue(r.lines[2].block_lo_pt > r.lines[0].block_lo_pt + 1.0f,
                           "第二行在第一行下面");
                // 行高 = 一行内容高 + 上下内边距（Auto 规则）。内边距不为 0 时格线
                //   不贴着字顶字底，空单元格的光标才不会压在格线上。
                expectNear(r.lines[2].block_lo_pt - r.lines[0].block_lo_pt,
                           r.lines[0].advance_pt + 2.0f * kTableCellPadYPt, 0.05,
                           "行高 = 一行内容高 + 上下内边距（Auto 规则）");
                expectNear(r.lines[2].inline_origin_pt, cx + pad, 0.01,
                           "第二行第一格又回到第一列的起点");
            }
        }
        // 二：格内断行用格宽（不是页宽），行高 = 内容高 + 上下内边距。
        {
            constexpr uint64_t kT = 8;
            // 20 个汉字 = 240 磅；格宽 = 280 - 8 = 272 也装得下 —— 所以缩成两列，
            //   让格宽只有 136，15 个汉字必断行。
            std::vector<BlockSpec> blocks = {mkCellBlock("一二三四五六七八九十一二三四五", 0, 0)};
            LayoutTable t = mkTable(kT, 1, 2);
            t.cells = cellsOf(1, 2);
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() >= 2, "格内文本按格宽断行（不是按页宽）");
            const float cell_cw = cw / 2.0f - 2.0f * pad;
            bool fits = !r.lines.empty();
            for (const LineBox& lb : r.lines) {
                if (lb.inline_end_pt > cell_cw + 0.01f) fits = false;
            }
            expectTrue(fits, "每一行的行内推进都不超过格内可用宽度");
            if (r.lines.size() >= 2) {
                // 格内相邻行的间距就是行高（内边距只在格的首尾各加一次，由行高结算时加）。
                const float line_h = r.lines[0].advance_pt;
                expectNear(r.lines[1].block_lo_pt - r.lines[0].block_lo_pt, line_h, 0.05,
                           "格内相邻行的间距 = 行高（内边距不逐行加）");
            }
        }
        // 三：显式行高（Exact）把整行撑到给定高度。
        {
            constexpr uint64_t kT = 9;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0), mkCellBlock("乙", 1, 0)};
            LayoutTable t = mkTable(kT, 2, 1);
            t.cells = cellsOf(2, 1);
            t.row_specs[0].height_pt = 40.0f;
            t.row_specs[0].height_rule = RowHeightRule::Exact;
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() == 2, "两行各一个行盒");
            if (r.lines.size() == 2) {
                expectNear(r.lines[1].block_lo_pt - r.lines[0].block_lo_pt, 40.0, 0.05,
                           "显式行高 40 磅：下一行从 40 磅之后开始");
            }
            // 行矩形按"这一行实际占的高度"给：行高比内容高时中间那段空白也要有格线，
            //   否则一行看起来断成两块（连"相邻两行的格线首尾相接"都不成立）。
            if (r.placed_tables.size() == 1 && r.placed_tables[0].placed.size() == 2) {
                const PlacedTable& pt = r.placed_tables[0];
                expectNear(pt.placed[1].y, pt.placed[0].y + pt.placed[0].h, 0.05,
                           "相邻两行的格线首尾相接（中间没有缝）");
                expectNear(pt.placed[0].h, 40.0, 0.05, "第一行的格线高度是显式行高 40 磅");
            }
        }
        // 四：显式列宽只占一列，其余列平分剩下的宽度。
        {
            constexpr uint64_t kT = 10;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0), mkCellBlock("乙", 0, 1)};
            LayoutTable t = mkTable(kT, 1, 2);
            t.width = WidthSpec::makeAbs(200.0f);
            t.col_widths[0] = WidthSpec::makeAbs(60.0f);
            t.col_widths[1] = WidthSpec::makeAbs(140.0f);
            t.cells = cellsOf(1, 2);
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() == 2, "两格各一行");
            if (r.lines.size() == 2) {
                expectNear(r.lines[1].inline_origin_pt - r.lines[0].inline_origin_pt, 60.0, 0.01,
                           "第一列宽 60：第二格从 60 磅之后开始");
            }
        }
        // 五：表宽百分比 + 居中 —— 表左边缘按对齐算。
        {
            constexpr uint64_t kT = 11;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0)};
            LayoutTable t = mkTable(kT, 1, 1);
            t.width = WidthSpec::makePercent(0.5f);  // 140
            t.align = TableHAlign::Center;
            t.cells = cellsOf(1, 1);
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() == 1, "一格一行");
            if (r.lines.size() == 1) {
                expectNear(r.lines[0].inline_origin_pt, cx + (cw - cw * 0.5f) * 0.5f + pad, 0.01,
                           "表宽一半并居中：左边缘落在内容区中间再偏移");
            }
        }
        // 六：反向一半 —— 同一批块不传表格结构时，按正文排（行内起点就是内容区左边）。
        {
            constexpr uint64_t kT = 12;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0), mkCellBlock("乙", 0, 1)};
            LayoutTable t = mkTable(kT, 1, 2);
            t.cells = cellsOf(1, 2);
            const LayoutResult with = layoutDocument(lib, blocks, page, {t});
            const LayoutResult without = layoutDocument(lib, blocks, page);
            expectTrue(with.lines.size() == 2 && without.lines.size() == 2,
                       "有无结构表都各排两个行盒（内容不丢）");
            if (with.lines.size() == 2 && without.lines.size() == 2) {
                expectNear(without.lines[0].inline_origin_pt, cx, 0.01,
                           "不传结构表：按正文排，起点是内容区左边");
                expectNear(without.lines[1].inline_origin_pt, cx, 0.01,
                           "不传结构表：第二格也按正文排（不起新列）");
                expectTrue(
                    with.lines[1].inline_origin_pt > without.lines[1].inline_origin_pt + 1.0f,
                    "传了结构表：第二格才起新列");
            }
        }
        // 七：整行放不下时整行换页，两个格一起走。
        {
            constexpr uint64_t kT = 13;
            PageMetrics small;
            small.width_pt = 200.0f;
            small.height_pt = 45.0f;  // 内容区高 25
            small.margins = PageMargins::all(10.0f);
            std::vector<BlockSpec> blocks;
            // 每行 3 个字：行高 = 单倍行高（约 17.4），两行 34.8 —— 第二行在本页
            //   只剩 7.6 磅时放不下，整行要换到第 2 页。
            for (int r = 0; r < 2; ++r) {
                for (int c = 0; c < 2; ++c) {
                    blocks.push_back(mkCellBlock("甲乙丙", r, c));
                }
            }
            LayoutTable t = mkTable(kT, 2, 2);
            t.cells = cellsOf(2, 2);
            const LayoutResult r = layoutDocument(lib, blocks, small, {t});
            expectTrue(r.lines.size() == 4, "两行两列共四个行盒");
            if (r.lines.size() == 4) {
                expectTrue(r.lines[0].page == 0 && r.lines[1].page == 0, "第一行的两格都在第 0 页");
                expectTrue(r.lines[2].page == 1 && r.lines[3].page == 1,
                           "第二行整行换页（两个格一起走，不拆散）");
                expectNear(r.lines[2].block_lo_pt, small.margins.left + kTableCellPadYPt, 0.05,
                           "换页后第二行落在新页的内容区顶部（上下内边距为 0）");
                expectNear(r.lines[2].block_lo_pt, r.lines[3].block_lo_pt, 0.01,
                           "换页后同一行的两格仍在同一基线上");
            }
        }
        // 八：行跨页断开 —— 允许断行的按行盒切片，不允许的整行挪到下一页。
        {
            constexpr uint64_t kT = 14;
            PageMetrics tiny;
            tiny.width_pt = 200.0f;
            tiny.height_pt = 100.0f;  // 内容区高 80
            tiny.margins = PageMargins::all(10.0f);
            // 第一行一个字（高 27.2），第二行一长串（格宽 192，每行 16 字，五行 96）。
            const std::string long_text =
                "一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十"
                "一二三四五六七八九十";
            auto build = [&](bool can_split) {
                std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0),
                                                 mkCellBlock(long_text, 1, 0)};
                LayoutTable t = mkTable(kT, 2, 1);
                t.cells = cellsOf(2, 1);
                t.row_specs[0].can_split_by_page = can_split;
                t.row_specs[1].can_split_by_page = can_split;
                return layoutDocument(lib, blocks, tiny, {t});
            };
            const LayoutResult split = build(true);
            int p0 = 0;
            int p1 = 0;
            float hi_on_p0 = 0.0f;
            for (const LineBox& lb : split.lines) {
                if (lb.page == 0) {
                    ++p0;
                    hi_on_p0 = std::max(hi_on_p0, lb.block_hi_pt);
                } else if (lb.page == 1) {
                    ++p1;
                }
            }
            expectTrue(p0 >= 2 && p1 >= 1, "允许跨页断行：这一行的行盒分布在两页上");
            expectTrue(hi_on_p0 <= tiny.margins.left + tiny.contentHeightPt() + 0.01f,
                       "本页上那半片段的底不超过内容区底边");

            const LayoutResult whole = build(false);
            int q0 = 0;
            int q1 = 0;
            for (const LineBox& lb : whole.lines) {
                if (lb.page == 0) ++q0;
                if (lb.page == 1) ++q1;
            }
            expectTrue(q0 == 1 && q1 >= 2, "不允许跨页断行：第二行整行挪到下一页，本页只剩第一行");
        }
        // 九：格内多行要撑开行高 —— 一格里两个段落，下一行表格行必须让开这两行。
        //   必须是"同一行里切格"的形状（两列）：切格时不结算上一格的内容高，只按一行算，
        //   下一行表格行压到格内第二段上（实测两边都是 y=117）。单列表抓不到这一条 ——
        //   它切格与切行同时发生，走的是另一条（会顺带结算的）路径。
        {
            constexpr uint64_t kT = 15;
            std::vector<BlockSpec> blocks = {mkCellBlock("甲", 0, 0), mkCellBlock("乙", 0, 0),
                                             mkCellBlock("丙", 0, 1), mkCellBlock("丁", 1, 0),
                                             mkCellBlock("戊", 1, 1)};
            LayoutTable t = mkTable(kT, 2, 2);
            t.cells = cellsOf(2, 2);
            const LayoutResult r = layoutDocument(lib, blocks, page, {t});
            expectTrue(r.lines.size() == 5, "格内两段 + 另一格 + 第二行两格，共五行");
            if (r.lines.size() == 5) {
                // 段 1 是格 (0,0) 的第二段；段 3 是第二行第一格。
                expectTrue(
                    r.lines[3].block_lo_pt >= r.lines[1].block_hi_pt + kTableCellPadYPt - 0.01f,
                    "第二行表格行在格内最后一行之下（格子被撑开）");
                expectTrue(r.lines[3].block_lo_pt > r.lines[0].block_lo_pt + 2.0f * kSize,
                           "第二行表格行至少让开两行的高度");
            }
        }
    }

    std::printf("\n[10] 块局部排版：与整篇排版的块行几何一致\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 400.0f;
        page.margins = PageMargins::all(10.0f);
        const float cw = page.contentWidthPt();  // 280

        auto mkBlock = [&](const std::string& text) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = kSize;
            b.line_spacing.factor = 1.6f;
            b.space_after_pt = 0.0f;
            b.setText(text);
            return b;
        };
        auto mkConstraints = [&](const BlockSpec& b, float width) {
            BlockConstraints c;
            c.width_pt = width;
            c.writing_mode = b.wm;
            return c;
        };

        const std::string text =
            "块局部排版只做三件事：整形、按约束断行、把行放到块内坐标。"
            "它不认识页，也不认识表格与浮动图。";
        const BlockSpec block = mkBlock(text);
        const BlockConstraints constraints = mkConstraints(block, cw);
        const LayoutResult full = layoutDocument(lib, {block}, page);
        ShapingCache cache;
        const BlockLayout local = layoutBlock(lib, block, constraints, &cache);

        expectTrue(local.status == BlockLayoutStatus::Ok, "横排正文块：状态是 Ok");
        expectTrue(full.lines.size() >= 2, "前提：整篇排版把这个块折成多行（实际 " +
                                               std::to_string(full.lines.size()) + " 行）");
        expectTrue(local.lines.size() == full.lines.size(), "块局部与整篇排出的行数相同");
        expectTrue(local.glyphs.size() == full.glyphs.size(), "字形数相同");
        // 分页消费的是不含坐标的断行片段；块内行盒由同一份片段落位，两者必须同度量。
        expectTrue(local.fragments.size() == local.lines.size(),
                   "块局部结果同时给出与页无关的断行片段");
        int bad_fragment = 0;
        if (local.fragments.size() == local.lines.size()) {
            for (size_t i = 0; i < local.fragments.size(); ++i) {
                const BrokenLine& f = local.fragments[i];
                const LineBox& lb = local.lines[i];
                const bool same = std::fabs(f.line_h - lb.advance_pt) < 1e-4f &&
                                  std::fabs(f.ascent - lb.ascent_pt) < 1e-4f &&
                                  std::fabs(f.descent - lb.descent_pt) < 1e-4f &&
                                  std::fabs(f.lead - lb.lead_top_pt) < 1e-4f &&
                                  f.clip == lb.clip_text;
                if (!same) ++bad_fragment;
            }
        }
        expectTrue(bad_fragment == 0, "断行片段的行度量与块内行盒逐条相同");

        // 两份结果的差别应当只有一个平移：整篇是页内坐标，块局部以内容区左上为原点。
        const float off_x = full.lines.empty() ? 0.0f : full.lines[0].inline_origin_pt;
        const float off_y =
            full.lines.empty() ? 0.0f : (full.lines[0].block_lo_pt - full.lines[0].lead_top_pt);
        expectNear(off_x, page.margins.left, 1e-4, "整篇的行内起点就是左边距");
        expectNear(off_y, page.margins.top, 1e-4, "整篇的首行行顶就是上边距");

        int bad_line = 0;
        if (local.lines.size() == full.lines.size()) {
            for (size_t i = 0; i < full.lines.size(); ++i) {
                const LineBox& a = full.lines[i];
                const LineBox& b = local.lines[i];
                bool same = a.first_cp == b.first_cp && a.last_cp == b.last_cp;
                same = same && std::fabs(a.ascent_pt - b.ascent_pt) < 1e-4f;
                same = same && std::fabs(a.descent_pt - b.descent_pt) < 1e-4f;
                same = same && std::fabs(a.advance_pt - b.advance_pt) < 1e-4f;
                same = same && std::fabs(a.lead_top_pt - b.lead_top_pt) < 1e-4f;
                same = same && std::fabs(a.inline_end_pt - b.inline_end_pt) < 1e-3f;
                same = same && std::fabs((a.block_lo_pt - off_y) - b.block_lo_pt) < 1e-3f;
                same = same && std::fabs((a.block_hi_pt - off_y) - b.block_hi_pt) < 1e-3f;
                same = same && std::fabs((a.inline_origin_pt - off_x) - b.inline_origin_pt) < 1e-3f;
                same = same && a.clip_text == b.clip_text && a.vertical == b.vertical;
                same = same && a.clusters.size() == b.clusters.size();
                for (size_t k = 0; same && k < a.clusters.size(); ++k) {
                    same = same && a.clusters[k].cp == b.clusters[k].cp &&
                           std::fabs(a.clusters[k].inline_pt - b.clusters[k].inline_pt) < 1e-3f &&
                           a.clusters[k].placeholder == b.clusters[k].placeholder;
                }
                if (!same) ++bad_line;
            }
        }
        expectTrue(bad_line == 0, "每一行的码点范围、行高、基线、簇位置都逐字段相同");

        int bad_glyph = 0;
        if (local.glyphs.size() == full.glyphs.size()) {
            for (size_t i = 0; i < full.glyphs.size(); ++i) {
                const PlacedGlyph& a = full.glyphs[i];
                const PlacedGlyph& b = local.glyphs[i];
                bool same = a.glyph_id == b.glyph_id && a.font == b.font && a.cluster == b.cluster;
                same = same && a.line == b.line && a.paragraph == b.paragraph;
                same = same && a.page == 0 && b.page == 0;
                same = same && std::fabs((a.x_pt - off_x) - b.x_pt) < 1e-3f;
                same = same && std::fabs((a.y_pt - off_y) - b.y_pt) < 1e-3f;
                same = same && std::fabs(a.advance_pt - b.advance_pt) < 1e-4f;
                same = same && a.color == b.color && a.size_pt == b.size_pt;
                same = same && a.weight == b.weight && a.italic == b.italic;
                same = same && a.underline == b.underline;
                if (!same) ++bad_glyph;
            }
        }
        expectTrue(bad_glyph == 0, "每一个字形的身份、坐标与字符属性都相同");

        // 行槽高度之和：不含段前距与段后距。
        if (!full.lines.empty()) {
            const float last_top = full.lines.back().block_lo_pt - full.lines.back().lead_top_pt;
            const float want_h = (last_top - off_y) + full.lines.back().advance_pt;
            expectNear(local.height_pt, want_h, 1e-3,
                       "height_pt 等于各行行槽高度之和（不含段后距）");
        }

        // 跨页的简单块：分页只改行盒的页内 y，不重新断行。整篇循环与 layoutBlock
        //   共用 breakBlock，所以两边的断点与行高必须逐条相同。
        PageMetrics small_page;
        small_page.width_pt = 300.0f;
        small_page.height_pt = 120.0f;
        small_page.margins = PageMargins::all(10.0f);
        const BlockSpec long_block = mkBlock(text + text + text);
        const LayoutResult full_long = layoutDocument(lib, {long_block}, small_page);
        const BlockLayout local_long =
            layoutBlock(lib, long_block, mkConstraints(long_block, cw), &cache);
        expectTrue(full_long.stats.page_count >= 2, "前提：这个块跨了页");
        expectTrue(local_long.status == BlockLayoutStatus::Ok &&
                       full_long.lines.size() == local_long.lines.size(),
                   "跨页块的行数与 layoutBlock 相同");
        int break_diff = 0;
        if (full_long.lines.size() == local_long.lines.size()) {
            for (size_t i = 0; i < full_long.lines.size(); ++i) {
                const LineBox& a = full_long.lines[i];
                const LineBox& b = local_long.lines[i];
                bool same = a.first_cp == b.first_cp && a.last_cp == b.last_cp &&
                            std::fabs(a.advance_pt - b.advance_pt) < 1e-4f &&
                            std::fabs(a.lead_top_pt - b.lead_top_pt) < 1e-4f &&
                            std::fabs(a.ascent_pt - b.ascent_pt) < 1e-4f &&
                            std::fabs(a.descent_pt - b.descent_pt) < 1e-4f &&
                            a.clusters.size() == b.clusters.size();
                for (size_t k = 0; same && k < a.clusters.size(); ++k) {
                    same = a.clusters[k].cp == b.clusters[k].cp &&
                           std::fabs(a.clusters[k].inline_pt - b.clusters[k].inline_pt) < 1e-3f;
                }
                if (!same) ++break_diff;
            }
        }
        expectTrue(break_diff == 0, "跨页之后每一行的断点与 layoutBlock 相同");
        int page_tops = 0;
        int pages = 0;
        for (size_t i = 0; i < full_long.lines.size(); ++i) {
            if (i > 0 && full_long.lines[i].page == full_long.lines[i - 1].page) continue;
            ++pages;
            if (std::fabs(full_long.lines[i].slotLoPt() - small_page.margins.top) > 1e-4f) {
                ++page_tops;
            }
        }
        expectTrue(pages >= 2 && page_tops == 0, "每页的第一行都从内容区上沿起排");

        // 长简单块跨多页：块局部断行给出全部行片段，分页只按页高落位。
        //   页首行必须从内容区上沿起；行片段逐条首尾相接（换页不重排、不漏行）；
        //   页高只改页数，不改断行。
        {
            const BlockSpec long_block6 = mkBlock(text + text + text + text + text + text);
            const LayoutResult full6 = layoutDocument(lib, {long_block6}, small_page);
            const BlockLayout local6 =
                layoutBlock(lib, long_block6, mkConstraints(long_block6, cw), &cache);
            expectTrue(full6.stats.page_count >= 3, "前提：长简单块跨了至少三页");
            expectTrue(local6.status == BlockLayoutStatus::Ok &&
                           local6.fragments.size() == full6.lines.size(),
                       "跨多页的断行片段数与整篇行数相同");

            int metric_diff = 0;
            int page_start_diff = 0;
            int page_jump_diff = 0;
            int cont_diff = 0;
            int pages_seen = 0;
            int prev_page = -1;
            for (size_t i = 0; i < full6.lines.size() && i < local6.fragments.size(); ++i) {
                const LineBox& lb = full6.lines[i];
                const BrokenLine& f = local6.fragments[i];
                const bool same_metric = std::fabs(lb.advance_pt - f.line_h) < 1e-4f &&
                                         std::fabs(lb.ascent_pt - f.ascent) < 1e-4f &&
                                         std::fabs(lb.descent_pt - f.descent) < 1e-4f &&
                                         std::fabs(lb.lead_top_pt - f.lead) < 1e-4f &&
                                         lb.clip_text == f.clip;
                if (!same_metric) ++metric_diff;
                if (lb.page == prev_page) continue;
                ++pages_seen;
                if (std::fabs(lb.slotLoPt() - small_page.margins.top) > 1e-3f) ++page_start_diff;
                if (prev_page >= 0 && lb.page != prev_page + 1) ++page_jump_diff;
                if (i > 0 && lb.first_cp != full6.lines[i - 1].last_cp) ++cont_diff;
                prev_page = lb.page;
            }
            expectTrue(metric_diff == 0, "每一页的行度量都与块局部断行片段逐条相同");
            expectTrue(pages_seen >= 3 && page_start_diff == 0, "每一页的第一行都从内容区上沿起排");
            expectTrue(page_jump_diff == 0, "单块分页的页号连续，不跳页");
            expectTrue(cont_diff == 0, "换页处上一行结束偏移就是下一行起始偏移");

            int chain_diff = 0;
            size_t expect_begin = 0;
            for (const BrokenLine& f : local6.fragments) {
                if (f.begin != expect_begin || f.end <= f.begin) ++chain_diff;
                expect_begin = f.end;
            }
            expectTrue(chain_diff == 0, "断行片段首尾相接，没有漏排或重排");
            if (!full6.para_index.empty()) {
                expectTrue(expect_begin == static_cast<size_t>(full6.para_index[0].cluster_count),
                           "断行片段铺满整段的簇（空白段没有丢行）");
            }

            // 页高只影响页内落位：同一个块换一张更高的页，行数与断点必须分毫不动。
            PageMetrics tall_page = small_page;
            tall_page.height_pt = 600.0f;
            const LayoutResult tall = layoutDocument(lib, {long_block6}, tall_page);
            expectTrue(tall.lines.size() == full6.lines.size(), "换页高之后行数不变");
            expectTrue(tall.stats.page_count < full6.stats.page_count,
                       "页高变大页数变少（证明分页真的按页高走）");
            int break_diff_tall = 0;
            if (tall.lines.size() == full6.lines.size()) {
                for (size_t i = 0; i < tall.lines.size(); ++i) {
                    const LineBox& a = tall.lines[i];
                    const LineBox& b = full6.lines[i];
                    const bool same = a.first_cp == b.first_cp && a.last_cp == b.last_cp &&
                                      std::fabs(a.advance_pt - b.advance_pt) < 1e-4f &&
                                      std::fabs(a.lead_top_pt - b.lead_top_pt) < 1e-4f;
                    if (!same) ++break_diff_tall;
                }
            }
            expectTrue(break_diff_tall == 0, "换页高只改页号与纵坐标，不改断点与行度量");
        }

        // 反向一半：宽度减半，行数必须变多 —— 约束真的进了断行。
        const BlockConstraints narrow = mkConstraints(block, cw * 0.5f);
        const BlockLayout narrow_local = layoutBlock(lib, block, narrow, &cache);
        expectTrue(narrow_local.status == BlockLayoutStatus::Ok &&
                       narrow_local.lines.size() > local.lines.size(),
                   "宽度减半之后行数变多（约束没有被忽略）");

        // 括号占位走同一条路径：行内几何仍然一致。
        BlockSpec with_controls = mkBlock("甲字段乙");
        {
            InlineObject open;
            open.cp = 0;
            open.kind = InlineObjectKind::ControlStart;
            open.owner = 3;
            with_controls.objects.push_back(open);
            InlineObject close;
            close.cp = 4;
            close.kind = InlineObjectKind::ControlEnd;
            close.owner = 3;
            with_controls.objects.push_back(close);
        }
        const LayoutResult full_ctl = layoutDocument(lib, {with_controls}, page);
        const BlockLayout local_ctl =
            layoutBlock(lib, with_controls, mkConstraints(with_controls, cw), &cache);
        expectTrue(local_ctl.status == BlockLayoutStatus::Ok &&
                       local_ctl.lines.size() == full_ctl.lines.size(),
                   "带控件边界的块：行数相同");
        if (!local_ctl.lines.empty() && !full_ctl.lines.empty()) {
            expectNear(local_ctl.lines[0].inline_end_pt, full_ctl.lines[0].inline_end_pt, 1e-3,
                       "括号占位计入行末位置，两份结果一致");
            expectTrue(local_ctl.lines[0].clusters.size() == full_ctl.lines[0].clusters.size(),
                       "簇数相同（括号占位没有被少插或多插）");
            expectTrue(local_ctl.glyphs.size() == full_ctl.glyphs.size(),
                       "字形数相同（占位簇不产生字形）");
        }

        // 行内原子（图片 / 公式）不是浮动图：块局部也要给出同样的行内几何与行高。
        BlockSpec with_atom = mkBlock("汉字汉字");
        {
            InlineObject img;
            img.cp = 2;
            img.kind = InlineObjectKind::Atom;
            img.atom = InlineAtom::Image;
            img.w_pt = 30.0f;
            img.h_pt = 24.0f;
            with_atom.objects.push_back(img);
        }
        const LayoutResult full_atom = layoutDocument(lib, {with_atom}, page);
        const BlockLayout local_atom =
            layoutBlock(lib, with_atom, mkConstraints(with_atom, cw), &cache);
        expectTrue(local_atom.status == BlockLayoutStatus::Ok &&
                       local_atom.lines.size() == full_atom.lines.size(),
                   "带行内原子的块：行数相同");
        if (!local_atom.lines.empty() && !full_atom.lines.empty()) {
            expectNear(local_atom.lines[0].inline_end_pt, full_atom.lines[0].inline_end_pt, 1e-3,
                       "原子宽度计入行末位置，两份结果一致");
            expectNear(local_atom.lines[0].block_hi_pt - local_atom.lines[0].block_lo_pt,
                       full_atom.lines[0].block_hi_pt - full_atom.lines[0].block_lo_pt, 1e-3,
                       "原子盒高撑起行盒，两份结果一致");
            expectTrue(local_atom.glyphs.size() == full_atom.glyphs.size(),
                       "原子不产生字形，字形数相同");
        }

        // 空段：块局部也要给出一个行盒，几何与整篇一致。
        const BlockSpec empty = mkBlock("");
        const LayoutResult full_empty = layoutDocument(lib, {empty}, page);
        const BlockLayout local_empty = layoutBlock(lib, empty, mkConstraints(empty, cw), &cache);
        expectTrue(local_empty.status == BlockLayoutStatus::Ok && local_empty.lines.size() == 1 &&
                       local_empty.glyphs.empty(),
                   "空段排出一个行盒、没有字形");
        expectTrue(local_empty.fragments.empty(), "空段没有断行片段（那一行不是断行产物）");
        if (local_empty.lines.size() == 1 && full_empty.lines.size() == 1) {
            expectNear(local_empty.lines[0].advance_pt, full_empty.lines[0].advance_pt, 1e-4,
                       "空段行高与整篇一致");
            expectNear(local_empty.height_pt, full_empty.lines[0].advance_pt, 1e-4,
                       "空段高度就是那一行的高度");
        }

        // 缓存：同内容第二次调用命中按内容指纹那一路，结果不变。
        const long long hits_before = cache.hits();
        const BlockLayout again = layoutBlock(lib, block, constraints, &cache);
        expectTrue(cache.hits() == hits_before + 1, "同内容第二次调用命中缓存");
        expectTrue(
            again.status == BlockLayoutStatus::Ok && again.lines.size() == local.lines.size(),
            "命中缓存后行数不变");
        if (!again.lines.empty() && !local.lines.empty()) {
            expectNear(again.height_pt, local.height_pt, 1e-6, "命中缓存后几何不变");
        }

        // 不支持的情形必须给出明确状态，而不是一份看起来正常的空结果。
        {
            BlockSpec vert = mkBlock("竖排");
            vert.wm = WritingMode::VerticalRL();
            BlockConstraints vc = mkConstraints(vert, cw);
            vc.column.left_pt = page.margins.left;
            vc.column.bottom_pt = page.margins.top + page.contentHeightPt();
            vc.column.first_top_pt = page.margins.top;
            vc.column.band_top_pt = page.margins.top;
            const BlockLayout r = layoutBlock(lib, vert, vc, nullptr);
            expectTrue(r.status == BlockLayoutStatus::Ok && r.columns.size() == 1 &&
                           r.lines.size() == 1 && r.lines[0].vertical,
                       "竖排块：一条带排出一列（块局部也支持竖排）");
        }
        {
            BlockSpec empty_vert = mkBlock("");
            empty_vert.wm = WritingMode::VerticalRL();
            BlockConstraints vc = mkConstraints(empty_vert, cw);
            vc.column.bottom_pt = page.margins.top + page.contentHeightPt();
            vc.column.first_top_pt = page.margins.top;
            vc.column.band_top_pt = page.margins.top;
            const BlockLayout r = layoutBlock(lib, empty_vert, vc, nullptr);
            expectTrue(r.status == BlockLayoutStatus::Ok && r.columns.size() == 1 &&
                           r.lines.size() == 1 && r.lines[0].vertical,
                       "空竖排段：块局部排出一条空列（与整篇共用 placeVerticalColumn）");
        }
        {
            BlockSpec cell = mkBlock("格内文本按格宽断行宽度收窄之后行数变多");
            const BlockLayout wide = layoutBlock(lib, cell, mkConstraints(cell, cw), nullptr);
            const BlockLayout narrow =
                layoutBlock(lib, cell, mkConstraints(cell, cw * 0.4f), nullptr);
            expectTrue(wide.status == BlockLayoutStatus::Ok && wide.lines.size() == 1 &&
                           wide.fragments.size() == 1,
                       "横排格内块：块局部按约束里的格宽排版");
            expectTrue(
                narrow.status == BlockLayoutStatus::Ok && narrow.lines.size() > wide.lines.size(),
                "横排格内块：格宽收窄之后行数变多（约束真的进了断行）");
        }
        {
            BlockSpec cell = mkBlock("格内竖排");
            cell.wm = WritingMode::VerticalRL();
            BlockConstraints vc = mkConstraints(cell, cw);
            vc.column.left_pt = page.margins.left;
            vc.column.bottom_pt = page.margins.top + page.contentHeightPt();
            vc.column.first_top_pt = page.margins.top;
            vc.column.band_top_pt = page.margins.top;
            const BlockLayout r = layoutBlock(lib, cell, vc, nullptr);
            expectTrue(r.status == BlockLayoutStatus::Ok && !r.columns.empty(),
                       "竖排格内块按传入的版心竖带排成列");
        }
        {
            BlockSpec floating = mkBlock("浮动图");
            InlineObject img;
            img.cp = 0;
            img.kind = InlineObjectKind::Atom;
            img.atom = InlineAtom::Image;
            img.w_pt = 20.0f;
            img.h_pt = 20.0f;
            img.floating = true;
            floating.objects.push_back(img);
            const BlockLayout r = layoutBlock(lib, floating, mkConstraints(floating, cw), nullptr);
            expectTrue(r.status == BlockLayoutStatus::UnsupportedFloat && r.lines.empty(),
                       "含浮动图的块明确报告不支持");
        }
        {
            BlockConstraints bad = mkConstraints(block, 0.0f);
            const BlockLayout r = layoutBlock(lib, block, bad, nullptr);
            expectTrue(r.status == BlockLayoutStatus::InvalidConstraints && r.lines.empty(),
                       "宽度不为正时明确报告约束无效");
        }
    }

    std::printf("\n[11] 简单块分页：页号与行槽顶只由行片段与页度量给出\n");
    {
        // 分页只读行高；begin/end 只为让片段看起来完整。
        auto mkLines = [](const std::vector<float>& heights) {
            std::vector<BrokenLine> lines;
            size_t begin = 0;
            for (float h : heights) {
                BrokenLine b;
                b.begin = begin;
                b.end = begin + 1;
                b.line_h = h;
                lines.push_back(b);
                ++begin;
            }
            return lines;
        };
        BlockPlanInput in;
        in.content_top_pt = 10.0f;
        in.content_bottom_pt = 110.0f;  // 版心高 100
        in.page_index = 0;
        in.cursor_y_pt = 10.0f;

        // 页内逐行推进，越过版心下沿才翻页，页首行回到版心上沿。
        {
            const BlockPlan r = planBlockLines(mkLines({30.0f, 30.0f, 30.0f, 30.0f}), in);
            const int want_pages[] = {0, 0, 0, 1};
            const float want_tops[] = {10.0f, 40.0f, 70.0f, 10.0f};
            bool same = r.lines.size() == 4;
            for (size_t i = 0; same && i < r.lines.size(); ++i) {
                same = r.lines[i].page == want_pages[i] &&
                       std::fabs(r.lines[i].top_pt - want_tops[i]) < 1e-6f;
            }
            expectTrue(same, "页底溢出的那一行翻页，并从版心上沿起排");
            expectTrue(r.page_index == 1 && std::fabs(r.cursor_y_pt - 40.0f) < 1e-6f,
                       "结束页号与结束游标（末行行顶 40 + 行高 30）");
            expectTrue(r.page_index + 1 == 2, "四行 30 磅的片段恰好占两页");
        }
        // 段前分页：块首不在版心顶时翻页；已经在版心顶时不空翻。
        {
            BlockPlanInput mid = in;
            mid.cursor_y_pt = 60.0f;
            mid.page_break_before = true;
            const std::vector<BrokenLine> frags = mkLines({20.0f});
            const BlockPlan r = planBlockLines(frags, mid);
            expectTrue(r.lines.size() == 1 && r.lines[0].page == 1 &&
                           std::fabs(r.lines[0].top_pt - 10.0f) < 1e-6f,
                       "段前分页：块首在页中时翻到下一页的版心上沿");
            mid.page_index = 4;
            mid.cursor_y_pt = 10.0f;
            const BlockPlan at_top = planBlockLines(frags, mid);
            expectTrue(at_top.lines.size() == 1 && at_top.lines[0].page == 4 &&
                           std::fabs(at_top.lines[0].top_pt - 10.0f) < 1e-6f,
                       "段前分页：块首已经在版心顶时不空翻一页");
        }
        // 段前距：文档首段与页中照加，页顶那一段不加。
        {
            BlockPlanInput sp = in;
            sp.space_before_pt = 25.0f;
            const std::vector<BrokenLine> frags = mkLines({20.0f});
            const BlockPlan r = planBlockLines(frags, sp);
            expectTrue(r.lines.size() == 1 && std::fabs(r.lines[0].top_pt - 35.0f) < 1e-6f,
                       "段前距：第一页的段首让出段前距");
            sp.page_index = 2;
            const BlockPlan top_page = planBlockLines(frags, sp);
            expectTrue(
                top_page.lines.size() == 1 && std::fabs(top_page.lines[0].top_pt - 10.0f) < 1e-6f,
                "段前距：页顶那一段不加（页首空白属于页边距）");
            sp.page_index = 0;
            sp.cursor_y_pt = 60.0f;
            sp.page_break_before = true;
            const BlockPlan after_break = planBlockLines(frags, sp);
            expectTrue(after_break.lines.size() == 1 && after_break.lines[0].page == 1 &&
                           std::fabs(after_break.lines[0].top_pt - 10.0f) < 1e-6f,
                       "段前分页之后落在页顶，段前距不再叠加");
        }
        // 段后距只进结束游标，不改行位。
        {
            BlockPlanInput sp = in;
            sp.space_before_pt = 5.0f;
            sp.space_after_pt = 7.0f;
            const BlockPlan r = planBlockLines(mkLines({20.0f}), sp);
            expectTrue(r.lines.size() == 1 && std::fabs(r.lines[0].top_pt - 15.0f) < 1e-6f,
                       "段后距不改最后一行的行槽顶");
            expectTrue(std::fabs(r.cursor_y_pt - 42.0f) < 1e-6f,
                       "结束游标 = 行槽顶 15 + 行高 20 + 段后距 7");
        }
        // 连页：不在页底断开，页号是行槽顶所在的条带。
        {
            BlockPlanInput cont = in;
            cont.continuous = true;
            cont.slab_pt = 100.0f;
            const BlockPlan r = planBlockLines(mkLines({30.0f, 30.0f, 30.0f, 30.0f}), cont);
            const int want_pages[] = {0, 0, 0, 1};
            const float want_tops[] = {10.0f, 40.0f, 70.0f, 100.0f};
            bool same = r.lines.size() == 4;
            for (size_t i = 0; same && i < r.lines.size(); ++i) {
                same = r.lines[i].page == want_pages[i] &&
                       std::fabs(r.lines[i].top_pt - want_tops[i]) < 1e-6f;
            }
            expectTrue(same, "连页：行顶一路累加，越过条带边界只换页号不回页顶");
            expectTrue(std::fabs(r.cursor_y_pt - 130.0f) < 1e-6f,
                       "连页结束游标是绝对 y（100 + 30）");
            const BlockPlan r2 = planBlockLines(mkLines({95.0f, 95.0f, 95.0f}), cont);
            const int want_p2[] = {0, 1, 2};
            const float want_t2[] = {10.0f, 105.0f, 200.0f};
            bool same2 = r2.lines.size() == 3;
            for (size_t i = 0; same2 && i < r2.lines.size(); ++i) {
                same2 = r2.lines[i].page == want_p2[i] &&
                        std::fabs(r2.lines[i].top_pt - want_t2[i]) < 1e-6f;
            }
            expectTrue(same2 && r2.page_index == 0, "连页：条带号由行槽顶现算，结束页号原样回传");
        }
        // 没有片段时不落行，游标只走段前距与段后距。
        {
            BlockPlanInput sp = in;
            sp.page_index = 2;
            sp.cursor_y_pt = 50.0f;
            sp.space_before_pt = 7.0f;
            sp.space_after_pt = 3.0f;
            const BlockPlan r = planBlockLines({}, sp);
            expectTrue(
                r.lines.empty() && r.page_index == 2 && std::fabs(r.cursor_y_pt - 60.0f) < 1e-6f,
                "没有片段时不落行，游标只累计段前距与段后距");
        }
        // 比整页还高的一行：两次判据都不通过，页号连进两格，与整篇循环同一口径。
        {
            const BlockPlan r = planBlockLines(mkLines({150.0f}), in);
            expectTrue(r.lines.size() == 1 && r.lines[0].page == 2 &&
                           std::fabs(r.lines[0].top_pt - 10.0f) < 1e-6f,
                       "比整页还高的一行连翻两页，仍从版心上沿起排");
        }

        // 与整篇排版对拍：同一个长块，逐行页号与行槽顶必须相同。
        PageMetrics small_page;
        small_page.width_pt = 300.0f;
        small_page.height_pt = 120.0f;
        small_page.margins = PageMargins::all(10.0f);
        BlockSpec long_block;
        long_block.font_index = cjk;
        long_block.size_pt = kSize;
        long_block.line_spacing.factor = 1.6f;
        long_block.space_after_pt = 0.0f;
        long_block.language = "zh";
        std::string long_text;
        for (int i = 0; i < 3; ++i) {
            long_text +=
                "患者诉晨起后心悸明显，持续约十分钟，休息后缓解。查体：心率 eighty-eight 次每分。";
        }
        long_block.setText(long_text);
        BlockConstraints lc;
        lc.width_pt = small_page.contentWidthPt();
        lc.writing_mode = long_block.wm;
        ShapingCache cache;
        const BlockLayout local = layoutBlock(lib, long_block, lc, &cache);
        const LayoutResult full = layoutDocument(lib, {long_block}, small_page);
        BlockPlanInput pin;
        pin.content_top_pt = small_page.margins.top;
        pin.content_bottom_pt = small_page.margins.top + small_page.contentHeightPt();
        pin.page_index = 0;
        pin.cursor_y_pt = small_page.margins.top;
        pin.continuous = small_page.continuous;
        pin.slab_pt = small_page.slabHeightPt();
        pin.space_before_pt = long_block.space_before_pt;
        pin.space_after_pt = long_block.space_after_pt;
        const BlockPlan plan = planBlockLines(local.fragments, pin);
        expectTrue(local.status == BlockLayoutStatus::Ok &&
                       plan.lines.size() == full.lines.size() && full.stats.page_count >= 2,
                   "对拍前提：分页结果与整篇行数相同，且真的跨了页");
        int page_diff = 0;
        int top_diff = 0;
        for (size_t i = 0; i < plan.lines.size() && i < full.lines.size(); ++i) {
            const LineBox& lb = full.lines[i];
            if (plan.lines[i].page != lb.page) ++page_diff;
            if (std::fabs(plan.lines[i].top_pt - lb.slotLoPt()) > 1e-3f) ++top_diff;
        }
        expectTrue(page_diff == 0, "逐行页号与整篇排版一致");
        expectTrue(top_diff == 0, "逐行行槽顶与整篇排版一致");
        expectTrue(plan.page_index + 1 == full.stats.page_count, "结束页号给出精确页数");
        if (!full.lines.empty()) {
            const float want_end = full.lines.back().slotLoPt() + full.lines.back().advance_pt +
                                   long_block.space_after_pt;
            expectNear(plan.cursor_y_pt, want_end, 1e-3, "结束游标与整篇的块尾位置一致");
        }
    }

    std::printf("\n[12] 简单竖排块：列分页与断列\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 300.0f;
        page.margins = PageMargins::all(10.0f);
        const float cw = page.contentWidthPt();
        const float cy = page.margins.top;
        const float bottom = cy + page.contentHeightPt();

        auto mkVertical = [&](const std::string& text) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = 16.0f;
            b.line_spacing.factor = 1.8f;
            b.space_after_pt = 0.0f;
            b.language = "zh";
            b.wm = WritingMode::VerticalRL();
            b.setText(text);
            return b;
        };
        auto mkLead = [&]() {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = 14.0f;
            b.line_spacing.factor = 1.0f;
            b.space_after_pt = 0.0f;
            b.language = "zh";
            b.wm = WritingMode::HorizontalTB();
            b.setText("甲乙");
            return b;
        };
        auto mkConstraints = [&](const BlockSpec& b, float first_top) {
            BlockConstraints c;
            c.width_pt = cw;
            c.writing_mode = b.wm;
            c.column.left_pt = page.margins.left;
            c.column.bottom_pt = bottom;
            c.column.first_top_pt = first_top;
            c.column.band_top_pt = cy;
            return c;
        };
        // 竖排约束的两种模式：分页的后续带都从版心上沿起排；连页的带顶是绝对坐标，
        //   逐带下移版心高。slab 取 320，与版心高 280 不同，页号与带号不会重合。
        const float cont_slab = 320.0f;
        auto mkVertBand = [&](const BlockSpec& b, float first_top, float bottom_pt,
                              float band_top) {
            BlockConstraints c;
            c.width_pt = cw;
            c.writing_mode = b.wm;
            c.column.left_pt = page.margins.left;
            c.column.bottom_pt = bottom_pt;
            c.column.first_top_pt = first_top;
            c.column.band_top_pt = band_top;
            return c;
        };
        auto mkContBand = [&](const BlockSpec& b, float first_top, float bottom_pt) {
            BlockConstraints c =
                mkVertBand(b, first_top, bottom_pt, first_top + page.contentHeightPt());
            c.column.band_step_pt = page.contentHeightPt();
            c.column.continuous = true;
            c.column.slab_pt = cont_slab;
            return c;
        };
        auto mkEmptyVertical = [&](WritingMode wm, int font, float size) {
            BlockSpec b;
            b.font_index = font;
            b.size_pt = size;
            b.line_spacing.factor = 1.8f;
            b.space_after_pt = 0.0f;
            b.language = "zh";
            b.wm = wm;
            b.setText("");
            return b;
        };

        std::string text;
        for (int i = 0; i < 30; ++i) text += "患者诉晨起后心悸，持续约十分钟。";
        const BlockSpec block = mkVertical(text);
        const LayoutResult full = layoutDocument(lib, {block}, page);
        ShapingCache cache;
        const BlockLayout local = layoutBlock(lib, block, mkConstraints(block, cy), &cache);

        expectTrue(local.status == BlockLayoutStatus::Ok && local.columns.size() >= 4 &&
                       local.columns.size() == full.lines.size() && full.stats.page_count >= 2,
                   "多列竖排块：块局部的列数与整篇相同（" + std::to_string(local.columns.size()) +
                       " 列 / " + std::to_string(full.stats.page_count) + " 页）");
        // 断列：列首尾相接，换带时列号归零、带号逐条递增。
        int chain_diff = 0;
        int band_diff = 0;
        size_t expect_begin = 0;
        int band_cursor = -1;
        int col_cursor = -1;
        for (const BlockColumn& c : local.columns) {
            if (c.line.begin != expect_begin || c.line.end <= c.line.begin) ++chain_diff;
            expect_begin = c.line.end;
            if (c.band == band_cursor) {
                if (c.column != col_cursor + 1) ++band_diff;
            } else if (c.band == band_cursor + 1) {
                if (c.column != 0) ++band_diff;
            } else {
                ++band_diff;
            }
            band_cursor = c.band;
            col_cursor = c.column;
        }
        expectTrue(chain_diff == 0, "列首尾相接，没有漏排或重排");
        expectTrue(band_diff == 0, "换带时列号归零、带号逐条递增");
        if (!full.para_index.empty()) {
            expectTrue(expect_begin == static_cast<size_t>(full.para_index[0].cluster_count),
                       "列铺满整段的簇");
        }

        // 块局部列盒与整篇列盒逐字段对拍：页号（带号）、码点范围、列位、簇位置。
        int line_diff = 0;
        for (size_t i = 0; i < full.lines.size() && i < local.lines.size(); ++i) {
            const LineBox& a = full.lines[i];
            const LineBox& b = local.lines[i];
            bool same = a.vertical == b.vertical && a.sideways == b.sideways && a.page == b.page &&
                        a.first_cp == b.first_cp && a.last_cp == b.last_cp &&
                        a.size_pt == b.size_pt && a.clusters.size() == b.clusters.size() &&
                        std::fabs(a.block_lo_pt - b.block_lo_pt) < 1e-4f &&
                        std::fabs(a.block_hi_pt - b.block_hi_pt) < 1e-4f &&
                        std::fabs(a.inline_origin_pt - b.inline_origin_pt) < 1e-4f &&
                        std::fabs(a.inline_end_pt - b.inline_end_pt) < 1e-3f &&
                        std::fabs(a.bracket_axis_pt - b.bracket_axis_pt) < 1e-4f;
            for (size_t k = 0; same && k < a.clusters.size(); ++k) {
                same = a.clusters[k].cp == b.clusters[k].cp &&
                       std::fabs(a.clusters[k].inline_pt - b.clusters[k].inline_pt) < 1e-3f;
            }
            if (!same) ++line_diff;
        }
        expectTrue(line_diff == 0, "每一列的码点范围、页号、列位与整篇逐字段相同");

        int glyph_diff = 0;
        for (size_t i = 0; i < full.glyphs.size() && i < local.glyphs.size(); ++i) {
            const PlacedGlyph& a = full.glyphs[i];
            const PlacedGlyph& b = local.glyphs[i];
            const bool same = a.glyph_id == b.glyph_id && a.page == b.page &&
                              a.vertical == b.vertical && a.rotate_deg == b.rotate_deg &&
                              a.cluster == b.cluster && std::fabs(a.x_pt - b.x_pt) < 1e-4f &&
                              std::fabs(a.y_pt - b.y_pt) < 1e-4f;
            if (!same) ++glyph_diff;
        }
        expectTrue(glyph_diff == 0, "每一枚字形的身份、坐标与页号都相同");

        int band_start_diff = 0;
        int prev_page = -1;
        for (const LineBox& lb : full.lines) {
            if (lb.page == prev_page) continue;
            if (std::fabs(lb.inline_origin_pt - cy) > 1e-4f) ++band_start_diff;
            prev_page = lb.page;
        }
        expectTrue(band_start_diff == 0, "每条带的第一列都从版心上沿起排");
        expectTrue(local.columns.back().band == full.stats.page_count - 1, "带号与页号一一对应");

        // 分页纯函数：页号随带号递增，行号在带内重新计数，块尾游标吃掉本页剩余高度。
        BlockPlanInput pin;
        pin.page_index = 0;
        pin.line_total = 0;
        pin.content_bottom_pt = bottom;
        pin.space_after_pt = 0.0f;
        pin.started_new_band = local.started_new_band;
        const BlockPlan plan = planBlockColumns(local.columns, pin);
        int plan_diff = 0;
        for (size_t i = 0; i < plan.lines.size(); ++i) {
            // 列宽不再是分页的输入：它必须等于片段的 line_h —— 这一条同时钉住
            //   "BlockColumn.width_pt 与 line.line_h 是同一件事"。
            if (plan.lines[i].page != local.columns[i].band ||
                plan.lines[i].line_no != local.columns[i].column ||
                plan.lines[i].top_pt != local.columns[i].top_pt ||
                plan.lines[i].parts.front().x != local.columns[i].center_pt ||
                plan.lines[i].parts.front().line.line_h != local.columns[i].width_pt) {
                ++plan_diff;
            }
        }
        expectTrue(!local.started_new_band && plan_diff == 0,
                   "分页：页号等于带号，行号在带内重新计数");
        expectTrue(plan.page_index == local.columns.back().band, "结束页号是最后一条带的页号");
        expectTrue(plan.line_total == local.columns.back().column + 1,
                   "结束行号只按最后一条带的列数推进（与旧循环同一本账）");
        expectNear(plan.cursor_y_pt, bottom, 1e-4, "块尾游标是版心下沿");

        // 页中起排：块首那条带更矮，第一列断得比整带起排时更早；后续带仍是满高。
        const BlockSpec lead = mkLead();
        const LayoutResult lead_full = layoutDocument(lib, {lead}, page);
        const float first_top = lead_full.lines[0].slotLoPt() + lead_full.lines[0].advance_pt;
        const LayoutResult mid_full = layoutDocument(lib, {lead, block}, page);
        const BlockLayout mid_local =
            layoutBlock(lib, block, mkConstraints(block, first_top), &cache);
        const BlockLayout top_local = layoutBlock(lib, block, mkConstraints(block, cy), &cache);
        expectTrue(mid_local.status == BlockLayoutStatus::Ok && !mid_local.started_new_band &&
                       mid_local.columns.size() == mid_full.lines.size() - 1,
                   "页中起排：第一条带仍留在本页，列数与整篇相同");
        expectTrue(mid_local.columns.size() >= 2 &&
                       mid_local.columns[0].line.end < top_local.columns[0].line.end,
                   "页中起排的第一带更矮，第一列断得更早（断列真的按列顶算）");
        if (!mid_local.columns.empty()) {
            expectNear(mid_local.columns[0].top_pt, first_top, 1e-4, "首列列顶就是块首列顶");
        }
        int later_top_diff = 0;
        for (const BlockColumn& c : mid_local.columns) {
            if (c.band == 0) continue;
            if (std::fabs(c.top_pt - cy) > 1e-4f) ++later_top_diff;
        }
        expectTrue(later_top_diff == 0, "第二条带起各列都从版心上沿起排");

        // 块首带放不下两条列位：块布局另起一条带，整篇在此翻页。
        PageMetrics tiny = page;
        tiny.height_pt = 80.0f;  // 版心高 60
        const float tiny_bottom = tiny.margins.top + tiny.contentHeightPt();
        const LayoutResult lead_tiny = layoutDocument(lib, {lead}, tiny);
        const float tiny_first_top = lead_tiny.lines[0].slotLoPt() + lead_tiny.lines[0].advance_pt;
        BlockConstraints fresh = mkConstraints(block, tiny_first_top);
        fresh.column.bottom_pt = tiny_bottom;
        const BlockLayout fresh_local = layoutBlock(lib, block, fresh, &cache);
        expectTrue(fresh_local.status == BlockLayoutStatus::Ok && fresh_local.started_new_band &&
                       !fresh_local.columns.empty() && fresh_local.columns[0].band == 0,
                   "块首带放不下两条列位时另起一条带");
        BlockPlanInput fresh_pin;
        fresh_pin.page_index = 3;
        fresh_pin.line_total = 5;
        fresh_pin.content_bottom_pt = tiny_bottom;
        fresh_pin.started_new_band = fresh_local.started_new_band;
        const BlockPlan fresh_plan = planBlockColumns(fresh_local.columns, fresh_pin);
        expectTrue(!fresh_plan.lines.empty() && fresh_plan.lines[0].page == 4 &&
                       std::fabs(fresh_plan.lines[0].top_pt - tiny.margins.top) < 1e-4f,
                   "另起的那条带落在下一页，列顶是版心上沿");
        const LayoutResult fresh_full = layoutDocument(lib, {lead, block}, tiny);
        expectTrue(fresh_full.lines.size() >= 2 && fresh_full.lines[1].page == 1 &&
                       std::fabs(fresh_full.lines[1].inline_origin_pt - tiny.margins.top) < 1e-4f,
                   "整篇：竖排块因放不下两条列位从第 1 页起");

        // 竖排块吃掉本页剩余高度：后面的横排块另起一页。
        const BlockSpec lead_block = mkLead();
        const LayoutResult with_tail = layoutDocument(lib, {block, lead_block}, page);
        expectTrue(with_tail.lines.size() > full.lines.size() &&
                       with_tail.lines[full.lines.size()].page == full.stats.page_count,
                   "竖排块之后的横排块另起一页（竖排吃掉本页剩余高度）");

        // 侧倒（蒙文）走同一条列路径：列序、列位与旋转都要一致。
        BlockSpec sideways;
        sideways.font_index = mn;
        sideways.size_pt = 20.0f;
        sideways.line_spacing.factor = 1.8f;
        sideways.language = "mn";
        sideways.wm = WritingMode::VerticalLR();
        std::string mnText;
        for (int i = 0; i < 12; ++i) {
            mnText += "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD";
        }
        sideways.setText(mnText);
        const LayoutResult mn_full = layoutDocument(lib, {sideways}, page);
        const BlockLayout mn_local =
            layoutBlock(lib, sideways, mkConstraints(sideways, cy), &cache);
        int mn_diff = 0;
        for (size_t i = 0; i < mn_full.lines.size() && i < mn_local.lines.size(); ++i) {
            const LineBox& a = mn_full.lines[i];
            const LineBox& b = mn_local.lines[i];
            const bool same = a.sideways == b.sideways && a.page == b.page &&
                              std::fabs(a.block_lo_pt - b.block_lo_pt) < 1e-4f &&
                              std::fabs(a.block_hi_pt - b.block_hi_pt) < 1e-4f &&
                              a.first_cp == b.first_cp && a.last_cp == b.last_cp;
            if (!same) ++mn_diff;
        }
        expectTrue(mn_local.status == BlockLayoutStatus::Ok &&
                       mn_local.columns.size() == mn_full.lines.size() && mn_diff == 0 &&
                       !mn_local.lines.empty() && mn_local.lines[0].sideways,
                   "侧倒块也走块局部列路径（列序与旋转不变）");

        // 约束不成立时不猜几何。
        BlockConstraints bad = mkConstraints(block, cy);
        bad.column.bottom_pt = bad.column.band_top_pt;
        const BlockLayout bad_local = layoutBlock(lib, block, bad, nullptr);
        expectTrue(
            bad_local.status == BlockLayoutStatus::InvalidConstraints && bad_local.columns.empty(),
            "竖带下沿不高于带顶时明确报告约束无效");

        // 空竖排段：块局部给出一条空列，整篇只重打全局页号与行号；列宽是空行行高，
        //   列心与列盒沿用竖带那把尺子。
        {
            const BlockSpec empty_v = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            const LayoutResult empty_full = layoutDocument(lib, {empty_v}, page);
            const BlockLayout empty_local =
                layoutBlock(lib, empty_v, mkVertBand(empty_v, cy, bottom, cy), &cache);
            expectTrue(empty_full.lines.size() == 1 && empty_full.lines[0].vertical &&
                           empty_full.lines[0].clusters.empty() && empty_full.glyphs.empty(),
                       "空竖排段：整篇只有一个空列盒，没有字形");
            expectTrue(empty_local.status == BlockLayoutStatus::Ok &&
                           empty_local.columns.size() == 1 && empty_local.lines.size() == 1 &&
                           empty_local.glyphs.empty(),
                       "空竖排段：块局部排出一条空列");
            expectTrue(empty_full.lines.size() == 1 && empty_local.lines.size() == 1 &&
                           empty_local.columns.size() == 1,
                       "空竖排段：块局部与整篇的列数相同");
            if (empty_full.lines.size() == 1 && empty_local.lines.size() == 1 &&
                empty_local.columns.size() == 1) {
                const LineBox& got = empty_full.lines[0];
                const LineBox& want = empty_local.lines[0];
                const BlockColumn& col = empty_local.columns[0];
                const bool same = got.vertical == want.vertical && got.sideways == want.sideways &&
                                  got.size_pt == want.size_pt && got.first_cp == want.first_cp &&
                                  got.last_cp == want.last_cp &&
                                  std::fabs(got.block_lo_pt - want.block_lo_pt) < 1e-4f &&
                                  std::fabs(got.block_hi_pt - want.block_hi_pt) < 1e-4f &&
                                  std::fabs(got.inline_origin_pt - want.inline_origin_pt) < 1e-4f &&
                                  std::fabs(got.inline_end_pt - want.inline_end_pt) < 1e-4f &&
                                  std::fabs(got.bracket_axis_pt - want.bracket_axis_pt) < 1e-4f;
                expectTrue(same, "空竖排段：列盒几何与块局部逐字段相同");
                expectNear(got.block_hi_pt - got.block_lo_pt, col.width_pt, 1e-4,
                           "空竖排段的列宽就是空行行高");
                expectNear(got.block_lo_pt + col.width_pt * 0.5f,
                           page.margins.left + cw - col.width_pt * 0.5f, 1e-4,
                           "中日竖排空列从右边缘往左推半个列宽");
                expectTrue(got.page == 0 && got.line == 0 && got.paragraph == 0,
                           "空竖排段落在第 0 页第 0 行");
                expectNear(got.inline_origin_pt, cy, 1e-4, "空竖排段的列顶就是版心上沿");
            }
            // 空竖排吃掉本页剩余高度：后续横排段另起一页。
            const LayoutResult after = layoutDocument(lib, {empty_v, lead}, page);
            expectTrue(after.lines.size() == 2 && after.lines[1].page == 1 &&
                           std::fabs(after.lines[1].block_lo_pt - cy) < 1e-3f,
                       "空竖排段吃掉本页剩余高度，后续横排段另起一页");
            // 页中起排：列顶是前一段结束的位置，不翻页。
            const LayoutResult mid = layoutDocument(lib, {lead, empty_v}, page);
            expectTrue(mid.lines.size() == 2 && mid.lines[1].page == 0,
                       "空竖排段可以接在页中的横排段之后");
            if (mid.lines.size() == 2) {
                expectNear(mid.lines[1].inline_origin_pt, mid.lines[0].block_hi_pt, 1e-4,
                           "页中起排的空竖排段：列顶是前一段的结束位置");
            }
            // 段前分页与段前距：翻页之后落在版心顶，段前距不再叠加；页中起排照加。
            BlockSpec break_v = empty_v;
            break_v.page_break_before = true;
            break_v.space_before_pt = 25.0f;
            const LayoutResult broke = layoutDocument(lib, {lead, break_v}, page);
            expectTrue(broke.lines.size() == 2 && broke.lines[1].page == 1 &&
                           std::fabs(broke.lines[1].inline_origin_pt - cy) < 1e-4f,
                       "空竖排段：段前分页生效，翻页之后段前距不再叠加");
            BlockSpec spaced_v = empty_v;
            spaced_v.space_before_pt = 25.0f;
            const LayoutResult spaced = layoutDocument(lib, {lead, spaced_v}, page);
            expectTrue(spaced.lines.size() == 2 && spaced.lines[1].page == 0,
                       "空竖排段：段前距不改变页号");
            if (spaced.lines.size() == 2) {
                expectNear(spaced.lines[1].inline_origin_pt, spaced.lines[0].block_hi_pt + 25.0f,
                           1e-4, "空竖排段：页中起排照样让出段前距");
            }
        }

        // 空竖排段的占位：括号与提示文字进簇、原子撑高列盒、侧倒的笔位退回列心。
        {
            BlockSpec vctl = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            {
                InlineObject open;
                open.cp = 0;
                open.kind = InlineObjectKind::ControlStart;
                open.owner = 3;
                open.hint = "请输入";
                vctl.objects.push_back(open);
                InlineObject close;
                close.cp = 0;
                close.kind = InlineObjectKind::ControlEnd;
                close.owner = 3;
                vctl.objects.push_back(close);
            }
            const LayoutResult full_ctl = layoutDocument(lib, {vctl}, page);
            const BlockLayout local_ctl =
                layoutBlock(lib, vctl, mkVertBand(vctl, cy, bottom, cy), &cache);
            expectTrue(local_ctl.status == BlockLayoutStatus::Ok && full_ctl.lines.size() == 1 &&
                           local_ctl.lines.size() == 1 && local_ctl.lines[0].clusters.size() == 3,
                       "空竖排段的括号与提示文字进簇（开括号 / 提示 / 闭括号）");
            if (full_ctl.lines.size() == 1 && local_ctl.lines.size() == 1) {
                const LineBox& got = full_ctl.lines[0];
                const LineBox& want = local_ctl.lines[0];
                bool same = got.clusters.size() == want.clusters.size() &&
                            std::fabs(got.inline_end_pt - want.inline_end_pt) < 1e-4f &&
                            std::fabs(got.bracket_axis_pt - want.bracket_axis_pt) < 1e-4f;
                for (size_t k = 0; same && k < got.clusters.size(); ++k) {
                    same =
                        got.clusters[k].cp == want.clusters[k].cp &&
                        got.clusters[k].placeholder == want.clusters[k].placeholder &&
                        got.clusters[k].control_hint == want.clusters[k].control_hint &&
                        got.clusters[k].bracket_side == want.clusters[k].bracket_side &&
                        got.clusters[k].owner_control == want.clusters[k].owner_control &&
                        std::fabs(got.clusters[k].inline_pt - want.clusters[k].inline_pt) < 1e-4f;
                }
                expectTrue(same, "空竖排段的占位簇与块局部逐字段相同");
            }
            // 侧倒的空列：没有字形时笔位退回列心（整篇的空列就是这一份几何）。
            const BlockSpec vside = mkEmptyVertical(WritingMode::VerticalLR(), mn, 20.0f);
            const LayoutResult full_side = layoutDocument(lib, {vside}, page);
            expectTrue(full_side.lines.size() == 1 && full_side.lines[0].sideways,
                       "侧倒空竖排段仍然记侧倒");
            if (full_side.lines.size() == 1) {
                const LineBox& lb = full_side.lines[0];
                expectNear(lb.bracket_axis_pt,
                           lb.block_lo_pt + (lb.block_hi_pt - lb.block_lo_pt) * 0.5f, 1e-4,
                           "侧倒空列的笔位退回列心");
            }
            // 原子撑高列盒：空段里只有一枚图片时，列宽按盒高定，不是按空行字号。
            BlockSpec vatom = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            {
                InlineObject img;
                img.cp = 0;
                img.kind = InlineObjectKind::Atom;
                img.atom = InlineAtom::Image;
                img.w_pt = 30.0f;
                img.h_pt = 24.0f;
                vatom.objects.push_back(img);
            }
            const LayoutResult full_atom = layoutDocument(lib, {vatom}, page);
            const LayoutResult full_plain =
                layoutDocument(lib, {mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f)}, page);
            expectTrue(full_atom.lines.size() == 1 && full_plain.lines.size() == 1 &&
                           (full_atom.lines[0].block_hi_pt - full_atom.lines[0].block_lo_pt) >
                               (full_plain.lines[0].block_hi_pt - full_plain.lines[0].block_lo_pt),
                       "空竖排段里的原子撑高列盒（列宽按盒高定）");
            if (full_atom.lines.size() == 1) {
                expectTrue(full_atom.lines[0].clusters.size() == 1 &&
                               full_atom.lines[0].clusters[0].atom == InlineAtom::Image,
                           "空竖排段的原子进簇并带盒高");
            }
        }

        // 块首带放不下两条列位：空竖排段也另起一条带，整篇在这里翻页。
        {
            PageMetrics tiny = page;
            tiny.height_pt = 80.0f;  // 版心高 60，空列宽约 41.7
            const float tiny_bottom = tiny.margins.top + tiny.contentHeightPt();
            const BlockSpec empty_v = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            const LayoutResult tiny_lead = layoutDocument(lib, {lead}, tiny);
            const float first_top =
                tiny_lead.lines.empty() ? tiny.margins.top : tiny_lead.lines[0].block_hi_pt;
            const BlockLayout local =
                layoutBlock(lib, empty_v,
                            mkVertBand(empty_v, first_top, tiny_bottom, tiny.margins.top), &cache);
            expectTrue(local.status == BlockLayoutStatus::Ok && local.started_new_band &&
                           local.columns.size() == 1 &&
                           std::fabs(local.columns[0].top_pt - tiny.margins.top) < 1e-4f,
                       "空竖排段：块首带放不下两条列位时另起一条带");
            BlockPlanInput pin;
            pin.page_index = 3;
            pin.line_total = 5;
            pin.content_bottom_pt = tiny_bottom;
            pin.started_new_band = local.started_new_band;
            const BlockPlan plan = planBlockColumns(local.columns, pin);
            expectTrue(plan.lines.size() == 1 && plan.lines[0].page == 4 &&
                           std::fabs(plan.lines[0].top_pt - tiny.margins.top) < 1e-4f,
                       "空竖排段另起的带落在下一页，列顶是版心上沿");
            const LayoutResult full = layoutDocument(lib, {lead, empty_v}, tiny);
            expectTrue(full.lines.size() == 2 && full.lines[1].page == 1 &&
                           std::fabs(full.lines[1].inline_origin_pt - tiny.margins.top) < 1e-4f,
                       "整篇：空竖排段因放不下两条列位从第 1 页起");
        }

        // 连页竖排：带顶是块轴绝对坐标，逐带下移版心高；容量仍是"带下沿减绝对带顶"的
        //   旧式子，块首带从页中起排时首列断得更早，后续带的容量也不会变成常量带高。
        {
            PageMetrics cont = page;
            cont.continuous = true;
            cont.slab_pt = cont_slab;
            std::string czh;
            for (int i = 0; i < 30; ++i) czh += "患者诉晨起后心悸，持续约十分钟。";
            const BlockSpec cblock = mkVertical(czh);
            const LayoutResult lead_cont = layoutDocument(lib, {lead}, cont);
            expectTrue(lead_cont.lines.size() == 1, "前提：连页首段只排一行");
            const float first_top = lead_cont.lines.empty() ? cy : lead_cont.lines[0].block_hi_pt;
            expectTrue(first_top > cy + 1.0f, "前提：竖排块首列顶落在页中");
            const BlockLayout from_top =
                layoutBlock(lib, cblock, mkContBand(cblock, cy, bottom), &cache);
            const BlockLayout local =
                layoutBlock(lib, cblock, mkContBand(cblock, first_top, bottom), &cache);
            expectTrue(local.status == BlockLayoutStatus::Ok &&
                           from_top.status == BlockLayoutStatus::Ok && !local.columns.empty() &&
                           local.columns.size() == local.lines.size(),
                       "连页竖排块：块局部支持，每列都有列盒");
            expectTrue(from_top.columns.size() >= 1 && local.columns.size() >= 2 &&
                           local.columns[0].line.end < from_top.columns[0].line.end,
                       "连页首带更矮：首列断得比整带起排时更早");
            // 带顶逐带累加：换带的那一列，列顶等于上一带列顶加版心高。
            int band_top_diff = 0;
            int same_band_top_diff = 0;
            int prev_band = -1;
            float prev_band_top = 0.0f;
            for (const BlockColumn& c : local.columns) {
                if (c.band != prev_band) {
                    if (prev_band >= 0 &&
                        std::fabs(c.top_pt - (prev_band_top + page.contentHeightPt())) > 1e-4f) {
                        ++band_top_diff;
                    }
                    prev_band = c.band;
                    prev_band_top = c.top_pt;
                } else if (std::fabs(c.top_pt - prev_band_top) > 1e-4f) {
                    ++same_band_top_diff;
                }
            }
            expectTrue(band_top_diff == 0, "连页的带顶逐带累加版心高（绝对坐标，不是常量带高）");
            expectTrue(same_band_top_diff == 0, "同一条带里的列共用一个列顶");
            // 容量 = 带下沿减绝对带顶：越往后的带顶越靠下，容量随之变成负数，一列只放
            //   得下一个簇。改成"常量带高"的实现会让后续带仍是满列。
            expectTrue(local.columns.back().top_pt > bottom,
                       "连页带顶可以越过带下沿（容量按绝对差值算，不是常量带高）");
            int short_column_diff = 0;
            for (const BlockColumn& c : local.columns) {
                if (c.band < 2) continue;
                if (c.line.end - c.line.begin != 1) ++short_column_diff;
            }
            expectTrue(short_column_diff == 0,
                       "容量为负的后续带一列只放得下一个簇（绝对带顶公式的直接后果）");
            // 页号与页内坐标由列顶现算：页号 = floor(列顶 / 条带高)。
            int page_diff = 0;
            int origin_diff = 0;
            for (size_t i = 0; i < local.columns.size() && i < local.lines.size(); ++i) {
                const BlockColumn& c = local.columns[i];
                const LineBox& lb = local.lines[i];
                const int want_page = static_cast<int>(std::floor(c.top_pt / cont_slab));
                if (lb.page != want_page) ++page_diff;
                if (std::fabs(lb.inline_origin_pt - (c.top_pt - want_page * cont_slab)) > 1e-4f)
                    ++origin_diff;
            }
            expectTrue(page_diff == 0, "连页列盒的页号由列顶与条带高现算");
            expectTrue(origin_diff == 0, "连页列盒的 y 折回页内（列顶减页号乘条带高）");
            // 整篇与块局部对拍：列序、页号、列位与簇位置逐字段相同。
            const LayoutResult full = layoutDocument(lib, {lead, cblock}, cont);
            expectTrue(full.lines.size() == local.lines.size() + 1, "连页竖排块的列数与整篇相同");
            int line_diff = 0;
            for (size_t i = 0; i < local.lines.size() && i + 1 < full.lines.size(); ++i) {
                const LineBox& a = full.lines[i + 1];
                const LineBox& b = local.lines[i];
                const bool same = a.vertical && b.vertical && a.first_cp == b.first_cp &&
                                  a.last_cp == b.last_cp && a.page == b.page &&
                                  std::fabs(a.block_lo_pt - b.block_lo_pt) < 1e-4f &&
                                  std::fabs(a.block_hi_pt - b.block_hi_pt) < 1e-4f &&
                                  std::fabs(a.inline_origin_pt - b.inline_origin_pt) < 1e-4f &&
                                  std::fabs(a.inline_end_pt - b.inline_end_pt) < 1e-3f;
                if (!same) ++line_diff;
            }
            expectTrue(line_diff == 0, "连页竖排块：整篇每一列的码点范围、页号与列位都相同");
            // 分页纯函数：页号由列顶现算，块尾游标是最后一条带的带顶加版心高。
            BlockPlanInput pin;
            pin.page_index = 4;
            pin.line_total = 9;
            pin.content_bottom_pt = bottom;
            pin.started_new_band = local.started_new_band;
            pin.continuous = true;
            pin.slab_pt = cont_slab;
            pin.band_height_pt = page.contentHeightPt();
            const BlockPlan plan = planBlockColumns(local.columns, pin);
            expectTrue(plan.page_index == 4, "连页：结束页号原样回传");
            expectTrue(plan.line_total == 9 + local.columns.back().column + 1,
                       "连页：行号只按最后一条带的列数推进");
            expectNear(plan.cursor_y_pt, local.columns.back().top_pt + page.contentHeightPt(), 1e-4,
                       "连页块尾游标 = 最后列顶 + 版心高");
            int plan_page_diff = 0;
            for (size_t i = 0; i < plan.lines.size(); ++i) {
                if (plan.lines[i].page !=
                    static_cast<int>(std::floor(local.columns[i].top_pt / cont_slab))) {
                    ++plan_page_diff;
                }
            }
            expectTrue(plan_page_diff == 0, "连页分页：每一列的页号由列顶现算");
            // 块尾之后的内容从那个绝对位置接着排。
            const LayoutResult tail = layoutDocument(lib, {lead, cblock, lead}, cont);
            expectTrue(tail.lines.size() == full.lines.size() + 1, "连页竖排块之后还能接横排段");
            if (tail.lines.size() == full.lines.size() + 1) {
                const LineBox& t = tail.lines.back();
                const float abs_y = static_cast<float>(t.page) * cont_slab + t.block_lo_pt;
                expectNear(abs_y, plan.cursor_y_pt, 1e-3, "连页竖排块之后的内容从块尾游标接着排");
            }
        }

        // 连页空竖排段：只占一条列宽的横带，块尾游标是列顶加列宽（不是版心高）。
        {
            PageMetrics cont = page;
            cont.continuous = true;
            cont.slab_pt = cont_slab;
            const BlockSpec empty_v = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            const LayoutResult one = layoutDocument(lib, {empty_v}, cont);
            expectTrue(one.lines.size() == 1 && one.lines[0].page == 0 &&
                           std::fabs(one.lines[0].inline_origin_pt - cy) < 1e-4f,
                       "连页空竖排段：一列落在第一条带，列顶是块轴起点");
            const float column_w =
                one.lines.empty() ? 0.0f : (one.lines[0].block_hi_pt - one.lines[0].block_lo_pt);
            const LayoutResult two = layoutDocument(lib, {empty_v, lead}, cont);
            expectTrue(two.lines.size() == 2, "连页空竖排段之后还能接横排段");
            if (two.lines.size() == 2) {
                const LineBox& t = two.lines[1];
                const float abs_y = static_cast<float>(t.page) * cont_slab + t.block_lo_pt;
                expectNear(abs_y, cy + column_w, 1e-3,
                           "连页空竖排段只占一条列宽的横带（块尾游标 = 列顶 + 列宽）");
            }
            const LayoutResult three = layoutDocument(lib, {empty_v, empty_v, lead}, cont);
            expectTrue(three.lines.size() == 3, "连页多个空竖排段逐段累加");
            if (three.lines.size() == 3) {
                const LineBox& t = three.lines[2];
                const float abs_y = static_cast<float>(t.page) * cont_slab + t.block_lo_pt;
                expectNear(abs_y, cy + column_w * 2.0f, 1e-3, "连页两个空竖排段让出两条列宽");
            }
            // 块局部的页内 y：第一条带上页号 0，列顶与块轴起点重合。
            const BlockLayout local =
                layoutBlock(lib, empty_v, mkContBand(empty_v, cy, bottom), &cache);
            expectTrue(local.status == BlockLayoutStatus::Ok && local.columns.size() == 1 &&
                           local.lines.size() == 1 && local.lines[0].page == 0,
                       "连页空竖排段：块局部页号由列顶现算");
            if (local.lines.size() == 1) {
                expectNear(local.lines[0].inline_origin_pt, cy, 1e-4,
                           "连页空竖排段：页内 y 就是块轴 y（第一条带）");
            }
        }

        // 连页分页纯函数：页号由列顶现算，块尾游标是最后列顶加带高加段后距。
        {
            auto mkCol = [](float top, int band, int col, float width) {
                BlockColumn c;
                c.top_pt = top;
                c.band = band;
                c.column = col;
                c.width_pt = width;
                c.center_pt = top;
                return c;
            };
            const std::vector<BlockColumn> cols = {
                mkCol(30.0f, 0, 0, 40.0f), mkCol(310.0f, 1, 0, 40.0f), mkCol(590.0f, 2, 1, 40.0f)};
            BlockPlanInput pin;
            pin.continuous = true;
            pin.slab_pt = 320.0f;
            pin.band_height_pt = 280.0f;
            pin.page_index = 7;
            pin.line_total = 5;
            pin.space_after_pt = 3.0f;
            pin.content_bottom_pt = 300.0f;
            const BlockPlan r = planBlockColumns(cols, pin);
            expectTrue(r.lines.size() == 3 && r.lines[0].page == 0 && r.lines[1].page == 0 &&
                           r.lines[2].page == 1,
                       "连页分页：页号 = floor(列顶 / 条带高)");
            expectTrue(r.page_index == 7, "连页分页：结束页号原样回传");
            expectTrue(r.line_total == 7, "连页分页：行号按最后一条带的列数推进");
            expectNear(r.cursor_y_pt, 590.0f + 280.0f + 3.0f, 1e-4,
                       "连页分页：结束游标 = 最后列顶 + 带高 + 段后距");
        }

        // 断行层没有整形结果：空块不给片段与列，空行由 layoutBlock 给，两条路径分工明确。
        {
            const BlockSpec empty_v = mkEmptyVertical(WritingMode::VerticalRL(), cjk, 16.0f);
            const BlockLayout r = layoutBlockFragments(empty_v, mkVertBand(empty_v, cy, bottom, cy),
                                                       BlockShaping{}, {});
            expectTrue(
                r.status == BlockLayoutStatus::Ok && r.fragments.empty() && r.columns.empty(),
                "空块在断行层没有片段与列（空行与空列由 layoutBlock 给）");
        }
    }

    std::printf("\n[13] 浮动图排除：块局部绕排与页流\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 300.0f;
        page.margins = PageMargins::all(10.0f);

        auto mkText = [&](const std::string& text, float space_after) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = 12.0f;
            b.line_spacing.factor = 1.6f;
            b.space_after_pt = space_after;
            b.language = "zh";
            b.setText(text);
            return b;
        };
        auto mkFloat = [](int para, uint32_t cp, float w, float h, ImageWrap wrap, float dx,
                          float dy) {
            LayoutFloat f;
            f.handle = 100 + static_cast<uint64_t>(para) * 10 + cp;
            f.para = para;
            f.cp = cp;
            f.w_pt = w;
            f.h_pt = h;
            f.wrap = wrap;
            f.dx_pt = dx;
            f.dy_pt = dy;
            f.content_hash = 0x1234;
            return f;
        };
        const std::string sentence = "患者诉晨起后心悸明显，持续约十分钟，休息后缓解。";
        std::string long_text;
        for (int i = 0; i < 12; ++i) long_text += sentence;

        auto run = [&](const std::string& name, const std::vector<BlockSpec>& blocks,
                       const PageMetrics& pg,
                       const std::vector<LayoutFloat>& floats) -> LayoutResult {
            LayoutResult r = layoutDocument(lib, blocks, pg, {}, floats);
            std::printf("     %s: digest=0x%016llx lines=%zu glyphs=%zu pages=%d floats=%zu\n",
                        name.c_str(), static_cast<unsigned long long>(floatCaseDigest(r)),
                        r.lines.size(), r.glyphs.size(), r.stats.page_count,
                        r.placed_floats.size());
            for (const PlacedFloat& pf : r.placed_floats) {
                std::printf("       float page=%d x=%.3f y=%.3f w=%.1f h=%.1f wrap=%d\n", pf.page,
                            pf.x, pf.y, pf.w, pf.h, static_cast<int>(pf.wrap));
            }
            return r;
        };
        auto checkDigest = [&](const LayoutResult& r, uint64_t want, const std::string& what) {
            const uint64_t got = floatCaseDigest(r);
            char buf[320] = {};
            std::snprintf(buf, sizeof(buf), "%s（实际 0x%016llx）", what.c_str(),
                          static_cast<unsigned long long>(got));
            expectTrue(got == want, buf);
        };

        // 指纹取自切换前的旧循环：行盒、字形与浮动几何逐位相同才算没有改输出。
        const uint64_t kPlainDigest = 0xa83c5f924f6e6252ull;
        const uint64_t kAroundRightDigest = 0x476081211a78aa59ull;
        const uint64_t kAroundMiddleDigest = 0x43ec847165b8f2afull;
        const uint64_t kAboveDigest = 0x44d82abf84a4b6bfull;
        const uint64_t kBehindDigest = 0x5a5b45cda77eccd6ull;
        const uint64_t kInFrontDigest = 0xc81d49c2e908b863ull;
        const uint64_t kAcrossDigest = 0xa25ebaa45bd6a664ull;
        const uint64_t kShiftDigest = 0x521d737ed50bafbcull;
        const uint64_t kReflowDigest = 0x53a5e7e76147fddbull;
        const uint64_t kContinuousDigest = 0xaf22ded7cc6e5eedull;

        const BlockSpec long_block = mkText(long_text, 0.0f);
        const LayoutResult plain = run("plain", {long_block}, page, {});
        checkDigest(plain, kPlainDigest, "无浮动图：简单块分页的摘要未变");

        const LayoutResult around_right = run("around-right", {long_block}, page,
                                              {mkFloat(0, 0, 120, 80, ImageWrap::Square, 0, 0)});
        checkDigest(around_right, kAroundRightDigest, "四周型贴左：摘要未变");
        {
            const PlacedFloat& pf = around_right.placed_floats.front();
            int narrowed = 0;
            bool origin_ok = true;
            for (const LineBox& lb : around_right.lines) {
                if (lb.page != pf.page) continue;
                if (lb.block_hi_pt <= pf.y || lb.block_lo_pt >= pf.y + pf.h) continue;
                ++narrowed;
                if (lb.inline_origin_pt < pf.x + pf.w + kFloatWrapGapPt - 0.5f) origin_ok = false;
            }
            expectTrue(narrowed >= 3 && origin_ok,
                       "四周型贴左：图旁的行从图右侧起排（" + std::to_string(narrowed) + " 行）");
            expectTrue(around_right.lines.size() > plain.lines.size(),
                       "四周型让出宽度之后行变多（排除带真的进了断行）");
        }

        const LayoutResult around_middle = run("around-middle", {long_block}, page,
                                               {mkFloat(0, 0, 80, 60, ImageWrap::Square, 100, 0)});
        checkDigest(around_middle, kAroundMiddleDigest, "四周型居中：摘要未变");
        {
            const PlacedFloat& pf = around_middle.placed_floats.front();
            int two_sided = 0;
            bool sides_ok = true;
            for (const LineBox& lb : around_middle.lines) {
                if (lb.page != pf.page) continue;
                if (lb.block_hi_pt <= pf.y || lb.block_lo_pt >= pf.y + pf.h) continue;
                if (lb.spans.size() < 2) continue;
                ++two_sided;
                const float gap_lo = pf.x - lb.inline_origin_pt;
                const float gap_hi = pf.x + pf.w - lb.inline_origin_pt;
                bool any_left = false;
                bool any_right = false;
                bool in_gap = false;
                for (const LineCluster& c : lb.clusters) {
                    if (c.inline_pt < gap_lo - 1.0f) any_left = true;
                    if (c.inline_pt >= gap_hi) any_right = true;
                    if (c.inline_pt > gap_lo && c.inline_pt < gap_hi) in_gap = true;
                }
                if (!any_left || !any_right || in_gap) sides_ok = false;
            }
            expectTrue(two_sided > 0 && sides_ok, "四周型居中：一行分成左右两段，簇不落在图上");
        }

        const LayoutResult above =
            run("above", {long_block}, page, {mkFloat(0, 0, 120, 80, ImageWrap::None, 0, 0)});
        checkDigest(above, kAboveDigest, "上下型：摘要未变");
        {
            const PlacedFloat& pf = above.placed_floats.front();
            expectNear(above.lines.front().slotLoPt(), pf.y + pf.h, 1e-3,
                       "上下型：首行从图底开始（行被整行下推）");
        }

        const LayoutResult behind =
            run("behind", {long_block}, page, {mkFloat(0, 0, 120, 80, ImageWrap::Behind, 0, 0)});
        checkDigest(behind, kBehindDigest, "衬于下方：摘要未变（只记浮动几何）");
        {
            bool same = plain.lines.size() == behind.lines.size();
            for (size_t i = 0; same && i < plain.lines.size(); ++i) {
                const LineBox& a = plain.lines[i];
                const LineBox& b = behind.lines[i];
                same = a.page == b.page && a.first_cp == b.first_cp && a.last_cp == b.last_cp &&
                       std::fabs(a.slotLoPt() - b.slotLoPt()) < 1e-4f &&
                       std::fabs(a.inline_origin_pt - b.inline_origin_pt) < 1e-4f &&
                       std::fabs(a.inline_end_pt - b.inline_end_pt) < 1e-4f &&
                       a.spans.size() == b.spans.size();
            }
            expectTrue(same, "衬于下方不占位：行几何与无浮动图逐行相同");
        }

        const LayoutResult infront =
            run("infront", {long_block}, page, {mkFloat(0, 0, 120, 80, ImageWrap::InFront, 0, 0)});
        checkDigest(infront, kInFrontDigest, "浮于上方：摘要未变（只记浮动几何）");
        {
            bool same = plain.lines.size() == infront.lines.size();
            for (size_t i = 0; same && i < plain.lines.size(); ++i) {
                const LineBox& a = plain.lines[i];
                const LineBox& b = infront.lines[i];
                same = a.page == b.page && a.first_cp == b.first_cp && a.last_cp == b.last_cp &&
                       std::fabs(a.slotLoPt() - b.slotLoPt()) < 1e-4f &&
                       std::fabs(a.inline_end_pt - b.inline_end_pt) < 1e-4f;
            }
            expectTrue(same, "浮于上方不占位：行几何与无浮动图逐行相同");
        }

        const BlockSpec short_block = mkText("甲", 0.0f);
        const LayoutResult across = run("across-blocks", {short_block, long_block}, page,
                                        {mkFloat(0, 0, 120, 80, ImageWrap::Square, 0, 0)});
        checkDigest(across, kAcrossDigest, "浮动图跨段生效：摘要未变");
        {
            const LineBox* first_next = nullptr;
            for (const LineBox& lb : across.lines) {
                if (lb.paragraph == 1) {
                    first_next = &lb;
                    break;
                }
            }
            expectTrue(
                first_next != nullptr && first_next->inline_origin_pt > page.margins.left + 60.0f,
                "跨段：下一段的首行仍让开上一段锚定的图");
        }

        PageMetrics small = page;
        small.height_pt = 120.0f;
        const BlockSpec lead = mkText("甲乙", 85.0f);
        const LayoutResult shifted = run("shift", {lead, long_block}, small,
                                         {mkFloat(1, 0, 120, 30, ImageWrap::Square, 0, 0)});
        checkDigest(shifted, kShiftDigest, "段首行被翻页推走：摘要未变");
        {
            const PlacedFloat& pf = shifted.placed_floats.front();
            const LineBox* first_next = nullptr;
            for (const LineBox& lb : shifted.lines) {
                if (lb.paragraph == 1) {
                    first_next = &lb;
                    break;
                }
            }
            expectTrue(pf.page == 1, "段首平移：图跟着段首行落到第 1 页");
            expectNear(pf.y, 17.194, 0.02, "段首平移：图在页内下移了同一段位移");
            expectTrue(first_next != nullptr && first_next->page == 1 &&
                           first_next->inline_origin_pt > pf.x + 60.0f,
                       "段首平移之后的首行仍按平移后的图让宽");
        }

        PageMetrics tiny = page;
        tiny.height_pt = 120.0f;
        const BlockSpec pad = mkText("甲乙", 57.0f);
        const LayoutResult reflow = run("page-reflow", {pad, long_block}, tiny,
                                        {mkFloat(1, 0, 120, 60, ImageWrap::Square, 0, 0)});
        checkDigest(reflow, kReflowDigest, "页底翻页重排：摘要未变");
        {
            const PlacedFloat& pf = reflow.placed_floats.front();
            size_t first_next = reflow.lines.size();
            for (size_t i = 0; i < reflow.lines.size(); ++i) {
                if (reflow.lines[i].paragraph == 1) {
                    first_next = i;
                    break;
                }
            }
            expectTrue(pf.page == 0 && first_next + 1 < reflow.lines.size(),
                       "页底重排：图留在原页，续排在下一页");
            if (first_next + 1 < reflow.lines.size()) {
                expectTrue(reflow.lines[first_next].page == 1 &&
                               reflow.lines[first_next].inline_origin_pt > pf.x + 60.0f,
                           "页底重排：下一页首行仍让开图（宽度由翻页后的块轴位置给）");
                expectNear(reflow.lines[first_next + 1].inline_origin_pt, page.margins.left, 1e-3,
                           "页底重排：图下沿之后恢复整宽");
            }
        }

        PageMetrics cont = page;
        cont.continuous = true;
        cont.slab_pt = 320.0f;
        const LayoutResult continuous = run("continuous", {long_block}, cont,
                                            {mkFloat(0, 0, 120, 80, ImageWrap::Square, 0, 0)});
        checkDigest(continuous, kContinuousDigest, "连页：摘要未变");
        expectTrue(continuous.stats.page_count == 2 && continuous.lines.back().page == 1,
                   "连页：页号由行槽顶与条带高现算");

        // 排除带本身的算术：挖区间、上下型下推、清掉过去的带、块轴换算。
        {
            FloatFlow flow;
            flow.bands.push_back(FloatBand{10.0f, 130.0f, 10.0f, 90.0f, true});
            flow.bands.push_back(FloatBand{10.0f, 130.0f, 200.0f, 260.0f, false});
            std::vector<LineSpan> spans{{10.0f, 280.0f}};
            cutSpansAroundFloats(flow, 20.0f, 27.0f, kFloatWrapGapPt, &spans);
            expectTrue(spans.size() == 1 && std::fabs(spans[0].x - 136.0f) < 1e-4f &&
                           std::fabs(spans[0].w - 154.0f) < 1e-4f,
                       "四周型挖区间：图左侧那一段被切掉，右侧从图边加间隙起");
            float mid = 10.0f;
            pushLineBelowFloats(flow, 27.0f, &mid);
            expectTrue(std::fabs(mid - 10.0f) < 1e-6f, "上下型下推：行在图上方时不动");
            float low = 200.0f;
            pushLineBelowFloats(flow, 27.0f, &low);
            expectTrue(std::fabs(low - 260.0f) < 1e-6f, "上下型下推：行压在图里时移到图底");
            flow.prune(100.0f);
            expectTrue(flow.bands.size() == 1 && flow.bands[0].f0 == 200.0f,
                       "清掉过去的带：下沿已越过行顶的图不再生效");
            expectNear(floatFlowY(300.0f, false, 2, 10.0f), 610.0f, 1e-4,
                       "块轴换算：页号乘步距加页内 y");
            expectTrue(floatFlowPage(300.0f, 610.0f, 0) == 2 &&
                           std::fabs(floatFlowLocal(300.0f, 2, 610.0f) - 10.0f) < 1e-4f,
                       "块轴换算：页号与页内 y 能往返");
        }
    }

    std::printf("\n[14] 竖排块与文档级浮动图排除：块局部分页接管后逐位不变\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 300.0f;
        page.margins = PageMargins::all(10.0f);

        auto mkText = [&](const std::string& text) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = 12.0f;
            b.line_spacing.factor = 1.6f;
            b.language = "zh";
            b.setText(text);
            return b;
        };
        auto mkVertical = [&](const std::string& text) {
            BlockSpec b = mkText(text);
            b.wm = WritingMode::VerticalRL();
            return b;
        };
        auto mkFloat = [](int para, float w, float h, ImageWrap wrap) {
            LayoutFloat f;
            f.handle = 700 + static_cast<uint64_t>(para);
            f.para = para;
            f.cp = 0;
            f.w_pt = w;
            f.h_pt = h;
            f.wrap = wrap;
            f.content_hash = 0x5678;
            return f;
        };
        auto run = [&](const char* name, const std::vector<BlockSpec>& blocks,
                       const PageMetrics& pg, const std::vector<LayoutFloat>& floats) {
            LayoutResult r = layoutDocument(lib, blocks, pg, {}, floats);
            std::printf("     %s: digest=0x%016llx lines=%zu pages=%d floats=%zu\n", name,
                        static_cast<unsigned long long>(floatCaseDigest(r)), r.lines.size(),
                        r.stats.page_count, r.placed_floats.size());
            return r;
        };
        // 只取一段的行盒与字形做指纹：锚段自己的绕排会变，比较要跳过它。
        auto paraDigest = [](const LayoutResult& r, int para) {
            LayoutResult part;
            for (const LineBox& lb : r.lines) {
                if (lb.paragraph == para) part.lines.push_back(lb);
            }
            for (const PlacedGlyph& g : r.glyphs) {
                if (g.paragraph == para) part.glyphs.push_back(g);
            }
            return floatCaseDigest(part);
        };

        const std::string sentence = "患者诉晨起后心悸明显，持续约十分钟，休息后缓解。";
        std::string long_text;
        std::string short_text;
        for (int i = 0; i < 24; ++i) long_text += sentence;
        for (int i = 0; i < 8; ++i) short_text += sentence;

        const BlockSpec anchor = mkText("甲乙");
        const BlockSpec vertical = mkVertical(long_text);
        const BlockSpec vertical_short = mkVertical(short_text);

        const LayoutResult plain = run("vert-plain", {anchor, vertical}, page, {});
        const LayoutResult square = run("vert-square", {anchor, vertical}, page,
                                        {mkFloat(0, 60.0f, 200.0f, ImageWrap::Square)});
        // 指纹取自切换前的旧竖排分支：排除带只作用于横排，竖排列与页号都不该变。
        expectTrue(floatCaseDigest(square) == 0x8fb006d2716138d1ull,
                   "竖排块 + 四周型浮动图：整篇摘要未变");
        expectTrue(paraDigest(square, 1) == paraDigest(plain, 1),
                   "四周型排除带不改变竖排块的列盒与字形");
        expectTrue(square.stats.page_count == plain.stats.page_count, "有浮动图时竖排块的页数不变");
        expectTrue(plain.stats.page_count >= 2, "前提：这一份竖排块跨页");
        expectTrue(square.placed_floats.size() == 1 && square.placed_floats[0].page == 0,
                   "浮动图照常落在锚段所在页");
        expectNear(square.placed_floats[0].x, page.margins.left, 1e-4, "浮动图左沿 = 内容区左边");
        expectNear(square.placed_floats[0].y, page.margins.top, 1e-4, "浮动图顶 = 锚段首行行顶");

        PageMetrics cont = page;
        cont.continuous = true;
        cont.slab_pt = 320.0f;
        const LayoutResult plain_c = run("vert-cont-plain", {anchor, vertical_short}, cont, {});
        const LayoutResult square_c = run("vert-cont-square", {anchor, vertical_short}, cont,
                                          {mkFloat(0, 60.0f, 200.0f, ImageWrap::Square)});
        expectTrue(floatCaseDigest(square_c) == 0x6ce03c7463c9fb50ull,
                   "连页竖排块 + 四周型浮动图：整篇摘要未变");
        expectTrue(paraDigest(square_c, 1) == paraDigest(plain_c, 1),
                   "连页：排除带不改变竖排块的列盒、字形与条带号");

        BlockSpec breaking = vertical;
        breaking.page_break_before = true;
        const LayoutResult plain_b = run("vert-break-plain", {anchor, breaking}, page, {});
        const LayoutResult square_b = run("vert-break-square", {anchor, breaking}, page,
                                          {mkFloat(0, 60.0f, 200.0f, ImageWrap::Square)});
        expectTrue(floatCaseDigest(square_b) == 0x1042579eb2081375ull,
                   "段前分页的竖排块 + 四周型浮动图：整篇摘要未变");
        expectTrue(paraDigest(square_b, 1) == paraDigest(plain_b, 1),
                   "段前分页：排除带不改变竖排块的列盒与字形");
        {
            int first_page = -1;
            for (const LineBox& lb : square_b.lines) {
                if (lb.paragraph == 1) {
                    first_page = lb.page;
                    break;
                }
            }
            expectTrue(first_page == 1, "竖排块的段前分页在浮动图文档里照样另起一页");
        }

        const BlockSpec empty_vertical = mkVertical("");
        const LayoutResult plain_e = run("empty-vert-plain", {anchor, empty_vertical}, page, {});
        const LayoutResult square_e = run("empty-vert-square", {anchor, empty_vertical}, page,
                                          {mkFloat(0, 60.0f, 200.0f, ImageWrap::Square)});
        expectTrue(paraDigest(square_e, 1) == paraDigest(plain_e, 1),
                   "空竖排段的空列不受排除带影响");
        expectTrue(square_e.stats.page_count == plain_e.stats.page_count,
                   "空竖排段在浮动图文档里页数不变");
        expectTrue(square_e.placed_floats.size() == 1, "空竖排文档里的浮动图照常落下");

        const BlockSpec tail = mkText(long_text);
        const LayoutResult square_t = run("tail-square", {anchor, vertical, tail}, page,
                                          {mkFloat(0, 60.0f, 200.0f, ImageWrap::Square)});
        const LayoutResult behind_t = run("tail-behind", {anchor, vertical, tail}, page,
                                          {mkFloat(0, 60.0f, 200.0f, ImageWrap::Behind)});
        // 到了横排段时那张图早已过去：只剩"浮动图存在"这一条，没有生效的排除带。
        expectTrue(paraDigest(square_t, 2) == paraDigest(behind_t, 2),
                   "竖排块之后的横排段：已过去的排除带不影响它");
        expectTrue(square_t.stats.page_count == behind_t.stats.page_count, "竖排块之后的页数不变");

        {
            BlockSpec owning = vertical;
            InlineObject img;
            img.cp = 0;
            img.kind = InlineObjectKind::Atom;
            img.atom = InlineAtom::Image;
            img.floating = true;
            owning.objects.push_back(img);
            BlockConstraints constraints;
            constraints.width_pt = page.contentWidthPt();
            constraints.writing_mode = owning.wm;
            constraints.column.left_pt = page.margins.left;
            constraints.column.bottom_pt = page.margins.top + page.contentHeightPt();
            constraints.column.first_top_pt = page.margins.top;
            constraints.column.band_top_pt = page.margins.top;
            const BlockLayout r = layoutBlockFragments(owning, constraints, BlockShaping{}, {});
            expectTrue(r.status == BlockLayoutStatus::UnsupportedFloat && r.columns.empty(),
                       "竖排块自身带浮动原子时块局部仍报告不支持（整篇留在旧路径）");
        }
    }

    std::printf("\n[15] 表格格内块：断行与落位由块局部排版给出，几何与整篇逐字段相同\n");
    {
        PageMetrics page;
        page.width_pt = 300.0f;
        page.height_pt = 240.0f;
        page.margins = PageMargins::all(10.0f);
        const float cw = page.contentWidthPt();  // 280
        const float cy = page.margins.top;

        std::vector<std::pair<int, int>> where;
        auto mkCell = [&](const std::string& text, int row, int cell) {
            where.push_back({row, cell});
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = kSize;
            b.line_spacing.factor = 1.6f;
            b.language = "zh";
            b.space_after_pt = 0.0f;
            b.setText(text);
            return b;
        };
        auto mkTable = [&](uint64_t handle, int rows, int cols, float width) {
            LayoutTable t;
            t.handle = handle;
            t.rows = rows;
            t.cols = cols;
            t.col_widths.assign(static_cast<size_t>(cols), WidthSpec{});
            t.row_specs.assign(static_cast<size_t>(rows), LayoutRow{});
            t.width = WidthSpec::makeAbs(width);
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    LayoutCell lc;
                    lc.row = r;
                    lc.cell = c;
                    int first = -1;
                    int count = 0;
                    for (size_t i = 0; i < where.size(); ++i) {
                        if (where[i].first != r || where[i].second != c) continue;
                        if (first < 0) first = static_cast<int>(i);
                        ++count;
                    }
                    if (first >= 0) {
                        lc.first_block = first;
                        lc.block_count = count;
                    }
                    t.cells.push_back(lc);
                }
            }
            where.clear();
            return t;
        };
        // 整篇结果里某个块的行盒，按出现顺序。格内的行盒都在自己的段落号下。
        auto linesOf = [](const LayoutResult& r, int para) {
            std::vector<const LineBox*> out;
            for (const LineBox& lb : r.lines) {
                if (lb.paragraph == para) out.push_back(&lb);
            }
            return out;
        };
        auto cellConstraints = [&](const BlockSpec& b, float width) {
            BlockConstraints c;
            c.width_pt = width;
            c.writing_mode = b.wm;
            return c;
        };

        const std::string cell_text =
            "格内文本按格宽断行，宽度收窄之后行数必须变多，行高由表格在块排完之后结算。";
        // 两列：左格长文本，右格一个字。格内容区宽度是列宽减两侧内边距。
        {
            constexpr uint64_t kT = 21;
            std::vector<BlockSpec> blocks = {mkCell(cell_text, 0, 0), mkCell("乙", 0, 1)};
            LayoutTable t = mkTable(kT, 1, 2, cw);
            const LayoutResult full = layoutDocument(lib, blocks, page, {t});
            ShapingCache cache;
            const float cell_w = cw * 0.5f - 2.0f * kTableCellPadXPt;
            const BlockLayout local =
                layoutBlock(lib, blocks[0], cellConstraints(blocks[0], cell_w), &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(local.status == BlockLayoutStatus::Ok && local.lines.size() >= 3,
                       "格内长文本在格宽下折成多行（块局部报告支持并给出片段）");
            expectTrue(got.size() == local.lines.size(), "整篇的格内行数等于块局部断行片段数");

            int metric_diff = 0;
            int top_diff = 0;
            float want_top = cy + kTableCellPadYPt;
            for (size_t i = 0; i < got.size() && i < local.lines.size(); ++i) {
                const LineBox& lb = *got[i];
                const LineBox& bl = local.lines[i];
                const bool same = std::fabs(lb.advance_pt - bl.advance_pt) < 1e-4f &&
                                  std::fabs(lb.lead_top_pt - bl.lead_top_pt) < 1e-4f &&
                                  std::fabs(lb.ascent_pt - bl.ascent_pt) < 1e-4f &&
                                  std::fabs(lb.descent_pt - bl.descent_pt) < 1e-4f;
                if (!same) ++metric_diff;
                if (std::fabs(lb.slotLoPt() - want_top) > 1e-3f) ++top_diff;
                want_top += bl.advance_pt;
            }
            expectTrue(metric_diff == 0, "格内每一行的行度量与块局部逐字段相同");
            expectTrue(top_diff == 0, "格内行槽顶 = 格内容区上沿 + 逐行累加行高");

            float content_h = 0.0f;
            for (const BrokenLine& f : local.fragments) content_h += f.line_h;
            if (!full.placed_tables.empty() && !full.placed_tables[0].placed.empty()) {
                expectNear(full.placed_tables[0].placed[0].h, content_h + 2.0f * kTableCellPadYPt,
                           1e-3, "行高 = 格内内容高 + 上下内边距");
            }
            // 反向一半：同一批块按正文宽度排，行数必须更少 —— 断行输入是格宽。
            const LayoutResult body = layoutDocument(lib, blocks, page);
            expectTrue(body.lines.size() < got.size(),
                       "同一块按正文宽度排出的行更少（格宽真的决定了断行）");
        }
        // 格内两段：段后距与下一段的段前距相加，最后一段的段后距不进行高。
        {
            constexpr uint64_t kT = 22;
            BlockSpec first = mkCell("甲乙丙", 0, 0);
            first.space_after_pt = 6.0f;
            BlockSpec second = mkCell("丁戊", 0, 0);
            second.space_before_pt = 9.0f;
            std::vector<BlockSpec> blocks = {first, second};
            LayoutTable t = mkTable(kT, 1, 1, cw);
            const LayoutResult full = layoutDocument(lib, blocks, page, {t});
            const std::vector<const LineBox*> a = linesOf(full, 0);
            const std::vector<const LineBox*> b = linesOf(full, 1);
            expectTrue(a.size() == 1 && b.size() == 1, "格内两段各排一行");
            if (a.size() == 1 && b.size() == 1) {
                const float first_top = cy + kTableCellPadYPt;
                expectNear(a[0]->slotLoPt(), first_top, 1e-3, "格内首段从格内容区上沿起排");
                expectNear(b[0]->slotLoPt(), first_top + a[0]->advance_pt + 6.0f + 9.0f, 1e-3,
                           "格内段后距与下一段的段前距相加，不折叠");
                if (!full.placed_tables.empty() && !full.placed_tables[0].placed.empty()) {
                    expectNear(
                        full.placed_tables[0].placed[0].h,
                        a[0]->advance_pt + 6.0f + 9.0f + b[0]->advance_pt + 2.0f * kTableCellPadYPt,
                        1e-3, "行高含两段行高与段间距，不含最后一段的段后距");
                }
            }
        }
        // 一行高过整页：行盒按自己的纵向位置切片，切片之后行数与断点都不变。
        {
            constexpr uint64_t kT = 23;
            PageMetrics tiny;
            tiny.width_pt = 200.0f;
            tiny.height_pt = 100.0f;  // 内容区高 80
            tiny.margins = PageMargins::all(10.0f);
            std::string long_text;
            for (int i = 0; i < 12; ++i) long_text += "一二三四五六七八九十";
            std::vector<BlockSpec> blocks = {mkCell(long_text, 0, 0)};
            LayoutTable t = mkTable(kT, 1, 1, tiny.contentWidthPt());
            const LayoutResult full = layoutDocument(lib, blocks, tiny, {t});
            ShapingCache cache;
            const float cell_w = tiny.contentWidthPt() - 2.0f * kTableCellPadXPt;
            const BlockLayout local =
                layoutBlock(lib, blocks[0], cellConstraints(blocks[0], cell_w), &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(local.status == BlockLayoutStatus::Ok && got.size() == local.lines.size(),
                       "跨页切片之后行数仍等于块局部断行片段数");
            expectTrue(full.placed_tables.size() == 1 && full.placed_tables[0].placed.size() >= 3,
                       "这一行确实被切成跨页的多个片段");

            int page_starts = 0;
            int pages_seen = 0;
            int stack_diff = 0;
            int prev_page = -1;
            bool first_line = true;
            const float content_top = tiny.margins.top + kTableCellPadYPt;
            float prev_slot = 0.0f;
            for (const LineBox* lb : got) {
                if (lb->page != prev_page) {
                    ++pages_seen;
                    // 续排片段的内容区上沿是页面内容区加一层内边距；首片从行自己的行槽顶起。
                    const float want_lo =
                        first_line ? (content_top + lb->lead_top_pt) : content_top;
                    if (std::fabs(lb->block_lo_pt - want_lo) > 1e-3f) ++page_starts;
                    prev_page = lb->page;
                } else if (std::fabs(lb->slotLoPt() - prev_slot) > 1e-3f) {
                    ++stack_diff;
                }
                prev_slot = lb->slotLoPt() + lb->advance_pt;
                first_line = false;
            }
            expectTrue(pages_seen >= 3 && page_starts == 0,
                       "每一片的第一行都从续排片段的内容区上沿起");
            expectTrue(stack_diff == 0, "同一片内的行按行高首尾相接，切片不重排");
        }
        // 连页：格内行的页号是绝对行顶所在的条带，页内坐标按条带折回。
        {
            constexpr uint64_t kT = 24;
            PageMetrics cont_page;
            cont_page.width_pt = 200.0f;
            cont_page.height_pt = 120.0f;
            cont_page.margins = PageMargins::all(10.0f);
            cont_page.continuous = true;
            cont_page.slab_pt = 60.0f;
            const float slab = cont_page.slabHeightPt();
            std::string tall;
            for (int i = 0; i < 6; ++i) tall += "一二三四五六七八九十";
            std::vector<BlockSpec> blocks = {mkCell(tall, 0, 0)};
            LayoutTable t = mkTable(kT, 1, 1, cont_page.contentWidthPt());
            const LayoutResult full = layoutDocument(lib, blocks, cont_page, {t});
            ShapingCache cache;
            const float cell_w = cont_page.contentWidthPt() - 2.0f * kTableCellPadXPt;
            const BlockLayout local =
                layoutBlock(lib, blocks[0], cellConstraints(blocks[0], cell_w), &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(local.status == BlockLayoutStatus::Ok && got.size() == local.lines.size(),
                       "连页下的格内行数等于块局部断行片段数");
            float abs_top = cont_page.margins.top + kTableCellPadYPt;
            int fold_diff = 0;
            int side_diff = 0;
            for (const LineBox* lb : got) {
                const int want_page = static_cast<int>(std::floor(abs_top / slab));
                if (lb->page != want_page) ++fold_diff;
                const float want_y =
                    abs_top + lb->lead_top_pt - static_cast<float>(want_page) * slab;
                if (std::fabs(lb->block_lo_pt - want_y) > 1e-3f) ++side_diff;
                abs_top += lb->advance_pt;
            }
            expectTrue(fold_diff == 0, "连页下格内行的页号由绝对行顶与条带高现算");
            expectTrue(side_diff == 0, "连页下格内行按条带折回页内坐标");
        }
        // 空格：空行的字体度量、括号占位与原子盒高由块局部给出，页内锚点仍按旧路径的
        //   加法次序重算；行高、内边距与行切片都读同一份几何。
        {
            constexpr uint64_t kT = 25;
            BlockSpec empty = mkCell("", 0, 0);
            BlockSpec neighbor = mkCell("乙", 0, 1);
            LayoutTable t = mkTable(kT, 1, 2, cw);
            const LayoutResult full = layoutDocument(lib, {empty, neighbor}, page, {t});
            ShapingCache cache;
            const float col_w = cw * 0.5f;
            const float cell_w = col_w - 2.0f * kTableCellPadXPt;
            const BlockLayout local =
                layoutBlock(lib, empty, cellConstraints(empty, cell_w), &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(got.size() == 1 && local.lines.size() == 1 && local.glyphs.empty(),
                       "空格：整篇与块局部各排出一行，没有字形");
            if (got.size() == 1 && local.lines.size() == 1) {
                const LineBox& a = *got[0];
                const LineBox& b = local.lines[0];
                const bool same = std::fabs(a.advance_pt - b.advance_pt) < 1e-4f &&
                                  std::fabs(a.lead_top_pt - b.lead_top_pt) < 1e-4f &&
                                  std::fabs(a.ascent_pt - b.ascent_pt) < 1e-4f &&
                                  std::fabs(a.descent_pt - b.descent_pt) < 1e-4f &&
                                  a.clip_text == b.clip_text &&
                                  a.clusters.size() == b.clusters.size();
                expectTrue(same, "空格的行度量与块局部逐字段相同");
                expectNear(a.slotLoPt(), cy + kTableCellPadYPt, 1e-3,
                           "空格的行槽顶就是格内容区上沿");
                expectNear(a.block_hi_pt - a.block_lo_pt, a.ascent_pt + a.descent_pt, 1e-3,
                           "空格的行盒高仍是墨迹高");
                expectNear(a.cell_lo_pt, page.margins.left + kTableCellPadXPt, 1e-3,
                           "空格的行盒带格内容区左沿");
                expectNear(a.cell_hi_pt, page.margins.left + col_w - kTableCellPadXPt, 1e-3,
                           "空格的行盒带格内容区右沿");
                expectNear(a.cell_box_hi_pt, page.margins.left + col_w, 1e-3,
                           "空格的外框右沿是格线");
                if (!full.placed_tables.empty() && !full.placed_tables[0].placed.empty()) {
                    expectNear(full.placed_tables[0].placed[0].h,
                               a.advance_pt + 2.0f * kTableCellPadYPt, 1e-3,
                               "空格的行高仍是内容高加上下内边距");
                }
            }
        }
        // 空格的占位与原子：括号、提示文字、原子盒高都要进空行，且块局部与整篇同一份。
        {
            constexpr uint64_t kT = 26;
            BlockSpec decorated = mkCell("", 0, 0);
            InlineObject start;
            start.kind = InlineObjectKind::ControlStart;
            start.owner = 7;
            start.hint = "提示";
            decorated.objects.push_back(start);
            InlineObject stop;
            stop.kind = InlineObjectKind::ControlEnd;
            stop.owner = 7;
            decorated.objects.push_back(stop);
            InlineObject atom;
            atom.kind = InlineObjectKind::Atom;
            atom.atom = InlineAtom::Image;
            atom.w_pt = 24.0f;
            atom.h_pt = 40.0f;
            atom.content_hash = 99;
            decorated.objects.push_back(atom);
            LayoutTable t = mkTable(kT, 1, 1, cw);
            const LayoutResult full = layoutDocument(lib, {decorated}, page, {t});
            ShapingCache cache;
            const BlockLayout local = layoutBlock(
                lib, decorated, cellConstraints(decorated, cw - 2.0f * kTableCellPadXPt), &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(got.size() == 1 && local.lines.size() == 1 && got[0]->clusters.size() == 4 &&
                           local.lines[0].clusters.size() == 4,
                       "空格的括号、提示文字与原子都进簇（块局部与整篇同数）");
            if (got.size() == 1 && local.lines.size() == 1) {
                int hints = 0;
                int atoms = 0;
                float atom_h = 0.0f;
                for (const LineCluster& c : got[0]->clusters) {
                    if (c.control_hint) ++hints;
                    if (c.atom != InlineAtom::None) {
                        ++atoms;
                        atom_h = c.atom_h_pt;
                    }
                }
                expectTrue(hints == 1 && atoms == 1, "空行里有一枚提示文字与一枚原子");
                expectNear(atom_h, 40.0f, 1e-4, "原子盒高进了簇");
                expectNear(got[0]->ascent_pt, 40.0f, 1e-4, "原子盒高撑起空行的 ascent");
                expectNear(got[0]->advance_pt, local.lines[0].advance_pt, 1e-4,
                           "带原子的空行行高与块局部一致");
                expectNear(got[0]->block_hi_pt - got[0]->block_lo_pt,
                           local.lines[0].block_hi_pt - local.lines[0].block_lo_pt, 1e-4,
                           "带原子的空行墨迹高与块局部一致");
                if (!full.placed_tables.empty() && !full.placed_tables[0].placed.empty()) {
                    expectNear(full.placed_tables[0].placed[0].h,
                               got[0]->advance_pt + 2.0f * kTableCellPadYPt, 1e-3,
                               "原子撑起的空行照样按内容高结算行高");
                }
            }
        }
        // 竖排格内块：列长与列位取版心，块局部按页的竖带断列；行高仍由表格结算。
        {
            constexpr uint64_t kT = 27;
            BlockSpec vcell = mkCell("", 0, 0);
            vcell.wm = WritingMode::VerticalRL();
            std::string vtext;
            for (int i = 0; i < 8; ++i) vtext += "患者诉晨起后心悸持续约十分钟。";
            vcell.setText(vtext);
            LayoutTable t = mkTable(kT, 1, 1, cw);
            // 竖排格吃掉本页剩余高度，行高恒超出；不许切片时整行留在原页，便于逐列对拍。
            t.row_specs[0].can_split_by_page = false;
            const LayoutResult full = layoutDocument(lib, {vcell}, page, {t});
            ShapingCache cache;
            BlockConstraints vc;
            vc.width_pt = cw;
            vc.writing_mode = vcell.wm;
            vc.column.left_pt = page.margins.left;
            vc.column.bottom_pt = page.margins.top + page.contentHeightPt();
            vc.column.first_top_pt = page.margins.top + kTableCellPadYPt;
            vc.column.band_top_pt = page.margins.top;
            const BlockLayout local = layoutBlock(lib, vcell, vc, &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(local.status == BlockLayoutStatus::Ok && got.size() >= 2 &&
                           got.size() == local.lines.size() && got.size() == local.columns.size(),
                       "竖排格内块：整篇的列数等于块局部断出的列数");
            int col_diff = 0;
            int page_diff = 0;
            for (size_t i = 0; i < got.size() && i < local.columns.size(); ++i) {
                const LineBox& lb = *got[i];
                const BlockColumn& col = local.columns[i];
                const float w = lb.block_hi_pt - lb.block_lo_pt;
                const float center = 0.5f * (lb.block_lo_pt + lb.block_hi_pt);
                if (std::fabs(w - col.width_pt) > 1e-3f ||
                    std::fabs(center - col.center_pt) > 1e-3f ||
                    std::fabs(lb.inline_origin_pt - col.top_pt) > 1e-3f) {
                    ++col_diff;
                }
                if (lb.page != 0) ++page_diff;
            }
            expectTrue(col_diff == 0, "竖排格内块的列宽、列心与列顶与块局部逐列相同");
            expectTrue(page_diff == 0, "竖排格内块的首条带落在表格当前页");
            expectNear(local.columns.front().top_pt, page.margins.top + kTableCellPadYPt, 1e-3,
                       "竖排格内块的首列顶是格内容区上沿");
            if (!full.placed_tables.empty() && !full.placed_tables[0].placed.empty()) {
                expectNear(full.placed_tables[0].placed[0].h,
                           page.contentHeightPt() + kTableCellPadYPt, 1e-3,
                           "竖排格吃掉本页剩余高度，行高是版心高加上内边距");
            }
        }
        // 空竖排格：一条空列，列宽是空行行高，列心按书写模式取边。
        {
            constexpr uint64_t kT = 28;
            BlockSpec vempty = mkCell("", 0, 0);
            vempty.wm = WritingMode::VerticalRL();
            LayoutTable t = mkTable(kT, 1, 1, cw);
            t.row_specs[0].can_split_by_page = false;
            const LayoutResult full = layoutDocument(lib, {vempty}, page, {t});
            ShapingCache cache;
            BlockConstraints vc;
            vc.width_pt = cw;
            vc.writing_mode = vempty.wm;
            vc.column.left_pt = page.margins.left;
            vc.column.bottom_pt = page.margins.top + page.contentHeightPt();
            vc.column.first_top_pt = page.margins.top + kTableCellPadYPt;
            vc.column.band_top_pt = page.margins.top;
            const BlockLayout local = layoutBlock(lib, vempty, vc, &cache);
            const std::vector<const LineBox*> got = linesOf(full, 0);
            expectTrue(local.status == BlockLayoutStatus::Ok && local.columns.size() == 1 &&
                           local.lines.size() == 1 && got.size() == 1 && got[0]->clusters.empty(),
                       "空竖排格：块局部与整篇各排出一条空列，没有字形");
            if (got.size() == 1 && local.columns.size() == 1) {
                const LineBox& lb = *got[0];
                const float w = lb.block_hi_pt - lb.block_lo_pt;
                expectNear(w, local.columns[0].width_pt, 1e-3, "空列的列宽就是空行行高");
                expectNear(0.5f * (lb.block_lo_pt + lb.block_hi_pt), local.columns[0].center_pt,
                           1e-3, "空列的列心与块局部相同（中日竖排贴右边缘）");
                expectNear(lb.inline_origin_pt, page.margins.top + kTableCellPadYPt, 1e-3,
                           "空列的列顶是格内容区上沿");
                expectNear(lb.cell_lo_pt, page.margins.left + kTableCellPadXPt, 1e-3,
                           "空格列带格内容区左沿");
            }
        }
        // 带浮动图的格内块：排除带由 FloatFlow 提供，断行逐行取宽；格内不翻页。
        {
            constexpr uint64_t kT = 29;
            BlockSpec lead;
            lead.font_index = cjk;
            lead.size_pt = kSize;
            lead.line_spacing.factor = 1.6f;
            lead.language = "zh";
            lead.space_after_pt = 0.0f;
            lead.setText("图前");
            std::string cell_text;
            for (int i = 0; i < 8; ++i) cell_text += "格内绕排文字要避开浮动图占掉的区间。";
            std::vector<BlockSpec> blocks = {lead, mkCell(cell_text, 0, 0)};
            // 表要在块之后建：格的块区间是归属的唯一记录，而 where 是随 mkCell 记下来的。
            //   图前那一段在正文里，所以格从第 1 块起 —— 正文块不占格。
            LayoutTable t = mkTable(kT, 1, 1, cw);
            t.cells[0].first_block = 1;
            t.cells[0].block_count = 1;
            LayoutFloat fl;
            fl.handle = 1;
            fl.para = 0;
            fl.w_pt = 80.0f;
            fl.h_pt = 80.0f;
            fl.wrap = ImageWrap::Square;
            fl.dx_pt = 90.0f;
            const LayoutResult with = layoutDocument(lib, blocks, page, {t}, {fl});
            const LayoutResult plain = layoutDocument(lib, blocks, page, {t});
            const std::vector<const LineBox*> got = linesOf(with, 1);
            const std::vector<const LineBox*> plain_got = linesOf(plain, 1);
            expectTrue(with.placed_floats.size() == 1 && with.placed_floats[0].page == 0,
                       "格内块文档里的浮动图照常落在第 0 页");
            expectTrue(got.size() > plain_got.size(), "绕排把格内的行切窄，行数比没有浮动图时多");
            const float content_x = page.margins.left + kTableCellPadXPt;
            const float cut_lo = with.placed_floats[0].x - kFloatWrapGapPt;
            const float cut_hi =
                with.placed_floats[0].x + with.placed_floats[0].w + kFloatWrapGapPt;
            int split_lines = 0;
            int gap_clusters = 0;
            const PlacedFloat& pf = with.placed_floats[0];
            for (const LineBox* lb : got) {
                // 只有与图纵向相交的行才受排除带约束；图下面恢复整宽的行照常排进那一带。
                const float line_top = lb->slotLoPt();
                const float line_bottom = line_top + lb->advance_pt;
                const bool overlaps =
                    lb->page == pf.page && line_top < pf.y + pf.h && line_bottom > pf.y;
                if (!overlaps) continue;
                if (lb->spans.size() == 2) {
                    ++split_lines;
                    // 左段可能提前断行，只要求不越过排除带左沿；右段必须正好从右沿起排。
                    const float left_end = lb->inline_origin_pt + lb->spans[0].hi_pt;
                    const float right_start = lb->inline_origin_pt + lb->spans[1].lo_pt;
                    if (left_end > cut_lo + 1e-3f || std::fabs(right_start - cut_hi) > 1e-3f) {
                        ++gap_clusters;
                    }
                }
                for (const LineCluster& c : lb->clusters) {
                    const float x = lb->inline_origin_pt + c.inline_pt;
                    if (x > cut_lo + 0.01f && x < cut_hi - 0.01f) ++gap_clusters;
                }
            }
            expectTrue(split_lines >= 1, "与浮动图纵向相交的格内行分成左右两段");
            expectTrue(gap_clusters == 0, "格内行没有把簇排进浮动图让开的区间");
            expectNear(got.front()->inline_origin_pt, content_x, 1e-3,
                       "格内绕排首段的起点仍是格内容区左沿");
        }
        // 上下型浮动图：格内行顶被推到图底，页号不变（格内不翻页）。
        {
            constexpr uint64_t kT = 30;
            BlockSpec lead;
            lead.font_index = cjk;
            lead.size_pt = kSize;
            lead.line_spacing.factor = 1.6f;
            lead.language = "zh";
            lead.space_after_pt = 0.0f;
            lead.setText("图前");
            std::string cell_text;
            for (int i = 0; i < 4; ++i) cell_text += "格内文字不得压在上方浮动图上。";
            std::vector<BlockSpec> blocks = {lead, mkCell(cell_text, 0, 0)};
            // 表要在块之后建：格的块区间是归属的唯一记录，而 where 是随 mkCell 记下来的。
            //   图前那一段在正文里，所以格从第 1 块起 —— 正文块不占格。
            LayoutTable t = mkTable(kT, 1, 1, cw);
            t.cells[0].first_block = 1;
            t.cells[0].block_count = 1;
            LayoutFloat fl;
            fl.handle = 2;
            fl.para = 0;
            fl.w_pt = 60.0f;
            fl.h_pt = 40.0f;
            fl.wrap = ImageWrap::None;
            fl.dy_pt = 30.0f;
            const LayoutResult full = layoutDocument(lib, blocks, page, {t}, {fl});
            const std::vector<const LineBox*> got = linesOf(full, 1);
            expectTrue(full.placed_floats.size() == 1 && !got.empty(),
                       "上下型浮动图照常落下，格内有行");
            if (!got.empty() && full.placed_floats.size() == 1) {
                const float float_bottom = full.placed_floats[0].y + full.placed_floats[0].h;
                expectTrue(got.front()->slotLoPt() >= float_bottom - 1e-3f,
                           "格内首行被上下型浮动图推到图底");
                bool same_page = true;
                for (const LineBox* lb : got) same_page = same_page && lb->page == 0;
                expectTrue(same_page, "格内绕排不翻页：所有行都在表格当前页");
            }
        }
    }

    std::printf("\n[16] 后缀重新分页：FragmentIndex 的块行切片与续排逐字段相同\n");
    {
        PageMetrics suffix_page;
        suffix_page.width_pt = 300.0f;
        suffix_page.height_pt = 190.0f;
        suffix_page.margins = PageMargins::all(20.0f);
        std::vector<BlockSpec> blocks;
        for (int i = 0; i < 9; ++i) {
            BlockSpec b;
            b.font_index = cjk;
            b.size_pt = 13.0f;
            b.line_spacing.factor = 1.5f;
            b.language = "zh";
            b.space_before_pt = (i == 4) ? 9.0f : 2.0f;
            b.space_after_pt = 5.0f;
            b.page_break_before = (i == 6);
            if (i == 4) {
                // 空段也占一行：后缀分页不能把它当成"没有内容"跳过。
                b.setText("");
            } else {
                b.setText("第 " + std::to_string(i) +
                          " 段：后缀重新分页要与从这一段续排逐字段相同，段前距与段后距都算。");
            }
            blocks.push_back(std::move(b));
        }
        const int n = static_cast<int>(blocks.size());
        const LayoutResult full = layoutDocument(lib, blocks, suffix_page);
        const int from = 3;
        const int line_base = full.para_index[static_cast<size_t>(from)].line_begin;
        const int glyph_base = full.para_index[static_cast<size_t>(from)].glyph_begin;
        ReflowState anchor = full.para_index[static_cast<size_t>(from)].state_at_start;
        anchor.cursor_y += 17.0f;

        LayoutResult by_fragment = full;
        ReflowState fragment_end;
        expectTrue(
            repaginateSuffix(&by_fragment, blocks, suffix_page, from, n, anchor, &fragment_end),
            "repaginateSuffix 接受纯横排后缀");
        expectTrue(by_fragment.lines.size() == full.lines.size() &&
                       by_fragment.glyphs.size() == full.glyphs.size(),
                   "重新分页不改行数与字形数");

        LayoutResult by_resume = full;
        by_resume.lines.resize(static_cast<size_t>(line_base));
        by_resume.glyphs.resize(static_cast<size_t>(glyph_base));
        by_resume.para_index.resize(static_cast<size_t>(from));
        by_resume.para_cluster_starts.resize(static_cast<size_t>(from));
        const ReflowState resume_end = layoutDocumentRangeWithTables(
            lib, blocks, suffix_page, {}, from, n, anchor, nullptr, &by_resume);
        expectTrue(fragment_end == resume_end, "后缀结束锚点与续排相同");

        auto samePos = [](float a, float b) { return std::fabs(a - b) <= 1e-3f; };
        int line_bad = 0;
        const size_t line_count = std::min(by_fragment.lines.size(), by_resume.lines.size());
        for (size_t i = 0; i < line_count; ++i) {
            const LineBox& a = by_fragment.lines[i];
            const LineBox& b = by_resume.lines[i];
            if (a.paragraph != b.paragraph || a.line != b.line || a.page != b.page ||
                a.vertical != b.vertical || a.sideways != b.sideways || a.first_cp != b.first_cp ||
                a.last_cp != b.last_cp || a.clusters.size() != b.clusters.size() ||
                !samePos(a.block_lo_pt, b.block_lo_pt) || !samePos(a.block_hi_pt, b.block_hi_pt) ||
                !samePos(a.bracket_axis_pt, b.bracket_axis_pt) ||
                !samePos(a.advance_pt, b.advance_pt) || !samePos(a.lead_top_pt, b.lead_top_pt) ||
                !samePos(a.ascent_pt, b.ascent_pt) || !samePos(a.descent_pt, b.descent_pt)) {
                ++line_bad;
            }
        }
        expectTrue(line_bad == 0, "后缀每一行的页号、行号与纵向几何都与续排相同");
        int glyph_bad = 0;
        const size_t glyph_count = std::min(by_fragment.glyphs.size(), by_resume.glyphs.size());
        for (size_t i = 0; i < glyph_count; ++i) {
            const PlacedGlyph& a = by_fragment.glyphs[i];
            const PlacedGlyph& b = by_resume.glyphs[i];
            if (a.glyph_id != b.glyph_id || a.font != b.font || a.page != b.page ||
                a.line != b.line || a.paragraph != b.paragraph || a.cluster != b.cluster ||
                !samePos(a.x_pt, b.x_pt) || !samePos(a.y_pt, b.y_pt)) {
                ++glyph_bad;
            }
        }
        expectTrue(glyph_bad == 0, "后缀每个字形的页号、行号与坐标都与续排相同");
        int para_bad = 0;
        for (int p = from; p < n; ++p) {
            const ParaIndex& a = by_fragment.para_index[static_cast<size_t>(p)];
            const ParaIndex& b = by_resume.para_index[static_cast<size_t>(p)];
            if (a.line_begin != b.line_begin || a.line_end != b.line_end ||
                a.glyph_begin != b.glyph_begin || a.glyph_end != b.glyph_end ||
                a.state_at_start != b.state_at_start) {
                ++para_bad;
            }
        }
        expectTrue(para_bad == 0, "后缀每段的切片与起始锚点都与续排相同");
        expectTrue(by_fragment.stats.page_count == by_resume.stats.page_count,
                   "后缀重新分页的页数与续排相同");
        by_fragment.rebuildPageIndex();
        expectTrue(by_fragment.fragmentIndexValid(),
                   "重新分页之后 FragmentIndex 的页/块/行三表自洽");

        // 竖排后缀明确拒绝：调用方退回续排，且拒绝时一个字段都不许改。
        BlockSpec vertical;
        vertical.font_index = cjk;
        vertical.size_pt = 13.0f;
        vertical.wm = WritingMode::VerticalRL();
        vertical.language = "zh";
        vertical.setText("竖排后缀不进片段重分页。");
        std::vector<BlockSpec> with_vertical = blocks;
        with_vertical.push_back(std::move(vertical));
        const LayoutResult vertical_full = layoutDocument(lib, with_vertical, suffix_page);
        LayoutResult vertical_copy = vertical_full;
        ReflowState vertical_end;
        expectTrue(
            !repaginateSuffix(&vertical_copy, with_vertical, suffix_page, from,
                              static_cast<int>(with_vertical.size()),
                              vertical_full.para_index[static_cast<size_t>(from)].state_at_start,
                              &vertical_end),
            "竖排后缀被明确拒绝");
        expectTrue(vertical_copy.lines.size() == vertical_full.lines.size() &&
                       vertical_copy.glyphs.size() == vertical_full.glyphs.size() &&
                       vertical_copy.lines.back().page == vertical_full.lines.back().page &&
                       vertical_copy.lines.front().block_lo_pt ==
                           vertical_full.lines.front().block_lo_pt &&
                       vertical_copy.para_index[static_cast<size_t>(from)].state_at_start ==
                           vertical_full.para_index[static_cast<size_t>(from)].state_at_start,
                   "拒绝竖排后缀时行盒、字形与段状态都未改动");
    }

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
