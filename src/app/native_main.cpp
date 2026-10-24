/**
 * 原生宿主。
 * 作用不是"最终产品"，而是轨道 A 的快速迭代载体：
 * 同一条算法核，在原生里用文件输出 + 单测验证正确性，比在浏览器里调试快得多。
 * 它输出的是 PNG，便于肉眼确认：中文字形对不对、蒙文竖排的列序对不对。
 */
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/png_writer.h"
#include "core/engine/engine.h"
#include "demo/demo_documents.h"

#ifndef HEDITOR_NATIVE_ASSET_DIR
#define HEDITOR_NATIVE_ASSET_DIR "."
#endif

using namespace heditor;

namespace {

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    const char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + "/" + name;
}

/**
 * 大字号对照：同一个蒙文单词，横排 vs 竖排。
 * 用途是肉眼判定两件事：竖排是否启用了连接形态，以及字形朝向对不对。
 */
std::vector<BlockSpec> wordDemo(int mn_font) {
    const char* word =
        "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF";  // ᠮᠣᠩᠭᠣᠯ
    std::vector<BlockSpec> blocks;

    BlockSpec h;
    h.font_index = mn_font;
    h.size_pt = 56.0f;
    h.wm = WritingMode::HorizontalTB();
    h.language = "mn";
    h.color = Color::rgb(150, 40, 40);
    h.setText(word);
    blocks.push_back(std::move(h));

    BlockSpec v;
    v.font_index = mn_font;
    v.size_pt = 56.0f;
    v.wm = WritingMode::VerticalLR();
    v.line_factor = 1.05f;
    v.language = "mn";
    v.page_break_before = true;
    v.color = Color::rgb(20, 60, 150);
    v.setText(word);
    blocks.push_back(std::move(v));

    return blocks;
}

}  // namespace

