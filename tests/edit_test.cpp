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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/document.h"
#include "core/doc/section_spec.h"
#include "core/engine/engine.h"
#include "core/layout/layout.h"
#include "core/text/shaper.h"
#include "demo/demo_documents.h"

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

/**
 * 两个控件区间是否"互相穿过"：半开区间有交集。
 * 相邻（只共享边界点，如 [5,9) 与 [9,12)）不算 —— 那正是合法且必要的一格；
 * 嵌套（一个盖住另一个）也不算 —— 那是嵌套控件的正常形态。
 * 只有"一半重叠"才是非法状态：它会同时让区间归属和括号顺序失去意义。
 */
bool spansCross(const ControlRange& a, const ControlRange& b) {
    const bool overlap = a.start < b.end && b.start < a.end;
    if (!overlap) return false;
    return !rangeCovers(a, b) && !rangeCovers(b, a);
}

/**
 * 独立口径：某个像素点落在"谁的内容框"里 —— 取最深的那个（嵌套时内层优先）。
 * 内容框是控件自己投影出来的（不含括号），所以这是对"屏幕上看到并点中的那个框"的
 * 直接陈述，不依赖字形宽度；与引擎侧的偏移判定完全独立，两者对不上就是真错。
 */
int expectedOwnerAt(EditorEngine& eng, float x, float y) {
    int best = -1;
    int best_depth = -1;
    for (int ci = 0; ci < eng.controls().size(); ++ci) {
        for (const EditorEngine::ControlRect& r : eng.controlRects(ci)) {
            if (x < r.r_pt.x - 0.01f || x > r.r_pt.x + r.r_pt.w + 0.01f) continue;
            if (y < r.r_pt.y - 0.01f || y > r.r_pt.y + r.r_pt.h + 0.01f) continue;
            if (eng.controls().at(ci).depth > best_depth) {
                best_depth = eng.controls().at(ci).depth;
                best = ci;
            }
        }
    }
    return best;
}

/**
 * 括号的颜色判据：可编辑的括号亮蓝、只读的灰。
 * 判据用颜色而不是"猜字形"，是因为"谁在左"于是像素上直接可读，
 * 不会因为换字体或换实现而失效。亮蓝那条与 [11] 的 (f) 同源：光标色 r=31 被排除。
 */
bool isBracketBlue(uint8_t r, uint8_t g, uint8_t b) {
    (void)g;
    return r > 45 && (static_cast<int>(b) - static_cast<int>(r)) > 60 && b > 140;
}

/**
 * 灰（只读括号，标记灰 = 168）：三通道接近且在 [140,195]。
 * 上限 195 排掉页边框（206,212,218）与抗锯齿混白，下限 140 排掉正文黑字的抗锯齿尾巴。
 */
bool isBracketGray(uint8_t r, uint8_t g, uint8_t b) {
    const int drg = std::abs(static_cast<int>(r) - static_cast<int>(g));
    const int dgb = std::abs(static_cast<int>(g) - static_cast<int>(b));
    return r >= 140 && r <= 195 && drg <= 8 && dgb <= 8;
}

/**
 * 在像素窗口 [x0, x1]（列）内找某类颜色的列范围；一个都没有则 lo = -1。
 */
void scanColumns(const std::vector<uint8_t>& f, int frame_w, int x0, int x1,
                 bool (*pred)(uint8_t, uint8_t, uint8_t), int* lo, int* hi) {
    *lo = -1;
    *hi = -1;
    for (size_t i = 0; i + 3 < f.size(); i += 4) {
        const int x = static_cast<int>(i >> 2) % frame_w;
        if (x < x0 || x > x1) continue;
        if (!pred(f[i], f[i + 1], f[i + 2])) continue;
        if (*lo < 0) *lo = x;
        *hi = x;
    }
}

/**
 * "增量上屏 == 从零上屏"的比对器：拿第二个引擎（同文档、同配置、同控件、同光标，
 * 从零重排重画）当参照物，与被测引擎的当前帧逐像素比。
 *
 * 为什么需要它：auditFrame() 比的是"帧 vs 从底图整块重贴"，只能抓上屏漏贴；
 * 底图自己漏画（页指纹判错、某个变了的页没被重画）时两边会一起错、它照样是 0。
 * 只有拿一个真正从零开始渲染的参照物，才照得出来。
 *
 * @param out_max_ch 最大通道差：用来区分"结构性错位"与"坐标末位差翻了一格抗锯齿"
 * @return 不符的像素数；-1 = 帧尺寸不同（本身就不符）
 */
long long frameDiffAgainstFresh(EditorEngine& a, const std::vector<std::string>& fonts,
                                const std::vector<Control>& ctrls, EditMode mode,
                                long long* out_max_ch) {
    EditorEngine r;
    if (!r.init(fonts)) return -1;
    r.setConfig(a.config());
    r.setBlocks(a.blocks());
    // setBlocks 只换数据不排版（"谁改文档谁负责 relayout"）—— 这里必须自己排一次，
    // 否则帧高会按 0 页算，比出来的东西没意义。
    r.relayout();
    if (!ctrls.empty()) r.setControls(ctrls);
    r.setEditMode(mode);
    r.relayout();
    r.setCaret(a.caret().para, a.caret().offset);
    r.showCaret(true);
    r.repaint();

    a.showCaret(true);
    a.repaint();
    const size_t n = a.frameBytes();
    if (n != r.frameBytes()) return -1;
    const uint8_t* fa = a.frame();
    const uint8_t* fb = r.frame();
    long long diff = 0, max_ch = 0;
    for (size_t k = 0; k + 3 < n; k += 4) {
        if (fa[k] == fb[k] && fa[k + 1] == fb[k + 1] && fa[k + 2] == fb[k + 2]) continue;
        ++diff;
        for (int ch = 0; ch < 3; ++ch) {
            const long long d =
                std::llabs(static_cast<long long>(fa[k + ch]) - static_cast<long long>(fb[k + ch]));
            if (d > max_ch) max_ch = d;
        }
    }
    if (out_max_ch) *out_max_ch = max_ch;
    return diff;
}

/**
 * 两个引擎的整帧逐字节比较（只比 RGB，忽略 alpha）。
 *
 * 用于"两条路径必须产出同一帧"这类判据。它比逐字段比 LayoutResult 更强，也更省事：
 * 任何一处几何、字体、颜色不同都会落到像素上，不必逐个字段列举 —— 而"漏比一个字段"
 * 正是这类判据最容易变成空话的地方。
 * 返回不符的像素数；-1 表示帧尺寸不同（本身就不符）。
 */
