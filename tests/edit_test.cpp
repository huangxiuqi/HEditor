/**
 * 档 1 的可证伪断言：文档模型、光标几何、命中测试、增量呈现。
 * 这一批断言和整形那批的性质不同。
 * 整形那批验的是"算得对不对"；这一批验的是"到底有没有做增量"。
 * 所以每条断言都尽量落在一个数字上（写了多少像素、重排了多少行），
 * 而不是"看起来流畅"。
 * 其中必须包含一条故意断言档 1 局限的断言："敲字仍然是整页重画"。
 * 档 2（布局增量化）完成之后这条应当失败，届时应更新基准。
 * 把一条已知的坏性质写成断言看起来别扭，但这比"整页重画无人知晓"要好得多：
 * 可证伪的基准，前提是基准本身是可以被证伪的。
 */
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/doc/document.h"
#include "core/engine.h"
#include "core/layout/layout.h"
#include "core/text/shaper.h"

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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string cjkPath = joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string mnPath =
        joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinPath = joinPath(CED_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

    std::printf("[1] 码点工具：UTF-8 ↔ 码点往返\n");
    {
        const std::string s = "10\xE2\x81\xB9/\xC3\xA9\xE4\xB8\xAD";  // 10⁹/é中
        const std::vector<uint32_t> cps = utf8ToCodepoints(s);
        std::printf("     \"10⁹/é中\" = %zu 字节 → %zu 码点\n", s.size(), cps.size());
        expectTrue(cps.size() == 6, "6 个码点（1,0,⁹,/,é,中）");
        expectTrue(codepointsToUtf8(cps) == s, "码点 → UTF-8 往返无损");

        // ⁹ 占 3 字节但只有 1 个码点 —— 这正是"字节偏移会切进字符内部"的根因
        expectTrue(std::string("\xE2\x81\xB9").size() == 3, "U+2079 占 3 个 UTF-8 字节");

        // 非法码点（代理区 / 超出 U+10FFFF）必须被替换，输出永远是合法 UTF-8
        const std::vector<uint32_t> bad{0xD800, 0x110000, 'A'};
        const std::string enc = codepointsToUtf8(bad);
        expectTrue(utf8ToCodepoints(enc).size() == 3,
                   "非法码点被替换为 U+FFFD，解码回来仍是 3 个码点（输出是合法 UTF-8）");
        bool hasFffd = false;
        for (uint32_t c : utf8ToCodepoints(enc)) hasFffd = hasFffd || (c == 0xFFFD);
        expectTrue(hasFffd, "确有 U+FFFD 被写入");
    }

    std::printf("\n[2] 编辑原语与簇吸附的边界\n");
    {
        std::vector<uint32_t> buf{'a', 'b', 'c'};
        textbuf::insertAt(buf, 99, std::vector<uint32_t>{'z'});
        expectTrue(buf.size() == 4 && buf.back() == 'z',
                   "插入点越界被夹到末尾（不是崩溃、不是静默丢弃）");

        expectTrue(textbuf::eraseRange(buf, 2, 1) == 0, "逆序范围 [2,1) 什么都不删");
        expectTrue(textbuf::eraseRange(buf, 1, 99) == 3, "删除范围越界被夹住，返回真实删除数 3");
        expectTrue(buf.size() == 1 && buf[0] == 'a', "删完只剩第一个码点");

        textbuf::insertAt(buf, 0, std::vector<uint32_t>{});
        expectTrue(buf.size() == 1, "插入空数组是空操作");

        // 簇起点表：{0, 2, 5, 7}，7 是段长哨兵
        const std::vector<uint32_t> starts{0, 2, 5, 7};
        expectTrue(clusterFloor(starts, 0) == 0, "clusterFloor(0) = 0");
        expectTrue(clusterFloor(starts, 3) == 2, "落在簇内部 → 回退到该簇的起点");
        expectTrue(clusterFloor(starts, 7) == 7, "段末命中哨兵");
        expectTrue(clusterCeil(starts, 2) == 5, "clusterCeil(2) = 5（下一个簇首）");
        expectTrue(clusterCeil(starts, 6) == 7, "clusterCeil(6) = 7（哨兵）");
        expectTrue(clusterCeil(starts, 7) == 7, "段末没有下一个簇，返回自身");

        // 哨兵是必需的：没有它，"最后一个簇"的 floor 会退化成 0
        const std::vector<uint32_t> noSentinel{0, 2, 5};
        expectTrue(clusterFloor(noSentinel, 6) == 5, "无哨兵时 floor 仍能给出最后一个簇首");
        expectTrue(clusterCeil(noSentinel, 6) == 6, "无哨兵时 ceil 返回入参本身（调用方据此退化）");
    }

    // 引擎准备
    EditorEngine e;
    if (!e.init({cjkPath, mnPath, latinPath})) {
        std::fprintf(stderr, "字体加载失败：%s\n", cjkPath.c_str());
        return 2;
    }

    EngineConfig cfg;
    cfg.zoom = 1.5f;
    e.setConfig(cfg);

    BlockSpec doc;
    doc.font_index = 0;  // CJK
    doc.size_pt = 12.0f;
    doc.wm = WritingMode::HorizontalTB();
    doc.line_factor = 1.75f;
    doc.language = "zh";
    doc.setText(
        "\xE6\x82\xA3\xE8\x80\x85\xE4\xB8\xBB\xE8\xAF\x89\xEF\xBC\x9A"
        "\xE5\x8F\x8D\xE5\xA4\x8D\xE5\xBF\x83\xE6\x82\xB8"
        "3\xE5\xA4\xA9\xE3\x80\x82");  // 患者主诉：反复心悸3天。
    e.setBlocks({doc});
    e.relayout();
    e.repaint();

    const long long framePixels =
        static_cast<long long>(e.editStats().frame_w) * e.editStats().frame_h;
    const long long fullPaint = e.editStats().repainted_pixels;
    std::printf("     单页文档：帧 %d × %d = %.2f M 像素；首次全量写入 %.2f M 像素\n",
                e.editStats().frame_w, e.editStats().frame_h,
                static_cast<double>(framePixels) / 1e6, static_cast<double>(fullPaint) / 1e6);
    expectTrue(fullPaint > framePixels / 2, "首次呈现确实是全量（写入量 ≥ 半帧）");

    std::printf("\n[3] 敲字：文档真的变了、光标跟着走\n");
    {
        const size_t cpsBefore = e.editStats().doc_codepoints;
        e.setCaret(0, 3);
        expectTrue(e.caret().offset == 3, "光标设到段内偏移 3");

        const bool typed = e.typeTextAndRender("\xE6\xB5\x8B");  // 测
        expectTrue(typed, "输入一个汉字返回 true");
        expectTrue(e.editStats().doc_codepoints == cpsBefore + 1, "全文码点数 +1");
        expectTrue(e.caret().offset == 4, "光标停在新字之后（不是停在字前、也不是跳到段首）");
        expectTrue(e.blocks()[0].cp_text[3] == 0x6D4B, "第 3 个码点就是刚插入的 U+6D4B（测）");

        // 这一节是单段文档：脏段就是第 0 段，前面没有可复用的前缀，
        //   所以这里按全量走是必然的，不是"档 2 没做"。多段文档的增量见 [15]。
        const EditStats& es = e.editStats();
        std::printf("     重排行数 %d / 总行数 %d；layout_was_full=%d\n", es.relaid_lines,
                    es.total_lines, es.layout_was_full ? 1 : 0);
        expectTrue(es.layout_was_full && es.relaid_lines == es.total_lines,
                   "单段文档没有可复用的前缀 → 全量（多段增量见 [15]）");

        // 同样是单段文档的必然结果：插入字符让所在页内容变了 → 整页重画
        std::printf("     敲字后写入 %.2f M 像素（全帧 %.2f M）\n",
                    static_cast<double>(es.repainted_pixels) / 1e6,
                    static_cast<double>(framePixels) / 1e6);
        expectTrue(es.repainted_pixels > fullPaint / 2, "敲字让所在页内容变了 → 该页整页重画");

        // 退回原状，便于后续小节
        e.setCaret(0, 4);
        expectTrue(e.backspaceAndRender(), "退格返回 true");
        expectTrue(e.editStats().doc_codepoints == cpsBefore, "退格后码点数回到原值");
        expectTrue(e.caret().offset == 3, "退格后光标回到插入点之前");
    }

    std::printf("\n[4] 字素簇不可切分：e + U+0301 是一个字符，不是一个半\n");
    {
        EditorEngine e2;
        expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        e2.setConfig(cfg);

        BlockSpec b;
        b.font_index = 2;  // 拉丁（有组合重音）
        b.size_pt = 24.0f;
        b.wm = WritingMode::HorizontalTB();
        b.language = "en";
        b.cp_text = {static_cast<uint32_t>('e'), 0x0301u};  // e + 组合重音 = é
        e2.setBlocks({b});
        e2.relayout();
        e2.repaint();

        const std::vector<uint32_t>& starts = e2.layout().para_cluster_starts[0];
        std::printf("     2 个码点 → 簇起点表 ");
        for (uint32_t v : starts) std::printf("%u ", v);
        std::printf("\n");
        expectTrue(starts.size() == 2, "两个码点合成为 1 个簇（表 = {0, 2}：簇首 + 段长哨兵）");
        expectTrue(starts.size() == 2 && starts[0] == 0 && starts[1] == 2, "簇覆盖整个段");

        // 光标吸附：偏移 1 落在簇内部（e 与重音之间），必须被吸回 0
        e2.setCaret(0, 1);
        expectTrue(e2.caret().offset == 0, "偏移 1 被吸附到簇首 0（光标不会停在半个 é 里）");

        Rect r0{}, r1{};
        e2.setCaret(0, 0);
        const bool ok0 = e2.caretRectPt(&r0);
        e2.setCaret(0, 1);
        const bool ok1 = e2.caretRectPt(&r1);
        expectTrue(ok0 && ok1, "两个偏移都能查到光标矩形");
        expectNear(r0.x, r1.x, 0.01, "簇内任意偏移映射到同一个 x（吸附是几何上真的发生了）");

        // 退格必须删掉整个簇。只删一个码点会留下孤立的 U+0301。
        e2.setCaret(0, 2);
        expectTrue(e2.backspace(), "退格返回 true");
        std::printf("     退格后剩 %zu 个码点\n", e2.blocks()[0].cp_text.size());
        expectTrue(e2.blocks()[0].cp_text.empty(),
                   "退格一次删掉 2 个码点（删的是簇，不是码点；否则会留下孤立的组合重音）");
        expectTrue(e2.caret().offset == 0, "退格后光标在段首");
    }

    std::printf("\n[5] 几何正反查互逆：正查出的光标矩形，反查必须回到同一位置\n");
    {
        const std::vector<uint32_t>& starts = e.layout().para_cluster_starts[0];
        int total = 0, ok = 0;
        for (size_t i = 0; i < starts.size(); ++i) {
            const size_t cp = starts[i];
            e.setCaret(0, cp);
            Rect r{};
            if (!e.caretRectPt(&r)) continue;
            ++total;

            // 探针取在光标"沿行内轴的正方向一点点"，而不是矩形中心。
            //   取中心会命中一个真实的语义陷阱：光标在簇首，中心点落在该簇的
            //   后半段，反查就应当吸附到下一个簇 —— 那是正确行为，但会让
            //   round-trip 失败。要在同一个簇内探测，只能贴着簇首往正方向偏一点。
            float px = r.x + r.w * 0.5f;
            float py = r.y + r.h * 0.5f;
            if (r.w > r.h) {
                py += 0.1f;  // 竖排：光标是横条，沿 y 探
            } else {
                px += 0.1f;  // 横排：光标是竖条，沿 x 探
            }

            TextPos back{};
            if (e.hitTestPt(px, py, &back) && back.para == 0 && back.offset == cp) ++ok;
        }
        std::printf("     %d 个插入点，round-trip 成功 %d 个\n", total, ok);
        expectTrue(total >= 3, "至少查到 3 个插入点的几何");
        expectTrue(ok == total, "正查 → 反查全部回到原位置（光标绘制与鼠标点选共用同一份几何）");

        // 点在页外必须失败，而不是给一个胡乱的位置
        TextPos junk{};
        expectTrue(!e.hitTestPt(-50.0f, 100.0f, &junk), "横向出了页外 → 命中失败（不猜）");
        expectTrue(!e.hitTestPt(200.0f, cfg.page.height_pt + 7.0f, &junk),
                   "纵向落在页间空隙 → 命中失败（不猜）");
        expectTrue(!e.hitTestPt(200.0f, 5000.0f, &junk),
                   "纵向落到没有内容的远处页面 → 命中失败（不猜）");
    }

    std::printf("\n[6] 光标移动 = 真增量：写入量与全帧差两个数量级\n");
    {
        e.setCaret(0, 0);
        e.repaint();
        const bool moved = e.moveCaretAndRender(1);
        const long long caretPaint = e.editStats().repainted_pixels;

        // 基准必须取第一次呈现的写入量（fullPaint = 全帧），不能取"再 repaint 一次"
        //   的数字 —— 后者本身已经是增量了（帧尺寸没变，只贴了个旧光标），
        //   拿它当分母会得出"100%"的荒谬结论。
        //   结论：验证增量的分母只能是真正的全量。
        std::printf("     光标右移一次：写入 %lld 像素（首次全量 %lld，占比 %.4f%%）\n", caretPaint,
                    fullPaint,
                    100.0 * static_cast<double>(caretPaint) / static_cast<double>(fullPaint));
        expectTrue(moved, "光标右移返回 true");
        // 这条是"局部重绘真的落地了"的核心证据：
        //   底图一个字都没变，只有旧/新两个光标矩形被贴回 + 画了一条光标。
        expectTrue(caretPaint * 100 < fullPaint,
                   "光标移动的写入量 < 首次全量的 1/100（局部重绘不是估算，是记账）");

        // 记账的归属也必须干净。
        //   光标移动没有重排，所以这三个字段必须是 0。
        //   它们从前是在 presentInternal() 里被无条件写成满值的 —— 于是"移动一次光标"
        //   会被上报成"重排了 12 行、布局耗时 3ms"。数字本身是真的（来自上一次重排），
        //   归属是错的，而这种假账最容易被当成性能证据引用出去。
        expectTrue(!e.editStats().layout_ran, "光标移动：本轮未重排（layout_ran=false）");
        expectTrue(e.editStats().relaid_lines == 0,
                   "光标移动：重排行数上报 0，不是上一次重排的残留");
        expectTrue(e.editStats().layout_ms == 0.0,
                   "光标移动：布局耗时上报 0，不是上一次重排的残留");
        expectTrue(!e.editStats().layout_was_full,
                   "光标移动：不能被标成「全量布局」（那会让人以为光标移动很贵）");

        // 光标不可见时，写入量应该更小（只擦不画）
        e.showCaret(false);
        e.moveCaretAndRender(-1);
        const long long hiddenPaint = e.editStats().repainted_pixels;
        e.showCaret(true);
        std::printf("     光标隐藏时写入 %lld 像素\n", hiddenPaint);
        expectTrue(hiddenPaint < caretPaint, "光标隐藏时写入量更小（只擦不画）");

        // 移动到位移边界必须停住，而不是越界
        e.setCaret(0, 0);
        expectTrue(!e.moveCaretBy(-1), "已在段首，向左移动返回 false（没动就不该重绘）");
        const size_t len = e.blocks()[0].cp_text.size();
        e.setCaret(0, len);
        expectTrue(!e.moveCaretBy(1), "已在段末，向右移动返回 false");
    }

    std::printf("\n[7] 竖排（蒙文侧倒）的光标几何：横条、沿列向下\n");
    {
        EditorEngine e3;
        expectTrue(e3.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        e3.setConfig(cfg);

        BlockSpec b;
        b.font_index = 1;  // 蒙文
        b.size_pt = 20.0f;
        b.wm = WritingMode::VerticalLR();
        b.line_factor = 1.8f;
        b.language = "mn";
        b.setText("\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF");
        e3.setBlocks({b});
        e3.relayout();
        e3.repaint();

        const std::vector<uint32_t>& starts = e3.layout().para_cluster_starts[0];
        std::printf("     蒙文 6 码点 → %zu 个簇边界\n", starts.size());
        expectTrue(starts.size() >= 2, "至少有 1 个簇 + 哨兵");

        Rect first{};
        e3.setCaret(0, starts.front());
        expectTrue(e3.caretRectPt(&first), "竖排光标矩形可查");
        std::printf("     光标矩形 w=%.2f h=%.2f（列宽 %.2f）\n", first.w, first.h,
                    b.size_pt * b.line_factor);
        expectTrue(first.w > first.h, "竖排光标是横条（宽 = 列盒宽 > 高 = 光标粗细）");
        expectNear(first.w, b.size_pt * b.line_factor, 0.5, "光标宽度 = 列宽（占满列盒）");

        // 沿列向下：相邻簇边界的光标 y 必须递增
        Rect prev{};
        e3.setCaret(0, starts.front());
        e3.caretRectPt(&prev);
        int increasing = 0, pairs = 0;
        for (size_t i = 1; i < starts.size(); ++i) {
            Rect r{};
            e3.setCaret(0, starts[i]);
            if (!e3.caretRectPt(&r)) continue;
            ++pairs;
            if (r.y > prev.y) ++increasing;
            prev = r;
        }
        std::printf("     沿列 %d 对相邻插入点，y 递增 %d 对\n", pairs, increasing);
        expectTrue(pairs > 0 && increasing == pairs, "竖排光标沿列向下单调递增（列序方向没搞反）");

        // 竖排也要能做命中反查（探针沿 y 正方向偏一点）
        e3.setCaret(0, starts[0]);
        Rect r{};
        e3.caretRectPt(&r);
        TextPos back{};
        const bool hit = e3.hitTestPt(r.x + r.w * 0.5f, r.y + r.h * 0.5f + 0.1f, &back);
        expectTrue(hit && back.offset == starts[0], "竖排的命中反查同样回到原位置");
    }

    std::printf("\n[8] 连字「ff」不得合并：一个字母就是一格\n");
    {
        // 这条断言守的是删除语义，不是排版美观。
        //   HarfBuzz 默认开 liga/clig，"ff"/"fi" 会被合成一个连字字形，
        //   两个码点的 cluster 也被归并成一个 —— 簇表里第二个 f 的起点就此消失，
        //   于是"删一个簇"= 删两个字母。全中文的用例一条都盖不到这个 bug。
        EditorEngine e4;
        expectTrue(e4.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c4 = cfg;
        e4.setConfig(c4);

        BlockSpec b;
        b.font_index = 2;  // 拉丁
        b.size_pt = 24.0f;
        b.wm = WritingMode::HorizontalTB();
        b.language = "en";
        b.cp_text = {static_cast<uint32_t>('o'), static_cast<uint32_t>('f'),
                     static_cast<uint32_t>('f'), static_cast<uint32_t>('i'),
                     static_cast<uint32_t>('c'), static_cast<uint32_t>('e')};
        e4.setBlocks({b});
        e4.relayout();
        e4.repaint();

        const std::vector<uint32_t>& starts = e4.layout().para_cluster_starts[0];
        std::printf("     \"office\" 6 个码点 → 字形 %d 个，簇表 = ",
                    e4.layout().stats.glyph_count);
        for (uint32_t v : starts) std::printf("%u ", v);
        std::printf("\n");

        // 决定性判据：六个字母、六个字形、七个簇表项。
        //   只要 liga 被重新打开，"ff""fi" 会各并一次，字形数掉到 4、簇表掉到 5 项。
        expectTrue(e4.layout().stats.glyph_count == 6,
                   "6 个码点 → 6 个字形（ff / fi 没有被合成连字字形）");
        expectTrue(starts.size() == 7,
                   "6 个码点 → 7 项簇表（6 个簇首 + 段长哨兵）；连字一合并这里就少");

        // 光标停在两个 f 之间：偏移 2 必须仍然是一个合法簇边界（不被吸回 0 或 1）
        e4.setCaret(0, 2);
        expectTrue(e4.caret().offset == 2, "偏移 2（两个 f 之间）是一个合法插入点，未被吸附走");

        expectTrue(e4.typeTextAndRender("x"), "在两个 f 之间插入 x");
        expectTrue(e4.blocks()[0].cp_text.size() == 7, "文档变成 7 个码点");
        expectTrue(e4.backspaceAndRender(), "退格");
        expectTrue(e4.blocks()[0].cp_text.size() == 6,
                   "退格只删掉刚插入的 x 一个码点（不是把两个 f 一起带走）");

        // 从段尾连退三次：每次都只该少一个码点
        e4.setCaret(0, 6);
        for (int i = 0; i < 3; ++i) e4.backspaceAndRender();
        std::printf("     从段尾连退 3 次：6 → %zu 个码点\n", e4.blocks()[0].cp_text.size());
        expectTrue(e4.blocks()[0].cp_text.size() == 3,
                   "连退三次各删 1 个码点：6 → 3（ff/fi 若被合并，这里会变成 6 → 2）");
    }

    std::printf("\n[9] 页数增长时帧缓冲必须跟着长（否则新页永远看不见）\n");
    {
        // 现场还原：文档正好填满一页，光标停在页尾，再敲一个字 → 内容溢到第 2 页。
        //   修复前 repaint() 把帧范围锁在"底图已经画过多少页"上，
        //   于是 page_count 变成 2 而 frame_h 不动 → 第 2 页永远不进帧，
        //   屏幕上"字打了但什么也没多"。而且不会自愈，之后每次重绘都照样按 1 页算。
        EditorEngine e5;
        expectTrue(e5.init({cjkPath, mnPath, latinPath}), "引擎初始化");

        EngineConfig c5;
        c5.zoom = 1.0f;  // 1 pt = 1 px：便于直接按磅定位像素
        c5.page.width_pt = 200.0f;
        c5.page.height_pt = 80.0f;
        c5.page.margin_pt = 10.0f;
        c5.page_gap_pt = 10.0f;
        e5.setConfig(c5);

        BlockSpec probe;
        probe.font_index = 0;  // 中文
        probe.size_pt = 12.0f;
        probe.wm = WritingMode::HorizontalTB();
        probe.line_factor = 1.75f;
        probe.language = "zh";

        // 不硬编码"每页几个字" —— 那是从字体度量推出来的，度量一改断言就假红。
        //   改成现场搜出临界字数：第一个让它排到第 2 页的字数。
        int n_crit = 0;
        for (int n = 2; n <= 400; ++n) {
            probe.cp_text.assign(static_cast<size_t>(n), 0x6D4B);  // n 个「测」
            e5.setBlocks({probe});
            e5.relayout();
            if (e5.stats().page_count >= 2) {
                n_crit = n;
                break;
            }
        }
        std::printf("     临界字数 = %d（第 %d 个字把文档推到了第 2 页）\n", n_crit, n_crit);
        expectTrue(n_crit > 1, "搜到了跨页临界点");

        // 刚好一页，并且真的呈现一次 —— 只有 repaint 过 base_valid_ 才为真，
        // 而这条 bug 只在 base_valid_ 为真之后才暴露。
        probe.cp_text.assign(static_cast<size_t>(n_crit - 1), 0x6D4B);
        e5.setBlocks({probe});
        e5.relayout();
        e5.repaint();

        const int pages1 = e5.stats().page_count;
        const int h1 = e5.frameHeight();
        std::printf("     %d 字 → %d 页，帧高 %d px\n", n_crit - 1, pages1, h1);
        expectTrue(pages1 == 1, "临界字数减一 → 仍是 1 页");

        // 在页尾敲一个字
        e5.setCaret(0, static_cast<size_t>(n_crit - 1));
        expectTrue(e5.typeTextAndRender("\xE6\xB5\x8B"), "在页尾插入一个汉字");  // 测

        const int pages2 = e5.stats().page_count;
        const int h2 = e5.frameHeight();
        std::printf("     %d 字 → %d 页，帧高 %d px\n", n_crit, pages2, h2);
        expectTrue(pages2 == 2, "这一个字把文档推到了第 2 页");

        // 核心断言。修复前 count 锁在 base_page_count_（=1），
        //   frame_h 与宽高都没变 → geom_changed 恒 false → h2 == h1，新页不可见。
        expectTrue(h2 > h1, "帧缓冲高度跟随页数增长（修复前这里恒等 → 新页不可见）");
        expectTrue(e5.editStats().total_pages == 2, "本次呈现覆盖 2 页");
        expectTrue(e5.editStats().repainted_pages == 2,
                   "页数变了 → 整帧重建，2 页都重画（不是只贴第 1 页）");

        // 更硬的一条：最后一页的区域里必须真的画出了字（暗像素），
        // 而不只是一块空白页底 —— 否则"帧变高了"也可能是块白板。
        {
            int g0 = 0, g1 = 0, gother = 0;
            float maxy0 = -1e9f, miny1 = 1e9f, maxy1 = -1e9f;
            for (const auto& g : e5.layout().glyphs) {
                if (g.page == 0) {
                    ++g0;
                    maxy0 = std::max(maxy0, g.y_pt);
                } else if (g.page == 1) {
                    ++g1;
                    miny1 = std::min(miny1, g.y_pt);
                    maxy1 = std::max(maxy1, g.y_pt);
                } else
                    ++gother;
            }
            std::printf(
                "     [诊断] 字形分布 page0=%d page1=%d other=%d；"
                "page0 最大 y=%.2f pt；page1 y ∈ [%.2f, %.2f] pt\n",
                g0, g1, gother, maxy0, (g1 ? miny1 : 0.0f), (g1 ? maxy1 : 0.0f));
        }
        const int page_h_px = static_cast<int>(std::lround(c5.page.height_pt * c5.zoom));
        const int gap_px = static_cast<int>(std::lround(c5.page_gap_pt * c5.zoom));
        const int last_local = pages2 - 1;
        const int y0 = last_local * (page_h_px + gap_px);
        const int fw = e5.frameWidth();
        const uint8_t* px = e5.frame();
        int dark = 0;
        int nonwhite_lo = 0, darkest_lo = 255, nonwhite_hi = 0, darkest_hi = 255;
        for (int y = 0; y < h2; ++y) {
            for (int x = 0; x < fw; ++x) {
                const uint8_t* p =
                    px +
                    (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4;
                if (p[0] < 128 && p[1] < 128 && p[2] < 128) ++dark;
                if (y < y0) {
                    const int mn = std::min(std::min(p[0], p[1]), p[2]);
                    if (mn < 200) ++nonwhite_lo;
                    darkest_lo = std::min(darkest_lo, mn);
                }
            }
        }
        // 为什么两段分开扫：只有当"第 1 页区有内容、第 2 页区没有"时，
        //   才能把问题定性成"新页没画"，而不是"整帧都是白的"。
        for (int y = y0; y < h2; ++y) {
            for (int x = 0; x < fw; ++x) {
                const uint8_t* p =
                    px +
                    (static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4;
                const int mn = std::min(std::min(p[0], p[1]), p[2]);
                if (mn < 200) ++nonwhite_hi;
                darkest_hi = std::min(darkest_hi, mn);
            }
        }
        std::printf(
            "     [诊断] 上半区(第1页): 非白 %d 最暗 %d ｜ 下半区(第2页): 非白 %d 最暗 %d\n",
            nonwhite_lo, darkest_lo, nonwhite_hi, darkest_hi);
        std::printf("     第 %d 页区域的暗像素 = %d\n", pages2, dark);
        expectTrue(dark > 0, "新页上确实画出了字形（不是一块空白页底）");

        // 反向：页数不变时仍然必须走增量，别为了让上面这条通过就把整帧重建常态化
        e5.setCaret(0, 0);
        e5.repaint();  // setCaret 只改位置不动像素；统计要 present 之后才有效
        const int h3 = e5.frameHeight();
        const long long px3 = e5.editStats().repainted_pixels;
        std::printf("     光标移到段首：帧高 %d px，写入 %lld px（全帧 %d px）\n", h3, px3,
                    e5.frameWidth() * e5.frameHeight());
        expectTrue(h3 == h2, "光标移动不改变帧尺寸");
        expectTrue(px3 < static_cast<long long>(e5.frameWidth()) * h3 / 100,
                   "页数不变时仍是增量：写入量 < 全帧的 1/100（没有退化成整帧重建）");
    }

    std::printf("\n[10] 字形缓存：必须逐字节等价于直接光栅\n");
    {
        // 同一份文档、同样的 zoom，渲染两遍：一次带缓存、一次不带。
        //
        // 这是缓存敢不敢上线的唯一依据。任何 glyph cache（包括 Skia 的
        //   SkGlyphCache）都受同一条约束：覆盖率是按像素分箱算的，
        //   亚像素相位一变就是另一份 mask。如果这条断言不成立，
        //   那么"优化"两个字就是错的 —— 那是改了渲染。
        auto renderDoc = [&](bool with_cache, double* glyph_ms, long long* hits, long long* misses,
                             int* entries) -> std::vector<uint8_t> {
            EditorEngine e;
            if (!e.init({cjkPath, mnPath, latinPath})) return {};
            EngineConfig c = cfg;
            c.zoom = 1.5f;
            e.setConfig(c);
            e.setBlocks(sampleDocument(0, 1, 2));
            if (!with_cache) e.disableGlyphCache();
            e.relayout();
            e.repaint();
            const EngineStats& s = e.stats();
            if (glyph_ms) *glyph_ms = s.paint_glyph_ms;
            if (hits) *hits = s.glyph_cache_hits;
            if (misses) *misses = s.glyph_cache_misses;
            if (entries) *entries = s.glyph_cache_entries;
            return std::vector<uint8_t>(e.frame(), e.frame() + e.frameBytes());
        };

        double gms_cached = 0.0, gms_raw = 0.0;
        long long hits = 0, misses = 0;
        int entries = 0;
        const std::vector<uint8_t> a = renderDoc(true, &gms_cached, &hits, &misses, &entries);
        const std::vector<uint8_t> b = renderDoc(false, &gms_raw, nullptr, nullptr, nullptr);

        std::printf(
            "     带缓存 %zu B / 不带 %zu B；字形分段 %.3f ms vs %.3f ms；"
            "命中 %lld / 未命中 %lld，缓存条目 %d\n",
            a.size(), b.size(), gms_cached, gms_raw, hits, misses, entries);
        expectTrue(!a.empty() && a.size() == b.size(), "两次渲染的帧尺寸一致");
        expectTrue((a.size() == b.size()) && std::memcmp(a.data(), b.data(), a.size()) == 0,
                   "缓存路径与直接光栅路径逐字节相同（缓存是纯等价变换）");

        // 必须有的第三条：两遍都有墨迹。
        //
        //   为什么"逐字节相同"这一条不够 —— 而且本次是真栽在这上面：
        //   覆盖率缓冲改成成员复用之后，cov 每行都被清空、搬进 mask 的那次 memcpy
        //   却留在整个 y 循环之外，于是每一行都填上了"最后一行"的覆盖率（几乎恒为空）。
        //   结果 mask 尺寸/位置/cacheable 全对、cov 恒为 0，一个字都画不出来；
        //   而"逐字节相同"照样成立（两边都空，当然相等），连 PNG 的 SHA-256 都稳定。
        //   所以"等价"必须配一条"确实有东西"才有意义。同一条纪律见 bitmap.cpp
        //   的 rasterizeGlyph 里那段说明。
        auto inkOf = [](const std::vector<uint8_t>& px) {
            size_t n = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                if (px[i] < 128 && px[i + 1] < 128 && px[i + 2] < 128) ++n;
            }
            return n;
        };
        const size_t ink_a = inkOf(a);
        const size_t ink_b = inkOf(b);
        std::printf("     墨迹像素：带缓存 %zu / 不带 %zu（占全帧 %.4f%%）\n", ink_a, ink_b,
                    100.0 * static_cast<double>(ink_a) / static_cast<double>(a.size() / 4));
        expectTrue(ink_a > 2000 && ink_a == ink_b,
                   "两遍渲染都真的画出了字形（墨迹像素数相同且足够多）");

        // 反向两条，防"因为缓存根本没生效而假绿"
        const int glyphs = static_cast<int>(hits + misses);
        expectTrue(hits > 0, "缓存确实被命中过（不是「没生效所以碰巧相同」）");
        expectTrue(glyphs > 0 && misses < glyphs,
                   "未命中数 < 字形总数 —— 同一个字形的重复出现复用了 mask");
        expectTrue(entries > 0 && entries < glyphs,
                   "缓存条目数 < 字形总数 —— 去重确实发生了（亚像素相位没有散开）");
    }

    std::printf("\n[11] 表单：控件是区间、边界是策略\n");
    {
        //  "患者主诉：反复心悸三天，加重一天。"
        //   0患 1者 2主 3诉 4： 5反 6复 7心 8悸 9三 10天 11， 12加 13重 14一 15天 16。
        //   K1 = [5,9)   "反复心悸"  可编辑
        //   K2 = [12,16) "加重一天"  可编辑
        //   → 0..4、9..11、16..17 是没有控件的普通正文。
        auto T = [](int o) { return TextPos{0, static_cast<size_t>(o)}; };
        auto mk = [](int a, int b, const char* id, bool ro) {
            Control c;
            // 不能用外层的 T：本 lambda 无捕获，隐式捕获在 C++20 下是错误。
            c.range.start = TextPos{0, static_cast<size_t>(a)};
            c.range.end = TextPos{0, static_cast<size_t>(b)};
            c.id = id;
            c.read_only = ro;
            return c;
        };

        // 策略层是纯函数，所以边界情况在这里穷举
        {
            ControlSet cs;
            cs.add(mk(5, 9, "k1", false));
            cs.add(mk(12, 16, "k2", false));
            cs.normalize();
            expectTrue(cs.size() == 2, "控件集合里 2 个控件");

            expectTrue(EditPolicy::canInsert(cs, EditMode::Form, T(6)), "字段内：可插入");
            expectTrue(EditPolicy::canInsert(cs, EditMode::Form, T(9)),
                       "字段末尾（end）可插入 —— 追加是合法编辑，所以插入判定要按闭区间看");
            expectTrue(!EditPolicy::canInsert(cs, EditMode::Form, T(10)), "end 之后一位：出界，拒");
            expectTrue(!EditPolicy::canInsert(cs, EditMode::Form, T(2)), "字段之外：拒");

            expectTrue(EditPolicy::canErase(cs, EditMode::Form, T(6), T(7)), "字段内：可删");
            expectTrue(EditPolicy::canErase(cs, EditMode::Form, T(5), T(6)), "字段首字：可删");
            expectTrue(!EditPolicy::canErase(cs, EditMode::Form, T(4), T(6)), "从字段外删进来：拒");
            expectTrue(!EditPolicy::canErase(cs, EditMode::Form, T(7), T(10)),
                       "删除区间伸出字段外：拒（两个端点都查，才挡得住这种）");
            expectTrue(EditPolicy::canErase(cs, EditMode::Document, T(7), T(10)),
                       "同一个操作在文档模式下允许 —— 差异全部来自策略这一个开关");

            expectTrue(EditPolicy::canPlaceCaret(cs, EditMode::Form, T(9)), "光标可停在字段末尾");
            expectTrue(!EditPolicy::canPlaceCaret(cs, EditMode::Form, T(10)),
                       "光标不能停在字段外一位");
            expectTrue(EditPolicy::canPlaceCaret(cs, EditMode::Document, T(10)),
                       "文档模式：随便停");
        }

        // 嵌套：内层策略优先
        {
            ControlSet cs;
            cs.add(mk(5, 11, "outer", false));
            cs.add(mk(9, 11, "inner", true));
            cs.normalize();
            expectTrue(cs.at(1).parent == 0 && cs.at(1).depth == 1,
                       "内层控件的 parent / depth 建对了（区间树）");
            expectTrue(cs.at(0).children.size() == 1 && cs.at(0).children[0] == 1,
                       "外层的 children 里挂着内层");
            expectTrue(cs.innermostAt(T(6), true) == 0, "位置 6 只被外层覆盖 → 最内层是外层");
            expectTrue(cs.innermostAt(T(9), true) == 1, "位置 9 被内外层同时覆盖 → 取最内层");
            expectTrue(EditPolicy::canInsert(cs, EditMode::Form, T(6)), "外层可编辑区域：允许");
            expectTrue(!EditPolicy::canInsert(cs, EditMode::Form, T(9)),
                       "内层只读区域：拒 —— 这就是「嵌套」的全部含义（内层策略优先）");
            expectTrue(!EditPolicy::canInsert(cs, EditMode::Form, T(10)), "内层覆盖的 10 也拒");
        }

        // 安全阀 + 只读模式
        {
            ControlSet none;
            expectTrue(EditPolicy::canInsert(none, EditMode::Form, T(3)),
                       "一个控件都没配时，表单模式退化为可编辑（不是突然打不进一个字）");

            ControlSet cs;
            cs.add(mk(5, 9, "k1", false));
            cs.normalize();
            expectTrue(!EditPolicy::canInsert(cs, EditMode::ReadOnly, T(6)),
                       "只读模式：字段内也不许插");
            expectTrue(EditPolicy::canPlaceCaret(cs, EditMode::ReadOnly, T(0)),
                       "只读模式：光标仍能自由移动（只读约束的是编辑，不是浏览）");
        }

        // 引擎层
        EditorEngine ef;
        if (!ef.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败：%s\n", cjkPath.c_str());
            return 2;
        }
        EngineConfig cfgf;
        cfgf.zoom = 1.5f;
        ef.setConfig(cfgf);

        BlockSpec bf;
        bf.font_index = 0;
        bf.size_pt = 12.0f;
        bf.wm = WritingMode::HorizontalTB();
        bf.line_factor = 1.75f;
        bf.language = "zh";
        bf.setText("患者主诉：反复心悸三天，加重一天。");
        ef.setBlocks({bf});
        ef.relayout();
        ef.repaint();

        const size_t LEN0 = ef.blocks()[0].cp_text.size();
        std::printf("     测试文本 %zu 个码点\n", LEN0);
        expectTrue(LEN0 == 17, "测试文本 17 个码点（后面所有区间下标都以它为准）");

        // (a) 没配控件 / 没切模式 → 旧行为一字不变
        {
            const long long b0 = ef.blockedEdits();
            ef.setCaret(0, 2);
            expectTrue(ef.typeTextAndRender("X"), "文档模式下任意位置都能插入");
            expectTrue(ef.backspaceAndRender(), "退格把它删掉");
            expectTrue(ef.blocks()[0].cp_text.size() == LEN0, "回到原长度");
            expectTrue(ef.blockedEdits() == b0,
                       "文档模式一次都没被拒 —— 策略层对旧行为是完全透明的");
        }

        // (b) "显示才占位"：光标不在字段里 → 不占位；进了字段 → 把正文挤开
        {
            ef.setCaret(0, 0);  // 文档开头 —— 在任何字段之外
            const float end_none =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;
            const int glyphs_none = ef.stats().glyph_count;

            ef.setControls({mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
            const float end_away =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;

            ef.setEditMode(EditMode::Form);
            // 表单模式会把光标夹进最近的字段 → 恰好就是"光标进了字段"
            const float end_in =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;

            std::printf(
                "     首行宽：无控件 %.2f pt ｜ 有控件但光标在字段外 %.2f pt ｜ "
                "光标在字段内 %.2f pt（字形始终 %d 个）\n",
                end_none, end_away, end_in, ef.stats().glyph_count);

            expectTrue(std::fabs(end_away - end_none) < 0.01f,
                       "光标不在字段里 → 括号不显示 → 不占位（行宽与没有控件时一模一样）");
            expectTrue(end_in > end_away + 1.0f,
                       "光标进入字段 → 括号显示 → 占位（首行宽变大，正文被挤开了）");
            expectTrue(ef.stats().glyph_count == glyphs_none,
                       "括号不产生字形（占位簇 g0==g1，墨迹数量一个都没变）");
            expectTrue(ef.editMode() == EditMode::Form && ef.controls().size() == 2,
                       "已切到表单模式，2 个字段");
        }

        // (b2) 占位必须体现在画出来的字形上，而不只是行盒的几何数字。
        //
        //   这条断言的存在理由（一个真实且不易察觉的缺陷）：
        //     行盒的簇位置来自 cluster_adv_pt（含占位宽度），而字形位置
        //     只用 pen_x += sg.x_advance 一个个字形累加 —— 占位簇没有字形
        //     （g0 == g1），那个循环一次都不执行，pen_x 在它身上纹丝不动。
        //     于是形成一劈两半的状态：
        //         光标 / 控件矩形 / 命中测试  → 按"文字让开了"算
        //         真正画出来的字              → 根本没让开
        //     表象就是方括号压在文字上。
        //
        //   为什么上面 (b) 那条抓不到它：它只查 inline_end_pt，
        //     而 inline_end_pt 就是 cluster_adv_pt 的和 —— 那是"行盒认为的宽度"，
        //     与"字形实际画在哪"是两套数。断言只查了前者，所以永远是绿的。
        //     教训：几何元数据与绘制结果必须分别断言，不能拿前者替后者。
        {
            auto glyphX = [](EditorEngine& eng, uint32_t cp) {
                float best = 1e9f;
                for (const PlacedGlyph& g : eng.layout().glyphs) {
                    if (g.vertical || g.paragraph != 0 || g.cluster != cp) continue;
                    best = std::min(best, g.x_pt);
                }
                return best;
            };

            ef.setControls({});                   // 退回"没有表单"的排版
            const float x_sick0 = glyphX(ef, 0);  // 患 —— 字段之前，不该动
            const float x_fan0 = glyphX(ef, 5);   // 反 —— 字段第一个字，该让 1 个括号
            const float x_san0 = glyphX(ef, 9);   // 三 —— 字段之后，该让 2 个括号
            const float x_per0 = glyphX(ef, 16);  // 。 —— 行尾，该让 2 个括号

            // 默认档（FormBracketScope::All）下两个字段都占位 ——
            //   所以 #0 之后 +2 槽、#1 之后 +4 槽。这正是"括号一直显示"的代价：
            //   正文被多推开一对括号。不要再按"只有聚焦字段占位"去写期望值。
            ef.setControls({mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
            ef.setEditMode(EditMode::Form);  // 光标被夹进 k1；但两个字段都显示括号
            const float x_sick1 = glyphX(ef, 0);
            const float x_fan1 = glyphX(ef, 5);
            const float x_san1 = glyphX(ef, 9);
            const float x_per1 = glyphX(ef, 16);

            const float bw = x_fan1 - x_fan0;
            std::printf(
                "     字形 x 位移：患 %.3f→%.3f（Δ%.3f）｜ 反 %.3f→%.3f（Δ%.3f）｜ "
                "三 Δ%.3f ｜ 。 Δ%.3f ｜ 括号宽 %.3f pt\n",
                x_sick0, x_sick1, x_sick1 - x_sick0, x_fan0, x_fan1, bw, x_san1 - x_san0,
                x_per1 - x_per0, bw);

            expectTrue(std::fabs(x_sick1 - x_sick0) < 0.01f,
                       "字段之前的字形一动不动（患 Δ=0）—— 占位是局部的，不是整体平移");
            // 位移量是一个括号槽，不是括号字形的 advance：
            //   槽 = 括号 advance（本字体 12pt 下实测 4.055pt）
            //      + 排版留的间隙 kControlBracketSlotGapPt。
            //   间隙也是宽度，必须算进来 —— 不然后面那句 2*bw 会跟着错。
            const float one_slot = 4.055f + kControlBracketSlotGapPt;
            expectTrue(bw > 1.0f && std::fabs(bw - one_slot) < 0.6f,
                       "字段第一个字整体右移了恰好一个括号槽（反 Δ≈5.56pt）"
                       "—— 这条一旦是 0，就是「排版算了占位、画字没算」，"
                       "表象为方括号压在文字上");
            // 必须同时要求"真的动了"和"动的量恰好是两个括号宽"。
            //   只写 fabs(Δ - 2*bw) 会退化：当绘制与几何一起坏掉（bw == 0）时
            //   fabs(0 - 0) < 0.05 恒成立，断言照样绿 —— 那是假绿。
            //   （真命中过：注入 bug 后这两条没红，只有"反"那两条红了。）
            expectTrue((x_san1 - x_san0) > 1.0f && std::fabs((x_san1 - x_san0) - 2.0f * bw) < 0.05f,
                       "字段之后的字移了两个括号宽（三 Δ≈8.11pt）");
            // 「。」在两个字段之后 → 让 4 个槽（每个字段一对括号）。
            // 这条同时是"默认档是 All（全部字段占位）"的探测器：
            //   改回 Focused 档，这里会变成 2 个槽而红。
            expectTrue((x_per1 - x_per0) > 1.0f && std::fabs((x_per1 - x_per0) - 4.0f * bw) < 0.05f,
                       "行尾那个「。」移了四个括号宽 —— 两个字段各一对括号都占位"
                       "（默认档 All：括号常驻，不是只有聚焦字段占位）");

            // 最强的一条不变式：画出来的字必须正好从字段矩形的左缘开始。
            //   它把"两套数"这件事直接钉死 —— 左边是绘制结果，右边是几何元数据，
            //   两边一旦各算各的，这条必红（正是上面那个 bug 的形状）。
            const std::vector<EditorEngine::ControlRect> r0b = ef.controlRects(0);
            expectTrue(!r0b.empty() && std::fabs(x_fan1 - r0b[0].r_pt.x) < 0.02f,
                       "字形「反」的 x == 字段 #0 矩形左缘（绘制与几何同一把尺子）");

            // (b3) 占位只属于表单模式
            //   文档模式下标着同一份控件、光标也在字段里，但括号根本不画 ——
            //   那就不该占宽度。否则会凭空多出一段空隙，谁也没要，
            //   而且很难发现（正文看起来"字距有点怪"，不像个 bug）。
            ef.setEditMode(EditMode::Document);
            ef.setCaret(0, 6);  // 光标仍在 k1 里（控件还挂着）
            const float x_doc = glyphX(ef, 5);
            ef.setEditMode(EditMode::Form);
            const float x_form = glyphX(ef, 5);
            std::printf("     文档模式字形 x=%.3f（无表单基线 %.3f）｜ 表单模式 %.3f\n", x_doc,
                        x_fan0, x_form);
            expectTrue(std::fabs(x_doc - x_fan0) < 0.01f,
                       "文档模式下不占位：光标即使落在字段里，字形也回到无表单时的位置");
            expectTrue(x_form - x_doc > 1.0f,
                       "切回表单模式立刻占位（证明\"切模式会重排\"确实接上了）");
        }

        // (c) 区间外拒、区间内允、区间随内容伸缩
        {
            const long long b0 = ef.blockedEdits();
            const size_t len0 = ef.blocks()[0].cp_text.size();

            // "区间外"必须用直接给位置的接口（insertTextUtf8）来测。
            //   不能用 setCaret + typeText：setCaret 在表单模式下会把光标
            //   吸附到最近的字段里（那是另一条要测的行为），于是"区间外"
            //   这个位置根本到不了 —— 看起来断言失败，其实是被测路径选错了。
            expectTrue(!ef.insertTextUtf8(0, 2, "X"), "字段外（offset 2）插入被拒");
            expectTrue(ef.blocks()[0].cp_text.size() == len0, "文档长度一个码点都没变");
            expectTrue(ef.blockedEdits() == b0 + 1,
                       "这次拒绝被记了账（不是静默失败 —— 静默失败和「程序卡了」无法区分）");

            ef.setCaret(0, 2);
            expectTrue(ef.caret().offset == 5,
                       "表单模式下 setCaret(2) 被吸附到最近的字段起点 5（光标出不去）");

            ef.setCaret(0, 6);
            expectTrue(ef.typeTextAndRender("X"), "字段内插入被允许");
            expectTrue(ef.blocks()[0].cp_text.size() == len0 + 1, "长度 +1");
            expectTrue(ef.controls().at(0).range.end.offset == 10,
                       "控件的 end 跟着长：9 → 10（区间是活的，不是死矩形）");
            expectTrue(ef.blockedEdits() == b0 + 1, "被允许的那次不记账");

            expectTrue(ef.backspaceAndRender(), "字段内退格成功");
            expectTrue(ef.controls().at(0).range.end.offset == 9, "退格后 end 缩回 9");
            expectTrue(ef.blocks()[0].cp_text.size() == len0, "长度退回");

            ef.setCaret(0, 5);
            expectTrue(!ef.backspaceAndRender(), "在字段开头退格 → 拒（不许从字段里删到字段外）");
            expectTrue(ef.blocks()[0].cp_text.size() == len0, "长度仍然不变");
        }

        // (d) 方向键不跨控件：一步一格，走不动就停
        //
        //   语义纠偏（用户提出）：这里写着"跨过字段外的 10~11，落到下一个
        //     字段起点 12" —— 那正是"光标移动跨两个字符"。
        //     一个方向键指令只该做一件事：往那个方向走一格；走不过去就停住，
        //     不能自作主张投影到另一个控件里。
        //   代价（说清楚）：字段之间不再能靠方向键走过去 —— 中间不是可停位置。
        //     要进另一个字段就点鼠标。这是"不跨控件"的必然结果。
        {
            ef.setCaret(0, 8);
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 9,
                       "右移到字段末尾 9 —— 末尾可以停（否则没法在末尾追加）");
            expectTrue(!ef.moveCaretAndRender(1),
                       "再右移：原地不动（10 已经在只读字段 #1 里，不是可停位置）");
            expectTrue(ef.caret().offset == 9, "光标仍停在 9 —— 没有跨到下一个字段去");
            expectTrue(ef.moveCaretAndRender(-1) && ef.caret().offset == 8,
                       "左移回到 8：一步一格，正常");

            // 那一下：在 #2 起点按左键，不许跨过只读字段 #1 落到 #0
            ef.setCaret(0, 12);
            expectTrue(!ef.moveCaretAndRender(-1),
                       "在 #2 起点左移：原地不动（11/10 在只读字段里，9 在另一个字段）");
            expectTrue(ef.caret().offset == 12 && ef.focusedControlIndex() == 1,
                       "光标仍在 #2 的起点 12 —— 没有跨控件");

            ef.setCaret(0, 16);
            expectTrue(!ef.moveCaretAndRender(1),
                       "在最后一个字段末尾右移：原地不动（后面还有正文，但去不了）");
            expectTrue(ef.caret().offset == 16, "光标仍停在 16");

            // 穷尽：在 #2 里左右各走一趟，每一步的偏移跨度必须 ≤ 1，
            //   而且走过的位置必须全部落在字段区间内。这一条把"瞬移"整类杀掉。
            {
                ef.setCaret(0, 12);
                int worst = 0;
                bool inside = true;
                for (int i = 0; i < 8; ++i) {
                    const size_t b = ef.caret().offset;
                    ef.moveCaretAndRender(1);
                    const size_t a = ef.caret().offset;
                    const int d = static_cast<int>(a) - static_cast<int>(b);
                    if (d > worst) worst = d;
                    if (a < 12 || a > 16) inside = false;
                }
                for (int i = 0; i < 8; ++i) {
                    const size_t b = ef.caret().offset;
                    ef.moveCaretAndRender(-1);
                    const size_t a = ef.caret().offset;
                    const int d = static_cast<int>(b) - static_cast<int>(a);
                    if (d > worst) worst = d;
                    if (a < 12 || a > 16) inside = false;
                }
                std::printf(
                    "     [诊断] #2 内左右各走 8 步：单步最大跨度 %d（应为 1），"
                    "始终在字段内=%d\n",
                    worst, inside ? 1 : 0);
                expectTrue(worst == 1, "左右各走 8 步，单步跨度恒为 1（没有任何一步瞬移）");
                expectTrue(inside, "走完一趟，光标从未离开 #2 = [12,16)");
            }

            // Home/End 同一条原则：不跨控件
            ef.setCaret(0, 14);
            expectTrue(ef.moveCaretToLineStartAndRender() && ef.caret().offset == 12,
                       "Home 退到本字段起点 12（不是行首 0，更不是别的字段的 5）");
            expectTrue(ef.moveCaretToLineEndAndRender() && ef.caret().offset == 16,
                       "End 到本字段终点 16（不是行尾 17）");
            expectTrue(!ef.moveCaretToLineStartAndRender() || ef.caret().offset == 12,
                       "再按 Home 停在 12，不会掉进别的字段");
        }

        // (e) Tab = 缩进（不是跳字段）
        //
        //   语义纠偏：Tab 在编辑器里是缩进。绑成"跳到下一个控件"是
        //     表单/网页里字段之间的约定，放到正文编辑器里会把肌肉记忆打断 ——
        //     用户明确纠正过一次。字段之间用鼠标点或者方向键（光标本来就会跨字段）。
        //   最硬的一条是"逐字节等价"：缩进必须和手打 4 个空格得到完全
        //     相同的帧与相同的文档 —— 否则它就是一条绕过策略/记账的特权通道。
        {
            auto frameOf = [](EditorEngine& eng) {
                eng.repaint();
                return std::vector<uint8_t>(eng.frame(), eng.frame() + eng.frameBytes());
            };
            auto sameFrame = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
                return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
            };

            ef.setEditMode(EditMode::Document);
            const size_t N0 = ef.blocks()[0].cp_text.size();  // 17

            // Tab 缩进：在光标处插 4 个空格，光标停在其后
            ef.setCaret(0, 2);
            expectTrue(ef.indentAndRender(), "Tab 缩进返回 true");
            const std::vector<uint8_t> f_tab = frameOf(ef);
            const size_t len_tab = ef.blocks()[0].cp_text.size();
            std::printf("     [诊断] 缩进后 长度 %zu→%zu，光标 %zu\n", N0, len_tab,
                        ef.caret().offset);
            expectTrue(len_tab == N0 + 4 && ef.caret().offset == 6,
                       "Tab 在光标处插入 4 个空格，光标停在其后");

            // 手打 4 个空格：必须得到逐字节相同的帧
            //   先把文档恢复，保证两条路的起点完全一致（不然比的是两次插入的叠加）。
            expectTrue(ef.outdentAndRender() && ef.blocks()[0].cp_text.size() == N0,
                       "反缩进先把文档恢复（下面才能公平对比）");
            ef.setCaret(0, 2);
            expectTrue(ef.typeTextAndRender("    "), "手打 4 个空格");
            const std::vector<uint8_t> f_sp = frameOf(ef);
            expectTrue(ef.blocks()[0].cp_text.size() == len_tab, "两条路的文档长度一致");
            expectTrue(sameFrame(f_tab, f_sp),
                       "Tab 缩进的帧 == 手打 4 个空格的帧（逐字节相同）"
                       "—— 缩进不是特权通道，策略/区间伸缩/记账全走同一条路");

            // Shift+Tab 反缩进
            expectTrue(ef.outdentAndRender() && ef.blocks()[0].cp_text.size() == N0,
                       "Shift+Tab 反缩进：把 4 个空格删回来");
            expectTrue(!ef.outdentAndRender(), "光标前没有空格时反缩进返回 false（不误删正文）");

            // 缩进走的是正常编辑路径：字段内的策略对缩进一样生效
            ef.setControls({mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
            ef.setEditMode(EditMode::Form);
            const long long b_tab = ef.blockedEdits();
            ef.setCaret(0, 6);                                         // 在 k1 里
            const size_t end0 = ef.controls().at(0).range.end.offset;  // 9
            expectTrue(ef.indentAndRender(), "字段内 Tab 缩进：允许（和敲字同一条策略）");
            std::printf("     [诊断] 字段内缩进：长度 %zu→%zu，k1 end %zu→%zu，光标 %zu\n", N0,
                        ef.blocks()[0].cp_text.size(), end0, ef.controls().at(0).range.end.offset,
                        ef.caret().offset);
            expectTrue(ef.controls().at(0).range.end.offset == end0 + 4,
                       "缩进也把字段区间撑长 4（区间是活的，缩进不是例外）");
            for (int i = 0; i < 4; ++i) ef.backspaceAndRender();
            expectTrue(
                ef.blocks()[0].cp_text.size() == N0 && ef.controls().at(0).range.end.offset == end0,
                "四次退格把缩进删回去，文档与区间都回到原样");
            expectTrue(ef.blockedEdits() == b_tab, "整个过程一次都没被拒（缩进没有绕过策略）");
        }

        // (e2) 「改状态却不收尾」是这一整类"画布新旧混合"bug 的总根。
        //
        //   不变式：*AndRender 系列入口返回之后，编辑统计里的光标必须等于真实光标。*
        //   统计是在 presentInternal 里写的，所以"统计跟得上" ⟺ "已经收尾"。
        //
        //   这条断言来自真机现象：Tab 跳字段后，旧字段的方括号留在屏幕上、
        //     右边半截渲染也不对。根因是 focusControl 只改状态不重绘，
        //     而 JS 的 Tab 处理紧接着就去读 editStatsJson() 拿脏矩形上屏 ——
        //     读到的是上一次呈现的账，于是画布只被贴了一小块，
        //     变成"引擎帧是新的、屏幕上是新旧混合"。
        {
            ef.setCaret(0, 5);  // 回到 k1 里
            ef.repaint();       // 先让统计与状态对齐
            const size_t off_before = ef.editStats().caret_offset;
            const bool moved = ef.focusControl(true);
            const size_t off_after = ef.editStats().caret_offset;
            std::printf("     focusControl 后光标 %zu（统计里仍是 %zu）\n", ef.caret().offset,
                        off_after);
            expectTrue(moved && ef.caret().offset == 12, "focusControl 真的把光标挪到了下一个字段");
            expectTrue(off_after == off_before,
                       "只改状态的 focusControl 不会更新统计 —— "
                       "这就是宿主拿到过期脏矩形的来源（不是缺陷，是它没承诺收尾）");
            ef.focusControlAndRender(false);
            expectTrue(ef.editStats().caret_offset == ef.caret().offset,
                       "focusControlAndRender 必须把统计刷成当前状态"
                       "（== 它自己收尾了；删掉那行 repaint() 这条立刻红）");
        }

        // (f) 标记必须真的画出来、跟着光标走，切回去要真的擦掉
        {
            auto snapshot = [](EditorEngine& eng) {
                eng.repaint();
                return std::vector<uint8_t>(eng.frame(), eng.frame() + eng.frameBytes());
            };
            // 判"这个像素属于控件标记"（亮蓝系，含抗锯齿混色）。
            //
            // 为什么不用精确色相等：括号现在是字形，笔画边缘大量是"部分覆盖"
            //   的中间色，精确匹配只能数到 3~4 个像素 —— 那样的断言脆得没法用。
            //   判据三条：
            //     偏蓝够远（b - r > 60）→ 排除所有灰阶（正文黑字、页边框、背景）
            //     蓝够亮（b > 140）→ 排除深色
            //     红通道 > 45 → 把光标排除掉：光标色 (31,111,235) 的 r=31，
            //       标记色 (56,139,253) 的 r=56。这两者是刻意分开的，就靠 r 的差。
            auto isMark = [](uint8_t r, uint8_t /*g*/, uint8_t b) {
                return r > 45 && (static_cast<int>(b) - static_cast<int>(r)) > 60 && b > 140;
            };
            auto countCols = [&isMark](const std::vector<uint8_t>& px, int fw, int x0, int x1) {
                size_t n = 0;
                for (size_t i = 0; i + 3 < px.size(); i += 4) {
                    const int x = static_cast<int>(i >> 2) % fw;
                    if (x < x0 || x > x1) continue;
                    if (isMark(px[i], px[i + 1], px[i + 2])) ++n;
                }
                return n;
            };

            const float zoom = ef.config().zoom;
            const int fw = ef.frameWidth();
            // 本测试只配了 2 个控件：#0 = [5,9)「反复心悸」，#1 = [12,16)「加重一天」
            //
            // 窗口一律当场现算（不缓存矩形）：All 档下括号常驻、矩形确实不随
            //   光标动，但"不随光标动"本身就是要验的东西 —— 缓存下来等于把它假设掉了。
            //   （Focused 档下光标进出字段会让整行重排、矩形差一个括号宽，
            //     拿旧值算窗口会差出几像素。）
            auto bandLeft = [&](int ci) -> std::pair<int, int> {
                const auto r = ef.controlRects(ci);
                if (r.empty()) return {0, -1};
                return {static_cast<int>(std::lround((r[0].r_pt.x - 10.0f) * zoom)),
                        static_cast<int>(std::lround((r[0].r_pt.x - 1.0f) * zoom))};
            };
            auto bandRight = [&](int ci) -> std::pair<int, int> {
                const auto r = ef.controlRects(ci);
                if (r.empty()) return {0, -1};
                return {static_cast<int>(std::lround((r[0].r_pt.x + r[0].r_pt.w + 1.2f) * zoom)),
                        static_cast<int>(std::lround((r[0].r_pt.x + r[0].r_pt.w + 14.0f) * zoom))};
            };
            auto inkIn = [&](const std::vector<uint8_t>& px, std::pair<int, int> b) {
                return countCols(px, fw, b.first, b.second);
            };

            expectTrue(!ef.controlRects(0).empty() && !ef.controlRects(1).empty(),
                       "控件 #0 / #1 都投影出了矩形");

            // 默认档：括号常驻，两个字段同时都有、且不随光标变化
            //
            //   这一条是被用户一句话定下来的：「括号应该一直在，
            //   不然不知道这块可以输入了」。
            //   旧行为（Focused 档）是"光标不在就只画一个小点"——
            //   光标一移开，那个字段看上去就不像字段了。
            {
                ef.setCaret(0, 6);  // 在控件 #0 里
                const std::vector<uint8_t> f = snapshot(ef);
                const size_t a_left0 = inkIn(f, bandLeft(0));
                const size_t a_left1 = inkIn(f, bandLeft(1));

                ef.setCaret(0, 13);  // 在控件 #1 里
                const std::vector<uint8_t> g = snapshot(ef);
                const size_t b_left0 = inkIn(g, bandLeft(0));
                const size_t b_left1 = inkIn(g, bandLeft(1));

                std::printf(
                    "     光标在 #0：左缘像素 #0=%zu / #1=%zu ｜ "
                    "光标在 #1： #0=%zu / #1=%zu\n",
                    a_left0, a_left1, b_left0, b_left1);
                expectTrue(a_left0 > 0 && a_left1 > 0,
                           "光标在 #0 时，两个字段的左括号都在（常驻，不看光标）");
                expectTrue(b_left0 > 0 && b_left1 > 0,
                           "光标移到 #1 时，两个左括号仍然都在（没有跟光标走）");
                expectTrue(a_left0 == b_left0 && a_left1 == b_left1,
                           "括号的墨迹量与光标位置无关（常驻 ⟺ 帧里那几笔不随光标变）");
            }

            ef.setEditMode(EditMode::Document);
            const std::vector<uint8_t> f_doc = snapshot(ef);
            const size_t doc_mark = countCols(f_doc, fw, 0, fw - 1);
            expectTrue(doc_mark == 0, "切回文档模式：标记一个像素都不剩");

            ef.setEditMode(EditMode::Form);
            const std::vector<uint8_t> f_form = snapshot(ef);
            const size_t form_mark = countCols(f_form, fw, 0, fw - 1);
            expectTrue(form_mark > 0, "切回表单模式：标记又出现了（不是「切走一次就再也不画」）");

            // 回归：光标停在字段末尾时，右括号必须还在。
            //
            //   它和光标矩形重叠（光标中心正好压在字段右边缘上）。
            //   画光标之前有一次 copyRectFrom(base_, cr)，会把落在光标矩形内的
            //   标记一起抹掉 —— 而 control_marks_prev_ 已经把它记成"画过了"，
            //   于是每次重画又被抹一次，永远补不上。"括号渲染不更新"就是它。
            //   现在：擦旧光标只在 refresh 里做一次，标记又画在光标之后，不会被盖。
            //
            //   取"右边缘外侧"这条竖带：右括号在那儿，小点在右边缘内侧。
            ef.setCaret(0, 9);  // 控件 #0 = [5,9) 的末尾 —— 光标紧挨右括号
            const size_t atEnd = inkIn(snapshot(ef), bandRight(0));
            ef.setCaret(0, 13);  // 光标在 #1 里 —— 右括号照样在（常驻）
            const size_t awayEnd = inkIn(snapshot(ef), bandRight(0));

            std::printf("     控件 #0 右缘外侧：光标在 #0 末尾 %zu px ／ 光标在 #1 时 %zu px\n",
                        atEnd, awayEnd);
            expectTrue(atEnd > 0 && awayEnd > 0,
                       "光标停在字段末尾时右括号仍在（重叠时画序错了会被抹掉），"
                       "光标离开该字段后它也在（常驻）");

            // (f5) Focused 档回归：非聚焦字段回到"只有右下角小点"
            //
            //   这一档是旧行为，也是被真机验证过的那一版，不能因为换了默认值就失修。
            //   判据和旧版一致：聚焦字段的左缘外侧有括号，其余字段为空。
            {
                EngineConfig cf = ef.config();
                cf.form_brackets = FormBracketScope::Focused;
                ef.setConfig(cf);

                // 窗口必须按帧分别算：Focused 档下光标进出字段会让整行重排，
                //   控件矩形跟着平移一个括号宽 —— 拿上一帧的窗口去数，量到的就是
                //   旁边那块像素。
                ef.setCaret(0, 6);  // 在 #0 里
                const std::vector<uint8_t> f = snapshot(ef);
                const size_t a0 = inkIn(f, bandLeft(0));
                const size_t a1 = inkIn(f, bandLeft(1));

                ef.setCaret(0, 13);  // 在 #1 里 → 标记该换边
                const std::vector<uint8_t> g = snapshot(ef);
                const size_t b0 = inkIn(g, bandLeft(0));
                const size_t b1 = inkIn(g, bandLeft(1));

                std::printf(
                    "     Focused 档：光标在 #0 → #0=%zu／#1=%zu ；"
                    "光标在 #1 → #0=%zu／#1=%zu\n",
                    a0, a1, b0, b1);
                expectTrue(a0 > 0 && a1 == 0,
                           "Focused 档：光标在 #0 → 只有 #0 有括号，#1 左缘为空（只有小点）");
                expectTrue(b0 == 0 && b1 > 0, "Focused 档：光标移到 #1 → 括号跟着搬过去");

                EngineConfig ca = ef.config();
                ca.form_brackets = FormBracketScope::All;  // 回默认档，后面几节都按它
                ef.setConfig(ca);
            }

            // 穷尽不变式：光标走遍每一个可停位置，每一步都检查每个字段的括号 ——
            //     All 档下：每个可编辑字段的左缘外侧都必须有括号，而且墨迹量恒定
            //     （恒定 = 常驻；只要哪一步漏画/多画，max-min 立刻不为 0）。
            //
            //   为什么值得穷尽：是"有时括号渲染不更新"。
            //   单点断言（只试一两个位置）抓不到"某个位置刚好漏掉"，
            //   而这类漏掉几乎总是由"某个矩形刚好和光标重叠、被后画的东西盖掉"
            //   造成的 —— 只有走遍位置才碰得到。这一条是那条 bug 的探测器。
            {
                int checked = 0, bad = 0;
                // min/max 必须按字段分别统计：两个字段的括号墨迹量本来就不同
                //   （不同字形、不同亚像素相位），混在一起算会让这条恒为假。
                const int nc = ef.controls().size();
                std::vector<size_t> lo(static_cast<size_t>(nc), 1u << 30);
                std::vector<size_t> hi(static_cast<size_t>(nc), 0);
                const int total = static_cast<int>(ef.blocks()[0].cp_text.size());
                for (int off = 0; off <= total; ++off) {
                    ef.setCaret(0, off);
                    const std::vector<uint8_t> f = snapshot(ef);
                    for (int ci = 0; ci < nc; ++ci) {
                        if (ef.controls().at(ci).read_only) continue;
                        const auto b = bandLeft(ci);
                        if (b.second < b.first) continue;  // 没有矩形（空字段）→ 跳过
                        const size_t n = countCols(f, fw, b.first, b.second);
                        ++checked;
                        if (n == 0) ++bad;
                        lo[static_cast<size_t>(ci)] = std::min(lo[static_cast<size_t>(ci)], n);
                        hi[static_cast<size_t>(ci)] = std::max(hi[static_cast<size_t>(ci)], n);
                    }
                }
                std::printf(
                    "     穷尽不变式：%d 项检查，不符 %d 项；"
                    "墨迹量 #0 min=%zu max=%zu ｜ #1 min=%zu max=%zu\n",
                    checked, bad, lo[0], hi[0], lo[1], hi[1]);
                expectTrue(checked > 20 && bad == 0,
                           "光标走遍全部可停位置，每个字段的括号始终在（All 档常驻）");
                expectTrue(lo[0] > 0 && lo[0] == hi[0] && lo[1] > 0 && lo[1] == hi[1],
                           "每个字段括号的墨迹量与光标位置逐像素一致（常驻的硬判据）");
            }

            // (f6) 那个 bug：把字段内容删光，括号不能消失
            //
            //   原话：「文本控件删除最后一个字符后，括号也不渲染了，但是好像括号
            //   位置还占着，输入一个字符后括号出现」。
            //
            //   根因：老的绘制层是按控件矩形遍历的，而空区间（start == end）
            //   投影不出矩形 → 一个标记都画不出来；可占位槽照插，
            //   于是"位置还占着、却什么都看不见"。
            //   现在绘制从占位簇派生：槽在哪就画在哪，和有没有字形无关。
            {
                // 删之前先量一下同一个窗口（同一个括号字形、同一个位置 → 应当一致）
                const size_t before = inkIn(snapshot(ef), bandLeft(0));

                // 必须用 backspaceAndRender（编辑+重排+呈现的三合一入口）：
                //   裸 backspace() 只改文档，不重排 —— 接着去读 layout 会拿到
                //   旧的那份（本次真命中了：占位槽还是删之前那 4 个 cp，
                //   于是"数到 4 个槽"、并且按旧坐标去找右括号当然找不到）。
                //   宿主的真实路径也是前者，用它才测得准。
                ef.setCaret(0, 9);
                for (int i = 0; i < 4; ++i) ef.backspaceAndRender();  // 把「反复心悸」删光
                std::printf("     删空字段后：文档 %zu 个码点（原 17）\n",
                            ef.blocks()[0].cp_text.size());
                expectTrue(ef.blocks()[0].cp_text.size() == 13,
                           "前置：字段内容真的被删光了（17 - 4 = 13）");
                expectTrue(ef.controlRects(0).empty(),
                           "前提：空字段投影不出矩形 —— 老实现就是死在这一步"
                           "（括号没得画，占位却还在）");

                // 占位槽 + 墨迹：直接从排版里找那两个槽（不看控件矩形）。
                //
                // 只数空字段自己的那两个槽（cp == 5）。整篇里还有另一个字段的
                //   两个槽（cp=8/12），一起数就会把"4"当成"2"—— 这一步必须按 cp 认人，
                //   因为空字段的两个槽落在同一个 cp 上（这正是它"没有内容"的形状）。
                float open_pt = -1.0f;
                int slots = 0;
                int empty_side_first = -1, empty_side_second = -1;
                for (const LineBox& lb : ef.layout().lines) {
                    if (lb.paragraph != 0) continue;
                    for (const LineCluster& c : lb.clusters) {
                        if (!c.placeholder) continue;
                        const float x_pt = lb.inline_origin_pt + c.inline_pt;
                        std::printf("       槽：cp=%u side=%d x=%.2f pt\n", c.cp,
                                    static_cast<int>(c.bracket_side), x_pt);
                        if (c.cp != 5) continue;  // 只认空字段那个边界
                        ++slots;
                        if (empty_side_first < 0)
                            empty_side_first = static_cast<int>(c.bracket_side);
                        else
                            empty_side_second = static_cast<int>(c.bracket_side);
                        if (c.bracket_side == BracketSide::Open) open_pt = x_pt;
                    }
                }
                expectTrue(slots == 2, "空字段仍有两个占位槽（开+闭）—— 位置确实还占着");
                // 空字段的一对必须是 [开, 闭]（[]）—— 它和"相邻字段"是
                //   相反的顺序（那里是 ][）。两条一起把优先级规则钉死：
                //   顺序取决于"槽属于谁"，不是"开还是闭"。
                expectTrue(empty_side_first == static_cast<int>(BracketSide::Open) &&
                               empty_side_second == static_cast<int>(BracketSide::Close),
                           "空字段自己的两个槽是 [开, 闭]（`[]`），与相邻字段的 `][` 相反");

                const std::vector<uint8_t> f = snapshot(ef);
                const float slot = 4.055f + kControlBracketSlotGapPt;  // 槽宽（参考值）
                const int x_open = static_cast<int>(std::lround(open_pt * zoom));
                const int x_close = static_cast<int>(std::lround((open_pt + slot) * zoom));
                // 闭括号的字形贴槽尾（间隙在内侧），所以它的墨迹从
                //   「闭槽首 + 间隙」开始 —— 窗口要按这个来取，否则数到的是空的。
                const int x_close_glyph = static_cast<int>(
                    std::lround((open_pt + slot + kControlBracketSlotGapPt) * zoom));
                const size_t ink_open =
                    countCols(f, fw, x_open - 2, x_open + static_cast<int>(6.0f * zoom));
                const size_t ink_close = countCols(f, fw, x_close_glyph - 1,
                                                   x_close_glyph + static_cast<int>(6.0f * zoom));
                std::printf(
                    "     删空前同窗口墨迹 %zu px ｜ 删空后：开括号窗口 %zu px、"
                    "闭括号窗口 %zu px（开槽 x=%d，闭槽 x=%d）\n",
                    before, ink_open, ink_close, x_open, x_close);
                expectTrue(ink_open > 0 && before > 0,
                           "空字段的左括号照样画出来了（删之前/之后都在）");
                expectTrue(ink_close > 0, "空字段的右括号也画出来了（两个槽都填上了）");

                // 收尾：把内容打回去，后面 (g) 那些断言用的还是这段文本。
                // 同样走 *AndRender：它的语义就是"编辑一次并收尾"。
                ef.setCaret(0, 5);
                expectTrue(ef.typeTextAndRender("反复心悸"), "前置：把删掉的 4 个字打回去");
                expectTrue(ef.blocks()[0].cp_text.size() == 17 && !ef.controlRects(0).empty(),
                           "前置：文档与控件矩形都回到原样（后面几节依赖它）");
            }

            // 这两条（画出来 / 擦干净）同时钉死一个容易漏掉的失效点：
            //   标记画在帧上、不进底图，切模式时底图一个字都不变。
            //   如果忘了置 frame_needs_full_refresh_，切回文档模式后括号会残留
            //   —— 表象是"随机不刷新"，从代码上极难看出来的那种。
        }

        // (g) 区间 → 矩形：单行 1 块、跨行多块、命中测试
        {
            const std::vector<EditorEngine::ControlRect> r0 = ef.controlRects(0);
            std::printf("     字段 #0「反复心悸」→ %zu 个矩形块\n", r0.size());
            expectTrue(r0.size() == 1, "单行短字段 → 1 个矩形块");
            if (!r0.empty()) {
                std::printf("        page=%d x=%.2f y=%.2f w=%.2f h=%.2f pt\n", r0[0].page,
                            r0[0].r_pt.x, r0[0].r_pt.y, r0[0].r_pt.w, r0[0].r_pt.h);
                expectTrue(r0[0].r_pt.w > 40.0f && r0[0].r_pt.w < 60.0f,
                           "矩形宽 ≈ 4 个 12pt 汉字（48pt）—— 不是少了一个字的宽度");
                expectTrue(r0[0].r_pt.h > 10.0f, "矩形高 ≈ 行高");

                const float cx = r0[0].r_pt.x + r0[0].r_pt.w * 0.5f;
                const float cy = r0[0].r_pt.y + r0[0].r_pt.h * 0.5f;
                const float stride = ef.config().page.height_pt + ef.config().page_gap_pt;
                expectTrue(ef.controlAtPt(cx, cy + static_cast<float>(r0[0].page) * stride) == 0,
                           "点在字段矩形中心 → 命中该字段");
                expectTrue(ef.controlAtPt(cx, ef.config().page.height_pt +
                                                  ef.config().page_gap_pt * 0.5f) == -1,
                           "点在页间空隙 → 不命中任何字段");
            }
        }

        // 跨行：长文本上挂一个长区间 → 必须给多个矩形块
        {
            EditorEngine ew;
            if (!ew.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败：%s\n", cjkPath.c_str());
                return 2;
            }
            EngineConfig cw;
            cw.zoom = 1.5f;
            ew.setConfig(cw);
            BlockSpec bw;
            bw.font_index = 0;
            bw.size_pt = 12.0f;
            bw.wm = WritingMode::HorizontalTB();
            bw.line_factor = 1.75f;
            bw.language = "zh";
            std::string longtext;
            for (int i = 0; i < 4; ++i) longtext += "患者主诉：反复心悸三天，加重一天。";
            bw.setText(longtext);
            ew.setBlocks({bw});
            ew.setControls({mk(5, 60, "long", false)});
            ew.relayout();
            ew.repaint();

            const int lines = ew.stats().placed_line_count;
            const std::vector<EditorEngine::ControlRect> rl = ew.controlRects(0);
            std::printf("     跨行字段：文档排成 %d 行，区间 [5,60) → %zu 个矩形块\n", lines,
                        rl.size());
            expectTrue(lines >= 2, "长文档确实排成了多行（否则这条测试没有意义）");
            expectTrue(rl.size() >= 2, "跨行字段 → 多个矩形块 —— 这就是「控件不是矩形」的直接证据");
            if (rl.size() >= 2) {
                expectTrue(rl[1].r_pt.y > rl[0].r_pt.y, "后一块排在更下方（按行序）");
                expectTrue(rl[0].page == rl[1].page, "同一页内的两块（这条文档只有一页）");
            }
        }
    }

    std::printf("\n[12] 上屏契约：引擎写过的区域必须累积到宿主读取为止\n");
    {
        //  这一节存在的理由：一次操作触发两次 present 时，若宿主拿后一次的脏矩形
        //  上屏，前一次的全帧重绘就丢了。症状是引擎帧完全正确（auditFrame 差 0）、
        //  画布却是旧的（selfCheck 差上千像素），也就是"光标走了、文字没动、
        //  方括号还留在原地"。方向因此钉死在"引擎写了、宿主没贴"上。
        //
        //  机制：一次用户操作会触发两次 present ——
        //    mod.setCaret() → mod.present()（第一次，改排版，脏矩形是整帧）
        //    宿主发现 editStats.caretVisible == false（闪烁正停在隐藏那半拍）
        //      → mod.showCaret(true) → mod.present()（第二次，只写光标与标记，很小）
        //      → 用第二次的脏矩形上屏，第一次就被整个丢掉了。
        //  所以判据不是"第一次写了多少"，而是"两次读之间引擎一共写过多少"。
        //  下面按真实顺序把这条路走一遍。
        //
        //  这一节显式用 Focused 档：它要量的是"第一次 present 写了整帧"那条路径，
        //     而"换字段 → 括号占位换位置 → 整行重排"只在 Focused 档成立。
        //     （默认的 All 档下换字段根本不重排，那是括号常驻带来的收益，
        //      它的对照断言在后面那段，别把两者混起来。）
        EditorEngine eg;
        if (!eg.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig cg;
        cg.zoom = 1.5f;
        cg.form_brackets = FormBracketScope::Focused;
        eg.setConfig(cg);

        BlockSpec bg;
        bg.font_index = 0;
        bg.size_pt = 12.0f;
        bg.wm = WritingMode::HorizontalTB();
        bg.line_factor = 1.75f;
        bg.language = "zh";
        bg.setText("患者主诉：反复心悸三天，加重一天。");
        eg.setBlocks({bg});
        eg.relayout();

        auto TC = [](int a, int b, const char* id) {
            Control c;
            c.range.start = TextPos{0, static_cast<size_t>(a)};
            c.range.end = TextPos{0, static_cast<size_t>(b)};
            c.id = id;
            return c;
        };

        eg.setControls({TC(5, 9, "k1"), TC(12, 16, "k2")});
        eg.setEditMode(EditMode::Form);
        eg.setCaret(0, 6);  // 光标在 #0（反复心悸）
        eg.repaint();
        (void)eg.takeBlitBox();  // 把之前的账清零，下面量的才是这一串

        // 把闪烁相位摆到"隐藏"：这条路径只在隐藏半拍触发
        //    （若开头调了 stopBlink()，这一半概率就被屏蔽掉了）
        eg.showCaret(false);
        eg.repaint();
        const Rect small_box = eg.takeBlitBox();

        // 换字段（= 鼠标点「加重一天」）：
        //    聚焦字段变 → 括号占位换位置 → 整行文字重排。
        eg.setCaret(0, 14);
        eg.repaint();                          // 第一次 present
        const EditStats st1 = eg.editStats();  // 宿主此刻读到的账

        // 宿主看到 caretVisible == false，走它的补救路径
        const bool took_rescue = !st1.caret_visible;
        eg.showCaret(true);
        eg.repaint();  // 第二次 present
        const EditStats st2 = eg.editStats();
        const Rect box = eg.takeBlitBox();  // 宿主真正该贴的区域

        std::printf("     隐藏相位那次 present：%d×%d px\n", static_cast<int>(small_box.w),
                    static_cast<int>(small_box.h));
        std::printf("     换字段后第一次 present 的本轮账：%d×%d px（整帧 %d×%d）\n",
                    st1.repainted_w, st1.repainted_h, eg.frameWidth(), eg.frameHeight());
        std::printf("     补救时第二次 present 的本轮账：%d×%d px\n", st2.repainted_w,
                    st2.repainted_h);
        std::printf("     待上屏并集：%d×%d px ｜ 走了补救路径=%d\n", static_cast<int>(box.w),
                    static_cast<int>(box.h), took_rescue ? 1 : 0);

        expectTrue(took_rescue,
                   "前提成立：光标在隐藏半拍时，第一次 present 的 caretVisible 为 false "
                   "—— 宿主正是靠它决定走补救路径的");
        expectTrue(st2.repainted_w < st1.repainted_w,
                   "补救那次 present 的本轮脏矩形确实小得多 "
                   "（这就是宿主被误导的地方：拿它上屏会漏掉第一次 present）");
        expectTrue(box.w == eg.frameWidth() && box.h == eg.frameHeight(),
                   "但 takeBlitBox() 给出的并集必须覆盖整帧（第一次 present 的功劳）"
                   "—— 少算了，画布就会停在旧排版上：引擎帧对、屏幕是旧的");
        expectTrue(small_box.w < eg.frameWidth(),
                   "对照：隐藏相位那次 present 自己远不是整帧 —— 所以并集只能靠累加器");

        // 读完即清零：下一次没写任何东西时，盒子必须是空的
        const Rect after = eg.takeBlitBox();
        expectTrue(after.w == 0.0f || after.h == 0.0f,
                   "takeBlitBox() 是读后清零的（否则宿主每次都会重贴整帧）");

        // 对照：默认档（All）下换字段不重排，上屏量必须远小于整帧。
        //
        //   这条钉死的是"括号常驻"带来的真正好处：占位集合与光标无关，
        //   于是"点一下另一个字段"不再引发整行重排（Focused 档下会），
        //   上屏量从"整帧"掉到"一块小矩形"。
        //   判据取"远小于整帧"而不是某个精确像素数：精确值随字体/字号变。
        {
            EngineConfig ca = eg.config();
            ca.form_brackets = FormBracketScope::All;
            eg.setConfig(ca);  // 切档会重排（占位集合变了）
            eg.setCaret(0, 6);
            eg.repaint();
            (void)eg.takeBlitBox();

            eg.setCaret(0, 14);  // 换到另一个字段 —— All 档下排版不动
            eg.repaint();
            const Rect cross = eg.takeBlitBox();
            std::printf("     ⑤ All 档换字段：上屏盒 %d×%d px（整帧 %d×%d）\n",
                        static_cast<int>(cross.w), static_cast<int>(cross.h), eg.frameWidth(),
                        eg.frameHeight());
            expectTrue(cross.w > 0.0f && cross.h > 0.0f, "⑤ 换字段确实写了点什么（光标）");
            expectTrue(cross.w < eg.frameWidth() / 2 && cross.h < eg.frameHeight() / 2,
                       "默认档下换字段不再整帧重贴（占位与光标无关 → 排版不动），"
                       "而 Focused 档下同样动作会写整帧（见 ② 的账）");
        }
    }

    std::printf("\n[13] 光标在字段两端必须落在括号之内\n");
    {
        //光标停在「重」上（字段最后一个字），再按一次右方向键，
        //  光标直接跳到 ] 的右边 —— 中间那个"在 ] 之内"的位置被跳过了，
        //  看起来像光标从字里蹿出了框外。
        //
        //  根因在 LineBox::inlineOffsetFor：它取"最后一个 cp <= target 的簇"，
        //  而闭括号占位后面紧跟着一个 cp 相同的真实簇（字段外紧邻的那个字，
        //  这里是。）。best 被那个簇覆盖，于是落点跑到 ] 之外去了。
        //  开括号那边反而不受影响 —— 它要的就是"继续往前走、取到后面那个真实簇"。
        //
        //  判据取"光标 x == 控件矩形的边缘"，因为 controlRects 本来就是
        //    [内容] 里内容那一段（它已经跳过占位簇）—— 两端一致才是对的。
        EditorEngine ex;
        if (!ex.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig cx;
        cx.zoom = 1.5f;
        ex.setConfig(cx);

        BlockSpec bx;
        bx.font_index = 0;
        bx.size_pt = 12.0f;
        bx.wm = WritingMode::HorizontalTB();
        bx.line_factor = 1.75f;
        bx.language = "zh";
        bx.setText("患者主诉：反复心悸三天，加重一天。");
        ex.setBlocks({bx});
        ex.relayout();

        Control k1;
        k1.range.start = TextPos{0, 5};
        k1.range.end = TextPos{0, 9};
        k1.id = "k1";
        Control k2;
        k2.range.start = TextPos{0, 12};  // 加
        k2.range.end = TextPos{0, 16};    // 天 之后
        k2.id = "k2";
        ex.setControls({k1, k2});
        ex.setEditMode(EditMode::Form);
        ex.setCaret(0, 12);
        ex.repaint();

        const std::vector<EditorEngine::ControlRect> rk = ex.controlRects(1);
        expectTrue(!rk.empty(), "k2 投影出了矩形块");
        const float L = rk[0].r_pt.x;
        const float R = rk[0].r_pt.x + rk[0].r_pt.w;

        // 必须换算成插入点再比：caretRectPt().x 是光标矩形的左边缘，
        //   比真正的插入点靠左半个光标宽（1.4pt 的条 → 左 0.7pt）。
        //   直接拿它去比控件矩形边缘会永远差 0.7pt，所以这里换算成插入点再比
        //   （做成一个有名字的 lambda，避免再犯）。
        const float half_caret = ex.config().caret_width_pt * 0.5f;
        auto caretX = [&](size_t off) {
            ex.setCaret(0, off);
            Rect r{};
            ex.caretRectPt(&r);
            return r.x + half_caret;
        };
        const float x12 = caretX(12);  // 字段首字「加」的左边
        const float x13 = caretX(13);
        const float x15 = caretX(15);                          // 末字「天」的左边
        const float x16 = caretX(16);                          // 字段末尾（＝「天」右边、`]` 左边）
        const float slot = 4.055f + kControlBracketSlotGapPt;  // 括号槽（参考值）

        std::printf("     插入点 x：off12=%.3f  off13=%.3f  off15=%.3f  off16=%.3f\n", x12, x13,
                    x15, x16);
        std::printf("     控件矩形（＝内容段）x ∈ [%.3f, %.3f]，括号槽 ≈ %.3f pt\n", L, R, slot);

        expectTrue(std::fabs(x12 - L) < 0.02f,
                   "光标在字段起点 → 落在 `[` 之内（x == 控件矩形左缘）");
        expectTrue(x12 < x13 && x13 < x15 && x15 < x16, "光标在字段内逐字右移，单调、没有跳位");
        expectTrue(std::fabs(x16 - R) < 0.02f,
                   "光标在字段末尾 → 落在 `]` 之内（x == 控件矩形右缘）。"
                   "跑到 `]` 右边就是用户看到的「光标直接蹿到括号外」");
        expectTrue(x16 - x15 > 1.0f,
                   "而且它确实是动了一步才到那儿的（不是原地没动、也不是跨过整格）");

        // 反查也要自洽：把落点喂回去必须回到同一个偏移
        {
            Rect r16{};
            ex.setCaret(0, 16);
            ex.caretRectPt(&r16);
            TextPos back{};
            const bool ok = ex.hitTestPt(r16.x + 0.2f, r16.y + r16.h * 0.5f, &back);
            expectTrue(ok && back.para == 0 && back.offset == 16,
                       "正反查自洽：把字段末尾的光标位置喂回命中测试，仍解析为同一个偏移");
        }
    }

    std::printf("\n[14] 相邻字段的括号顺序是 `][`，不是 `[]`\n");
    {
        //  （截图里是 心悸[]三天）：两个字段紧挨着时，前一个字段的
        //  右括号画到了后一个字段左括号的右边 —— 顺序反了。
        //
        //  根因：共享边界（前一个字段的结束 == 后一个字段的开始 == 同一个偏移）
        //  上落着两个槽，而插入时一律"开在前、闭在后"。那条规则只对空字段
        //  成立（同一个字段的一对，start == end → []）；相邻字段要的是闭在前
        //  （前一个字段的内容在左、后一个在右）。
        //  现在按"这槽属于谁"排优先级：内容结束处的闭括号最靠前（见 layout.cpp 的
        //  Slot::prio）。][（相邻）、[]（空字段）、[[ ]]（嵌套同区间）三种都对。
        //
        //  判据用颜色：可编辑括号亮蓝、只读括号灰 —— "谁在左"于是像素上直接可读，
        //    不需要去猜字形，也不会因为字体换实现而失效。
        EditorEngine ea;
        if (!ea.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig ca;
        ca.zoom = 1.5f;
        ea.setConfig(ca);

        BlockSpec ba;
        ba.font_index = 0;
        ba.size_pt = 12.0f;
        ba.wm = WritingMode::HorizontalTB();
        ba.line_factor = 1.75f;
        ba.language = "zh";
        ba.setText("患者主诉：反复心悸三天，加重一天。");
        ea.setBlocks({ba});
        ea.relayout();

        Control ka;
        ka.range.start = TextPos{0, 5};
        ka.range.end = TextPos{0, 9};  // 反复心悸（可编辑 → 亮蓝）
        ka.id = "a";
        Control kb;
        kb.range.start = TextPos{0, 9};  // 紧挨着：A 的末尾就是 B 的开头
        kb.range.end = TextPos{0, 11};   // 三天（只读 → 灰）
        kb.id = "b";
        kb.read_only = true;
        ea.setControls({ka, kb});
        ea.setEditMode(EditMode::Form);
        ea.setCaret(0, 6);
        ea.repaint();

        // (a) 结构性：那一处边界的占位簇顺序必须是 [闭, 开]，且闭的在左
        float p_close = -1.0f, p_open = -1.0f;
        int side_first = -1, side_second = -1, n_ph = 0;
        for (const LineBox& lb : ea.layout().lines) {
            if (lb.paragraph != 0) continue;
            for (const LineCluster& c : lb.clusters) {
                if (!c.placeholder || c.cp != 9) continue;  // 只看共享边界
                ++n_ph;
                if (side_first < 0)
                    side_first = static_cast<int>(c.bracket_side);
                else
                    side_second = static_cast<int>(c.bracket_side);
                const float x = lb.inline_origin_pt + c.inline_pt;
                if (c.bracket_side == BracketSide::Close)
                    p_close = x;
                else
                    p_open = x;
            }
        }
        const std::vector<EditorEngine::ControlRect> ra0 = ea.controlRects(0);
        const float content_right = ra0.empty() ? 0.0f : ra0[0].r_pt.x + ra0[0].r_pt.w;
        std::printf(
            "     共享边界（偏移 9）：占位 %d 个，顺序=[%d,%d]，闭槽 x=%.2f 开槽 x=%.2f，"
            "A 内容右缘 x=%.2f pt\n",
            n_ph, side_first, side_second, p_close, p_open, content_right);

        expectTrue(n_ph == 2 && side_first == static_cast<int>(BracketSide::Close) &&
                       side_second == static_cast<int>(BracketSide::Open),
                   "共享边界上的占位顺序是 [闭, 开] —— 一律「开在前」就是 `[]`（用户报的）");
        expectTrue(p_close > 0.0f && p_open > p_close,
                   "闭括号的槽在开括号的槽左边（内容结束处的括号先占位）");
        expectTrue(std::fabs(p_close - content_right) < 0.02f,
                   "闭括号的槽紧贴 A 的内容右缘（不是浮在别处）");

        // (b) 像素级：亮蓝的 ] 必须整个在灰的 [ 左边
        {
            auto snap = [](EditorEngine& eng) {
                eng.repaint();
                return std::vector<uint8_t>(eng.frame(), eng.frame() + eng.frameBytes());
            };
            // 亮蓝（可编辑括号）：与 [11] 的 (f) 同源判据，光标色 r=31 被排除。
            auto isBlue = [](uint8_t r, uint8_t g, uint8_t b) {
                return r > 45 && (static_cast<int>(b) - static_cast<int>(r)) > 60 && b > 140;
            };
            // 灰（只读括号，标记灰 = 168）：三通道接近且在 [140,195]。
            //   上限 195 是为了排掉页边框（206,212,218）+ 抗锯齿混白；
            //     下限 140 是为了排掉正文黑字（24,24,24）的抗锯齿尾巴。
            //     （正文与灰括号在颜色上本来就可能撞，所以窗口还要止于 B 的内容之前。）
            auto isGray = [](uint8_t r, uint8_t g, uint8_t b) {
                const int drg = std::abs(static_cast<int>(r) - static_cast<int>(g));
                const int dgb = std::abs(static_cast<int>(g) - static_cast<int>(b));
                return r >= 140 && r <= 195 && drg <= 8 && dgb <= 8;
            };
            struct Span {
                int lo = -1;
                int hi = -1;
                int count() const { return lo < 0 ? 0 : hi - lo + 1; }
            };
            const float zoom = ea.config().zoom;
            const int fw = ea.frameWidth();
            const std::vector<uint8_t> f = snap(ea);
            const int x0 = static_cast<int>(std::lround(p_close * zoom)) - 2;
            // 止于 B 的开槽末（再往里 1pt 就是「三」的墨迹了）
            const float slot_len = p_open - p_close;
            const int x1 = static_cast<int>(std::lround((p_open + slot_len - 1.0f) * zoom));

            Span blue, gray;
            auto scan = [&](auto pred, Span* out) {
                for (size_t i = 0; i + 3 < f.size(); i += 4) {
                    const int x = static_cast<int>(i >> 2) % fw;
                    if (x < x0 || x > x1) continue;
                    if (!pred(f[i], f[i + 1], f[i + 2])) continue;
                    if (out->lo < 0) out->lo = x;
                    out->hi = x;
                }
            };
            scan(isBlue, &blue);
            scan(isGray, &gray);
            std::printf("     窗口 x ∈ [%d, %d]：亮蓝 %d..%d（%d 列）｜ 灰 %d..%d（%d 列）\n", x0,
                        x1, blue.lo, blue.hi, blue.count(), gray.lo, gray.hi, gray.count());

            expectTrue(blue.count() > 0 && gray.count() > 0,
                       "窗口里两种颜色的括号都在（蓝的= A 的右括号、灰的= B 的左括号）");
            expectTrue(blue.hi > 0 && blue.hi < gray.lo,
                       "亮蓝 `]` 整个在灰 `[` 左边 —— 相邻字段是 `][`。"
                       "反过来的话就是用户截图里的 `[]`");
        }
    }

    std::printf("\n[15] 增量排版：脏段之后重排、前缀原样复用（档 2）\n");
    {
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig c;
        c.zoom = 1.0f;
        e.setConfig(c);

        // 200 段病历：单段文档没有可复用的前缀，增量与全量必然相等，
        // 只有多段文档才能把"只重排脏段"这件事量出来。
        const int kParas = 200;
        std::vector<BlockSpec> doc;
        for (int i = 0; i < kParas; ++i) {
            BlockSpec b;
            b.font_index = 0;
            b.size_pt = 14.0f;
            b.line_factor = 1.85f;
            b.space_after_pt = 6.0f;
            b.language = "zh";
            b.setText("第 " + std::to_string(i + 1) +
                      " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解，"
                      "未见黑矇与晕厥。");
            doc.push_back(std::move(b));
        }
        e.setBlocks(doc);
        e.relayout();
        e.repaint();

        expectTrue(e.editStats().layout_was_full, "前提：首次排版是全量（整篇都脏）");
        const int total_lines = e.editStats().total_lines;
        std::printf("     %d 段 → %d 行，首次全量耗时 %.3f ms\n", kParas, total_lines,
                    e.stats().layout_ms);
        expectTrue(total_lines > kParas, "200 段排出多于 200 行（每段不止一行）");

        // 在第 100 段中间敲一个字：前缀应当原样复用。
        e.setCaret(100, 10);
        const bool typed = e.typeTextAndRender("\xE6\xB5\x8B");  // 测
        expectTrue(typed, "第 100 段插入一个汉字");
        const EditStats es = e.editStats();
        std::printf("     敲一个字：重排 %d 段 / %d 段，%d 行 / %d 行，%s，%.3f ms\n",
                    es.relaid_paragraphs, es.total_paragraphs, es.relaid_lines, es.total_lines,
                    es.layout_was_full ? "全量" : "增量", es.layout_ms);
        expectTrue(!es.layout_was_full, "敲字后走的是增量排版");
        expectTrue(es.relaid_paragraphs == kParas - 100, "只重排第 100 段起的后缀（100 段）");
        expectTrue(es.relaid_lines < es.total_lines, "重排行数真的小于总行数");
        expectTrue(e.auditLayout() == 0, "增量结果与全量重排逐字段一致（diff 0）");

        // 随机编辑序列：每一步都要求增量 == 全量。
        uint32_t seed = 20261010u;
        auto rnd = [&](uint32_t n) {
            seed = seed * 1103515245u + 12345u;
            return n ? (seed >> 16) % n : 0u;
        };
        int bad = 0, incr = 0, full = 0;
        for (int step = 0; step < 60; ++step) {
            const int p = static_cast<int>(rnd(static_cast<uint32_t>(kParas)));
            const size_t len = e.blocks()[static_cast<size_t>(p)].cp_text.size();
            if (rnd(2) == 0 && len > 2) {
                e.setCaret(p, static_cast<size_t>(1 + rnd(static_cast<uint32_t>(len - 1))));
                e.backspaceAndRender();
            } else {
                e.setCaret(p, static_cast<size_t>(rnd(static_cast<uint32_t>(len + 1))));
                e.typeTextAndRender(rnd(2) ? "\xE6\xB5\x8B" : "a");
            }
            if (e.editStats().layout_was_full) {
                ++full;
            } else {
                ++incr;
            }
            if (e.auditLayout() != 0) ++bad;
        }
        std::printf("     随机 60 步：增量 %d 步 / 全量 %d 步，与全量不符 %d 步\n", incr, full,
                    bad);
        expectTrue(bad == 0, "★ 60 步随机编辑，每一步的增量排版都等于全量");
        expectTrue(incr >= 55, "★ 绝大多数步子走的是增量（只有脏段是第一段时才只能全量）");

        // 相对耗时：与全量在同一次测量里比，不依赖机器绝对速度。
        //
        // 单次 layout_ms 的噪声很大（系统调度与缓存抖动都能差出一倍以上），所以两边各测
        // 5 次取中位数再比 —— 报出去的绝对值也必须是稳的，否则它只是一串好看的数字。
        // 全量的求法：没有脏段时 relayout() 按全量处理（见 engine.h 的说明）。
        auto median = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        std::vector<double> full_s, tail_s, mid_s;
        for (int i = 0; i < 5; ++i) {
            e.relayout();
            full_s.push_back(e.stats().layout_ms);
            e.setCaret(195 - i, 5);  // 换着段敲，保证每次都是真编辑
            e.typeTextAndRender("\xE6\xB5\x8B");
            tail_s.push_back(e.stats().layout_ms);
        }
        for (int i = 0; i < 5; ++i) {
            e.setCaret(100 - i, 5);
            e.typeTextAndRender("\xE6\xB5\x8B");
            mid_s.push_back(e.stats().layout_ms);
        }
        const double full_ms = median(full_s);
        const double tail_ms = median(tail_s);
        const double mid_ms = median(mid_s);
        std::printf(
            "     中位数：全量重排 %.3f ms ｜ 尾部敲字 %.3f ms（%.1f%%）｜ 中部敲字 %.3f ms（%.1f%%）\n",
            full_ms, tail_ms, full_ms > 0.0 ? 100.0 * tail_ms / full_ms : 0.0, mid_ms,
            full_ms > 0.0 ? 100.0 * mid_ms / full_ms : 0.0);
        // 文档太小时全量只要零点几毫秒，比值会被计时分辨率主导 —— 那时退成"更快"。
        // 宁可少判一条，也不要让"机器越快越容易假红"。
        if (full_ms > 1.0) {
            expectTrue(tail_ms * 5.0 < full_ms, "★ 靠后敲字：增量耗时不到全量的 20%");
        } else {
            expectTrue(tail_ms < full_ms, "靠后敲字的增量耗时小于全量（全量不足 1ms，比值不判）");
        }
        // 中部编辑的脏后缀是整个后半篇，成本本就该跟着后缀走；整形缓存让未变的段全部命中，
        // 所以它显著便宜于全量，但不会是"几乎免费"。
        expectTrue(mid_ms < full_ms * 0.6, "中部敲字：脏后缀是半篇，成本仍显著低于全量");

        // 边界：脏段就是第一段 → 没有前缀可复用 → 只能全量（不是缺陷）。
        e.setCaret(0, 0);
        e.typeTextAndRender("a");
        expectTrue(e.editStats().layout_was_full, "在第一段打字只能全量（没有前缀可复用）");
        expectTrue(e.auditLayout() == 0, "全量路径同样与对照一致");

        // 页度量：整篇标脏 → 全量，而且结果仍与对照一致。
        PageMetrics p2 = e.config().page;
        p2.width_pt -= 20.0f;
        e.setPage(p2);
        e.relayout();
        e.repaint();
        expectTrue(e.editStats().layout_was_full, "改页度量 → 整篇标脏 → 全量重排");
        expectTrue(e.auditLayout() == 0, "改页度量后依然与对照一致");

        // 控件：改控件集合只标脏"最靠前的那个控件所在段"，不是整篇。
        {
            const size_t len = e.blocks()[120].cp_text.size();
            Control k1;
            k1.range.start = TextPos{120, 0};
            k1.range.end = TextPos{120, std::min<size_t>(8, len)};
            k1.id = "k1";
            e.setControls({k1});
            e.setEditMode(EditMode::Form);
            // EditStats 由 presentInternal 刷新：setControls / setEditMode 只改状态与排版，
            // 不呈现 —— 要读这一轮的账就得先呈现一次。
            e.repaint();
            const EditStats cs = e.editStats();
            std::printf("     挂控件：重排 %d 段 / %d 段（%s）\n", cs.relaid_paragraphs,
                        cs.total_paragraphs, cs.layout_was_full ? "全量" : "增量");
            expectTrue(!cs.layout_was_full && cs.relaid_paragraphs == kParas - 120,
                       "★ 挂控件只重排第 120 段起的后缀，不是整篇");
            expectTrue(e.auditLayout() == 0, "挂控件之后的排版同样与对照一致");
        }
    }

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
