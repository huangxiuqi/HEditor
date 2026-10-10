/**
 * 可证伪的整形与布局单测。
 * 目的不是"跑通"，而是把关键结论变成断言，让"整形真的发生了""竖排度量真的取到了"
 * 这类事情不靠肉眼、也不靠"能编译"来判定。
 * 失败时退出码非 0，并打印期望值/实际值。
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/engine.h"
#include "core/layout/layout.h"
#include "core/text/fallback.h"
#include "core/text/shaper.h"
#include "core/utf8.h"

#include <hb.h>

#ifndef CED_NATIVE_ASSET_DIR
#define CED_NATIVE_ASSET_DIR "."
#endif

using namespace ced;

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
    const std::string cjkPath = joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string mnPath =
        joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinPath = joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

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
        engine.setBlocks(sampleDocument(0, 1, 2));
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

        eOff.setBlocks({mixedParagraphBlock(cjk)});        // 不回退
        eOn.setBlocks({mixedParagraphBlock(cjk, latin)});  // 回退到拉丁
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
        e.setBlocks({b});
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

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
