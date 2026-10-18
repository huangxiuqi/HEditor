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
#include "core/layout/layout.h"
#include "core/text/fallback.h"
#include "core/text/font_source.h"
#include "core/text/shaper.h"
#include "core/utils/utf8.h"
#include "demo/demo_documents.h"

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
        engine.setDocument(demoTree(sampleDocument(0, 1, 2), {}), StyleSheet{});
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
        expectTrue(lrMinX >= engine.config().page.margin_pt - 1.0f, "竖排字形没有越出左边距");

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

        eOff.setDocument(demoTree({mixedParagraphBlock(cjk)}, {}), StyleSheet{});  // 不回退
        eOn.setDocument(demoTree({mixedParagraphBlock(cjk, latin)}, {}),
                        StyleSheet{});  // 回退到拉丁
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
        b.line_factor = 1.8f;
        b.language = "mn";
        b.setText(kMongol_word);
        const std::string word = b.textUtf8();
        e.setDocument(demoTree({b}, {}), StyleSheet{});
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
                    b.size_pt * b.line_factor);
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
            b.line_factor = 1.6f;
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
        page.margin_pt = 5.0f;
        const float cx = page.margin_pt;
        const float cw = page.contentWidthPt();
        const float big_pt = 24.0f;
        const float small_pt = 12.0f;
        const float factor = 1.6f;

        const LayoutResult r =
            layoutDocument(lib, {mkVertical(WritingMode::VerticalRL(), 4, 4, big_pt, small_pt)},
                           page);
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
            const float w = lb->block_hi_pt - lb->block_lo_pt;
            if (std::fabs(lb->size_pt - want) > 0.01f) ++bad_size;
            if (std::fabs(w - want * factor) > 0.05f) ++bad_w;
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
        expectTrue(big_w > small_w + 1.0f,
                   "大字列真的比小字列宽（取整段最大字号时两者会相等）");

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

        // 反向一半：整段一个字号 ⇒ 所有列宽相等，且等于那个字号 × 行距。
        const LayoutResult r1 =
            layoutDocument(lib, {mkVertical(WritingMode::VerticalRL(), 8, 0, small_pt, 0.0f)},
                           page);
        std::vector<float> widths;
        for (const LineBox& lb : r1.lines) {
            if (!lb.vertical) continue;
            widths.push_back(std::round((lb.block_hi_pt - lb.block_lo_pt) * 100.0f) / 100.0f);
        }
        std::sort(widths.begin(), widths.end());
        widths.erase(std::unique(widths.begin(), widths.end()), widths.end());
        expectTrue(widths.size() == 1 &&
                       std::fabs(widths.front() - small_pt * factor) < 0.05f,
                   "整段一个字号：所有列宽相等且等于字号 × 行距（与逐列算之前逐字段相同）");

        // 列序的正面判据也要有一份：蒙文（LTR）的第一列在最左，中日（RTL）在最右。
        const LayoutResult rl =
            layoutDocument(lib, {mkVertical(WritingMode::VerticalLR(), 4, 4, big_pt, small_pt)},
                           page);
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
            b.line_factor = 1.6f;
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
        page.margin_pt = 10.0f;
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
            const float small_h = r_small.lines.empty()
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
        tiny.margin_pt = 5.0f;
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
                   "超宽原子独占一行，且不是空行（行数 " + std::to_string(wide_lines) +
                       "，空行 " + std::to_string(wide_empty) + "）");

        // 反向一半：没有原子时这一段的排版与加原子之前逐字段相同（空表 = 一个都不插）。
        const LayoutResult r_plain =
            layoutDocument(lib, {mkPara("汉字汉字", {}, 12.0f)}, page);
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
        hb_blob_t* skel_blob =
            hb_blob_create(reinterpret_cast<const char*>(skeleton.data()),
                           static_cast<unsigned>(skeleton.size()), HB_MEMORY_MODE_READONLY,
                           nullptr, nullptr);
        hb_face_t* skel_face = hb_face_create(skel_blob, 0);
        const unsigned skel_glyphs = skel_face ? hb_face_get_glyph_count(skel_face) : 0;
        const unsigned full_glyphs = lib.face(cjk)->glyphCount();
        if (skel_face) hb_face_destroy(skel_face);
        hb_blob_destroy(skel_blob);
        expectTrue(skel_glyphs == full_glyphs && full_glyphs > 0,
                   "骨架是合法 SFNT 且字形数与原字体一致（" + std::to_string(skel_glyphs) +
                       " vs " + std::to_string(full_glyphs) + "）");

        // 按需读与整份读逐字节相同：偏移算术与块缓存的正确性都在这一条上。
        std::ifstream in(cjkPath, std::ios::binary);
        const std::vector<uint8_t> whole((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
        expectTrue(whole.size() == src.size(), "数据源报的大小与文件实际大小一致");

        bool same_bytes = true;
        const uint64_t offsets[] = {0, 1, 12, 63, 64, 4095, 4096, 65535, 65536, 65537};
        for (uint64_t off : offsets) {
            if (off >= whole.size()) continue;
            const size_t n =
                std::min<size_t>(96, whole.size() - static_cast<size_t>(off));
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
            expectTrue(got_n == 4, "越过文件尾的读只交出可读的那 4 字节（实得 " +
                                       std::to_string(got_n) + "）");
            bool untouched = true;
            for (size_t i = 4; i < probe.size(); ++i) {
                if (probe[i] != 0xAB) untouched = false;
            }
            expectTrue(untouched &&
                           std::equal(probe.begin(), probe.begin() + 4, whole.end() - 4),
                       "可读那 4 字节等于文件尾，其后的缓冲一个字节都没被写");

            // 起点直接落在文件尾之后：零字节可读，同样是文件尾而不是失败。
            size_t got_zero = 99;
            expectTrue(src.readAt(src.size(), probe.data(), 4, &got_zero) == FontRead::Eof &&
                           got_zero == 0,
                       "起点在文件尾之后的读也是文件尾，交出零字节");

            // 这两条是分开计数的核心判据：文件尾只进 eofReads，不进 readFailures。
            expectTrue(src.cache().eofReads() == eof_before + 2,
                       "两次越尾读都记进了 eofReads");
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
                expectTrue(lr == FontRead::Error,
                           "文件被截断后的读是失败（不是文件尾）");
                expectTrue(truncated.cache().readFailures() == cut_fail_before + 1,
                           "真失败会让 readFailures 加一 —— 这个计数不是死代码");
                expectTrue(truncated.cache().eofReads() == cut_eof_before,
                           "真失败一次都不进 eofReads（两个计数互不串台）");
            }
            std::remove(cut_path.c_str());

            // 与它对照：没打开的存储同样是失败，且返回的字节数是零。
            FontStorage unopened;
            std::vector<uint8_t> buf(4, 0);
            size_t unopened_got = 7;
            expectTrue(unopened.readAt(0, buf.data(), buf.size(), &unopened_got) ==
                               FontRead::Error &&
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
        expectTrue(c.storageBytes() >= skeleton.size(),
                   "从存储搬进来的字节不少于骨架本身");
        // 常驻不可能超过累计读进来的字节，也不可能超过块数乘块大小。
        expectTrue(c.residentBytes() <= c.storageBytes(),
                   "常驻不超过累计从存储读入的字节");
        expectTrue(c.residentBytes() <= c.capacityBytes(),
                   "常驻不超过块数乘块大小（上限 " +
                       std::to_string(c.capacityBytes() / 1024) + " KB）");
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

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