int main(int argc, char** argv) {
    // 关掉 stdout 缓冲：崩溃时不至于把已经打出来的诊断信息一起丢掉
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string assetDir = HEDITOR_NATIVE_ASSET_DIR;
    std::string outDir = "out";
    std::string demo;
    float zoom = 1.5f;
    int onlyPage = -1;
    int dumpCount = 0;
    int repeat = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto takeArg = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (a == "--asset-dir") {
            assetDir = takeArg();
        } else if (a == "--out-dir") {
            outDir = takeArg();
        } else if (a == "--zoom") {
            zoom = std::stof(takeArg());
        } else if (a == "--page") {
            onlyPage = std::stoi(takeArg());
        } else if (a == "--demo") {
            demo = takeArg();
        } else if (a == "--dump") {
            dumpCount = std::stoi(takeArg());
        } else if (a == "--repeat") {
            repeat = std::max(1, std::stoi(takeArg()));
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "用法: heditor_native [--asset-dir DIR] [--out-dir DIR] [--zoom N] [--page N]"
                " [--demo word] [--repeat N]\n"
                "  --repeat N         进程内把\"排版+绘制\"整轮跑 N 次并报 min/中位/max。\n"
                "                     单次数字不能用来做性能决策：实测同一个二进制连跑 6 次，\n"
                "                       4 页 @1.5× 的绘制耗时在 135~179 ms 之间摆（±15%%）。\n");
            return 0;
        }
    }

    const std::string cjkFont = joinPath(assetDir, "fonts/NotoSansSC-VF.ttf");
    const std::string mnFont = joinPath(assetDir, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinFont = joinPath(assetDir, "fonts/NotoSans-Latin-VF.ttf");

    EditorEngine engine;
    if (!engine.init({cjkFont, mnFont, latinFont})) {
        std::fprintf(stderr,
                     "字体加载失败。\n  CJK: %s\n  蒙文: %s\n  拉丁: %s\n"
                     "用 --asset-dir 指定 assets 目录。\n",
                     cjkFont.c_str(), mnFont.c_str(), latinFont.c_str());
        return 2;
    }

    EngineConfig cfg;
    cfg.zoom = zoom;
    engine.setConfig(cfg);
    // 一轮完整流程：装文档 → 全量排版 → 绘制。
    // 抽成 lambda 是为了让 --repeat 能重复整轮（而不是只重复 repaint —— 那走的是
    // 增量路径，量不到"全量绘制"这个本实现真正关心、也是 Skia 会替换掉的那一段）。
    auto runOnce = [&]() {
        std::vector<BlockSpec> blocks;
        if (demo == "word") {
            blocks = wordDemo(1);
        } else if (demo == "mixed") {
            // 0 = 中文主字体，2 = 拉丁回退字体
            blocks = {mixedParagraphBlock(0, 2)};
        } else {
            blocks = sampleDocument(0, 1, 2);
        }
        // 建树交给引擎（与 web 那条路同一份代码）：这里没有字段，字段名单给空。
        engine.setDocument(demoTree(blocks, {}), StyleSheet{});
        engine.relayout();
        engine.repaint();
    };

    runOnce();  // 首次（顺带把字体缓存/字形缓存预热掉）

    // 重复测量
    //
    // 为什么必须有这一段：单次数字不能用来做性能决策。
    //   实测（同一二进制、同一台机器、连跑 6 次，4 页 @1.5×）：
    //       绘制耗时 = 135.5 / 141.7 / 153.9 / 159.8 / 167.9 / 178.5 ms
    //   极差 43 ms（占最小的 32%）。拿两个都只有 ~10% 差异的单次数字去
    //   判断"某个光栅优化有没有效果"，那纯粹是在读噪声。
    //   所以这里进程内重复，并且报 min / 中位 / max：中位数用来下结论，
    //   min 用来判断"这套代码最快能到多少"（受调度干扰最少）。
    if (repeat > 1) {
        std::vector<double> sh, la, pa, pf, pg, pp;
        sh.reserve(static_cast<size_t>(repeat));
        la.reserve(static_cast<size_t>(repeat));
        pa.reserve(static_cast<size_t>(repeat));
        pf.reserve(static_cast<size_t>(repeat));
        pg.reserve(static_cast<size_t>(repeat));
        pp.reserve(static_cast<size_t>(repeat));
        for (int i = 0; i < repeat; ++i) {
            runOnce();
            const EngineStats& s = engine.stats();
            sh.push_back(s.shape_ms);
            la.push_back(s.layout_ms);
            pa.push_back(s.paint_ms);
            pf.push_back(s.paint_fill_ms);
            pg.push_back(s.paint_glyph_ms);
            pp.push_back(s.paint_present_ms);
        }
        auto report = [&](const char* name, std::vector<double>& v) {
            std::sort(v.begin(), v.end());
            const double lo = v.front();
            const double hi = v.back();
            const double med = v[v.size() / 2];
            std::printf("  %-10s min %8.3f  中位 %8.3f  max %8.3f  ms\n", name, lo, med, hi);
        };
        std::printf("\n=== 重复 %d 次的分布（用中位数下结论，别用单次）===\n", repeat);
        report("shape", sh);
        report("layout", la);
        report("paint", pa);
        std::printf("  --- paint 三段分解 ---\n");
        report("铺面积", pf);
        report("字形", pg);
        report("呈现其余", pp);
    } else {
        std::printf("\n（提示：加 --repeat 15 再跑，单次计时在噪声里）\n");
    }

    // 再跑一轮，让下面那份 stats 报告来自一次真实的运行
    runOnce();

    const EngineStats& st = engine.stats();
    std::printf("=== 布局 ===\n");
    std::printf("页数            : %d\n", st.page_count);
    std::printf("字形数          : %d\n", st.glyph_count);
    std::printf("行/列数         : %d\n", st.placed_line_count);
    std::printf("cluster 数      : %d\n", st.cluster_count);
    std::printf("字体 run 数     : %d   （>块数 说明发生了回退切段）\n", st.font_run_count);
    std::printf("缺字(.notdef)   : %d   （跑完整条回退链仍无字形）\n", st.missing_glyph_count);
    std::printf("整形耗时        : %.3f ms\n", st.shape_ms);
    std::printf("布局总耗时      : %.3f ms\n", st.layout_ms);
    std::printf("绘制耗时        : %.3f ms   (zoom=%.2f, %dx%d px)\n", st.paint_ms, st.zoom,
                st.frame_width, st.frame_height);
    std::printf("  ├ 铺面积      : %.3f ms\n", st.paint_fill_ms);
    std::printf("  ├ 字形        : %.3f ms   (共 %d 个字形)\n", st.paint_glyph_ms, st.glyph_count);
    std::printf("  └ 呈现其余    : %.3f ms   (脏矩形记账 + 瞬态差分)\n", st.paint_present_ms);
    // 占用贴着上限就会被整份抹掉 —— 这是"预热了却不见变快"的成因。
    std::printf("字形缓存        : 占用 %.1f MB / 上限 %.1f MB\n",
                static_cast<double>(st.glyph_cache_bytes) / (1024.0 * 1024.0),
                static_cast<double>(st.glyph_cache_limit) / (1024.0 * 1024.0));
    // 这是"要导出的话多大"，不是常驻占用：导出图只在真被 frame() 读到时才拼。
    std::printf("窗口导出图尺寸  : %.2f MB（按需拼）\n",
                static_cast<double>(engine.frameBytes()) / (1024.0 * 1024.0));

    std::printf("\n=== 整形诊断（中文，横排）===\n%s",
                engine
                    .describeShaping(0, 12.0f, "\xE7\x97\x85\xE5\x8E\x86",
                                     WritingMode::HorizontalTB(), "zh")
                    .c_str());
    std::printf("\n=== 整形诊断（蒙文，vertical-lr）===\n%s",
                engine
                    .describeShaping(
                        1, 16.0f,
                        "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF",
                        WritingMode::VerticalLR(), "mn")
                    .c_str());

    if (dumpCount > 0) {
        const LayoutResult& r = engine.layout();
        std::printf(
            "\n=== 前 %d 个已定位字形（页面坐标，单位磅；页面 %.1f x %.1f，边距 %.1f）===\n",
            dumpCount, engine.config().page.width_pt, engine.config().page.height_pt,
            engine.config().page.margin_pt);
        for (int i = 0; i < dumpCount && i < static_cast<int>(r.glyphs.size()); ++i) {
            const PlacedGlyph& pg = r.glyphs[i];
            std::printf("  [%2d] page=%d line=%2d %s gid=%5u  x=%8.2f  y=%8.2f  cluster=%u\n", i,
                        pg.page, pg.line, pg.vertical ? "V" : "H", pg.glyph_id, pg.x_pt, pg.y_pt,
                        pg.cluster);
        }
        // 每个块的纵向范围
        std::printf("\n=== 各页字形包围盒 ===\n");
        for (int p = 0; p < r.stats.page_count; ++p) {
            float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
            int n = 0;
            for (const PlacedGlyph& pg : r.glyphs) {
                if (pg.page != p) continue;
                minx = std::min(minx, pg.x_pt);
                miny = std::min(miny, pg.y_pt);
                maxx = std::max(maxx, pg.x_pt);
                maxy = std::max(maxy, pg.y_pt);
                ++n;
            }
            if (!n) continue;
            std::printf("  page %d: n=%d  x∈[%.1f, %.1f]  y∈[%.1f, %.1f]\n", p, n, minx, maxx, miny,
                        maxy);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    const int totalPages = std::max(1, st.page_count);
    for (int p = 0; p < totalPages; ++p) {
        if (onlyPage >= 0 && p != onlyPage) continue;

        engine.paintPageRange(p, 1);
        char name[256];
        std::snprintf(name, sizeof(name), "page-%d.png", p);
        const std::string outFile = joinPath(outDir, name);

        const bool ok =
            png::writeRgba(outFile, engine.frame(), engine.frameWidth(), engine.frameHeight());
        std::printf("写出 %s  %s  (%dx%d)\n", outFile.c_str(), ok ? "OK" : "FAILED",
                    engine.frameWidth(), engine.frameHeight());
        if (!ok) return 3;
    }

    return 0;
}