long long frameDiffBetween(EditorEngine& a, EditorEngine& b) {
    a.showCaret(true);
    b.showCaret(true);
    a.repaint();
    b.repaint();
    if (a.frameBytes() != b.frameBytes()) return -1;
    const uint8_t* fa = a.frame();
    const uint8_t* fb = b.frame();
    long long diff = 0;
    for (size_t k = 0; k + 3 < a.frameBytes(); k += 4) {
        if (fa[k] == fb[k] && fa[k + 1] == fb[k + 1] && fa[k + 2] == fb[k + 2]) continue;
        ++diff;
    }
    return diff;
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

    const std::string cjkPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string mnPath =
        joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

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

            // 光标位置不归 EditPolicy 管：字段外、只读字段里都能停（策略约束的是编辑）。
            //   对应的断言在 [21] 用引擎实例验（那里才有"光标能不能移动过去"这件事）。
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
            // 只读模式的光标同样不受约束（只读约束的是编辑，不是浏览）—— 断言见 [21]。
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
            //   不能用 setCaret + typeText：那样混进了"光标能不能停在区间外"这条独立行为
            //   （光标现在哪儿都能停，见下一条），失败时分不清是哪一件事。
            expectTrue(!ef.insertTextUtf8(0, 2, "X"), "字段外（offset 2）插入被拒");
            expectTrue(ef.blocks()[0].cp_text.size() == len0, "文档长度一个码点都没变");
            expectTrue(ef.blockedEdits() == b0 + 1,
                       "这次拒绝被记了账（不是静默失败 —— 静默失败和「程序卡了」无法区分）");

            ef.setCaret(0, 2);
            expectTrue(ef.caret().offset == 2,
                       "光标停在字段外的 2 —— 位置不受控件约束，受约束的只有编辑");

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

        // (d) 方向键一步一格，且能走到字段之外
        //
        //   规则调整（用户提出）：光标只在"编辑"上受约束，位置本身不受约束 ——
        //     字段外、只读字段里都能停。方向键因此能跨字段、能走进正文；唯一保留的
        //     判据是"一步一格"：一个方向键指令只往那个方向走一格，绝不瞬移。
        {
            ef.setCaret(0, 8);
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 9,
                       "右移到字段 #0 末尾 9 —— 末尾当然可以停");
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 10,
                       "再右移：走进字段之间的正文 10（光标不是被关在字段里）");
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 11, "继续右移到 11");
            expectTrue(ef.moveCaretAndRender(-1) && ef.caret().offset == 10, "左移回到 10");

            // 那一下：在 #2 起点按左键，要一格一格地退出来
            ef.setCaret(0, 12);
            expectTrue(ef.moveCaretAndRender(-1) && ef.caret().offset == 11,
                       "在 #2 起点左移：退到 11（一步一格，不是瞬移）");
            expectTrue(ef.moveCaretAndRender(-1) && ef.caret().offset == 10, "再左移到 10");

            ef.setCaret(0, 16);
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 17,
                       "最后一个字段末尾右移：走到段落末尾 17（后面的正文也去得）");
            expectTrue(!ef.moveCaretAndRender(1), "到段尾之后右移返回 false（没动就不该重绘）");

            // 穷尽：整段左右各走一趟 —— 单步跨度必须恒为 1，而且这一趟真的走出过 #2。
            //   只断言"没瞬移"是不够的：光标压根不动也满足它。
            {
                ef.setCaret(0, 12);
                int worst = 0;
                bool saw_outside = false;
                for (int i = 0; i < 8; ++i) {
                    const size_t b = ef.caret().offset;
                    ef.moveCaretAndRender(1);
                    const size_t a = ef.caret().offset;
                    const int d = static_cast<int>(a) - static_cast<int>(b);
                    if (d > worst) worst = d;
                    if (a < 12 || a > 16) saw_outside = true;
                }
                for (int i = 0; i < 8; ++i) {
                    const size_t b = ef.caret().offset;
                    ef.moveCaretAndRender(-1);
                    const size_t a = ef.caret().offset;
                    const int d = static_cast<int>(b) - static_cast<int>(a);
                    if (d > worst) worst = d;
                    if (a < 12 || a > 16) saw_outside = true;
                }
                std::printf("     [诊断] 左右各走 8 步：单步最大跨度 %d（应为 1），走出过字段=%d\n",
                            worst, saw_outside ? 1 : 0);
                expectTrue(worst == 1, "左右各走 8 步，单步跨度恒为 1（没有任何一步瞬移）");
                expectTrue(saw_outside, "这一趟真的走出了 #2 = [12,16)（字段不再是牢笼）");
            }

            // Home/End：就是行首 / 行尾，不再往字段边界上收
            ef.setCaret(0, 14);
            expectTrue(ef.moveCaretToLineStartAndRender() && ef.caret().offset == 0,
                       "Home 到行首 0（落点不在任何字段里也照样到）");
            expectTrue(ef.moveCaretToLineEndAndRender() && ef.caret().offset == 17, "End 到行尾 17");
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
            ea.repaint();
            const std::vector<uint8_t> f(ea.frame(), ea.frame() + ea.frameBytes());
            const float zoom = ea.config().zoom;
            const int fw = ea.frameWidth();
            const int x0 = static_cast<int>(std::lround(p_close * zoom)) - 2;
            // 止于 B 的开槽末（再往里 1pt 就是「三」的墨迹了）
            const float slot_len = p_open - p_close;
            const int x1 = static_cast<int>(std::lround((p_open + slot_len - 1.0f) * zoom));
            int blue_lo = -1, blue_hi = -1, gray_lo = -1, gray_hi = -1;
            scanColumns(f, fw, x0, x1, isBracketBlue, &blue_lo, &blue_hi);
            scanColumns(f, fw, x0, x1, isBracketGray, &gray_lo, &gray_hi);
            std::printf("     窗口 x ∈ [%d, %d]：亮蓝 %d..%d（%d 列）｜ 灰 %d..%d（%d 列）\n", x0,
                        x1, blue_lo, blue_hi, blue_hi < 0 ? 0 : blue_hi - blue_lo + 1, gray_lo,
                        gray_hi, gray_lo < 0 ? 0 : gray_hi - gray_lo + 1);

            expectTrue(blue_lo >= 0 && gray_lo >= 0,
                       "窗口里两种颜色的括号都在（蓝的 = A 的右括号、灰的 = B 的左括号）");
            expectTrue(blue_hi > 0 && blue_hi < gray_lo,
                       "亮蓝 ] 整个在灰 [ 左边 —— 相邻字段是 ][。反过来的话就是那个 []（报过的）");
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

        // 页指纹的增量重算靠二分定位"受影响页的字形区间"，依据是
        // "layout_ 的字形数组按页单调"（排版逐段追加，页号只增不减）。
        // 这条不变式一旦被打破（有人改了追加顺序、平移时把页码写反），
        // 二分就会静默地圈错区间 —— 所以在这里把它钉住，而不是只在注释里提一句。
        auto expectPagesMonotonic = [&](const char* when) {
            const std::vector<PlacedGlyph>& gl = e.layout().glyphs;
            int bad = 0;
            for (size_t i = 1; i < gl.size(); ++i) {
                if (gl[i].page < gl[i - 1].page) ++bad;
            }
            expectTrue(bad == 0, std::string("字形数组按页单调（页指纹二分的依据）：") + when);
        };
        expectPagesMonotonic("整篇全量之后");

        // 在第 100 段中间敲一个字：前缀应当原样复用。
        e.setCaret(100, 10);
        const bool typed = e.typeTextAndRender("\xE6\xB5\x8B");  // 测
        expectTrue(typed, "第 100 段插入一个汉字");
        const EditStats es = e.editStats();
        std::printf(
            "     敲一个字：重排 %d 段 / 复用 %d 段 / 共 %d 段，%d 行 / %d 行，%s，%.3f ms\n",
            es.relaid_paragraphs, es.reused_paragraphs, es.total_paragraphs, es.relaid_lines,
            es.total_lines, es.layout_was_full ? "全量" : "增量", es.layout_ms);
        expectTrue(!es.layout_was_full, "敲字后走的是增量排版");
        // 行结构没变 → 只重排脏段自己，后缀整块搬回（一个字节都不动）。
        expectTrue(es.relaid_paragraphs == 1 && es.reused_paragraphs == kParas - 1 - 100,
                   "★ 只重排第 100 段，其余 99 段原样复用");
        expectTrue(es.relaid_lines < es.total_lines, "重排行数真的小于总行数");
        expectTrue(e.auditLayout() == 0, "增量结果与全量重排逐字段一致（diff 0）");
        expectPagesMonotonic("后缀原样复用之后");

        // 撑出一行（行结构变了）→ 后缀整体平移复用：只重排脏段，后缀的断行结果
        // 与字形的行内位置一个字节都不重算，只把块轴位置与页码/行号平移过去。
        e.setCaret(100, 5);
        e.typeTextAndRender(std::string(80, 'a'));  // 80 个半角字母，足够撑出一行
        const EditStats grew = e.editStats();
        std::printf("     撑出一行后：重排 %d 段 / 复用 %d 段（平移=%d，%s）\n",
                    grew.relaid_paragraphs, grew.reused_paragraphs, grew.suffix_shifted ? 1 : 0,
                    grew.layout_was_full ? "全量" : "增量");
        expectTrue(grew.suffix_shifted && grew.relaid_paragraphs == 1 &&
                       grew.reused_paragraphs == kParas - 1 - 100,
                   "★ 撑出一行 → 后缀整体平移复用（只重排 1 段，99 段不重排）");
        // 平移是"改坐标"不是"重排"，所以判据落在坐标上：坐标允许 1e-3 pt 的舍入差
        // （两种加法），结构性字段（页号/行号/段号/簇）必须逐位一致。
        expectTrue(e.auditLayout() == 0, "★ 平移复用的结果与全量重排一致（几何差 < 1e-3 pt）");
        expectPagesMonotonic("后缀平移复用之后");

        // 平移复用的反面：后缀里有竖排段 → "横排行槽折线"那条规则不成立 → 必须续排。
        // 这条同时证明常规续排路径没有被平移路径挤掉（两条路都要被走到）。
        {
            EditorEngine ev;
            expectTrue(ev.init({cjkPath, mnPath, latinPath}), "引擎初始化（竖排后缀）");
            ev.setConfig(c);
            std::vector<BlockSpec> d2;
            for (int i = 0; i < 12; ++i) {
                BlockSpec b;
                b.font_index = 0;
                b.size_pt = 14.0f;
                b.line_factor = 1.85f;
                b.space_after_pt = 6.0f;
                b.language = "zh";
                b.setText("第 " + std::to_string(i + 1) +
                          " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解。");
                d2.push_back(std::move(b));
            }
            BlockSpec v;
            v.font_index = 0;
            v.size_pt = 14.0f;
            v.line_factor = 1.9f;
            v.wm = WritingMode::VerticalRL();
            v.language = "zh";
            v.setText("患者主诉：反复心悸3天，加重1天。现病史：患者3天前无明显诱因出现心悸。");
            d2.push_back(std::move(v));
            ev.setBlocks(d2);
            ev.relayout();
            ev.repaint();

            ev.setCaret(5, 5);
            ev.typeTextAndRender(std::string(80, 'a'));
            const EditStats sv = ev.editStats();
            std::printf("     后缀含竖排段：重排 %d 段 / 复用 %d 段（平移=%d）\n",
                        sv.relaid_paragraphs, sv.reused_paragraphs, sv.suffix_shifted ? 1 : 0);
            expectTrue(
                !sv.suffix_shifted && sv.relaid_paragraphs == 13 - 5 && sv.reused_paragraphs == 0,
                "★ 后缀含竖排段 → 平移规则不成立，走常规续排（重排整个后缀）");
            expectTrue(ev.auditLayout() == 0, "竖排后缀的续排结果同样与全量一致");
        }

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
        // 全量的求法：没有脏段时 relayout() 按全量处理（见 engine/engine.h 的说明）。
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
        // 中部敲字同样走快路（行结构没变）→ 成本与尾部一样接近免费。
        // 这条同时钉住"靠位置换性能"的假象：中部与尾部都在 10% 以内才算数。
        expectTrue(mid_ms < full_ms * 0.6, "★ 中部敲字与尾部一样便宜（不是靠离文档末尾近）");

        // 端到端成本：排版 + 统计同步 + 页指纹 + 上屏。
        //
        // layout_ms 只量排版本身，看不到另外三项 —— 而用户感觉到的是这一整条。
        // 所以这里按真实的调用方式再量一遍（typeTextAndRender 内部就是
        // 编辑 + 重排 + 呈现）。每次测完把文档改回原样，恢复不参与测量。
        {
            const std::vector<BlockSpec> pristine = e.blocks();
            auto restore = [&]() {
                e.setBlocks(pristine);
                e.relayout();
                e.repaint();
            };
            auto e2e = [&](int para, size_t off, const std::string& text) {
                e.setCaret(para, static_cast<size_t>(off));
                const auto t0 = std::chrono::steady_clock::now();
                e.typeTextAndRender(text);
                const auto t1 = std::chrono::steady_clock::now();
                return std::chrono::duration<double, std::milli>(t1 - t0).count();
            };
            std::string wrap_text;
            for (int i = 0; i < 40; ++i) wrap_text += "\xE6\xB5\x8B";  // 40 个汉字，必定撑出一行

            std::vector<double> v_full, v_tail, v_mid, v_wrap_tail, v_wrap_mid;
            for (int i = 0; i < 5; ++i) {
                const auto f0 = std::chrono::steady_clock::now();
                e.relayout();
                e.repaint();
                const auto f1 = std::chrono::steady_clock::now();
                v_full.push_back(std::chrono::duration<double, std::milli>(f1 - f0).count());

                v_tail.push_back(e2e(195 - i, 5, "\xE6\xB5\x8B"));
                restore();
                v_mid.push_back(e2e(100 - i, 5, "\xE6\xB5\x8B"));
                restore();
                v_wrap_tail.push_back(e2e(195 - i, 5, wrap_text));
                restore();
                v_wrap_mid.push_back(e2e(100 - i, 5, wrap_text));
                restore();
            }
            const double e_full = median(v_full), e_tail = median(v_tail), e_mid = median(v_mid);
            const double e_wt = median(v_wrap_tail), e_wm = median(v_wrap_mid);
            std::printf(
                "     端到端：全量 %.3f ms ｜ 尾部敲 1 字 %.3f ms（%.1f%%）｜ 中部敲 1 字 %.3f ms"
                "（%.1f%%）\n",
                e_full, e_tail, 100.0 * e_tail / e_full, e_mid, 100.0 * e_mid / e_full);
            std::printf(
                "             撑出一行（后缀要跟着动）：尾部 %.3f ms（%.1f%%）｜ "
                "中部 %.3f ms（%.1f%%）\n",
                e_wt, 100.0 * e_wt / e_full, e_wm, 100.0 * e_wm / e_full);
            expectTrue(e_full > 0.0, "前提：端到端全量可以量");

            // 页指纹的增量化：整篇重算 vs 只算脏段覆盖的那几页。
            // 判据落在页数（EditStats::dirty_page_count：内容指纹变了、需要重画的页数），
            // 不落在耗时上 —— 耗时是两次独立计时，机器有负载时会翻（实测约三次里翻一次），
            // 那样的判据会让"全绿"变成抽奖。耗时仍打印出来做对照，但不参与断言。
            auto minOf3 = [](const std::vector<double>& v) {
                return *std::min_element(v.begin(), v.end());
            };
            auto maxOf3 = [](const std::vector<int>& v) {
                return *std::max_element(v.begin(), v.end());
            };
            std::vector<double> v_fp_full, v_fp_splice, v_fp_shift;
            std::vector<int> v_dp_full, v_dp_splice, v_dp_shift;
            for (int i = 0; i < 3; ++i) {
                restore();
                e.relayout();
                e.repaint();
                v_fp_full.push_back(e.stats().page_fp_ms);
                v_dp_full.push_back(e.editStats().dirty_page_count);

                restore();
                e.setCaret(195 - i, 5);
                e.typeTextAndRender("\xE6\xB5\x8B");
                v_fp_splice.push_back(e.stats().page_fp_ms);
                v_dp_splice.push_back(e.editStats().dirty_page_count);

                restore();
                e.setCaret(100 - i, 5);
                e.typeTextAndRender(wrap_text);
                v_fp_shift.push_back(e.stats().page_fp_ms);
                v_dp_shift.push_back(e.editStats().dirty_page_count);
            }
            const double fp_full = minOf3(v_fp_full);
            const double fp_splice = minOf3(v_fp_splice);
            const double fp_shift = minOf3(v_fp_shift);
            // 页数取三次里最大的：它是"最坏那次脏了多少页"，不受调度影响（同一编辑的脏页数是确定的）。
            const int dp_full = maxOf3(v_dp_full);
            const int dp_splice = maxOf3(v_dp_splice);
            const int dp_shift = maxOf3(v_dp_shift);
            // 整篇的上界用总页数：全量重排是把底图整块重建，此时"脏页"这个概念不适用
            // （没有可比对的旧底图，diff 出 0），而指纹确实重算了每一页。
            const int total_pages = e.editStats().total_pages;
            std::printf(
                "     页指纹：整篇 %.3f ms（重算 %d 页，脏页 %d）｜ 敲 1 字 %.3f ms / %d 页 ｜ "
                "平移复用 %.3f ms / %d 页\n",
                fp_full, total_pages, dp_full, fp_splice, dp_splice, fp_shift, dp_shift);
            expectTrue(fp_full > 0.0, "前提：页指纹耗时有记账（说明该项统计是活的）");
            expectTrue(total_pages > 1, "前提：文档多于一页，才有\"少于整篇\"可言");
            // 敲字（后缀原样复用）：重算范围就是脏段占的那一两页 → 应当小一个量级。
            expectTrue(dp_splice * 4 <= total_pages,
                       "★ 页指纹增量化：敲字只重算脏段覆盖的页（不超过整篇的 1/4）");
            // 平移复用：后缀整体挪了位置，从脏段所在页到末页的内容都真的变了 ——
            // 那几页本来就必须重画，指纹也得跟着重算。省下的只是脏段之前的页。
            // 所以这里只判"比敲字多、比整篇少"，不给比值（那种阈值会随编辑位置变成假判据）。
            expectTrue(dp_splice <= dp_shift && dp_shift < total_pages,
                       "★ 平移复用：只重算脏段及其后的页（少于整篇，但省不了那么多）");

            // 分段耗时：一次编辑拆成 编辑 / 重排（排版 + 页指纹）/ 呈现 三段。
            // 这条打印存在的理由：端到端时间里"排版"只占几个百分点，不拆开就看不出钱花在哪
            // （实测呈现占 80% 以上：重画一页的字形是大头，脏页多的时候更明显）。
            for (int i = 0; i < 2; ++i) {
                const char* tag =
                    (i == 0) ? "尾部敲 1 字（后缀原样复用）" : "中部撑出一行（后缀平移复用）";
                e.setCaret(i == 0 ? 195 : 100, 5);
                const auto a0 = std::chrono::steady_clock::now();
                e.typeTextUtf8(i == 0 ? "\xE6\xB5\x8B" : wrap_text);
                const auto a1 = std::chrono::steady_clock::now();
                e.relayout();
                const auto a2 = std::chrono::steady_clock::now();
                e.repaint();
                const auto a3 = std::chrono::steady_clock::now();
                const EditStats d = e.editStats();
                const EngineStats g = e.stats();
                std::printf(
                    "     分段：%s ｜ 编辑 %.3f ｜ 重排 %.3f（排版 %.3f + 页指纹 %.3f）｜ 呈现 "
                    "%.3f（贴图 %.3f 填 %.3f 字形 %.3f）｜ 脏页 %d 重画页 %d\n",
                    tag, std::chrono::duration<double, std::milli>(a1 - a0).count(),
                    std::chrono::duration<double, std::milli>(a2 - a1).count(), d.layout_ms,
                    g.page_fp_ms, std::chrono::duration<double, std::milli>(a3 - a2).count(),
                    g.paint_present_ms, g.paint_fill_ms, g.paint_glyph_ms, d.dirty_page_count,
                    d.repainted_pages);
                restore();
            }

            // 平移复用省掉了什么：同一个编辑，后缀里塞一个竖排段就把平移路径关掉，
            // 于是后缀必须真重排。两边各测 3 次取最小值（最小值对负载尖峰最稳），
            // 报出的两个数就是"后缀重排"与"后缀平移"的对照。
            std::vector<BlockSpec> with_vertical = pristine;
            {
                BlockSpec v;
                v.font_index = 0;
                v.size_pt = 14.0f;
                v.line_factor = 1.9f;
                v.wm = WritingMode::VerticalRL();
                v.language = "zh";
                v.setText("附：本页为竖排附注，与上面的横排正文无关。");
                with_vertical.push_back(std::move(v));
            }
            std::vector<double> v_shift, v_resume;
            for (int i = 0; i < 3; ++i) {
                e.setCaret(100, 5);
                e.typeTextUtf8(wrap_text);
                const auto s0 = std::chrono::steady_clock::now();
                e.relayout();
                const auto s1 = std::chrono::steady_clock::now();
                v_shift.push_back(std::chrono::duration<double, std::milli>(s1 - s0).count());
                restore();

                e.setBlocks(with_vertical);
                e.relayout();
                e.repaint();
                e.setCaret(100, 5);
                e.typeTextUtf8(wrap_text);
                const auto r0 = std::chrono::steady_clock::now();
                e.relayout();
                const auto r1 = std::chrono::steady_clock::now();
                v_resume.push_back(std::chrono::duration<double, std::milli>(r1 - r0).count());
                restore();
            }
            const double shift_ms = minOf3(v_shift), resume_ms = minOf3(v_resume);
            std::printf("     后缀 100 段：平移复用 %.3f ms ｜ 续排重排 %.3f ms（%.0f%%）\n",
                        shift_ms, resume_ms, 100.0 * shift_ms / std::max(resume_ms, 1e-9));
            expectTrue(shift_ms * 1.5 < resume_ms,
                       "★ 后缀平移复用比续排重排快 1.5 倍以上（断行与字形放置全省掉）");
        }

        // 边界：脏段就是第 0 段（没有前缀），但后缀仍然可以整块搬回 ——
        // "没有前缀"不等于"必须重排全篇"。
        e.setCaret(0, 0);
        e.typeTextAndRender("a");
        const EditStats first = e.editStats();
        expectTrue(!first.layout_was_full && first.relaid_paragraphs == 1,
                   "★ 在第 0 段打字也走快路：重排 1 段、复用其余全部");
        expectTrue(first.reused_paragraphs == kParas - 1, "复用 kParas - 1 段");
        expectTrue(e.auditLayout() == 0, "第 0 段的快路同样与对照一致");

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
            std::printf("     挂控件：重排 %d 段 / 复用 %d 段 / 共 %d 段（%s）\n",
                        cs.relaid_paragraphs, cs.reused_paragraphs, cs.total_paragraphs,
                        cs.layout_was_full ? "全量" : "增量");
            expectTrue(!cs.layout_was_full && cs.relaid_paragraphs == 1 &&
                           cs.reused_paragraphs == kParas - 1 - 120,
                       "★ 挂控件只重排第 120 段，其余 79 段原样复用");
            expectTrue(e.auditLayout() == 0, "挂控件之后的排版同样与对照一致");
        }
    }

    std::printf("\n[16] 增量上屏 == 从零上屏：整帧逐字节比（含脏页集合对不对）\n");
    {
        // 为什么需要这一节：auditFrame() 比的是"帧 vs 从底图整块重贴"，抓的是上屏漏贴。
        //   但如果底图自己就漏画了（页指纹判错、某个变了的页没被重画），
        //   帧与底图会一起错，auditFrame 照样是 0 —— 那一类只有拿
        //   "从零重排 + 从零重画"的第二个引擎当参照物才照得出来。
        //   判据形状与 auditLayout 一致：增量的正确性由"与从零重来逐字节相同"定义。
        EditorEngine a;
        expectTrue(a.init({cjkPath, mnPath, latinPath}), "引擎初始化（逐帧比对）");
        EngineConfig cfg16;
        cfg16.zoom = 1.0f;
        a.setConfig(cfg16);

        std::vector<BlockSpec> doc;
        for (int i = 0; i < 60; ++i) {
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
        a.setBlocks(doc);
        a.relayout();
        a.repaint();

        const std::vector<std::string> fonts16{cjkPath, mnPath, latinPath};
        std::vector<Control> no_ctrl;
        // 判据两档，都写在断言里（不接受"看着差不多就放行"）：
        //   严格档：逐字节相同。没有重算坐标的路径（敲字、光标移动、表单标记）必须落在这一档。
        //   抗锯齿档：只给"平移复用"用。平移的坐标是"旧值 + 偏移量"，比全量重排的
        //     "从 cursor_y 重新累加"多一次舍入（实测 ≤ 6e-5 pt，见 auditLayout 的容差说明），
        //     于是会在抗锯齿边界上翻掉几个像素、每个像素差 1 个通道值。
        //     阈值取"差异像素 ≤ 十万分之一个像素、且通道差 ≤ 1"：
        //     真正的错位（整页没重画、后缀挪错行）是几十万像素、通道差上百，逃不掉。
        auto compare = [&](const char* label, const std::vector<Control>& ctrl, EditMode mode,
                           bool allow_aa_noise) {
            long long max_ch = 0;
            const long long diff = frameDiffAgainstFresh(a, fonts16, ctrl, mode, &max_ch);
            const long long total_px = static_cast<long long>(a.frameBytes() / 4);
            const bool ok = allow_aa_noise ? (diff >= 0 && diff * 100000 <= total_px && max_ch <= 1)
                                           : (diff == 0);
            std::printf("     %s：帧差 %lld 像素（最大通道差 %lld，阈值 %s）\n", label, diff,
                        max_ch, allow_aa_noise ? "≤十万分之一像素且通道差≤1" : "0");
            expectTrue(ok,
                       std::string("★ ") +
                           (allow_aa_noise ? "增量上屏 ≈ 从零上屏（平移的坐标末位差只翻抗锯齿）："
                                           : "增量上屏 == 从零上屏（逐字节相同）：") +
                           label);
        };

        // 一：敲一个字（后缀原样复用）
        a.setCaret(30, 5);
        a.typeTextAndRender("\xE6\xB5\x8B");
        compare("敲 1 字", no_ctrl, EditMode::Document, /*allow_aa_noise=*/false);

        // 二：撑出一行（后缀平移复用）
        a.setCaret(30, 5);
        a.typeTextAndRender(std::string(80, 'a'));
        compare("撑出一行", no_ctrl, EditMode::Document, /*allow_aa_noise=*/true);

        // 三：退格（内容变少，页可能变少）
        a.backspaceAndRender();
        compare("退格", no_ctrl, EditMode::Document, /*allow_aa_noise=*/true);

        // 四：只移动光标（不重排：底图一个字都不该动，但上屏矩形必须对）
        a.moveCaretAndRender(-1);
        compare("光标左移", no_ctrl, EditMode::Document, /*allow_aa_noise=*/true);

        // 五：表单模式 + 控件（括号画在帧上、底图里没有 —— 上屏矩形要把它算进去）
        {
            const size_t len = a.blocks()[20].cp_text.size();
            Control k1;
            k1.range.start = TextPos{20, 0};
            k1.range.end = TextPos{20, std::min<size_t>(8, len)};
            k1.id = "k1";
            std::vector<Control> ctrl{k1};
            a.setCaret(20, 4);
            a.setControls(ctrl);
            a.setEditMode(EditMode::Form);
            a.repaint();
            compare("表单括号", ctrl, EditMode::Form, /*allow_aa_noise=*/true);
        }
    }

    std::printf("\n[17] 跨段编辑：回车分段 / 段首退格合并 / 段末 Delete 合并\n");
    {
        const std::vector<std::string> fonts17{cjkPath, mnPath, latinPath};
        auto mkBlocks = [](const std::vector<std::string>& texts) {
            std::vector<BlockSpec> v;
            for (const std::string& t : texts) {
                BlockSpec b;
                b.font_index = 0;
                b.size_pt = 14.0f;
                b.line_factor = 1.85f;
                b.space_after_pt = 6.0f;
                b.language = "zh";
                b.setText(t);
                v.push_back(std::move(b));
            }
            return v;
        };
        auto textOf = [](const EditorEngine& e, int para) {
            return e.blocks()[static_cast<size_t>(para)].textUtf8();
        };

        // 空段落有一行、且光标画得出来 —— 这是回车的落点。
        //   空段落若整段跳过（没有行盒），回车之后既看不见空行、光标也没地方画：
        //   表现就是"回车没反应"。所以"空段落占一行"是分段能用的前提。
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（空段落）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"甲乙丙", "", "丁戊己"}));
            e.relayout();
            e.repaint();
            expectTrue(e.layout().lines.size() == 3, "三个段各占一行（空段落也占一行）");
            const LineBox* mid = nullptr;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.paragraph == 1) mid = &lb;
            }
            expectTrue(mid != nullptr && mid->clusters.empty(),
                       "空段落那一行没有真实簇（只有可能存在的括号占位）");
            expectTrue(mid != nullptr && mid->block_lo_pt > e.layout().lines[0].block_lo_pt &&
                           mid->block_lo_pt < e.layout().lines[2].block_lo_pt,
                       "空行夹在前后两行之间（块轴位置正确）");
            e.setCaret(1, 0);
            Rect r{};
            expectTrue(e.caretRectPt(&r) && r.w > 0.0f && r.h > 0.0f,
                       "★ 空段落里的光标有几何（能画出来 = 回车之后看得见落点）");
        }

        // 回车分段：文本、样式、光标、翻页标记
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（分段）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"患者主诉：反复心悸3天加重1天。"}));
            e.relayout();
            e.repaint();
            const size_t total0 = e.blocks()[0].cp_text.size();
            const int lines0 = e.layout().stats.placed_line_count;

            e.setCaret(0, 5);
            expectTrue(e.splitParagraphAndRender(), "回车分段返回 true");
            expectTrue(e.blocks().size() == 2, "段数 1 → 2");
            expectTrue(textOf(e, 0) == "患者主诉：" && textOf(e, 1) == "反复心悸3天加重1天。",
                       "文本正好断在光标处（前半段 + 后半段 = 原文）");
            expectTrue(e.blocks()[0].cp_text.size() + e.blocks()[1].cp_text.size() == total0,
                       "分段不丢字符也不重复");
            expectTrue(e.caret() == TextPos{1, 0}, "光标落在新段段首");

            // 样式整体继承，只有 page_break_before 必须清掉（否则中间回车会多翻一页）。
            const BlockSpec& h = e.blocks()[0];
            const BlockSpec& t = e.blocks()[1];
            expectTrue(t.font_index == h.font_index && t.size_pt == h.size_pt &&
                           t.line_factor == h.line_factor && t.space_after_pt == h.space_after_pt &&
                           t.language == h.language && t.wm.inline_axis == h.wm.inline_axis &&
                           t.wm.block_flow == h.wm.block_flow && t.color.r == h.color.r &&
                           t.color.g == h.color.g && t.color.b == h.color.b,
                       "★ 新段整体继承原段的样式（字体/字号/行距/段后距/书写模式/颜色）");
            expectTrue(!t.page_break_before, "★ 新段不带 page_break_before（回车不该多翻一页）");

            expectTrue(e.layout().stats.placed_line_count == lines0 + 1,
                       "分段后行数 +1（段落分隔符占一行）");
            expectTrue(e.auditLayout() == 0, "分段之后增量排版 == 全量重排（逐字段）");
            expectTrue(frameDiffAgainstFresh(e, fonts17, {}, EditMode::Document, nullptr) == 0,
                       "★ 分段之后增量上屏 == 从零上屏（逐字节）");

            // 分段之后继续打字：新段是可编辑的段落
            e.typeTextAndRender("前");
            expectTrue(textOf(e, 1).substr(0, 3) == "前" && e.caret().para == 1,
                       "分段之后可以在新段继续输入");
            expectTrue(e.auditLayout() == 0, "继续输入之后排版仍与全量一致");

            // 段首退格把它并回去（往返：分段 + 合并 = 原样）
            e.setCaret(1, 0);
            expectTrue(e.backspaceAndRender(), "段首退格 = 并入上一段");
            expectTrue(e.blocks().size() == 1, "段数 2 → 1");
            expectTrue(e.caret() == TextPos{0, 5}, "合并之后光标落在合并点（原断点处）");
            expectTrue(e.auditLayout() == 0, "合并之后排版与全量一致");
            expectTrue(frameDiffAgainstFresh(e, fonts17, {}, EditMode::Document, nullptr) == 0,
                       "★ 合并之后增量上屏 == 从零上屏（逐字节）");
        }

        // 分段的排版粒度：前缀仍复用、后缀一律不复用。
        //   要有一段真正的前缀才谈得上这件事（单段文档在第 0 段分段时没有前缀，只能全量）。
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（分段粒度）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"第一段内容甲乙丙丁。", "第二段内容戊己庚辛。",
                                  "第三段内容壬癸子丑。", "第四段内容寅卯辰巳。"}));
            e.relayout();
            e.repaint();
            e.setCaret(2, 5);
            expectTrue(e.splitParagraphAndRender(), "在第 2 段中间回车分段");
            const EditStats sp = e.editStats();
            std::printf("     4 段文档在第 2 段分段：重排 %d 段 / 复用 %d 段 / 共 %d 段（%s）\n",
                        sp.relaid_paragraphs, sp.reused_paragraphs, sp.total_paragraphs,
                        sp.layout_was_full ? "全量" : "增量");
            expectTrue(
                !sp.layout_was_full && sp.reused_paragraphs == 0 && sp.relaid_paragraphs == 3,
                "★ 段数变了：[0,2) 前缀复用，[2,5) 续排，后缀不复用（段号已经错位）");
            expectTrue(e.auditLayout() == 0, "分段粒度下的排版仍与全量逐字段一致");
        }

        // 合并的两个入口对称：段首退格 与 段末 Delete 得到同一个文档
        {
            EditorEngine byBack, byDel;
            expectTrue(byBack.init(fonts17) && byDel.init(fonts17), "引擎初始化（两个合并入口）");
            EngineConfig c;
            c.zoom = 1.0f;
            byBack.setConfig(c);
            byDel.setConfig(c);
            const std::vector<std::string> texts{"第一段文字", "第二段文字"};
            byBack.setBlocks(mkBlocks(texts));
            byDel.setBlocks(mkBlocks(texts));
            byBack.relayout();
            byDel.relayout();

            byBack.setCaret(1, 0);
            expectTrue(byBack.backspace(), "段首退格");
            byDel.setCaret(0, 9);  // 第一段末尾（"第一段文字" = 5 个汉字 2 个… 共 7 码点）
            byDel.setCaret(0, static_cast<size_t>(byDel.blocks()[0].cp_text.size()));
            expectTrue(byDel.deleteForward(), "段末 Delete");
            expectTrue(byBack.blocks().size() == 1 && byDel.blocks().size() == 1,
                       "两个入口都让段数 -1");
            expectTrue(byBack.blocks()[0].cp_text == byDel.blocks()[0].cp_text,
                       "★ 两个入口得到同一段文本（对称）");
            expectTrue(byBack.caret() == byDel.caret(), "两个入口的光标落点也相同");
        }

        // 边界与策略
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（边界）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"甲", "乙"}));
            e.relayout();

            e.setCaret(0, 0);
            e.backspace();  // 首段段首：没有上一段
            expectTrue(e.blocks().size() == 2 && e.blockedEdits() == 0,
                       "首段段首退格无效，且不计入「被策略拒绝」——「没有可合并的」不是「不许改」");

            e.setCaret(1, 1);  // 末段段末
            e.deleteForward();
            expectTrue(e.blocks().size() == 2, "末段段末 Delete 无效（没有下一段）");

            // 只读模式：分段与合并都被拒
            e.setEditMode(EditMode::ReadOnly);
            const int b0 = e.blockedEdits();
            e.setCaret(0, 0);
            expectTrue(!e.splitParagraph(), "只读模式：回车分段被拒");
            e.setCaret(1, 0);
            expectTrue(!e.backspace(), "只读模式：段首退格（合并）被拒");
            expectTrue(e.blockedEdits() == b0 + 2, "两次被拒都记在 blocked_edits 上");
            e.setEditMode(EditMode::Document);
        }

        // 表单策略：断点能不能写，由"插入策略"决定（回车 = 在断点插入一个段落分隔符）
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（表单策略）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"甲", "乙"}));
            e.relayout();

            // 只读字段盖住第 0 段、可编辑字段盖住第 1 段。这样安排的用处：
            //   合并点 (0,1) 落在只读字段里（→ 该拒），而"光标停在第 1 段段首"本身是
            //   可放的位置（→ 能走到判定那一步）。
            std::vector<Control> ctrls;
            Control ro;
            ro.range.start = TextPos{0, 0};
            ro.range.end = TextPos{0, 1};
            ro.read_only = true;
            ro.id = "ro";
            ctrls.push_back(ro);
            Control ed;
            ed.range.start = TextPos{1, 0};
            ed.range.end = TextPos{1, 1};
            ed.id = "ed";
            ctrls.push_back(ed);
            e.setControls(ctrls);
            e.setEditMode(EditMode::Form);

            e.setCaret(1, 0);
            const long long b1 = e.blockedEdits();
            expectTrue(!e.backspace(), "★ 表单模式：合并点落在只读字段里 → 被拒");
            expectTrue(e.blockedEdits() == b1 + 1 && e.blocks().size() == 2,
                       "被拒并计数，且文档一个字都没动");
            expectTrue(e.splitParagraph(), "表单模式：在可编辑字段里回车分段 → 放行");
            expectTrue(e.blocks().size() == 3, "确实断成了三段（空段 + 甲 + 乙）");
        }

        // 策略层直接测：把"能不能在这里分段/合并"与光标能否到达分开。
        //   表单模式下 setCaret 会把光标投影进最近的字段，所以"光标停在所有控件之外"
        //   这件事在表单模式里根本到不了 —— 想验"控件之外不许写"只能直接调策略函数。
        {
            ControlSet cs;
            Control a;
            a.range.start = TextPos{0, 0};
            a.range.end = TextPos{0, 2};
            a.id = "a";
            cs.add(a);
            Control r;
            r.range.start = TextPos{1, 0};
            r.range.end = TextPos{1, 2};
            r.read_only = true;
            r.id = "r";
            cs.add(r);
            cs.normalize();

            expectTrue(EditPolicy::canSplit(cs, EditMode::Form, TextPos{0, 1}),
                       "可编辑字段之内：可以分段");
            expectTrue(!EditPolicy::canSplit(cs, EditMode::Form, TextPos{2, 0}),
                       "所有控件之外：不可以分段");
            expectTrue(!EditPolicy::canSplit(cs, EditMode::Form, TextPos{1, 1}),
                       "只读字段之内：不可以分段");
            expectTrue(!EditPolicy::canSplit(cs, EditMode::ReadOnly, TextPos{0, 1}),
                       "只读模式：一律不可以分段");
            expectTrue(EditPolicy::canSplit(cs, EditMode::Document, TextPos{2, 0}),
                       "文档模式：控件之外也能分段");
            expectTrue(EditPolicy::canMerge(cs, EditMode::Form, TextPos{0, 2}),
                       "合并点落在可编辑字段末尾：可以合并");
            expectTrue(!EditPolicy::canMerge(cs, EditMode::Form, TextPos{1, 2}),
                       "合并点落在只读字段末尾：不可以合并");
            expectTrue(EditPolicy::canMerge(cs, EditMode::Document, TextPos{1, 2}),
                       "文档模式：合并点在哪都行");
        }

        // 控件区间的适配：分段把区间拆到两段，合并再拼回来（往返必须回到原样）
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（控件适配）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(mkBlocks({"甲乙丙丁戊己庚辛壬癸"}));  // 10 个汉字
            e.relayout();

            std::vector<Control> ctrls;
            auto add = [&](int ps, size_t po, int pe, size_t eo, const char* id) {
                Control k;
                k.range.start = TextPos{ps, po};
                k.range.end = TextPos{pe, eo};
                k.id = id;
                ctrls.push_back(k);
            };
            add(0, 0, 0, 4, "before");  // 断点之前
            add(0, 4, 0, 8, "across");  // 跨断点
            add(0, 9, 0, 10, "after");  // 断点之后
            add(0, 5, 0, 5, "empty");   // 恰好落在断点上的空字段
            e.setControls(ctrls);
            e.setEditMode(EditMode::Form);

            e.setCaret(0, 5);
            expectTrue(e.splitParagraph(), "在 (0,5) 处分段");
            const ControlSet& c1 = e.controls();
            auto at = [&](const char* id) -> const Control& {
                for (int i = 0; i < c1.size(); ++i) {
                    if (c1.at(i).id == id) return c1.at(i);
                }
                static Control none;
                return none;
            };
            std::printf(
                "     分段后：before=[%d:%zu,%d:%zu] across=[%d:%zu,%d:%zu] "
                "after=[%d:%zu,%d:%zu] empty=[%d:%zu,%d:%zu]\n",
                at("before").range.start.para, at("before").range.start.offset,
                at("before").range.end.para, at("before").range.end.offset,
                at("across").range.start.para, at("across").range.start.offset,
                at("across").range.end.para, at("across").range.end.offset,
                at("after").range.start.para, at("after").range.start.offset,
                at("after").range.end.para, at("after").range.end.offset,
                at("empty").range.start.para, at("empty").range.start.offset,
                at("empty").range.end.para, at("empty").range.end.offset);
            auto rangeIs = [&](const char* id, int sp, size_t so, int ep, size_t eo) {
                const ControlRange& r = at(id).range;
                return r.start.para == sp && r.start.offset == so && r.end.para == ep &&
                       r.end.offset == eo;
            };
            expectTrue(rangeIs("before", 0, 0, 0, 4), "断点之前的字段原样不动");
            expectTrue(rangeIs("across", 0, 4, 1, 3),
                       "★ 跨断点的字段自动跟着断到两段（内容一点没少）");
            expectTrue(rangeIs("after", 1, 4, 1, 5),
                       "断点之后的字段整段后移（段号 +1、偏移 -= 断点）");
            expectTrue(rangeIs("empty", 1, 0, 1, 0),
                       "落在断点上的空字段归新段（与光标落在新段段首同一口径）");

            // 合并回来：四个区间必须逐字段回到原样
            e.setCaret(1, 0);
            expectTrue(e.backspace(), "把刚拆开的两段合回去");
            bool same = true;
            for (int i = 0; i < c1.size(); ++i) {
                const ControlRange& a = c1.at(i).range;
                const ControlRange& b = e.controls().at(i).range;
                if (a.start.para != b.start.para || a.start.offset != b.start.offset ||
                    a.end.para != b.end.para || a.end.offset != b.end.offset) {
                    same = false;
                }
            }
            expectTrue(same, "★ 分段 + 合并 = 控件区间逐字段回到原样（往返不变）");
            expectTrue(e.auditLayout() == 0, "控件适配之后排版仍与全量一致");
        }

        // 回车在长文档里的代价：与"撑出一行"同一量级（后缀要续排、后缀的页都要重画），
        //   但它不是整篇重排：前缀一个字都没动。
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（长文档回车）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            std::vector<BlockSpec> doc;
            for (int i = 0; i < 200; ++i) {
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

            double best_ms = 1e9;
            EditStats first{};  // 账目只认第一次分段之后那一份（后面几次段数还在涨）
            for (int i = 0; i < 3; ++i) {
                e.setCaret(100 - i, 5);
                const auto t0 = std::chrono::steady_clock::now();
                e.splitParagraphAndRender();
                const auto t1 = std::chrono::steady_clock::now();
                if (i == 0) first = e.editStats();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                if (ms < best_ms) best_ms = ms;
            }
            std::printf(
                "     200 段文档在第 100 段回车：端到端 %.3f ms（3 次取最小）｜ 重排 %.3f ms"
                "（重排 %d 段 / 复用 %d 段）｜ 脏页 %d / 总页 %d\n",
                best_ms, first.layout_ms, first.relaid_paragraphs, first.reused_paragraphs,
                first.dirty_page_count, first.total_pages);
            expectTrue(first.total_paragraphs == 201 && !first.layout_was_full &&
                           first.reused_paragraphs == 0 && first.relaid_paragraphs == 101,
                       "★ 长文档里回车：前缀 [0,100) 复用、后缀 [100,201) 续排（不是整篇重排）");
        }

        // 文档模式下的多段编辑序列：每一步都要"增量 == 全量"
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（随机跨段编辑）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            e.setBlocks(
                mkBlocks({"第一段：患者诉心悸。", "第二段：休息后缓解。", "第三段：无晕厥。"}));
            e.relayout();
            e.repaint();

            uint32_t seed = 20261010u;
            auto rnd = [&](uint32_t n) {
                seed = seed * 1103515245u + 12345u;
                return n ? (seed >> 16) % n : 0u;
            };
            int bad = 0, splits = 0, merges = 0, full = 0, expect_full = 0, full_with_prefix = 0;
            for (int step = 0; step < 40; ++step) {
                const int np = static_cast<int>(e.blocks().size());
                const int lines_before = e.layout().stats.placed_line_count;
                const int p = static_cast<int>(rnd(static_cast<uint32_t>(np)));
                const size_t len = e.blocks()[static_cast<size_t>(p)].cp_text.size();
                const uint32_t what = rnd(4);
                int dirty = p;  // 这一步会弄脏哪一段（决定"有没有前缀可复用"）
                bool edited = true;
                if (what == 0) {
                    e.setCaret(p, static_cast<size_t>(rnd(static_cast<uint32_t>(len + 1))));
                    edited = e.splitParagraphAndRender();
                    if (edited) ++splits;
                } else if (what == 1 && np > 1) {
                    e.setCaret(p, 0);
                    edited = e.backspaceAndRender();
                    if (edited) ++merges;
                    dirty = p - 1;  // 合并脏的是前一段
                } else if (what == 2 && len > 0) {
                    e.setCaret(p, static_cast<size_t>(1 + rnd(static_cast<uint32_t>(len))));
                    edited = e.backspaceAndRender();
                } else {
                    e.setCaret(p, static_cast<size_t>(rnd(static_cast<uint32_t>(len + 1))));
                    edited = e.typeTextAndRender(rnd(2) ? "文" : "a");
                }
                // 没改成的步子（比如在第 0 段段首退格：没有上一段）根本不触发重排，
                //   不参与"全量步数"的计数 —— 否则账目会凭空多出几步。
                if (!edited) continue;
                // 整篇重排的充要条件（两侧都数，比"全量很少"强得多）：脏段之前没有前缀
                //   可复用（dirty == 0），并且"后缀复用"不成立。后者有三种情形：
                //   段数变了、行结构变了（行数跟着变）、以及根本没有后缀（文档只剩这一段）。
                //   只改字而行结构没变时，第 0 段的后缀照样能整块复用（重排 1 段就够）。
                const int n_after = static_cast<int>(e.blocks().size());
                const bool struct_changed =
                    n_after != np || e.layout().stats.placed_line_count != lines_before;
                const bool has_suffix = dirty >= 0 && dirty + 1 < n_after;
                if (dirty <= 0 && (struct_changed || !has_suffix)) ++expect_full;
                if (e.editStats().layout_was_full) {
                    ++full;
                    if (dirty > 0) ++full_with_prefix;  // 有前缀还整篇重排 = 账目错了
                }
                if (e.auditLayout() != 0) ++bad;
            }
            std::printf(
                "     随机 40 步（含跨段编辑）：分段 %d 次 / 合并 %d 次 / 整篇重排 %d 步"
                "（应恰好 %d 步）｜ 与全量不符 %d 步\n",
                splits, merges, full, expect_full, bad);
            expectTrue(splits > 0 && merges > 0, "随机序列确实走到了分段与合并");
            expectTrue(bad == 0, "★ 40 步随机编辑（含跨段）每一步的增量排版都等于全量");
            expectTrue(full_with_prefix == 0,
                       "★ 有前缀可复用的步子从不整篇重排（跨段编辑不放大代价）");
            expectTrue(full == expect_full,
                       "★ 整篇重排恰好发生在「脏段为第 0 段且段数/行数变了」的那几步");
            expectTrue(frameDiffAgainstFresh(e, fonts17, {}, EditMode::Document, nullptr) == 0,
                       "★ 随机跨段编辑之后，增量上屏 == 从零上屏（逐字节）");
        }
    }

    std::printf("\n[18] 相邻字段共享边界：打字、删空都不许把括号画错位\n");
    {
        //  报过的现象：两个相邻控件，在第一个控件末尾打一个字，括号又变成 []
        //  （截图：患者主诉：[反悸[▏]三天]，[加重一天]。「▏」是光标）。
        //
        //  根因不在排版而在区间伸缩：共享边界 at 同时是 A 的末尾与 B 的开头，两条端点
        //  规则同时命中 —— A 的终点 >= at 右移（字进 A），B 的起点要 > at 才右移
        //  （于是 B 的起点原地不动）→ 插入的那个字同时落进两个控件：A=[5,10)、B=[9,12)
        //  重叠。排版于是看到"B 从 9 开始"（左括号插在 悸 之后）、"A 结束于 10"
        //  （右括号插在那个字之后），画出来正是 悸[字]三。
        //
        //  所以 [14] 只钉住了"静态相邻"，没钉住"相邻之后再打字"这一格。
        //  判据三层：区间不重叠（结构性）、共享边界上的占位是 [闭, 开]、像素上亮蓝的
        //  右括号整个在灰的左括号左边（与 [14] 同一把尺子，不依赖字形实现）。
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig c18;
        c18.zoom = 1.5f;
        e.setConfig(c18);

        BlockSpec b18;
        b18.font_index = 0;
        b18.size_pt = 12.0f;
        b18.wm = WritingMode::HorizontalTB();
        b18.line_factor = 1.75f;
        b18.language = "zh";
        b18.setText("患者主诉：反复心悸三天，加重一天。");
        e.setBlocks({b18});
        e.relayout();

        Control ka;
        ka.range.start = TextPos{0, 5};
        ka.range.end = TextPos{0, 9};  // 反复心悸
        ka.id = "a";
        Control kb;
        kb.range.start = TextPos{0, 9};  // 共享边界：A 的末尾 == B 的开头
        kb.range.end = TextPos{0, 11};   // 三天（只读）
        kb.id = "b";
        kb.read_only = true;
        e.setControls({ka, kb});
        e.setEditMode(EditMode::Form);
        e.setCaret(0, 9);
        e.repaint();

        expectTrue(e.focusedControlIndex() == 0,
                   "光标停在共享边界上时归前一个字段（末尾算在区间内）");
        expectTrue(e.typeTextAndRender("X"), "在共享边界上打字被放行（不是被拒）");

        const Control& a2 = e.controls().at(0);
        const Control& b2 = e.controls().at(1);
        std::printf("     打字后区间：A=[%zu,%zu)  B=[%zu,%zu)\n", a2.range.start.offset,
                    a2.range.end.offset, b2.range.start.offset, b2.range.end.offset);

        expectTrue(
            a2.range.end.offset == 10 && b2.range.start.offset == 10 && b2.range.end.offset == 12,
            "字进了前一个字段（A 变长到 10），后一个字段整体右移（B 从 10 开始）");
        expectTrue(a2.range.end.offset <= b2.range.start.offset,
                   "★ 两段区间不重叠 —— 重叠就是那个 `[]` 的根因（字同时属于两个字段）");

        // (b) 排版：新边界（10）上是 [闭, 开]，旧边界（9）上什么都不该剩
        int n_at9 = 0, n_at10 = 0, first10 = -1, second10 = -1;
        float close_x = -1.0f, open_x = -1.0f;
        for (const LineBox& lb : e.layout().lines) {
            if (lb.paragraph != 0) continue;
            for (const LineCluster& c : lb.clusters) {
                if (!c.placeholder) continue;
                if (c.cp == 9) ++n_at9;
                if (c.cp != 10) continue;
                ++n_at10;
                if (first10 < 0)
                    first10 = static_cast<int>(c.bracket_side);
                else
                    second10 = static_cast<int>(c.bracket_side);
                const float x = lb.inline_origin_pt + c.inline_pt;
                if (c.bracket_side == BracketSide::Close)
                    close_x = x;
                else
                    open_x = x;
            }
        }
        std::printf(
            "     占位：旧边界 9 上 %d 个 ｜ 新边界 10 上 %d 个，顺序=[%d,%d]，"
            "闭槽 x=%.2f 开槽 x=%.2f\n",
            n_at9, n_at10, first10, second10, close_x, open_x);
        expectTrue(n_at9 == 0, "旧边界 9 上不再有占位（字进了 A，A 的右括号跟着挪到 10）");
        expectTrue(n_at10 == 2 && first10 == static_cast<int>(BracketSide::Close) &&
                       second10 == static_cast<int>(BracketSide::Open),
                   "★ 新共享边界上的占位顺序仍是 [闭, 开] —— 即 `][`");
        expectTrue(close_x > 0.0f && open_x > close_x, "闭括号的槽在开括号的槽左边");

        // (c) 像素：亮蓝的右括号整个在灰的左括号左边（与 [14] 同源判据）
        {
            e.repaint();
            const std::vector<uint8_t> f(e.frame(), e.frame() + e.frameBytes());
            const float zoom = e.config().zoom;
            const int fw = e.frameWidth();
            const int x0 = static_cast<int>(std::lround(close_x * zoom)) - 2;
            const float slot_len = open_x - close_x;
            const int x1 = static_cast<int>(std::lround((open_x + slot_len - 1.0f) * zoom));
            int blue_lo = -1, blue_hi = -1, gray_lo = -1, gray_hi = -1;
            scanColumns(f, fw, x0, x1, isBracketBlue, &blue_lo, &blue_hi);
            scanColumns(f, fw, x0, x1, isBracketGray, &gray_lo, &gray_hi);
            std::printf("     窗口 x ∈ [%d, %d]：亮蓝 %d..%d ｜ 灰 %d..%d\n", x0, x1, blue_lo,
                        blue_hi, gray_lo, gray_hi);
            expectTrue(blue_lo >= 0 && gray_lo >= 0,
                       "窗口里两种颜色的括号都在（蓝的 = A 的右括号、灰的 = B 的左括号）");
            expectTrue(blue_hi > 0 && blue_hi < gray_lo,
                       "★ 亮蓝 ] 整个在灰 [ 左边 —— 相邻字段是 ][，不是那份报过的 []");
        }

        // (d) 紧挨着 A 末尾的空字段：它是这个位置上的最内层命中（空区间被 A 包住），
        //     所以字进的是空字段，A 反而要留在原地（终点 == at 且不是主人 → 不动）。
        //     判据仍是"不重叠 + 区间合法"这两条硬约束，而不是"谁该拿到这个字"。
        {
            EditorEngine e4;
            if (!e4.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e4.setConfig(c18);
            e4.setBlocks({b18});
            e4.relayout();
            Control k1;
            k1.range.start = TextPos{0, 5};
            k1.range.end = TextPos{0, 9};
            k1.id = "a";
            Control k2;  // 空字段，紧挨着 A 的末尾
            k2.range.start = TextPos{0, 9};
            k2.range.end = TextPos{0, 9};
            k2.id = "empty";
            e4.setControls({k1, k2});
            e4.setEditMode(EditMode::Form);
            e4.setCaret(0, 9);
            const int owner4 = e4.focusedControlIndex();
            e4.typeTextAndRender("Y");
            const Control& q1 = e4.controls().at(0);
            const Control& q2 = e4.controls().at(1);
            std::printf("     空字段相邻：主人=#%d  A=[%zu,%zu)  空字段=[%zu,%zu)\n", owner4,
                        q1.range.start.offset, q1.range.end.offset, q2.range.start.offset,
                        q2.range.end.offset);
            expectTrue(owner4 == 1, "空字段是这个位置上的最内层命中（区间被 A 包住）");
            expectTrue(q2.range.start.offset == 9 && q2.range.end.offset == 10,
                       "字进了空字段（它成了 [9,10)）");
            expectTrue(q1.range.start.offset == 5 && q1.range.end.offset == 9,
                       "A 不许跟着长：它的终点正好在 at 处，新字已经是空字段的第一个字了 ——"
                       "跟着长就等于一个字同时属于两个字段（括号会画成 [A[Y]]）");
            expectTrue(!spansCross(q1.range, q2.range),
                       "★ 两段区间不相交（相邻而不是嵌套）—— 渲染出来就是 [反复心悸][Y]");
        }

        // (e) 空控件（start == end）的两端必须同进同退，否则会写出 start > end 的非法区间。
        //     这一格在引擎的常规编辑路径上到不了（空控件在它自己的位置上永远是最内层命中、
        //     也就是吸收者），所以直接调区间伸缩函数把这条守护钉住：两个区间完全相同的空
        //     控件里，只有下标小的那个吸收，另一个整对端点右移、保持为空。
        {
            ControlSet cs;
            Control e1;
            e1.range.start = TextPos{0, 9};
            e1.range.end = TextPos{0, 9};
            e1.id = "e1";
            Control e2;
            e2.range.start = TextPos{0, 9};
            e2.range.end = TextPos{0, 9};
            e2.id = "e2";
            cs.add(e1);
            cs.add(e2);
            cs.normalize();
            adjustControlsOnInsert(cs, 0, 9, 1);
            std::printf("     两个相同的空控件：e1=[%zu,%zu)  e2=[%zu,%zu)\n",
                        cs.at(0).range.start.offset, cs.at(0).range.end.offset,
                        cs.at(1).range.start.offset, cs.at(1).range.end.offset);
            expectTrue(cs.at(0).range.start.offset == 9 && cs.at(0).range.end.offset == 10,
                       "吸收者的空区间被新字填上（9 → 10）");
            expectTrue(cs.at(1).range.start.offset == 10 && cs.at(1).range.end.offset == 10,
                       "另一个空控件整对端点右移、仍然为空（不会写成 [10,9)）");
        }

        // (f) 反向的两格：字段开头打字、以及末尾追加 —— 字都必须落进字段，
        //     这两条是既有的行为，改归属规则时最容易一并弄丢。
        {
            EditorEngine e5;
            if (!e5.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e5.setConfig(c18);
            e5.setBlocks({b18});
            e5.relayout();
            Control only;
            only.range.start = TextPos{0, 5};
            only.range.end = TextPos{0, 9};
            only.id = "a";
            e5.setControls({only});
            e5.setEditMode(EditMode::Form);

            e5.setCaret(0, 5);  // 字段开头
            e5.typeTextAndRender("Z");
            expectTrue(e5.controls().at(0).range.start.offset == 5 &&
                           e5.controls().at(0).range.end.offset == 10,
                       "在字段开头打的字属于该字段（起点留在 5，区间变长到 10）");

            const size_t tail = e5.controls().at(0).range.end.offset;
            e5.setCaret(0, tail);  // 字段末尾（末尾追加）
            e5.typeTextAndRender("W");
            expectTrue(e5.controls().at(0).range.end.offset == tail + 1,
                       "在字段末尾追加的字属于该字段（长度继续变长）");
        }

        // (g) 把第一个字段的内容全删光：它变成零宽的空字段，正好夹在"到此结束的内容"
        //     与"从此开始的内容"之间的那条缝里。空字段的一对方括号必须整对留在缝里 ——
        //     让后一个字段的左括号插进它们中间，一对 [] 就被劈成两半，画成 [[▏]
        //     （报过的第二张图：删掉文本控件的最后一个字符之后长这样）。
        {
            EditorEngine e6;
            if (!e6.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e6.setConfig(c18);
            e6.setBlocks({b18});
            e6.relayout();
            e6.setControls({ka, kb});
            e6.setEditMode(EditMode::Form);
            e6.setCaret(0, 9);  // 共享边界：A 的末尾 == B 的开头
            for (int i = 0; i < 4; ++i) {
                expectTrue(e6.backspaceAndRender(), "退格删掉 #0 末尾的一个字");
            }
            const Control& z0 = e6.controls().at(0);
            const Control& z1 = e6.controls().at(1);
            expectTrue(z0.range.start.offset == 5 && z0.range.end.offset == 5,
                       "第一个字段被删成零宽的空字段（停在 5）");
            expectTrue(z1.range.start.offset == 5, "后一个字段左移到 5（两个字段仍然紧挨着）");

            std::vector<int> sides;
            float x_open = -1.0f, x_close = -1.0f, x_next = -1.0f;
            for (const LineBox& lb : e6.layout().lines) {
                if (lb.paragraph != 0) continue;
                for (const LineCluster& c : lb.clusters) {
                    if (!c.placeholder || c.cp != 5) continue;
                    sides.push_back(static_cast<int>(c.bracket_side));
                    const float x = lb.inline_origin_pt + c.inline_pt;
                    if (c.read_only) {
                        if (c.bracket_side == BracketSide::Open) x_next = x;
                    } else if (c.bracket_side == BracketSide::Open) {
                        x_open = x;
                    } else {
                        x_close = x;
                    }
                }
            }
            std::printf(
                "     边界（偏移 5）上的占位 %zu 个：顺序=[%d,%d,%d]，空字段 x ∈ [%.2f, %.2f]，"
                "下一个字段的左括号 x=%.2f\n",
                sides.size(), sides.size() > 0 ? sides[0] : -1, sides.size() > 1 ? sides[1] : -1,
                sides.size() > 2 ? sides[2] : -1, x_open, x_close, x_next);
            expectTrue(sides.size() == 3 && sides[0] == static_cast<int>(BracketSide::Open) &&
                           sides[1] == static_cast<int>(BracketSide::Close) &&
                           sides[2] == static_cast<int>(BracketSide::Open),
                       "★ 顺序是 [空字段的开, 空字段的闭, 后一个字段的开] —— 空字段的一对连着");
            expectTrue(x_open > 0.0f && x_open < x_close && x_close < x_next,
                       "★ 空字段的一对整对留在缝里（自己先 [ 后 ]，且都在后一个字段的左括号左边）");

            // 光标也要落在这对括号之内 —— 被后面那个字段的左括号顶出去，就是那张截图的观感
            const float half = e6.config().caret_width_pt * 0.5f;
            Rect cr{};
            expectTrue(e6.caretRectPt(&cr), "空字段上的光标有矩形");
            const float caret_x = cr.x + half;
            std::printf("     光标插入点 x=%.2f\n", caret_x);
            expectTrue(caret_x > x_open && caret_x < x_next,
                       "★ 光标落在空字段的一对括号之内（没被后一个字段的左括号顶到右边）");

            // 像素证据：窗口里蓝的（空字段那一对）必须整个在灰的（只读字段的左括号）左边。
            // 窗口止于三个槽里最靠右的那个加一个槽宽 —— 不能按"顺序应为"去取 x_next：
            //   那样一旦顺序真的错了（正是要抓的情况），窗口会短一截，把跑到右边的那个
            //   蓝括号关在窗口外，断言反而变绿。
            e6.repaint();
            const std::vector<uint8_t> f6(e6.frame(), e6.frame() + e6.frameBytes());
            const float zoom6 = e6.config().zoom;
            const float x_last = std::max(x_close, x_next);
            const int w0 = static_cast<int>(std::lround(x_open * zoom6)) - 2;
            const int w1 =
                static_cast<int>(std::lround((x_last + 4.055f + kControlBracketSlotGapPt) * zoom6));
            int blue_lo = -1, blue_hi = -1, gray_lo = -1, gray_hi = -1;
            scanColumns(f6, e6.frameWidth(), w0, w1, isBracketBlue, &blue_lo, &blue_hi);
            scanColumns(f6, e6.frameWidth(), w0, w1, isBracketGray, &gray_lo, &gray_hi);
            std::printf("     窗口 x ∈ [%d, %d]：蓝 %d..%d ｜ 灰 %d..%d\n", w0, w1, blue_lo,
                        blue_hi, gray_lo, gray_hi);
            expectTrue(blue_hi > 0 && gray_lo > 0 && blue_hi < gray_lo,
                       "★ 蓝的那一对整个在灰的左括号左边 —— 被劈开的话，蓝的右括号会落到灰的右边");
        }

        // (h) 三个可编辑字段连排，把中间那个删空：边界上会有四个槽
        //     （左字段的闭、空字段的开、空字段的闭、右字段的开），顺序必须还是这个。
        //     这一格是演示样例里的"三连可编辑字段"删空中间那个之后的样子，
        //     也是"空字段夹在两个字段中间"的唯一一种排法。
        {
            EditorEngine e7;
            if (!e7.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e7.setConfig(c18);
            e7.setBlocks({b18});
            e7.relayout();
            Control m1, m2, m3;
            m1.range.start = TextPos{0, 5};
            m1.range.end = TextPos{0, 7};  // 反复
            m1.id = "m1";
            m2.range.start = TextPos{0, 7};
            m2.range.end = TextPos{0, 9};  // 心悸
            m2.id = "m2";
            m3.range.start = TextPos{0, 9};
            m3.range.end = TextPos{0, 11};  // 三天
            m3.id = "m3";
            e7.setControls({m1, m2, m3});
            e7.setEditMode(EditMode::Form);
            e7.setCaret(0, 9);
            for (int i = 0; i < 2; ++i) expectTrue(e7.backspaceAndRender(), "退格删掉中间字段的字");
            expectTrue(e7.controls().at(1).range.start == e7.controls().at(1).range.end,
                       "中间那个字段被删成零宽的空字段");
            expectTrue(e7.controls().at(2).range.start.offset == 7,
                       "右边那个字段左移，仍然紧挨着空字段");

            std::vector<int> sides8;
            std::vector<float> xs8;
            std::vector<uint8_t> ro8;
            for (const LineBox& lb : e7.layout().lines) {
                if (lb.paragraph != 0) continue;
                for (const LineCluster& c : lb.clusters) {
                    if (!c.placeholder || c.cp != 7) continue;
                    sides8.push_back(static_cast<int>(c.bracket_side));
                    xs8.push_back(lb.inline_origin_pt + c.inline_pt);
                    ro8.push_back(c.read_only ? 1 : 0);
                }
            }
            std::printf("     边界（偏移 7）上的占位 %zu 个：顺序=[%d,%d,%d,%d]\n", sides8.size(),
                        sides8.size() > 0 ? sides8[0] : -1, sides8.size() > 1 ? sides8[1] : -1,
                        sides8.size() > 2 ? sides8[2] : -1, sides8.size() > 3 ? sides8[3] : -1);
            const int kClose = static_cast<int>(BracketSide::Close);
            const int kOpen = static_cast<int>(BracketSide::Open);
            expectTrue(sides8.size() == 4 && sides8[0] == kClose && sides8[1] == kOpen &&
                           sides8[2] == kClose && sides8[3] == kOpen,
                       "★ 四个槽的顺序是 [闭(左字段), 开(空), 闭(空), 开(右字段)]");
            expectTrue(xs8.size() == 4 && xs8[0] < xs8[1] && xs8[1] < xs8[2] && xs8[2] < xs8[3],
                       "★ 四个槽沿行内轴依次排开（空字段的一对夹在中间，整对挨着）");

            Rect cr8{};
            const float half8 = e7.config().caret_width_pt * 0.5f;
            expectTrue(e7.caretRectPt(&cr8), "空字段上的光标有矩形");
            const float caret8 = cr8.x + half8;
            std::printf("     光标插入点 x=%.2f（四个槽 x ∈ [%.2f, %.2f, %.2f, %.2f]）\n", caret8,
                        xs8[0], xs8[1], xs8[2], xs8[3]);
            // 偏移 7 首先是"左边那个字段内容的右端"，所以光标落在这一组括号的最左端 ——
            //   三个字段连排也不会让它跑到空字段或右边字段的括号里。
            expectTrue(caret8 >= xs8[0] - 0.02f && caret8 <= xs8[1] + 0.02f,
                       "★ 光标画在这一组括号的最左端（位置由偏移本身定，不由后面的槽挤）");

            // 像素证据：三个字段都可编辑时括号同色，像素上分不出谁在左，所以做一次差分 ——
            //   只把中间那个改成只读（区间不变）再排一遍，两帧之间变的列就只有它那一对括号
            //   （从蓝变灰）。那一对落在哪儿于是可以逐列判。
            //   为什么不用"灰"这个颜色本身当判据：正文黑字的抗锯齿尾巴正好也落在灰带里
            //   （实测它就压在左边第一个槽之前），会把两侧的墨迹混进窗口。
            std::vector<Control> now8 = e7.controls().all();
            now8[1].read_only = true;
            e7.setControls(now8);
            e7.relayout();
            e7.repaint();
            const std::vector<uint8_t> fa(e7.frame(), e7.frame() + e7.frameBytes());
            now8[1].read_only = false;
            e7.setControls(now8);
            e7.relayout();
            e7.repaint();
            const std::vector<uint8_t> fb(e7.frame(), e7.frame() + e7.frameBytes());

            std::vector<int> changed(static_cast<size_t>(e7.frameWidth()), 0);
            for (size_t i = 0; i + 3 < fa.size(); i += 4) {
                if (fa[i] == fb[i] && fa[i + 1] == fb[i + 1] && fa[i + 2] == fb[i + 2]) continue;
                changed[static_cast<size_t>((i >> 2) % static_cast<size_t>(e7.frameWidth()))] = 1;
            }
            int ch_lo = -1, ch_hi = -1, ch_n = 0;
            for (size_t x = 0; x < changed.size(); ++x) {
                if (!changed[x]) continue;
                if (ch_lo < 0) ch_lo = static_cast<int>(x);
                ch_hi = static_cast<int>(x);
                ++ch_n;
            }
            const int px_open2 = static_cast<int>(std::lround(xs8[0] * c18.zoom));
            const int px_open3 = static_cast<int>(std::lround(xs8[3] * c18.zoom));
            std::printf(
                "     中间字段改只读带来的变化：%d 列 ∈ [%d, %d]；"
                "左字段闭括号 x=%d px，右字段开括号 x=%d px\n",
                ch_n, ch_lo, ch_hi, px_open2, px_open3);
            expectTrue(ch_n > 0 && ch_lo > px_open2 && ch_hi < px_open3,
                       "★ 变的只有中间那一对括号，且整对夹在左右两个字段的括号之间 ——"
                       "空字段的一对被劈开的话，它的右括号会落到右字段开括号的右边");
        }

        // (i) 三个可编辑字段连排、把中间那个删空之后的两条硬性质：
        //     空字段仍然"进得去、打得进字"（它零宽，正是自己位置上的最内层）；
        //     方向键不离开出发时所在的字段（邻居可编辑时，下一个格子就在隔壁字段里 ——
        //     只判"下一步可停"会放它过去，实测从 5 连按右键会走到 8、9）。
        {
            EditorEngine e8;
            if (!e8.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e8.setConfig(c18);
            e8.setBlocks({b18});
            e8.relayout();
            Control n1, n2, n3;
            n1.range.start = TextPos{0, 5};
            n1.range.end = TextPos{0, 7};  // 反复
            n1.id = "n1";
            n2.range.start = TextPos{0, 7};
            n2.range.end = TextPos{0, 9};  // 心悸
            n2.id = "n2";
            n3.range.start = TextPos{0, 9};
            n3.range.end = TextPos{0, 11};  // 三天
            n3.id = "n3";
            e8.setControls({n1, n2, n3});
            e8.setEditMode(EditMode::Form);
            e8.setCaret(0, 9);
            for (int i = 0; i < 2; ++i) e8.backspaceAndRender();

            const Control& w1 = e8.controls().at(1);
            std::printf(
                "     删空之后：#1=[%zu,%zu) depth=%d parent=%d，"
                "偏移 7 归属=%d，光标在 %zu\n",
                w1.range.start.offset, w1.range.end.offset, w1.depth, w1.parent,
                e8.controls().innermostAt(TextPos{0, 7}, true), e8.caret().offset);
            expectTrue(w1.range.start.offset == 7 && w1.range.end.offset == 7,
                       "中间那个字段被删成零宽的空字段");
            expectTrue(w1.depth == 1,
                       "★ 零宽字段被邻居按闭合区间包含 → 区间树重算后它是内层（depth 1）。"
                       "树停在编辑前那一刻的话这里还是 0，位置会被左邻居抢走");
            expectTrue(e8.controls().innermostAt(TextPos{0, 7}, true) == 1 &&
                           e8.focusedControlIndex() == 1,
                       "★ 它自己那个位置归它（最内层）—— 否则光标再也进不去、字也打不进去");

            // 打字必须进这个空字段，而不是被左边的字段抢走
            expectTrue(e8.typeTextAndRender("痛"), "在空字段的位置上打字被放行");
            const Control& x0 = e8.controls().at(0);
            const Control& x1 = e8.controls().at(1);
            const Control& x2 = e8.controls().at(2);
            std::printf("     打字之后：#0=[%zu,%zu)  #1=[%zu,%zu)  #2=[%zu,%zu)\n",
                        x0.range.start.offset, x0.range.end.offset, x1.range.start.offset,
                        x1.range.end.offset, x2.range.start.offset, x2.range.end.offset);
            expectTrue(x1.range.start.offset == 7 && x1.range.end.offset == 8,
                       "★ 字进了空字段（它成了 [7,8)）");
            expectTrue(x0.range.start.offset == 5 && x0.range.end.offset == 7,
                       "左边那个字段没有被撑大（字没有同时落进两个字段）");
            expectTrue(x2.range.start.offset == 8, "右边那个字段整体右移");

            // 方向键：一次一格，且整次移动都不许离开出发时所在的字段
            EditorEngine e9;
            if (!e9.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e9.setConfig(c18);
            e9.setBlocks({b18});
            e9.relayout();
            e9.setControls({n1, n2, n3});
            e9.setEditMode(EditMode::Form);
            e9.setCaret(0, 9);
            for (int i = 0; i < 2; ++i) e9.backspaceAndRender();

            std::vector<size_t> walk;
            int worst = 0;
            bool left_field = false;
            e9.setCaret(0, 5);
            for (int i = 0; i < 6; ++i) {
                const size_t before = e9.caret().offset;
                e9.moveCaretAndRender(1);
                const size_t now = e9.caret().offset;
                walk.push_back(now);
                worst = std::max(worst, static_cast<int>(now) - static_cast<int>(before));
                if (now < 5 || now > 7) left_field = true;  // 走出了 #0 = [5,7]
            }
            std::printf("     从 5 连按 6 次右键：");
            for (size_t o : walk) std::printf(" %zu", o);
            std::printf("（单步最大跨度 %d）\n", worst);
            expectTrue(worst <= 1, "★ 方向键一次只走一格（没有瞬移）");
            expectTrue(left_field, "★ 越过 7 之后就走出了 #0 —— 方向键不再被字段边界挡住");

            e9.setCaret(0, 8);  // 在 #2 里（删空之后 #2 = [7,9)）
            std::vector<size_t> back;
            for (int i = 0; i < 4; ++i) {
                e9.moveCaretAndRender(-1);
                back.push_back(e9.caret().offset);
            }
            std::printf("     从 8 连按 4 次左键：");
            for (size_t o : back) std::printf(" %zu", o);
            std::printf("\n");
            expectTrue(back.front() == 7 && back.back() == 4,
                       "★ 从 #2 里往左走同样是逐格退出（8 → 7 6 5 4，一路走出字段）");
        }
    }

    std::printf("\n[19] 点击归属：共享边界上点的是哪一个字段（含三层嵌套样例）\n");
    {
        // 背景：相邻两个字段共享边界时（[反复][心悸]，两枚括号并排在同一列），
        //   "前一个字段的末尾"与"后一个字段的开头"是同一个偏移。光看偏移，
        //   归属只能按"最内层、同深度取下标小者"来定 —— 恒定给前一个字段。
        //   后果：屏幕上点后一个字段的开头（它的开括号，或者它第一个字的左半边），
        //   光标虽然停住了，字却落进前一个字段；前一个字段只读时那次编辑直接被拒 ——
        //   看起来就是"这个字段点不进去"。
        //   现在：命中测试按"点到的是哪一枚括号"给出归属（占位簇自己记着 owner_control），
        //   宿主原样传给 setCaretAt，光标画在所属字段的内侧，编辑也照着这个归属落笔。
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig c19;
        c19.zoom = 1.5f;
        e.setConfig(c19);
        BlockSpec b19;
        b19.font_index = 0;
        b19.size_pt = 12.0f;
        b19.wm = WritingMode::HorizontalTB();
        b19.line_factor = 1.75f;
        b19.language = "zh";
        b19.setText("患者主诉：反复心悸三天，加重一天。");
        e.setBlocks({b19});
        e.setEditMode(EditMode::Form);
        Control t1;
        t1.range.start = TextPos{0, 5};
        t1.range.end = TextPos{0, 7};
        Control t2;
        t2.range.start = TextPos{0, 7};
        t2.range.end = TextPos{0, 9};
        Control t3;
        t3.range.start = TextPos{0, 9};
        t3.range.end = TextPos{0, 11};
        e.setControls({t1, t2, t3});
        e.relayout();

        // 独立口径见文件头的 expectedOwnerAt：点在"谁的内容框"里取最深那个。
        // 在"每个控件内容框的内部"采样（避开边缘 0.5pt：那正是相邻框相接的地方，
        //   语义本来就模糊），逐点核对归属。
        auto sweepBoxes = [&](EditorEngine& eng, int n_ctrl, const char* tag) {
            int total = 0;
            int bad = 0;
            std::string first_bad;
            for (int ci = 0; ci < n_ctrl; ++ci) {
                for (const EditorEngine::ControlRect& r : eng.controlRects(ci)) {
                    for (float x = r.r_pt.x + 0.5f; x <= r.r_pt.x + r.r_pt.w - 0.5f; x += 0.5f) {
                        const float y = r.r_pt.y + r.r_pt.h * 0.5f;
                        TextPos hp{};
                        int own = -1;
                        if (!eng.hitTestPt(x, y, &hp, &own)) continue;
                        const int want = expectedOwnerAt(eng, x, y);
                        ++total;
                        if (own != want) {
                            ++bad;
                            if (first_bad.empty())
                                first_bad = "例：x=" + std::to_string(static_cast<int>(x)) +
                                            " offset=" + std::to_string(hp.offset) +
                                            " 归属=" + std::to_string(own) +
                                            " 应为=" + std::to_string(want);
                        }
                    }
                }
            }
            std::printf("     %s：内容框内采样 %d 个点，归属错的 %d 个  %s\n", tag, total, bad,
                        first_bad.c_str());
            return bad == 0 && total > 30;
        };

        // (a) 最强的结构性判据：一个控件自己画出来的内容框，点进去必须归它自己。
        //     这一格是最典型的错法：中间那个字段内容框的左半边归了左边那个字段。
        expectTrue(sweepBoxes(e, 3, "三连相邻"),
                   "★ 点任意一个字段的内容框都归它自己 —— 共享边界不再把后一个字段吃掉");

        // (b) 同一个偏移 7，两种归属必须画在不同列上：
        //     归属前一个字段 → 贴在它的闭括号之内；归属后一个字段 → 贴在后一个字段的内容
        //     左边缘（即两枚括号之右）。这正是"光标到底进没进去"的视觉判据。
        Rect r_plain{};
        Rect r_right{};
        Rect r_left{};
        e.setCaret(0, 7);
        e.caretRectPt(&r_plain);
        e.setCaretAt(0, 7, 1);
        e.caretRectPt(&r_right);
        e.setCaretAt(0, 7, 0);
        e.caretRectPt(&r_left);
        float mid_left_edge = 0.0f;
        for (const EditorEngine::ControlRect& r : e.controlRects(1)) mid_left_edge = r.r_pt.x;
        std::printf(
            "     偏移 7 的光标 x：中性 %.2f ｜ 归#1 %.2f ｜ 归#0 %.2f"
            "（#1 内容框左缘 %.2f）\n",
            r_plain.x, r_right.x, r_left.x, mid_left_edge);
        expectTrue(r_left.x < r_right.x - 6.0f,
                   "★ 归属不同 → 画在不同列（相差超过一个槽宽，肉眼可辨）");
        // 容差 1.2pt：槽宽里含着 1.5pt 的间隙，光标落在"槽末"而内容框从"字的前缘"起算，
        //   两者差不到一个槽。判的是"进没进去"，不是亚像素对齐。
        expectTrue(std::fabs(r_right.x - mid_left_edge) < 1.2f,
                   "★ 归属后一个字段时光标落在它内容的左边缘（=`][` 之右，视觉上真的进去了）");
        expectTrue(std::fabs(r_left.x - r_plain.x) < 0.01f,
                   "不带归属时的画法与老行为一致（贴在 `]` 之内）—— 键盘/宿主 API 不受影响");

        // (c) 落笔归属：同一个位置，带不带归属，字进不同的字段。
        e.setCaretAt(0, 7, 1);
        const bool ok_right = e.insertTextUtf8(0, 7, "痛");
        std::printf("     在偏移 7 打字：带归属(#1) → %s  #0=[%zu,%zu) #1=[%zu,%zu)\n",
                    ok_right ? "成功" : "被拒", e.controls().at(0).range.start.offset,
                    e.controls().at(0).range.end.offset, e.controls().at(1).range.start.offset,
                    e.controls().at(1).range.end.offset);
        expectTrue(ok_right && e.controls().at(0).range.end.offset == 7 &&
                       e.controls().at(1).range.end.offset == 10,
                   "★ 带归属在共享边界上打字 → 字进后一个字段（前一个字段没被撑大）");

        e.setCaret(0, 7);  // 不带归属（键盘/宿主 API 的老路径）
        const bool ok_plain = e.insertTextUtf8(0, 7, "痛");
        std::printf("     同样的位置：不带归属 → %s  #0=[%zu,%zu) #1=[%zu,%zu)\n",
                    ok_plain ? "成功" : "被拒", e.controls().at(0).range.start.offset,
                    e.controls().at(0).range.end.offset, e.controls().at(1).range.start.offset,
                    e.controls().at(1).range.end.offset);
        expectTrue(ok_plain && e.controls().at(0).range.end.offset == 8,
                   "不带归属时仍按最内层判（字进 #0）—— 老行为没被顺手改掉");

        // (d) 只读的前邻居：光标能停在共享边界上（位置不受编辑权限约束），
        //     但"字进谁"仍由归属决定 —— 归 #1 才写得进去。这是"点不进去"最直白的一种。
        //     先复位文档与控件：上面的 (c) 往里插过字，区间已经整体右移了。
        {
            e.setBlocks({b19});
            e.setEditMode(EditMode::Form);
            std::vector<Control> ro = {t1, t2, t3};
            ro[0].read_only = true;  // #0 只读，#1 可编辑，边界 7 仍共享
            e.setControls(ro);
            e.relayout();
            std::printf("     #0 只读（#0=[%zu,%zu) ro=%d ｜ #1=[%zu,%zu) ro=%d）\n",
                        e.controls().at(0).range.start.offset, e.controls().at(0).range.end.offset,
                        e.controls().at(0).read_only ? 1 : 0, e.controls().at(1).range.start.offset,
                        e.controls().at(1).range.end.offset, e.controls().at(1).read_only ? 1 : 0);
            e.setCaret(0, 11);
            e.setCaret(0, 7);  // 中性：光标照样停在 7（只读约束的是编辑，不是光标）
            const size_t after_plain = e.caret().offset;
            const int owner_plain = e.focusedControlIndex();
            e.setCaretAt(0, 7, 1);  // 点的是 #1 的开括号
            const size_t after_hint = e.caret().offset;
            const int owner_hint = e.focusedControlIndex();
            std::printf("     #0 只读时点偏移 7：不带归属 caret=%zu field=%d ｜ 带归属 caret=%zu "
                        "field=%d\n",
                        after_plain, owner_plain, after_hint, owner_hint);
            expectTrue(after_plain == 7, "不带归属时光标也停在 7（位置不受控件约束）");
            expectTrue(after_hint == 7 && owner_hint == 1,
                       "★ 点可编辑字段的开括号 → 光标停在 7 且归属它（只读的左邻居不否决）");
            const bool ok = e.insertTextUtf8(0, 7, "痛");
            std::printf("     打字：%s  #0=[%zu,%zu) #1=[%zu,%zu)\n", ok ? "成功" : "被拒",
                        e.controls().at(0).range.start.offset, e.controls().at(0).range.end.offset,
                        e.controls().at(1).range.start.offset, e.controls().at(1).range.end.offset);
            expectTrue(ok && e.controls().at(1).range.end.offset == 10 &&
                           e.controls().at(0).range.end.offset == 7,
                       "★ 并且字真的进得去（只读字段一个字都没被改）");
        }

        // (e) 三层嵌套、三层都可编辑（前端 variant 3 的形状）。
        {
            EditorEngine e3;
            if (!e3.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            EngineConfig c;
            c.zoom = 1.5f;
            e3.setConfig(c);
            e3.setBlocks({b19});
            e3.setEditMode(EditMode::Form);
            Control outer;
            outer.range.start = TextPos{0, 5};
            outer.range.end = TextPos{0, 11};
            outer.id = "outer";
            Control mid;
            mid.range.start = TextPos{0, 6};
            mid.range.end = TextPos{0, 10};
            mid.id = "middle";
            Control inner;
            inner.range.start = TextPos{0, 7};
            inner.range.end = TextPos{0, 9};
            inner.id = "inner";
            e3.setControls({outer, mid, inner});
            e3.relayout();
            std::printf("     嵌套结构：");
            for (int i = 0; i < 3; ++i) {
                std::printf("#%d[%zu,%zu) depth=%d parent=%d  ", i,
                            e3.controls().at(i).range.start.offset,
                            e3.controls().at(i).range.end.offset, e3.controls().at(i).depth,
                            e3.controls().at(i).parent);
            }
            std::printf("\n");
            expectTrue(e3.controls().at(0).depth == 0 && e3.controls().at(1).depth == 1 &&
                           e3.controls().at(2).depth == 2,
                       "三层嵌套的深度是 0/1/2（区间树算出来的，不是配的）");
            expectTrue(e3.controls().at(1).parent == 0 && e3.controls().at(2).parent == 1,
                       "父链是外→中→内");

            // 归属取最内层：在内层里打字，三层一起变长（区间不许互相穿过）。
            e3.setCaret(0, 8);
            const int owner8 = e3.focusedControlIndex();
            e3.insertTextUtf8(0, 8, "痛");
            std::printf(
                "     在内层打字：owner=%d  #0=[%zu,%zu) #1=[%zu,%zu) #2=[%zu,%zu)\n", owner8,
                e3.controls().at(0).range.start.offset, e3.controls().at(0).range.end.offset,
                e3.controls().at(1).range.start.offset, e3.controls().at(1).range.end.offset,
                e3.controls().at(2).range.start.offset, e3.controls().at(2).range.end.offset);
            expectTrue(owner8 == 2, "归属取最内层（不是外层）");
            expectTrue(e3.controls().at(0).range.end.offset == 12 &&
                           e3.controls().at(1).range.end.offset == 11 &&
                           e3.controls().at(2).range.end.offset == 10,
                       "★ 改内层 → 中/外层一起变长（父子区间不许互相穿过）");
            expectTrue(e3.controls().at(0).range.start.offset == 5 &&
                           e3.controls().at(1).range.start.offset == 6 &&
                           e3.controls().at(2).range.start.offset == 7,
                       "起点都没动（在字段内部打字是「内含」伸缩）");

            // 嵌套的框是互相套着的 —— 判据不能是"我点的是哪个框"，而要按最内层：
            //   子字段的括号画在父字段的内容框里面（[外[中[内 心心]悸]三]），
            //   点在子字段的括号上当然归子字段，哪怕那个 x 还在父字段的框内。
            auto ownerAt = [&](float x, float y) {
                TextPos hp{};
                int own = -1;
                e3.hitTestPt(x, y, &hp, &own);
                return own;
            };
            // 内侧那一格：最内层内容框的中部 → 最内层（三层的框在这一格重叠，必须内层优先）
            {
                const std::vector<EditorEngine::ControlRect> br = e3.controlRects(2);
                expectTrue(!br.empty(), "内层字段投影出了内容框");
                if (!br.empty()) {
                    const float x = br[0].r_pt.x + br[0].r_pt.w * 0.5f;
                    const int own = ownerAt(x, br[0].r_pt.y + br[0].r_pt.h * 0.5f);
                    std::printf("     内层内容框中部 → 归属 %d（应为 2）\n", own);
                    expectTrue(own == 2, "★ 点最内层内容框的中部 → 归最内层（外层不吃掉它）");
                }
            }
            // 外侧那一格：只被外层覆盖的位置（外层第一个字的左半）→ 外层
            {
                const std::vector<EditorEngine::ControlRect> br = e3.controlRects(0);
                if (!br.empty()) {
                    const float x = br[0].r_pt.x + 1.0f;
                    const float y = br[0].r_pt.y + br[0].r_pt.h * 0.5f;
                    const int want = expectedOwnerAt(e3, x, y);
                    const int own = ownerAt(x, y);
                    std::printf("     外层框左端 +1pt → 归属 %d（该点只落在第 %d 个框里）\n", own,
                                want);
                    expectTrue(want == 0 && own == 0, "★ 只被外层覆盖的位置 → 归外层");
                }
            }
            // 中间那一格：外层里点中层（不在内层框内）→ 中层
            {
                const std::vector<EditorEngine::ControlRect> br = e3.controlRects(1);
                if (!br.empty()) {
                    const float x = br[0].r_pt.x + 1.0f;
                    const int own = ownerAt(x, br[0].r_pt.y + br[0].r_pt.h * 0.5f);
                    std::printf("     中层框左端 +1pt → 归属 %d（应为 1）\n", own);
                    expectTrue(own == 1, "★ 内层优先只在内层框盖住的位置生效，别处仍归中层");
                }
            }
        }
    }

    std::printf("\n[21] 控件边界总覆盖：相邻 / 嵌套 / 换行 / 删除 / 添加 / 光标\n");
    {
        // 控件边界这件事按六类边界条件系统过一遍。判据只用两种互相独立的证据：
        //   结构性证据：区间端点、区间树的 depth 与 parent、占位槽的边序与归属；
        //   几何性证据：控件投影出的内容框、光标矩形、帧上括号的像素列。
        // 两者来自不同的推算路径，任一条错都能单独照出来，不会一起错。
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig c21;
        c21.zoom = 1.5f;
        e.setConfig(c21);

        const std::string kText = "患者主诉：反复心悸三天，加重一天。";  // 17 个码点
        auto mk21 = [](size_t s, size_t en, bool ro) {
            Control c;
            c.range.start = TextPos{0, s};
            c.range.end = TextPos{0, en};
            c.read_only = ro;
            return c;
        };
        // 每条用例都从零铺一份文档与控件，避免上一条用例的区间伸缩串到下一条。
        auto reset = [&](const std::string& text, const std::vector<Control>& cs) {
            BlockSpec b;
            b.font_index = 0;
            b.size_pt = 12.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.75f;
            b.language = "zh";
            b.setText(text);
            e.setBlocks({b});
            e.setControls(cs);
            e.setEditMode(EditMode::Form);
            e.relayout();
            e.repaint();
        };
        // 一处边界上的占位槽，按渲染顺序（x 递增）给出。槽自带它所在的行号，
        //   跨行字段的括号落在哪一行可以据此直接判。
        struct SlotInfo {
            int side;
            int ro;
            int owner;
            int line;
            float x;
        };
        auto slotsAt = [&](size_t cp) {
            std::vector<SlotInfo> out;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.paragraph != 0) continue;
                for (const LineCluster& c : lb.clusters) {
                    if (!c.placeholder || c.cp != cp) continue;
                    out.push_back(SlotInfo{static_cast<int>(c.bracket_side), c.read_only ? 1 : 0,
                                           c.owner_control, lb.line,
                                           lb.inline_origin_pt + c.inline_pt});
                }
            }
            return out;
        };
        // 槽的边序写成字符串：闭写 ]、开写 [，于是 ][ / [] / [[ ]] 一眼可读。
        auto sides = [](const std::vector<SlotInfo>& v) {
            std::string s;
            for (const SlotInfo& q : v)
                s += (q.side == static_cast<int>(BracketSide::Open)) ? '[' : ']';
            return s;
        };
        // 区间两两不穿线：半开区间不许"一半重叠"。相邻与嵌套都是合法形态。
        auto noCross = [&]() {
            for (int i = 0; i < e.controls().size(); ++i)
                for (int j = i + 1; j < e.controls().size(); ++j)
                    if (spansCross(e.controls().at(i).range, e.controls().at(j).range)) return false;
            return true;
        };
        auto ranges = [&]() {
            std::string s;
            for (int i = 0; i < e.controls().size(); ++i) {
                if (i) s += " ";
                s += "[" + std::to_string(e.controls().at(i).range.start.offset) + "," +
                     std::to_string(e.controls().at(i).range.end.offset) + ")";
            }
            return s;
        };
        // 两个字段的矩形有没有真的叠在一起（面积交集非空）。
        auto rectsOverlap = [&](int a, int b) {
            for (const EditorEngine::ControlRect& ra : e.controlRects(a)) {
                for (const EditorEngine::ControlRect& rb : e.controlRects(b)) {
                    const bool ox = ra.r_pt.x < rb.r_pt.x + rb.r_pt.w - 0.01f &&
                                    rb.r_pt.x < ra.r_pt.x + ra.r_pt.w - 0.01f;
                    const bool oy = ra.r_pt.y < rb.r_pt.y + rb.r_pt.h - 0.01f &&
                                    rb.r_pt.y < ra.r_pt.y + ra.r_pt.h - 0.01f;
                    if (ox && oy) return true;
                }
            }
            return false;
        };
        // 把某个控件标成只读（其余原样）—— 用来让某一对括号变成灰色，做像素判据。
        auto withRo = [&](int idx, bool ro) {
            std::vector<Control> cs;
            for (int i = 0; i < e.controls().size(); ++i) {
                Control c = e.controls().at(i);
                c.read_only = (i == idx) ? ro : c.read_only;
                cs.push_back(c);
            }
            return cs;
        };

        std::printf("     ---- 相邻：单共享边界 ----\n");
        {
            reset(kText, {mk21(5, 9, false), mk21(9, 11, true)});
            const size_t bd = e.controls().at(0).range.end.offset;
            expectTrue(bd == e.controls().at(1).range.start.offset,
                       "前置：#0 的末尾与 #1 的开头是同一个位置（共享边界）");
            const std::vector<SlotInfo> s = slotsAt(bd);
            std::printf("     边界 %zu 上的占位：%s（%zu 个）\n", bd, sides(s).c_str(), s.size());
            expectTrue(s.size() == 2 && sides(s) == "][",
                       "★ 单共享边界的占位是 ][：闭在左、开在右（不是 []）");
            expectTrue(s.size() == 2 && s[0].owner == 0 && s[1].owner == 1,
                       "★ 两枚括号各归各的字段 —— 命中测试据此分清点的是哪一个");
            expectTrue(s.size() == 2 && s[0].ro == 0 && s[1].ro == 1,
                       "括号的颜色跟着字段走（只读的那一枚是灰的）");
            const std::vector<EditorEngine::ControlRect> r0 = e.controlRects(0);
            const std::vector<EditorEngine::ControlRect> r1 = e.controlRects(1);
            const float right0 = r0.empty() ? -1.0f : r0.back().r_pt.x + r0.back().r_pt.w;
            const float left1 = r1.empty() ? -1.0f : r1.front().r_pt.x;
            std::printf("     #0 内容框右缘 %.2f ≤ #1 内容框左缘 %.2f\n", right0, left1);
            expectTrue(right0 > 0.0f && left1 > 0.0f && right0 <= left1 + 0.01f,
                       "★ 相邻字段的内容框不重叠（重叠就是区间穿了线）");
            expectTrue(!rectsOverlap(0, 1), "★ 面积判据同上：两块矩形没有交集");
        }

        std::printf("     ---- 相邻：双共享边界（三连） ----\n");
        {
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            std::string both;
            bool both_ok = true;
            for (size_t cp : {size_t{7}, size_t{9}}) {
                const std::vector<SlotInfo> s = slotsAt(cp);
                both += " " + std::to_string(cp) + ":" + sides(s);
                if (s.size() != 2 || sides(s) != "][") both_ok = false;
            }
            std::printf("     两条边界上的占位：%s\n", both.c_str());
            expectTrue(both_ok, "★ 三连的两条共享边界都是 ][（不是 []）");
            expectTrue(!rectsOverlap(0, 1) && !rectsOverlap(1, 2) && !rectsOverlap(0, 2),
                       "★ 三个字段的内容框两两不重叠");
            e.setCaret(0, 7);
            expectTrue(e.focusedControlIndex() == 0,
                       "光标停在共享边界上时归前一个字段（末尾算在区间内）");
            expectTrue(e.typeTextAndRender("痛"), "在共享边界上打字被放行（不是被拒）");
            std::printf("     打字后区间：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,8) [8,10) [10,12)",
                       "★ 字进 #0，后两个整体右移（首尾相接）");
            expectTrue(noCross(), "★ 三连区间两两不穿线（结构性判据）");
            expectTrue(sides(slotsAt(8)) == "][", "★ 边界跟着字段端点搬家：新边界上仍是 ][");
            expectTrue(slotsAt(7).empty(), "旧边界上不再留占位（括号跟着端点走）");
        }

        std::printf("     ---- 删除文字：把三连的后两个字段删空 ----\n");
        {
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            for (int i = 2; i >= 1; --i) {
                int guard = 0;
                while (e.controls().at(i).range.end.offset >
                           e.controls().at(i).range.start.offset &&
                       guard++ < 8) {
                    e.setCaretAt(0, e.controls().at(i).range.end.offset, i);
                    if (!e.backspaceAndRender()) break;
                }
            }
            std::printf("     删空 #1 #2 之后的区间：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,7) [7,7) [7,7)",
                       "两个字段被删成零宽，都收在 7 上（内容没了，位置还在）");
            const std::vector<SlotInfo> s = slotsAt(7);
            std::string own;
            for (const SlotInfo& q : s) own += " " + std::to_string(q.owner);
            std::printf("     该处 %zu 个占位：%s ｜ 归属 %s\n", s.size(), sides(s).c_str(),
                        own.c_str());
            expectTrue(s.size() == 5 && sides(s) == "][][]",
                       "★ 两个零宽字段的括号交替成 ][][]:] 属 #0、[] 属 #1、[] 属 #2");
            expectTrue(s.size() == 5 && s[0].owner == 0 && s[1].owner == 1 && s[2].owner == 1 &&
                           s[3].owner == 2 && s[4].owner == 2,
                       "★ 五个槽各归各的字段（两个空字段没有被画成互相嵌套）");
            expectTrue(e.controls().at(1).depth == 1 && e.controls().at(2).depth == 1,
                       "★ 两个零宽字段是兄弟（depth 都是 1）—— 完全重合的区间不许互相认父");
            expectTrue(e.controls().at(1).parent == 0 && e.controls().at(2).parent == 0,
                       "两个零宽字段的父都是 #0（区间树重算过，父链不成环）");
            expectTrue(noCross(), "删空之后区间仍然两两不穿线");
            // 像素证据：把 #2 标成只读，它那对括号变灰。逐个槽量两件事：
            //   颜色（灰还是亮蓝）与墨迹的横向重心。方括号是镜像字形 —— 开括号的墨
            //   偏在槽的左半、闭括号偏在右半，所以重心能直接从像素上读出边序。
            //   这一条不看任何区间数据，只读帧，是排版结果最直接的可证伪判据。
            if (s.size() == 5) {
                e.setControls(withRo(2, true));
                e.relayout();
                e.showCaret(false);
                e.repaint();
                const std::vector<SlotInfo> sp = slotsAt(7);
                const float z = e.config().zoom;
                const float pitch = (sp.size() >= 2) ? (sp[1].x - sp[0].x) : 6.0f;
                const std::vector<uint8_t> fr(e.frame(), e.frame() + e.frameBytes());
                const int fw = e.frameWidth();
                // 只扫槽的内部（两侧各让开 0.5pt）：槽带前后紧挨着真实字形，
                //   而字形抗锯齿的中间档恰好落在灰括号的判据区间里，会让"灰"误报。
                auto inkStats = [&](float xa, float xb, bool gray, int* count, float* centroid) {
                    *count = 0;
                    *centroid = 0.0f;
                    const int c0 = static_cast<int>(std::lround(xa * z));
                    const int c1 = static_cast<int>(std::lround(xb * z));
                    double sum = 0.0;
                    for (size_t i = 0; i + 3 < fr.size(); i += 4) {
                        const int x = static_cast<int>(i >> 2) % fw;
                        if (x < c0 || x > c1) continue;
                        const uint8_t r = fr[i], g = fr[i + 1], b = fr[i + 2];
                        if (!(gray ? isBracketGray(r, g, b) : isBracketBlue(r, g, b))) continue;
                        ++(*count);
                        sum += static_cast<double>(x - c0);
                    }
                    if (*count > 0) *centroid = static_cast<float>(sum / *count);
                };
                int blue_n[5] = {0, 0, 0, 0, 0};
                int gray_n[5] = {0, 0, 0, 0, 0};
                float blue_c[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                float gray_c[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 5; ++k) {
                    const float xa = sp[static_cast<size_t>(k)].x + 0.5f;
                    const float xb = xa + pitch - 1.0f;
                    inkStats(xa, xb, false, &blue_n[k], &blue_c[k]);
                    inkStats(xa, xb, true, &gray_n[k], &gray_c[k]);
                }
                std::printf("     五个槽从左到右：亮蓝墨量 %d %d %d %d %d ｜ 灰墨量 %d %d %d %d %d\n",
                            blue_n[0], blue_n[1], blue_n[2], blue_n[3], blue_n[4], gray_n[0],
                            gray_n[1], gray_n[2], gray_n[3], gray_n[4]);
                std::printf("     亮蓝重心 %.2f %.2f %.2f ｜ 灰重心 %.2f %.2f\n", blue_c[0], blue_c[1],
                            blue_c[2], gray_c[3], gray_c[4]);
                expectTrue(blue_n[0] > 0 && blue_n[1] > 0 && blue_n[2] > 0 && gray_n[0] == 0 &&
                               gray_n[1] == 0 && gray_n[2] == 0,
                           "★ 左边三枚括号都是亮蓝的（#0 的 ] 与 #1 的一对，都可编辑）");
                expectTrue(gray_n[3] > 0 && gray_n[4] > 0 && blue_n[3] == 0 && blue_n[4] == 0,
                           "★ 右边两枚括号都是灰的（#2 只读），且没有蓝的混进来");
                expectTrue(blue_c[0] > blue_c[1] && blue_c[1] < blue_c[2],
                           "★ 像素上的边序就是 ][][]: 左 ] 的重心在 [ 之右，[ 又在 ] 之左");
                expectTrue(gray_c[3] < gray_c[4],
                           "★ #2 那一对同样是 []（[ 的重心在 ] 之左），没有被画成 [[ ");
                e.showCaret(true);
                e.setControls(withRo(2, false));
                e.relayout();
            }
            // 零宽字段在自己位置上仍是最内层：否则位置被左邻居抢走，字就再也打不进去。
            e.setCaret(0, 7);
            expectTrue(e.focusedControlIndex() == 1,
                       "★ 零宽字段在自己位置上是内层（同位取下标小者）");
            expectTrue(e.typeTextAndRender("痛"), "零宽字段还能接住输入（删空不等于锁死）");
            std::printf("     在 7 上打字之后：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,7) [7,8) [8,8)",
                       "★ 字进它自己：左邻居没被撑大，右边那个空字段让位到 8");
            expectTrue(e.controls().at(0).range.end.offset == 7,
                       "★ 左邻居的端点一动不动（零宽字段不会把邻居撑大）");
            expectTrue(noCross(), "打字之后区间仍不穿线");
        }

        std::printf("     ---- 嵌套：三层可编辑 ----\n");
        {
            reset(kText, {mk21(5, 11, false), mk21(6, 10, false), mk21(7, 9, false)});
            expectTrue(e.controls().at(0).depth == 0 && e.controls().at(1).depth == 1 &&
                           e.controls().at(2).depth == 2,
                       "三层嵌套的深度是 0/1/2（区间树算出来的，不是配的）");
            expectTrue(e.controls().at(1).parent == 0 && e.controls().at(2).parent == 1,
                       "父链是外到中到内");
            // 六处边界各有一枚括号，且开括号按外中内从左到右、闭括号按内中外。
            const size_t bcps[6] = {5, 6, 7, 9, 10, 11};
            const char* bside[6] = {"[", "[", "[", "]", "]", "]"};
            std::string got;
            bool ok = true;
            float prev_x = -1.0f;
            for (int k = 0; k < 6; ++k) {
                const std::vector<SlotInfo> s = slotsAt(bcps[k]);
                got += " " + std::to_string(bcps[k]) + ":" + sides(s);
                if (s.size() != 1 || sides(s) != bside[k]) ok = false;
                if (!s.empty()) {
                    if (prev_x >= 0.0f && !(s[0].x > prev_x)) ok = false;
                    prev_x = s[0].x;
                }
            }
            std::printf("     六处边界上的占位：%s\n", got.c_str());
            expectTrue(ok, "★ 括号在正确的边界、正确的一侧：[外[中[内 … ]内]中]外");
            // 嵌套的几何形态与相邻相反：内层内容框整块落在外层内容框里。
            {
                const std::vector<EditorEngine::ControlRect> ro0 = e.controlRects(0);
                const std::vector<EditorEngine::ControlRect> ri = e.controlRects(2);
                bool contained = !ro0.empty() && !ri.empty();
                if (contained)
                    contained = ro0.front().r_pt.x <= ri.front().r_pt.x + 0.01f &&
                                ri.front().r_pt.x + ri.front().r_pt.w <=
                                    ro0.front().r_pt.x + ro0.front().r_pt.w + 0.01f;
                std::printf("     外层框 [%.2f, %.2f] ｜ 内层框 [%.2f, %.2f]\n",
                            ro0.empty() ? 0.0f : ro0.front().r_pt.x,
                            ro0.empty() ? 0.0f : ro0.front().r_pt.x + ro0.front().r_pt.w,
                            ri.empty() ? 0.0f : ri.front().r_pt.x,
                            ri.empty() ? 0.0f : ri.front().r_pt.x + ri.front().r_pt.w);
                expectTrue(contained, "★ 嵌套的形态是包含：内层内容框整块落在外层框里");
            }
            e.setCaret(0, 8);
            expectTrue(e.focusedControlIndex() == 2, "光标在三层重叠区里归最内层");
            expectTrue(e.typeTextAndRender("痛"), "内层可编辑（嵌套没有把内层的编辑权吃掉）");
            std::printf("     在内层打字之后：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,12) [6,11) [7,10)",
                       "★ 内层变长，中、外层一起变长（起点不动，终点外推）");
            expectTrue(e.controls().at(0).depth == 0 && e.controls().at(2).depth == 2,
                       "编辑之后区间树重算，嵌套关系还在");
            expectTrue(noCross(), "★ 嵌套伸缩之后区间仍然两两不穿线");
            // 把内层删空：它变小成零宽，但层级与占位都不能散。
            for (int k = 0; k < 3; ++k) {
                e.setCaretAt(0, e.controls().at(2).range.end.offset, 2);
                if (!e.backspaceAndRender()) break;
            }
            std::printf("     把内层删空之后：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,9) [6,8) [7,7)",
                       "外、中两层跟着收，内层变成落在 7 上的零宽");
            const std::vector<SlotInfo> at7 = slotsAt(7);
            std::printf("     内层位置上的占位：%s（%zu 个）\n", sides(at7).c_str(), at7.size());
            expectTrue(sides(at7) == "[]",
                       "★ 删空的内层画成一对相邻的 []（不是拆成 [ 与 ] 分到两边）");
            expectTrue(e.controls().at(2).depth == 2 && e.controls().at(2).parent == 1,
                       "★ 删空之后内层仍是第 2 层（它没有被中层认成同一段）");
            expectTrue(sides(slotsAt(6)) == "[" && sides(slotsAt(8)) == "]" &&
                           sides(slotsAt(5)) == "[" && sides(slotsAt(9)) == "]",
                       "外、中两层的括号都还在原来的边界上");
            e.setCaret(0, 7);
            expectTrue(e.focusedControlIndex() == 2, "★ 删空的内层在自己位置上仍是最内层");
            expectTrue(e.typeTextAndRender("痛"), "删空的内层还能接住输入");
            std::printf("     往删空的内层里打字之后：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,10) [6,9) [7,8)",
                       "★ 字进内层，外、中两层一起外推（三层同步，谁也不穿谁）");
            expectTrue(noCross(), "填回来之后区间仍不穿线");
        }

        std::printf("     ---- 换行：跨行字段与断行处的边界 ----\n");
        {
            std::string longtext;
            for (int i = 0; i < 4; ++i) longtext += kText;
            reset(longtext, {});
            expectTrue(e.stats().placed_line_count >= 2, "长文档排成多行（否则这一节没有意义）");
            // 断行位置：先在没有控件的行盒上量一个粗值，再用它挂控件。
            //   括号占位本身有宽度，会把断行位置挪半格，所以最终的行盒要重新量一次。
            size_t brk = 0;
            for (const LineBox& lb : e.layout().lines)
                if (lb.paragraph == 0 && lb.line == 0) brk = lb.last_cp;
            const size_t A0 = (brk > 8) ? brk - 6 : 0;
            const size_t A1 = brk + 6;
            const size_t B1 = brk + 12;
            reset(longtext, {mk21(A0, A1, false), mk21(A1, B1, false)});
            size_t l0_end = 0, l1_start = 0;
            int nl = 0;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.paragraph != 0) continue;
                ++nl;
                if (lb.line == 0) l0_end = lb.last_cp;
                if (lb.line == 1) l1_start = lb.first_cp;
            }
            std::printf("     长文档 %d 行；第 0 行止于 %zu，第 1 行起于 %zu；"
                        "A=[%zu,%zu) B=[%zu,%zu)\n",
                        nl, l0_end, l1_start, A0, A1, A1, B1);
            expectTrue(nl >= 2 && l1_start == l0_end,
                       "第 1 行的起点就是第 0 行的终点（断行处两行共享一个偏移）");
            expectTrue(A0 <= l0_end && A1 > l0_end, "前置：A 确实跨在断行处（否则这条用例不成立）");
            const std::vector<EditorEngine::ControlRect> ra = e.controlRects(0);
            const std::vector<EditorEngine::ControlRect> rb = e.controlRects(1);
            std::printf("     A → %zu 个矩形块 ｜ B → %zu 个矩形块\n", ra.size(), rb.size());
            expectTrue(ra.size() >= 2, "★ 跨行字段投影出多个矩形块 —— 控件不是矩形");
            expectTrue(ra.size() >= 2 && ra.back().r_pt.y > ra.front().r_pt.y,
                       "★ A 的后一块排在更下方（按行序推进，不是横着排）");
            expectTrue(!rectsOverlap(0, 1), "★ 跨断行的相邻字段，矩形块仍然不重叠");
            const std::vector<SlotInfo> so = slotsAt(A0);
            const std::vector<SlotInfo> sbd = slotsAt(A1);
            std::printf("     A 的开括号在第 %d 行；边界上的 %s 在第 %d 行\n",
                        so.empty() ? -1 : so[0].line, sides(sbd).c_str(),
                        sbd.empty() ? -1 : sbd[0].line);
            expectTrue(so.size() == 1 && sbd.size() == 2 && so[0].line != sbd[0].line,
                       "★ 跨行字段：开括号在第 0 行、闭括号跟着落到下一行");
            expectTrue(sbd.size() == 2 && sides(sbd) == "][" && sbd[0].line == sbd[1].line,
                       "★ 共享边界上的两枚括号在同一行、顺序仍是 ][（换行不拆散边界）");
            Rect cline0{}, cline1{};
            e.setCaret(0, l0_end - 1);
            const bool ok0 = e.caretRectPt(&cline0);
            e.setCaret(0, l1_start + 1);
            const bool ok1 = e.caretRectPt(&cline1);
            std::printf("     断行前光标 (%.2f, %.2f) ｜ 断行后光标 (%.2f, %.2f)\n", cline0.x,
                        cline0.y, cline1.x, cline1.y);
            expectTrue(ok0 && ok1 && cline1.y > cline0.y + 1.0f,
                       "★ 跨过断行，光标落到下一行（y 变大）");
            expectTrue(cline1.x < cline0.x,
                       "★ 下一行从行首开始（x 回到左边）—— 断行处光标不是横着平移");
            e.setCaret(0, l0_end - 1);
            e.moveCaretToLineEnd();
            expectTrue(e.caret().offset == l0_end, "行末就是断行偏移（行末不是字段尾）");
            e.setCaret(0, l1_start + 1);
            e.moveCaretToLineStart();
            expectTrue(e.caret().offset == l1_start,
                       "★ 下一行的行首也是同一个偏移（一个位置，两条行）");
            e.setCaret(0, l0_end);
            expectTrue(e.focusedControlIndex() == 0, "断行处的归属不问行、只看区间：归 A");
            const size_t a_end0 = e.controls().at(0).range.end.offset;
            const bool typed = e.typeTextAndRender("痛");
            std::printf("     在断行处打字之后：%s\n", ranges().c_str());
            expectTrue(typed && e.controls().at(0).range.end.offset == a_end0 + 1 &&
                           e.controls().at(1).range.start.offset == a_end0 + 1,
                       "★ 字进 A、B 整体右移（换行处的共享边界与行内同一条规则）");
            expectTrue(noCross(), "跨行字段编辑之后区间仍不穿线");
            expectTrue(e.auditLayout() == 0,
                       "★ 跨行编辑之后增量排版等于全量重排（括号占位也跟着重排）");
        }

        std::printf("     ---- 添加文字：归属决定落笔，邻居不被误伤 ----\n");
        {
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            // 共享边界 7 上有两种落笔：归前一个字段（末尾追加），或归后一个字段（开头插入）。
            e.setCaretAt(0, 7, 0);
            expectTrue(e.typeTextAndRender("痛"), "归属前一个字段时打字被放行");
            std::printf("     归属 #0 时在 7 上打字：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,8) [8,10) [10,12)",
                       "★ 归 #0：字进 #0，#1 #2 整体右移");
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaretAt(0, 7, 1);
            expectTrue(e.focusedControlIndex() == 1, "带归属 1 时光标归后一个字段");
            expectTrue(e.typeTextAndRender("痛"), "归属后一个字段时打字被放行");
            std::printf("     归属 #1 时在 7 上打字：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,7) [7,10) [10,12)",
                       "★ 归 #1：字进 #1，它的左端点不动（新字成了它的第一个字）");
            expectTrue(noCross(), "两种归属下区间都不穿线");
            // 字段内部打字：只有自己变长。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaret(0, 8);
            expectTrue(e.focusedControlIndex() == 1, "位置 8 只被 #1 覆盖");
            expectTrue(e.typeTextAndRender("痛"), "在字段内部打字被放行");
            std::printf("     在 #1 内部打字：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,7) [7,10) [10,12)",
                       "★ 只有 #1 变长：左邻居不动、右邻居整体右移");
            // 字段末尾追加：自己变长，右邻居整体右移。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaretAt(0, 9, 1);
            expectTrue(e.typeTextAndRender("痛"), "在字段末尾追加被放行");
            std::printf("     在 #1 末尾追加：%s\n", ranges().c_str());
            expectTrue(ranges() == "[5,7) [7,10) [10,12)",
                       "★ 末尾追加也是只撑大自己，右邻居整体右移");
        }

        std::printf("     ---- 光标移动：一步一格、可出字段、点击命中即落点 ----\n");
        {
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaret(0, 5);
            std::vector<size_t> walk;
            for (int k = 0; k < 12; ++k) {
                e.moveCaretBy(1);
                walk.push_back(e.caret().offset);
            }
            std::string ws;
            for (size_t v : walk) ws += " " + std::to_string(v);
            std::printf("     从 5 连按右 12 次：%s\n", ws.c_str());
            bool step_ok = (walk.size() == 12);
            for (size_t k = 0; k < walk.size(); ++k)
                if (walk[k] != 6 + k) step_ok = false;
            expectTrue(step_ok, "★ 方向键一次一格：6 7 8 9 … 17（不跳格、不被字段挡住）");
            expectTrue(walk.size() == 12 && walk[11] == 17,
                       "★ 一路走出三个字段，一直走到文档末尾");
            e.setCaret(0, 5);
            std::vector<size_t> back;
            for (int k = 0; k < 6; ++k) {
                e.moveCaretBy(-1);
                back.push_back(e.caret().offset);
            }
            expectTrue(back.size() == 6 && back.front() == 4 && back.back() == 0,
                       "★ 反向一步一格、走到 0（字段的左边界不是墙）");
            e.setCaret(0, 4);
            expectTrue(e.focusedControlIndex() == -1,
                       "★ 光标能停在字段之外（位置不受控件约束）");
            const long long blocked0 = e.blockedEdits();
            expectTrue(!e.typeTextAndRender("X"), "★ 字段外打字被拒（约束编辑权，不约束光标）");
            expectTrue(e.blockedEdits() == blocked0 + 1, "被拒的编辑记了账（宿主能看见）");
            // 只读字段：光标进得去，字打不进去。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, true)});
            e.setCaret(0, 8);
            expectTrue(e.focusedControlIndex() == 1, "光标能进只读字段（只读约束编辑，不约束浏览）");
            const long long blocked1 = e.blockedEdits();
            expectTrue(!e.typeTextAndRender("X") && e.blockedEdits() == blocked1 + 1,
                       "★ 只读字段里打字被拒，且记账");
            // 键盘跨过共享边界：归属换人。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaret(0, 7);
            expectTrue(e.focusedControlIndex() == 0, "位置 7 归前一个字段");
            e.moveCaretBy(1);
            expectTrue(e.caret().offset == 8 && e.focusedControlIndex() == 1,
                       "★ 右移一格跨过共享边界，归属换成后一个字段");
            e.moveCaretBy(-1);
            expectTrue(e.focusedControlIndex() == 0, "左移回来归属又换回前一个字段");
            e.setCaret(0, 9);
            e.moveCaretToLineStart();
            expectTrue(e.caret().offset == 0, "Home 到行首（不是字段首）");
            e.moveCaretToLineEnd();
            expectTrue(e.caret().offset == 17, "End 到行尾（不是字段尾）");
            // 点击：横扫整行，每一个采样点都必须"命中即落点"，且没有回跳与空隙。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            float lo = 1e9f, hi = -1e9f;
            for (int ci = 0; ci < 3; ++ci)
                for (const EditorEngine::ControlRect& r : e.controlRects(ci)) {
                    lo = std::min(lo, r.r_pt.x);
                    hi = std::max(hi, r.r_pt.x + r.r_pt.w);
                }
            Rect cbase{};
            e.setCaret(0, 5);
            e.caretRectPt(&cbase);
            const float sy = cbase.y + cbase.h * 0.5f;
            int probed = 0, rewritten = 0, nonmono = 0;
            size_t prev = 0;
            bool first = true;
            bool covered[7] = {false, false, false, false, false, false, false};
            for (float x = lo - 4.0f; x <= hi + 4.0f; x += 0.5f) {
                TextPos hp{};
                int owner = -1;
                if (!e.hitTestPt(x, sy, &hp, &owner)) continue;
                if (hp.offset > 17) continue;
                ++probed;
                if (!first && hp.offset < prev) ++nonmono;
                first = false;
                prev = hp.offset;
                if (hp.offset >= 5 && hp.offset <= 11) covered[hp.offset - 5] = true;
                e.setCaretAt(0, hp.offset, owner);
                if (e.caret().offset != hp.offset) ++rewritten;
            }
            std::printf("     横扫 x ∈ [%.1f, %.1f]：采样 %d 点，落点被改写的 %d 个，"
                        "非单调 %d 处\n",
                        lo - 4.0f, hi + 4.0f, probed, rewritten, nonmono);
            expectTrue(probed > 100 && rewritten == 0,
                       "★ 点击命中即落点：命中偏移与光标位置永远一致（没有点不进去的位置）");
            expectTrue(nonmono == 0, "★ 横扫时命中偏移单调不减（没有回跳的死区）");
            bool all_covered = true;
            for (int k = 0; k < 7; ++k) all_covered = all_covered && covered[k];
            expectTrue(all_covered,
                       "★ 三个字段里每一个光标位置都能点出来（5..11 全覆盖，没有点不到的空隙）");
            // 逐字中点采样：字段里第 k 个字的中点百分之百落在它的内容里（与框缘无关，
            //   也不受括号槽带影响），点那里必须归这个字段。逐字覆盖，不靠抽样碰运气。
            int mid_probed = 0, mid_bad = 0;
            for (int ci = 0; ci < 3; ++ci) {
                const size_t a = e.controls().at(ci).range.start.offset;
                const size_t b = e.controls().at(ci).range.end.offset;
                for (size_t k = a; k < b; ++k) {
                    Rect ra{}, rb{};
                    e.setCaretAt(0, k, ci);
                    if (!e.caretRectPt(&ra)) continue;
                    e.setCaretAt(0, k + 1, ci);
                    if (!e.caretRectPt(&rb)) continue;
                    TextPos hp{};
                    int owner = -1;
                    if (!e.hitTestPt((ra.x + rb.x) * 0.5f, sy, &hp, &owner)) continue;
                    ++mid_probed;
                    if (owner != ci) ++mid_bad;
                }
            }
            std::printf("     逐字中点采样：%d 个字，归属错的 %d 个\n", mid_probed, mid_bad);
            expectTrue(mid_probed == 6 && mid_bad == 0,
                       "★ 点字段里任意一个字的中间都归它自己（逐字覆盖，不是抽样碰运气）");
        }
    }

    std::printf("\n[22] 分节强制分页：模型编译产物落到引擎，增量与全量一致\n");
    {
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig ec;
        ec.zoom = 1.0f;
        e.setConfig(ec);

        // 用文档模型搭两节：第一节三段、第二节（下一页）一段。编译产物直接喂给引擎 ——
        // 这一段验的是模型的分节强制分页落成引擎既有的段前分页，不引入新的排版路径。
        auto asc = [](const std::string& s) {
            std::vector<uint32_t> out;
            out.reserve(s.size());
            for (char ch : s) out.push_back(static_cast<uint32_t>(static_cast<unsigned char>(ch)));
            return out;
        };
        NodeTree tree;
        const NodeId s1 = tree.firstSectionId();
        for (int i = 0; i < 3; ++i) {
            const NodeId t = tree.appendChild(s1, NodeKind::TextRun);
            tree.setText(t, asc("Section 1 paragraph " + std::to_string(i + 1) + "."));
            tree.appendChild(s1, NodeKind::ParagraphMark);
        }
        const NodeId s2 = tree.insertChild(tree.rootId(), 1, NodeKind::Section);
        tree.find(s2)->section.break_kind = SectionBreak::NextPage;
        const NodeId t2 = tree.appendChild(s2, NodeKind::TextRun);
        tree.setText(t2, asc("Section 2 starts on a new page."));
        tree.appendChild(s2, NodeKind::ParagraphMark);

        const StyleSheet sheet;
        const CompileResult r = compileDocument(tree, FlowVisibility::Design, sheet);
        expectTrue(r.ok() && r.blocks.size() == 4 && r.blocks[3].page_break_before,
                   "模型编译出 4 段，末段带段前分页（第二节的起点）");

        e.setBlocks(r.blocks);
        e.relayout();
        e.repaint();
        expectTrue(e.stats().page_count >= 2, "分节强制分页真的起了新页");
        expectTrue(e.auditLayout() == 0, "分页之后增量结果与全量重排逐字段一致（diff 0）");

        // 在分页之前编辑：受影响的页重算，分页仍成立且与全量一致。
        e.setCaret(0, 0);
        e.typeTextAndRender("\xE6\xB5\x8B");  // 测
        expectTrue(e.stats().page_count >= 2 && e.auditLayout() == 0,
                   "★ 分页之前编辑之后，页数与增量结果仍与全量一致");
    }

    std::printf("\n[23] 字符格式 run：块级字段升为 run 之后，旧路径等价、按 run 取值\n");
    {
        // 这一段验的是引擎输入的一次形状替换：字符格式从"一段一套"升为"一串 run"。
        //   它是替换、不是新功能，所以第一判据必须是等价 —— run 要能原样表达旧形状，
        //   否则迁移期没人敢用。两条独立的等价判据：
        //   其一，runs 为空 == 显式一个 run（属性取块级字段）；
        //   其二，一个 run 切成 N 个属性相同的 run，产出不变。
        //   其二比其一强：它要求排版真的按 run 切了，而其一在"压根没读 runs"的实现下
        //   也成立。它也是 ECMA-376 原文给的（w:r 一节：把一个 run 切成任意多个属性
        //   相同的小 run，不改变文档内容）。
        //   但其二有个前提：切点不能落在簇或连字内部 —— run 边界就是整形边界，
        //   在 f 与 fi 之间切会把连字拆掉。所以切分判据用纯中文（一字一簇）。
        const std::vector<std::string> fonts = {cjkPath, mnPath, latinPath};
        auto mkBlock = [](int font_index, float size_pt, const char* lang,
                          const std::string& text) {
            BlockSpec b;
            b.font_index = font_index;
            b.size_pt = size_pt;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.75f;
            b.language = lang;
            b.color = Color::rgb(24, 24, 24);
            b.setText(text);  // 注意：setText 会清 runs，所以 runs 必须在它之后设
            return b;
        };
        auto setup = [&](EditorEngine& eng, const BlockSpec& b) {
            eng.init(fonts);
            eng.setConfig(cfg);
            eng.setBlocks({b});
            eng.relayout();
            eng.setCaret(0, 0);
        };

        // (a) 不变量：run 序列必须恰好铺满段落文本
        {
            const BlockSpec ok = mkBlock(0, 12.0f, "zh", "两把尺子求交");
            const uint32_t n = static_cast<uint32_t>(ok.cp_text.size());
            std::string why;

            BlockSpec gap = ok;
            FormatRun g0 = ok.wholeRun();
            g0.cp_end = 2;
            FormatRun g1 = ok.wholeRun();
            g1.cp_begin = 3;
            g1.cp_end = n;
            gap.runs = {g0, g1};
            const bool gap_ok = runsTileText(gap, &why);
            expectTrue(!gap_ok && why.find("空洞") != std::string::npos,
                       "不变量：run 之间有空洞被判出来（" + why + "）");

            BlockSpec over = ok;
            FormatRun o0 = ok.wholeRun();
            o0.cp_end = 4;
            FormatRun o1 = ok.wholeRun();
            o1.cp_begin = 3;
            o1.cp_end = n;
            over.runs = {o0, o1};
            why.clear();
            const bool over_ok = runsTileText(over, &why);
            expectTrue(!over_ok && why.find("重叠") != std::string::npos,
                       "不变量：run 之间重叠被判出来（" + why + "）");

            BlockSpec tail_gap = ok;
            FormatRun s0 = ok.wholeRun();
            s0.cp_end = n - 1;
            tail_gap.runs = {s0};
            why.clear();
            const bool tail_ok = runsTileText(tail_gap, &why);
            expectTrue(!tail_ok && why.find("尾部有空洞") != std::string::npos,
                       "不变量：没铺到文本末尾被判出来（" + why + "）");

            BlockSpec empty_run = ok;
            FormatRun e0 = ok.wholeRun();
            e0.cp_begin = 2;
            e0.cp_end = 2;
            empty_run.runs = {e0};
            why.clear();
            const bool empty_ok = runsTileText(empty_run, &why);
            expectTrue(!empty_ok && why.find("空区间") != std::string::npos,
                       "不变量：空区间被判出来（" + why + "）");

            BlockSpec beyond = ok;
            FormatRun b0 = ok.wholeRun();
            b0.cp_end = n + 3;
            beyond.runs = {b0};
            why.clear();
            const bool beyond_ok = runsTileText(beyond, &why);
            expectTrue(!beyond_ok && why.find("超出了文本末尾") != std::string::npos,
                       "不变量：越过文本末尾被判出来（" + why + "）");

            expectTrue(runsTileText(ok, nullptr), "不变量：runs 为空恒真（等价情形）");

            BlockSpec three = ok;
            FormatRun t0 = ok.wholeRun();
            t0.cp_end = 2;
            FormatRun t1 = ok.wholeRun();
            t1.cp_begin = 2;
            t1.cp_end = 4;
            FormatRun t2 = ok.wholeRun();
            t2.cp_begin = 4;
            t2.cp_end = n;
            three.runs = {t0, t1, t2};
            expectTrue(runsTileText(three, nullptr), "不变量：合法切分通过");
        }

        // (b) 等价：runs 为空 vs 显式一个 run
        {
            BlockSpec a = mkBlock(0, 12.0f, "zh", "两把尺子求交：格式 run 与字体 run。");
            BlockSpec b = a;
            b.runs.push_back(b.wholeRun());

            EditorEngine ea, eb;
            setup(ea, a);
            setup(eb, b);
            const long long d = frameDiffBetween(ea, eb);
            expectTrue(d == 0, "★ 等价：runs 为空与显式一个 run 产出同一帧（不符像素 " +
                                   std::to_string(d) + "）");
            const int ga = ea.layout().stats.glyph_count;
            const int gb = eb.layout().stats.glyph_count;
            const int la = ea.layout().stats.placed_line_count;
            const int lb = eb.layout().stats.placed_line_count;
            expectTrue(ga == gb && la == lb, "等价：字形数与行数也相同（" + std::to_string(ga) +
                                                 "/" + std::to_string(gb) + "，" +
                                                 std::to_string(la) + "/" + std::to_string(lb) +
                                                 "）");
            expectTrue(ea.auditLayout() == 0 && eb.auditLayout() == 0,
                       "两条路径各自都过增量与全量一致的审计");
        }

        // (c) 切分等价：一个 run 切成 3 个属性相同的 run
        {
            const std::string txt = "两把尺子求交同一段文本会被切两次";
            BlockSpec one = mkBlock(0, 12.0f, "zh", txt);
            one.runs.push_back(one.wholeRun());

            BlockSpec three = mkBlock(0, 12.0f, "zh", txt);
            const uint32_t n = static_cast<uint32_t>(three.cp_text.size());
            FormatRun r0 = three.wholeRun();
            r0.cp_end = 4;
            FormatRun r1 = three.wholeRun();
            r1.cp_begin = 4;
            r1.cp_end = 9;
            FormatRun r2 = three.wholeRun();
            r2.cp_begin = 9;
            r2.cp_end = n;
            three.runs = {r0, r1, r2};
            expectTrue(n == 16 && runsTileText(three, nullptr), "三段切分合法（文本 16 个码点）");

            EditorEngine e1, e3;
            setup(e1, one);
            setup(e3, three);
            const long long d = frameDiffBetween(e1, e3);
            expectTrue(d == 0, "★ 切分等价：一个 run 切成 3 个同属性 run 产出同一帧（不符像素 " +
                                   std::to_string(d) + "）");
            const int c1 = e1.layout().para_index[0].font_run_count;
            const int c3 = e3.layout().para_index[0].font_run_count;
            // 整形段数会随格式切分增加（每段各自跑一遍回退切分），但产出必须不变。
            //   这两句要一起看：只断言"段数不变"是错的（段数就是会变），
            //   只断言"产出不变"则漏掉了"确实按 run 切了"这半边 ——
            //   一个压根没读 runs 的实现两条都能过。
            expectTrue(c1 == 1 && c3 == 3, "整形段数随格式切分增加（" + std::to_string(c1) + " → " +
                                               std::to_string(c3) + "）");
            const int gc1 = e1.layout().stats.glyph_count;
            const int gc3 = e3.layout().stats.glyph_count;
            expectTrue(gc1 == gc3, "切分之后字形数不变（" + std::to_string(gc1) + " vs " +
                                       std::to_string(gc3) + "）");
        }

        // (d) 属性真的被读：run 的字号决定推进
        {
            // 块级字号故意与 run 不同，用来证伪"排版还在读块级字段"。
            BlockSpec blk12 = mkBlock(2, 12.0f, "en", "office");
            FormatRun r12 = blk12.wholeRun();
            r12.size_pt = 12.0f;
            blk12.runs = {r12};

            BlockSpec blk24 = mkBlock(2, 24.0f, "en", "office");
            FormatRun r24 = blk24.wholeRun();
            r24.size_pt = 24.0f;
            blk24.runs = {r24};

            BlockSpec trap = mkBlock(2, 24.0f, "en", "office");
            FormatRun rt = trap.wholeRun();
            rt.size_pt = 12.0f;
            trap.runs = {rt};

            EditorEngine e12, e24, etrap;
            setup(e12, blk12);
            setup(e24, blk24);
            setup(etrap, trap);
            const float w12 = e12.layout().lines[0].inline_end_pt;
            const float w24 = e24.layout().lines[0].inline_end_pt;
            const float wtrap = etrap.layout().lines[0].inline_end_pt;
            expectTrue(std::fabs(wtrap - w12) < 0.01f,
                       "★ 属性真的被读：块级字号 24 而 run 是 12，行宽跟随 run（" +
                           std::to_string(wtrap) + " ≈ " + std::to_string(w12) + "）");
            expectTrue(std::fabs(w24 - 2.0f * w12) < 1.0f, "字号翻倍则行宽翻倍（" +
                                                               std::to_string(w24) + " ≈ 2 × " +
                                                               std::to_string(w12) + "）");
        }

        // (e) 指纹含 run 属性：只改 run 字号（文本一字不改）必须重整形
        {
            BlockSpec b = mkBlock(2, 12.0f, "en", "office");
            FormatRun r = b.wholeRun();
            r.size_pt = 12.0f;
            b.runs = {r};

            EditorEngine e;
            setup(e, b);
            const float w_first = e.layout().lines[0].inline_end_pt;

            // 同一个引擎、同一段文本，只把 run 的字号改成 24 再重排。
            //   整形缓存是段级、按指纹判命中；指纹若不覆盖 run 的属性，这里会命中旧条目
            //   → 行宽原封不动。这正是要抓的静默复用（字形按旧字号算、排版看着"没变"）。
            b.runs[0].size_pt = 24.0f;
            e.setBlocks({b});  // 只换数据，不排版；整形缓存不清
            e.relayout();
            const float w_second = e.layout().lines[0].inline_end_pt;
            expectTrue(std::fabs(w_second - 2.0f * w_first) < 1.0f,
                       "★ 指纹含 run 属性：只改 run 字号也必须重整形（" + std::to_string(w_first) +
                           " → " + std::to_string(w_second) + "）");
        }

        // (f) 颜色按 run 取（颜色不进整形指纹，只能在放置时现取）
        {
            BlockSpec b = mkBlock(0, 12.0f, "zh", "红绿");
            const uint32_t n = static_cast<uint32_t>(b.cp_text.size());
            FormatRun c0 = b.wholeRun();
            c0.cp_end = 1;
            c0.color = Color::rgb(200, 20, 20);
            FormatRun c1 = b.wholeRun();
            c1.cp_begin = 1;
            c1.cp_end = n;
            c1.color = Color::rgb(20, 160, 20);
            b.runs = {c0, c1};

            EditorEngine e;
            setup(e, b);
            const std::vector<PlacedGlyph>& gs = e.layout().glyphs;
            int red = 0, green = 0, other = 0;
            for (const PlacedGlyph& g : gs) {
                if (g.color.r == 200 && g.color.g == 20) {
                    ++red;
                } else if (g.color.r == 20 && g.color.g == 160) {
                    ++green;
                } else {
                    ++other;
                }
            }
            expectTrue(gs.size() == 2 && red == 1 && green == 1 && other == 0,
                       "★ 颜色按 run 取：两个字各用自己 run 的颜色（红 " + std::to_string(red) +
                           " / 绿 " + std::to_string(green) + " / 其他 " + std::to_string(other) +
                           "）");

            b.runs[0].color = Color::rgb(10, 10, 200);
            e.setBlocks({b});
            e.relayout();
            expectTrue(e.layout().glyphs[0].color.b == 200,
                       "改颜色后重排，颜色跟着变（说明颜色是放置时按 run 现取的，不是缓存里的）");
        }

        // (g) 两把尺子求交：格式 run 的切分点会成为整形边界
        {
            // 文本里夹了希腊字母 α β：中文字体覆不覆盖它们取决于字体本身，所以这里
            //   不断言"一定断了一次回退"，只断言相对关系 —— 格式 run 切一刀，整形段数
            //   就多 1（切点落在某个字体 run 内部，边界的并集因此多出一个）。
            //   实测本机 NotoSansSC 覆盖 α β，于是整段只有 1 个字体 run（base = 1）；
            //   换一份不覆盖的字体则 base = 3，而 after 仍是 base + 1。判据两种都成立，
            //   这正是它不依赖字体内容的意思。若排版没读格式 run，after 会等于 base。
            const std::string txt = "体格检查αβ完毕";
            BlockSpec whole = mkBlock(0, 12.0f, "zh", txt);
            whole.fallback_font_indices.push_back(2);  // 拉丁字体兜住希腊字母
            whole.runs.push_back(whole.wholeRun());

            BlockSpec cut = mkBlock(0, 12.0f, "zh", txt);
            cut.fallback_font_indices.push_back(2);
            const uint32_t n = static_cast<uint32_t>(cut.cp_text.size());
            FormatRun k0 = cut.wholeRun();
            k0.cp_end = 5;  // 落在文本中段，即某个字体 run 的内部
            FormatRun k1 = cut.wholeRun();
            k1.cp_begin = 5;
            k1.cp_end = n;
            cut.runs = {k0, k1};

            EditorEngine ew, ec;
            setup(ew, whole);
            setup(ec, cut);
            const int base = ew.layout().para_index[0].font_run_count;
            const int after = ec.layout().para_index[0].font_run_count;
            expectTrue(base >= 1 && after == base + 1,
                       "★ 两把尺子求交：格式 run 切一刀，整形段按它增加（" + std::to_string(base) +
                           " → " + std::to_string(after) + "）");
            expectTrue(ew.layout().stats.glyph_count == ec.layout().stats.glyph_count,
                       "求交只改切分，不改字形数（" +
                           std::to_string(ew.layout().stats.glyph_count) + " vs " +
                           std::to_string(ec.layout().stats.glyph_count) + "）");
        }
    }

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
