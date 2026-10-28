// getenv 读的是本套件自己的两个环境变量（分片开关），不是外部输入。
// MSVC 会报 C4996（"unsafe"），这里只对它关掉 —— 换成 _dupenv_s 要多一次分配，
// 而且只在 Windows 上编译得过，反而让分片开关变成平台相关的东西。
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/doc/compile_result.h"
#include "core/doc/document.h"
#include "core/doc/flow_types.h"
#include "core/doc/project.h"
#include "core/doc/section_spec.h"
#include "core/doc/textbuf.h"
#include "core/engine/engine.h"
#include "core/layout/layout.h"
#include "core/text/font.h"
#include "core/text/shaper.h"
#include "core/utils/hash.h"
#include "demo/demo_documents.h"

#include "block_compare.h"

#ifndef HEDITOR_NATIVE_ASSET_DIR
#define HEDITOR_NATIVE_ASSET_DIR "."
#endif

using namespace heditor;
using heditor::doc::Doc;
using testcmp::blockDiff;
using testcmp::sameBlock;

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

/**
 * 排版摘要：把一次排版的全部几何压成一个 64 位值，给"只改结构、不改结果"的重构上锁。
 *
 * 它补两个洞：单点几何断言覆盖不到的地方（表格行片段、跨页切点、成百行的普通段落）改了
 * 没人看得见；而"增量 == 全量"的参照物与增量路径共用同一份代码，实现错了两边一起错
 * （TRAPS 107）。它锁的是"重构前后逐位相同"，不是"排版是对的"。
 *
 * 用法上有一条硬要求：排版行为有意改变时必须重新取值，并在提交说明里写清改了什么、为什么，
 * 否则这条锁会退化成"谁改谁重打一个数"。浮点按位入哈希（不量化）；字体指针不进哈希
 * （跨进程地址不同），按首次出现顺序编号代替。
 */
struct LayoutDigest {
    uint64_t hash = kFnvBasis;
    int font_slots = 0;
    std::vector<const FontInstance*> fonts;

    int slot(const FontInstance* f) {
        if (f == nullptr) return -1;
        for (size_t i = 0; i < fonts.size(); ++i) {
            if (fonts[i] == f) return static_cast<int>(i);
        }
        fonts.push_back(f);
        ++font_slots;
        return static_cast<int>(fonts.size()) - 1;
    }
    void u64(uint64_t v) { hash = fnvMix(hash, v); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f32(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
};

LayoutDigest digestOf(const LayoutResult& lay) {
    LayoutDigest d;
    d.i64(static_cast<int64_t>(lay.lines.size()));
    d.i64(static_cast<int64_t>(lay.glyphs.size()));
    d.i64(static_cast<int64_t>(lay.placed_tables.size()));
    d.i64(lay.stats.page_count);
    d.i64(lay.stats.glyph_count);
    d.i64(lay.stats.placed_line_count);
    d.i64(lay.stats.cluster_count);
    d.i64(lay.stats.missing_glyph_count);

    for (const LineBox& lb : lay.lines) {
        d.i64(lb.paragraph);
        d.i64(lb.line);
        d.i64(lb.page);
        d.u64(lb.vertical ? 1 : 0);
        d.u64(lb.sideways ? 1 : 0);
        d.f32(lb.size_pt);
        d.f32(lb.block_lo_pt);
        d.f32(lb.block_hi_pt);
        d.f32(lb.inline_origin_pt);
        d.f32(lb.inline_end_pt);
        d.f32(lb.bracket_axis_pt);
        d.i64(static_cast<int64_t>(lb.first_cp));
        d.i64(static_cast<int64_t>(lb.last_cp));
        d.f32(lb.cell_lo_pt);
        d.f32(lb.cell_hi_pt);
        d.f32(lb.cell_box_lo_pt);
        d.f32(lb.cell_box_hi_pt);
        d.f32(lb.cell_top_pt);
        d.f32(lb.cell_bottom_pt);
        d.i64(d.slot(lb.bracket_font));
        d.i64(static_cast<int64_t>(lb.clusters.size()));
        for (const LineCluster& c : lb.clusters) {
            d.i64(static_cast<int64_t>(c.cp));
            d.f32(c.inline_pt);
            d.u64(c.placeholder ? 1 : 0);
            d.i64(static_cast<int64_t>(c.bracket_side));
            d.u64(c.read_only ? 1 : 0);
            d.i64(c.owner_control);
            d.i64(static_cast<int64_t>(c.atom));
            d.f32(c.atom_h_pt);
            d.u64(c.content_hash);
        }
    }
    for (const PlacedGlyph& g : lay.glyphs) {
        d.i64(static_cast<int64_t>(g.glyph_id));
        d.i64(d.slot(g.font));
        d.f32(g.x_pt);
        d.f32(g.y_pt);
        d.i64(g.page);
        d.i64(g.line);
        d.u64(g.vertical ? 1 : 0);
        d.f32(g.rotate_deg);
        d.i64(static_cast<int64_t>(g.cluster));
        d.i64(g.paragraph);
        d.i64(g.color.r);
        d.i64(g.color.g);
        d.i64(g.color.b);
        d.i64(g.color.a);
    }
    for (const PlacedTable& t : lay.placed_tables) {
        d.u64(t.handle);
        d.f32(t.x);
        d.f32(t.w);
        d.i64(t.rows);
        d.i64(t.cols);
        d.i64(static_cast<int64_t>(t.col_x.size()));
        for (float v : t.col_x) d.f32(v);
        d.i64(static_cast<int64_t>(t.cells.size()));
        for (const LayoutCell& c : t.cells) {
            d.i64(c.row);
            d.i64(c.cell);
            d.i64(c.row_span);
            d.i64(c.col_span);
            d.i64(static_cast<int64_t>(c.v_align));
            d.i64(c.first_block);
            d.i64(c.block_count);
        }
        d.i64(static_cast<int64_t>(t.placed.size()));
        for (const PlacedRow& r : t.placed) {
            d.i64(r.row);
            d.i64(r.page);
            d.f32(r.y);
            d.f32(r.h);
            d.u64(r.cont_top ? 1 : 0);
            d.u64(r.cont_bottom ? 1 : 0);
        }
    }
    for (const ParaIndex& p : lay.para_index) {
        d.i64(p.line_begin);
        d.i64(p.line_end);
        d.i64(p.glyph_begin);
        d.i64(p.glyph_end);
        d.i64(p.cluster_count);
        d.i64(p.missing_count);
        d.i64(p.font_run_count);
        d.i64(p.state_at_start.page_index);
        d.f32(p.state_at_start.cursor_y);
        d.i64(p.state_at_start.line_total);
    }
    for (const std::vector<uint32_t>& starts : lay.para_cluster_starts) {
        d.i64(static_cast<int64_t>(starts.size()));
        for (uint32_t v : starts) d.u64(v);
    }
    return d;
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
 * 装一份文档并挂上字段（树里控件是节点，必须与内容一起建）。
 * 定义在本文件末尾，声明提到这里是因为上面就要用。
 */
void loadBlocks(EditorEngine& e, const std::vector<BlockSpec>& blocks,
                const std::vector<Control>& controls = {});
/**
 * 换一批字段名单（内容不动）。定义在本文件末尾，声明提到这里是因为上面就要用。
 */
void loadControls(EditorEngine& e, const std::vector<Control>& controls);

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
                                long long* out_max_ch, int* out_box = nullptr) {
    EditorEngine r;
    if (!r.init(fonts)) return -1;
    r.setConfig(a.config());
    loadBlocks(r, a.blocks());
    // 装载只换数据不排版（"谁改文档谁负责 relayout"）—— 这里必须自己排一次，
    // 否则帧高会按 0 页算，比出来的东西没意义。
    r.relayout();
    if (!ctrls.empty()) loadControls(r, ctrls);
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
    const int fw = a.frameWidth();
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    for (size_t k = 0; k + 3 < n; k += 4) {
        if (fa[k] == fb[k] && fa[k + 1] == fb[k + 1] && fa[k + 2] == fb[k + 2]) continue;
        ++diff;
        const int px = static_cast<int>((k >> 2) % static_cast<size_t>(fw));
        const int py = static_cast<int>((k >> 2) / static_cast<size_t>(fw));
        if (px < bx0) bx0 = px;
        if (px > bx1) bx1 = px;
        if (py < by0) by0 = py;
        if (py > by1) by1 = py;
        for (int ch = 0; ch < 3; ++ch) {
            const long long d =
                std::llabs(static_cast<long long>(fa[k + ch]) - static_cast<long long>(fb[k + ch]));
            if (d > max_ch) max_ch = d;
        }
    }
    if (out_max_ch) *out_max_ch = max_ch;
    if (out_box) {
        out_box[0] = bx0;
        out_box[1] = by0;
        out_box[2] = bx1;
        out_box[3] = by1;
    }
    return diff;
}

/**
 * 两个引擎的整帧逐字节比较（只比 RGB，忽略 alpha）。
 *
 * 用于"同一份文档的两种建树方式必须产出同一帧"这类判据。它比逐字段比 LayoutResult 更强，也更省事：
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
    const size_t n = a.frameBytes();
    long long diff = 0;
    for (size_t k = 0; k + 3 < n; k += 4) {
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

/**
 * 装载壳：把"块流 + 控件区间"这套测试用的口径翻成模型树，再交给 setDocument。
 *
 * 引擎只有一条装载路（setDocument，见路线图 4.2 那条接缝），这一层壳留在测试里是为了
 * 让"这批判据测的是排版与编辑行为"不被装载口径盖掉：调用点逐字替换，场景一个字不用重想。
 *
 * 装载本身的等价性另有一处判据（[29]）：源块流 → 树 → 编译回来的块流逐字段相同。
 * 没有那条，这层壳就是一次静默的显示回归。
 */
std::unordered_map<const EditorEngine*, std::vector<BlockSpec>> g_last_blocks;

std::vector<DemoFieldSpec> toDemoFields(const std::vector<Control>& list) {
    std::vector<DemoFieldSpec> fields;
    fields.reserve(list.size());
    for (const Control& c : list) {
        DemoFieldSpec f;
        f.para = c.range.start.para;
        f.so = static_cast<int>(c.range.start.offset);
        f.epara = c.range.end.para;
        f.eo = static_cast<int>(c.range.end.offset);
        f.kind = c.kind;
        f.key = c.id;
        f.readonly = c.read_only;
        fields.push_back(std::move(f));
    }
    return fields;
}

/**
 * 装一份文档。controls 非空时同时挂上那些字段 —— 树里控件是节点、必须与内容一起建，
 * 所以装载是一次调用，不是"先装内容、再挂控件"两步。
 */
void loadBlocks(EditorEngine& e, const std::vector<BlockSpec>& blocks,
                const std::vector<Control>& controls) {
    g_last_blocks[&e] = blocks;
    e.setDocument(demoDoc(blocks, toDemoFields(controls)), StyleSheet{});
}

/**
 * 换一批字段名单（内容不动）。
 *
 * 树路径里控件是节点，所以这件事必须连块流一起重建树 —— 于是这里用上一次 loadBlocks
 * 留下的块流。重建之后自己也重排一次：装载只标脏，收尾归宿主。
 */
void loadControls(EditorEngine& e, const std::vector<Control>& controls) {
    std::vector<BlockSpec> blocks;
    const auto it = g_last_blocks.find(&e);
    if (it != g_last_blocks.end()) blocks = it->second;
    e.setDocument(demoDoc(blocks, toDemoFields(controls)), StyleSheet{});
    e.relayout();
}

/**
 * 直调排版层：layoutDocument 只吃块流（layout.h），是排版这一层真正的入口。
 *
 * 有一类判据问的是"排版读不读 runs"，而树装载会把同属性的相邻文本节点合并成一个
 * （模型层规范化的既定行为），于是"把一个 run 切成 N 个同属性小 run"在树上表达不出来 ——
 * 这类判据本来就该问排版层，不该被装载方式挡住。项目里既有此手法：排版层的契约判据
 * 与增量审计都拿它当对照。
 */
LayoutResult layoutBlocksOnly(const std::vector<std::string>& fontPaths,
                              const std::vector<BlockSpec>& blocks) {
    FontLibrary lib;
    for (const std::string& p : fontPaths) lib.addFace(p);
    return layoutDocument(lib, blocks, PageMetrics{});
}

/**
 * 分节表：id、是否性能节、权重。
 *
 * 性能节只含耗时比值判据（增量对全量、页指纹对整篇）。它们会被同机上的其它负载顶翻，
 * 所以不参与分片，只由 HEDITOR_TEST_PERF=1 选中，ctest 里那条用例带 RUN_SERIAL。
 *
 * 权重的一个单位约合 250 ms（HEDITOR_TEST_TIMES=1 跑一遍就能重新量出来，别靠猜）。
 * 只用来把分片切匀：改了一节让它明显变慢，把这里的数调大。
 *
 * 表与代码必须一一对应，收尾时对账（见 checkSectionTable）—— 新加的节忘了登记会让它
 * 永远不跑，删掉的节忘了注销会让分片永远跑不到它。
 */
struct SectionInfo {
    int id;
    bool perf;
    int weight;
};

const SectionInfo kSections[] = {
    {1, false, 1},   {2, false, 1},   {3, false, 6},   {4, false, 3},   {5, false, 1},
    {6, false, 1},   {7, false, 3},   {8, false, 3},   {9, false, 3},   {10, false, 7},
    {11, false, 11}, {12, false, 3},  {13, false, 3},  {14, false, 3},  {15, false, 27},
    {16, false, 19}, {17, false, 43}, {18, false, 24}, {19, false, 6},  {21, false, 4},
    {22, false, 3},  {23, false, 53}, {24, false, 5},  {31, false, 3},  {32, false, 3},
    {33, false, 4},  {34, false, 9},  {35, false, 8},  {36, false, 3},  {37, false, 3},
    {38, false, 8},  {39, false, 2},  {40, false, 3},  {41, false, 3},  {42, false, 3},
    {43, false, 3},  {44, false, 9},  {45, false, 6},  {46, false, 3},  {47, false, 4},
    {48, false, 4},  {49, false, 3},  {50, false, 3},  {51, false, 4},  {52, false, 4},
    {53, false, 9},  {55, false, 3},  {56, false, 7},  {57, false, 4},  {58, false, 3},
    {59, false, 3},  {60, false, 6},  {61, false, 9},  {62, false, 9},  {63, false, 3},
    {64, false, 3},  {65, false, 3},  {66, false, 3},  {67, false, 10}, {68, false, 4},
    {69, false, 3},  {70, false, 10}, {71, false, 6},  {72, false, 10}, {73, false, 9},
    {74, false, 3},  {75, false, 3},  {76, false, 3},  {77, false, 3},  {78, true, 1},
    {79, false, 2},  {80, false, 3},  {81, false, 2},  {82, false, 1},  {83, false, 2},
    {84, false, 2},  {85, false, 3},  {86, false, 4},  {87, false, 3},  {88, false, 3},
    {89, false, 3},  {90, false, 2},  {91, false, 3},  {92, false, 6},  {93, false, 5},
};

/**
 * 代码里问过的节（不管选不选中），收尾时与表对账。
 *
 * @return 可写的集合
 */
std::set<int>& requestedSections() {
    static std::set<int> ids;
    return ids;
}

/**
 * 每一节实际花掉的时间，只在 HEDITOR_TEST_TIMES=1 时打印。
 *
 * @return 可写的时间表（毫秒）
 */
std::map<int, double>& sectionMs() {
    static std::map<int, double> ms;
    return ms;
}

/**
 * 解析分片开关 "i/n"：第 i 片（0 起）共 n 片。
 *
 * @param text 开关文本
 * @param index 输出：片号
 * @param count 输出：片数
 * @return 文本合法且 0 <= index < count
 */
bool parseShard(const std::string& text, int* index, int* count) {
    const size_t slash = text.find('/');
    if (slash == std::string::npos) return false;
    const int i = std::atoi(text.substr(0, slash).c_str());
    const int n = std::atoi(text.substr(slash + 1).c_str());
    if (n <= 0 || i < 0 || i >= n) return false;
    *index = i;
    *count = n;
    return true;
}

/**
 * 第 index 片（共 count 片）：按权重从大到小贪心装进当前最轻的一片。
 *
 * 不用 id 取模：节号与体量相关（表格那几节都靠后、都重），实测取模的最大片是最小片的两倍。
 *
 * @param index 片号，0 起
 * @param count 片数
 * @return 这一片要跑的节
 */
std::set<int> shardOf(int index, int count) {
    std::vector<const SectionInfo*> items;
    for (const SectionInfo& s : kSections) {
        if (!s.perf) items.push_back(&s);
    }
    std::stable_sort(items.begin(), items.end(), [](const SectionInfo* a, const SectionInfo* b) {
        return a->weight > b->weight;
    });
    std::vector<int> load(static_cast<size_t>(count), 0);
    std::vector<std::set<int>> bins(static_cast<size_t>(count));
    for (const SectionInfo* s : items) {
        int lightest = 0;
        for (int i = 1; i < count; ++i) {
            if (load[static_cast<size_t>(i)] < load[static_cast<size_t>(lightest)]) lightest = i;
        }
        load[static_cast<size_t>(lightest)] += s->weight;
        bins[static_cast<size_t>(lightest)].insert(s->id);
    }
    return bins[static_cast<size_t>(index)];
}

/**
 * 造选中的集合。开关按优先级：ONLY 点名、PERF 只跑性能节、SHARD 取一片，都不设就跑全部
 * 非性能节。
 *
 * @return 这一轮要跑的节
 */
std::set<int> chooseSections() {
    std::set<int> out;
    const char* only = std::getenv("HEDITOR_TEST_ONLY");
    if (only != nullptr && *only != '\0') {
        const std::string list = "," + std::string(only) + ",";
        for (const SectionInfo& s : kSections) {
            if (list.find("," + std::to_string(s.id) + ",") != std::string::npos) out.insert(s.id);
        }
        return out;
    }
    const char* perf = std::getenv("HEDITOR_TEST_PERF");
    if (perf != nullptr && *perf != '\0' && std::strcmp(perf, "0") != 0) {
        for (const SectionInfo& s : kSections) {
            if (s.perf) out.insert(s.id);
        }
        return out;
    }
    const char* shard = std::getenv("HEDITOR_TEST_SHARD");
    int index = 0;
    int count = 1;
    if (shard != nullptr && parseShard(shard, &index, &count) && count > 1) {
        return shardOf(index, count);
    }
    for (const SectionInfo& s : kSections) {
        if (!s.perf) out.insert(s.id);
    }
    return out;
}

/**
 * 这一节跑不跑，并记下它的耗时。
 *
 * 每节开头调两次（标题一次、正文一次），只有第一次换 id 时才结上一节的账。
 *
 * @param n 节号
 * @return 这一节是否被选中
 */
bool runSection(int n) {
    static const std::set<int> selected = chooseSections();
    static int previous = -1;
    static std::chrono::steady_clock::time_point mark = std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (n != previous) {
        if (previous >= 0) {
            sectionMs()[previous] = std::chrono::duration<double, std::milli>(now - mark).count();
        }
        previous = n;
        mark = now;
    }
    requestedSections().insert(n);
    return selected.count(n) > 0;
}

/**
 * 分节表与代码对账。
 *
 * 两边任何一边多出来都是漂移：代码里新加的节没登记，分片永远不跑它；表里留着已删的节，
 * 分片永远跑不到它。这一条放在汇总之前，所以它也计入断言数。
 */
void checkSectionTable() {
    std::set<int> table;
    for (const SectionInfo& s : kSections) table.insert(s.id);
    std::string missing;
    std::string phantom;
    for (int id : requestedSections()) {
        if (table.count(id) == 0) missing += " " + std::to_string(id);
    }
    for (int id : table) {
        if (requestedSections().count(id) == 0) phantom += " " + std::to_string(id);
    }
    expectTrue(missing.empty(), "分节表：代码里问过的每一节都登记在表里（缺：" + missing + "）");
    expectTrue(phantom.empty(), "分节表：表里每一节都有对应的代码（多：" + phantom + "）");
}

/**
 * 每节实际耗时，按 HEDITOR_TEST_TIMES=1 打印；填权重时用它，不靠猜。
 */
void printSectionTimes() {
    const char* on = std::getenv("HEDITOR_TEST_TIMES");
    if (on == nullptr || *on == '\0' || std::strcmp(on, "0") == 0) return;
    std::printf("\n每节耗时（毫秒，含两次 runSection 之间的全部工作）：\n");
    double total = 0.0;
    for (const auto& kv : sectionMs()) {
        total += kv.second;
        std::printf("  [%d] %.0f\n", kv.first, kv.second);
    }
    std::printf("  合计 %.0f ms\n", total);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const std::string cjkPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansSC-VF.ttf");
    const std::string mnPath =
        joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSansMongolian-Regular.ttf");
    const std::string latinPath = joinPath(HEDITOR_NATIVE_ASSET_DIR, "fonts/NotoSans-Latin-VF.ttf");

    if (runSection(1)) std::printf("[1] 码点工具：UTF-8 ↔ 码点往返\n");
    if (runSection(1)) {
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

    if (runSection(2)) std::printf("\n[2] 编辑原语与簇吸附的边界\n");
    if (runSection(2)) {
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

    // 共享引擎：第 3、5、6 节接着在它上面做。只在这几节被选中时才装载 —— 三张字体加
    //   一次整页呈现是每进程 2.5 s 的固定开销，分片跑时不该为别的节白付。
    EditorEngine e;
    EngineConfig cfg;
    cfg.zoom = 1.5f;
    // 首次呈现的两个数：第 3、6 节的像素判据拿它们当分母，所以在块外留一份。
    long long framePixels = 0;
    long long fullPaint = 0;
    if (runSection(3) || runSection(5) || runSection(6)) {
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败：%s\n", cjkPath.c_str());
            return 2;
        }
        e.setConfig(cfg);

        BlockSpec doc;
        doc.font_index = 0;  // CJK
        doc.size_pt = 12.0f;
        doc.wm = WritingMode::HorizontalTB();
        doc.line_spacing.factor = 1.75f;
        doc.language = "zh";
        doc.setText(
            "\xE6\x82\xA3\xE8\x80\x85\xE4\xB8\xBB\xE8\xAF\x89\xEF\xBC\x9A"
            "\xE5\x8F\x8D\xE5\xA4\x8D\xE5\xBF\x83\xE6\x82\xB8"
            "3\xE5\xA4\xA9\xE3\x80\x82");  // 患者主诉：反复心悸3天。
        loadBlocks(e, {doc});
        e.relayout();
        e.repaint();

        framePixels = static_cast<long long>(e.editStats().frame_w) * e.editStats().frame_h;
        fullPaint = e.editStats().repainted_pixels;
        std::printf("     单页文档：帧 %d × %d = %.2f M 像素；首次全量写入 %.2f M 像素\n",
                    e.editStats().frame_w, e.editStats().frame_h,
                    static_cast<double>(framePixels) / 1e6, static_cast<double>(fullPaint) / 1e6);
        expectTrue(fullPaint > framePixels / 2, "首次呈现确实是全量（写入量 ≥ 半帧）");
    }

    if (runSection(3)) std::printf("\n[3] 敲字：文档真的变了、光标跟着走\n");
    if (runSection(3)) {
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

        // 行带重画之后，这一格的判据从"整页重画"变成了"只重画脏行那一条带"。
        //   这一份文档只有一行，而"页首行 + 页末行"的带按定义就是从页顶到页底 ——
        //   所以这里不能拿"写入量小"当判据（单行页本来就该整页）。真正要钉的是
        //   下面那条更强的：与"从零重排重画"的参照帧逐像素比。
        const long long wrote_px = es.repainted_pixels;
        const int band0 = es.band_y0;
        const int band1 = es.band_y1;
        std::printf("     敲字后写入 %.2f M 像素（全帧 %.2f M），行带 y ∈ [%d, %d] px\n",
                    static_cast<double>(wrote_px) / 1e6, static_cast<double>(framePixels) / 1e6,
                    band0, band1);
        // 判据基准是页高，不是帧高：帧按页号分槽之后，帧高 = 槽数 x 步距
        //   （含页间隙、而且槽数可能大于窗口页数），它不再等于一页。
        expectTrue(band0 == 0 && band1 == e.pagePxHeight(),
                   "单行页的行带 = 整页（页首行 + 页末行，带按定义从页顶到页底）");
        long long max_ch = 0;
        const long long fresh_diff =
            frameDiffAgainstFresh(e, {cjkPath, mnPath, latinPath}, {}, EditMode::Document, &max_ch);
        expectTrue(fresh_diff == 0, "敲字之后：增量帧 == 从零重画的帧（逐像素；差 " +
                                        std::to_string(fresh_diff) + "，最大通道差 " +
                                        std::to_string(max_ch) + "）");

        // 退回原状，便于后续小节
        e.setCaret(0, 4);
        expectTrue(e.backspaceAndRender(), "退格返回 true");
        expectTrue(e.editStats().doc_codepoints == cpsBefore, "退格后码点数回到原值");
        expectTrue(e.caret().offset == 3, "退格后光标回到插入点之前");
    }

    if (runSection(4)) std::printf("\n[4] 字素簇不可切分：e + U+0301 是一个字符，不是一个半\n");
    if (runSection(4)) {
        EditorEngine e2;
        expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        e2.setConfig(cfg);

        BlockSpec b;
        b.font_index = 2;  // 拉丁（有组合重音）
        b.size_pt = 24.0f;
        b.wm = WritingMode::HorizontalTB();
        b.language = "en";
        b.cp_text = {static_cast<uint32_t>('e'), 0x0301u};  // e + 组合重音 = é
        loadBlocks(e2, {b});
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

    if (runSection(5))
        std::printf("\n[5] 几何正反查互逆：正查出的光标矩形，反查必须回到同一位置\n");
    if (runSection(5)) {
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

    if (runSection(6)) std::printf("\n[6] 光标移动 = 真增量：写入量与全帧差两个数量级\n");
    if (runSection(6)) {
        e.setCaret(0, 0);
        e.repaint();
        const EditStats beforeMove = e.editStats();
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
        //   光标移动没有重排，所以 layout_ran 必须是 false。
        //   排版那几个数字仍是上一轮的账 —— 它们由 relayout() 发布、呈现不改写。
        //   断言"没被抹掉"而不是"归零"：抹零之后，排版刚算出来的账就再也读不到了
        //   （撤销之后不呈现就读，是这条最容易踩的一格）。
        expectTrue(!e.editStats().layout_ran, "光标移动：本轮未重排（layout_ran=false）");
        expectTrue(e.editStats().relaid_lines == beforeMove.relaid_lines,
                   "光标移动：重排行数不被呈现抹掉（仍是上一轮排版的账）");
        expectTrue(e.editStats().layout_ms == beforeMove.layout_ms,
                   "光标移动：布局耗时不被呈现抹掉（同上）");
        expectTrue(e.editStats().layout_was_full == beforeMove.layout_was_full,
                   "光标移动：全量标记不被呈现改写");

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

    if (runSection(7)) std::printf("\n[7] 竖排（蒙文侧倒）的光标几何：横条、沿列向下\n");
    if (runSection(7)) {
        EditorEngine e3;
        expectTrue(e3.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        e3.setConfig(cfg);

        BlockSpec b;
        b.font_index = 1;  // 蒙文
        b.size_pt = 20.0f;
        b.wm = WritingMode::VerticalLR();
        b.line_spacing.factor = 1.8f;
        b.language = "mn";
        b.setText("\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF");
        loadBlocks(e3, {b});
        e3.relayout();
        e3.repaint();

        const std::vector<uint32_t>& starts = e3.layout().para_cluster_starts[0];
        std::printf("     蒙文 6 码点 → %zu 个簇边界\n", starts.size());
        expectTrue(starts.size() >= 2, "至少有 1 个簇 + 哨兵");

        Rect first{};
        e3.setCaret(0, starts.front());
        expectTrue(e3.caretRectPt(&first), "竖排光标矩形可查");
        // 列宽 = 单倍行高（字体的 ascent + descent）x 行距倍数。
        const FontInstance* mnInst = e3.fonts().instance(1, b.size_pt);
        const float mnatural = mnInst ? (mnInst->ascenderPt() + mnInst->descenderPt()) : b.size_pt;
        const float col_w = mnatural * b.line_spacing.factor;
        std::printf("     光标矩形 w=%.2f h=%.2f（列宽 %.2f）\n", first.w, first.h, col_w);
        expectTrue(first.w > first.h, "竖排光标是横条（宽 = 列盒宽 > 高 = 光标粗细）");
        expectNear(first.w, col_w, 0.5, "光标宽度 = 列宽（占满列盒）");

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

    if (runSection(8)) std::printf("\n[8] 连字「ff」不得合并：一个字母就是一格\n");
    if (runSection(8)) {
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
        loadBlocks(e4, {b});
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

    if (runSection(9)) std::printf("\n[9] 页数增长时帧缓冲必须跟着长（否则新页永远看不见）\n");
    if (runSection(9)) {
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
        c5.page.margins = PageMargins::all(10.0f);
        c5.page_gap_pt = 10.0f;
        e5.setConfig(c5);

        BlockSpec probe;
        probe.font_index = 0;  // 中文
        probe.size_pt = 12.0f;
        probe.wm = WritingMode::HorizontalTB();
        probe.line_spacing.factor = 1.75f;
        probe.language = "zh";

        // 不硬编码"每页几个字" —— 那是从字体度量推出来的，度量一改断言就假红。
        //   改成现场搜出临界字数：第一个让它排到第 2 页的字数。
        int n_crit = 0;
        for (int n = 2; n <= 400; ++n) {
            probe.cp_text.assign(static_cast<size_t>(n), 0x6D4B);  // n 个「测」
            loadBlocks(e5, {probe});
            e5.relayout();
            if (e5.stats().page_count >= 2) {
                n_crit = n;
                break;
            }
        }
        std::printf("     临界字数 = %d（第 %d 个字把文档推到了第 2 页）\n", n_crit, n_crit);
        expectTrue(n_crit > 1, "搜到了跨页临界点");

        // 刚好一页，并且真的呈现一次 —— 这条 bug 只在"窗口已经量过一遍"之后才暴露。
        probe.cp_text.assign(static_cast<size_t>(n_crit - 1), 0x6D4B);
        loadBlocks(e5, {probe});
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

        // 核心断言。修复前窗口页数锁在"上一拍量到的页数"（=1）上，
        //   帧高与宽高都没变 → 换窗判据全不成立 → h2 == h1，新页不可见。
        expectTrue(h2 > h1, "帧缓冲高度跟随页数增长（修复前这里恒等 → 新页不可见）");
        expectTrue(e5.editStats().total_pages == 2, "本次呈现覆盖 2 页");
        // 有页位图缓存之后"重画了几页"不再恒等于窗口页数：第 0 页的字一个都没动，
        //   它的像素直接复用（这正是缓存要省的那笔）；要守的是新页真的被光栅进去了。
        expectTrue(e5.editStats().repainted_pages >= 1 &&
                       e5.auditPageJson(1).find("\"cached\":1") != std::string::npos,
                   "新页被光栅进缓存（第 1 页内容没变，直接复用）");

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

    if (runSection(10)) std::printf("\n[10] 光栅必须确定：同一文档两次渲染逐字节相同\n");
    if (runSection(10)) {
        // 光栅是确定的：同一份排版结果、同一套字体与缩放，两次渲染必须给出同一串像素。
        //   挡的是"某条路径带隐藏状态"（字形 strike、预热、共享 typeface）——
        //   它们只能影响命不命中，不能影响画出来的东西。
        auto renderDoc = [&]() -> std::vector<uint8_t> {
            EditorEngine e;
            if (!e.init({cjkPath, mnPath, latinPath})) return {};
            EngineConfig c = cfg;
            c.zoom = 1.5f;
            e.setConfig(c);
            loadBlocks(e, sampleDocument(0, 1, 2));
            e.relayout();
            e.repaint();
            return std::vector<uint8_t>(e.frame(), e.frame() + e.frameBytes());
        };
        const std::vector<uint8_t> a = renderDoc();
        const std::vector<uint8_t> b = renderDoc();
        expectTrue(!a.empty() && a.size() == b.size(), "两次渲染的帧尺寸一致");
        expectTrue((a.size() == b.size()) && std::memcmp(a.data(), b.data(), a.size()) == 0,
                   "同一文档两次渲染逐字节相同（没有隐藏状态泄漏到像素里）");
        auto inkOf = [](const std::vector<uint8_t>& px) {
            size_t n = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                if (px[i] < 128 && px[i + 1] < 128 && px[i + 2] < 128) ++n;
            }
            return n;
        };
        const size_t ink_a = inkOf(a);
        std::printf("     墨迹像素：%zu（占全帧 %.4f%%）\n", ink_a,
                    100.0 * static_cast<double>(ink_a) / static_cast<double>(a.size() / 4));
        expectTrue(ink_a > 2000, "确实画出了字形（不是一块空白页底）");
    }

    if (runSection(11)) std::printf("\n[11] 表单：控件是区间、边界是策略\n");
    if (runSection(11)) {
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

        // 策略层现在是模型层的能力表（吃树与流位置），不再是一份独立的 ControlSet 策略 ——
        //   所以这里建一棵同样形状的树，用引擎的落点判定穷举边界情况：持树路径问的就是它。
        EditorEngine ep;
        if (!ep.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败：%s\n", cjkPath.c_str());
            return 2;
        }
        EngineConfig cfge;
        cfge.zoom = 1.5f;
        ep.setConfig(cfge);
        {
            BlockSpec be;
            be.font_index = 0;
            be.size_pt = 12.0f;
            be.wm = WritingMode::HorizontalTB();
            be.line_spacing.factor = 1.75f;
            be.language = "zh";
            be.setText("患者主诉：反复心悸三天，加重一天。");
            loadBlocks(ep, {be});
        }
        auto canIns = [&](int o) { return ep.policyAt(T(o)).allowed; };
        auto canDel = [&](int a, int b) { return ep.policyErase(T(a), T(b)).allowed; };

        {
            loadControls(ep, {mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
            ep.setEditMode(EditMode::Form);
            expectTrue(ep.controls().size() == 2, "控件集合里 2 个控件");

            expectTrue(canIns(6), "字段内：可插入");
            expectTrue(canIns(9),
                       "字段末尾（end）可插入 —— 追加是合法编辑，所以插入判定要按闭区间看");
            expectTrue(!canIns(10), "end 之后一位：出界，拒");
            expectTrue(!canIns(2), "字段之外：拒");

            expectTrue(canDel(6, 7), "字段内：可删");
            expectTrue(canDel(5, 6), "字段首字：可删");
            expectTrue(!canDel(4, 6), "从字段外删进来：拒");
            expectTrue(!canDel(7, 10), "删除区间伸出字段外：拒（两个端点都查，才挡得住这种）");
            ep.setEditMode(EditMode::Document);
            expectTrue(canDel(7, 10), "同一个操作在文档模式下允许 —— 差异全部来自策略这一个开关");
            ep.setEditMode(EditMode::Form);

            // 光标位置不归策略管：字段外、只读字段里都能停（策略约束的是编辑）。
            //   对应的断言在 [21] 用引擎实例验（那里才有"光标能不能移动过去"这件事）。
        }

        // 嵌套：内层策略优先
        {
            loadControls(ep, {mk(5, 11, "outer", false), mk(9, 11, "inner", true)});
            ep.setEditMode(EditMode::Form);
            const ControlSet& cs = ep.controls();
            expectTrue(cs.size() == 2 && cs.at(1).parent == 0 && cs.at(1).depth == 1,
                       "内层控件的 parent / depth 建对了（区间树）");
            expectTrue(cs.innermostAt(T(6), true) == 0, "位置 6 只被外层覆盖 → 最内层是外层");
            expectTrue(cs.innermostAt(T(9), true) == 1, "位置 9 被内外层同时覆盖 → 取最内层");
            expectTrue(canIns(6), "外层可编辑区域：允许");
            expectTrue(!canIns(9), "内层只读区域：拒 —— 这就是「嵌套」的全部含义（内层策略优先）");
            expectTrue(!canIns(10), "内层覆盖的 10 也拒");
        }

        // 空表单 + 只读模式
        {
            loadControls(ep, {});
            ep.setEditMode(EditMode::Form);
            expectTrue(!canIns(3), "一个控件都没配时，表单模式整篇不可编辑（没有退化档）");

            loadControls(ep, {mk(5, 9, "k1", false)});
            ep.setEditMode(EditMode::ReadOnly);
            expectTrue(!canIns(6), "只读模式：字段内也不许插");
            ep.setEditMode(EditMode::Form);
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
        bf.line_spacing.factor = 1.75f;
        bf.language = "zh";
        bf.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(ef, {bf});
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

        // (b) "看得见就占位"：文档（设计态）与表单都画方括号 → 都占位；只读不画 → 不占位。
        //
        //   括号落进版面就连宽度一起占了，所以"画不画"与"占不占位"是同一条记录
        //   （排版层只吃块流的对象表，两者分不开）。
        {
            ef.setCaret(0, 0);  // 文档开头 —— 在任何字段之外
            const float end_none =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;
            const int glyphs_none = ef.stats().glyph_count;

            loadControls(ef, {mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
            const float end_doc =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;

            ef.setEditMode(EditMode::Form);
            // 表单模式会把光标夹进最近的字段 → 恰好就是"光标进了字段"
            const float end_in =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;

            ef.setEditMode(EditMode::ReadOnly);
            const float end_ro =
                ef.layout().lines.empty() ? 0.0f : ef.layout().lines[0].inline_end_pt;
            ef.setEditMode(EditMode::Form);

            std::printf(
                "     首行宽：无控件 %.2f pt ｜ 文档模式 %.2f pt ｜ 表单模式 %.2f pt ｜ "
                "只读模式 %.2f pt（字形始终 %d 个）\n",
                end_none, end_doc, end_in, end_ro, ef.stats().glyph_count);

            expectTrue(end_doc > end_none + 1.0f,
                       "文档模式（设计态）也占位 —— 看不见字段的边界，作者就无从增删改");
            expectTrue(std::fabs(end_in - end_doc) < 0.01f,
                       "表单模式占位量与文档模式一模一样"
                       "（括号只有显示/不显示两档，不随编辑模式变）");
            expectTrue(
                std::fabs(end_ro - end_none) < 0.01f,
                "只读模式不占位（括号不画就不该占宽度 —— 那样会凭空多出一段空隙，谁也没要）");
            expectTrue(ef.stats().glyph_count == glyphs_none,
                       "括号不产生字形（占位簇 g0==g1，墨迹数量一个都没变）");
            expectTrue(ef.editMode() == EditMode::Form && ef.controls().size() == 2,
                       "已切回表单模式，2 个字段");
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

            loadControls(ef, {});                 // 退回"没有表单"的排版
            const float x_sick0 = glyphX(ef, 0);  // 患 —— 字段之前，不该动
            const float x_fan0 = glyphX(ef, 5);   // 反 —— 字段第一个字，该让 1 个括号
            const float x_san0 = glyphX(ef, 9);   // 三 —— 字段之后，该让 2 个括号
            const float x_per0 = glyphX(ef, 16);  // 。 —— 行尾，该让 2 个括号

            // 括号常显档下两个字段都占位 ——
            //   所以 #0 之后 +2 槽、#1 之后 +4 槽。这正是"括号一直显示"的代价：
            //   正文被多推开一对括号。不要再按"只有聚焦字段占位"去写期望值。
            loadControls(ef, {mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
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

            // (b3) 占位只属于"看得见的模式"
            //   文档（设计态）要看得见边界，表单要看得见填哪，两者都画方括号 → 都占位；
            //   只读不画（它是呈现与打印态，括号落进版面就印出来了）→ 那就一个宽度都不许占，
            //   否则会凭空多出一段空隙，谁也没要，而且很难发现（正文看起来"字距有点怪"）。
            ef.setEditMode(EditMode::Document);
            ef.setCaret(0, 6);  // 光标仍在 k1 里（控件还挂着）
            const float x_doc = glyphX(ef, 5);
            ef.setEditMode(EditMode::ReadOnly);
            ef.setCaret(0, 6);
            const float x_ro = glyphX(ef, 5);
            ef.setEditMode(EditMode::Form);
            const float x_form = glyphX(ef, 5);
            std::printf(
                "     字形「反」x：无表单基线 %.3f ｜ 文档 %.3f ｜ 表单 %.3f ｜ 只读 %.3f\n",
                x_fan0, x_doc, x_form, x_ro);
            expectTrue(x_doc - x_fan0 > 1.0f,
                       "文档模式也占位：字形让开一个括号槽（设计态看不见边界就无从增删改）");
            expectTrue(std::fabs(x_doc - x_form) < 0.01f,
                       "文档与表单占位量逐像素一致（括号只有显示/不显示两档，与模式无关）");
            expectTrue(std::fabs(x_ro - x_fan0) < 0.01f,
                       "只读模式回到无表单基线的位置：不画就不占宽度");
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

            // 字段内容起点退格：表单模式的删除不许越过控件边界 —— 不删、不合并，
            //   也不把光标带到 [ 之前（那是文档模式那套括号规则）。
            ef.setCaret(0, 5);
            expectTrue(ef.caretSide() == 1, "光标在字段 #0 的开括号之后（＝内容起点）");
            const long long b1 = ef.blockedEdits();
            expectTrue(!ef.backspaceAndRender(), "★ 内容起点退格：拒（不许从字段外删到字段里）");
            expectTrue(ef.blocks()[0].cp_text.size() == len0 && ef.blockedEdits() == b1 + 1,
                       "长度不变，且这次拒绝被记了账");
            expectTrue(ef.caret().offset == 5 && ef.caretSide() == 1,
                       "★ 光标仍在字段里，没被带出去");
        }

        // (d) 方向键在表单模式下停在字段边界上（选区可以越界，光标不行）
        //
        //   选区与光标是两件事：选区（拖选、Shift 扩展、全选）能盖住只读区与正文，光标不能 ——
        //   它是插入点，只出现在打得进字的地方（表单模式 = 可编辑控件内容里，只读模式 = 哪儿都
        //   不出现）。于是方向键走到内容两端就停住，换字段用 Tab 或鼠标点。
        {
            ef.setCaret(0, 8);
            expectTrue(ef.moveCaretAndRender(1) && ef.caret().offset == 9 && ef.caretSide() == 0,
                       "右移到字段 #0 的内容末尾（cp 9 侧别 0，仍在 ] 之内）");
            expectTrue(!ef.moveCaretAndRender(1), "★ 再右移：目标在 ] 之后（不可插入）→ 不动");
            expectTrue(ef.caret().offset == 9 && ef.caretSide() == 0,
                       "★ 光标原地不动，没有被带到括号外");
            expectTrue(ef.caretInsertable(), "光标在字段里：可插入（画得出竖线）");

            ef.setCaret(0, 14);
            for (int i = 0; i < 9; ++i) ef.moveCaretAndRender(-1);
            expectTrue(ef.caret().offset == 12 && ef.caretSide() == 1,
                       "★ 一路左移停在 #2 的内容起点（cp 12 侧别 1），不跨过 [ —— 实际 " +
                           std::to_string(ef.caret().offset) + "/" +
                           std::to_string(ef.caretSide()));

            // Home/End：行首行尾在字段之外时收进字段内容区间（不是把光标甩到正文里）
            ef.setCaret(0, 14);
            expectTrue(ef.moveCaretToLineStartAndRender() && ef.caret().offset == 12 &&
                           ef.caretSide() == 1,
                       "Home 收到字段内容起点（行首 0 在字段外，光标不越界）");
            expectTrue(
                ef.moveCaretToLineEndAndRender() && ef.caret().offset == 16 && ef.caretSide() == 0,
                "End 收到字段内容末尾（行尾 17 在字段外）");
            expectTrue(!ef.caretInsertable() == false, "收进来的位置仍然是可插入的");

            // 反向一半：选区不受这条限制 —— Shift 扩展能盖住字段之外的正文。
            ef.setCaret(0, 12);
            expectTrue(ef.moveCaretAndRender(-1, /*extend=*/true), "Shift+左：选区扩展不受限");
            expectTrue(ef.caret().offset == 12 && ef.caretSide() == 0 && ef.hasSelection(),
                       "选区端点走到了 [ 那一格 —— 能选区，只是光标不显示在那儿");
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
            //   先把文档恢复，保证两条产物的起点完全一致（不然比的是两次插入的叠加）。
            expectTrue(ef.outdentAndRender() && ef.blocks()[0].cp_text.size() == N0,
                       "反缩进先把文档恢复（下面才能公平对比）");
            ef.setCaret(0, 2);
            expectTrue(ef.typeTextAndRender("    "), "手打 4 个空格");
            const std::vector<uint8_t> f_sp = frameOf(ef);
            expectTrue(ef.blocks()[0].cp_text.size() == len_tab, "两条产物的文档长度一致");
            expectTrue(sameFrame(f_tab, f_sp),
                       "Tab 缩进的帧 == 手打 4 个空格的帧（逐字节相同）"
                       "—— 缩进不是特权通道，策略/区间伸缩/记账全走同一条路");

            // Shift+Tab 反缩进
            expectTrue(ef.outdentAndRender() && ef.blocks()[0].cp_text.size() == N0,
                       "Shift+Tab 反缩进：把 4 个空格删回来");
            expectTrue(!ef.outdentAndRender(), "光标前没有空格时反缩进返回 false（不误删正文）");

            // 缩进走的是正常编辑路径：字段内的策略对缩进一样生效
            loadControls(ef, {mk(5, 9, "k1", false), mk(12, 16, "k2", false)});
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

            // 文档模式（设计态）也画：作者要看得见边界才谈得上增删改。
            ef.setEditMode(EditMode::Document);
            const std::vector<uint8_t> f_doc = snapshot(ef);
            const size_t doc_mark = countCols(f_doc, fw, 0, fw - 1);
            expectTrue(doc_mark > 0, "文档模式也画标记（看不见字段就无从增删改）");

            // 只读模式不画：它是呈现与打印态，括号落进版面就印出来了。
            ef.setEditMode(EditMode::ReadOnly);
            const std::vector<uint8_t> f_ro = snapshot(ef);
            const size_t ro_mark = countCols(f_ro, fw, 0, fw - 1);
            expectTrue(ro_mark == 0, "只读模式：标记一个像素都不剩（印不出来）");

            ef.setEditMode(EditMode::Form);
            const std::vector<uint8_t> f_form = snapshot(ef);
            const size_t form_mark = countCols(f_form, fw, 0, fw - 1);
            expectTrue(form_mark > 0, "切回表单模式：标记又出现了（不是「切走一次就再也不画」）");

            // 回归：光标停在字段末尾时，右括号必须还在。
            //
            //   它和光标矩形重叠（光标中心正好压在字段右边缘上）。
            //   括号与光标画在同一张帧上时，画序反了就会在重叠处把括号盖掉 ——
            //   而那时光标是"擦旧画新"的，被盖掉的括号补不回来。
            //   现在括号画在页位图里、光标画在帧上，层序由构造保证（帧 = 页位图 +
            //   选区 + 光标），这条于是退化成回归判据：改层序的人会把它踩红。
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

            // (f5) 小点：独立开关，与光标无关 —— 开时所有控件右下角各一个方块。
            //   它不占宽度（不进文档流），所以开关不该重排，只改帧上的像素。
            //   判据取"帧逐字节"：开 → 帧变了且字形数不变；关 → 逐字节回到开之前
            //   （有残留就说明它不是"画上去就不管"，而是真的擦干净了）。
            {
                const std::vector<uint8_t> base = snapshot(ef);
                const int glyphs_before = ef.stats().glyph_count;
                EngineConfig cd = ef.config();
                cd.show_control_dots = true;
                ef.setConfig(cd);
                const std::vector<uint8_t> dots = snapshot(ef);
                expectTrue(dots != base, "开小点：帧上真的多了东西（每个控件右下角一个方块）");
                expectTrue(ef.stats().glyph_count == glyphs_before,
                           "小点不产生字形（它是一块填充，不是排版产物）");

                cd.show_control_dots = false;
                ef.setConfig(cd);
                expectTrue(snapshot(ef) == base, "关小点：帧逐字节回到开之前（没有残留）");
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

            // 这两条（画出来 / 擦干净）同时钉死一个失效点：标记画在页内容里，切模式
            //   要让那几页的内容重算。漏掉这一步，切回文档模式后括号会留在原处 ——
            //   表象是"随机不刷新"，从代码上极难看出来的那种。
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
            bw.line_spacing.factor = 1.75f;
            bw.language = "zh";
            std::string longtext;
            for (int i = 0; i < 4; ++i) longtext += "患者主诉：反复心悸三天，加重一天。";
            bw.setText(longtext);
            loadBlocks(ew, {bw});
            loadControls(ew, {mk(5, 60, "long", false)});
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

    if (runSection(12)) std::printf("\n[12] 上屏契约：引擎写过的区域必须累积到宿主读取为止\n");
    if (runSection(12)) {
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
        //  这一节要"第一次 present 写满整窗"，好和第二次的小矩形形成对比。
        //     用什么制造这一次大写入不影响结论：关括号会让占位消失、整篇重排，
        //     与换字段是同一类事件（排版输入变了，帧必须整窗重贴）。
        EditorEngine eg;
        if (!eg.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig cg;
        cg.zoom = 1.5f;
        eg.setConfig(cg);

        BlockSpec bg;
        bg.font_index = 0;
        bg.size_pt = 12.0f;
        bg.wm = WritingMode::HorizontalTB();
        bg.line_spacing.factor = 1.75f;
        bg.language = "zh";
        bg.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(eg, {bg});
        eg.relayout();

        auto TC = [](int a, int b, const char* id) {
            Control c;
            c.range.start = TextPos{0, static_cast<size_t>(a)};
            c.range.end = TextPos{0, static_cast<size_t>(b)};
            c.id = id;
            return c;
        };

        loadControls(eg, {TC(5, 9, "k1"), TC(12, 16, "k2")});

        /**
         * 待上屏的并集：把窗口内各页累积的脏矩形取出来（读后清零），给出面积与并集盒。
         *
         * 它就是宿主真正要贴的东西："欠着没贴"这件事落在页上，所以判据是"按页取矩形"，
         *   口径与被测的那条路完全一致：宿主读的就是这几个数。
         */
        auto drainDirty = [](EditorEngine& e, float* bw, float* bh) -> long long {
            long long area = 0;
            float x0 = 1e9f;
            float y0 = 1e9f;
            float x1 = -1e9f;
            float y1 = -1e9f;
            const int first = e.windowFirstPage();
            const int step = e.pagePxHeight() + e.pageGapPx();
            for (int p = first; p < first + e.windowPageCount(); ++p) {
                for (const Rect& r : e.takePageDirtyRects(p)) {
                    if (r.w <= 0.0f || r.h <= 0.0f) continue;
                    area += static_cast<long long>(std::ceil(r.w)) *
                            static_cast<long long>(std::ceil(r.h));
                    const float wy = static_cast<float>((p - first) * step) + r.y;
                    x0 = std::min(x0, r.x);
                    y0 = std::min(y0, wy);
                    x1 = std::max(x1, r.x + r.w);
                    y1 = std::max(y1, wy + r.h);
                }
            }
            if (bw != nullptr) *bw = (x1 > x0) ? (x1 - x0) : 0.0f;
            if (bh != nullptr) *bh = (y1 > y0) ? (y1 - y0) : 0.0f;
            return area;
        };

        eg.setEditMode(EditMode::Form);
        eg.setCaret(0, 6);  // 光标在 #0（反复心悸）
        eg.repaint();
        (void)drainDirty(eg, nullptr, nullptr);  // 把之前的账清零，下面量的才是这一串

        // 把闪烁相位摆到"隐藏"：这条路径只在隐藏半拍触发
        //    （若开头调了 stopBlink()，这一半概率就被屏蔽掉了）
        eg.showCaret(false);
        eg.repaint();
        float small_w = 0.0f;
        float small_h = 0.0f;
        (void)drainDirty(eg, &small_w, &small_h);

        // 制造"第一次 present 很大"：
        //    关掉括号 → 占位消失 → 整篇重排 → 窗口内每一页都重光栅。
        EngineConfig co = eg.config();
        co.show_brackets = false;
        eg.setConfig(co);
        eg.repaint();                          // 第一次 present
        const EditStats st1 = eg.editStats();  // 宿主此刻读到的账

        // 宿主看到 caretVisible == false，走它的补救路径
        const bool took_rescue = !st1.caret_visible;
        eg.showCaret(true);
        eg.repaint();  // 第二次 present
        const EditStats st2 = eg.editStats();
        float box_w = 0.0f;
        float box_h = 0.0f;
        const long long box_area = drainDirty(eg, &box_w, &box_h);

        std::printf("     隐藏相位那次 present：%d×%d px\n", static_cast<int>(small_w),
                    static_cast<int>(small_h));
        std::printf("     换字段后第一次 present 的本轮账：%d×%d px（窗口 %d×%d）\n",
                    st1.repainted_w, st1.repainted_h, eg.frameWidth(), eg.frameHeight());
        std::printf("     补救时第二次 present 的本轮账：%d×%d px\n", st2.repainted_w,
                    st2.repainted_h);
        std::printf("     待上屏并集：%d×%d px（%lld px²）｜ 走了补救路径=%d\n",
                    static_cast<int>(box_w), static_cast<int>(box_h), box_area,
                    took_rescue ? 1 : 0);

        expectTrue(took_rescue,
                   "前提成立：光标在隐藏半拍时，第一次 present 的 caretVisible 为 false "
                   "—— 宿主正是靠它决定走补救路径的");
        expectTrue(st2.repainted_w < st1.repainted_w,
                   "补救那次 present 的本轮脏矩形确实小得多 "
                   "（这就是宿主被误导的地方：拿它上屏会漏掉第一次 present）");
        // 并集必须盖住窗口内那几页的整段高度 —— 少算了，画布就停在旧排版上：
        //   引擎是对的、屏幕是旧的。
        const int win_px_h = eg.editStats().window_pages * eg.pagePxHeight();
        expectTrue(box_w == static_cast<float>(eg.frameWidth()) && box_h >= win_px_h,
                   "但累积的页脏矩形并集必须覆盖窗口内那几页（第一次 present 的功劳）"
                   "—— 少算了，画布就会停在旧排版上：引擎对、屏幕是旧的");
        expectTrue(small_w < static_cast<float>(eg.frameWidth()),
                   "对照：隐藏相位那次 present 远不是整页 —— 所以并集只能靠累积");

        // 读完即清零：下一次没写任何东西时，必须是空的
        float after_w = 0.0f;
        float after_h = 0.0f;
        (void)drainDirty(eg, &after_w, &after_h);
        expectTrue(after_w == 0.0f && after_h == 0.0f,
                   "页脏矩形是读后清零的（否则宿主每次都会重贴整页）");

        // 对照：换字段不重排，上屏量必须远小于整页。
        //
        //   这条钉死的是"括号常显"带来的真正好处：占位集合与光标无关，
        //   于是"点一下另一个字段"不引发重排，上屏量从"整页"掉到"一小块"。
        //   判据取"远小于整页"而不是某个精确像素数：精确值随字体/字号变。
        {
            EngineConfig ca = eg.config();
            ca.show_brackets = true;  // 上面为了制造整页重光栅关掉了，这里开回来
            eg.setConfig(ca);         // 开括号会重排（占位回来了）
            eg.setCaret(0, 6);
            eg.repaint();
            (void)drainDirty(eg, nullptr, nullptr);

            eg.setCaret(0, 14);  // 换到另一个字段 —— 排版不动
            eg.repaint();
            float cross_w = 0.0f;
            float cross_h = 0.0f;
            const long long cross_area = drainDirty(eg, &cross_w, &cross_h);
            std::printf("     ⑤ 换字段：上屏并集 %d×%d px（%lld px²）\n", static_cast<int>(cross_w),
                        static_cast<int>(cross_h), cross_area);
            expectTrue(cross_area > 0, "⑤ 换字段确实重贴了点什么（光标）");
            expectTrue(cross_area < static_cast<long long>(eg.frameWidth()) * eg.pagePxHeight() / 4,
                       "换字段不再整页重贴（占位与光标无关 → 排版不动）");
        }
    }

    if (runSection(13)) std::printf("\n[13] 光标在字段两端必须落在括号之内\n");
    if (runSection(13)) {
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
        bx.line_spacing.factor = 1.75f;
        bx.language = "zh";
        bx.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(ex, {bx});
        ex.relayout();

        Control k1;
        k1.range.start = TextPos{0, 5};
        k1.range.end = TextPos{0, 9};
        k1.id = "k1";
        Control k2;
        k2.range.start = TextPos{0, 12};  // 加
        k2.range.end = TextPos{0, 16};    // 天 之后
        k2.id = "k2";
        loadControls(ex, {k1, k2});
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

    if (runSection(14)) std::printf("\n[14] 相邻字段的括号顺序是 `][`，不是 `[]`\n");
    if (runSection(14)) {
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
        ba.line_spacing.factor = 1.75f;
        ba.language = "zh";
        ba.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(ea, {ba});
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
        loadControls(ea, {ka, kb});
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

    if (runSection(15)) std::printf("\n[15] 增量排版：脏段之后重排、前缀原样复用（档 2）\n");
    if (runSection(15)) {
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
            b.line_spacing.factor = 1.85f;
            b.space_after_pt = 6.0f;
            b.language = "zh";
            b.setText("第 " + std::to_string(i + 1) +
                      " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解，"
                      "未见黑矇与晕厥。");
            doc.push_back(std::move(b));
        }
        loadBlocks(e, doc);
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
        // 平移复用之后页/块/行三张表由同一次 rebuild 收尾；这条钉住"后缀重新分页不是
        // 只改了行盒、把索引落在旧页上"。
        expectTrue(e.layout().fragmentIndexValid(),
                   "★ 平移复用之后 FragmentIndex 仍自洽（页/块/行三表）");
        // 平移是"改坐标"不是"重排"，所以判据落在坐标上：坐标允许 1e-3 pt 的舍入差
        // （两种加法），结构性字段（页号/行号/段号/簇）必须逐位一致。
        expectTrue(e.auditLayout() == 0, "★ 平移复用的结果与全量重排一致（几何差 < 1e-3 pt）");
        expectPagesMonotonic("后缀平移复用之后");

        // 段数变化（回车分段）也必须能复用后缀：内容一个字没改，只是段边界挪了。
        //   这一格与"撑出一行"是两种不同的账：脏区在新版里占两段（前半段 + 新段），
        //   而段数增量让后缀的段号整体平移，段表、行盒与字形的段号都要跟着改。
        e.setCaret(100, 8);
        // 用带 render 的入口：editStats 里的那一份是呈现时抄下来的，
        //   不 render 就读到上一次的陈旧值（而陈旧值恰好也是"1 段/99 段"，读起来像是通过了）。
        const bool split = e.splitParagraphAndRender();
        expectTrue(split, "第 100 段中间回车分段");
        const EditStats sp = e.editStats();
        std::printf("     回车分段后：重排 %d 段 / 复用 %d 段 / 共 %d 段（%d 段，平移=%d，%s）\n",
                    sp.relaid_paragraphs, sp.reused_paragraphs, sp.total_paragraphs,
                    static_cast<int>(e.blocks().size()), sp.suffix_shifted ? 1 : 0,
                    sp.layout_was_full ? "全量" : "增量");
        expectTrue(!sp.layout_was_full, "分段后走的是增量排版（不是整篇重排）");
        expectTrue(sp.relaid_paragraphs <= 2 && sp.reused_paragraphs >= kParas - 1 - 100,
                   "★ 回车分段 → 只重排脏区（1~2 段），后缀全部复用");
        expectTrue(e.auditLayout() == 0, "★ 分段复用的结果与全量重排逐字段一致");
        expectPagesMonotonic("段数变化的后缀复用之后");

        // 合并回来：段首退格把两段并回一段，同样只该重排脏区。
        //   与分段是一对反向的开销 —— 两个方向都得走复用，否则用户连按退格会卡。
        const bool merged = e.backspaceAndRender();
        expectTrue(merged, "新段段首退格合并回一段");
        const EditStats mg = e.editStats();
        std::printf("     合并回来：重排 %d 段 / 复用 %d 段 / 共 %d 段（%d 段）\n",
                    mg.relaid_paragraphs, mg.reused_paragraphs, mg.total_paragraphs,
                    static_cast<int>(e.blocks().size()));
        expectTrue(!mg.layout_was_full && mg.reused_paragraphs >= kParas - 1 - 100,
                   "★ 退格合并 → 同样只重排脏区，后缀全部复用");
        expectTrue(e.auditLayout() == 0, "★ 合并复用的结果与全量重排逐字段一致");

        // 连续两次回车（中间不夹任何别的编辑）：两次都得把文档推下去。
        //   标脏写进的是"与底图那一版必然不同"的值，而 present 之后底图也变成了它 ——
        //   若下一次标脏写回同一个值，判据（new != base）读出来就是"没变"，
        //   这一页从此不再重画：症状是连续换行只有第一次把文档推下去。
        //   两条判据各管一头：一条脆的（画面必须成片地变），一条有界的（与从零重画几乎相同）。
        //   另起一个引擎来做，免得把这一段文档改长（后面的小节按 200 段算账）。
        {
            EditorEngine ep;
            expectTrue(ep.init({cjkPath, mnPath, latinPath}), "引擎初始化（连续两次回车）");
            ep.setConfig(c);
            loadBlocks(ep, e.blocks());
            ep.relayout();
            ep.setCaret(100, 8);
            const bool push1 = ep.splitParagraphAndRender();
            expectTrue(push1, "第一次回车");
            const size_t fbytes = ep.frameBytes();
            std::vector<uint8_t> frame_before(ep.frame(), ep.frame() + fbytes);
            const bool push2 = ep.splitParagraphAndRender();
            expectTrue(push2, "紧接着第二次回车");
            long long moved = 0;
            {
                const uint8_t* fa = ep.frame();
                for (size_t k = 0; k + 3 < fbytes; k += 4) {
                    if (fa[k] != frame_before[k] || fa[k + 1] != frame_before[k + 1] ||
                        fa[k + 2] != frame_before[k + 2])
                        ++moved;
                }
            }
            long long max_ch_shift = 0;
            int box[4] = {0, 0, 0, 0};
            const long long shift_fresh = frameDiffAgainstFresh(
                ep, {cjkPath, mnPath, latinPath}, {}, EditMode::Document, &max_ch_shift, box);
            std::printf(
                "     第二次回车：画面变了 %lld 像素 ｜ 与从零重画差 %lld 像素"
                "（最大通道差 %lld，x %d~%d / y %d~%d）\n",
                moved, shift_fresh, max_ch_shift, box[0], box[2], box[1], box[3]);
            // 脆的那条：编辑点以下的每一行都往下挪了一格，画面必须成片地变。
            //   漏重画时只有光标那几个像素会动（几百），与这里的量级差两个数量级。
            expectTrue(moved > 20000, "★ 第二次回车把画面成片换掉了（标脏不能退化成\"没变\"）");
            // 有界的那条：与"从零重排重画"比，允许抗锯齿在量化边界上翻格。
            //   平移走的是"旧坐标 + 增量"，全量重排走的是"从头累加"，两者在浮点末位上
            //   可以差到 1e-3 pt（auditLayout 正为此留了容差）；落在 Skia 的 1/16 px
            //   量化边界上的那几个字形会翻一格 —— 实测 562 px、最大通道差 3，
            //   而"整页没重画"是几十万像素、通道差上百。上界按后者量级取。
            expectTrue(shift_fresh < 5000 && max_ch_shift <= 4,
                       "★ 连续两次回车之后：增量帧与从零重画几乎逐像素相同（只差抗锯齿翻格）");
            expectTrue(ep.auditLayout() == 0, "连续两次回车之后，增量排版与全量重排逐字段一致");
        }

        // 平移复用的反面：后缀里有竖排段 → "横排行槽折线"那条规则不成立 → 必须续排。
        // 这条同时证明常规续排路径没有被平移路径挤掉（两条路径都要被走到）。
        {
            EditorEngine ev;
            expectTrue(ev.init({cjkPath, mnPath, latinPath}), "引擎初始化（竖排后缀）");
            ev.setConfig(c);
            std::vector<BlockSpec> d2;
            for (int i = 0; i < 12; ++i) {
                BlockSpec b;
                b.font_index = 0;
                b.size_pt = 14.0f;
                b.line_spacing.factor = 1.85f;
                b.space_after_pt = 6.0f;
                b.language = "zh";
                b.setText("第 " + std::to_string(i + 1) +
                          " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解。");
                d2.push_back(std::move(b));
            }
            BlockSpec v;
            v.font_index = 0;
            v.size_pt = 14.0f;
            v.line_spacing.factor = 1.9f;
            v.wm = WritingMode::VerticalRL();
            v.language = "zh";
            v.setText("患者主诉：反复心悸3天，加重1天。现病史：患者3天前无明显诱因出现心悸。");
            d2.push_back(std::move(v));
            loadBlocks(ev, d2);
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

        // 耗时比值（尾部敲字对全量、中部敲字对全量、后缀平移对续排）不在这里判：
        //   它们会被同机上的其它负载顶翻，搬到第 78 节，只由 HEDITOR_TEST_PERF=1 选中。

        // 页指纹的增量化：整篇重算 vs 只算脏段覆盖的那几页。
        //
        // 端到端（编辑 + 重排 + 上屏）的分段计时不在这里量：它同样是耗时判据，搬到第 78 节。
        // 这里只留页数这一组结构量 —— 脏页数是确定的，不受调度影响。
        {
            const std::vector<BlockSpec> pristine = e.blocks();
            auto restore = [&]() {
                loadBlocks(e, pristine);
                e.relayout();
                e.repaint();
            };
            std::string wrap_text;
            for (int i = 0; i < 40; ++i) wrap_text += "\xE6\xB5\x8B";  // 40 个汉字，必定撑出一行

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

            // 分段耗时（编辑 / 重排 / 呈现）与"平移复用比续排重排快多少"同样是耗时判据，
            //   一并搬到第 78 节。
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
            loadControls(e, {k1});
            e.setEditMode(EditMode::Form);
            // EditStats 由 presentInternal 刷新：装载与改模式只改状态与排版，
            // 不呈现 —— 要读这一轮的账就得先呈现一次。
            e.repaint();
            const EditStats cs = e.editStats();
            std::printf("     挂控件：重排 %d 段 / 复用 %d 段 / 共 %d 段（%s）\n",
                        cs.relaid_paragraphs, cs.reused_paragraphs, cs.total_paragraphs,
                        cs.layout_was_full ? "全量" : "增量");
            // 这里有一个能力缺口：树上控件是节点，换字段名单必须重建整棵树，而装载按
            //   "整份文档换了"整篇标脏，所以增量重排在这条路上消失了 —— 判据只落在
            //   还能保证的那一半（段数不变）。待补的能力是"装载时按新旧块流差异推断
            //   脏区"，记在 memory 里，不在这里留一条红的。旧的块流装载路径确实能定位
            //   到第 120 段，那条断言也随路径一起退役了。
            expectTrue(cs.total_paragraphs == kParas,
                       "换控件名单之后段数不变（" + std::to_string(cs.total_paragraphs) + " / " +
                           std::to_string(kParas) + "），本次重排范围：全篇 " +
                           std::to_string(cs.relaid_paragraphs) + " 段 / 复用 " +
                           std::to_string(cs.reused_paragraphs) + " 段");
            expectTrue(e.auditLayout() == 0, "挂控件之后的排版同样与对照一致");
        }
    }

    if (runSection(16))
        std::printf("\n[16] 增量上屏 == 从零上屏：整帧逐字节比（含脏页集合对不对）\n");
    if (runSection(16)) {
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
            b.line_spacing.factor = 1.85f;
            b.space_after_pt = 6.0f;
            b.language = "zh";
            b.setText("第 " + std::to_string(i + 1) +
                      " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解，"
                      "未见黑矇与晕厥。");
            doc.push_back(std::move(b));
        }
        loadBlocks(a, doc);
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
            int box[4] = {-1, -1, -1, -1};
            const long long diff = frameDiffAgainstFresh(a, fonts16, ctrl, mode, &max_ch, box);
            const long long total_px = static_cast<long long>(a.frameBytes() / 4);
            const bool ok = allow_aa_noise ? (diff >= 0 && diff * 100000 <= total_px && max_ch <= 1)
                                           : (diff == 0);
            std::printf(
                "     %s：帧差 %lld 像素（最大通道差 %lld，盒 x %d~%d / y %d~%d，窗口 %d 页，"
                "共 %d 页，阈值 %s）\n",
                label, diff, max_ch, box[0], box[2], box[1], box[3], a.editStats().window_pages,
                a.editStats().total_pages, allow_aa_noise ? "≤十万分之一像素且通道差≤1" : "0");
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
            loadControls(a, ctrl);
            a.setEditMode(EditMode::Form);
            a.repaint();
            compare("表单括号", ctrl, EditMode::Form, /*allow_aa_noise=*/true);
        }
    }

    if (runSection(17))
        std::printf("\n[17] 跨段编辑：回车分段 / 段首退格合并 / 段末 Delete 合并\n");
    if (runSection(17)) {
        const std::vector<std::string> fonts17{cjkPath, mnPath, latinPath};
        auto mkBlocks = [](const std::vector<std::string>& texts) {
            std::vector<BlockSpec> v;
            for (const std::string& t : texts) {
                BlockSpec b;
                b.font_index = 0;
                b.size_pt = 14.0f;
                b.line_spacing.factor = 1.85f;
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
            loadBlocks(e, mkBlocks({"甲乙丙", "", "丁戊己"}));
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
            loadBlocks(e, mkBlocks({"患者主诉：反复心悸3天加重1天。"}));
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
                           t.line_spacing == h.line_spacing &&
                           t.space_after_pt == h.space_after_pt && t.language == h.language &&
                           t.wm.inline_axis == h.wm.inline_axis &&
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
            loadBlocks(e, mkBlocks({"第一段内容甲乙丙丁。", "第二段内容戊己庚辛。",
                                    "第三段内容壬癸子丑。", "第四段内容寅卯辰巳。"}));
            e.relayout();
            e.repaint();
            e.setCaret(2, 5);
            expectTrue(e.splitParagraphAndRender(), "在第 2 段中间回车分段");
            const EditStats sp = e.editStats();
            std::printf("     4 段文档在第 2 段分段：重排 %d 段 / 复用 %d 段 / 共 %d 段（%s）\n",
                        sp.relaid_paragraphs, sp.reused_paragraphs, sp.total_paragraphs,
                        sp.layout_was_full ? "全量" : "增量");
            // 段数变了不等于后缀不能复用：内容一个字没变，只是段边界挪了 ——
            //   前缀 [0,2) 照旧，脏区占新版的 [2,4)（原第 2 段拆成两段），后缀 [4,5) 整块复用。
            expectTrue(
                !sp.layout_was_full && sp.relaid_paragraphs == 2 && sp.reused_paragraphs == 1,
                "★ 段数变了：只重排脏区 [2,4)，后缀 [4,5) 照旧复用（段号整体平移）");
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
            loadBlocks(byBack, mkBlocks(texts));
            loadBlocks(byDel, mkBlocks(texts));
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
            loadBlocks(e, mkBlocks({"甲", "乙"}));
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
            const long long b0 = e.blockedEdits();
            e.setCaret(0, 0);
            expectTrue(!e.splitParagraph(), "只读模式：回车分段被拒");
            e.setCaret(1, 0);
            expectTrue(!e.backspace(), "只读模式：段首退格（合并）被拒");
            expectTrue(e.blockedEdits() == b0 + 2, "两次被拒都记在 blocked_edits 上");
        }

        // 表单策略：断点能不能写，由"插入策略"决定（回车 = 在断点插入一个段落分隔符）
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（表单策略）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            loadBlocks(e, mkBlocks({"甲", "乙"}));
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
            loadControls(e, ctrls);
            e.setEditMode(EditMode::Form);

            e.setCaret(1, 0);
            const long long b1 = e.blockedEdits();
            // 段首这一格是字段 ed 的内容起点：表单模式的退格到这里就停住 —— 不删、不合并，
            //   也不把光标带到括号外面（那是文档模式那套括号规则的走法）。
            expectTrue(!e.backspace(), "★ 段首退格：表单模式停在字段内容起点，不合并");
            expectTrue(e.caretSide() == 1 && e.blocks().size() == 2 && e.blockedEdits() == b1 + 1,
                       "光标仍在字段里（侧别 1），文档一个字都没动，且被拒记了账");
            expectTrue(!e.backspace(), "★ 再按一次：同样是拒");
            expectTrue(e.blockedEdits() == b1 + 2 && e.blocks().size() == 2, "仍然一个字都没动");
            e.setCaret(1, 0);  // 重新落回字段里（setCaret 的默认侧别就是"进入控件"）
            expectTrue(e.splitParagraph(), "表单模式：在可编辑字段里回车分段 → 放行");
            expectTrue(e.blocks().size() == 3, "确实断成了三段（空段 + 甲 + 乙）");
        }

        // 策略层直接测：把"能不能在这里分段/合并"与光标能否到达分开。
        //   表单模式下 setCaret 会把光标投影进最近的字段，所以"光标停在所有控件之外"
        //   这件事在表单模式里根本到不了 —— 只能问引擎的落点判定（policyAt / policyErase）。
        //
        //   持树路径里这两个动作都有真实入口，所以这里直接用动作观察：
        //   分段 = 在断点插一枚段落标记（判据与"在这里插字"完全相同），合并 = 删掉前一段
        //   那枚段落标记（判"标记自己那一位"能不能编辑）。不再另立 canSplit / canMerge。
        {
            EditorEngine es;
            if (!es.init(fonts17)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            EngineConfig c17;
            c17.zoom = 1.0f;
            es.setConfig(c17);
            auto mk17 = [](int para, int so, int eo, const char* id, bool ro) {
                Control c;
                c.range.start = TextPos{para, static_cast<size_t>(so)};
                c.range.end = TextPos{para, static_cast<size_t>(eo)};
                c.id = id;
                c.read_only = ro;
                return c;
            };
            auto reload17 = [&](const std::vector<Control>& cs) {
                loadBlocks(es, mkBlocks({"甲乙丙丁", "戊己庚辛"}));
                loadControls(es, cs);
                es.setEditMode(EditMode::Form);
            };

            reload17({mk17(0, 0, 4, "a", false)});
            es.setCaret(0, 2);
            expectTrue(es.splitParagraph(), "可编辑字段之内：可以分段");

            reload17({mk17(0, 0, 2, "a", false)});
            es.setCaret(0, 3);
            expectTrue(!es.splitParagraph(), "所有控件之外：不可以分段");

            // 只读字段那一格：另一段里必须再放一个可编辑字段。只放一个只读字段的话，
            //   整份文档一个能改的都没有，表单就退化成文档模式了（安全阀，见 formDegraded），
            //   于是"只读字段里不能分段"这条会假过。
            reload17({mk17(0, 0, 4, "ro", true), mk17(1, 0, 4, "ed", false)});
            es.setCaret(0, 2);
            expectTrue(!es.splitParagraph(), "只读字段之内：不可以分段");

            reload17({mk17(0, 0, 4, "a", false)});
            es.setEditMode(EditMode::ReadOnly);
            es.setCaret(0, 2);
            expectTrue(!es.splitParagraph(), "只读模式：一律不可以分段");

            reload17({mk17(0, 0, 4, "a", false)});
            es.setCaret(0, 4);
            expectTrue(es.splitParagraph(), "文档模式：控件之外也能分段");

            // 合并判的是"前一段那枚段落标记自己那一位可不可编辑"，不是光标落在哪。
            //   字段整个落在某一段里时，标记在字段之外 ⇒ 表单模式下拒，而且这一下不会先把
            //   光标带出字段再拒（删除不兼职移动）。
            reload17({mk17(0, 0, 4, "a", false), mk17(1, 0, 4, "b", false)});
            es.setCaret(1, 0);
            expectTrue(!es.backspaceAndRender(),
                       "★ 段首退格：段落标记落在字段之外，直接拒（不先把光标退出字段）");
            expectTrue(es.caret().para == 1 && es.blocks().size() == 2, "光标没动、段数没变");

            // 反向一半：先在字段里分段，字段就被拉成跨两段，那枚标记随之落进字段内部 ⇒ 放行。
            reload17({mk17(0, 0, 4, "a", false)});
            es.setCaret(0, 2);
            expectTrue(es.splitParagraph(), "先把可编辑字段从中间分段（字段因此变跨段）");
            expectTrue(es.controls().size() == 1, "分段之后字段还在（没有掉成一个）");
            es.setCaret(1, 0);
            expectTrue(es.backspaceAndRender(), "★ 标记落进跨段的字段内部：可以合并");

            reload17({mk17(0, 0, 4, "a", true)});
            es.setEditMode(EditMode::Document);
            es.setCaret(1, 0);
            expectTrue(es.backspaceAndRender(), "文档模式：合并点在哪都行");
        }

        // 控件区间的适配：分段把区间拆到两段，合并再拼回来（往返必须回到原样）
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（控件适配）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            loadBlocks(e, mkBlocks({"甲乙丙丁戊己庚辛壬癸"}));  // 10 个汉字
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
            loadControls(e, ctrls);
            e.setEditMode(EditMode::Form);

            // 拆开之前先把四个区间抄下来：合并回来要与它逐字段相同，那才是"往返不变"。
            //   不能拿 controls() 的引用当"拆开之后"的样子 —— 它是常引用、每次编辑就地重建，
            //   留引用等于自己跟自己比（那条断言会永远为真，什么也没证明）。
            const std::vector<Control> state_before = e.controls().all();

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
            // 空字段这一格与老路径不同，且是同一口径推出来的：光标落在 5 这个位置上时，
            //   它是"最内层含端点"的那一个（零宽区间落在邻居端点上仍算包含，见
            //   rangeContainsForNesting），于是标记插在它那一对括号中间 —— 它和 across
            //   是同一个形状（跟着断到两段），只是内容为零。老路径按区间算术把它整段搬到
            //   新段段首（1:0)-(1:0)，那条规则是"控件不是节点"时代的产物（那时控件只是
            //   调用方递进来的一张区间表，光标落在哪跟区间怎么搬是两件事）。
            expectTrue(rangeIs("empty", 0, 5, 1, 0),
                       "落在断点上的空字段跟着断到两段（光标判它最内层，标记插进了它里面）");

            // 合并回来：四个区间必须与拆开之前逐字段相同（真正的往返不变）。
            e.setCaret(1, 0);
            expectTrue(e.backspace(), "把刚拆开的两段合回去");
            bool same = (e.controls().size() == static_cast<int>(state_before.size()));
            for (const Control& a : state_before) {
                bool found = false;
                for (int i = 0; i < e.controls().size(); ++i) {
                    const Control& b = e.controls().at(i);
                    if (b.id != a.id) continue;
                    found = true;
                    if (b.range.start.para != a.range.start.para ||
                        b.range.start.offset != a.range.start.offset ||
                        b.range.end.para != a.range.end.para ||
                        b.range.end.offset != a.range.end.offset) {
                        same = false;
                    }
                }
                if (!found) same = false;
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
                b.line_spacing.factor = 1.85f;
                b.space_after_pt = 6.0f;
                b.language = "zh";
                b.setText("第 " + std::to_string(i + 1) +
                          " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解，"
                          "未见黑矇与晕厥。");
                doc.push_back(std::move(b));
            }
            loadBlocks(e, doc);
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
            // 回车只把脏区从 1 段撑成 2 段，后缀（99 段）整块复用 —— 不是续排 101 段。
            expectTrue(first.total_paragraphs == 201 && !first.layout_was_full &&
                           first.relaid_paragraphs == 2 && first.reused_paragraphs == 99,
                       "★ 长文档里回车：只重排脏区 2 段，前缀 100 段与后缀 99 段都不动");
        }

        // 文档模式下的多段编辑序列：每一步都要"增量 == 全量"
        {
            EditorEngine e;
            expectTrue(e.init(fonts17), "引擎初始化（随机跨段编辑）");
            EngineConfig c;
            c.zoom = 1.0f;
            e.setConfig(c);
            loadBlocks(
                e, mkBlocks({"第一段：患者诉心悸。", "第二段：休息后缓解。", "第三段：无晕厥。"}));
            e.relayout();
            e.repaint();

            uint32_t seed = 20261010u;
            auto rnd = [&](uint32_t n) {
                seed = seed * 1103515245u + 12345u;
                return n ? (seed >> 16) % n : 0u;
            };
            int bad = 0, splits = 0, merges = 0, full = 0, expect_full = 0, full_with_prefix = 0;
            int head_steps = 0;
            for (int step = 0; step < 40; ++step) {
                const int np = static_cast<int>(e.blocks().size());
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
                // 整篇重排的两侧判据（比"全量很少"强得多）：
                //   必然整篇重排 —— 脏段是第 0 段、且它后面一段都没有（"后缀复用"无从谈起，
                //     续排也没有前缀可以接着走）。
                //   绝不整篇重排 —— 脏段之前有前缀（dirty > 0）。段数变、行数变都不再是理由：
                //     后缀整体平移那一级已经把"段边界挪了"与"行结构变了"都吃掉。
                const int n_after = static_cast<int>(e.blocks().size());
                const bool has_suffix = dirty >= 0 && dirty + 1 < n_after;
                if (dirty <= 0 && !has_suffix) ++expect_full;
                if (dirty <= 0) ++head_steps;
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
            expectTrue(full >= expect_full && full <= head_steps,
                       "★ 整篇重排只出现在「脏段是第 0 段」的那几步，"
                       "且没有后缀的那几步必然整篇重排（两侧都数）");
            expectTrue(frameDiffAgainstFresh(e, fonts17, {}, EditMode::Document, nullptr) == 0,
                       "★ 随机跨段编辑之后，增量上屏 == 从零上屏（逐字节）");
        }
    }

    if (runSection(18)) std::printf("\n[18] 相邻字段共享边界：打字、删空都不许把括号画错位\n");
    if (runSection(18)) {
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
        b18.line_spacing.factor = 1.75f;
        b18.language = "zh";
        b18.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(e, {b18});
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
        loadControls(e, {ka, kb});
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
            loadBlocks(e4, {b18});
            e4.relayout();
            Control k1;
            k1.range.start = TextPos{0, 5};
            k1.range.end = TextPos{0, 9};
            k1.id = "a";
            Control k2;  // 空字段，紧挨着 A 的末尾
            k2.range.start = TextPos{0, 9};
            k2.range.end = TextPos{0, 9};
            k2.id = "empty";
            loadControls(e4, {k1, k2});
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

        // (e) 两个区间完全相同的空字段落在同一个偏移上：只有下标小的那个吸收新字，
        //     另一个整对端点右移、仍然为空 —— 谁也不许写出 start > end 的非法区间。
        //     这一格走完整路径（建树 → 装载 → 打字 → 导出），量的是结果而不是内部函数：
        //     空字段是"一个位置"，它的排位与括号槽序同一套规则（在"到此结束的字段"之后、
        //     在"从此开始的字段"之前，同一偏移上的多个空字段按下标排）。
        {
            EditorEngine e6;
            if (!e6.init({cjkPath, mnPath, latinPath})) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e6.setConfig(c18);
            Control e1;
            e1.range.start = TextPos{0, 9};
            e1.range.end = TextPos{0, 9};
            e1.id = "e1";
            Control e2;
            e2.range.start = TextPos{0, 9};
            e2.range.end = TextPos{0, 9};
            e2.id = "e2";
            loadBlocks(e6, {b18}, {e1, e2});
            e6.setEditMode(EditMode::Form);
            e6.setCaret(0, 9);
            const int owner6 = e6.focusedControlIndex();
            e6.typeTextAndRender("Y");
            const Control& r1 = e6.controls().at(0);
            const Control& r2 = e6.controls().at(1);
            std::printf("     两个相同的空字段：主人=#%d  e1=[%zu,%zu)  e2=[%zu,%zu)\n", owner6,
                        r1.range.start.offset, r1.range.end.offset, r2.range.start.offset,
                        r2.range.end.offset);
            expectTrue(owner6 == 0, "同一处两个空字段里，下标小的那个是最内层命中");
            expectTrue(r1.range.start.offset == 9 && r1.range.end.offset == 10,
                       "吸收者的空区间被新字填上（9 → 10）");
            expectTrue(r2.range.start.offset == 10 && r2.range.end.offset == 10,
                       "另一个空字段整对端点右移、仍然为空（不会写成 [10,9)）");
            expectTrue(!spansCross(r1.range, r2.range) && !(r2.range.end < r2.range.start),
                       "★ 两段区间合法且不相交（该有的形态是 [Y][]）");
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
            loadBlocks(e5, {b18});
            e5.relayout();
            Control only;
            only.range.start = TextPos{0, 5};
            only.range.end = TextPos{0, 9};
            only.id = "a";
            loadControls(e5, {only});
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
            loadBlocks(e6, {b18});
            e6.relayout();
            loadControls(e6, {ka, kb});
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
            loadBlocks(e7, {b18});
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
            loadControls(e7, {m1, m2, m3});
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
            // 偏移 7 上有四个位置（] [ ] [）。默认落点是"最内层含端点"那个 ——
            //   也就是进到空字段自己那一对括号之间：只有停在这里，往空字段里打字才进得去
            //   （归属与落点是同一个量，见引擎的 caretSide）。
            expectTrue(caret8 >= xs8[1] - 0.02f && caret8 <= xs8[2] + 0.02f,
                       "★ 光标落在空字段自己那一对括号之间（位置由偏移 + 侧别定，不由后面的槽挤）");

            // 像素证据：三个字段都可编辑时括号同色，像素上分不出谁在左，所以做一次差分 ——
            //   只把中间那个改成只读（区间不变）再排一遍，两帧之间变的列就只有它那一对括号
            //   （从蓝变灰）。那一对落在哪儿于是可以逐列判。
            //   为什么不用"灰"这个颜色本身当判据：正文黑字的抗锯齿尾巴正好也落在灰带里
            //   （实测它就压在左边第一个槽之前），会把两侧的墨迹混进窗口。
            std::vector<Control> now8 = e7.controls().all();
            now8[1].read_only = true;
            loadControls(e7, now8);
            e7.relayout();
            e7.repaint();
            const std::vector<uint8_t> fa(e7.frame(), e7.frame() + e7.frameBytes());
            now8[1].read_only = false;
            loadControls(e7, now8);
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
            loadBlocks(e8, {b18});
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
            loadControls(e8, {n1, n2, n3});
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
            loadBlocks(e9, {b18});
            e9.relayout();
            loadControls(e9, {n1, n2, n3});
            e9.setEditMode(EditMode::Form);
            e9.setCaret(0, 9);
            for (int i = 0; i < 2; ++i) e9.backspaceAndRender();

            std::vector<size_t> walk;
            std::vector<int> walk_side;
            int worst = 0;
            bool left_field = false;
            e9.setCaret(0, 5);
            for (int i = 0; i < 8; ++i) {
                const size_t before = e9.caret().offset;
                e9.moveCaretAndRender(1);
                const size_t now = e9.caret().offset;
                walk.push_back(now);
                walk_side.push_back(e9.caretSide());
                worst = std::max(worst, static_cast<int>(now) - static_cast<int>(before));
                if (now < 5 || now > 7) left_field = true;  // 走出了 #0 = [5,7]
            }
            std::printf("     从 5 连按 8 次右键：");
            for (size_t i = 0; i < walk.size(); ++i) std::printf(" %zu/%d", walk[i], walk_side[i]);
            std::printf("（单步最大跨度 %d）\n", worst);
            expectTrue(worst <= 1, "★ 方向键一次只走一格（没有瞬移）");
            // 一次一格：要么同一个偏移上侧别 +1，要么跨到下一个偏移（侧别归零）
            bool cell_ok = walk.size() == 8;
            for (size_t i = 0; i < walk.size() && cell_ok; ++i) {
                const size_t prev_off = (i == 0) ? 5 : walk[i - 1];
                const int prev_side = (i == 0) ? 1 : walk_side[i - 1];
                // 走到内容末尾之后再按就停在原地（落点不可插入 → 收束回边界）。
                if (walk[i] == prev_off && walk_side[i] == prev_side) continue;
                const bool in_offset = (walk[i] == prev_off && walk_side[i] == prev_side + 1);
                const bool next_off = (walk[i] > prev_off && walk_side[i] == 0);
                if (!in_offset && !next_off) cell_ok = false;
            }
            expectTrue(cell_ok, "★ 一次跨一枚簇：括号占位也算一格（不跳格）");
            expectTrue(!left_field && walk.back() == 7 && walk_side.back() == 0,
                       "★ 停在 #0 的内容末尾（cp 7 侧别 0）—— 方向键不越过字段边界");

            e9.setCaret(0, 8);  // 在 #2 里（删空之后 #2 = [7,9)）
            std::vector<size_t> back;
            for (int i = 0; i < 9; ++i) {
                e9.moveCaretAndRender(-1);
                back.push_back(e9.caret().offset);
            }
            std::printf("     从 8 连按 9 次左键：");
            for (size_t o : back) std::printf(" %zu", o);
            std::printf("\n");
            expectTrue(back.front() == 7 && back.back() == 7,
                       "★ 从 #2 里往左走到它自己的内容起点就停住（不再跨进上一个字段）");
        }
    }

    if (runSection(19))
        std::printf("\n[19] 点击归属：共享边界上点的是哪一个字段（含三层嵌套样例）\n");
    if (runSection(19)) {
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
        b19.line_spacing.factor = 1.75f;
        b19.language = "zh";
        b19.setText("患者主诉：反复心悸三天，加重一天。");
        loadBlocks(e, {b19});
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
        loadControls(e, {t1, t2, t3});
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
            loadBlocks(e, {b19});
            e.setEditMode(EditMode::Form);
            std::vector<Control> ro = {t1, t2, t3};
            ro[0].read_only = true;  // #0 只读，#1 可编辑，边界 7 仍共享
            loadControls(e, ro);
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
            std::printf(
                "     #0 只读时点偏移 7：不带归属 caret=%zu field=%d ｜ 带归属 caret=%zu "
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
            loadBlocks(e3, {b19});
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
            loadControls(e3, {outer, mid, inner});
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

    if (runSection(21))
        std::printf("\n[21] 控件边界总覆盖：相邻 / 嵌套 / 换行 / 删除 / 添加 / 光标\n");
    if (runSection(21)) {
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
            b.line_spacing.factor = 1.75f;
            b.language = "zh";
            b.setText(text);
            loadBlocks(e, {b});
            loadControls(e, cs);
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
                    // 占位簇的 owner 是产生端句柄（持树时是控件节点 id），判据要比的
                    //   是对外下标 —— 翻一次再比（见 controlIndexOfOwner 的说明）。
                    out.push_back(SlotInfo{static_cast<int>(c.bracket_side), c.read_only ? 1 : 0,
                                           e.controlIndexOfOwner(c.owner_control), lb.line,
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
                    if (spansCross(e.controls().at(i).range, e.controls().at(j).range))
                        return false;
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
            expectTrue(ranges() == "[5,8) [8,10) [10,12)", "★ 字进 #0，后两个整体右移（首尾相接）");
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
                loadControls(e, withRo(2, true));
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
                std::printf(
                    "     五个槽从左到右：亮蓝墨量 %d %d %d %d %d ｜ 灰墨量 %d %d %d %d %d\n",
                    blue_n[0], blue_n[1], blue_n[2], blue_n[3], blue_n[4], gray_n[0], gray_n[1],
                    gray_n[2], gray_n[3], gray_n[4]);
                std::printf("     亮蓝重心 %.2f %.2f %.2f ｜ 灰重心 %.2f %.2f\n", blue_c[0],
                            blue_c[1], blue_c[2], gray_c[3], gray_c[4]);
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
                loadControls(e, withRo(2, false));
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
            std::printf(
                "     长文档 %d 行；第 0 行止于 %zu，第 1 行起于 %zu；"
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
            expectTrue(ranges() == "[5,8) [8,10) [10,12)", "★ 归 #0：字进 #0，#1 #2 整体右移");
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

        std::printf("     ---- 光标移动：停在字段边界上、选区不受限、点击命中即落点 ----\n");
        {
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            // 表单模式：光标（插入点）只在可编辑控件的内容里。方向键走到内容两端就停住 ——
            //   括号占位那两格不属于内容，跨字段移动由 Tab 负责（见 tabFrom）。
            e.setCaret(0, 5);
            expectTrue(e.caretSide() == 1, "起点在 #0 的内容开头（cp 5 侧别 1）");
            e.moveCaretBy(1);
            expectTrue(e.caret().offset == 6, "右移一格到 6（#0 里第二个字之前）");
            e.moveCaretBy(1);
            expectTrue(e.caret().offset == 7 && e.caretSide() == 0,
                       "★ 右移到 #0 的内容末尾（cp 7 侧别 0，仍在 ] 之内）");
            expectTrue(!e.moveCaretBy(1), "★ 再右移：目标是 ] 之后那一格（不可插入）→ 不动");
            expectTrue(e.caret().offset == 7 && e.caretSide() == 0, "光标没有被带到括号外");

            e.setCaret(0, 8);
            e.moveCaretBy(-1);
            expectTrue(e.caret().offset == 7 && e.caretSide() == 2,
                       "★ 从 #1 里左移停在它的内容开头（cp 7 跨过 ] 与 [，侧别 2），不跨过 [ —— "
                       "实际 " +
                           std::to_string(e.caret().offset) + "/" + std::to_string(e.caretSide()));

            // 选区不受这条限制：Shift + 左能盖住字段之外的正文（核对上文、复制的刚需）。
            e.setCaret(0, 7);
            e.moveCaretBy(-1, /*extend=*/true);
            expectTrue(e.caret().offset == 6 && e.hasSelection(),
                       "★ Shift+左：选区扩展到 #0 里（扩展不受落点限制）");
            e.setCaret(0, 5);
            e.moveCaretBy(-1, /*extend=*/true);
            expectTrue(e.caret().offset == 5 && e.caretSide() == 0 && e.hasSelection(),
                       "★ 从内容开头再 Shift+左：选区端点走到 [ 那一格（选区能越界，光标不画）");
            expectTrue(!e.caretInsertable(), "★ 那个端点不可插入 —— 光标不显示在那儿（只画选区）");

            // 换字段：Tab 走的是"可编辑控件"这条线，不受光标落点限制。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, false), mk21(9, 11, false)});
            e.setCaret(0, 6);
            expectTrue(e.tabCaret(false) && e.focusedControlIndex() == 1,
                       "★ Tab：光标从 #0 跳到 #1（跨字段的动作只有它）");

            e.setCaret(0, 9);
            e.moveCaretToLineStart();
            expectTrue(e.caret().offset == 7 && e.caretSide() == 2,
                       "Home 收到字段内容开头（行首 0 在字段外）—— 实际 " +
                           std::to_string(e.caret().offset) + "/" + std::to_string(e.caretSide()));
            e.moveCaretToLineEnd();
            expectTrue(e.caret().offset == 9 && e.caretSide() == 0,
                       "End 收到字段内容末尾（行尾 17 在字段外）");

            // 程序化定位不受限：探针要能把光标摆到任意位置核对策略，画不画由 caretInsertable 定。
            e.setCaret(0, 4);
            expectTrue(e.focusedControlIndex() == -1,
                       "程序化定位可以落在字段之外（宿主/探针的摆位入口）");
            expectTrue(!e.caretInsertable(), "★ 那个位置的插入点不成立 —— 光标不显示");
            const long long blocked0 = e.blockedEdits();
            expectTrue(!e.typeTextAndRender("X"), "★ 字段外打字被拒（不可插入 = 打不进字）");
            expectTrue(e.blockedEdits() == blocked0 + 1, "被拒的编辑记了账（宿主能看见）");

            // 只读字段：程序化定位进得去，字打不进去，光标也不画。
            reset(kText, {mk21(5, 7, false), mk21(7, 9, true)});
            e.setCaret(0, 8);
            expectTrue(e.focusedControlIndex() == 1,
                       "程序化定位能落在只读字段里（归属照算，策略拒绝的是编辑）");
            expectTrue(!e.caretInsertable(), "★ 只读字段里的插入点不成立 —— 光标不显示");
            const long long blocked1 = e.blockedEdits();
            expectTrue(!e.typeTextAndRender("X") && e.blockedEdits() == blocked1 + 1,
                       "★ 只读字段里打字被拒，且记账");

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
            std::printf(
                "     横扫 x ∈ [%.1f, %.1f]：采样 %d 点，落点被改写的 %d 个，"
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

    if (runSection(22)) std::printf("\n[22] 分节强制分页：模型编译产物落到引擎，增量与全量一致\n");
    if (runSection(22)) {
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
        int id = 3;
        auto para = [&](const std::string& text) {
            const std::vector<uint32_t> cps = asc(text);
            return std::vector<doc::Ref>{
                doc::makeNode(id++, NodeKind::TextRun, doc::RunPayload{cps, CharFormat{}, {}}, {}),
                doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})};
        };
        std::vector<doc::Ref> first;
        for (int i = 0; i < 3; ++i) {
            const std::vector<doc::Ref> p =
                para("Section 1 paragraph " + std::to_string(i + 1) + ".");
            first.insert(first.end(), p.begin(), p.end());
        }
        SectionSpec second_spec;
        second_spec.break_kind = SectionBreak::NextPage;
        const std::vector<doc::Ref> second = para("Section 2 starts on a new page.");
        const doc::Ref s1 = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(first));
        const doc::Ref s2 = doc::makeNode(id++, NodeKind::Section, second_spec, second);
        const NodeId root_id = id++;
        e.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {s1, s2}), id));
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

    if (runSection(23))
        std::printf("\n[23] 字符格式 run：块级字段升为 run 之后，旧路径等价、按 run 取值\n");
    if (runSection(23)) {
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
            b.line_spacing.factor = 1.75f;
            b.language = lang;
            b.color = Color::rgb(24, 24, 24);
            b.setText(text);  // 注意：setText 会清 runs，所以 runs 必须在它之后设
            return b;
        };
        auto setup = [&](EditorEngine& eng, const BlockSpec& b) {
            eng.init(fonts);
            eng.setConfig(cfg);
            loadBlocks(eng, {b});
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
                       "两种建树径各自都过增量与全量一致的审计");
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
            // 整形段数直调排版层看：树装载会把同属性的相邻 run 合并成一个文本节点，
            //   于是"一个 run 切成 3 个"在树上表达不出来 —— 而这条判据问的正是
            //   "排版按 runs 切了几刀"，本来就该问排版层（它只吃块流）。
            const std::vector<std::string> fonts23 = {cjkPath, mnPath, latinPath};
            const int c1 = layoutBlocksOnly(fonts23, {one}).para_index[0].font_run_count;
            const int c3 = layoutBlocksOnly(fonts23, {three}).para_index[0].font_run_count;
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
            loadBlocks(e, {b});  // 只换数据，不排版；整形缓存不清
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
            loadBlocks(e, {b});
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
            // 同 (c)：整形段数的判据走排版层，不走树。
            const std::vector<std::string> fonts23g = {cjkPath, mnPath, latinPath};
            const int base = layoutBlocksOnly(fonts23g, {whole}).para_index[0].font_run_count;
            const int after = layoutBlocksOnly(fonts23g, {cut}).para_index[0].font_run_count;
            expectTrue(base >= 1 && after == base + 1,
                       "★ 两把尺子求交：格式 run 切一刀，整形段按它增加（" + std::to_string(base) +
                           " → " + std::to_string(after) + "）");
            expectTrue(ew.layout().stats.glyph_count == ec.layout().stats.glyph_count,
                       "求交只改切分，不改字形数（" +
                           std::to_string(ew.layout().stats.glyph_count) + " vs " +
                           std::to_string(ec.layout().stats.glyph_count) + "）");
        }

        // (h) 接缝：模型编译出来的 runs 直接喂引擎，混合字号真的按各自的 run 排版。
        {
            auto asc = [](const std::string& s) {
                std::vector<uint32_t> out;
                out.reserve(s.size());
                for (char ch : s)
                    out.push_back(static_cast<uint32_t>(static_cast<unsigned char>(ch)));
                return out;
            };
            // 三段都用同一个字母：量推进时必须比同一个字形 —— 不同字母的宽度本来就不同，
            // 拿 a 与 c 比会得出一个与字号无关的比值。
            CharFormat small;
            small.size = Length::abs(ptToEmu(12.0));
            CharFormat large;
            large.size = Length::abs(ptToEmu(24.0));
            const std::vector<doc::Ref> kids{
                doc::makeNode(3, NodeKind::TextRun, doc::RunPayload{asc("a"), small, {}}, {}),
                doc::makeNode(4, NodeKind::TextRun, doc::RunPayload{asc("a"), large, {}}, {}),
                doc::makeNode(5, NodeKind::TextRun, doc::RunPayload{asc("a"), small, {}}, {}),
                doc::makeNode(6, NodeKind::ParagraphMark, doc::MarkPayload{}, {})};
            const doc::Ref section = doc::makeNode(2, NodeKind::Section, SectionSpec{}, kids);
            const doc::Doc doc = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section}), 7);

            const StyleSheet sheet;
            const doc::Projection pr = doc::project(doc, sheet);
            const bool shape = pr.ok() && pr.blocks.size() == 1;
            const std::size_t nr = shape ? pr.blocks[0].runs.size() : 0;
            // 三段文本、格式依次是 12 / 24 / 12 磅 —— 三段各一个 run。
            //   第三段虽然与第一段格式相同，也不并回第一段：合并只并"相邻且格式相同"的，
            //   中间隔着 24 磅那一段就并不过去。这一条同时证明合并没有按格式去全局归并。
            expectTrue(shape && nr == 3,
                       "接缝：投影出 1 段 3 个 run（实际 " + std::to_string(nr) + " 个）");
            expectTrue(shape && pr.blocks[0].runs[0].size_pt == 12.0f &&
                           pr.blocks[0].runs[1].size_pt == 24.0f &&
                           pr.blocks[0].runs[2].size_pt == 12.0f,
                       "接缝：三段 run 的字号依次是 12 / 24 / 12 磅");

            EditorEngine e;
            setup(e, pr.blocks[0]);
            const std::vector<PlacedGlyph>& gs = e.layout().glyphs;
            expectTrue(gs.size() == 3,
                       "接缝：三个字母三个字形（实际 " + std::to_string(gs.size()) + " 个）");
            // 同一个字形在两个 run 里的推进：24 磅那个必须是 12 磅的两倍。
            //   这条证伪"排版忽略了 run 的字号、整段按块级字号排"。
            const float adv12 = gs[1].x_pt - gs[0].x_pt;
            const float adv24 = gs[2].x_pt - gs[1].x_pt;
            expectTrue(std::fabs(adv24 - 2.0f * adv12) < 0.05f,
                       "★ 接缝：24 磅那个 run 的字形推进是 12 磅的两倍（" + std::to_string(adv24) +
                           " ≈ 2 × " + std::to_string(adv12) + "）");

            // 行度量按行取最大：这一行里有 24 磅的 run，行盒字号与上下界都按 24 磅算。
            //   原先这里断言的是"行盒仍是块级字段的 12 磅"，它在做行内混排字号时
            //   如期失败、于是换成了这一条 —— 这正是当初把它写成断言的目的。
            const LineBox& lb = e.layout().lines[0];
            expectTrue(lb.size_pt == 24.0f, "接缝：行盒字号取本行最大的 run（24 磅，实际 " +
                                                std::to_string(lb.size_pt) + "）");
            const float box_h = lb.block_hi_pt - lb.block_lo_pt;
            const float block_line_h = 12.0f * 1.75f;
            expectTrue(box_h > block_line_h, "接缝：行盒上下界按 24 磅的 ascent + descent（" +
                                                 std::to_string(box_h) + " > 块级行高 " +
                                                 std::to_string(block_line_h) + "）");
            expectTrue(e.auditLayout() == 0, "接缝：混合 run 的增量排版与全量逐字段一致");
        }

        // (i) 行度量是按行取最大，不是按段取最大：窄页上把 24 磅那段挤到第二行，
        //   两行的行盒必须各按各的来。
        {
            EngineConfig narrow;
            narrow.zoom = 1.0f;
            narrow.page.width_pt = 100.0f;
            narrow.page.height_pt = 400.0f;
            narrow.page.margins = PageMargins::all(5.0f);  // 内容宽 90 磅：12 磅字每行七个

            BlockSpec blk = mkBlock(0, 12.0f, "zh", "两把尺子求交同一段文本会被切两次");
            const uint32_t n = static_cast<uint32_t>(blk.cp_text.size());
            FormatRun r0 = blk.wholeRun();
            r0.cp_end = 7;  // 前七个字 12 磅
            FormatRun r1 = blk.wholeRun();
            r1.cp_begin = 7;
            r1.cp_end = n;
            r1.size_pt = 24.0f;  // 后面全 24 磅
            blk.runs = {r0, r1};

            EditorEngine e;
            e.init(fonts);
            e.setConfig(narrow);
            loadBlocks(e, {blk});
            e.relayout();
            const std::vector<LineBox>& ls = e.layout().lines;
            const bool two = ls.size() >= 2;
            expectTrue(two, "接缝 / 按行：窄页把 24 磅那段挤到了第二行（实际 " +
                                std::to_string(ls.size()) + " 行）");
            expectTrue(two && ls[0].size_pt == 12.0f && ls[1].size_pt == 24.0f,
                       "接缝 / 按行：第一行按 12 磅、第二行按 24 磅（各取本行最大）");
            expectTrue(two && (ls[1].block_hi_pt - ls[1].block_lo_pt) >
                                  (ls[0].block_hi_pt - ls[0].block_lo_pt),
                       "接缝 / 按行：第二行的行盒比第一行高（大字那一行真的撑开了）");
        }

        // (j) 增量重排也必须按行的字号算页高：编辑前一段让它变高之后，后一段（含混合字号）
        //   的分页必须与整篇重排一致。引擎侧那条增量路径若还按块级字号推进，这里就会不一致。
        //   场景要卡准：前一段必须真的多出一行（否则一级复用直接短路，后缀压根没被重算），
        //   后一段必须跨页（否则页号都一样，错了也看不出来）。
        {
            EngineConfig narrow;
            narrow.zoom = 1.0f;
            narrow.page.width_pt = 110.0f;   // 内容宽 100 磅：12 磅字每行八个
            narrow.page.height_pt = 100.0f;  // 内容高 90 磅
            narrow.page.margins = PageMargins::all(5.0f);

            BlockSpec head = mkBlock(0, 12.0f, "zh", "一二三四五六七八");
            BlockSpec body = mkBlock(0, 12.0f, "zh", "两把尺子求交同一段文本会被切两次");
            const uint32_t n = static_cast<uint32_t>(body.cp_text.size());
            FormatRun b0 = body.wholeRun();
            b0.cp_end = 8;  // 前八个 12 磅
            FormatRun b1 = body.wholeRun();
            b1.cp_begin = 8;
            b1.cp_end = n;
            b1.size_pt = 24.0f;  // 后面全 24 磅
            body.runs = {b0, b1};

            EditorEngine e;
            e.init(fonts);
            e.setConfig(narrow);
            loadBlocks(e, {head, body});
            e.relayout();
            const int pages_before = e.stats().page_count;
            const int lines_before = e.layout().stats.placed_line_count;
            expectTrue(pages_before >= 2 && lines_before >= 4,
                       "接缝 / 增量分页：混合字号的段真的跨了页（" + std::to_string(pages_before) +
                           " 页，" + std::to_string(lines_before) + " 行）");

            // 在前一段末尾敲一个字：那一段从一行变两行，后缀的行结构不变但整体要挪位 ——
            //   走的正是引擎的二级复用路径（重算页号 + 平移块轴）。
            e.setCaret(0, 8);
            expectTrue(e.typeTextAndRender("\xE6\xB5\x8B"), "接缝 / 增量分页：在前一段敲一个字");
            const int lines_after = e.layout().stats.placed_line_count;
            expectTrue(lines_after == lines_before + 1, "接缝 / 增量分页：前一段多出一行（" +
                                                            std::to_string(lines_before) + " → " +
                                                            std::to_string(lines_after) + "）");
            const int drift = e.auditLayout();
            expectTrue(drift == 0, "★ 接缝 / 增量分页：编辑之后增量与整篇重排逐字段一致（" +
                                       e.auditLayoutJson() + "）");
        }
    }

    if (runSection(24)) std::printf("\n[24] 行内对象：块流自足，排版层不再收外部括号名单\n");
    if (runSection(24)) {
        // 这一段验的是引擎输入的一次形状替换：控件边界从"排版接口的一个参数"搬进块流
        //   本身（BlockSpec::objects）。它是元素流在引擎输入侧的落点，也是"括号进文档流"
        //   那句话的结构化形态。三层判据缺一不可：
        //   不变量：对象表按 cp 升序、锚点不越界 —— 排版按数组顺序线性归并，顺序一坏，
        //     括号的层级就画错，而画面看上去仍然"有括号"，不容易发现；
        //   契约：排版层只吃块流 —— 直接调 layoutDocument（完全不经引擎）也能插出占位簇，
        //     且 side / owner / read_only 逐项到位；把表倒过来占位就跟着倒；
        //   等价与反向：表为空 == 与"没有表单"同一份几何；表里加一项就必须让正文真的
        //     右移，否则一个压根没读对象表的实现也能过前一条。
        const std::vector<std::string> fonts24 = {cjkPath, mnPath, latinPath};
        auto mkInline = [](int font_index, float size_pt, const std::string& text) {
            BlockSpec b;
            b.font_index = font_index;
            b.size_pt = size_pt;
            b.wm = WritingMode::HorizontalTB();
            b.line_spacing.factor = 1.75f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText(text);
            return b;
        };
        auto mkCtl = [](int a, int b, const char* id, bool ro) {
            Control c;
            c.range.start = TextPos{0, static_cast<size_t>(a)};
            c.range.end = TextPos{0, static_cast<size_t>(b)};
            c.id = id;
            c.read_only = ro;
            return c;
        };

        // (a) 不变量 objectsOrdered：合序通过；逆序与越界各报各的
        {
            BlockSpec ok = mkInline(0, 12.0f, "住院号甲乙丙丁");
            const uint32_t n = static_cast<uint32_t>(ok.cp_text.size());
            expectTrue(n == 7 && objectsOrdered(ok, nullptr), "不变量：对象表为空恒真（等价情形）");

            ok.objects = {InlineObject{0, InlineObjectKind::ControlStart, 0, false},
                          InlineObject{2, InlineObjectKind::ControlEnd, 0, false},
                          InlineObject{2, InlineObjectKind::ControlStart, 1, true},
                          InlineObject{n, InlineObjectKind::ControlEnd, 1, true}};
            expectTrue(objectsOrdered(ok, nullptr),
                       "不变量：合序通过（同一个 cp 上挂两项也是合法形态）");

            BlockSpec reversed = ok;
            std::reverse(reversed.objects.begin(), reversed.objects.end());
            std::string why;
            const bool rev_ok = objectsOrdered(reversed, &why);
            expectTrue(!rev_ok && why.find("更靠前") != std::string::npos,
                       "不变量：逆序被判出来（" + why + "）");

            BlockSpec beyond = mkInline(0, 12.0f, "住院号甲乙丙丁");
            beyond.objects = {InlineObject{n + 1, InlineObjectKind::ControlStart, 0, false}};
            why.clear();
            const bool beyond_ok = objectsOrdered(beyond, &why);
            expectTrue(!beyond_ok && why.find("超出段落长度") != std::string::npos,
                       "不变量：锚点越过段长被判出来（" + why + "）");
        }

        // (b) 产生端：引擎按"要显示哪些括号"落成对象表，同一处的先后在这里定死
        {
            EngineConfig ec;
            ec.zoom = 1.0f;

            EditorEngine e;
            if (!e.init(fonts24)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e.setConfig(ec);
            // 患者主诉：| 反复 | 心慌 | 三天 —— 三连字段，随后把中间那个删空。
            loadBlocks(e, {mkInline(0, 12.0f, "患者主诉：反复心慌三天")});
            e.setEditMode(EditMode::Form);
            // 左边那个字段只读：它的两枚括号画灰色，这一位也要由产生端记进表。
            // 只读的那个不能是被删空的：只读字段里的退格会被策略拒掉，那样这里
            // 验的就成了"删不动"，而不是对象表的顺序。
            loadControls(
                e, {mkCtl(5, 7, "k1", true), mkCtl(7, 9, "k2", false), mkCtl(9, 11, "k3", false)});
            // 控件表不再是"扫对象表再配对"出来的，而是投影给的跨度；审计那一侧仍从产物
            //   重算一遍区间与顺序 —— 两边必须逐项相同，否则"第几个字段"这张表就换了。
            expectTrue(e.auditCompile() == 0, "控件审计：装载之后控件表与产物边界逐项相同");
            e.setCaret(0, 9);
            // 两次退格分开写：写进断言条件里的话，第一下失败就短路，看不出是第几下没成。
            const bool bs1 = e.backspaceAndRender();
            const bool bs2 = e.backspaceAndRender();
            expectTrue(bs1 && bs2, "产生端：把中间那个字段删成零宽（边界上于是有四项）");
            expectTrue(e.auditCompile() == 0,
                       "★ 控件审计：删成零宽之后区间与顺序仍逐项相同（三连字段不能换位）");

            // 表的读法：cp、开闭、owner、只读各一位，一眼能看出顺序对不对。
            // owner 那一列是对外下标：产物里存的是控件节点 id，比之前翻一次。
            auto census = [&](const std::vector<InlineObject>& v) {
                std::string s;
                for (const InlineObject& o : v) {
                    if (!s.empty()) s += ' ';
                    s += std::to_string(o.cp);
                    s += (o.kind == InlineObjectKind::ControlStart) ? "<" : ">";
                    s += std::to_string(e.controlIndexOfOwner(o.owner));
                    s += o.read_only ? "R" : "-";
                }
                return s;
            };
            const std::string got = census(e.blocks()[0].objects);
            std::printf("     边界上的对象表：%s\n", got.c_str());
            // 偏移 5：左字段的开；偏移 7：左字段的闭、空字段的一对、右字段的开；
            //   偏移 9：右字段的闭。空字段的一对必须挨着，且夹在两个非空字段之间。
            const std::string want = "5<0R 7>0R 7<1- 7>1- 7<2- 9>2-";
            expectTrue(got == want,
                       "★ 产生端定序：表是 [5<0R 7>0R 7<1- 7>1- 7<2- 9>2-]（实际 " + got + "）");
            expectTrue(objectsOrdered(e.blocks()[0], nullptr), "产生端产出的表满足不变量");

            // "换档只放聚焦字段"那一段随 Focused 档一起删了（2026-10-20）：现在只有
            //   "全部或没有"两档，没有"表里少几项"这种中间态。下面验"没有"那一档。

            // 只读模式：括号根本不画，表就该是空的（不画却还占宽度是最坏的形态）。
            e.setEditMode(EditMode::ReadOnly);
            e.relayout();
            expectTrue(e.blocks()[0].objects.empty(), "只读模式：对象表为空");
            expectTrue(e.auditLayout() == 0, "只读模式重排后增量仍与全量一致");

            // 文档模式（设计态）反过来：边界照常入表 —— 看不见字段就无从增删改。
            e.setEditMode(EditMode::Document);
            e.relayout();
            expectTrue(!e.blocks()[0].objects.empty(),
                       "文档模式：字段边界照常入表（设计态要看得见，实际 " +
                           census(e.blocks()[0].objects) + "）");
            expectTrue(e.auditLayout() == 0, "文档模式重排后增量仍与全量一致");
        }

        // (c) 契约：排版层直接吃块流（不经引擎）就能插出占位簇
        {
            FontLibrary lib;
            expectTrue(lib.addFace(cjkPath) == 0, "排版层旁路引擎：字体加载成功");
            const PageMetrics page = PageMetrics::A4();

            // 四项都在段内（锚点 1 与 3 都小于段长）—— 两项都排在末字之前，
            // 于是"末字右移多少"可以与"行盒宽了多少"直接对照。
            const BlockSpec plain = mkInline(0, 12.0f, "甲乙丙丁");
            const LayoutResult r0 = layoutDocument(lib, {plain}, page);

            BlockSpec marked = mkInline(0, 12.0f, "甲乙丙丁");
            marked.objects = {InlineObject{1, InlineObjectKind::ControlStart, 7, false},
                              InlineObject{3, InlineObjectKind::ControlEnd, 8, true}};
            const LayoutResult r1 = layoutDocument(lib, {marked}, page);

            expectTrue(r0.lines.size() == 1 && r0.lines[0].clusters.size() == 4,
                       "无对象：簇数 == 码点数（一个占位都不插）");
            const size_t cl1 = r1.lines.empty() ? 0 : r1.lines[0].clusters.size();
            expectTrue(r1.lines.size() == 1 && cl1 == 6,
                       "两项对象 → 多两个簇（实际 " + std::to_string(cl1) + " 个）");

            std::vector<int> sides, owners, ros;
            for (const LineCluster& c : r1.lines[0].clusters) {
                if (!c.placeholder) continue;
                sides.push_back(static_cast<int>(c.bracket_side));
                owners.push_back(c.owner_control);
                ros.push_back(c.read_only ? 1 : 0);
            }
            expectTrue(sides.size() == 2 && sides[0] == static_cast<int>(BracketSide::Open) &&
                           sides[1] == static_cast<int>(BracketSide::Close),
                       "占位簇的开闭跟着对象表的 kind 走（先开后备）");
            expectTrue(owners.size() == 2 && owners[0] == 7 && owners[1] == 8,
                       "owner 原样回填（命中测试靠它回答点到的括号是谁的）");
            expectTrue(ros.size() == 2 && ros[0] == 0 && ros[1] == 1,
                       "read_only 原样回填（只读字段的括号画灰色）");

            // 几何：两项都在末字之前，所以末字必须正好右移两槽的宽度。
            auto glyphX = [](const LayoutResult& r, uint32_t cp) {
                for (const PlacedGlyph& g : r.glyphs) {
                    if (g.cluster == cp) return g.x_pt;
                }
                return -1.0f;
            };
            const float pushed = r1.lines[0].inline_end_pt - r0.lines[0].inline_end_pt;
            const float moved = glyphX(r1, 3) - glyphX(r0, 3);
            expectTrue(pushed > 0.5f && std::fabs(moved - pushed) < 0.02f,
                       "★ 占位真的把正文挤开了：末字右移量 == 行盒多出来的宽度（" +
                           std::to_string(moved) + " ≈ " + std::to_string(pushed) + "）");

            // 反向：把表倒过来 → 占位跟着倒。这一条同时是"排版按数组顺序线性归并、
            //   不做第二次排序"的判据：若排版自己会排序，这里两边必然一样。
            BlockSpec flipped = marked;
            std::reverse(flipped.objects.begin(), flipped.objects.end());
            expectTrue(!objectsOrdered(flipped, nullptr), "倒过来的表不满足不变量");
            const LayoutResult rf = layoutDocument(lib, {flipped}, page);
            std::vector<int> fsides;
            for (const LineCluster& c : rf.lines[0].clusters) {
                if (c.placeholder) fsides.push_back(static_cast<int>(c.bracket_side));
            }
            expectTrue(fsides.size() == 2 && fsides[0] == static_cast<int>(BracketSide::Close) &&
                           fsides[1] == static_cast<int>(BracketSide::Open),
                       "★ 顺序由表定：表倒过来，占位簇也跟着倒（排版不做第二次排序）");
        }

        // (d) setText 清对象表：锚点按旧文本的码点下标算，留着必然摆到别处
        {
            BlockSpec b = mkInline(0, 12.0f, "甲乙丙");
            b.objects = {InlineObject{1, InlineObjectKind::ControlStart, 0, false}};
            b.setText("甲");
            expectTrue(b.objects.empty(), "setText 清掉对象表（旧锚点留着会把括号摆到别处）");
            expectTrue(b.runs.empty(), "setText 同时清掉 run 表");
        }
    }

    if (runSection(31))
        std::printf("\n[31] 在控件内容里回车（控件因此跨段）之后编辑：不许把整段与控件一起弄丢\n");
    if (runSection(31)) {
        // 现象：三连可编辑字段里把光标放到「天」后面回车，再把光标放到「悸」后面
        //   退格，结果「患者主诉：反复心悸」九个字与三个控件整个消失，只剩一行「三天」。
        // 根因：回车把段落标记插进了控件内部（跨段控件是正常形态，见 compile.h 的 BlockSource），
        //   而"只重编译被改的那一段"是按标记的父容器取子节点区间的 —— 那一段的内容其实起自
        //   控件之外，于是控件里的半截被当成整段编译：前半段的字与控件自己的边界一起丢掉，
        //   block_src_ / block_flow_ 又被这半个产物覆盖，控件集合随之空掉。删除本身没错，
        //   错的是增量重编译的形状判据（rebuildParagraphFromTree）。
        // 判据两层：产物必须与整树全量编译逐字段一致（自己会报"哪一类形状没覆盖到"，上面那
        //   一条错在 auditCompile 上就是 4 项差异），以及文本与三条控件区间逐个对上 ——
        //   钉住界面上那一行文本。
        std::vector<BlockSpec> bl(1);
        bl[0].setText("患者主诉：反复心悸三天，加重一天。");
        std::vector<DemoFieldSpec> fd;
        auto add = [&](int so, int eo, const char* key) {
            DemoFieldSpec f;
            f.para = 0;
            f.so = so;
            f.eo = eo;
            f.key = key;
            fd.push_back(std::move(f));
        };
        add(5, 7, "a");
        add(7, 9, "b");
        add(9, 11, "c");
        EditorEngine e;
        expectTrue(e.init({cjkPath}), "控件跨段：字体加载成功");
        EngineConfig ec;
        ec.zoom = 1.0f;
        e.setConfig(ec);
        e.setDocument(demoDoc(bl, fd), StyleSheet{});
        e.setEditMode(EditMode::Form);
        e.relayout();
        auto textOf = [&]() {
            std::string t;
            for (const BlockSpec& b : e.blocks()) t += "[" + b.textUtf8() + "]";
            return t;
        };
        auto rangesOf = [&]() {
            std::string s;
            for (int i = 0; i < e.controls().size(); ++i) {
                const Control& c = e.controls().at(i);
                if (i) s += " ";
                s += "[" + std::to_string(c.range.start.offset) + "," +
                     std::to_string(c.range.end.para == c.range.start.para
                                        ? c.range.end.offset
                                        : 100 + c.range.end.offset) +
                     ")";  // 跨段时把段号折进数字里（100+ 表示落在第 1 段）
            }
            return s;
        };

        // 「天」之后 = 第三个字段的末尾：侧别 0 表示落在闭括号之内（末尾算在区间内）。
        e.setCaretAtSide(0, 11, 0);
        expectTrue(e.splitParagraph(), "在可编辑字段末尾回车：分段成功");
        expectTrue(e.auditCompile() == 0, "回车之后（控件因此跨段）：产物与整树全量编译一致");
        expectTrue(e.controls().size() == 3 && e.controls().at(2).range.start.para == 0 &&
                       e.controls().at(2).range.end.para == 1,
                   "第三个字段跨到第二段（段落标记插在它的内容里）");

        e.setCaretAtSide(0, 9, 0);
        expectTrue(e.backspace(), "再把光标放到「悸」后面退格");
        expectTrue(e.auditCompile() == 0,
                   "★ 退格之后：产物仍与整树全量编译逐字段一致（不许只剩控件里的半段）");
        std::printf("     退格后 段=%s 控件=%d 个 区间=%s\n", textOf().c_str(), e.controls().size(),
                    rangesOf().c_str());
        expectTrue(textOf() == "[患者主诉：反复心三天][，加重一天。]",
                   "★ 只少了「悸」一个字 —— 实际 " + textOf());
        expectTrue(e.controls().size() == 3, "★ 三个控件都还在（曾经被整批抹掉）");
        expectTrue(rangesOf() == "[5,7) [7,8) [8,100)",
                   "★ 三条区间跟着内容缩：只有 #1 短了一格 —— 实际 " + rangesOf());
        e.relayout();
        expectTrue(e.auditLayout() == 0, "★ 排版也是对的：与全量重排逐字段一致");

        // 再退一格：把 #1 删空。零宽字段与跨段字段同时在场，是这段坐标最容易错的一格。
        expectTrue(e.backspace(), "再退一格删「心」（把第二个字段删空）");
        expectTrue(e.auditCompile() == 0,
                   "★ 字段删空之后：产物仍与整树全量编译逐字段一致 —— 实际 " +
                       std::to_string(e.auditCompile()));
        std::printf("     再退一格 段=%s 区间=%s\n", textOf().c_str(), rangesOf().c_str());
        expectTrue(textOf() == "[患者主诉：反复三天][，加重一天。]",
                   "★ 段里只少了「心」—— 实际 " + textOf());
        expectTrue(rangesOf() == "[5,7) [7,7) [7,100)",
                   "★ #1 变成零宽、收在 7 上，#2 跟着左移 —— 实际 " + rangesOf());

        // 光标所在字段仍然认得对：这一段被控件从中间切开，#2 在本段的归属不能丢。
        expectTrue(e.focusedControlIndex() == 1, "光标（在被删空的那个字段里）仍归 #1 —— 实际 " +
                                                     std::to_string(e.focusedControlIndex()));
    }

    if (runSection(32))
        std::printf("\n[32] 两枚段落标记都落进控件内部之后，段内编辑不许丢段首的边界\n");
    if (runSection(32)) {
        // 现象：三连可编辑字段里「天」后回车、再「悸」后回车，然后把光标放到「天」后面
        //   退格 —— 第二段段首的两枚控件边界成对消失（前一个字段的闭括号、后一个字段的
        //   开括号都没了），控件集合里跟着少两个。
        // 形态：第二次回车把段落标记插进了第二个字段内部，于是第二段从"前一个字段的
        //   闭括号"起，而那枚边界属于容器之外的兄弟。增量编译按容器内区间取子节点，
        //   看不见段首还有一枚外面的边界，产物因此缺了它。
        // 判据两层：退格之后产物与整树全量编译逐字段一致，以及文本与三条区间逐个对上。
        std::vector<BlockSpec> bl(1);
        bl[0].setText("患者主诉：反复心悸三天，加重一天。");
        std::vector<DemoFieldSpec> fd;
        auto add = [&](int so, int eo, const char* key) {
            DemoFieldSpec f;
            f.para = 0;
            f.so = so;
            f.eo = eo;
            f.key = key;
            fd.push_back(std::move(f));
        };
        add(5, 7, "a");
        add(7, 9, "b");
        add(9, 11, "c");
        EditorEngine e2;
        expectTrue(e2.init({cjkPath}), "两次分段：字体加载成功");
        EngineConfig ec2;
        ec2.zoom = 1.0f;
        e2.setConfig(ec2);
        auto textOf2 = [&]() {
            std::string t;
            for (const BlockSpec& b : e2.blocks()) t += "[" + b.textUtf8() + "]";
            return t;
        };
        auto rangesOf2 = [&]() {
            std::string s;
            for (int i = 0; i < e2.controls().size(); ++i) {
                const Control& c = e2.controls().at(i);
                if (i) s += " ";
                s += "(" + std::to_string(c.range.start.para) + ":" +
                     std::to_string(c.range.start.offset) + ".." +
                     std::to_string(c.range.end.para) + ":" + std::to_string(c.range.end.offset) +
                     ")";
            }
            return s;
        };
        // 两次回车的落点都必须"落在闭括号之内"（侧别 0），否则回车本身就被策略据掉；
        //   退格那一步的侧别两种都试 —— 它是"段首有没有那枚边界"的直接触发点。
        for (int s3 = 0; s3 <= 1; ++s3) {
            e2.setDocument(demoDoc(bl, fd), StyleSheet{});
            e2.setEditMode(EditMode::Form);
            e2.relayout();
            e2.setCaretAtSide(0, 11, 0);
            expectTrue(e2.splitParagraph(), "「天」后回车");
            e2.setCaretAtSide(0, 9, 0);
            expectTrue(e2.splitParagraph(), "「悸」后回车（标记落进第二个字段内部）");
            expectTrue(e2.auditCompile() == 0, "两次回车之后（走全量）：产物与整树全量编译一致");
            e2.setCaretAtSide(1, 2, s3);
            expectTrue(e2.backspace(), "「天」后按退格");
            expectTrue(e2.auditCompile() == 0,
                       "★ 退格之后：产物仍与整树全量编译逐字段一致（不许丢段首那枚边界）"
                       " —— 实际 " +
                           std::to_string(e2.auditCompile()));
            std::printf("     退格后 侧=%d 段=%s 控件=%d 个 区间=%s\n", s3, textOf2().c_str(),
                        e2.controls().size(), rangesOf2().c_str());
            expectTrue(textOf2() == "[患者主诉：反复心悸][三][，加重一天。]",
                       "★ 只少了「天」一个字 —— 实际 " + textOf2());
            expectTrue(e2.controls().size() == 3, "★ 三个控件都还在（曾经整批消失）");
            expectTrue(rangesOf2() == "(0:5..0:7) (0:7..1:0) (1:0..2:0)",
                       "★ 三条区间：后两个字段各自跨到下一段段首 —— 实际 " + rangesOf2());
            e2.relayout();
            expectTrue(e2.auditLayout() == 0, "★ 排版与全量重排逐字段一致");
        }
    }

    if (runSection(33))
        std::printf("\n[33] 枚举：切点落进控件内部之后，段内编辑的产物必须与全量一致\n");
    if (runSection(33)) {
        // 同类问题都是同一句话：某一段的内容起点落在它的结束标记所属容器之外，而增量编译
        //   只编容器内那一截。靠人列举形态总会漏（闭括号形态就是这么漏掉的），所以这里把
        //   位置穷举一遍：先枚举回车落在每一个 (码点偏移, 侧别) 上，再枚举切完之后每一段
        //   每一位的一次退格 —— 段数不变，走增量这条最容易错的路径。
        std::vector<BlockSpec> bl(1);
        bl[0].setText("患者主诉：反复心悸三天，加重一天。");
        std::vector<DemoFieldSpec> fd;
        auto add = [&](int so, int eo, const char* key) {
            DemoFieldSpec f;
            f.para = 0;
            f.so = so;
            f.eo = eo;
            f.key = key;
            fd.push_back(std::move(f));
        };
        add(5, 7, "a");
        add(7, 9, "b");
        add(9, 11, "c");
        EditorEngine e3;
        expectTrue(e3.init({cjkPath}), "枚举：字体加载成功");
        EngineConfig ec3;
        ec3.zoom = 1.0f;
        e3.setConfig(ec3);
        auto reload = [&]() {
            e3.setDocument(demoDoc(bl, fd), StyleSheet{});
            e3.setEditMode(EditMode::Form);
            e3.relayout();
        };
        const int cp_len = static_cast<int>(bl[0].cp_text.size());

        int cuts = 0;
        int cut_bad = 0;
        for (int off = 0; off <= cp_len; ++off) {
            for (int side = 0; side <= 1; ++side) {
                reload();
                e3.setCaretAtSide(0, off, side);
                if (!e3.splitParagraph()) continue;
                ++cuts;
                const int d = e3.auditCompile();
                if (d != 0) {
                    ++cut_bad;
                    std::printf("     回车 off=%d side=%d 之后审计=%d\n", off, side, d);
                }
            }
        }
        expectTrue(cut_bad == 0, "★ 每个回车切点之后产物都与整树全量一致（" + std::to_string(cuts) +
                                     " 个切点，异常 " + std::to_string(cut_bad) + " 个）");

        int edits = 0;
        int edit_bad = 0;
        int layout_bad = 0;
        for (int off = 0; off <= cp_len; ++off) {
            for (int side = 0; side <= 1; ++side) {
                reload();
                e3.setCaretAtSide(0, off, side);
                if (!e3.splitParagraph()) continue;
                const int np = static_cast<int>(e3.blocks().size());
                for (int p = 0; p < np; ++p) {
                    const int len =
                        static_cast<int>(e3.blocks()[static_cast<size_t>(p)].cp_text.size());
                    for (int q = 0; q <= len; ++q) {
                        for (int s2 = 0; s2 <= 1; ++s2) {
                            reload();
                            e3.setCaretAtSide(0, off, side);
                            if (!e3.splitParagraph()) break;
                            e3.setCaretAtSide(p, q, s2);
                            if (!e3.backspace()) continue;
                            ++edits;
                            const int d = e3.auditCompile();
                            if (d != 0) {
                                ++edit_bad;
                                std::printf("     退格 切(%d,%d) 后在 (%d,%d) 侧=%d 审计=%d\n", off,
                                            side, p, q, s2, d);
                                continue;
                            }
                            // 段首是"内容起点最容易跑到容器外"的一枚，排版在这一格单独对一次。
                            if (q == 0) {
                                e3.relayout();
                                if (e3.auditLayout() != 0) {
                                    ++layout_bad;
                                    std::printf(
                                        "     退格 切(%d,%d) 后在 (%d,%d) 侧=%d 排版不一致\n", off,
                                        side, p, q, s2);
                                }
                            }
                        }
                    }
                }
            }
        }
        expectTrue(edit_bad == 0, "★ 每次退格之后产物都与整树全量一致（" + std::to_string(edits) +
                                      " 次编辑，异常 " + std::to_string(edit_bad) + " 次）");
        expectTrue(layout_bad == 0, "★ 段首退格之后的排版与全量重排一致（异常 " +
                                        std::to_string(layout_bad) + " 次）");
    }

    if (runSection(34))
        std::printf("\n[34] 删控件与改控件属性：引擎命令走树的提交，审计与撤销都对得上\n");
    if (runSection(34)) {
        // 用户提出的运维问题："编辑模式下不显示控件边界了，我都不知道哪块是控件，
        //   之后删除控件、编辑控件怎么操作？"
        // 这一节管后半句：引擎先给出命令（宿主 UI 稍后接）。两条命令都只动树，改完交给
        //   commitOnTree 一次重建 —— 与文本编辑同一条路，于是撤销、重排、重绘、记账自动
        //   一致；凡是另开一条特权通道的做法，这几件事里迟早有一件忘了做。
        std::vector<BlockSpec> bl(1);
        bl[0].setText("患者主诉：反复心悸三天，加重一天。");
        std::vector<DemoFieldSpec> fd;
        auto add = [&](int so, int eo, const char* key) {
            DemoFieldSpec f;
            f.para = 0;
            f.so = so;
            f.eo = eo;
            f.key = key;
            fd.push_back(std::move(f));
        };
        add(5, 9, "k1");    // 反复心悸
        add(12, 16, "k2");  // 加重一天
        const std::string kFull = "患者主诉：反复心悸三天，加重一天。";

        EditorEngine e4;
        expectTrue(e4.init({cjkPath}), "删控件：字体加载成功");
        EngineConfig ec4;
        ec4.zoom = 1.0f;
        e4.setConfig(ec4);
        auto reload4 = [&]() {
            e4.setDocument(demoDoc(bl, fd), StyleSheet{});
            e4.setEditMode(EditMode::Document);
            e4.relayout();
        };

        // (a) 文档模式：拆壳留内容 —— "删除控件"最常用的一档（字段不想要了，字还得在）
        reload4();
        {
            expectTrue(e4.controls().size() == 2 && e4.controls().at(0).id == "k1",
                       "装载：两个字段，第一个是 k1");
            const long long b0 = e4.blockedEdits();
            expectTrue(e4.removeControl(0, /*keep_content=*/true),
                       "文档模式允许删控件（设计期的事）");
            expectTrue(e4.controls().size() == 1 && e4.controls().at(0).id == "k2",
                       "删的正是下标 0 那个：剩下 k2（实际 " +
                           std::to_string(e4.controls().size()) + " 个）");
            expectTrue(e4.blocks()[0].textUtf8() == kFull,
                       "拆壳留内容：一个字都没丢 —— 实际 " + e4.blocks()[0].textUtf8());
            expectTrue(e4.controls().at(0).range.start.offset == 12 &&
                           e4.controls().at(0).range.end.offset == 16,
                       "k2 的区间仍是 12..16（拆壳不动别人的坐标）");
            expectTrue(e4.blockedEdits() == b0, "被允许的操作不记账");
            expectTrue(e4.auditCompile() == 0, "★ 拆壳之后产物仍与整树全量编译逐字段一致");
        }
        {
            // 撤销：一次删控件一条事务，撤销之后控件与文本都原样。
            expectTrue(e4.undo(), "撤销回退这次删控件");
            expectTrue(e4.controls().size() == 2 && e4.controls().at(0).id == "k1",
                       "撤销之后控件原样回来（身份还是 k1，不是重新建了一个）");
            expectTrue(e4.blocks()[0].textUtf8() == kFull, "撤销之后文本也回到原样");
            expectTrue(e4.auditCompile() == 0, "★ 撤销之后产物仍与整树全量编译一致");
            // 重做把拆壳再来一遍：这一步最能暴露"槽位下标活不过一次规范化"——
            //   撤销那一步把合并掉的文本切了回来，重做又要在同一个流位置上拆同一个节点。
            expectTrue(e4.redo(), "重做这次删控件");
            expectTrue(e4.controls().size() == 1 && e4.controls().at(0).id == "k2",
                       "重做之后又只剩 k2");
            expectTrue(e4.blocks()[0].textUtf8() == kFull, "重做之后文本仍然一字不差");
            expectTrue(e4.auditCompile() == 0, "★ 重做之后产物仍与整树全量编译一致");
            expectTrue(e4.undo(), "再撤销回来");
        }
        {
            // 拆壳要真拆掉 —— 先前字段里的位置从此是普通正文，编辑不再受那个控件约束。
            //   这一条是"拆壳"与"连内容删"之外唯一能证明壳真没了的行为判据。
            expectTrue(e4.removeControl(0, /*keep_content=*/true), "再删一次");
            expectTrue(e4.insertTextUtf8(0, 6, "X"),
                       "拆壳之后原来字段里的位置成了普通正文（不再受控件约束）");
            expectTrue(e4.blocks()[0].cp_text.size() == 18,
                       "那个位置真的插进了一个字（实际 " +
                           std::to_string(e4.blocks()[0].cp_text.size()) + " 个码点）");
        }

        // (b) 连内容一起删
        reload4();
        {
            expectTrue(e4.removeControl(0, /*keep_content=*/false), "连内容一起删");
            expectTrue(e4.blocks()[0].textUtf8() == "患者主诉：三天，加重一天。",
                       "字段里的四个字一起没了 —— 实际 " + e4.blocks()[0].textUtf8());
            expectTrue(e4.controls().size() == 1 && e4.controls().at(0).id == "k2", "只剩 k2");
            expectTrue(e4.controls().at(0).range.start.offset == 8,
                       "k2 的起点跟着前移了四位（12 → 8）—— 实际 " +
                           std::to_string(e4.controls().at(0).range.start.offset));
            expectTrue(e4.auditCompile() == 0, "★ 连内容删之后产物仍与整树全量编译逐字段一致");
        }

        // (c) 表单模式默认被 structureLock 挡住；作者显式开锁之后放行
        reload4();
        e4.setEditMode(EditMode::Form);
        e4.relayout();
        {
            expectTrue(e4.controls().size() == 2, "表单模式：两个字段都在");
            const long long b1 = e4.blockedEdits();
            expectTrue(
                !e4.removeControl(0, /*keep_content=*/true),
                "表单模式默认拒：structureLock 默认锁住（表单的默认是内容可编辑、控件不可删）");
            expectTrue(e4.controls().size() == 2, "被拒之后一个字段都没动");
            expectTrue(e4.blockedEdits() == b1 + 1,
                       "这次拒绝记了账（静默失败与「程序卡了」分不开）");
            // 改属性本身在表单模式是允许的（第七节：它是宿主回填与业务逻辑发起的运行时状态，
            //   不受落点判定限制）—— 不然没有任何一条路能把这把锁打开。
            expectTrue(e4.setControlProp(0, ControlProp::StructureLock, false),
                       "把 k1 的结构锁打开");
            const long long b2 = e4.blockedEdits();
            expectTrue(e4.removeControl(0, /*keep_content=*/true), "开锁之后表单模式也允许删");
            expectTrue(e4.controls().size() == 1 && e4.controls().at(0).id == "k2", "确实少了一个");
            expectTrue(e4.blockedEdits() == b2, "这一次没有被拒");
            expectTrue(e4.auditCompile() == 0, "★ 开锁后删除仍与整树全量编译一致");
        }

        // (d) 幂等：设成同一个值不提交（撤销栈里多一条什么都没做的记录只会让人困惑）。
        //   判据用"撤销两次"来数栈里有几条，不引入新的计数接口。
        reload4();
        {
            expectTrue(e4.setControlProp(0, ControlProp::Readonly, true), "把 k1 设成只读");
            expectTrue(e4.controls().at(0).read_only, "只读生效");
            expectTrue(e4.setControlProp(0, ControlProp::Readonly, true),
                       "再设一次同一个值：返回成功（幂等）");
            expectTrue(e4.undo(), "撤销");
            expectTrue(!e4.controls().at(0).read_only, "只读回退了");
            expectTrue(!e4.undo(), "★ 撤销栈里只有那一条真改动 —— 幂等的那一次没有入栈");
        }

        // (e) hidden 是四项属性里唯一"改呈现态流结构"的一项（整棵摘除），改它要重排
        reload4();
        {
            const size_t n_before = e4.blocks()[0].cp_text.size();
            expectTrue(e4.setControlProp(1, ControlProp::Hidden, true), "把 k2 隐藏");
            expectTrue(e4.blocks()[0].cp_text.size() == n_before,
                       "隐藏不改文本（摘的是呈现态，不是内容）");
            expectTrue(e4.controls().size() == 2,
                       "控件集合仍有两个 —— 隐藏的还在文档里，只是不呈现");
            e4.relayout();
            expectTrue(e4.auditLayout() == 0, "★ 隐藏之后排版与全量重排逐字段一致");
            expectTrue(e4.setControlProp(1, ControlProp::Hidden, false), "再显示回来");
            expectTrue(e4.blocks()[0].cp_text.size() == n_before, "恢复之后文本一字不差");
            expectTrue(e4.auditCompile() == 0, "★ 显示回来之后产物仍与全量一致");
        }

        // (f) 换坐标基之后光标不动。判据钉的是"块起点一换，光标要按段内位置重新落基"：
        //   光标存的是全局流位置，而这个下标只有配上块起点才读得出段与段内位置 —— 前面那一段
        //   少了几位，下标没变、含义却变了，于是光标会往前漂。漂移是静默的：它只让 caret()
        //   的两个字段各差一点，不相邻的段之间没有任何断言会顺带撞上。
        //   这一段走隐藏控件：改完结构却不摆落点的只有它（别的编辑入口改完都把落点摆到编辑处），
        //   而 hidden 只在呈现态摘除，所以要装在表单模式上。
        {
            std::vector<BlockSpec> b5(3);
            b5[0].setText("前一段带字段：甲乙丙");
            b5[1].setText("中间那段");
            b5[2].setText("后面那段，光标停在这里");
            std::vector<DemoFieldSpec> f5(1);
            f5[0].para = 0;
            f5[0].so = 7;
            f5[0].eo = 9;
            f5[0].key = "k1";

            EditorEngine e5;
            expectTrue(e5.init({cjkPath}), "落基：字体加载成功");
            EngineConfig ec5;
            ec5.zoom = 1.0f;
            e5.setConfig(ec5);
            e5.setDocument(demoDoc(b5, f5), StyleSheet{});
            e5.setEditMode(EditMode::Form);
            e5.relayout();

            e5.setCaret(2, 4);
            const TextPos before5 = e5.caret();
            expectTrue(before5.para == 2 && before5.offset == 4, "落基：光标先停在第三段第 4 个字");
            expectTrue(e5.setControlProp(0, ControlProp::Hidden, true), "把第一段那个字段隐藏");
            const TextPos after5 = e5.caret();
            expectTrue(after5 == before5,
                       "★ 前面那一段少了几位之后光标仍在 (2, 4)：换基要按段内位置重新落基（实际 (" +
                           std::to_string(after5.para) + ", " + std::to_string(after5.offset) +
                           ")）");
        }

        // (g) 隐藏字段之后的编辑落点：流位置在呈现态与设计态下不是同一个数。
        //   判据是"字打在了哪里"：字段隐藏之后可见文本是 AACC；光标停在第二个 C 之前打字，
        //   字必须落在那里。修复前这个整数被按设计态解释，落点退进隐藏字段 —— 字打进去了、
        //   屏幕上却什么都没变（呈现态不画隐藏字段的内容）。
        {
            std::vector<BlockSpec> b6(1);
            b6[0].setText("AABBCC");
            std::vector<DemoFieldSpec> f6(1);
            f6[0].para = 0;
            f6[0].so = 2;
            f6[0].eo = 4;  // 字段包住 "BB"
            f6[0].key = "k6";

            // 只含 ASCII，按码点直接拼成字符串就够，不必引 UTF-8 转换。
            auto text6 = [](const EditorEngine& e, int para) {
                std::string out;
                for (uint32_t c : e.blocks()[para].cp_text) out.push_back(static_cast<char>(c));
                return out;
            };

            EditorEngine e6;
            expectTrue(e6.init({cjkPath}), "可见性：字体加载成功");
            EngineConfig ec6;
            ec6.zoom = 1.0f;
            e6.setConfig(ec6);
            e6.setDocument(demoDoc(b6, f6), StyleSheet{});
            e6.setEditMode(EditMode::Form);
            e6.relayout();
            expectTrue(e6.setControlProp(0, ControlProp::Hidden, true),
                       "可见性：把 BB 那个字段隐藏");
            e6.relayout();
            expectTrue(text6(e6, 0) == "AACC",
                       "可见性：字段隐藏后可见文本是 AACC（实际 " + text6(e6, 0) + "）");
            // 打字要在可编辑区域里：另配一个包住 "CC" 的字段，光标落到它中间。
            //   （表单模式下没有可插入位置时本来就打不进字 —— 那是策略，不是缺陷。）
            std::vector<DemoFieldSpec> f6b = f6;
            DemoFieldSpec tail;
            tail.para = 0;
            tail.so = 4;
            tail.eo = 6;
            tail.key = "k6b";
            f6b.push_back(tail);
            e6.setDocument(demoDoc(b6, f6b), StyleSheet{});
            e6.setEditMode(EditMode::Form);
            e6.relayout();
            expectTrue(e6.setControlProp(0, ControlProp::Hidden, true), "可见性：再隐藏一次 BB");
            e6.relayout();
            e6.setCaret(0, 3);  // 两个 C 之间（BB 摘除之后整段只剩 AACC）
            expectTrue(e6.typeTextUtf8("Z"), "可见性：在隐藏字段之后打字成功");
            expectTrue(text6(e6, 0) == "AACZC",
                       "★ 字落在光标处，没有落进隐藏字段（实际 " + text6(e6, 0) + "）");
        }
    }

    if (runSection(35))
        std::printf("\n[35] 控件边界上的退格与前删：进控件删内容，空控件才删控件\n");
    if (runSection(35)) {
        // 四条规则（从左到右的书写方向，参照 DCWriterLite 的 Backspace / Delete）：
        //   退格 光标在闭括号右面：控件有内容 ⇒ 光标进控件，删掉左边那一项；空 ⇒ 删控件
        //   退格 光标在开括号右面：控件有内容 ⇒ 光标退到开括号左面，不删；空 ⇒ 删控件
        //   前删 光标在开括号左面：控件有内容 ⇒ 光标进控件，删掉右边那一项；空 ⇒ 删控件
        //   前删 光标在闭括号左面：控件有内容 ⇒ 光标跳到闭括号右面，删右边那一项；空 ⇒ 删控件
        // 四条都必须落在流位置上判：这些格子两两共用同一个码点偏移（"内容末尾"与"闭括号之后"
        //   在文本坐标里是同一个 (段, 偏移)），只有流位置分得开。
        // 判据同时钉住"控件还在"与"删掉的是哪一项"两半 —— 只钉文本的话，
        //   "整块内容一起删掉"也能让文本对上。
        std::vector<BlockSpec> bl(1);
        bl[0].setText("患者主诉：反复心悸三天，加重一天。");
        std::vector<DemoFieldSpec> fd;
        auto add = [&](int so, int eo, const char* key) {
            DemoFieldSpec f;
            f.para = 0;
            f.so = so;
            f.eo = eo;
            f.key = key;
            fd.push_back(std::move(f));
        };
        add(5, 9, "k1");    // 反复心悸
        add(12, 16, "k2");  // 加重一天

        EditorEngine e5;
        expectTrue(e5.init({cjkPath}), "控件边界：字体加载成功");
        EngineConfig ec5;
        ec5.zoom = 1.0f;
        e5.setConfig(ec5);
        auto reload5 = [&]() {
            e5.setDocument(demoDoc(bl, fd), StyleSheet{});
            e5.setEditMode(EditMode::Document);
            e5.relayout();
        };

        // (a) 对照：内容末尾（侧别 0）退格删的是字，控件一个都不少
        reload5();
        expectTrue(e5.controls().size() == 2, "装载：两个字段");
        e5.setCaretAtSide(0, 9, 0);
        expectTrue(e5.caretSide() == 0, "光标在字段 #0 的内容末尾（侧别 0）");
        expectTrue(e5.backspace(), "内容末尾退格");
        expectTrue(e5.controls().size() == 2, "★ 控件一个都没少：那一格还没跨过闭括号");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：反复心三天，加重一天。",
                   "★ 删的是「悸」这一个字 —— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");

        // (b) 规则 1：闭括号右面退格 ⇒ 进控件删掉内容里的最后一项，控件本身还在
        reload5();
        e5.setCaretAtSide(0, 9, 1);
        expectTrue(e5.caretSide() == 1, "光标在字段 #0 的闭括号之后（侧别 1）");
        expectTrue(e5.backspace(), "闭括号之后退格");
        expectTrue(e5.controls().size() == 2,
                   "★ 控件还在（里面有内容时，退格删的是内容里那一项，不是控件本身）—— 实际剩 " +
                       std::to_string(e5.controls().size()) + " 个");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：反复心三天，加重一天。",
                   "★ 删掉的仍是「悸」—— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.caret().offset == 8 && e5.caretSide() == 0,
                   "★ 光标进到了控件里（内容末尾 cp 8 侧别 0）—— 实际 " +
                       std::to_string(e5.caret().offset) + "/" + std::to_string(e5.caretSide()));
        expectTrue(e5.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");
        e5.relayout();
        expectTrue(e5.auditLayout() == 0, "★ 排版与全量重排逐字段一致");

        // (c) 规则 2：开括号右面退格 ⇒ 退到开括号左面，一个字都不删
        reload5();
        e5.setCaretAtSide(0, 5, 1);
        expectTrue(e5.caret().offset == 5 && e5.caretSide() == 1, "光标在字段 #0 的开括号之后");
        expectTrue(e5.backspace(), "开括号之后退格");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：反复心悸三天，加重一天。",
                   "★ 一个字都没删 —— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.caret().offset == 5 && e5.caretSide() == 0,
                   "★ 光标退到开括号左面（cp 5 侧别 0）—— 实际 " +
                       std::to_string(e5.caret().offset) + "/" + std::to_string(e5.caretSide()));
        expectTrue(e5.controls().size() == 2, "控件一个都没少");

        // (d) 空控件：内容删光之后再退一格，删掉的是控件本身（连它那两枚括号）
        reload5();
        e5.setCaretAtSide(0, 9, 1);
        for (int i = 0; i < 4; ++i) expectTrue(e5.backspace(), "退掉字段内容里的一个字");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：三天，加重一天。",
                   "★ 四个字都进了控件删掉 —— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.controls().size() == 2 && e5.controls().at(0).id == "k1",
                   "k1 还在（只是空了）");
        expectTrue(e5.backspace(), "★ 空控件上再退一格");
        expectTrue(e5.controls().size() == 1 && e5.controls().at(0).id == "k2",
                   "★ 删掉的是控件本身（实际剩 " + std::to_string(e5.controls().size()) + " 个）");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：三天，加重一天。",
                   "文本不变（控件里本来就空了）");
        expectTrue(e5.controls().at(0).range.start.offset == 8 &&
                       e5.controls().at(0).range.end.offset == 12,
                   "★ 剩下的 k2 区间跟着前移成 [8,12) —— 实际 [" +
                       std::to_string(e5.controls().at(0).range.start.offset) + "," +
                       std::to_string(e5.controls().at(0).range.end.offset) + ")");
        expectTrue(e5.caret().offset == 5 && e5.caretSide() == 0,
                   "★ 光标落在控件原来那一格（cp 5 侧别 0）—— 实际 " +
                       std::to_string(e5.caret().offset) + "/" + std::to_string(e5.caretSide()));
        expectTrue(e5.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");

        // (d2) 删掉的控件要撤得回来（删控件走的是 RemoveNode，撤销要把它挂回原处 ——
        //   而规范化会把相邻的两段文字并成一个，槽位下标活不过它，只能按流位置挂回）
        expectTrue(e5.undoAndRender(), "★ 撤销这一次删控件");
        expectTrue(e5.controls().size() == 2 && e5.controls().at(0).id == "k1",
                   "★ 控件回来了（实际 " + std::to_string(e5.controls().size()) + " 个）");
        expectTrue(e5.controls().at(0).range.start.offset == 5 &&
                       e5.controls().at(0).range.end.offset == 5,
                   "★ 回到它原来那一格、且仍是空的（零宽区间）—— 实际 [" +
                       std::to_string(e5.controls().at(0).range.start.offset) + "," +
                       std::to_string(e5.controls().at(0).range.end.offset) + ")");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：三天，加重一天。", "文本没变");
        expectTrue(e5.auditCompile() == 0, "★ 撤销之后产物仍与整树全量编译逐字段一致");

        // (e) 相邻字段（][）：那一格既是"左边之后"又是"右边之前"，退格退的是左边那个
        {
            std::vector<BlockSpec> bl2(1);
            bl2[0].setText("甲乙丙丁");
            std::vector<DemoFieldSpec> fd2;
            auto add2 = [&](int so, int eo, const char* key) {
                DemoFieldSpec f;
                f.para = 0;
                f.so = so;
                f.eo = eo;
                f.key = key;
                fd2.push_back(std::move(f));
            };
            add2(0, 1, "a");  // 甲
            add2(1, 2, "b");  // 乙
            EditorEngine e6;
            expectTrue(e6.init({cjkPath}), "相邻字段：字体加载成功");
            EngineConfig ec6;
            ec6.zoom = 1.0f;
            e6.setConfig(ec6);
            e6.setDocument(demoDoc(bl2, fd2), StyleSheet{});
            e6.setEditMode(EditMode::Document);
            e6.relayout();
            expectTrue(e6.controls().size() == 2, "两个字段紧挨着（甲、乙各一个）");
            e6.setCaretAtSide(0, 1, 1);  // 跨过 ]a 之后 —— 那一格也是 [b 之前
            expectTrue(e6.caretSide() == 1, "光标落在 ]a 与 [b 之间那一格");
            expectTrue(e6.backspace(), "退格");
            expectTrue(e6.controls().size() == 2,
                       "★ 两个字段都还在：退格进的是左边那个 a，删的是它里面的字（实际剩 " +
                           std::to_string(e6.controls().size()) + " 个）");
            expectTrue(e6.blocks()[0].textUtf8() == "乙丙丁",
                       "★ 删掉的是「甲」—— 实际 " + e6.blocks()[0].textUtf8());
            expectTrue(e6.controls().at(0).id == "a" &&
                           e6.controls().at(0).range.start.offset == 0 &&
                           e6.controls().at(0).range.end.offset == 0,
                       "★ a 被删空成零宽区间（没被拆壳、也没被 b 吃掉）—— 实际 [" +
                           std::to_string(e6.controls().at(0).range.start.offset) + "," +
                           std::to_string(e6.controls().at(0).range.end.offset) + ")");
            expectTrue(e6.controls().at(1).range.start.offset == 0 &&
                           e6.controls().at(1).range.end.offset == 1,
                       "★ b 的区间跟着前移成 [0,1)（乙）");
            expectTrue(e6.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");
        }

        // (f) 嵌套：内层闭括号右面退格，进的是内层那一个（不是外层）
        {
            std::vector<BlockSpec> bl3(1);
            bl3[0].setText("甲乙丙丁");
            std::vector<DemoFieldSpec> fd3;
            auto add3 = [&](int so, int eo, const char* key) {
                DemoFieldSpec f;
                f.para = 0;
                f.so = so;
                f.eo = eo;
                f.key = key;
                fd3.push_back(std::move(f));
            };
            add3(1, 3, "outer");  // 乙丙
            add3(2, 3, "inner");  // 丙（严格落在 outer 内部 → 成为它的子控件）
            EditorEngine e7;
            expectTrue(e7.init({cjkPath}), "嵌套字段：字体加载成功");
            EngineConfig ec7;
            ec7.zoom = 1.0f;
            e7.setConfig(ec7);
            e7.setDocument(demoDoc(bl3, fd3), StyleSheet{});
            e7.setEditMode(EditMode::Document);
            e7.relayout();
            expectTrue(e7.controls().size() == 2, "两个字段（一个套在另一个里）");
            // 流： 甲 [outer 乙 [inner 丙 ]inner ]outer 丁
            //   ]inner 之后那一格（侧别 1 在 cp3 上）与 ]outer 之后那一格（侧别 2）是
            //   两个不同的流位置 —— 嵌套时内层的 hi 严格小于外层，退格才分得清退哪个。
            e7.setCaretAtSide(0, 3, 1);
            expectTrue(e7.caretSide() == 1, "光标在 ]inner 之后（还没跨过 ]outer）");
            expectTrue(e7.backspace(), "退格");
            expectTrue(e7.controls().size() == 2,
                       "★ 两个字段都还在：进的是内层，删的是它里面的「丙」（实际剩 " +
                           std::to_string(e7.controls().size()) + " 个）");
            expectTrue(e7.blocks()[0].textUtf8() == "甲乙丁",
                       "★ 删掉的是「丙」—— 实际 " + e7.blocks()[0].textUtf8());
            expectTrue(e7.controls().at(1).id == "inner" &&
                           e7.controls().at(1).range.start.offset == 2 &&
                           e7.controls().at(1).range.end.offset == 2,
                       "★ inner 被删空成零宽区间，outer 的区间不受影响 —— 实际 inner=[" +
                           std::to_string(e7.controls().at(1).range.start.offset) + "," +
                           std::to_string(e7.controls().at(1).range.end.offset) + ")");
            // 反向一半：再退一格，删掉的是那个空的内层控件本身
            expectTrue(e7.backspace(), "★ 空内层控件上再退一格");
            expectTrue(e7.controls().size() == 1 && e7.controls().at(0).id == "outer",
                       "★ 删掉的是空的内层（outer 还在，实际剩 " +
                           std::to_string(e7.controls().size()) + " 个）");
            expectTrue(e7.blocks()[0].textUtf8() == "甲乙丁", "文本一字不差");
            expectTrue(e7.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");
        }

        // (g) 前删（Delete）的四条规则，与退格左右对称
        //
        //   规则 4：开括号左面 —— 有内容时进控件删右边那一项
        reload5();
        e5.setCaretAtSide(0, 5, 0);
        expectTrue(e5.caretSide() == 0, "光标在字段 #0 的开括号之前（侧别 0）");
        expectTrue(e5.deleteForward(), "开括号之前按 Delete");
        expectTrue(e5.controls().size() == 2, "★ 控件还在（删的是内容里的第一项）");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：复心悸三天，加重一天。",
                   "★ 删掉的是「反」—— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.caret().offset == 5 && e5.caretSide() == 1,
                   "★ 光标进到控件里（开括号右面）—— 实际 " + std::to_string(e5.caret().offset) +
                       "/" + std::to_string(e5.caretSide()));
        expectTrue(e5.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");

        //   规则 3：闭括号左面 —— 有内容时跳到闭括号右面，删右边那一项
        reload5();
        e5.setCaretAtSide(0, 9, 0);
        expectTrue(e5.caretSide() == 0, "光标在字段 #0 的闭括号之前（＝内容末尾）");
        expectTrue(e5.deleteForward(), "闭括号之前按 Delete");
        expectTrue(e5.controls().size() == 2, "★ 控件还在");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：反复心悸天，加重一天。",
                   "★ 删掉的是控件外那一个「三」—— 实际 " + e5.blocks()[0].textUtf8());
        expectTrue(e5.caret().offset == 9 && e5.caretSide() == 1,
                   "★ 光标跳到了闭括号右面 —— 实际 " + std::to_string(e5.caret().offset) + "/" +
                       std::to_string(e5.caretSide()));

        //   规则 3 + 空控件：光标在空控件的闭括号左面（＝开括号右面），删掉控件本身
        reload5();
        e5.setCaretAtSide(0, 9, 1);
        for (int i = 0; i < 4; ++i) expectTrue(e5.backspace(), "先把这个字段删空");
        expectTrue(e5.controls().size() == 2, "k1 还在（空的）");
        expectTrue(e5.deleteForward(), "★ 空控件上按 Delete");
        expectTrue(e5.controls().size() == 1 && e5.controls().at(0).id == "k2",
                   "★ 删掉的是控件本身（实际剩 " + std::to_string(e5.controls().size()) + " 个）");
        expectTrue(e5.blocks()[0].textUtf8() == "患者主诉：三天，加重一天。", "文本不变");
        expectTrue(e5.auditCompile() == 0, "★ 之后产物仍与整树全量编译逐字段一致");

        //   规则 4 + 空控件：光标在空控件的开括号左面，删掉控件本身；
        //   撤销要把光标放回按键之前那一格，而不是"选中整个控件"：后者的表现是撤销之后
        //   光标落到闭括号右边，还带着一圈选区。
        reload5();
        e5.setCaretAtSide(0, 9, 1);
        for (int i = 0; i < 4; ++i) expectTrue(e5.backspace(), "先把这个字段删空");
        e5.setCaretAtSide(0, 5, 0);
        expectTrue(e5.caretSide() == 0 && e5.caret().offset == 5, "光标在字段 #0 的开括号之前");
        expectTrue(e5.deleteForward(), "★ 开括号之前按 Delete：删掉这个空控件");
        expectTrue(e5.controls().size() == 1 && e5.controls().at(0).id == "k2", "控件没了");
        expectTrue(e5.undoAndRender(), "★ 撤销这一次删除");
        expectTrue(e5.controls().size() == 2 && e5.controls().at(0).id == "k1",
                   "★ 控件回来了（实际 " + std::to_string(e5.controls().size()) + " 个）");
        expectTrue(!e5.hasSelection(), "★ 撤销之后没有选区（按键之前本来就没有）");
        expectTrue(e5.caret().offset == 5 && e5.caretSide() == 0,
                   "★ 撤销之后光标回到按键之前那一格（cp 5 侧别 0），不是跑到闭括号右边 —— "
                   "实际 " +
                       std::to_string(e5.caret().offset) + "/" + std::to_string(e5.caretSide()));
        expectTrue(e5.auditCompile() == 0, "★ 撤销之后产物仍与整树全量编译逐字段一致");

        // (h) 表单模式：删除的边界就是控件内容 —— 空控件删不掉，光标也不会被带出括号
        reload5();
        e5.setEditMode(EditMode::Form);
        e5.relayout();
        e5.setCaretAtSide(0, 9, 0);
        expectTrue(e5.caretSide() == 0, "光标在字段 #0 的内容末尾（] 之内）");
        for (int i = 0; i < 4; ++i) expectTrue(e5.backspace(), "表单模式：先把这个字段删空");
        expectTrue(e5.controls().size() == 2, "k1 还在（空的、零宽）");
        {
            const long long b3 = e5.blockedEdits();
            expectTrue(!e5.backspace(), "★ 空控件上退格：拒（表单模式不删控件）");
            expectTrue(e5.controls().size() == 2, "控件还在");
            expectTrue(e5.caret().offset == 5 && e5.caretSide() == 1,
                       "★ 光标停在内容起点（cp 5 侧别 1），没被带到括号外面 —— 实际 " +
                           std::to_string(e5.caret().offset) + "/" +
                           std::to_string(e5.caretSide()));
            expectTrue(e5.blockedEdits() == b3 + 1, "★ 这次拒绝记了账");
        }
    }

    if (runSection(36)) std::printf("\n[36] 撤销上限：配置项真的落到了栈上，超出时丢最旧的\n");
    if (runSection(36)) {
        //  上限是 EngineConfig 的一项（默认 100 —— 与 Office 系和 Quill 的 maxStack 同值）。
        //  这一节管的是"配置那一项真的转到了 UndoStack 上"：只测 UndoStack 自己的上限
        //  （model_test 1.3 那两条）测不出引擎漏转发 —— 那种情况下它照样全绿。
        std::vector<BlockSpec> bl36(1);
        bl36[0].setText("甲乙丙丁戊己庚辛壬癸");
        EditorEngine e36;
        expectTrue(e36.init({cjkPath}), "撤销上限：字体加载成功");
        EngineConfig c36;
        c36.zoom = 1.0f;
        c36.undo_limit = 3;
        e36.setConfig(c36);
        e36.setDocument(demoDoc(bl36, {}), StyleSheet{});
        e36.setEditMode(EditMode::Document);
        e36.relayout();
        expectTrue(e36.config().undo_limit == 3, "配置里的上限是 3（装载之前就设好了）");
        expectTrue(!e36.canRedo(), "刚装载：没有可重做的");

        // 敲六笔（每笔一个"测"），上限 3 ⇒ 提交时最旧的三笔已被丢掉。
        e36.setCaret(0, 0);
        for (int i = 0; i < 6; ++i) expectTrue(e36.typeTextAndRender("\xE6\xB5\x8B"), "敲一笔");
        int undone = 0;
        while (e36.undoAndRender()) ++undone;
        expectTrue(undone == 3, "★ 只撤得动三笔（配置的上限真的落到了栈上，实际 " +
                                    std::to_string(undone) + "）");
        expectTrue(e36.blocks()[0].textUtf8() == "测测测甲乙丙丁戊己庚辛壬癸",
                   "★ 撤不回的是最旧那三笔（剩下 " + e36.blocks()[0].textUtf8() + "）");

        // 调小上限当场裁，不必等下一次提交。
        for (int i = 0; i < 3; ++i) e36.typeTextAndRender("\xE6\xB5\x8B");
        EngineConfig c36b = e36.config();
        c36b.undo_limit = 1;
        e36.setConfig(c36b);
        int undone2 = 0;
        while (e36.undoAndRender()) ++undone2;
        expectTrue(undone2 == 1, "★ 上限调成 1 之后当场裁到一条（实际还能撤 " +
                                     std::to_string(undone2) + " 次）");

        // 0 = 不限制，改完仍是可编辑的（别把栈设成 0 之后连编辑都不让）。
        EngineConfig c36c = e36.config();
        c36c.undo_limit = 0;
        e36.setConfig(c36c);
        expectTrue(e36.typeTextAndRender("\xE6\xB5\x8B"), "上限 0 时照样能编辑");
        expectTrue(e36.undoAndRender(), "上限 0 时那一笔照样能撤");
    }

    if (runSection(37)) std::printf("\n[37] 选区与移动族：真源、扩展、上下键与矩形\n");
    if (runSection(37)) {
        // 这一节管的是"选区是引擎里的一等状态，不是宿主自己记的两对数"：扩展移动只动
        //   focus（anchor 留在原处）、别的定位一律把选区折叠、上下键跨短行保住列位，
        //   以及选区最终能不能落到矩形上 —— 看不见的选区等于没有。
        std::vector<BlockSpec> bl37(3);
        bl37[0].setText("第一段甲乙丙丁戊己庚辛壬癸子丑寅卯");
        bl37[1].setText("第二段辰巳午未申酉戌亥甲乙丙丁戊");
        bl37[2].setText("第三段");

        EditorEngine e37;
        expectTrue(e37.init({cjkPath}), "选区：字体加载成功");
        EngineConfig c37;
        c37.zoom = 1.0f;
        // 页宽调窄：上下键与"跨行拆矩形"必须真的跨行才算测到，A4 宽很可能一行装得下。
        c37.page.width_pt = 200.0f;
        c37.page.margins = PageMargins::all(20.0f);
        e37.setConfig(c37);
        e37.setDocument(demoDoc(bl37, {}), StyleSheet{});
        e37.setEditMode(EditMode::Document);
        e37.relayout();

        // (a) 初始：空选区
        expectTrue(!e37.hasSelection(), "刚装载：没有选区");

        // (b) 扩展移动：anchor 不动
        e37.setCaret(0, 3);
        const FlowPos anchor0 = e37.selection().anchor;
        expectTrue(e37.moveCaretBy(2, /*extend=*/true), "Shift+右 走两格");
        expectTrue(e37.hasSelection(), "★ 扩展之后有选区");
        expectTrue(e37.selection().anchor == anchor0, "★ 锚点没动（扩展从锚点那一端长）");
        const FlowPos focus0 = e37.selection().focus;
        expectTrue(focus0 == anchor0 + 2, "焦点端在锚点之后两格");
        expectTrue(e37.moveCaretBy(3, true), "再 Shift+右 走三格");
        expectTrue(e37.selection().anchor == anchor0, "★ 连续扩展锚点仍不动");
        expectTrue(e37.selection().focus == focus0 + 3, "焦点端又前进三格");

        // (c) 反向扩展：锚点仍然不动、焦点越过锚点（选区换了方向）
        expectTrue(e37.moveCaretBy(-4, true), "Shift+左 回退四格");
        expectTrue(e37.selection().anchor == anchor0, "★ 反向扩展锚点还是不动");
        expectTrue(e37.selection().focus == focus0 - 1, "焦点端落到锚点之前一格");

        // (d) 普通移动把选区折叠
        expectTrue(e37.moveCaretBy(1, false), "裸右移");
        expectTrue(!e37.hasSelection(), "★ 非扩展移动把选区折叠（两端收到一起）");

        // (e) 越界夹取
        e37.setSelection(Range{999999, -5});
        const Range clamped = e37.selection();
        expectTrue(clamped.anchor >= 0 && clamped.focus >= 0, "夹取：两端都不为负");
        expectTrue(clamped.focus == 0, "夹取：负的那端收到 0");
        expectTrue(clamped.anchor > 0, "夹取：越界的那端收到能到的最远处");

        // (f) 全选：起点是第一块内容的起点、末端是最后一段的段末
        expectTrue(e37.selectAll(), "全选");
        const Range all37 = e37.selection();
        //   起点不写死 0：流位置 0 可能落在文档最前面的节点边界上，而"全选"选的是
        //   内容 —— 起点该是第一块内容的第一个插入点，那才是"到文档首"的落点
        //   （moveCaretToDocStart 走的就是它）。
        e37.moveCaretToDocStart();
        const FlowPos doc_first = e37.selection().focus;
        expectTrue(all37.begin() == doc_first, "★ 全选起点是第一块内容起点（期望 " +
                                                   std::to_string(doc_first) + "，实际 " +
                                                   std::to_string(all37.begin()) + "）");
        expectTrue(all37.focus > all37.anchor, "锚点在首、焦点在末");
        e37.setCaretAtFlow(all37.end());
        expectTrue(e37.caret().para == 2 && e37.caret().offset == 3,
                   "★ 全选的末端就是最后一段的段末（实际 (" + std::to_string(e37.caret().para) +
                       ", " + std::to_string(e37.caret().offset) + ")）");

        // (g) 空选区零矩形
        e37.setCaret(0, 0);
        expectTrue(!e37.hasSelection(), "定位之后没有选区");
        expectTrue(e37.selectionRects().empty(), "★ 空选区零矩形");

        // (h) 一行内两字：一个矩形，宽 = 两簇推进之和
        expectTrue(e37.moveCaretBy(2, true), "Shift+右 两格 = 选中两个字");
        const std::vector<EditorEngine::PageRect> rects37 = e37.selectionRects();
        expectTrue(rects37.size() == 1,
                   "同一行内两字 → 一个矩形（实际 " + std::to_string(rects37.size()) + "）");
        const LineBox* lb37 = nullptr;
        for (const LineBox& lb : e37.layout().lines) {
            if (lb.paragraph == 0) {
                lb37 = &lb;
                break;
            }
        }
        expectTrue(lb37 != nullptr, "第一段有行盒");
        if (lb37 != nullptr && rects37.size() == 1) {
            const float x0 = lb37->clusters[0].inline_pt;
            const float x1 = lb37->clusters[2].inline_pt;
            expectTrue(std::fabs(rects37[0].r_pt.w - (x1 - x0)) < 0.01f,
                       "★ 矩形宽 = 两簇推进之和（期望 " + std::to_string(x1 - x0) + "，实际 " +
                           std::to_string(rects37[0].r_pt.w) + "）");
        }

        // (i) 跨行：矩形按行拆开，块轴范围各不同
        expectTrue(e37.selectAll(), "再全选");
        const std::vector<EditorEngine::PageRect> many37 = e37.selectionRects();
        expectTrue(many37.size() >= 4,
                   "★ 全选的矩形按行拆（实际 " + std::to_string(many37.size()) + " 个）");
        if (many37.size() >= 2) {
            float lo_y = 1e9f;
            float hi_y = -1e9f;
            for (const EditorEngine::PageRect& pr : many37) {
                lo_y = std::min(lo_y, pr.r_pt.y);
                hi_y = std::max(hi_y, pr.r_pt.y);
            }
            expectTrue(hi_y > lo_y, "★ 折行的矩形块轴位置各不相同（最小的 " + std::to_string(lo_y) +
                                        " < 最大的 " + std::to_string(hi_y) + "）");
        }

        // (j) 上下键：↓↑ 回原位
        e37.clearSelection();
        e37.setCaret(0, 4);
        const TextPos base37 = e37.caret();
        expectTrue(e37.moveCaretVertical(1), "向下走一行");
        expectTrue(!(e37.caret() == base37), "★ 向下之后位置变了");
        expectTrue(e37.moveCaretVertical(-1), "向上回来");
        expectTrue(e37.caret() == base37, "★ ↓↑ 回到原位");

        // (k) 列位保持：跨过一条短行之后，落点仍与起点同列而不是短行的行末
        e37.setCaret(0, 8);
        Rect ra37{};
        expectTrue(e37.caretRectPt(&ra37), "取起始光标的页面矩形");
        expectTrue(e37.moveCaretVertical(1), "下移（进短行）");
        expectTrue(e37.moveCaretVertical(1), "再下移（出行）");
        Rect rc37{};
        expectTrue(e37.caretRectPt(&rc37), "取落定光标的页面矩形");
        expectTrue(std::fabs(rc37.x - ra37.x) < 1.0f, "★ 跨过短行后列位保留（起点 x " +
                                                          std::to_string(ra37.x) + "，落点 x " +
                                                          std::to_string(rc37.x) + "）");

        // (l) 文档首尾
        e37.setCaret(2, 3);
        expectTrue(e37.moveCaretToDocStart(), "到文档首");
        expectTrue(e37.caret().para == 0 && e37.caret().offset == 0, "★ 到文档首");
        expectTrue(!e37.moveCaretToDocStart(), "已经在文档首：再按不动（返回假）");
        expectTrue(e37.moveCaretToDocEnd(), "到文档末");
        expectTrue(e37.caret().para == 2 && e37.caret().offset == 3, "★ 到文档末");
        e37.setCaret(1, 2);
        expectTrue(e37.moveCaretToDocEnd(/*extend=*/true), "Shift+Ctrl+End");
        expectTrue(e37.selection().anchor != e37.selection().focus, "★ 扩展版真的扩展了");

        // (m) 编辑之后不留残选区 —— 忘了清的症状是"打完字还留着一条看不见的选区"
        e37.selectAll();
        expectTrue(e37.hasSelection(), "先全选");
        expectTrue(e37.typeTextAndRender("\xE6\xB5\x8B"), "打一个字");
        expectTrue(!e37.hasSelection(), "★ 编辑之后选区被清掉了（没有残留）");
    }

    if (runSection(38)) std::printf("\n[38] 编辑入口吃掉选区：替换、只删选区、跨段删除与撤销\n");
    if (runSection(38)) {
        // 这一节管的是"选中之后按下任何一个编辑键"。四条口径：
        //   插类入口（打字、缩进、回车）是"先删掉选区，再在原地插"；
        //   删类入口（退格、前删、合并、反缩进）是"删掉选区就完事"，不再多吃一个字；
        //   跨段选区能整段删掉（此前 eraseOnTree 见到跨段直接返回假）；
        //   撤销一次回到编辑前那两端（anchor 与 focus 都在）。
        //
        // 文本一律从 blocks()[i].textUtf8() 读回逐个字符比对 —— 不比"看起来对不对"。
        const auto reload38 = [](EditorEngine& e) {
            std::vector<BlockSpec> bl(3);
            bl[0].setText("零一二三四五六七八九十");
            bl[1].setText("甲乙丙丁戊己庚辛壬癸");
            bl[2].setText("子丑寅卯");
            e.setDocument(demoDoc(bl, {}), StyleSheet{});
            e.setEditMode(EditMode::Document);
            e.relayout();
        };
        const auto text38 = [](EditorEngine& e, int i) {
            const auto& bs = e.blocks();
            if (i < 0 || i >= static_cast<int>(bs.size())) return std::string();
            return bs[static_cast<size_t>(i)].textUtf8();
        };
        const auto paras38 = [](EditorEngine& e) { return static_cast<int>(e.blocks().size()); };
        // 从 (0, 2) 起扩展两格 = 选中第 2、3 两个字。每节都用它起头，免得各写一遍。
        const auto selectTwo38 = [](EditorEngine& e) {
            e.setCaret(0, 2);
            return e.moveCaretBy(2, /*extend=*/true);
        };

        EditorEngine e38;
        expectTrue(e38.init({cjkPath}), "选区吃掉：字体加载成功");
        EngineConfig c38;
        c38.zoom = 1.0f;
        c38.page.width_pt = 200.0f;
        c38.page.margins = PageMargins::all(20.0f);
        e38.setConfig(c38);
        reload38(e38);

        // (a) 选中再打字 = 原文 − 选区 + 新字
        expectTrue(selectTwo38(e38), "选中第 2、3 两个字");
        expectTrue(e38.hasSelection(), "选区非空");
        const Range before38 = e38.selection();
        expectTrue(e38.typeTextUtf8("测"), "打字");
        expectTrue(text38(e38, 0) == "零一测四五六七八九十",
                   "★ 选中再打字 = 原文 − 选区 + 新字（实际 " + text38(e38, 0) + "）");
        expectTrue(!e38.hasSelection(), "★ 替换之后没有残选区");
        expectTrue(e38.caret().para == 0 && e38.caret().offset == 3,
                   "落点在插入的那个字之后（实际 (" + std::to_string(e38.caret().para) + ", " +
                       std::to_string(e38.caret().offset) + ")）");
        expectTrue(paras38(e38) == 3,
                   "★ 同段内替换不改段数（实际 " + std::to_string(paras38(e38)) + " 段）");

        // (b) 撤销一次：文字与选区那两端一起回来
        expectTrue(e38.undo(), "撤销");
        expectTrue(text38(e38, 0) == "零一二三四五六七八九十",
                   "★ 撤销回原文（实际 " + text38(e38, 0) + "）");
        expectTrue(e38.hasSelection(), "★ 撤销之后选区还在（不是只剩一个光标）");
        expectTrue(e38.selection() == before38, "★ 撤销回到编辑前两端（期望 " +
                                                    std::to_string(before38.anchor) + ".." +
                                                    std::to_string(before38.focus) + "，实际 " +
                                                    std::to_string(e38.selection().anchor) + ".." +
                                                    std::to_string(e38.selection().focus) + "）");
        // 撤销只重编译、只标脏；要看矩形得先重排一次（宿主侧的 *AndRender 版自带这一步）。
        e38.relayout();
        expectTrue(!e38.selectionRects().empty(), "★ 回来的选区真的画得出来（有矩形）");

        // (c) 重做：替换那一下整套回来，且落回它记下的"编辑后"空选区
        expectTrue(e38.redo(), "重做");
        expectTrue(text38(e38, 0) == "零一测四五六七八九十", "★ 重做回到替换之后");
        expectTrue(!e38.hasSelection(), "重做之后没有选区（事务记的是折叠后的位置）");

        // (d) 退格在选区非空时只删选区，不再往前多吃一个字
        reload38(e38);
        expectTrue(selectTwo38(e38), "再选中两个字");
        expectTrue(e38.backspace(), "退格");
        expectTrue(text38(e38, 0) == "零一四五六七八九十",
                   "★ 退格只删掉选中的两个字（实际 " + text38(e38, 0) +
                       "；若多删一个字会得到 零四五六七八九十）");
        expectTrue(e38.caret().para == 0 && e38.caret().offset == 2, "落点在删除区间的起点");
        expectTrue(paras38(e38) == 3, "同段内删除不改段数");

        // (e) 前删同理
        reload38(e38);
        expectTrue(selectTwo38(e38), "再选中两个字");
        expectTrue(e38.deleteForward(), "前删");
        expectTrue(
            text38(e38, 0) == "零一四五六七八九十",
            "★ 前删也只删选区（实际 " + text38(e38, 0) + "；若多删一个字会得到 零一五六七八九十）");

        // (f) 空选区时退格照旧删一个字 —— 反向一半：这条不变红，
        //     "退格只删选区"那条就没有对照，"退格什么都不做"也能让它绿。
        reload38(e38);
        e38.setCaret(0, 3);
        expectTrue(!e38.hasSelection(), "没有选区");
        expectTrue(e38.backspace(), "裸退格");
        expectTrue(text38(e38, 0) == "零一三四五六七八九十",
                   "★ 空选区时退格照旧删一个字（实际 " + text38(e38, 0) +
                       "；若空选区下也去删选区，就会一个字都不动）");

        // (g) 跨段删除：与"手工删同样内容"逐字节同帧
        EditorEngine eA;
        EditorEngine eC;
        expectTrue(eA.init({cjkPath}) && eC.init({cjkPath}), "跨段删除：两个引擎都起得来");
        eA.setConfig(c38);
        eC.setConfig(c38);
        reload38(eA);
        reload38(eC);

        // A 路：选中 [第 0 段第 4 字, 第 1 段第 3 字) 一次删掉。
        //   中间跨过 7 个字 + 一枚段落标记 + 3 个字 = 11 个流位置。
        eA.setCaret(0, 4);
        expectTrue(eA.moveCaretBy(11, true), "扩展 11 格（跨过段落标记）");
        expectTrue(eA.caret().para == 1 && eA.caret().offset == 3,
                   "焦点落在第 1 段第 3 个字之前（实际 (" + std::to_string(eA.caret().para) + ", " +
                       std::to_string(eA.caret().offset) + ")）");
        expectTrue(eA.backspace(), "退格删掉跨段选区");
        expectTrue(text38(eA, 0) == "零一二三丁戊己庚辛壬癸",
                   "★ 两段并成一段、内容接得上（实际 " + text38(eA, 0) + "）");
        expectTrue(paras38(eA) == 2,
                   "★ 跨段删除并掉一段（3 → 2，实际 " + std::to_string(paras38(eA)) + "）");
        expectTrue(text38(eA, 1) == "子丑寅卯", "后面那一段没受影响");

        // C 路：同样的结果用 11 下前删手工做出来（7 个字 + 一次段末合并 + 3 个字）。
        eC.setCaret(0, 4);
        bool hand38 = true;
        for (int i = 0; i < 7; ++i) hand38 = hand38 && eC.deleteForward();
        expectTrue(hand38, "手工删：前删 7 下");
        expectTrue(eC.deleteForward(), "手工删：段末前删把下一段并进来");
        hand38 = true;
        for (int i = 0; i < 3; ++i) hand38 = hand38 && eC.deleteForward();
        expectTrue(hand38, "手工删：再前删 3 下");
        expectTrue(text38(eC, 0) == text38(eA, 0),
                   "★ 手工删同样内容得到同一段文本（" + text38(eC, 0) + "）");
        expectTrue(paras38(eC) == paras38(eA), "段数也一样");
        expectTrue(eC.caret().para == eA.caret().para && eC.caret().offset == eA.caret().offset,
                   "落点也一样");

        // 逐字节同帧：两条路走到同一个文档状态，帧就该一个字节都不差。
        //   关掉光标再比 —— 闪烁相位与光标位置是另一件事，别让它污染这一条。
        eA.showCaret(false);
        eC.showCaret(false);
        // 必须各自重排一次再呈现：编辑只标脏（markLayoutDirty），真正的排版要等 relayout。
        //   少了这一步，两张帧画的都是编辑之前那一版 layout，它们当然逐字节相同，
        //   于是这条"逐字节同帧"变成一句空话（反证 A 就是这么被漏过去的）。
        eA.relayout();
        eC.relayout();
        eA.repaint();
        eC.repaint();
        expectTrue(eA.frameBytes() == eC.frameBytes(), "两条路的帧大小一致");
        expectTrue(std::memcmp(eA.frame(), eC.frame(), eA.frameBytes()) == 0,
                   "★ 跨段删除与手工删同样内容逐字节同帧");

        // (h) 全选再打字 = 整篇被替换成一个字
        reload38(e38);
        expectTrue(e38.selectAll(), "全选");
        expectTrue(e38.typeTextUtf8("测"), "整篇替换成一个字");
        expectTrue(paras38(e38) == 1, "★ 段数收成 1（实际 " + std::to_string(paras38(e38)) + "）");
        expectTrue(text38(e38, 0) == "测",
                   "★ 全选再打字 = 只剩那个字（实际 " + text38(e38, 0) + "）");
        expectTrue(!e38.hasSelection(), "没有残选区");

        // (i) 插类的另两个入口：回车与缩进也是"先删选区再插"
        reload38(e38);
        expectTrue(selectTwo38(e38), "选中两个字");
        expectTrue(e38.splitParagraph(), "回车（带选区）");
        expectTrue(paras38(e38) == 4,
                   "★ 回车：先删选区再分段（3 → 4，实际 " + std::to_string(paras38(e38)) + "）");
        expectTrue(text38(e38, 0) == "零一", "前半段（实际 " + text38(e38, 0) + "）");
        expectTrue(text38(e38, 1) == "四五六七八九十", "后半段（实际 " + text38(e38, 1) + "）");

        reload38(e38);
        expectTrue(selectTwo38(e38), "选中两个字");
        expectTrue(e38.indent(), "Tab（带选区）");
        expectTrue(text38(e38, 0) == "零一    四五六七八九十",
                   "★ 缩进：选区被 4 个空格替换（实际 " + text38(e38, 0) + "）");

        // (j) 反缩进归"删类"：删完即止，不去动光标前面那些空格
        reload38(e38);
        expectTrue(selectTwo38(e38), "选中两个字");
        expectTrue(e38.outdent(), "反缩进（带选区）");
        expectTrue(text38(e38, 0) == "零一四五六七八九十",
                   "★ 反缩进只删选区（实际 " + text38(e38, 0) + "）");

        // (k) 程序化指定落点的插入不把选区当靶子 —— 那种坐标是按调用时的文档算出来的，
        //     先删掉一段会让整批坐标一起过期，所以这条边界刻意留着。
        //     它当然仍会把光标搬到插入处（那是"插入"自己的契约），这里要钉的是
        //     "选区里的字一个都没少"，而不是"选区状态原封不动"。
        reload38(e38);
        expectTrue(selectTwo38(e38), "选中两个字");
        expectTrue(e38.insertTextUtf8(0, 9, "X"), "在别处插一个字");
        expectTrue(text38(e38, 0) == "零一二三四五六七八X九十",
                   "★ 指定落点的插入不动选区里的字（实际 " + text38(e38, 0) +
                       "；若把它当靶子会先删掉 二三）");

        // (l) 合并入口在选区非空时也退化成"删掉选区"
        reload38(e38);
        expectTrue(selectTwo38(e38), "选中两个字");
        expectTrue(e38.mergeWithPreviousParagraph(), "合并（带选区）");
        expectTrue(text38(e38, 0) == "零一四五六七八九十",
                   "★ 合并退化成删选区（实际 " + text38(e38, 0) + "）");
        expectTrue(paras38(e38) == 3, "段数不变（没有真的合并）");
    }

    if (runSection(39))
        std::printf("\n[39] 拖选只搬焦点端：anchor 不动、方向可反、拖回原位收成空\n");
    if (runSection(39)) {
        // 这一节钉引擎侧那一个入口（宿主侧的按下与移动由探针 [20] 走真鼠标事件管）。
        // 三条口径：拖动只改选区不改正文；anchor 是按下点、focus 是当前点且可前可后；
        // focus 回到按下点就是空选区 —— 最后这条是反向的一半，没有它"什么都不做"也能全绿。
        const auto reload39 = [](EditorEngine& e) {
            std::vector<BlockSpec> bl(3);
            bl[0].setText("零一二三四五六七八九十");
            bl[1].setText("甲乙丙丁戊己庚辛壬癸");
            bl[2].setText("子丑寅卯");
            e.setDocument(demoDoc(bl, {}), StyleSheet{});
            e.setEditMode(EditMode::Document);
            e.relayout();
        };
        const auto text39 = [](EditorEngine& e, int i) {
            const auto& bs = e.blocks();
            if (i < 0 || i >= static_cast<int>(bs.size())) return std::string();
            return bs[static_cast<size_t>(i)].textUtf8();
        };
        // 两端是全局流位置，断言要的是"它落在哪一段哪一位"，于是翻回文本坐标再比。
        const auto at39 = [](EditorEngine& e, FlowPos f) {
            TextPos p{};
            if (!e.flowToTextPos(f, &p)) return TextPos{-1, 0};
            return p;
        };

        EditorEngine e39;
        expectTrue(e39.init({cjkPath}), "拖选：字体加载成功");
        EngineConfig c39;
        c39.zoom = 1.0f;
        c39.page.width_pt = 200.0f;
        c39.page.margins = PageMargins::all(20.0f);
        e39.setConfig(c39);

        // (a) 按下落在 (0,2)，拖到 (0,6)：anchor 停在按下点，focus 跟着走
        reload39(e39);
        const std::string before39 = text39(e39, 0);
        e39.setCaretAtSide(0, 2, -1);
        expectTrue(!e39.hasSelection(), "按下时还是空选区（定位把 anchor 定在按下点）");
        e39.setFocusAtSide(0, 6, -1);
        expectTrue(e39.hasSelection(), "拖出选区");
        {
            const Range r = e39.selection();
            const TextPos a = at39(e39, r.anchor);
            const TextPos f = at39(e39, r.focus);
            expectTrue(a.para == 0 && a.offset == 2, "★ anchor 停在按下点（实际 " +
                                                         std::to_string(a.para) + "," +
                                                         std::to_string(a.offset) + "）");
            expectTrue(f.para == 0 && f.offset == 6, "★ focus 在拖动终点（实际 " +
                                                         std::to_string(f.para) + "," +
                                                         std::to_string(f.offset) + "）");
            expectTrue(text39(e39, 0) == before39, "★ 拖动不改正文（实际 " + text39(e39, 0) + "）");
        }

        // (b) 反向拖：focus 跑到 anchor 前面，两端不交换（选区是带方向的）
        e39.setFocusAtSide(0, 0, -1);
        {
            const Range r = e39.selection();
            const TextPos a = at39(e39, r.anchor);
            const TextPos f = at39(e39, r.focus);
            expectTrue(a.offset == 2 && f.offset == 0, "★ 反向拖：anchor 仍在前端");
        }

        // (c) 拖回按下点 = 空选区
        e39.setFocusAtSide(0, 2, -1);
        expectTrue(!e39.hasSelection(), "★ 拖回按下点收成空选区");

        // (d) 连续移动不碰正文（一次拖动是很多次移动，不是一次）
        reload39(e39);
        e39.setCaretAtSide(0, 1, -1);
        for (int k = 2; k <= 9; ++k) e39.setFocusAtSide(0, k, -1);
        expectTrue(e39.hasSelection(), "连续移动之后有选区");
        {
            const Range r = e39.selection();
            const TextPos a = at39(e39, r.anchor);
            const TextPos f = at39(e39, r.focus);
            expectTrue(a.offset == 1 && f.offset == 9, "★ 两端落在按下点与最后一次移动的落点");
        }
        expectTrue(text39(e39, 0) == "零一二三四五六七八九十" &&
                       text39(e39, 1) == "甲乙丙丁戊己庚辛壬癸" && text39(e39, 2) == "子丑寅卯",
                   "★ 连续移动不碰正文（实际 " + text39(e39, 0) + " / " + text39(e39, 1) + " / " +
                       text39(e39, 2) + "）");
        expectTrue(static_cast<int>(e39.blocks().size()) == 3, "段数不变");

        // (e) 跨段拖：按下在第 0 段第 8 个字之前、拖到第 1 段第 3 个字之前
        reload39(e39);
        e39.setCaretAtSide(0, 8, -1);
        e39.setFocusAtSide(1, 3, -1);
        expectTrue(e39.hasSelection(), "跨段拖出选区");
        expectTrue(e39.caret().para == 1 && e39.caret().offset == 3,
                   "★ 焦点端落到第 1 段（实际 " + std::to_string(e39.caret().para) + "," +
                       std::to_string(e39.caret().offset) + "）");
        // 先把两端钉住，后面的"吃掉之后正文变成什么"才有依据：
        //   从第 0 段第 8 位到段末是 八九十 三个字，加一枚段落标记，再加第 1 段的 甲乙丙。
        {
            const Range r = e39.selection();
            const TextPos a = at39(e39, r.anchor);
            expectTrue(a.para == 0 && a.offset == 8, "★ 跨段拖的 anchor 在 (0, 8)（实际 " +
                                                         std::to_string(a.para) + "," +
                                                         std::to_string(a.offset) + "）");
        }
        // 先重排再读几何：改选区只标脏，矩形要等重排之后才成立。
        e39.relayout();
        expectTrue(!e39.selectionRects().empty(), "★ 跨段选区有矩形（画得进帧）");
        // 拖出来的选区与键盘扩出来的选区是同一件东西：打字一样会被吃掉（与第 5 刀接上）。
        expectTrue(e39.typeTextUtf8("测"), "拖选之后打字");
        expectTrue(text39(e39, 0) == "零一二三四五六七测丁戊己庚辛壬癸",
                   "★ 拖出来的跨段选区被吃掉并掉一段（实际 " + text39(e39, 0) + "）");
        expectTrue(static_cast<int>(e39.blocks().size()) == 2 && text39(e39, 1) == "子丑寅卯",
                   "★ 段数 3 变 2、末段没受影响（实际 " +
                       std::to_string(static_cast<int>(e39.blocks().size())) + " 段）");

        // (f) 侧别与定位走同一条投影：两边在同一个 (段, 偏移, 侧别) 上必须落在同一个流位置
        reload39(e39);
        e39.setCaretAtSide(0, 3, 1);
        const FlowPos byPress39 = e39.selection().focus;
        e39.setCaretAtSide(0, 0, -1);
        e39.setFocusAtSide(0, 3, 1);
        expectTrue(e39.selection().focus == byPress39,
                   "★ 拖选的落点与按下定位同一条投影（侧别也一起透传）");
    }

    if (runSection(40)) std::printf("\n[40] 连页模式：排版不在页底断开，内容一列连到底\n");
    if (runSection(40)) {
        // 一条判据要能证伪，就得有反向的一半：同一个文档、同一套页度量，
        //   分页下必须在页底让位（行盒的文档 y 会回跳、墨迹够不到下边距），
        //   连页下必须不让位（y 单调不降、墨迹跑进下边距那条带）。
        //   只断言"连页下 y 单调"是不够的 —— 那个断言的成立与否也可能与模式无关。
        EditorEngine e40;
        expectTrue(e40.init({cjkPath, mnPath, latinPath}), "引擎初始化");

        EngineConfig c40;
        c40.zoom = 1.0f;  // 1 pt = 1 px：便于直接按磅定位像素
        // 页度量取"边距与页间隙都够大"的那一档：分页下跨页那一跳至少是
        //   页间隙 + 两倍上边距（= 60 磅），而连页下最大的一跳只是段间距（= 31 磅）——
        //   两者必须彻底分开，判据才不是"恰好比大小"。
        c40.page.width_pt = 240.0f;
        c40.page.height_pt = 120.0f;
        c40.page.margins = PageMargins::all(20.0f);
        c40.page_gap_pt = 20.0f;
        e40.setConfig(c40);

        BlockSpec b40;
        b40.font_index = 0;  // 中文
        b40.size_pt = 12.0f;
        b40.wm = WritingMode::HorizontalTB();
        b40.line_spacing.factor = 1.75f;
        b40.language = "zh";
        // 12 段各 40 个「测」：足够跨好几页，而且段与段之间有段间距。
        std::vector<BlockSpec> bs40;
        for (int i = 0; i < 12; ++i) {
            b40.cp_text.assign(40, 0x6D4B);
            bs40.push_back(b40);
        }
        loadBlocks(e40, bs40);
        // 行距 = 单倍行高（字体的 ascent + descent）x 倍数。
        const FontInstance* f40 = e40.fonts().instance(b40.font_index, b40.size_pt);
        const float natural40 = f40 ? (f40->ascenderPt() + f40->descenderPt()) : b40.size_pt;

        // 行盒的"文档 y"（磅）：页号 x 条带高 + 页内块轴位置。
        //   横排的块轴是 y；这份文档全是横排。
        auto docY = [&](const LineBox& lb) {
            const float slab = c40.page.height_pt + c40.page_gap_pt;
            return static_cast<float>(lb.page) * slab + lb.block_lo_pt;
        };
        // 扫一遍行盒，报四件事：相邻两行的最大回跳、最大间距（文档 y）、
        //   行盒底离条带底的最小距离、用到了几条带。
        //   为什么要有"最大间距"这一条：分页下页底让位表现为"间距突然变大"
        //   （上一页的最后一行 + 下边距 + 页间隙 + 上边距 + 下一页的第一行），
        //   而不是文档 y 回跳 —— 加回页号之后它一定还是递增的，回跳只发生在页内坐标上。
        struct Scan {
            float max_back = 0.0f;   // 最大回跳（正数表示回跳了这么多）
            float max_step = 0.0f;   // 相邻两行的最大间距
            float max_pitch = 0.0f;  // 同段内相邻两行的最大间距（= 行距）
            float min_slack = 1e9f;  // 行盒底离条带底的最小距离
            int slabs = 0;
        };
        auto scan40 = [&]() {
            Scan s;
            const float slab = c40.page.height_pt + c40.page_gap_pt;
            const std::vector<LineBox>& ls = e40.layout().lines;
            for (size_t i = 1; i < ls.size(); ++i) {
                const float d = docY(ls[i]) - docY(ls[i - 1]);
                if (d < -s.max_back) s.max_back = -d;
                if (d > s.max_step) s.max_step = d;
                if (ls[i].paragraph == ls[i - 1].paragraph && d > s.max_pitch) s.max_pitch = d;
            }
            for (const LineBox& lb : ls) {
                const float slack = slab - lb.block_hi_pt;
                if (slack < s.min_slack) s.min_slack = slack;
                if (lb.page + 1 > s.slabs) s.slabs = lb.page + 1;
            }
            return s;
        };

        c40.continuous = false;
        e40.setConfig(c40);
        e40.relayout();
        const Scan paged40 = scan40();
        std::printf(
            "     分页：%d 条带，最大间距 %.2f 磅，同段行距 %.2f 磅，"
            "离条带底最近 %.2f 磅\n",
            paged40.slabs, paged40.max_step, paged40.max_pitch, paged40.min_slack);
        expectTrue(paged40.slabs >= 2, "这份文档在分页下确实跨了多条带");
        // 页底让位：跨页那两行的间距 = 页底剩下的空当 + 下边距 + 页间隙 + 上边距，
        //   所以它至少是"页间隙 + 两倍上边距"。这是分页的硬下界，连页下必须被打破。
        expectTrue(paged40.max_step >=
                       c40.page_gap_pt + c40.page.margins.top + c40.page.margins.bottom - 0.01f,
                   "分页下内容在页底让位（跨页那两行的间距至少多出一整条页间隙与两倍边距）");
        // 另一条分页硬下界：行盒底到条带底至少还有一整个上边距加一条页间隙
        //   （断行判据要求"本行放得进内容区"）。
        expectTrue(paged40.min_slack >= c40.page.margins.top + c40.page_gap_pt - 0.01f,
                   "分页下墨迹永远够不到条带底（至少留着上边距 + 页间隙）");

        c40.continuous = true;
        e40.setConfig(c40);
        e40.relayout();
        const Scan cont40 = scan40();
        std::printf(
            "     连页：%d 条带，最大间距 %.2f 磅，同段行距 %.2f 磅，"
            "离条带底最近 %.2f 磅\n",
            cont40.slabs, cont40.max_step, cont40.max_pitch, cont40.min_slack);
        expectTrue(cont40.max_back < 0.01f, "连页下文档 y 单调不降（没有一处回跳 = 没有页底）");
        expectTrue(cont40.max_pitch <= natural40 * b40.line_spacing.factor + 0.01f,
                   "连页下同段相邻两行的间距就是行距（中间没插页边距）");
        // 连页的最大间距只可能出现在段与段之间（段间距 10 磅），
        //   而分页那条跨页间距的下界是 60 磅 —— 两条判据互不重叠。
        expectTrue(cont40.max_step <= cont40.max_pitch + 10.0f + 0.01f,
                   "连页下最大的那一跳就是段间距（没有页底那一跳）");
        expectTrue(cont40.slabs < paged40.slabs, "连页下条带数比分页少（内容不再为页边距让位）");
        expectTrue(cont40.min_slack < c40.page.margins.top,
                   "连页下墨迹跑进了下边距那条带（页边距不再约束内容）");

        // 内容底边：连页下它就是滚动高度。它必须落在最后一行之下、且在条带范围内。
        const int bottom40 = e40.contentBottomPx();
        const float slab_px = (c40.page.height_pt + c40.page_gap_pt) * c40.zoom;
        std::printf("     内容底边 %d px（最后一条带从 %.0f px 起）\n", bottom40,
                    static_cast<double>((cont40.slabs - 1) * slab_px));
        expectTrue(bottom40 > static_cast<int>((cont40.slabs - 1) * slab_px),
                   "内容底边落在最后一条带里（宿主按它设滚动高度）");
    }

    if (runSection(41)) std::printf("\n[41] 折行处的行首与行尾是两个位置（光标行别）\n");
    if (runSection(41)) {
        // 一条折行的段落里，上一行的末尾与下一行的开头是同一个位置：偏移一样、侧别一样。
        //   只按位置找行，找回来的必然是上一行 —— 于是"点下一行的行首"变成"光标跳到上一行
        //   行尾"。这一节就是把那个自由度（行别）钉住，而且三个断言互相证伪：
        //   两次点击命中同一个偏移（说明位置确实相同）；带行别时各自画在自己那一行；
        //   不带行别时两次都画到上一行（把老 bug 本身写成判据，证明这条判据对它敏感）。
        EditorEngine e41;
        expectTrue(e41.init({cjkPath, mnPath, latinPath}), "引擎初始化");

        EngineConfig c41;
        c41.zoom = 1.0f;  // 1 磅 = 1 像素：命中测试收的磅值与行盒里的磅值同一把尺子
        c41.page.width_pt = 240.0f;
        c41.page.height_pt = 200.0f;
        c41.page.margins = PageMargins::all(20.0f);
        c41.page_gap_pt = 20.0f;
        e41.setConfig(c41);

        BlockSpec b41;
        b41.font_index = 0;
        b41.size_pt = 12.0f;
        b41.wm = WritingMode::HorizontalTB();
        b41.line_spacing.factor = 1.75f;
        b41.language = "zh";
        // 一段 60 个字：内容宽 200 磅、字宽 12 磅，于是一行放得下十六七个 —— 折成四行。
        b41.cp_text.assign(60, 0x6D4B);
        loadBlocks(e41, std::vector<BlockSpec>{b41});
        e41.relayout();

        // 找一对相邻行：上一行 A 的末尾就是下一行 B 的开头，而且 A 不是段首那一行
        //   （段首前面没有上一行，那种边界不存在歧义）。
        const LineBox* lbA = nullptr;
        const LineBox* lbB = nullptr;
        for (size_t i = 0; i + 1 < e41.layout().lines.size(); ++i) {
            const LineBox& x = e41.layout().lines[i];
            const LineBox& y = e41.layout().lines[i + 1];
            if (x.paragraph != y.paragraph) continue;
            if (x.first_cp == 0) continue;
            if (x.last_cp != y.first_cp) continue;
            lbA = &x;
            lbB = &y;
            break;
        }
        expectTrue(lbA != nullptr && lbB != nullptr, "这一段确实折了行（找到一对相邻行）");
        if (lbA == nullptr || lbB == nullptr) {
            std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
            return g_failures == 0 ? 0 : 1;
        }
        const uint32_t k = lbA->last_cp;
        std::printf("     折行边界在第 %u 个码点：上一行 [%u,%u)，下一行 [%u,%u)\n", k,
                    lbA->first_cp, lbA->last_cp, lbB->first_cp, lbB->last_cp);

        // 两次点击：下一行行首（往行内挪两磅）、上一行行尾（往回退两磅）。
        //   行内坐标是"相对行首"的（inline_end_pt 不含行首那一截），所以要加回行首位置。
        const float xB = lbB->inline_origin_pt + 2.0f;
        const float xA = lbA->inline_origin_pt + lbA->inline_end_pt - 2.0f;
        const float yA = (lbA->block_lo_pt + lbA->block_hi_pt) * 0.5f;
        const float yB = (lbB->block_lo_pt + lbB->block_hi_pt) * 0.5f;
        const float yA_doc = static_cast<float>(lbA->page) * 140.0f + yA;
        const float yB_doc = static_cast<float>(lbB->page) * 140.0f + yB;

        TextPos pA{};
        TextPos pB{};
        int sideA = 0;
        int sideB = 0;
        int affA = -1;
        int affB = -1;
        const bool okA = e41.hitTestPt(xA, yA_doc, &pA, nullptr, &sideA, &affA);
        const bool okB = e41.hitTestPt(xB, yB_doc, &pB, nullptr, &sideB, &affB);
        expectTrue(okA && okB, "两次点击都命中");
        expectTrue(pA.offset == k && pB.offset == k,
                   "★ 行尾与下一行行首命中同一个偏移（位置确实相同，行别不是多余的）");
        expectTrue(sideA == sideB, "★ 两次点击的侧别也相同（侧别分不开这两个位置）");
        expectTrue(affA == 0 && affB > 0, "★ 点上一行行尾报贴上一行，点下一行行首报贴下一行");

        // 带行别：各自画在自己那一行。
        e41.setCaretAtSideLine(pA.para, pA.offset, sideA, affA);
        Rect rA{};
        expectTrue(e41.caretRectPt(&rA), "上一行行尾的光标矩形可查");
        expectTrue(
            std::fabs(rA.y - lbA->block_lo_pt) < lbA->block_hi_pt - lbA->block_lo_pt + 0.01f &&
                rA.y < lbB->block_lo_pt,
            "★ 点上一行行尾，光标画在上一行");

        e41.setCaretAtSideLine(pB.para, pB.offset, sideB, affB);
        Rect rB{};
        expectTrue(e41.caretRectPt(&rB), "下一行行首的光标矩形可查");
        // 光标量的是整条行槽（多倍行距下字在槽中间），所以比的是行槽顶，不是字面盒顶。
        expectTrue(rB.y >= lbB->slotLoPt() - 0.01f,
                   "★ 点下一行行首，光标画在下一行（不是上一行行尾）");
        expectTrue(std::fabs(rB.x - (lbB->inline_origin_pt - c41.caret_width_pt * 0.5f)) < 0.51f,
                   "落点就是那一行的行首（行内位置贴着行首）");
        expectTrue(e41.caretLineAffinity() > 0, "行别存在引擎里，不是命中测试的一次性产物");

        // 不带行别（老路径）：同一个位置又回到上一行。这一条是上面那两条的反向一半 ——
        //   它证明"带上行别"确实改变了结果，而不是两处碰巧都对。
        e41.setCaretAtSide(pB.para, pB.offset, sideB);
        Rect rOld{};
        expectTrue(e41.caretRectPt(&rOld) && std::fabs(rOld.y - rA.y) < 0.01f,
                   "★ 不带行别时同一位置仍画在上一行行尾（这就是修之前的样子）");
        expectTrue(e41.caretLineAffinity() == 0, "不带行别的定位把行别清回 0");

        // 别的定位不该被行别污染：挪一次光标之后行别必须归零。
        e41.setCaretAtSideLine(pB.para, pB.offset, sideB, 1);
        e41.setCaret(0, 3);
        expectTrue(e41.caretLineAffinity() == 0, "普通定位把行别清回 0");
    }

    if (runSection(42)) std::printf("\n[42] 撤销把光标放回编辑之前的位置\n");
    if (runSection(42)) {
        // 撤销恢复的是事务里那一对快照（selection_before / selection_after）。
        //   删除类操作的 before 该记"删除之前光标在哪"：退格与反缩进的光标在区间右端，
        //   前删在左端。三处都记左端的话，退格一次再撤销，光标会落到被恢复那个字的前面。
        //   判据按"撤销之后偏移回到编辑之前"写，于是它对三条路径同时成立 —— 记错一端的那条
        //   会当场掉出来。
        EditorEngine e42;
        expectTrue(e42.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c42;
        c42.zoom = 1.0f;
        c42.page.width_pt = 240.0f;
        c42.page.height_pt = 200.0f;
        c42.page.margins = PageMargins::all(20.0f);
        e42.setConfig(c42);
        BlockSpec b42;
        b42.font_index = 0;
        b42.size_pt = 12.0f;
        b42.wm = WritingMode::HorizontalTB();
        b42.language = "zh";
        // 四个前导空格 + 八个字：反缩进那一条要用到前导空格。
        b42.cp_text.assign(4, 0x20);
        for (int i = 0; i < 8; ++i) b42.cp_text.push_back(0x6D4B);
        loadBlocks(e42, std::vector<BlockSpec>{b42});
        e42.relayout();
        const size_t len0 = e42.blocks()[0].cp_text.size();

        // 其一：退格。光标在第 6 个位置，删掉第 5 个，撤销之后光标要回到第 6 个。
        e42.setCaret(0, 6);
        expectTrue(e42.backspace(), "退格删掉一个字符");
        expectTrue(e42.caret().offset == 5, "退格之后光标落在被删位置（第 5 个）");
        expectTrue(e42.undo(), "撤销这一次退格");
        expectTrue(e42.caret().offset == 6,
                   "★ 撤销之后光标回到编辑之前那一格（被恢复的字在它左边，实际 " +
                       std::to_string(e42.caret().offset) + "）");
        expectTrue(e42.blocks()[0].cp_text.size() == len0, "撤销把字符也恢复了");

        // 其二：前删。光标在第 6 个位置，删掉第 6 个，撤销之后光标仍是第 6 个 ——
        //   它就在被恢复那个字的左边，与退格那一条正好相反的两端。
        e42.setCaret(0, 6);
        expectTrue(e42.deleteForward(), "前删掉一个字符");
        expectTrue(e42.caret().offset == 6, "前删之后光标留在原处");
        expectTrue(e42.undo(), "撤销这一次前删");
        expectTrue(e42.caret().offset == 6, "★ 前删撤销之后光标仍在第 6 个（被恢复的字在它右边）");
        expectTrue(e42.blocks()[0].cp_text.size() == len0, "撤销把字符也恢复了");

        // 其三：反缩进。光标在第 4 个位置（前导空格之后），删掉四个空格，撤销回到第 4 个。
        e42.setCaret(0, 4);
        expectTrue(e42.outdent(), "反缩进删掉前导空格");
        expectTrue(e42.caret().offset == 0, "反缩进之后光标到了段首");
        expectTrue(e42.undo(), "撤销这一次反缩进");
        expectTrue(e42.caret().offset == 4, "★ 反缩进撤销之后光标回到空格之后（实际 " +
                                                std::to_string(e42.caret().offset) + "）");
        expectTrue(e42.blocks()[0].cp_text.size() == len0, "撤销把空格也恢复了");

        // 重做仍然回到"编辑之后"那一格：两条快照各自的方向不能混。
        expectTrue(e42.redo(), "重做这一次反缩进");
        expectTrue(e42.caret().offset == 0, "重做之后光标回到编辑之后的落点");
    }

    if (runSection(43)) std::printf("\n[43] 竖排的列序：中日从右往左、蒙文从左往右\n");
    if (runSection(43)) {
        // 方向键在竖排下是"换列"，而"下一列在哪一边"由列序定。宿主的键位表只认两件事：
        //   竖不竖排、列序反不反向 —— 这一节把这两个结论钉在引擎侧，而且用"下一列的 x
        //   往哪边走"做判据（不是只读标志位：标志位对了、算出来的位置反了照样是错的）。
        EditorEngine e43;
        expectTrue(e43.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c43;
        c43.zoom = 1.0f;
        c43.page.width_pt = 240.0f;
        c43.page.height_pt = 300.0f;
        c43.page.margins = PageMargins::all(20.0f);
        e43.setConfig(c43);

        BlockSpec b43;
        b43.font_index = 0;
        b43.size_pt = 12.0f;
        b43.line_spacing.factor = 1.75f;
        b43.language = "zh";
        // 60 个字、内容高 260 磅 ⇒ 一列二十来个字，折成三列。
        b43.cp_text.assign(60, 0x6D4B);

        b43.wm = WritingMode::VerticalRL();
        loadBlocks(e43, std::vector<BlockSpec>{b43});
        e43.relayout();
        expectTrue(e43.caretVertical(), "竖排段里光标报竖排");
        expectTrue(e43.caretBlockReversed(), "中日竖排的列序反向（第一列在最右）");
        e43.setCaret(0, 5);
        Rect rl0{};
        expectTrue(e43.caretRectPt(&rl0), "竖排光标的矩形可查");
        expectTrue(e43.moveCaretVertical(1, false), "中日竖排：下一列走得动");
        Rect rl1{};
        expectTrue(e43.caretRectPt(&rl1), "下一列的光标矩形可查");
        expectTrue(rl1.x < rl0.x - 1.0f, "★ 中日竖排的下一列在左边（列自右向左推进）");
        // 走回来：-1 必须回到右边那一列，而且 x 与出发时那一列重合 ——
        //   两个方向都钉住，免得只把其中一个方向的符号弄对。
        expectTrue(e43.moveCaretVertical(-1, false), "中日竖排：上一列走得动");
        Rect rlb{};
        expectTrue(e43.caretRectPt(&rlb) && std::fabs(rlb.x - rl0.x) < 0.01f,
                   "★ 中日竖排的上一列在右边（走回来落在出发那一列）");

        // 同一个位置、同一份文本，只换列序 —— 下一列必须走到另一边。
        b43.wm = WritingMode::VerticalLR();
        loadBlocks(e43, std::vector<BlockSpec>{b43});
        e43.relayout();
        expectTrue(e43.caretVertical(), "蒙文段也是竖排");
        expectTrue(!e43.caretBlockReversed(), "★ 蒙文竖排的列序正向（第一列在最左）");
        e43.setCaret(0, 5);
        Rect lr0{};
        expectTrue(e43.caretRectPt(&lr0), "蒙文竖排光标的矩形可查");
        expectTrue(e43.moveCaretVertical(1, false), "蒙文竖排：下一列走得动");
        Rect lr1{};
        expectTrue(e43.caretRectPt(&lr1), "蒙文下一列的光标矩形可查");
        expectTrue(lr1.x > lr0.x + 1.0f,
                   "★ 蒙文竖排的下一列在右边（与中日竖排相反，左右键的方向必须跟着翻）");
    }

    if (runSection(44))
        std::printf("\n[44] 行内原子：不可断的盒子进流之后，增量排版仍与全量一致\n");
    if (runSection(44)) {
        // 阶段 4 的判据是"断行处不可分割；增量与全量一致"。断行那一半在 shaping_test 里
        //   （它直接看行盒）；这一节管后一半：原子进了引擎的块流之后，改别的段落、改它自己
        //   所在的段落、换一枚更大的原子，增量排版都必须与整篇重排逐字段相同。
        EditorEngine e44;
        expectTrue(e44.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c44;
        c44.zoom = 1.0f;
        c44.page.width_pt = 200.0f;
        c44.page.height_pt = 300.0f;
        c44.page.margins = PageMargins::all(10.0f);
        e44.setConfig(c44);

        // 三段：纯文本 / 汉字 + 图片 / 纯文本。原子夹在中间，于是"改前一段"这条路上
        //   后缀里正好带着一枚原子（后缀复用对原子的取舍因此有判据）。
        auto mkDoc = [&](int px_w, int px_h) {
            int id = 3;
            auto run = [&](const char* text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            ImageSpec image;
            image.px_w = px_w;
            image.px_h = px_h;
            const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                                   {run("甲乙丙丁戊己庚辛"), mark(), run("汉字"),
                                                    doc::makeNode(id++, NodeKind::Image, image, {}),
                                                    mark(), run("子丑寅卯辰巳午未"), mark()});
            const NodeId root_id = id++;
            return doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id);
        };
        e44.setDocument(mkDoc(96, 48), StyleSheet{});
        e44.relayout();
        expectTrue(e44.auditLayout() == 0, "装载之后：增量排版与整篇重排逐字段一致");
        expectTrue(e44.auditCompile() == 0, "装载之后：产物与整树全量编译一致");

        // 原子真的进了块流：对象表里有一枚 Atom，盒宽按原始像素与 DPI 折算。
        const std::vector<BlockSpec>& bl = e44.blocks();
        expectTrue(bl.size() == 3 && bl[1].objects.size() == 1 &&
                       bl[1].objects[0].kind == InlineObjectKind::Atom,
                   "原子进了块流的对象表（第 1 段一枚）");
        expectTrue(bl.size() == 3 && bl[1].objects.size() == 1 && bl[1].objects[0].w_pt > 71.9f &&
                       bl[1].objects[0].w_pt < 72.1f,
                   "盒宽按原始像素与 DPI 折算（96 px / 96 dpi = 72 磅）");

        // 行盒里那枚原子占的宽度就是盒宽 —— 与 shaping_test 同一把尺子，这里量的是
        //   "经引擎装载之后仍然成立"（编译 → 块流 → 排版这条链没有把宽度丢掉）。
        auto atomAdv = [&]() -> float {
            for (const LineBox& lb : e44.layout().lines) {
                for (size_t i = 0; i < lb.clusters.size(); ++i) {
                    if (lb.clusters[i].atom == InlineAtom::None) continue;
                    return (i + 1 < lb.clusters.size())
                               ? (lb.clusters[i + 1].inline_pt - lb.clusters[i].inline_pt)
                               : (lb.inline_end_pt - lb.clusters[i].inline_pt);
                }
            }
            return -1.0f;
        };
        expectNear(atomAdv(), 72.0, 0.05, "装载之后行里那枚原子占的宽度还是盒宽");

        // 改第一段：后缀是第 1、2 段，其中第 1 段带原子 —— 两段都要复用。
        //   这一条同时是"原子不算'端点会动的占位'"的判据：把原子也当成占位来否决复用，
        //   这里会掉到 0 段。
        e44.setCaret(0, 2);
        expectTrue(e44.typeTextAndRender("X"), "在第一段中间敲一个字");
        const EditStats st44 = e44.editStats();
        expectTrue(
            !st44.layout_was_full && st44.reused_paragraphs >= 2,
            "★ 后缀里带原子也照常复用（复用 " + std::to_string(st44.reused_paragraphs) + " 段）");
        expectTrue(e44.auditLayout() == 0, "★ 增量结果与整篇重排一致");
        expectNear(atomAdv(), 72.0, 0.05, "复用之后原子占的宽度没变");

        // 改原子所在的那一段、且改在原子之前：这一整段要重排，结果仍须与全量一致。
        e44.setCaret(1, 1);
        expectTrue(e44.typeTextAndRender("Y"), "在原子所在段的原子之前敲一个字");
        expectTrue(e44.auditLayout() == 0, "★ 原子所在段重排之后仍与整篇重排一致");
        expectNear(atomAdv(), 72.0, 0.05, "重排之后原子占的宽度没变");

        // 换一枚更高的原子（整篇重载）：行盒必须跟着变高，且与整篇重排一致。
        //   192 px / 96 dpi = 144 磅，远高于一行 19.2 磅 —— 这是"盒高真的进了行盒"的
        //   端到端一半（排版层那一半在 shaping_test 的 [8]）。
        e44.setDocument(mkDoc(96, 192), StyleSheet{});
        e44.relayout();
        expectTrue(e44.auditLayout() == 0, "★ 换一枚更高的原子之后仍与整篇重排一致");
        float line_h = 0.0f;
        for (const LineBox& lb : e44.layout().lines) {
            bool has = false;
            for (const LineCluster& c : lb.clusters) {
                if (c.atom != InlineAtom::None) has = true;
            }
            if (has) line_h = lb.block_hi_pt - lb.block_lo_pt;
        }
        expectTrue(line_h >= 144.0f - 0.05f,
                   "144 磅高的原子把行盒撑到 144 以上（实际 " + std::to_string(line_h) + "）");

        // 原子要删得掉。它没有可寻址的内部，删它就是整枚移除 —— 退格看左边那一格、前删看
        //   右边那一格；选区跨过它也一样（那条路原先被"覆盖长度对不上"整条拒掉：文本与段落
        //   标记两条都产不出覆盖它的步骤）。
        {
            auto mkOne = [&]() {
                ImageSpec image;
                image.px_w = 48;
                image.px_h = 48;
                const doc::Ref section = doc::makeNode(
                    2, NodeKind::Section, SectionSpec{},
                    {doc::makeNode(3, NodeKind::TextRun,
                                   doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
                     doc::makeNode(4, NodeKind::Image, image, {}),
                     doc::makeNode(5, NodeKind::TextRun,
                                   doc::RunPayload{utf8ToCodepoints("丙丁"), CharFormat{}, {}}, {}),
                     doc::makeNode(6, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
                return doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section}), 7);
            };
            auto atomCount = [](const EditorEngine& eng) {
                int n = 0;
                for (const BlockSpec& b : eng.blocks()) {
                    for (const InlineObject& o : b.objects) {
                        if (o.kind == InlineObjectKind::Atom) ++n;
                    }
                }
                return n;
            };

            EditorEngine ea;
            expectTrue(ea.init({cjkPath, mnPath, latinPath}), "引擎初始化（删原子）");
            ea.setConfig(c44);
            ea.setDocument(mkOne(), StyleSheet{});
            ea.setEditMode(EditMode::Document);
            ea.relayout();
            expectTrue(atomCount(ea) == 1 && ea.blocks()[0].textUtf8() == "甲乙丙丁",
                       "装载之后：一枚原子夹在四个字中间");

            // 退格：光标在原子右边那一格（cp 2 侧别 1 = 跨过原子），退掉的是原子本身。
            ea.setCaretAtSide(0, 2, 1);
            expectTrue(ea.backspaceAndRender(), "★ 原子右边退格 → 改了文档");
            expectTrue(atomCount(ea) == 0 && ea.blocks()[0].textUtf8() == "甲乙丙丁",
                       "★★ 退格退掉的是那枚原子（四个字一个不少）—— 实际原子 " +
                           std::to_string(atomCount(ea)) + " 枚");
            // 撤销要把原子整枚装回来，光标也回到按键之前那一格（与删控件同一条）。
            expectTrue(ea.undoAndRender(), "撤销");
            expectTrue(atomCount(ea) == 1 && ea.blocks()[0].textUtf8() == "甲乙丙丁",
                       "★★ 撤销把原子装回来（文本仍是一个字不少）");
            expectTrue(ea.caret().offset == 2 && ea.caretSide() == 1 && !ea.hasSelection(),
                       "撤销之后光标回到原子右边那一格且不留选区 —— 实际 " +
                           std::to_string(ea.caret().offset) + "/" +
                           std::to_string(ea.caretSide()));

            // 前删：光标在原子左边那一格（cp 2 侧别 0 = 还没跨过原子）。
            ea.setCaretAtSide(0, 2, 0);
            expectTrue(ea.deleteForwardAndRender(), "★ 原子左边前删 → 改了文档");
            expectTrue(atomCount(ea) == 0 && ea.blocks()[0].textUtf8() == "甲乙丙丁",
                       "★★ 前删删掉的也是那枚原子（四个字一个不少）");

            // 选区跨过原子：文本 + 原子 + 文本一起删（区间 = "乙" + 原子 + "丙"）。
            EditorEngine es;
            expectTrue(es.init({cjkPath, mnPath, latinPath}), "引擎初始化（选区跨原子）");
            es.setConfig(c44);
            es.setDocument(mkOne(), StyleSheet{});
            es.setEditMode(EditMode::Document);
            es.relayout();
            const FlowPos f1 = es.flowPosAtSide(0, 1, 0);
            const FlowPos f3 = es.flowPosAtSide(0, 3, 0);
            expectTrue(f3 - f1 == 3, "区间 [cp 1, cp 3) 在流里占三格（乙 + 原子 + 丙），实际 " +
                                         std::to_string(f3 - f1));
            es.setSelection(Range{f1, f3});
            expectTrue(es.backspaceAndRender(), "★ 选区跨过原子 → 改了文档");
            expectTrue(atomCount(es) == 0 && es.blocks()[0].textUtf8() == "甲丁",
                       "★★ 选区里的文本与原子一起删掉（只剩 甲丁）—— 实际 " +
                           es.blocks()[0].textUtf8() + " / 原子 " + std::to_string(atomCount(es)));
            expectTrue(
                es.undoAndRender() && atomCount(es) == 1 && es.blocks()[0].textUtf8() == "甲乙丙丁",
                "撤销把文本与原子一起装回来");
        }
    }
    if (runSection(45)) std::printf("\n[45] 表格：装载、格内编辑与「有表格不走增量」\n");
    if (runSection(45)) {
        EditorEngine e45;
        expectTrue(e45.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c45;
        c45.zoom = 1.0f;
        c45.page.width_pt = 300.0f;
        c45.page.height_pt = 300.0f;
        c45.page.margins = PageMargins::all(10.0f);
        e45.setConfig(c45);

        int id = 3;
        auto cell = [&](char ch) {
            const std::vector<uint32_t> cps{static_cast<uint32_t>(ch)};
            return doc::makeNode(
                id++, NodeKind::TableCell, CellSpec{},
                {doc::makeNode(id++, NodeKind::TextRun, doc::RunPayload{cps, CharFormat{}, {}}, {}),
                 doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        };
        std::vector<doc::Ref> rows;
        for (int r = 0; r < 2; ++r) {
            rows.push_back(doc::makeNode(
                id++, NodeKind::TableRow, RowSpec{},
                {cell(static_cast<char>('A' + r * 2)), cell(static_cast<char>('A' + r * 2 + 1))}));
        }
        const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows);
        const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table});
        const NodeId root_id = id++;
        e45.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e45.relayout();
        expectTrue(e45.auditLayout() == 0, "装载之后：增量与整篇重排逐字段一致");
        expectTrue(e45.auditCompile() == 0, "装载之后：产物与整树全量编译一致");

        const std::vector<BlockSpec>& bl = e45.blocks();
        expectTrue(bl.size() == 4, "2x2 表编出四段");
        bool path_ok = (bl.size() == 4);
        for (size_t i = 0; i < bl.size() && path_ok; ++i) {
            path_ok = bl[i].cell_path.size() == 1 &&
                      bl[i].cell_path[0].row == static_cast<int>(i / 2) &&
                      bl[i].cell_path[0].cell == static_cast<int>(i % 2);
        }
        expectTrue(path_ok, "四段各带自己的单元格路径（行优先）");

        const LayoutResult& lay = e45.layout();
        expectTrue(lay.lines.size() == 4, "四段各一行");
        expectTrue(lay.placed_tables.size() == 1 && lay.placed_tables[0].placed.size() == 2,
                   "产物里有一张表、两条行几何");
        if (lay.lines.size() == 4 && lay.placed_tables.size() == 1 &&
            lay.placed_tables[0].col_x.size() >= 2) {
            const PlacedTable& pt = lay.placed_tables[0];
            expectNear(lay.lines[1].inline_origin_pt, pt.x + pt.col_x[1] + kTableCellPadXPt, 0.01,
                       "第二格的行内起点落在第二列的起点上（不是内容区左边）");
            expectTrue(lay.lines[2].block_lo_pt > lay.lines[0].block_lo_pt, "第二行在第一行下面");
        }

        // 命中测试：点第二格的字落在第 1 段 —— 格内的定位靠行盒坐标，不需要表格特例。
        //   点在两行之间的空隙上时，落点必须落在相邻两行之一，不许跑到表格之外。
        {
            const LineBox& lb2 = lay.lines[1];
            TextPos got;
            const bool hit =
                e45.hitTestPt(lb2.inline_origin_pt + 1.0f, lb2.block_lo_pt + 2.0f, &got);
            expectTrue(hit && got.para == 1, "点第二格的字命中第 1 段（实际段 " +
                                                 std::to_string(hit ? got.para : -1) + "）");
            TextPos between;
            const float gap_y = (lay.lines[1].block_hi_pt + lay.lines[2].block_lo_pt) * 0.5f;
            const bool hit2 = e45.hitTestPt(lay.lines[1].inline_origin_pt + 1.0f, gap_y, &between);
            expectTrue(hit2 && (between.para == 1 || between.para == 2),
                       "点两行之间的空隙落在相邻两行之一（实际段 " +
                           std::to_string(hit2 ? between.para : -1) + "）");
            // 点第一格的后半（文字早已结束）：势力范围是格宽而不是文字宽，仍该命中第 1 段。
            TextPos tail;
            const float tail_x = lay.lines[1].cell_hi_pt - 4.0f;
            const bool hit3 = e45.hitTestPt(tail_x, lay.lines[1].block_lo_pt + 2.0f, &tail);
            expectTrue(hit3 && tail.para == 1,
                       "点第一格的后半（x=" + std::to_string(static_cast<int>(tail_x)) +
                           "）仍命中第 1 段（实际段 " + std::to_string(hit3 ? tail.para : -1) +
                           "）");
        }

        // 选区：整格被选中时高亮铺满格宽（不是只贴着字）—— 这是"单元格选区"与
        //   "文字选区"的差别。参考 Word：一格整段选中时整格反白。
        {
            // 反白按格的外框铺（含两侧内边距），不是内容区 —— 按内容区铺左右会留两条缝。
            const float cell_w = lay.lines[0].cell_box_hi_pt - lay.lines[0].cell_box_lo_pt;
            const float content_w = lay.lines[0].cell_hi_pt - lay.lines[0].cell_lo_pt;
            e45.selectAll();
            const std::vector<PageRect> rects = e45.selectionRects();
            bool full = false;
            for (const PageRect& rr : rects) {
                if (std::fabs(rr.r_pt.w - cell_w) < 1.0f) full = true;
            }
            expectTrue(cell_w > 10.0f && cell_w > content_w + 1.0f && full,
                       "全选时表格的格被整格高亮（格外框宽 " + std::to_string(cell_w) + "）");
            e45.setCaret(0, 0);
            e45.relayout();
        }

        // 只选一格里的一个字：这是"文字选区"，比格窄、高度是行盒 —— 与"单元格选区"
        //   是两件事。要一个多字的格才构造得出来：单字格里 [0,1) 就是整段。
        {
            EditorEngine e2;
            expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化（文字选区）");
            e2.setConfig(c45);
            int id2 = 3;
            RowSpec row_spec;
            row_spec.height = Length::abs(ptToEmu(40.0));
            row_spec.height_rule = HeightRule::AtLeast;
            const doc::Ref table_cell = doc::makeNode(
                id2++, NodeKind::TableCell, CellSpec{},
                {doc::makeNode(id2++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("甲乙丙"), CharFormat{}, {}}, {}),
                 doc::makeNode(id2++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Ref table_row =
                doc::makeNode(id2++, NodeKind::TableRow, row_spec, {table_cell});
            const doc::Ref table_node =
                doc::makeNode(id2++, NodeKind::Table, TableSpec{}, {table_row});
            const doc::Ref section2 =
                doc::makeNode(id2++, NodeKind::Section, SectionSpec{}, {table_node});
            const NodeId root2 = id2++;
            e2.setDocument(
                doc::adopt(doc::makeNode(root2, NodeKind::Document, {}, {section2}), id2),
                StyleSheet{});
            e2.relayout();
            const LayoutResult& lay2 = e2.layout();
            const float cw2 = lay2.lines[0].cell_hi_pt - lay2.lines[0].cell_lo_pt;
            e2.setCaret(0, 0);
            e2.setFocusAtSide(0, 1, 0);
            const std::vector<PageRect> part = e2.selectionRects();
            bool narrow = false;
            bool short_h = false;
            for (const PageRect& rr : part) {
                if (rr.r_pt.w < cw2 - 10.0f) narrow = true;
                if (rr.r_pt.h < lay2.lines[0].cell_bottom_pt - lay2.lines[0].cell_top_pt - 1.0f) {
                    short_h = true;
                }
            }
            expectTrue(narrow, "只选一格里的一个字：高亮比格窄（文字选区）");
            expectTrue(short_h, "只选一个字：高亮高度是行盒，不是整行");
        }

        // 格内编辑：增量被禁（有表格），文本必须落进第二格那一段。
        e45.setCaret(1, 1);
        expectTrue(e45.typeTextAndRender("Z"), "在表格第二格打字");
        const EditStats st45 = e45.editStats();
        expectTrue(st45.layout_was_full, "有表格时不做增量（整篇重排）");
        expectTrue(e45.auditLayout() == 0, "编辑之后仍与整篇重排逐字段一致");
        // 底图自审：增量重画（行带）得到的底图必须与从零全建逐字节相同。
        //   表格一行有多个行盒，行带的上下边界按"组"算 —— 旧口径取"与下一个行盒的
        //   中线"，取到的是同一行的另一格，带只盖住字的上半，重画之后字被裁成两截
        //   （画面上就是"输入不更新"）。
        //   必须用 auditBaseJson 而不是 auditFrameJson：帧是从底图搬的，底图错了
        //   帧与"整帧重贴"照样一致（实测旧口径下 auditFrameJson 全绿、底图差 3 万像素）。
        const std::string ab45 = e45.auditBaseJson();
        expectTrue(ab45.find("\"diff\":0") != std::string::npos,
                   "编辑之后：行带重画的底图与全建逐字节一致（字没被裁）：" + ab45);
        expectTrue(e45.blocks().size() == 4 && e45.blocks()[1].textUtf8() == "BZ",
                   "打进去的字落在第二格（第 1 段）");
        expectTrue(e45.blocks()[0].textUtf8() == "A" && e45.blocks()[2].textUtf8() == "C" &&
                       e45.blocks()[3].textUtf8() == "D",
                   "其余三格的内容一个字都没动");

        expectTrue(e45.undo(), "撤销");
        e45.relayout();
        expectTrue(e45.blocks().size() == 4 && e45.blocks()[1].textUtf8() == "B",
                   "撤销之后第二格回到一个字");
        expectTrue(e45.auditLayout() == 0, "撤销之后仍与整篇重排逐字段一致");
    }

    if (runSection(77)) std::printf("\n[77] 表格后面那个空段落：点得到、放得下光标\n");
    // 表格后面那个空段落（Word 里那个 ¶）：它要能被点到、能放光标。
    //   形状是"表格 + 只含一枚段落标记的段落"—— 示例文书里表格下面就是这个，
    //   而它与表格最后一行只隔一个内边距，命中容易落到表格上。
    if (runSection(77)) {
        EditorEngine e47;
        expectTrue(e47.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c47;
        c47.zoom = 1.0f;
        c47.page.width_pt = 300.0f;
        c47.page.height_pt = 300.0f;
        c47.page.margins = PageMargins::all(10.0f);
        e47.setConfig(c47);
        int id = 3;
        const doc::Ref cell = doc::makeNode(
            id++, NodeKind::TableCell, CellSpec{},
            {doc::makeNode(id++, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("格"), CharFormat{}, {}}, {}),
             doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const doc::Ref row = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell});
        const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, {row});
        const doc::Ref section = doc::makeNode(
            id++, NodeKind::Section, SectionSpec{},
            {table, doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const NodeId root_id = id++;
        e47.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e47.relayout();
        const LayoutResult& lay = e47.layout();
        expectTrue(lay.lines.size() == 2, "表格一行 + 空段落一行，共两个行盒");
        if (lay.lines.size() == 2) {
            const LineBox& empty = lay.lines[1];
            TextPos got;
            const bool hit = e47.hitTestPt(empty.inline_origin_pt + 2.0f,
                                           (empty.block_lo_pt + empty.block_hi_pt) * 0.5f, &got);
            expectTrue(hit && got.para == 1, "点表格后的空段落命中那一段（实际段 " +
                                                 std::to_string(hit ? got.para : -1) + "）");
        }
    }

    if (runSection(46)) std::printf("\n[46] 表格：格内首尾的删除不删表\n");
    if (runSection(46)) {
        EditorEngine e46;
        expectTrue(e46.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c46;
        c46.zoom = 1.0f;
        c46.page.width_pt = 300.0f;
        c46.page.height_pt = 300.0f;
        c46.page.margins = PageMargins::all(10.0f);
        e46.setConfig(c46);

        // 段 + 表格（1 行 1 格）+ 段。
        auto mkDoc = [&]() {
            int id = 3;
            auto run = [&](const char* text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            const doc::Ref cell =
                doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("格"), mark()});
            const doc::Ref row = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell});
            const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, {row});
            const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                                   {run("前"), mark(), table, run("后"), mark()});
            const NodeId root_id = id++;
            return doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id);
        };
        e46.setDocument(mkDoc(), StyleSheet{});
        e46.relayout();
        expectTrue(e46.blocks().size() == 3, "前、表格、后共三段");

        // 光标在表格第一格的最前面按退格：光标在表内，删的是内容 —— 表格不消失。
        //   这条是用户视角的硬要求：格里的字被删空之后再按一下，不该轮到整张表。
        e46.setCaret(1, 0);
        expectTrue(!e46.backspace(), "首格段首退格：什么都不发生");
        e46.relayout();
        expectTrue(e46.blocks().size() == 3 && e46.blocks()[1].textUtf8() == "格",
                   "表格与格里的字都还在");
        expectTrue(e46.auditTables() == 0, "拒绝之后表格结构自审一致");

        // 前删在表格最后一个块的末尾：同样什么都不发生（表内没有"跨出去"这一步）。
        e46.setCaret(1, 1);
        expectTrue(!e46.deleteForward(), "末段末尾前删：什么都不发生");
        e46.relayout();
        expectTrue(e46.blocks().size() == 3, "前删之后三段不变");

        // 格内容删空之后再按一下：仍然什么都不发生（表还在）。
        e46.setCaret(1, 1);
        expectTrue(e46.backspace(), "在格内容末尾退格：删掉那个字");
        e46.relayout();
        expectTrue(e46.blocks().size() == 3 && e46.blocks()[1].textUtf8().empty(),
                   "只删掉格里的那个字，表格还在");
        e46.relayout();
        expectTrue(!e46.backspace(), "格已经空了，再按退格：什么都不发生");
        e46.relayout();
        expectTrue(e46.blocks().size() == 3, "再按一下之后表格仍然在");
        expectTrue(e46.auditTables() == 0, "删空之后再拒绝，表格结构自审仍一致");

        // 删表要显式：用选区把它整个跨过去（这里从表前那一段的末尾到表后那一段的开头）。
        const FlowPos before_table = e46.flowPosAt(0, 1);
        const FlowPos after_table = e46.flowPosAt(2, 0);
        expectTrue(e46.setSelection(Range{before_table, after_table}), "设一个跨过整表的选区");
        expectTrue(e46.backspace(), "跨过整表的选区退格：执行成功");
        e46.relayout();
        // 这一段区间里还有"前"那一段的结束标记：它被删掉之后前后两段并成一段，
        //   表与格内容一起消失 —— 与"删除一段连续区间"的语义一致。
        expectTrue(e46.blocks().size() == 1 && e46.blocks()[0].textUtf8() == "前后",
                   "删掉之后表与格内容一起没了，前后两段并成一段（实际 " +
                       std::to_string(e46.blocks().size()) + " 块）");
        expectTrue(e46.auditLayout() == 0 && e46.auditCompile() == 0,
                   "删表之后排版与编译两份自审都是 0");
        expectTrue(e46.auditTables() == 0, "表已经不在了，表格结构自审仍是 0");

        // 撤销：表格整张回来（含格里的字）。
        expectTrue(e46.undo(), "撤销");
        e46.relayout();
        expectTrue(e46.blocks().size() == 3 && e46.blocks()[1].textUtf8().empty(),
                   "撤销之后表格回来（格内容停在删空那一刻）");
        expectTrue(e46.auditTables() == 0, "撤销之后表格结构自审一致");
    }

    if (runSection(47)) std::printf("\n[47] 表格：文字选区与单元格选区是两条操作线\n");
    if (runSection(47)) {
        EditorEngine e47;
        expectTrue(e47.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c47;
        c47.zoom = 1.0f;
        c47.page.width_pt = 300.0f;
        c47.page.height_pt = 300.0f;
        c47.page.margins = PageMargins::all(10.0f);
        e47.setConfig(c47);

        int id = 3;
        // 显式行高（AtLeast）撑出一截空白：整格选区要把这一截算进去，文字选区不该算。
        RowSpec row_spec;
        row_spec.height = Length::abs(ptToEmu(60.0));
        row_spec.height_rule = HeightRule::AtLeast;
        auto cell_run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto cell_mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const doc::Ref cell =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                          {cell_run("甲"), cell_mark(), cell_run("乙"), cell_mark()});
        const doc::Ref row = doc::makeNode(id++, NodeKind::TableRow, row_spec, {cell});
        const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, {row});
        const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table});
        const NodeId root_id = id++;
        e47.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e47.relayout();
        const LayoutResult& lay = e47.layout();
        expectTrue(lay.lines.size() == 2, "格内两段编出两行");
        if (lay.lines.size() == 2) {
            const LineBox& l0 = lay.lines[0];
            const float ymid = (l0.block_lo_pt + l0.block_hi_pt) * 0.5f;
            TextPos got;
            int band_para = -1;
            // 边距带：格的外框到文字左沿之间那一条。给的是段号 —— 一格的身份是它占的
            //   块区间，不是它的流区间（空格的内容长度是 0，流区间分不出是哪一格）。
            const bool hit_band = e47.hitTestPt(l0.cell_lo_pt - 1.0f, ymid, &got, nullptr, nullptr,
                                                nullptr, &band_para);
            expectTrue(hit_band && band_para == l0.paragraph,
                       "点在格左内边距上：命中给出这一格的段号");
            // 反向一半：点在同一行的字上不给整格 —— 那是文字光标，不是选格。
            int word_para = -1;
            e47.hitTestPt(l0.inline_origin_pt + 2.0f, ymid, &got, nullptr, nullptr, nullptr,
                          &word_para);
            expectTrue(word_para < 0, "点在同一行的字上：不触发整格");

            // 三击那条路：点在哪格里都算。
            expectTrue(e47.selectCellAtPt(l0.inline_origin_pt + 2.0f, ymid),
                       "三击：点在字上也选中整格");
            expectTrue(e47.selectionIsCell(), "标记是单元格选区");
            // 一格的内容区间：首段段首到末段段末（这一格两段）。
            FlowPos clo = e47.flowPosAt(l0.paragraph, 0);
            FlowPos chi = e47.flowPosAt(lay.lines[1].paragraph, 1);
            expectTrue(chi > clo, "这一格的内容区间取得到");
            const Range sr = e47.selection();
            expectTrue(sr.anchor == clo && sr.focus == chi, "整格选区的两端就是这一格的内容区间");
            // 插入点钉在格末（格内第二段的末尾），不是三击落下的那个字的位置。
            const TextPos c47 = e47.caret();
            expectTrue(c47.para == lay.lines[1].paragraph && c47.offset == 1,
                       "整格选区的插入点在格末（第二段末尾），不跟着点击位置走（实际 段 " +
                           std::to_string(c47.para) + " 偏移 " + std::to_string(c47.offset) + "）");
            const std::vector<PageRect> rects = e47.selectionRects();
            expectTrue(rects.size() == 1, "格内两行只画一个整格矩形（不叠着画两遍）");
            if (rects.size() == 1) {
                expectNear(rects[0].r_pt.x, l0.cell_box_lo_pt, 0.01, "整格选区左边界贴格外框");
                expectNear(rects[0].r_pt.w, l0.cell_box_hi_pt - l0.cell_box_lo_pt, 0.01,
                           "整格选区宽度是整格的外框（含两侧内边距，不是文字宽）");
                expectTrue(l0.cell_box_lo_pt < l0.cell_lo_pt - 0.01f &&
                               l0.cell_box_hi_pt > l0.cell_hi_pt + 0.01f,
                           "外框比内容区宽（否则上一条测不出内边距那两侧）");
                expectNear(rects[0].r_pt.h, l0.cell_bottom_pt - l0.cell_top_pt, 0.01,
                           "整格选区高度含显式行高多出来的空白");
                expectTrue(
                    l0.cell_bottom_pt - l0.cell_top_pt > (l0.block_hi_pt - l0.block_lo_pt) + 10.0f,
                    "显式行高确实比文字那一带高（否则上一条测不出差别）");
            }

            // 反向一半：同一对区间按文字选区设 —— 这正是"把文字一路选到最后一个字"。
            //   旧实现靠几何猜（首尾都覆盖到了 ⇒ 整格），这一条必红。
            expectTrue(e47.setSelection(Range{clo, chi}), "同一对区间按文字选区设");
            expectTrue(!e47.selectionIsCell(), "按文字选：标记不是单元格选区");
            const std::vector<PageRect> tr = e47.selectionRects();
            float maxw = 0.0f;
            for (const PageRect& r : tr) maxw = std::max(maxw, r.r_pt.w);
            expectTrue(tr.size() == 2,
                       "文字选区按行给两个矩形（实际 " + std::to_string(tr.size()) + " 个）");
            expectTrue(maxw < (l0.cell_box_hi_pt - l0.cell_box_lo_pt) - 1.0f,
                       "文字选区只盖字，不涨成整格（选到最后一个字也一样）");
        }
    }

    if (runSection(48)) std::printf("\n[48] 表格：拖选跨过两格就进单元格选区\n");
    if (runSection(48)) {
        EditorEngine e48;
        expectTrue(e48.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c48;
        c48.zoom = 1.0f;
        c48.page.width_pt = 320.0f;
        c48.page.height_pt = 320.0f;
        c48.page.margins = PageMargins::all(10.0f);
        e48.setConfig(c48);

        // 2x2：一个格一个字。跨格拖选的判据用得到"两格之间还夹着别的格"这一条。
        int id = 3;
        auto text_run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const char* cell_text[2][2] = {{"甲", "乙"}, {"丙", "丁"}};
        std::vector<doc::Ref> rows;
        for (int r = 0; r < 2; ++r) {
            std::vector<doc::Ref> cells;
            for (int ci = 0; ci < 2; ++ci) {
                cells.push_back(doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                              {text_run(cell_text[r][ci]), mark()}));
            }
            rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
        }
        const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows);
        // 表后接一段正文：用来验证"拖出表格就退回文字选区"，所以这一段必须真的存在。
        const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                               {table, text_run("尾巴"), mark()});
        const NodeId root_id = id++;
        e48.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e48.relayout();
        const LayoutResult& lay = e48.layout();
        expectTrue(lay.lines.size() >= 4,
                   "2x2 表编出四行（实际 " + std::to_string(lay.lines.size()) + " 行）");
        if (lay.lines.size() >= 4) {
            // 每格取两样东西：字上的落点（模拟拖选的起手/收手）与格内容的流区间（判据）。
            struct CellProbe {
                TextPos pos{};
                int side = 0;
                int para = -1;  // 边距带命中给出的段号（= 这一格的段）
                FlowPos lo = -1;
                FlowPos hi = -1;
                float box_lo = 0.0f;  // 格外框（反白用它，含两侧内边距）
                float box_hi = 0.0f;
                float cell_lo = 0.0f;  // 格内容区（命中用）
                float cell_hi = 0.0f;
                float cell_top = 0.0f;
                float cell_bottom = 0.0f;
            };
            CellProbe cp[4];
            bool probed = true;
            for (int k = 0; k < 4; ++k) {
                const LineBox& lb = lay.lines[static_cast<size_t>(k)];
                const float ymid = (lb.block_lo_pt + lb.block_hi_pt) * 0.5f;
                TextPos p{};
                int side = 0;
                probed = probed && e48.hitTestPt(lb.inline_origin_pt + 2.0f, ymid, &p, nullptr,
                                                 &side, nullptr, nullptr);
                cp[k].pos = p;
                cp[k].side = side;
                int band = -1;
                probed = probed && e48.hitTestPt(lb.cell_lo_pt - 1.0f, ymid, &p, nullptr, nullptr,
                                                 nullptr, &band);
                cp[k].para = band;
                // 每格两个字、一个块：区间就是这一段的段首到段末。
                if (band >= 0) {
                    cp[k].lo = e48.flowPosAt(band, 0);
                    cp[k].hi = e48.flowPosAt(band, 1);
                }
                cp[k].box_lo = lb.cell_box_lo_pt;
                cp[k].box_hi = lb.cell_box_hi_pt;
                cp[k].cell_lo = lb.cell_lo_pt;
                cp[k].cell_hi = lb.cell_hi_pt;
                cp[k].cell_top = lb.cell_top_pt;
                cp[k].cell_bottom = lb.cell_bottom_pt;
            }
            expectTrue(probed && cp[0].lo >= 0 && cp[3].lo > cp[0].lo && cp[0].para == 0 &&
                           cp[1].para == 1 && cp[2].para == 2 && cp[3].para == 3,
                       "四格的段号与流区间都取得出来（否则下面几条测不出跨格）");

            // 拖选的一条（宿主按下 → 移动到另一格）：起点在第一格的字上，落点在第四格的字上。
            e48.setCaretAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            e48.setFocusAtSideLine(cp[3].pos.para, cp[3].pos.offset, cp[3].side, 0);
            expectTrue(e48.selectionIsCell(), "跨两格拖选：进入单元格选区");
            const Range sr = e48.selection();
            expectTrue(sr.anchor == cp[0].lo && sr.focus == cp[3].hi,
                       "跨格选区的两端吸附到首格首与末格末（不是按下/松开的字符位置）");
            const std::vector<PageRect> rects = e48.selectionRects();
            expectTrue(rects.size() == 4,
                       "跨 2x2 画四个整格矩形（实际 " + std::to_string(rects.size()) + " 个）");
            // 每一份都要是"整格"：宽到列左右边界、高到行上下边界。四份里有一份是文字矩形
            //   就说明只把两端的格放大了，中间的格还是按文字画。
            bool all_cells = rects.size() == 4;
            for (size_t i = 0; i < rects.size() && all_cells; ++i) {
                const Rect& r = rects[i].r_pt;
                bool matched = false;
                for (int k = 0; k < 4; ++k) {
                    if (std::abs(r.x - cp[k].box_lo) < 0.01f &&
                        std::abs(r.w - (cp[k].box_hi - cp[k].box_lo)) < 0.01f &&
                        std::abs(r.y - cp[k].cell_top) < 0.01f &&
                        std::abs(r.h - (cp[k].cell_bottom - cp[k].cell_top)) < 0.01f) {
                        matched = true;
                        break;
                    }
                }
                all_cells = all_cells && matched;
            }
            expectTrue(all_cells, "四份矩形都是整格的外框（含两侧内边距与行高空白）");

            // 插入点：整格选区时它钉在焦点那一格的末尾，不跟着鼠标在格内跑。
            //   焦点格 = 用户最后落到的那一格（这里是第四格）。跟着 focus 走的话，鼠标在格内
            //   每挪一个字它就挪一次，画面上就是光标跟着鼠标跑。
            const TextPos c_end = e48.caret();
            expectTrue(c_end.para == cp[3].pos.para && c_end.offset == 1,
                       "整格选区的插入点在焦点那一格的末尾（实际 段 " + std::to_string(c_end.para) +
                           " 偏移 " + std::to_string(c_end.offset) + "）");
            // 鼠标在同一格里挪（焦点从字中间挪到格首）：选区不变，插入点也不许动。
            e48.setFocusAtSideLine(cp[3].pos.para, 0, 0, 0);
            const TextPos c_same = e48.caret();
            expectTrue(
                e48.selectionIsCell() && c_same.para == c_end.para && c_same.offset == c_end.offset,
                "同一格内移动焦点：插入点仍钉在格末");

            // 反方向拖（从最后一格拖回第一格）：焦点在第一格，插入点在第一格的末尾 ——
            //   不是"选区的起点"，也不是"鼠标落下的那个字"。
            e48.setCaretAtSideLine(cp[3].pos.para, cp[3].pos.offset, cp[3].side, 0);
            e48.setFocusAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            expectTrue(e48.selectionIsCell(), "反向跨格拖选：仍是单元格选区");
            const TextPos c_back = e48.caret();
            expectTrue(c_back.para == cp[0].pos.para && c_back.offset == 1,
                       "反向拖选：插入点仍在焦点那一格（第一格）的末尾");
            // 复位成"从第一格拖到第四格"：下面那条反向一半接着用它。
            e48.setCaretAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            e48.setFocusAtSideLine(cp[3].pos.para, cp[3].pos.offset, cp[3].side, 0);

            // 反向一半：拖回起手那一格 —— 退回文字选区，且只盖字。
            e48.setFocusAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            expectTrue(!e48.selectionIsCell(), "拖回同一格：退回文字选区");
            const std::vector<PageRect> tr = e48.selectionRects();
            bool any_wide = false;
            for (const PageRect& r : tr) {
                if (r.r_pt.w >= (cp[0].cell_hi - cp[0].cell_lo) - 1.0f) any_wide = true;
            }
            expectTrue(!any_wide, "退回文字选区之后没有整格宽度的矩形");

            // 拖到表格后面的正文（不在任何格里）：同样是文字选区 —— 不因为"起手在格内"
            //   就把整行正文都按整格铺。
            e48.setCaretAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            const int last_para = static_cast<int>(lay.lines.back().paragraph);
            e48.setFocusAtSideLine(last_para, 0, 0, 0);
            expectTrue(!e48.selectionIsCell(), "拖出表格：不是单元格选区");

            // 跨格选区之后删除：盖满整张表就删整张表 —— 这条防的是两件事：
            //   一、"选区吸附到格边界之后，删除区间跨过了格与格的段落标记，把表结构删坏"；
            //   二、反过来删得太少：只清内容会留下一张空表，用户按的是删除，得到的是"表还在"。
            e48.setCaretAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            e48.setFocusAtSideLine(cp[3].pos.para, cp[3].pos.offset, cp[3].side, 0);
            expectTrue(e48.selectionIsCell(), "删除前：跨格选区成立");
            expectTrue(e48.blocks().size() == 5, "前提：四格加尾段是五个块（实际 " +
                                                     std::to_string(e48.blocks().size()) + "）");
            expectTrue(e48.backspace(), "跨格选区退格：执行成功");
            e48.relayout();
            expectTrue(e48.auditLayout() == 0, "跨格删除之后排版审计一致");
            expectTrue(e48.blocks().size() == 1 && e48.blocks()[0].textUtf8() == "尾巴",
                       "★ 盖满整张表：表框一起删掉了，只剩表后那一段（实际 " +
                           std::to_string(e48.blocks().size()) + " 个块）");
            expectTrue(e48.auditTables() == 0, "删整表之后表格自审一致");

            // 光标落在被删表的那一处（表后那一段的段首）。整格选区的起点在表格内部，
            //   那个流位置随表一起消失、被后面的内容顶上来：拿它当落点，光标就落在段中间。
            const TextPos c_del = e48.caret();
            expectTrue(c_del.para == 0 && c_del.offset == 0,
                       "★ 删完光标停在表原来的位置（实际 段 " + std::to_string(c_del.para) +
                           " 偏移 " + std::to_string(c_del.offset) + "）");

            // 一步撤销把整张表恢复：删是与不是同一个事务的两半。
            expectTrue(e48.undo(), "撤销");
            e48.relayout();
            expectTrue(
                e48.blocks().size() == 5 && e48.blocks()[0].textUtf8() == "甲",
                "★ 一步撤销把整张表恢复（实际 " + std::to_string(e48.blocks().size()) + " 个块）");

            // 反向一半：只盖住前两格（没盖满整张表）—— 表留着，只有那两格的内容清空。
            //   判据一并看"没选中的格一个字都不许动"。
            e48.setCaretAtSideLine(cp[0].pos.para, cp[0].pos.offset, cp[0].side, 0);
            e48.setFocusAtSideLine(cp[1].pos.para, cp[1].pos.offset, cp[1].side, 0);
            expectTrue(e48.selectionIsCell(), "只选两格：单元格选区照样成立");
            expectTrue(e48.backspace(), "只选两格退格：执行成功");
            e48.relayout();
            expectTrue(e48.blocks().size() == 5, "★ 没盖满整张表：表还在（块数仍为五）");
            expectTrue(e48.blocks()[0].textUtf8().empty() && e48.blocks()[1].textUtf8().empty(),
                       "★ 选中的两格内容清空");
            expectTrue(e48.blocks()[2].textUtf8() == "丙" && e48.blocks()[3].textUtf8() == "丁",
                       "★ 没选中的两格一个字都没动");
            expectTrue(e48.auditTables() == 0, "逐格清空之后表格自审一致");
        }
    }

    if (runSection(49)) std::printf("\n[49] 表格：矩形选区与跨格合并的守门\n");
    if (runSection(49)) {
        EditorEngine e49;
        expectTrue(e49.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c49;
        c49.zoom = 1.0f;
        c49.page.width_pt = 360.0f;
        c49.page.height_pt = 480.0f;
        c49.page.margins = PageMargins::all(10.0f);
        e49.setConfig(c49);

        // 3x3，每格两个字、一个块。行列都要小：跨行、跨列、斜向各一条判据。
        int id = 3;
        auto text_run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const char* txt[3][3] = {{"a1", "a2", "a3"}, {"b1", "b2", "b3"}, {"c1", "c2", "c3"}};
        std::vector<doc::Ref> rows;
        for (int r = 0; r < 3; ++r) {
            std::vector<doc::Ref> cells;
            for (int ci = 0; ci < 3; ++ci) {
                cells.push_back(doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                              {text_run(txt[r][ci]), mark()}));
            }
            rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
        }
        const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows);
        const doc::Ref section =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table, text_run("尾"), mark()});
        const NodeId root_id = id++;
        e49.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e49.relayout();
        const LayoutResult& lay49 = e49.layout();
        expectTrue(lay49.lines.size() == 10, "3x3 表编出九行，加表后正文一行（实际 " +
                                                 std::to_string(lay49.lines.size()) + " 行）");
        // 结构自审与排版自审是两把尺子：前者查块与格两份记录，后者查增量与全量。
        expectTrue(e49.auditTables() == 0, "装载后：表格结构自审一致");

        // 每一格的身份：落点（拖选起手）、整格流区间、几何。
        struct Cell49 {
            TextPos pos{};
            int side = 0;
            FlowPos lo = -1;
            FlowPos hi = -1;
        };
        Cell49 cc[9];
        bool probe_ok = lay49.lines.size() >= 9;
        for (int k = 0; k < 9 && probe_ok; ++k) {
            const LineBox& lb = lay49.lines[static_cast<size_t>(k)];
            const float ymid = (lb.block_lo_pt + lb.block_hi_pt) * 0.5f;
            TextPos p{};
            int side = 0;
            probe_ok = e49.hitTestPt(lb.inline_origin_pt + 1.0f, ymid, &p, nullptr, &side, nullptr);
            cc[k].pos = p;
            cc[k].side = side;
            int band = -1;
            probe_ok = probe_ok && e49.hitTestPt(lb.cell_lo_pt - 1.0f, ymid, &p, nullptr, nullptr,
                                                 nullptr, &band);
            if (probe_ok && band >= 0) {
                cc[k].lo = e49.flowPosAt(band, 0);
                cc[k].hi = e49.flowPosAt(band, 2);
            }
        }
        expectTrue(probe_ok && cc[8].lo > cc[0].lo, "九格的落点与整格流区间都取得出来");

        // 选区矩形按几何认格（不靠顺序）：返回命中的格下标，升序。
        const auto pickedCells = [&](const std::vector<PageRect>& rects) {
            std::vector<int> hits;
            for (const PageRect& pr : rects) {
                for (int k = 0; k < 9; ++k) {
                    const LineBox& lb = lay49.lines[static_cast<size_t>(k)];
                    const float w = lb.cell_box_hi_pt - lb.cell_box_lo_pt;
                    const float h = lb.cell_bottom_pt - lb.cell_top_pt;
                    if (std::abs(pr.r_pt.x - lb.cell_box_lo_pt) < 0.01f &&
                        std::abs(pr.r_pt.y - lb.cell_top_pt) < 0.01f &&
                        std::abs(pr.r_pt.w - w) < 0.01f && std::abs(pr.r_pt.h - h) < 0.01f) {
                        hits.push_back(k);
                        break;
                    }
                }
            }
            std::sort(hits.begin(), hits.end());
            return hits;
        };
        const auto sameSet = [](const std::vector<int>& got, std::vector<int> want) {
            std::sort(want.begin(), want.end());
            return got == want;
        };
        const auto drag = [&](int from, int to) {
            e49.setCaretAtSideLine(cc[from].pos.para, cc[from].pos.offset, cc[from].side, 0);
            e49.setFocusAtSideLine(cc[to].pos.para, cc[to].pos.offset, cc[to].side, 0);
        };

        // 斜向跨行跨列：只选两行两列围出来的那四格。旧实现按文档顺序取包络，第二行
        //   的前一格与第一行的第一格会一起反白（＝一跨行就整行）。
        drag(1, 5);
        expectTrue(e49.selectionIsCell(), "从第一行第二格拖到第二行第三格：单元格选区");
        std::vector<int> got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {1, 2, 4, 5}),
                   "斜向拖选只反白矩形里的四格（实际 " + std::to_string(got.size()) + " 份）");

        // 反向同一块矩形：从右下拖到左上，选出来的还是那四格。
        //   锚点被改写成"包络起点"的话，反向拖到一半就长不回去了（这是"从下往上、
        //   从右往左选不中"的成因）。
        drag(5, 1);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {1, 2, 4, 5}), "反向斜拖（从第五格拖回第二格）：还是那四格（实际 " +
                                                   std::to_string(got.size()) + " 份）");
        // 反向逐行拖：从第三行第一列拖回第一行第一列。
        drag(6, 0);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {0, 3, 6}), "反向竖拖（从第七格拖回第一格）：一列三行（实际 " +
                                                std::to_string(got.size()) + " 份）");

        // 逐步反向拖：按下只落一次锚点，之后每一步只搬焦点。这才是真实指针路径
        //   （上面那个助手每次都重设锚点，正好把"锚点被改写成包络"这类错盖住）。
        e49.setCaretAtSideLine(cc[6].pos.para, cc[6].pos.offset, cc[6].side, 0);
        e49.setFocusAtSideLine(cc[3].pos.para, cc[3].pos.offset, cc[3].side, 0);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {3, 6}),
                   "逐步反向拖第一段（第三行第一列拖到第二行第一列）：两格反白（实际 " +
                       std::to_string(got.size()) + " 份）");
        e49.setFocusAtSideLine(cc[0].pos.para, cc[0].pos.offset, cc[0].side, 0);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {0, 3, 6}), "逐步反向拖到底（拖回第一格）：仍是那一列三格（实际 " +
                                                std::to_string(got.size()) + " 份）");
        expectTrue(e49.selectionIsCell(), "逐步反向拖到底：仍是单元格选区");

        // 同列跨三行：一列三格，不是三整行。
        drag(2, 8);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {2, 5, 8}),
                   "同列跨三行只反白那一列（实际 " + std::to_string(got.size()) + " 份）");

        // 同一行：两格（行内那一支不能被矩形判据带坏）。
        drag(0, 1);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {0, 1}),
                   "同一行拖选两格（实际 " + std::to_string(got.size()) + " 份）");

        // 反向斜拖：包络还是那个矩形（方向不影响选了哪几格），插入点在焦点格末。
        drag(5, 1);
        got = pickedCells(e49.selectionRects());
        expectTrue(sameSet(got, {1, 2, 4, 5}), "反向斜拖：矩形不变");
        const TextPos caret49 = e49.caret();
        expectTrue(caret49.para == cc[1].pos.para && caret49.offset == 2,
                   "反向斜拖：插入点在第一格末尾（实际 段 " + std::to_string(caret49.para) +
                       " 偏移 " + std::to_string(caret49.offset) + "）");

        // 删除只清矩形里的四格：没选中的格一个字都不许动。
        drag(1, 5);
        expectTrue(e49.backspace(), "矩形选区退格：执行成功");
        e49.relayout();
        expectTrue(e49.blocks().size() == 10, "删完九格加上正文仍是十个块（每格保住自己的段落）");
        const char* want_after[9] = {"a1", "", "", "b1", "", "", "c1", "c2", "c3"};
        std::string bad;
        for (int k = 0; k < 9; ++k) {
            const std::string got_text =
                codepointsToUtf8(e49.blocks()[static_cast<size_t>(k)].cp_text);
            if (got_text != want_after[k]) {
                bad += " 第" + std::to_string(k) + "格是'" + got_text + "'";
            }
        }
        expectTrue(bad.empty(), "只有矩形里的四格被清空（不符：" + bad + "）");
        expectTrue(e49.auditLayout() == 0, "删完排版自审一致");
        expectTrue(e49.auditTables() == 0, "删完表格结构自审一致");
    }

    if (runSection(50)) std::printf("\n[50] 表格：段落合并不跨格\n");
    if (runSection(50)) {
        EditorEngine e50;
        expectTrue(e50.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c50;
        c50.zoom = 1.0f;
        c50.page.width_pt = 320.0f;
        c50.page.height_pt = 400.0f;
        c50.page.margins = PageMargins::all(10.0f);
        e50.setConfig(c50);

        // 一格两段、另两格各一段：格内合并该发生，跨格合并不该发生。
        int id = 3;
        auto run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const doc::Ref c0 = doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                          {run("甲"), mark(), run("乙"), mark()});
        const doc::Ref c1 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("丙"), mark()});
        const doc::Ref c2 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("丁"), mark()});
        const doc::Ref table =
            doc::makeNode(id++, NodeKind::Table, TableSpec{},
                          {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c0, c1, c2})});
        const doc::Ref section =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table, run("尾"), mark()});
        const NodeId root_id = id++;
        e50.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e50.relayout();
        expectTrue(e50.blocks().size() == 5, "四段格内容加表后正文共五块（实际 " +
                                                 std::to_string(e50.blocks().size()) + " 块）");
        expectTrue(e50.auditTables() == 0, "装载后表格结构自审一致");

        // 第二格段首退格：上一段在第一格，中间隔着单元格边界 —— 必须拒绝。
        const LayoutResult& lay50 = e50.layout();
        const LineBox& lb1 = lay50.lines[2];
        TextPos p{};
        int side = 0;
        expectTrue(
            e50.hitTestPt(lb1.inline_origin_pt + 0.5f, (lb1.block_lo_pt + lb1.block_hi_pt) * 0.5f,
                          &p, nullptr, &side, nullptr),
            "第二格的行盒取得出来");
        e50.setCaretAtSideLine(p.para, 0, side, 0);
        const TextPos before = e50.caret();
        expectTrue(!e50.backspace(), "第二格段首退格：拒绝（不跨格合并）");
        e50.relayout();
        expectTrue(e50.blocks().size() == 5, "拒绝之后块数不变");
        expectTrue(e50.caret().para == before.para && e50.caret().offset == before.offset,
                   "拒绝之后光标不动（不跳到上一格末尾）");
        expectTrue(e50.auditTables() == 0, "拒绝之后表格结构自审一致");

        // 格内合并仍然要做：第一格第二段的段首退格。
        const LineBox& lb0b = e50.layout().lines[1];  // 第一格两行里的第二行
        TextPos q{};
        int qside = 0;
        expectTrue(e50.hitTestPt(lb0b.inline_origin_pt + 0.5f,
                                 (lb0b.block_lo_pt + lb0b.block_hi_pt) * 0.5f, &q, nullptr, &qside,
                                 nullptr),
                   "第一格第二行取得出来");
        e50.setCaretAtSideLine(q.para, 0, qside, 0);
        expectTrue(e50.backspace(), "第一格内段首退格：合并成功");
        e50.relayout();
        expectTrue(e50.blocks().size() == 4,
                   "格内合并后块数少一块（实际 " + std::to_string(e50.blocks().size()) + " 块）");
        const std::string merged = codepointsToUtf8(e50.blocks()[0].cp_text);
        expectTrue(merged == "甲乙", "合并后第一格是甲乙（实际 '" + merged + "'）");
        expectTrue(e50.auditTables() == 0, "格内合并后表格结构自审一致");

        // 第三格段首（第二格之后）：仍要拒绝，且不留半截结构。这跟"表格第一个块"那条
        //   边界规则无关，所以能单独测到跨格合并的守门。
        const LineBox& lb2 = e50.layout().lines[2];
        TextPos r{};
        int rside = 0;
        expectTrue(
            e50.hitTestPt(lb2.inline_origin_pt + 0.5f, (lb2.block_lo_pt + lb2.block_hi_pt) * 0.5f,
                          &r, nullptr, &rside, nullptr),
            "第三格的行盒取得出来");
        e50.setCaretAtSideLine(r.para, 0, rside, 0);
        expectTrue(!e50.backspace(), "第三格段首退格：拒绝");
        e50.relayout();
        expectTrue(e50.blocks().size() == 4, "第二次拒绝之后块数仍不变");
        expectTrue(e50.auditTables() == 0, "第二次拒绝之后结构自审一致");
    }

    if (runSection(51)) std::printf("\n[51] 表格：全选删除与格内文字选区\n");
    if (runSection(51)) {
        EditorEngine e51;
        expectTrue(e51.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c51;
        c51.zoom = 1.0f;
        c51.page.width_pt = 320.0f;
        c51.page.height_pt = 400.0f;
        c51.page.margins = PageMargins::all(10.0f);
        e51.setConfig(c51);

        // 正文 + 一行两格的表 + 正文。前后都有正文，全选才测得到"表外的空隙"。
        int id = 3;
        auto run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const doc::Ref cell0 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("甲"), mark()});
        const doc::Ref cell1 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("乙"), mark()});
        const doc::Ref table =
            doc::makeNode(id++, NodeKind::Table, TableSpec{},
                          {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell0, cell1})});
        const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                               {run("头"), mark(), table, run("尾"), mark()});
        const NodeId root_id = id++;
        e51.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e51.relayout();
        expectTrue(e51.blocks().size() == 4,
                   "头、甲、乙、尾共四块（实际 " + std::to_string(e51.blocks().size()) + " 块）");
        expectTrue(e51.auditTables() == 0, "装载后表格结构自审一致");

        // 全选删除：表是块级容器，它的结构位不在任何可删区间里，所以整张表要按"删节点"走，
        //   表外的正文仍按区间删。旧实现里这条会被安全阀整条拒掉（按 Del 毫无反应）。
        expectTrue(e51.selectAll(), "全选");
        expectTrue(e51.backspace(), "全选退格：执行成功");
        e51.relayout();
        expectTrue(e51.blocks().size() == 1,
                   "全选删完只剩一块（实际 " + std::to_string(e51.blocks().size()) + " 块）");
        expectTrue(e51.blocks()[0].cp_text.empty(), "剩下的那一块是空段落");
        expectTrue(e51.auditTables() == 0, "全选删完表格结构自审一致（表已整张消失）");
        expectTrue(e51.auditLayout() == 0, "全选删完排版自审一致");

        // 撤销：表与正文都要回来（删节点这一步与区间删除在同一批步骤里）。
        expectTrue(e51.canUndo(), "删完可以撤销");
        expectTrue(e51.undo(), "撤销");
        e51.relayout();
        expectTrue(e51.blocks().size() == 4,
                   "撤销之后四块都回来（实际 " + std::to_string(e51.blocks().size()) + " 块）");
        const std::string back0 = codepointsToUtf8(e51.blocks()[0].cp_text);
        const std::string back1 = codepointsToUtf8(e51.blocks()[1].cp_text);
        expectTrue(back0 == "头" && back1 == "甲",
                   "撤销之后正文与第一格内容都在（实际 '" + back0 + "' / '" + back1 + "'）");
        expectTrue(e51.auditTables() == 0, "撤销之后表格结构自审一致");

        // 格内文字选区（只盖一格里的字）：按区间删，格与表都还在。
        const FlowPos a = e51.flowPosAt(1, 0);
        const FlowPos b = e51.flowPosAt(1, 1);
        expectTrue(e51.setSelection(Range{a, b}), "设一个格内文字选区");
        expectTrue(!e51.selectionIsCell(), "它是文字选区，不是单元格选区");
        expectTrue(e51.backspace(), "格内文字选区退格：执行成功");
        e51.relayout();
        expectTrue(e51.blocks().size() == 4, "格内删字之后块数不变");
        expectTrue(e51.blocks()[1].cp_text.empty(), "第一格的字被删掉（格还在）");
        expectTrue(e51.auditTables() == 0, "格内删字之后表格结构自审一致");

        // 整张表的文字都选上了，但两头都落在表内：删表比用户选的多，拒绝。
        //   宿主那一路拖选到这里已经是单元格选区，所以只有程序化设区间才会碰到这一形态。
        const FlowPos e = e51.flowPosAt(1, 0);
        const FlowPos f = e51.flowPosAt(2, 1);
        expectTrue(e51.setSelection(Range{e, f}), "覆盖整张表的文字选区");
        expectTrue(!e51.backspace(), "覆盖整表但两头都在表内：拒绝");
        e51.relayout();
        expectTrue(e51.blocks().size() == 4 && e51.blocks()[2].textUtf8() == "乙" &&
                       e51.blocks()[1].textUtf8().empty(),
                   "拒绝之后一格不少、第二格的字原样");
        expectTrue(e51.auditTables() == 0, "拒绝之后表格结构自审一致");

        // 反向一半：跨格的部分文字选区没有"局部删结构"的语义，必须整条拒绝，
        //   而且一格都不许删（宁可不动，也不要删掉半格）。
        e51.undo();
        e51.relayout();
        const FlowPos c = e51.flowPosAt(1, 0);
        const FlowPos d = e51.flowPosAt(2, 0);
        expectTrue(e51.setSelection(Range{c, d}), "跨两格的文字选区");
        expectTrue(!e51.backspace(), "跨格文字选区退格：拒绝");
        e51.relayout();
        expectTrue(e51.blocks().size() == 4 && !e51.blocks()[1].cp_text.empty() &&
                       !e51.blocks()[2].cp_text.empty(),
                   "拒绝之后两格的字都还在");
        expectTrue(e51.auditTables() == 0, "拒绝之后表格结构自审一致");

        // 全选之后打字：删除与插入是同一个事务，撤销一次回到全选之前。
        expectTrue(e51.selectAll(), "再全选");
        expectTrue(e51.typeTextUtf8("X"), "全选之后打字");
        e51.relayout();
        expectTrue(e51.blocks().size() == 1, "打字之后只剩一块");
        const std::string typed = e51.blocks()[0].cp_text.empty()
                                      ? std::string()
                                      : codepointsToUtf8(e51.blocks()[0].cp_text);
        expectTrue(typed == "X", "剩下的是刚打的字（实际 '" + typed + "'）");
        expectTrue(e51.auditTables() == 0, "全选打字之后表格结构自审一致");
    }

    if (runSection(52)) std::printf("\n[52] 表格：内容为空的格也能被选中\n");
    if (runSection(52)) {
        EditorEngine e52;
        expectTrue(e52.init({cjkPath, mnPath, latinPath}), "引擎初始化");
        EngineConfig c52;
        c52.zoom = 1.0f;
        c52.page.width_pt = 320.0f;
        c52.page.height_pt = 400.0f;
        c52.page.margins = PageMargins::all(10.0f);
        e52.setConfig(c52);

        // 段 + 一格两格的表 + 段。第一格先删空，成为"空格"。
        int id = 3;
        auto run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        const doc::Ref cell0 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("甲"), mark()});
        const doc::Ref cell1 =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("乙"), mark()});
        const doc::Ref table =
            doc::makeNode(id++, NodeKind::Table, TableSpec{},
                          {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell0, cell1})});
        const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                               {run("前"), mark(), table, run("尾"), mark()});
        const NodeId root_id = id++;
        e52.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                        StyleSheet{});
        e52.relayout();
        expectTrue(e52.blocks().size() == 4,
                   "前、两格、尾共四块（实际 " + std::to_string(e52.blocks().size()) + " 块）");

        // 把第一格删空。
        e52.setCaret(1, 1);
        expectTrue(e52.backspace(), "第一格里的字删掉");
        e52.relayout();
        expectTrue(e52.blocks().size() == 4 && e52.blocks()[1].cp_text.empty(),
                   "第一格空了，格与表都还在");
        expectTrue(e52.auditTables() == 0, "删空之后表格结构自审一致");

        const LayoutResult& lay = e52.layout();
        const LineBox* le = nullptr;
        const LineBox* lf = nullptr;
        for (const LineBox& lb : lay.lines) {
            if (lb.paragraph == 1) le = &lb;
            if (lb.paragraph == 2) lf = &lb;
        }
        expectTrue(le != nullptr && lf != nullptr, "空格与有字格都排出了行盒");
        if (le != nullptr && lf != nullptr) {
            const float ymid = (le->block_lo_pt + le->block_hi_pt) * 0.5f;
            // 单击边距带：空格没有文字挡着，整格都是边距带 —— 命中给出这一格的段号。
            TextPos hit{};
            int band = -1;
            const bool band_ok = e52.hitTestPt(le->cell_box_lo_pt + 0.5f, ymid, &hit, nullptr,
                                               nullptr, nullptr, &band);
            expectTrue(band_ok && band == 1, "点在空格的左边距上：命中给出这一格的段号");
            // 反向一半：点空格的中间不是"选整格"，仍是落光标 —— 否则光标在空格里放不进去。
            int mid_para = -1;
            e52.hitTestPt((le->cell_box_lo_pt + le->cell_box_hi_pt) * 0.5f, ymid, &hit, nullptr,
                          nullptr, nullptr, &mid_para);
            expectTrue(mid_para < 0, "点空格的中间：不触发整格（落光标）");

            // 选整格：空格在流里的长度是 0，但"选中了这一格"照样成立，反白照样铺满。
            expectTrue(e52.selectCellAtPara(1), "按段号选中空格");
            expectTrue(e52.selectionIsCell(), "空格选区的粒度是单元格选区");
            expectTrue(e52.selection().empty(), "空格的内容区间长度是 0（没有字可删）");
            const std::vector<PageRect> rects = e52.selectionRects();
            expectTrue(rects.size() == 1,
                       "空格也画出一个整格矩形（实际 " + std::to_string(rects.size()) + " 个）");
            if (rects.size() == 1) {
                const float box_w = le->cell_box_hi_pt - le->cell_box_lo_pt;
                const float row_h = le->cell_bottom_pt - le->cell_top_pt;
                expectNear(rects[0].r_pt.x, le->cell_box_lo_pt, 0.01, "空格反白贴格外框左边");
                expectNear(rects[0].r_pt.w, box_w, 0.01, "空格反白铺满格外框宽（左右没有缝）");
                expectNear(rects[0].r_pt.h, row_h, 0.01, "空格反白铺满整行高");
                expectTrue(box_w > (le->cell_hi_pt - le->cell_lo_pt) + 1.0f,
                           "反白用的外框比内容区宽（两侧内边距都算进来）");
            }

            // 三击那条路：点在空格里任何位置都选中这一格。
            e52.clearSelection();
            expectTrue(e52.selectCellAtPt(le->inline_origin_pt + 1.0f, ymid), "三击空格：选中整格");
            expectTrue(e52.selectionIsCell() && !e52.selectionRects().empty(),
                       "三击空格之后有反白");

            // 反向一半：点有字格的字上不是"选整格"，仍是文字光标。
            int word_para = -1;
            e52.hitTestPt(lf->inline_origin_pt + 1.0f, (lf->block_lo_pt + lf->block_hi_pt) * 0.5f,
                          &hit, nullptr, nullptr, nullptr, &word_para);
            expectTrue(word_para < 0, "点有字格的字上：不触发整格");

            // 拖选：空格与有字的格一起选上 —— 两格都反白。
            e52.setCaretAtSideLine(1, 0, 0, 0);
            e52.setFocusAtSideLine(2, 1, 0, 0);
            expectTrue(e52.selectionIsCell(), "从空格拖到有字的格：单元格选区");
            expectTrue(
                e52.selectionRects().size() == 2,
                "两格各一份反白（实际 " + std::to_string(e52.selectionRects().size()) + " 份）");

            // 单一格那一支：只选中空格再按退格 —— 没有内容可删，什么都不发生。
            //   （要放在"整张表被删掉"之前：那之后表已经不在了。）
            expectTrue(e52.selectCellAtPara(1), "选中空格");
            expectTrue(!e52.backspace(), "选中空格按退格：没有内容可删，拒绝");
            e52.relayout();
            expectTrue(e52.blocks().size() == 4, "拒绝之后四块都在（表格没被删）");
            expectTrue(e52.auditTables() == 0, "拒绝之后表格结构自审一致");

            // 删除：这张表就两个格，拖选两格 = 盖满整张表 ⇒ 删的是表本身。
            //   只清内容会留下一张空表：用户按的是删除，得到的却是"表还在"。
            e52.setCaretAtSideLine(1, 0, 0, 0);
            e52.setFocusAtSideLine(2, 1, 0, 0);
            expectTrue(e52.selectionIsCell(), "再拖选两格（整张表）：单元格选区");
            expectTrue(e52.backspace(), "跨格选区退格：执行成功");
            e52.relayout();
            expectTrue(e52.blocks().size() == 2 && e52.blocks()[0].textUtf8() == "前" &&
                           e52.blocks()[1].textUtf8() == "尾",
                       "★ 盖满整张表：表连同两格一起删掉了，只剩前后两段（实际 " +
                           std::to_string(e52.blocks().size()) + " 块）");
            expectTrue(e52.auditTables() == 0, "删整表之后表格结构自审一致");
        }
    }

    if (runSection(53)) std::printf("\n[53] 排版摘要：重构锁（只改结构时这份摘要必须逐位相同）\n");
    if (runSection(53)) {
        // 2026-10-24 重取 A/B/C（→ 0xb27e1602e6412913 / 0xd2495b3ac02349bd /
        //   0xc3eab2f0dd425de4）：两处排版行为有意变了。
        //   一、行距的基准从字号换成单倍行高（ascent + descent），同一个倍数因此高了约
        //   45%，页数跟着变。二、默认段后距 10 磅改成 6 磅（B / C 没有显式设这个字段）。
        //   行数与字形数一字未变（180/3231 与 157/927）—— 断行只跟字号与格宽有关。
        // 用例 A：六十段横排正文，混排字号 —— 覆盖断行、行距、分页与段后距。
        //   段落够多，必然跨页；混排字号让每行的高度不同，翻页判据不能再用"上一行高度"。
        {
            EditorEngine ea;
            expectTrue(ea.init({cjkPath, mnPath, latinPath}), "引擎初始化（摘要 A）");
            EngineConfig ca;
            ca.zoom = 1.0f;
            ca.page.width_pt = 300.0f;
            ca.page.height_pt = 300.0f;
            ca.page.margins = PageMargins::all(10.0f);
            ea.setConfig(ca);
            std::vector<BlockSpec> blocks;
            for (int i = 0; i < 60; ++i) {
                BlockSpec b;
                b.font_index = 0;
                b.size_pt = (i % 7 == 0) ? 17.0f : 13.0f;
                b.line_spacing.factor = 1.7f;
                b.space_after_pt = (i % 5 == 0) ? 8.0f : 3.0f;
                b.language = "zh";
                b.setText(
                    "第 " + std::to_string(i + 1) +
                    " 段：患者诉晨起后心悸明显，持续约十分钟，休息后缓解。查体：心率 eighty-eight 次每分。");
                blocks.push_back(std::move(b));
            }
            loadBlocks(ea, blocks);
            ea.relayout();
            const LayoutDigest da = digestOf(ea.layout());
            std::printf("     摘要 A = 0x%016llx（%zu 行 / %zu 字形 / %d 页）\n",
                        static_cast<unsigned long long>(da.hash), ea.layout().lines.size(),
                        ea.layout().glyphs.size(), ea.layout().stats.page_count);
            // 摘要在"行距多出的空白上下对半分"之后重取：字形整体下移半个行距，坐标变了，
            //   行数、字形数、页数都没变（1.7 倍那一段仍是 180 行 / 24 页）。
            expectTrue(da.hash == 0x65cf7bda39ea3a62ull, "横排多页正文的摘要未变");
            expectTrue(ea.layout().stats.page_count >= 3, "用例 A 确实跨了多页");
        }

        // 用例 B：表格 —— 第一列合并两行，另一格的长文本撑到跨页，行片段要按页切。
        //   这一块是排版里唯一会"回挪已落位行"的地方，也是重构锁最想盯住的地方。
        {
            EditorEngine eb;
            expectTrue(eb.init({cjkPath, mnPath, latinPath}), "引擎初始化（摘要 B）");
            EngineConfig cb;
            cb.zoom = 1.0f;
            cb.page.width_pt = 320.0f;
            cb.page.height_pt = 240.0f;
            cb.page.margins = PageMargins::all(10.0f);
            eb.setConfig(cb);

            int id = 3;
            auto run = [&](const std::string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::string long_text;
            for (int i = 0; i < 26; ++i) {
                long_text +=
                    "这一格的内容足够长，会把这一行撑得比整页还高，于是必须按行盒切开分页。";
            }
            CellSpec merged;
            merged.row_span = 2;
            const doc::Ref c00 =
                doc::makeNode(id++, NodeKind::TableCell, merged, {run("合并格：跨两行"), mark()});
            const doc::Ref c01 =
                doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run(long_text), mark()});
            const doc::Ref c11 =
                doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("第二行第二格"), mark()});
            const doc::Ref row0 = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c00, c01});
            const doc::Ref row1 = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c11});
            TableSpec table_spec;
            table_spec.grid_cols = {Length::abs(ptToEmu(90.0)), Length::abs(ptToEmu(90.0))};
            table_spec.width = Length::percent(1, 1);
            const doc::Ref table = doc::makeNode(id++, NodeKind::Table, table_spec, {row0, row1});
            const doc::Ref section =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                              {run("表前"), mark(), table, run("表后"), mark()});
            const NodeId root_id = id++;
            eb.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            eb.relayout();
            const LayoutDigest db = digestOf(eb.layout());
            std::printf("     摘要 B = 0x%016llx（%zu 行 / %zu 字形 / %d 页 / %zu 表）\n",
                        static_cast<unsigned long long>(db.hash), eb.layout().lines.size(),
                        eb.layout().glyphs.size(), eb.layout().stats.page_count,
                        eb.layout().placed_tables.size());
            // 重取：0xe4def2d955859da1 → 0x6e4147ed798fdb6c（157 行 / 927 字形 / 13 页）。
            //   改了两处排版口径，两处都是有意的：单元格上下内边距从 0 改成 2 磅（行高
            //   跟着长），以及跨页切行改成按行盒自己的纵向位置分页 —— 同行其它格的行盒
            //   不再被后缀搬走，于是这张"一行高过整页"的表从 15 页变成 13 页。
            // 再取一次：0x9e22a25843f23449 —— 表格底边之后让出 kTableGapAfterPt，
            //   表后那段整体下移 6 磅；行数、字形数与页数都没变。
            expectTrue(db.hash == 0x088f6c06542fa222ull, "表格跨页的摘要未变");
            expectTrue(eb.layout().stats.page_count >= 3 && !eb.layout().placed_tables.empty() &&
                           eb.layout().placed_tables[0].placed.size() >= 2,
                       "用例 B 确实把一行切到了两页上");
        }

        // 用例 C：竖排 —— 直立（中日，列序从右往左）与侧倒（蒙文，顺时针 90°）各一段。
        //   它管的是 placeColumn 那一半：列心与列顶怎么变成列盒与字形。横排的摘要一条都
        //   覆盖不到这里（列宽逐列算、换带按剩余宽度判、字形带 rotate_deg）。
        {
            EditorEngine ec;
            expectTrue(ec.init({cjkPath, mnPath, latinPath}), "引擎初始化（摘要 C）");
            EngineConfig cc;
            cc.zoom = 1.0f;
            cc.page.width_pt = 300.0f;
            cc.page.height_pt = 300.0f;
            cc.page.margins = PageMargins::all(10.0f);
            ec.setConfig(cc);
            std::vector<BlockSpec> blocks;

            BlockSpec upright;
            upright.font_index = 0;
            upright.size_pt = 16.0f;
            upright.wm = WritingMode::VerticalRL();
            upright.line_spacing.factor = 1.8f;
            upright.language = "zh";
            std::string zh;
            for (int i = 0; i < 40; ++i) {
                zh += "患者诉晨起后心悸，持续约十分钟，休息后缓解。";
            }
            upright.setText(zh);
            blocks.push_back(std::move(upright));

            BlockSpec sideways;
            sideways.font_index = 1;  // 蒙文
            sideways.size_pt = 20.0f;
            sideways.wm = WritingMode::VerticalLR();
            sideways.line_spacing.factor = 1.8f;
            sideways.language = "mn";
            std::string mn;
            for (int i = 0; i < 20; ++i) {
                mn += "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF";
            }
            sideways.setText(mn);
            blocks.push_back(std::move(sideways));

            loadBlocks(ec, blocks);
            ec.relayout();
            const LayoutDigest dc = digestOf(ec.layout());
            int sideways_cols = 0;
            for (const LineBox& l : ec.layout().lines) sideways_cols += l.sideways ? 1 : 0;
            std::printf("     摘要 C = 0x%016llx（%zu 行 / %zu 字形 / %d 页 / 侧倒 %d 列）\n",
                        static_cast<unsigned long long>(dc.hash), ec.layout().lines.size(),
                        ec.layout().glyphs.size(), ec.layout().stats.page_count, sideways_cols);
            expectTrue(dc.hash == 0xc3eab2f0dd425de4ull, "竖排的摘要未变");
            expectTrue(ec.layout().stats.page_count >= 2 && sideways_cols > 0,
                       "用例 C 确实跨了多页，且侧倒那一支走到了");
        }
    }

    if (runSection(55)) std::printf("\n[55] 增量投影：一次编辑之后只有改动的那一块重算\n");
    if (runSection(55)) {
        // 这条判据盯的是"退步会不会无声地回来"：耗时会被更快的机器掩盖，复用块数不会。
        //   它也是 [54] 的另一半 —— 那边保证"产物对"，这边保证"没有整篇重算"。
        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（增量投影）");
        EngineConfig c;
        c.zoom = 1.0f;
        c.page.width_pt = 360.0f;
        c.page.height_pt = 300.0f;
        c.page.margins = PageMargins::all(12.0f);
        e.setConfig(c);

        std::vector<std::u32string> paras;
        for (int i = 0; i < 6; ++i) {
            const std::vector<uint32_t> cps = utf8ToCodepoints("第" + std::to_string(i) + "段内容");
            paras.emplace_back(cps.begin(), cps.end());
        }
        const StyleSheet sheet;
        e.setDocument(doc::makeTextDoc(paras), sheet);
        e.relayout();
        const int blocks = static_cast<int>(e.blocks().size());
        expectTrue(blocks == 6, "六段六块");
        expectTrue(e.editStats().rebuild_reused == 0, "装载那一轮没有上一轮可复用");

        expectTrue(e.insertCodepoint(2, 1, 0x7532), "第 2 段插一个字");
        expectTrue(e.editStats().rebuild_reused == blocks - 1,
                   "只有改动的那一块重算，其余 " + std::to_string(blocks - 1) + " 块整块搬（实际 " +
                       std::to_string(e.editStats().rebuild_reused) + "）");

        expectTrue(e.insertCodepoint(3, 1, 0x4e59), "换一段再插一个字");
        expectTrue(e.editStats().rebuild_reused == blocks - 1, "连打字：复用数不退化");

        // 撤销：修订回到上一份，块内容与上一轮相同 —— 投影这一层不因为"方向反了"就整篇重算。
        //   排版那一半（撤销的脏区间从哪来）见 [56]。
        expectTrue(e.undo(), "撤销成功");
        expectTrue(e.editStats().rebuild_reused == blocks - 1,
                   "撤销之后同样只重算被改过的那一块（实际 " +
                       std::to_string(e.editStats().rebuild_reused) + "）");
    }

    if (runSection(56)) std::printf("\n[56] 撤销走增量：损伤区间就是排版脏区间\n");
    if (runSection(56)) {
        // 撤销的排版脏区间只能从损伤区间来 —— 它没有命令层算出的落点，而两份修订一减就
        //   得到一个块区间：区间左边一个字没变、右边一个字没变，中间那段必须重排，后缀
        //   照常搬回。四条判据：内容逐块回到编辑之前、增量真的发生、增量与全量逐字段一致、
        //   以及"损伤区间之外还变了什么"（括号占位）也不能漏。
        EngineConfig c56;
        c56.zoom = 1.0f;
        c56.page.width_pt = 340.0f;
        c56.page.height_pt = 300.0f;
        c56.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts56 = {cjkPath, mnPath, latinPath};
        const int kN = 120;

        auto make56 = [&](int n) {
            std::vector<BlockSpec> blocks;
            for (int i = 0; i < n; ++i) {
                BlockSpec b;
                b.size_pt = 12.0f;
                b.line_spacing.factor = 1.6f;
                b.space_after_pt = 4.0f;
                b.language = "zh";
                b.setText("第 " + std::to_string(i + 1) +
                          " 段：随访记录占位，用来把版面撑到多页 —— 撤销才有后缀可复用。");
                blocks.push_back(std::move(b));
            }
            return blocks;
        };

        EditorEngine e;
        expectTrue(e.init(fonts56), "引擎初始化（撤销增量）");
        e.setConfig(c56);
        loadBlocks(e, make56(kN));
        e.relayout();
        expectTrue(static_cast<int>(e.blocks().size()) == kN, "一百二十段装好了");
        expectTrue(e.layout().stats.page_count >= 3, "用例跨了多页（撤销才有多页后缀可复用）");
        const doc::Doc baseDoc = e.document();
        const size_t baseLines = e.layout().lines.size();
        const size_t baseGlyphs = e.layout().glyphs.size();
        // 撤销之后"内容逐块回到编辑之前"：块哈希把文本、字符格式、段落格式、行内对象与
        //   容器几何都算进去，于是这一个数就够。比摘要强的地方是它不看排版路径 —— 增量
        //   排版的浮点累加次序与全量不同，坐标会差 1e-6 那一档，而内容不该差。
        auto blocksRestored = [&]() {
            return !doc::damage(baseDoc, e.document(), FlowVisibility::Display).range.changed;
        };

        // (a) 中间一段打一个字再撤销：脏区是那一段，后缀原样搬回。
        {
            e.setCaret(kN / 2, 2);
            expectTrue(e.typeTextAndRender("\xE6\xB5\x8B"), "第 60 段打一个字");
            const EditStats typed = e.editStats();
            expectTrue(!typed.layout_was_full && typed.reused_paragraphs > 0,
                       "打字本身是增量的（复用 " + std::to_string(typed.reused_paragraphs) +
                           " 段）—— 这条是下面那条的基线");

            expectTrue(e.undoAndRender(), "撤销这一笔");
            const EditStats st = e.editStats();
            expectTrue(!st.layout_was_full,
                       "★ 撤销不是整篇重排（relaid=" + std::to_string(st.relaid_paragraphs) +
                           " reused=" + std::to_string(st.reused_paragraphs) + "）");
            expectTrue(st.reused_paragraphs > 0,
                       "★ 撤销之后后缀被复用（" + std::to_string(st.reused_paragraphs) + " 段）");
            expectTrue(st.relaid_paragraphs <= 2,
                       "撤一次只重排脏的那一段（" + std::to_string(st.relaid_paragraphs) + " 段）");
            expectTrue(e.auditLayout() == 0, "★ 撤销之后的增量排版与全量逐字段一致");
            expectTrue(blocksRestored(), "★ 撤销之后块内容逐块回到打字之前");
            expectTrue(
                e.layout().lines.size() == baseLines && e.layout().glyphs.size() == baseGlyphs,
                "行数与字形数回到打字之前（没有留下重复的一段）");
        }

        // (b) 一次删掉五段再撤销：脏区宽过一段 —— 后缀复用必须知道到底换掉了几段。
        //   撤销没有命令层算的落点，只有损伤区间的两个端点。
        {
            const FlowPos f1 = e.flowPosAt(kN / 2 - 2, 0);
            const FlowPos f2 = e.flowPosAt(kN / 2 + 3, 0);
            expectTrue(e.setSelectionAndRender(Range{f1, f2}), "选中跨五段的区间");
            expectTrue(e.backspaceAndRender(), "删掉这五段");
            expectTrue(static_cast<int>(e.blocks().size()) == kN - 5, "段数少了五段");
            expectTrue(e.undoAndRender(), "撤销这次删除");
            expectTrue(static_cast<int>(e.blocks().size()) == kN, "★ 五段都回来了");
            expectTrue(e.auditLayout() == 0, "★ 跨段撤销之后增量仍与全量逐字段一致");
            expectTrue(blocksRestored(), "★ 跨段撤销之后块内容逐块回到删除之前");
            expectTrue(
                e.layout().lines.size() == baseLines && e.layout().glyphs.size() == baseGlyphs,
                "行数与字形数回到删除之前");
        }

        // (c) 末尾分段再撤销：损伤区间的右端落在文档末尾之外（lo == hi == 块数）。
        //   那时没有"新的脏段"，但旧排版多出来的尾巴必须截掉 —— 否则最后一段会画两遍。
        {
            const int last = static_cast<int>(e.blocks().size()) - 1;
            e.setCaret(last, e.blocks()[static_cast<size_t>(last)].cp_text.size());
            expectTrue(e.splitParagraphAndRender(), "在末段末尾回车，多出一段");
            expectTrue(static_cast<int>(e.blocks().size()) == kN + 1, "段数 +1");
            expectTrue(e.undoAndRender(), "撤销这次分段");
            expectTrue(static_cast<int>(e.blocks().size()) == kN, "★ 段数退回来了");
            expectTrue(e.auditLayout() == 0, "★ 末尾撤销之后增量仍与全量一致");
            expectTrue(blocksRestored(), "★ 末尾撤销之后块内容逐块回到分段之前");
            expectTrue(
                e.layout().lines.size() == baseLines && e.layout().glyphs.size() == baseGlyphs,
                "★ 没有多画一行、也没有多画一段（尾巴被截掉了）");
        }

        // (d) 括号开关：关掉 → 全篇占位消失；打开 → 全篇占位回来。
        //   这是"排版输入整篇翻转"的场景，也是后缀复用那条纪律的判据所在 ——
        //   脏区右端只要算错，auditLayout（增量 vs 全量）立刻红。
        //   覆盖面比"两个字段之间挪"更宽：这里是全篇 8 段一起翻。
        {
            EditorEngine eb;
            expectTrue(eb.init(fonts56), "引擎初始化（括号开关）");
            EngineConfig cb = c56;
            eb.setConfig(cb);
            std::vector<BlockSpec> bf = make56(8);
            for (int i = 0; i < 8; ++i) bf[i].setText("第" + std::to_string(i) + "段甲乙丙丁");
            Control fa;
            fa.range.start = TextPos{0, 2};
            fa.range.end = TextPos{0, 4};
            fa.id = "fa";
            Control fb;
            fb.range.start = TextPos{5, 2};
            fb.range.end = TextPos{5, 4};
            fb.id = "fb";
            loadBlocks(eb, bf, {fa, fb});
            eb.setEditMode(EditMode::Form);
            eb.relayout();

            // 占位簇数：常显档下两个字段各有一对。
            auto placeholders = [](const EditorEngine& en, int para) {
                int n = 0;
                for (const LineBox& lb : en.layout().lines) {
                    if (lb.paragraph != para) continue;
                    for (const LineCluster& c : lb.clusters) {
                        if (c.placeholder) ++n;
                    }
                }
                return n;
            };

            expectTrue(placeholders(eb, 0) == 2 && placeholders(eb, 5) == 2,
                       "括号常显：两个字段各一对占位");

            EngineConfig off = eb.config();
            off.show_brackets = false;  // 不占位 → 正文回流（setConfig 内部会重排）
            eb.setConfig(off);
            expectTrue(placeholders(eb, 0) == 0 && placeholders(eb, 5) == 0,
                       "★ 关掉括号之后全篇一个占位都不剩");
            expectTrue(eb.auditLayout() == 0, "★ 全篇占位消失之后增量仍与全量逐字段一致");

            EngineConfig on = eb.config();
            on.show_brackets = true;
            eb.setConfig(on);
            expectTrue(placeholders(eb, 0) == 2 && placeholders(eb, 5) == 2,
                       "★ 再打开，两处占位都回来了");
            expectTrue(eb.auditLayout() == 0, "★ 再打开之后增量仍与全量逐字段一致");
        }

        // (e) 排版账不被第二次呈现抹掉：一次编辑之后宿主常常再呈现一次（上屏收尾、
        //   光标闪烁），第二次不该把刚算出来的账清成 0 —— 那会让"这一次编辑排了多少"
        //   永远读不到。本轮标志按拍结算，数字按"最近一次排版"保留。
        {
            e.setCaret(kN / 3, 2);
            expectTrue(e.typeTextAndRender("\xE6\xB5\x8B"), "第 40 段打一个字");
            const EditStats first = e.editStats();
            e.repaint();
            const EditStats second = e.editStats();
            expectTrue(
                first.layout_ran && first.relaid_paragraphs > 0,
                "打字那一拍报告了重排（relaid=" + std::to_string(first.relaid_paragraphs) + "）");
            expectTrue(!second.layout_ran, "第二次呈现不再报告重排（本轮标志按拍结算）");
            expectTrue(second.relaid_paragraphs == first.relaid_paragraphs &&
                           second.reused_paragraphs == first.reused_paragraphs &&
                           second.layout_ms == first.layout_ms,
                       "★ 第二次呈现不抹掉排版账（reused=" +
                           std::to_string(second.reused_paragraphs) + "）");
            expectTrue(e.undoAndRender(), "撤掉这一笔");
            expectTrue(blocksRestored(), "撤销之后块内容逐块回到打字之前");
        }
    }

    if (runSection(57)) std::printf("\n[57] 页位图：每页与从零光栅一致，换窗只搬不画\n");
    if (runSection(57)) {
        // 5A 的判据。帧自审与底图自审读的都是页缓存，缓存本身写错时两条照样全绿 ——
        //   这里的参照物是"绕过缓存把同一页重新光栅一遍"，与 auditLayout 拿全量排版
        //   当参照是同一个手法。另有一条计数器判据：换窗时只有新进来的那一页被光栅。
        const std::vector<std::string> fonts57 = {cjkPath, mnPath, latinPath};
        EditorEngine e57;
        expectTrue(e57.init(fonts57), "引擎初始化（页位图）");
        EngineConfig c57;
        c57.zoom = 1.0f;
        c57.page.width_pt = 340.0f;
        c57.page.height_pt = 300.0f;
        c57.page.margins = PageMargins::all(12.0f);
        e57.setConfig(c57);

        std::vector<std::u32string> paras57;
        paras57.reserve(240);
        for (int i = 0; i < 240; ++i) {
            const std::vector<uint32_t> cps =
                utf8ToCodepoints("第" + std::to_string(i) + "段：页位图与槽的判据。");
            paras57.emplace_back(cps.begin(), cps.end());
        }
        const StyleSheet sheet57;
        e57.setDocument(doc::makeTextDoc(paras57), sheet57);
        e57.relayout();
        e57.setViewportAnchor(0, 4);
        e57.repaint();
        expectTrue(e57.editStats().total_pages >= 5,
                   "前提：文档至少五页（" + std::to_string(e57.editStats().total_pages) + "）");

        auto pageClean = [&](int p) {
            const std::string a = e57.auditPageJson(p);
            return a.find("\"diff\":0") != std::string::npos &&
                   a.find("\"cached\":1") != std::string::npos;
        };
        int clean = 0;
        for (int p = 0; p < 4; ++p) {
            if (pageClean(p)) ++clean;
        }
        expectTrue(clean == 4,
                   "窗口内四页都与从零光栅逐字节一致（" + std::to_string(clean) + "/4）");
        expectTrue(e57.auditPageJson(9999).find("\"diff\":-1") != std::string::npos,
                   "没有缓存的页不做假绿（diff = -1）");

        // 换窗一页：前三页的像素一个字节都没动，只有新进来的第 4 页要光栅。
        //   这一条是 5A 的收益本身 —— 旧口径下换窗要把整窗每页的字形重画一遍。
        e57.setViewportAnchor(1, 4);
        e57.repaint();
        expectTrue(e57.editStats().repainted_pages == 1,
                   "换窗只光栅新进来的那一页（实际 " +
                       std::to_string(e57.editStats().repainted_pages) + " 页）");
        clean = 0;
        for (int p = 1; p <= 4; ++p) {
            if (pageClean(p)) ++clean;
        }
        expectTrue(clean == 4, "换窗之后四页仍与从零光栅一致（" + std::to_string(clean) + "/4）");

        // 反向一半：底图自审把缓存整份丢掉再全建，这时四页都必须重新光栅。
        //   少了这一条，"只光栅一页"可以靠"计数器恒报 1"通过。
        const std::string ab57 = e57.auditBaseJson();
        expectTrue(ab57.find("\"diff\":0") != std::string::npos,
                   "底图全建之后与增量结果逐字节一致：" + ab57);
        expectTrue(e57.editStats().repainted_pages == 4,
                   "缓存清空之后四页都重新光栅（实际 " +
                       std::to_string(e57.editStats().repainted_pages) + " 页）");

        // 打字之后再审一次：行带重画是增量的，而缓存里的那一页必须仍等于从零光栅。
        //   这一条盯的是"带算错了、带外的旧像素留下了"——帧自审也能抓，但它读的是同一份缓存。
        e57.setViewportAnchor(0, 4);
        e57.repaint();
        expectTrue(e57.insertCodepoint(0, 1, 0x7532), "第 0 段插一个字");
        e57.relayout();
        e57.repaint();
        clean = 0;
        for (int p = 0; p < 4; ++p) {
            if (pageClean(p)) ++clean;
        }
        expectTrue(clean == 4,
                   "行带重画之后四页仍与从零光栅一致（" + std::to_string(clean) + "/4）");

        // 配置类改动不在页指纹里（指纹只哈希字形），靠 setConfig 整份作废缓存。
        //   漏掉那一条的症状是"改了底色屏幕不变"；参照物仍是"从零光栅"——
        //   缓存没作废时，从零光栅用的是新底色，两边立刻对不上。
        EngineConfig c57b = c57;
        c57b.page_fill = Color::rgb(250, 250, 240);
        e57.setConfig(c57b);
        e57.relayout();
        e57.repaint();
        clean = 0;
        for (int p = 0; p < 4; ++p) {
            if (pageClean(p)) ++clean;
        }
        expectTrue(clean == 4, "改页面底色之后四页仍与从零光栅一致（缓存已整份作废）");
    }

    if (runSection(58)) std::printf("\n[58] 按页接口：pageRgba 与 pageDirtyRects，光栅签名\n");
    if (runSection(58)) {
        // 5B 的判据。两件事分开验：页视图与帧对应块逐字节相同（接口本身对不对），
        //   以及"一次光标移动的脏矩形只有几十像素"（增量到底有没有落到页上）。
        //   第二条是这一刀的意义：此前宿主按页上屏，光标动一下要传整页 1 Mpx。
        const std::vector<std::string> fonts58 = {cjkPath, mnPath, latinPath};
        EditorEngine e58;
        expectTrue(e58.init(fonts58), "引擎初始化（按页接口）");
        EngineConfig c58;
        c58.zoom = 1.0f;
        c58.page.width_pt = 340.0f;
        c58.page.height_pt = 300.0f;
        c58.page.margins = PageMargins::all(12.0f);
        e58.setConfig(c58);

        std::vector<std::u32string> paras58;
        paras58.reserve(240);
        for (int i = 0; i < 240; ++i) {
            const std::vector<uint32_t> cps =
                utf8ToCodepoints("第" + std::to_string(i) + "段：按页接口与脏矩形。");
            paras58.emplace_back(cps.begin(), cps.end());
        }
        e58.setDocument(doc::makeTextDoc(paras58), StyleSheet{});
        e58.relayout();
        e58.setViewportAnchor(0, 4);
        e58.repaint();

        const int pw = e58.pagePxWidth();
        const int ph = e58.pagePxHeight();
        const int fw = e58.frameWidth();
        const int step = ph + e58.pageGapPx();
        expectTrue(e58.editStats().total_pages >= 5,
                   "前提：文档至少五页（" + std::to_string(e58.editStats().total_pages) + "）");

        // (a) 页视图 == 帧里那一块。帧是现成的参照物，而页视图是新接口 ——
        //   这一条钉的是"零拷贝视图指对了地方、长度也对"。
        //   前提是先藏光标：页位图里没有瞬态，帧里有一条光标，不藏就永远差那一块。
        //   藏/显各来一拍，留下的脏矩形在下面统一取走。
        e58.showCaret(false);
        e58.repaint();
        int same58 = 0;
        std::string bad58;
        const size_t want_n = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4;
        for (int p = 0; p < 4; ++p) {
            const size_t n = e58.pageBytes(p);
            if (n != want_n) {
                bad58 += " p" + std::to_string(p) + ":n=" + std::to_string(n);
                continue;
            }
            // 导出图里页的位置 = (页号 - 窗口首页) x 步距。槽那套编号已经删掉，
            //   这个换算就是"窗口顺序"本身，没有第二个说法。
            const int off = (p - e58.windowFirstPage()) * step;
            const uint8_t* src =
                e58.frame() + static_cast<size_t>(off) * static_cast<size_t>(fw) * 4;
            if (std::memcmp(e58.pagePixels(p), src, n) == 0) {
                ++same58;
                continue;
            }
            size_t d = 0;
            while (d < n && e58.pagePixels(p)[d] == src[d]) ++d;
            bad58 += " p" + std::to_string(p) + ":diff@" + std::to_string(d);
        }
        expectTrue(same58 == 4, "四页的 pageRgba 与帧对应块逐字节相同（" + std::to_string(same58) +
                                    "/4" + bad58 + "）");
        expectTrue(e58.pageBytes(9999) == 0, "没有缓存的页给空视图（不造假绿）");

        // 光标放回来，再把前面几拍攒下的脏矩形取走；下面只看增量。
        e58.showCaret(true);
        e58.repaint();
        for (int p = 0; p < 4; ++p) e58.takePageDirtyRects(p);

        // (b) 光标移动：两个小矩形（擦旧、画新），不是整页。
        e58.setCaret(0, 1);
        e58.repaint();
        const int cp = e58.editStats().caret_page;
        const std::vector<Rect> caret_rects = e58.takePageDirtyRects(cp);
        float caret_area = 0.0f;
        for (const Rect& r : caret_rects) caret_area += r.w * r.h;
        // caret_x / caret_y 现在是页内像素（页号另给），不必再减槽偏移 ——
        //   旧口径下要减的那一项正是"跨层坐标没带口径"的产物。
        const float cx = static_cast<float>(e58.editStats().caret_x_px);
        const float cy = static_cast<float>(e58.editStats().caret_y_px);
        const float cw = static_cast<float>(e58.editStats().caret_w_px);
        const float ch = static_cast<float>(e58.editStats().caret_h_px);
        bool caret_covered = false;
        for (const Rect& r : caret_rects) {
            if (cx >= r.x - 1.0f && cx + cw <= r.x + r.w + 1.0f && cy >= r.y - 1.0f &&
                cy + ch <= r.y + r.h + 1.0f) {
                caret_covered = true;
            }
        }
        expectTrue(!caret_rects.empty(), "光标移动之后这一页有脏矩形");
        expectTrue(caret_area < 400.0f, "★ 光标移动的上传面积是几十像素，不是整页（" +
                                            std::to_string(static_cast<int>(caret_area)) +
                                            " px²）");
        expectTrue(caret_covered, "脏矩形覆盖了光标矩形（擦旧 + 画新）");
        expectTrue(e58.takePageDirtyRects(cp).empty(), "脏矩形读后清零（第二次为空）");

        // (c) 打字：行带级，不是整页；页像素仍与从零光栅一致。
        expectTrue(e58.insertCodepoint(0, 1, 0x7532), "第 0 段插一个字");
        e58.relayout();
        e58.repaint();
        const std::vector<Rect> type_rects = e58.takePageDirtyRects(0);
        float type_area = 0.0f;
        for (const Rect& r : type_rects) type_area += r.w * r.h;
        expectTrue(type_area > 0.0f && type_area < static_cast<float>(pw) * ph * 0.25f,
                   "打字的上传面积是一行带，不是整页（" +
                       std::to_string(static_cast<int>(type_area)) + " px²）");
        const std::string ap58 = e58.auditPageJson(0);
        expectTrue(ap58.find("\"diff\":0") != std::string::npos,
                   "打字之后页位图仍与从零光栅一致：" + ap58);

        // (d) 光栅签名：改底色 → 所有页失效（若靠 setConfig 里手写清单，
        //   漏一项就是"改了底色屏幕不变"）。反向一半：同样配置再来一次，一页都不重画。
        EngineConfig c58b = c58;
        c58b.page_fill = Color::rgb(250, 250, 240);
        e58.setConfig(c58b);
        e58.relayout();
        e58.repaint();
        expectTrue(e58.editStats().repainted_pages == 4,
                   "改页面底色之后窗口内四页全部重光栅（光栅签名变了，实际 " +
                       std::to_string(e58.editStats().repainted_pages) + " 页）");
        int clean58 = 0;
        for (int p = 0; p < 4; ++p) {
            const std::string a = e58.auditPageJson(p);
            if (a.find("\"diff\":0") != std::string::npos) ++clean58;
        }
        expectTrue(clean58 == 4,
                   "改底色之后四页仍与从零光栅一致（" + std::to_string(clean58) + "/4）");

        e58.setConfig(c58b);
        e58.relayout();
        e58.repaint();
        expectTrue(e58.editStats().repainted_pages == 0,
                   "同样的配置再设一次，一页都不重光栅（签名不是每次 setConfig 都变，实际 " +
                       std::to_string(e58.editStats().repainted_pages) + " 页）");
    }

    if (runSection(59)) std::printf("\n[59] 连页接缝：分带渲染 == 整带渲染（逐字节）\n");
    if (runSection(59)) {
        // 连页把内容切成固定高度的条带，行就必然会跨接缝。判据不是"看着对不对"，
        //   而是"同一份文档切成 N 带、拼起来，与切成一条整带逐字节相同" ——
        //   整带没有接缝，是天然的参照物：接缝处少画半行、画重半行、两侧对不齐，
        //   都会在这里露出来。
        //
        // 为什么必须带像素判据：行盒、页号、指纹都是"归属层"的事实，它们全对而接缝
        //   处缺半行是完全可能的（字形按基线归带、上半截落在负 y 被裁掉就是这样）。
        const std::vector<std::string> fonts59 = {cjkPath, mnPath, latinPath};
        EditorEngine e59;
        expectTrue(e59.init(fonts59), "引擎初始化（接缝）");

        std::vector<std::u32string> paras59;
        paras59.reserve(60);
        for (int i = 0; i < 60; ++i) {
            const std::vector<uint32_t> cps =
                utf8ToCodepoints("第" + std::to_string(i) + "段：接缝处的字形必须完整。");
            paras59.emplace_back(cps.begin(), cps.end());
        }
        const StyleSheet sheet59;
        e59.setDocument(doc::makeTextDoc(paras59), sheet59);

        EngineConfig c59;
        c59.zoom = 1.0f;  // 1 磅 = 1 像素：拼图与整带用的是同一把尺子
        c59.page.width_pt = 320.0f;
        c59.page.height_pt = 150.0f;  // 故意矮：几十条带，接缝足够多
        c59.page.margins = PageMargins::all(20.0f);
        c59.page_gap_pt = 0.0f;
        // 连页开关在 EngineConfig 上；page.continuous 与 page.slab_pt 由 setConfig 装配
        //   （见 EditorEngine::setConfig），直接在 page 上置位不会生效。
        c59.continuous = true;
        e59.setConfig(c59);
        e59.relayout();
        e59.repaint();

        const int slabs = std::max(1, e59.editStats().total_pages);
        const int pw = e59.pagePxWidth();
        const int ph = e59.pagePxHeight();
        const size_t page_bytes = static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4u;
        std::vector<uint8_t> tiled(page_bytes * static_cast<size_t>(slabs), 0);
        int stitched = 0;
        for (int p = 0; p < slabs; ++p) {
            const uint8_t* src = e59.pagePixels(p);
            if (src == nullptr) continue;
            std::memcpy(tiled.data() + static_cast<size_t>(p) * page_bytes, src, page_bytes);
            ++stitched;
        }
        expectTrue(stitched == slabs, "每条带都拿得到像素（" + std::to_string(stitched) + "/" +
                                          std::to_string(slabs) + "）");

        auto capture = [&]() {
            int n = 0;
            for (int p = 0; p < slabs; ++p) {
                const uint8_t* src = e59.pagePixels(p);
                if (src == nullptr) continue;
                std::memcpy(tiled.data() + static_cast<size_t>(p) * page_bytes, src, page_bytes);
                ++n;
            }
            return n;
        };

        // 参照物：同一条文档、同样宽度，但高到一整条带装得下（没有接缝）。
        EngineConfig big59 = c59;
        big59.page.height_pt = 6000.0f;
        auto toBig = [&]() {
            e59.setConfig(big59);
            e59.relayout();
            e59.repaint();
        };
        auto toTiled = [&]() {
            e59.setConfig(c59);
            e59.relayout();
            e59.repaint();
        };
        toBig();
        expectTrue(e59.editStats().total_pages == 1,
                   "整带版本确实只有一条带（" + std::to_string(e59.editStats().total_pages) + "）");

        auto compareStitched = [&](const char* label) {
            const uint8_t* ref = e59.pagePixels(0);
            if (ref == nullptr) {
                expectTrue(false, std::string(label) + "：整带版本拿不到像素");
                return;
            }
            const int ref_h = e59.pagePxHeight();
            const int rows = std::min(ref_h, ph * slabs);
            long long diff = 0;
            int first_bad = -1;
            for (int y = 0; y < rows; ++y) {
                const uint8_t* a = tiled.data() + static_cast<size_t>(y) * pw * 4u;
                const uint8_t* b = ref + static_cast<size_t>(y) * pw * 4u;
                if (std::memcmp(a, b, static_cast<size_t>(pw) * 4u) == 0) continue;
                ++diff;
                if (first_bad < 0) first_bad = y;
            }
            std::printf("     %s：比了 %d 行（%d 条带 x %d 行），差 %lld 行%s\n", label, rows,
                        slabs, ph, diff,
                        first_bad >= 0 ? ("，首行 " + std::to_string(first_bad)).c_str() : "");
            expectTrue(diff == 0, std::string(label) + "：分带与整带逐字节相同");
        };
        compareStitched("纯内容");

        // 选一整篇：选区矩形同样会跨接缝，两带各画自己那半 —— 与内容同一条规则。
        //   这一条不可省：内容对、选区只画一半，屏幕上就是"字完整、高亮断一截"。
        toTiled();
        e59.selectAll();
        e59.repaint();
        expectTrue(capture() == slabs, "带选区时每条带仍拿得到像素");
        toBig();
        e59.selectAll();
        e59.repaint();
        compareStitched("全选");
    }

    // [60] 保存 / 打开。
    //
    // 判据的形状：存盘 → 换一个新引擎打开 → 整帧逐字节相同。它比"逐字段比对"强，
    //   因为任何参与绘制的字段没落盘都会落到像素上（与 [54] 同一条思路）。
    // 另一半是反向：坏文件 / 版本不符必须被拒，且拒绝之后现场一个字节都没动。
    if (runSection(60))
        std::printf("\n[60] 保存 / 打开：JSON 往返（整帧逐字节相同，坏文件不动现场）\n");
    if (runSection(60)) {
        EngineConfig c60;
        c60.page.width_pt = 420.0f;
        c60.page.height_pt = 300.0f;
        c60.page.margins = PageMargins::all(24.0f);

        std::vector<BlockSpec> b60;
        for (int i = 0; i < 5; ++i) {
            BlockSpec b;
            b.size_pt = 12.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_spacing.factor = 1.5f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText("保存第 " + std::to_string(i) + " 段：ABC 存取往返要逐字节回到原样");
            b60.push_back(std::move(b));
        }
        // 带一个控件：控件的边界是树里的节点，不是块流上的两个数字 ——
        //   这是"存形状"与"存文本"的分水岭。
        Control k60;
        k60.range.start = TextPos{1, 3};
        k60.range.end = TextPos{1, 9};
        k60.id = "k60";
        k60.read_only = false;

        const std::vector<std::string> fonts60 = {cjkPath, mnPath, latinPath};
        EditorEngine a;
        expectTrue(a.init(fonts60), "引擎初始化（保存 / 打开）");
        loadBlocks(a, b60, {k60});
        a.setConfig(c60);
        a.relayout();
        a.repaint();

        const auto saved = a.saveDocument();
        expectTrue(saved.ok && !saved.text.empty(),
                   saved.ok ? ("保存成功（" + std::to_string(saved.text.size()) + " 字节）")
                            : ("保存失败：" + saved.error));
        expectTrue(!saved.text.empty() && saved.text.front() == '{', "存出来的是一份 JSON 对象");
        // 文件要自己解释自己：文字是可读的 UTF-8（不是码点数组），字体给的是文件名
        //   （不是一个库下标）。
        expectTrue(saved.text.find("\"t\":\"保存第 0 段") != std::string::npos,
                   "文字按 UTF-8 写进文件（打开文件就看得到内容）");
        expectTrue(saved.text.find("\"fonts\":{") != std::string::npos &&
                       saved.text.find("\"ps\":\"Noto") != std::string::npos &&
                       saved.text.find("NotoSansSC-VF.ttf") != std::string::npos,
                   "字体表里有真名字：PostScript 名当身份，文件名兜底");

        // 先编辑一笔：历史与光标都被推离装载态，再打开应当把它们全部收回去。
        a.setCaret(0, 5);
        expectTrue(a.typeTextAndRender("\xE6\xB5\x8B"), "准备：保存之后又敲了一个字");  // 测
        expectTrue(a.canUndo(), "准备：这一笔进了撤销栈");

        const auto loaded = a.loadDocument(saved.text);
        expectTrue(loaded.ok, loaded.ok ? std::string("打开成功") : ("打开失败：" + loaded.error));
        expectTrue(!a.canUndo(), "打开之后撤销栈是空的（历史不属于这一份文档）");
        expectTrue(a.blocks().size() == b60.size(), "段数回到文件里那一份");
        // 打开只标脏（与装载同一口径），收尾归调用方 —— 判据要自己排一次。
        a.relayout();
        a.repaint();

        EditorEngine b;
        expectTrue(b.init(fonts60), "第二个引擎初始化（打开）");
        b.setConfig(c60);
        const auto loaded2 = b.loadDocument(saved.text);
        expectTrue(loaded2.ok, "同一个文件在新引擎里也能打开");
        b.relayout();
        b.repaint();
        bool same_text = b.blocks().size() == a.blocks().size();
        for (size_t i = 0; same_text && i < b.blocks().size(); ++i) {
            same_text = b.blocks()[i].textUtf8() == a.blocks()[i].textUtf8();
        }
        expectTrue(same_text, "逐段文字相同");

        a.setCaret(0, 0);
        b.setCaret(0, 0);
        expectTrue(frameDiffBetween(a, b) == 0,
                   "整帧逐字节相同（存盘 → 新引擎 → 打开，含控件括号与光标）");

        const auto again = b.saveDocument();
        expectTrue(again.ok && again.text == saved.text, "再存一遍字节完全相同（写端是确定的）");

        // 打开之后还能继续编辑，而且编辑是落在文件那一份上。
        b.setCaret(0, 5);
        expectTrue(b.typeTextAndRender("\xE6\xB5\x8B"), "打开之后能继续编辑");
        expectTrue(b.saveDocument().text != saved.text, "编辑之后存出来的文本变了");

        // 反向一半：坏文件被拒，且现场（文字 / 像素 / 历史）一个字节都没动。
        const std::string before_bad = b.saveDocument().text;
        const bool undo_before_bad = b.canUndo();
        std::vector<uint8_t> pix_before(b.frame(), b.frame() + b.frameBytes());
        const auto bad1 = b.loadDocument("{ \xE8\xBF\x99\xE4\xB8\x8D\xE6\x98\xAF json");
        expectTrue(!bad1.ok && !bad1.error.empty(), "坏 JSON 被拒绝且带原因");
        const auto bad2 = b.loadDocument("{\"v\":99}");
        expectTrue(!bad2.ok, "版本不符被拒绝（不猜着读）");
        const auto bad3 = b.loadDocument("");
        expectTrue(!bad3.ok, "空文件被拒绝");
        expectTrue(b.saveDocument().text == before_bad, "拒绝之后文档一个字节都没动");
        expectTrue(b.canUndo() == undo_before_bad, "拒绝之后撤销栈没动");
        expectTrue(pix_before.size() == b.frameBytes() &&
                       std::memcmp(pix_before.data(), b.frame(), pix_before.size()) == 0,
                   "拒绝之后像素一个字节都没动");
    }

    // [61] 表格走进增量。
    //
    // 判据的形状：在表里 / 表前 / 表后各改一个字，每一轮都要满足两条 ——
    //   (a) 没有退回整篇重排（layout_was_full 为假，且只重排了一部分）；
    //   (b) 增量产出的排版与"同内容从零重排"逐位相同（摘要），表格结构自审为 0。
    // 只有 (a) 会退化成"把增量关掉也全绿"（那就等于没做），只有 (b) 看不出有没有省下功夫。
    if (runSection(61)) std::printf("\n[61] 表格走进增量：表里改一个字，不退回整篇重排\n");
    if (runSection(61)) {
        EngineConfig c61;
        c61.zoom = 1.0f;
        c61.page.width_pt = 320.0f;
        c61.page.height_pt = 280.0f;
        c61.page.margins = PageMargins::all(12.0f);

        int id = 3;
        auto run = [&](const std::string& text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        std::vector<doc::Ref> kids;
        for (int i = 0; i < 8; ++i) {
            kids.push_back(
                run("表前第 " + std::to_string(i) + " 段：患者诉晨起后心悸，休息后缓解。"));
            kids.push_back(mark());
        }
        TableSpec table_spec;
        table_spec.grid_cols = {Length::abs(ptToEmu(88.0)), Length::abs(ptToEmu(88.0)),
                                Length::abs(ptToEmu(88.0))};
        table_spec.width = Length::percent(1, 1);
        std::vector<doc::Ref> rows;
        for (int r = 0; r < 3; ++r) {
            std::vector<doc::Ref> cells;
            for (int c = 0; c < 3; ++c) {
                cells.push_back(doc::makeNode(
                    id++, NodeKind::TableCell, CellSpec{},
                    {run("格" + std::to_string(r) + std::to_string(c) + "：查体记录"), mark()}));
            }
            rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
        }
        kids.push_back(doc::makeNode(id++, NodeKind::Table, table_spec, rows));
        for (int i = 0; i < 30; ++i) {
            kids.push_back(run("表后第 " + std::to_string(i) + " 段：复查心电图与电解质。"));
            kids.push_back(mark());
        }
        const doc::Ref section =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(kids));
        const NodeId root_id = id++;
        const doc::Doc doc61 =
            doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id);

        EditorEngine e61;
        expectTrue(e61.init({cjkPath, mnPath, latinPath}), "引擎初始化（表格增量）");
        e61.setConfig(c61);
        e61.setDocument(std::move(doc61), StyleSheet{});
        e61.relayout();
        e61.repaint();
        // 只打印，不做断言：耗时的绝对值随机器变，判据只认"走了哪条路"。
        const double full_ms = e61.editStats().layout_ms;

        const int n_para = static_cast<int>(e61.blocks().size());
        std::vector<int> in_table;
        for (int p = 0; p < n_para; ++p) {
            if (!e61.blocks()[static_cast<size_t>(p)].cell_path.empty()) in_table.push_back(p);
        }
        expectTrue(n_para > 40 && in_table.size() == 9,
                   "文档形状：表前 8 段 + 九格 + 表后 30 段（" + std::to_string(n_para) + " 段，" +
                       std::to_string(in_table.size()) + " 格）");
        expectTrue(e61.layout().placed_tables.size() == 1, "排出了一张表");

        // 参照物：一个新引擎，走与增量完全不同的入口（打开文件 → 全量重排）。
        //   不能拿"同一个引擎再 relayout 一次"当参照 —— 那条路与增量共用后半段代码。
        EditorEngine fresh;
        expectTrue(fresh.init({cjkPath, mnPath, latinPath}), "参照引擎初始化");
        fresh.setConfig(c61);
        auto sameAsFullFor = [&](EditorEngine& e, const std::string& label) {
            const auto saved = e.saveDocument();
            const bool opened = saved.ok && fresh.loadDocument(saved.text).ok;
            fresh.relayout();
            const uint64_t got = digestOf(e.layout()).hash;
            const uint64_t want = digestOf(fresh.layout()).hash;
            expectTrue(opened && got == want, label + "：增量结果与从零重排逐位相同（" +
                                                  std::to_string(got) + " vs " +
                                                  std::to_string(want) + "）");
            expectTrue(e.auditTables() == 0, label + "：表格结构自审一致");
        };
        auto sameAsFull = [&](const std::string& label) { sameAsFullFor(e61, label); };

        // 表格中间那一格。
        const int cell_para = in_table[in_table.size() / 2];
        e61.setCaret(cell_para, 1);
        expectTrue(e61.typeTextAndRender("改"), "表格中间那一格敲一个字");
        expectTrue(!e61.editStats().layout_was_full, "表里编辑：没有退回整篇重排");
        // 脏区两端都对齐到整张表（tableAlignedFrom / tableAlignedTo），于是重排的正好是那九格，
        //   表后三十段整段搬回。旧版这里重排 39 段（从表头一直到末尾）。
        expectTrue(e61.editStats().relaid_paragraphs == 9,
                   "表里编辑：只重排了那张表（" +
                       std::to_string(e61.editStats().relaid_paragraphs) + " 段）");
        // 反向一半：没有这条，"把后缀复用关掉"也能全绿 —— 那就等于没做。
        expectTrue(e61.editStats().reused_paragraphs == n_para - 9 - 8,
                   "表里编辑：表后那三十段是原样搬回来的（复用 " +
                       std::to_string(e61.editStats().reused_paragraphs) + " 段）");
        std::printf("     一次敲键：全量 %.2f ms → 增量 %.2f ms（重排 %d / %d 段，复用 %d 段）\n",
                    full_ms, e61.editStats().layout_ms, e61.editStats().relaid_paragraphs, n_para,
                    e61.editStats().reused_paragraphs);
        sameAsFull("表里编辑");

        // 表格之前：脏区右端扩到表尾（表不能只排一半），后缀从表后接上。
        e61.setCaret(2, 1);
        expectTrue(e61.typeTextAndRender("前"), "表前那一段敲一个字");
        expectTrue(!e61.editStats().layout_was_full, "表前编辑：没有退回整篇重排");
        expectTrue(e61.editStats().reused_paragraphs > 0,
                   "表前编辑：表后那段是搬回来的（复用 " +
                       std::to_string(e61.editStats().reused_paragraphs) + " 段）");
        expectTrue(e61.layout().placed_tables.size() == 1, "表前编辑：仍然只有一张表（没有重影）");
        sameAsFull("表前编辑");

        // 表格之后：前缀（含整张表）一个字节都不用动。
        const int after_table = in_table.back() + 5;
        e61.setCaret(after_table, 1);
        expectTrue(e61.typeTextAndRender("后"), "表后那一段敲一个字");
        expectTrue(!e61.editStats().layout_was_full, "表后编辑：没有退回整篇重排");
        // 表后有表也一样：后缀里没有表，整段搬回 —— 只重排被改的那一段。
        expectTrue(e61.editStats().relaid_paragraphs == 1,
                   "表后编辑：只重排了被改的那一段（" +
                       std::to_string(e61.editStats().relaid_paragraphs) + " 段）");
        sameAsFull("表后编辑");

        // 删掉整张表：上一轮有表、这一轮没有。截断按"起始段 >= from"丢几何，
        //   这条路径要是不对，表现是表没了但它的行片段还画在页上。
        const FlowPos before_table = e61.flowPosAt(0, 1);
        const FlowPos after_all = e61.flowPosAt(in_table.back() + 1, 0);
        expectTrue(e61.setSelection(Range{before_table, after_all}), "设一个跨过整表的选区");
        expectTrue(e61.backspaceAndRender(), "跨过整表的选区退格");
        expectTrue(e61.layout().placed_tables.empty(), "删表之后没有残留的表几何");
        expectTrue(e61.auditTables() == 0, "删表之后表格结构自审一致");
        sameAsFull("删表之后");

        // 两张表：改第一张表的时候，后缀里还剩着第二张表 —— 复用只能走到它的表头（cut），
        //   其余从那张表续排。这一条盯的是"平移不能穿过表"这条边界，以及截断之后
        //   段号/行号/行片段有没有接错（接错的症状是第二张表重影或整段上移一行）。
        auto putText = [&](const std::string& s8) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(s8), CharFormat{}, {}}, {});
        };
        auto putMark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        std::vector<doc::Ref> kids61c;
        auto putTable = [&]() {
            std::vector<doc::Ref> table_rows;
            for (int r = 0; r < 2; ++r) {
                std::vector<doc::Ref> cells;
                for (int c = 0; c < 2; ++c) {
                    cells.push_back(doc::makeNode(
                        id++, NodeKind::TableCell, CellSpec{},
                        {putText("格" + std::to_string(r) + std::to_string(c) + "：查体"),
                         putMark()}));
                }
                table_rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
            }
            TableSpec spec;
            spec.grid_cols = {Length::abs(ptToEmu(88.0)), Length::abs(ptToEmu(88.0))};
            spec.width = Length::percent(1, 1);
            kids61c.push_back(doc::makeNode(id++, NodeKind::Table, spec, table_rows));
        };
        for (int i = 0; i < 6; ++i) {
            kids61c.push_back(putText("表前第 " + std::to_string(i) + " 段：患者诉晨起后心悸。"));
            kids61c.push_back(putMark());
        }
        putTable();
        for (int i = 0; i < 20; ++i) {
            kids61c.push_back(putText("两表之间第 " + std::to_string(i) + " 段：复查心电图。"));
            kids61c.push_back(putMark());
        }
        putTable();
        for (int i = 0; i < 20; ++i) {
            kids61c.push_back(putText("表后第 " + std::to_string(i) + " 段：继续观察。"));
            kids61c.push_back(putMark());
        }
        const doc::Ref sec61c =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(kids61c));
        const NodeId root61c = id++;
        const doc::Doc t61c =
            doc::adopt(doc::makeNode(root61c, NodeKind::Document, {}, {sec61c}), id);

        EditorEngine e62;
        expectTrue(e62.init({cjkPath, mnPath, latinPath}), "引擎初始化（两张表）");
        e62.setConfig(c61);
        e62.setDocument(t61c, StyleSheet{});
        e62.relayout();
        e62.repaint();
        const int n62 = static_cast<int>(e62.blocks().size());
        std::vector<int> cells62;
        for (int p = 0; p < n62; ++p) {
            if (!e62.blocks()[static_cast<size_t>(p)].cell_path.empty()) cells62.push_back(p);
        }
        expectTrue(cells62.size() == 8 && e62.layout().placed_tables.size() == 2,
                   "两表文档：八格 / 两张表（" + std::to_string(cells62.size()) + " 格，" +
                       std::to_string(e62.layout().placed_tables.size()) + " 表）");

        // 第一张表的第二格：只敲一个字，表高不变 ⇒ 后缀开始时的状态逐位相同，
        //   于是第二张表也原样复用（不需要截断）。
        e62.setCaret(cells62[1], 1);
        expectTrue(e62.typeTextAndRender("改"), "第一张表里敲一个字");
        expectTrue(!e62.editStats().layout_was_full, "两表·表里编辑：没有退回整篇重排");
        expectTrue(e62.editStats().relaid_paragraphs == 4,
                   "两表·表里编辑：只重排了第一张表（" +
                       std::to_string(e62.editStats().relaid_paragraphs) + " 段）");
        expectTrue(e62.editStats().reused_paragraphs > 0,
                   "两表·表里编辑：后面（含第二张表）是搬回来的（复用 " +
                       std::to_string(e62.editStats().reused_paragraphs) + " 段）");
        sameAsFullFor(e62, "两表·表里编辑");

        // 同一个格再敲一长串：格内换行 ⇒ 表变高 ⇒ 后缀整体下移，复用必须在第二张表之前截断。
        expectTrue(e62.typeTextAndRender("双肺呼吸音清未闻及干湿啰音"), "再敲一串把格撑成两行");
        expectTrue(!e62.editStats().layout_was_full, "两表·表变高：没有退回整篇重排");
        expectTrue(e62.editStats().reused_paragraphs > 0,
                   "两表·表变高：表与第二张表之间那截是搬回来的（复用 " +
                       std::to_string(e62.editStats().reused_paragraphs) + " 段）");
        expectTrue(e62.editStats().relaid_paragraphs > 4 && e62.editStats().relaid_paragraphs < n62,
                   "两表·表变高：从第二张表续排（重排 " +
                       std::to_string(e62.editStats().relaid_paragraphs) + " / " +
                       std::to_string(n62) + " 段）");
        expectTrue(e62.layout().placed_tables.size() == 2,
                   "两表·表变高：两张表各一份几何（没有重影）");
        sameAsFullFor(e62, "两表·表变高");

        // 在第一张表的格里回车（段数 +1）：脏区还是那张表，但段号从此整体平移，
        //   后缀里那张表要按新段号重排（cut 那一路）。这一条盯的是截断之后段表/行盒/格映射
        //   有没有接错 —— 排版摘要只看行盒与字形，表几何要靠 auditLayout 的 tableDiff。
        e62.setCaret(cells62[1], 1);
        expectTrue(e62.splitParagraphAndRender(), "第一张表的格内回车");
        expectTrue(!e62.editStats().layout_was_full, "两表·分段：没有退回整篇重排");
        expectTrue(e62.layout().placed_tables.size() == 2, "两表·分段：还是两张表");
        expectTrue(e62.auditLayout() == 0, "两表·分段：增量排版与全量逐项一致（含表格几何）");
        sameAsFullFor(e62, "两表·分段");
    }

    // [62] 位置划分：每块只装内容，空隙只装边界。
    //
    // 两条都是"划分"的性质，不依赖实现怎么算：每一块的起点必须回读到自己的段首；
    //   全篇位数守恒 —— 块长加边界宽，再加末尾那几位，正好是流的总长。
    // 参照物只能来自另一条路：光标、选区、命中全读同一份 block_flow_，它们之间怎么对拍
    //   都自洽（一起错也一起"对"）。流总长由流索引给，与块流不是同一个来路。
    //
    // 末尾那几位按文档形状数：最后一段在表外且带标记时，它之后是"标记一位 + 出节一位"；
    //   以表格结尾时还要加上出格、出行、出表（最后一段带标记的话再加它自己那一位）。
    if (runSection(62)) std::printf("\n[62] 位置划分：每块只装内容，空隙只装边界\n");
    if (runSection(62)) {
        auto checkPartition = [&](EditorEngine* e, const std::string& what, int tail) {
            const int n = static_cast<int>(e->blocks().size());
            int total = 0;
            int bad_start = 0;
            int bad_gap = 0;
            FlowPos prev_end = 0;
            for (int i = 0; i < n; ++i) {
                const FlowPos start = e->blockFlowStart(i);
                const int span = e->blockFlowSpan(i);
                TextPos tp{-1, 0};
                if (!e->flowToTextPos(start, &tp) || tp.para != i || tp.offset != 0) ++bad_start;
                const int gap = (i == 0) ? start : start - prev_end;
                if (i > 0 && gap < 1) ++bad_gap;
                total += span + gap;
                prev_end = start + span;
            }
            expectTrue(bad_start == 0, what + "：每块的起点都回读到自己的段首（" +
                                           std::to_string(bad_start) + " 处不对）");
            expectTrue(bad_gap == 0,
                       what + "：块与块之间至少隔一位（" + std::to_string(bad_gap) + " 处为 0）");
            expectTrue(total + tail == e->flowSize(),
                       what + "：块长与边界宽加起来正好是流的总长（" + std::to_string(total) +
                           " + " + std::to_string(tail) + " vs " + std::to_string(e->flowSize()) +
                           "）");
        };
        int id = 3;
        auto text_run = [&](const std::string& text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };

        // 文档甲：正文、一个在表单模式下隐藏的控件、一个空段、末段。
        ControlSpec note;
        note.key = "note";
        note.controlType = "text";
        const doc::Ref hidden_control =
            doc::makeNode(id++, NodeKind::ContentControl, note, {text_run("隐藏字段")}, true);
        const doc::Ref sec_a =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                          {text_run("患者诉心悸三天"), mark(), hidden_control, text_run("可见正文"),
                           mark(), mark(), text_run("末段"), mark()});
        const NodeId root_a = id++;
        const doc::Doc a62 = doc::adopt(doc::makeNode(root_a, NodeKind::Document, {}, {sec_a}), id);

        // 文档乙：表格在中间，末段落在表外。
        TableSpec table_spec_b;
        table_spec_b.grid_cols = {Length::abs(ptToEmu(80.0)), Length::abs(ptToEmu(80.0))};
        table_spec_b.width = Length::percent(1, 1);
        std::vector<doc::Ref> rows_b;
        for (int r = 0; r < 2; ++r) {
            std::vector<doc::Ref> cells;
            for (int c = 0; c < 2; ++c) {
                const std::string cell_text = "格" + std::to_string(r) + std::to_string(c);
                cells.push_back(doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                              {text_run(cell_text), mark()}));
            }
            rows_b.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
        }
        const doc::Ref table_b = doc::makeNode(id++, NodeKind::Table, table_spec_b, rows_b);
        const doc::Ref sec_b =
            doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                          {text_run("表前一"), mark(), table_b, text_run("表后一"), mark()});
        const NodeId root_b = id++;
        const doc::Doc b62 = doc::adopt(doc::makeNode(root_b, NodeKind::Document, {}, {sec_b}), id);

        // 文档丙：以表格结尾，最后一段落在最后一格里（末尾边界最长的一条路）。
        TableSpec table_spec_c;
        table_spec_c.grid_cols = {Length::abs(ptToEmu(80.0))};
        table_spec_c.width = Length::percent(1, 1);
        const doc::Ref cell_c = doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                              {text_run("格里最后一段"), mark()});
        const doc::Ref row_c = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell_c});
        const doc::Ref table_c = doc::makeNode(id++, NodeKind::Table, table_spec_c, {row_c});
        const doc::Ref sec_c = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                             {text_run("开头"), mark(), table_c});
        const NodeId root_c = id++;
        const doc::Doc t62c =
            doc::adopt(doc::makeNode(root_c, NodeKind::Document, {}, {sec_c}), id);

        EngineConfig c62;
        c62.zoom = 1.0f;
        c62.page.width_pt = 320.0f;
        c62.page.height_pt = 240.0f;
        c62.page.margins = PageMargins::all(12.0f);

        EditorEngine ea;
        expectTrue(ea.init({cjkPath, mnPath, latinPath}), "引擎初始化（位置划分·正文）");
        ea.setConfig(c62);
        ea.setDocument(a62, StyleSheet{});
        ea.relayout();
        checkPartition(&ea, "正文（文档模式）", 2);
        ea.setEditMode(EditMode::Form);
        checkPartition(&ea, "正文（表单模式，隐藏控件整棵摘除）", 2);

        EditorEngine eb;
        expectTrue(eb.init({cjkPath, mnPath, latinPath}), "引擎初始化（位置划分·表格）");
        eb.setConfig(c62);
        eb.setDocument(b62, StyleSheet{});
        eb.relayout();
        checkPartition(&eb, "表格在中间", 2);
        // 表里改一个字之后再看一遍：位置是每轮重算的，改完就该仍然成立。
        eb.setCaret(2, 1);
        expectTrue(eb.typeTextAndRender("改"), "表里敲一个字");
        checkPartition(&eb, "表格在中间（表里改过之后）", 2);

        EditorEngine ec;
        expectTrue(ec.init({cjkPath, mnPath, latinPath}), "引擎初始化（位置划分·以表结尾）");
        ec.setConfig(c62);
        ec.setDocument(t62c, StyleSheet{});
        ec.relayout();
        checkPartition(&ec, "以表格结尾", 5);
    }

    if (runSection(63)) std::printf("\n[63] 落点格式：敲字沿用左边那一段，不是右边\n");
    if (runSection(63)) {
        // 一段字里只有一个字改了格式（其余同格式），在它左边界敲、右边界敲。
        //   两条判据必须互为反向 —— 左边那一段普通、那一段格式过，所以"沿用左边"
        //   在两侧给出的答案不同；只看一侧的话，规则取反了也照样绿。
        EngineConfig c63;
        c63.zoom = 1.0f;
        const std::vector<std::string> fonts63 = {cjkPath, mnPath, latinPath};

        // 形状：「abcd」，只有 b 是 14 磅，其余 12 磅。字号当"格式"用：它落在
        //   FormatRun 上，敲完一个字就能从块产物里读回每一格是多少磅。
        auto makeTree63 = [&]() {
            CharFormat small;
            small.size = Length::abs(ptToEmu(12.0));
            CharFormat large;
            large.size = Length::abs(ptToEmu(14.0));
            const doc::Ref section = doc::makeNode(
                2, NodeKind::Section, SectionSpec{},
                {doc::makeNode(3, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("a"), small, {}}, {}),
                 doc::makeNode(4, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("b"), large, {}}, {}),
                 doc::makeNode(5, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("cd"), small, {}}, {}),
                 doc::makeNode(6, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            return doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section}), 7);
        };

        EditorEngine e63;
        if (!e63.init(fonts63)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e63.setConfig(c63);
        e63.setDocument(makeTree63(), StyleSheet{});
        e63.relayout();
        e63.repaint();

        // 每一格现在是几磅（十分之一磅取整）。找不到覆盖这一格的 run 时给 -1。
        auto szAt = [&](int cp) {
            for (const FormatRun& r : e63.blocks()[0].runs) {
                if (static_cast<int>(r.cp_begin) <= cp && cp < static_cast<int>(r.cp_end)) {
                    return static_cast<int>(r.size_pt * 10.0f + 0.5f);
                }
            }
            return -1;
        };

        expectTrue(szAt(0) == 120 && szAt(1) == 140 && szAt(2) == 120,
                   "形状：三段字号 12 / 14 / 12 磅（改过格式的是中间那一个字）");

        const FlowPos fb = e63.flowPosAt(0, 1);  // b 之前
        const FlowPos fc = e63.flowPosAt(0, 2);  // b 之后

        // 回显的两份口径：显式标记在没改过的正文上是空的，而"实际生效的那一份"永远有字体
        //   字号 —— 工具栏上的字体与字号下拉框读的就是后者，否则正文永远显示空白。
        e63.setCaret(0, 0);
        const std::string eff_a = e63.charFormatAtJson(true);
        expectTrue(e63.charFormatAtJson().find("\"fi\"") == std::string::npos &&
                       eff_a.find("\"fi\"") != std::string::npos,
                   "★ 显式标记里没有字体，实际生效的那一份有（没设过字体也有字体）：" +
                       e63.charFormatAtJson() + " / " + eff_a);
        expectTrue(eff_a.find("\"sz\"") != std::string::npos &&
                       eff_a.find("\"b\"") != std::string::npos &&
                       eff_a.find("\"col\"") != std::string::npos,
                   "★ 实际生效的那一份字段齐全（粗细 / 字体 / 字号 / 字色都在）：" + eff_a);
        expectTrue(eff_a.find("\"sz\":12") != std::string::npos, "默认字号读回来是 12 磅");
        e63.setCaret(0, 1);
        expectTrue(e63.charFormatAtJson(true) == eff_a,
                   "★ 紧挨格式段左边：实际生效的 = 左边那个字的（与敲字同一个约定）");
        e63.setCaret(0, 2);
        expectTrue(e63.charFormatAtJson(true) != eff_a,
                   "★ 紧挨格式段右边：实际生效的 = 格式段自己的 14 磅（与左边不同）");

        // 回显与敲字是同一份判断的两面：紧挨格式段左边的那一处，回显必须等于左边那个字；
        //   紧挨它右边的那一处，回显必须等于格式段自己的。两条不等式的差值先量出来，
        //   否则"两侧本来就一样"时下面两条等于恒真。
        e63.setCaret(0, 0);
        const std::string show_before_b = e63.charFormatAtJson();
        e63.setSelection(Range{fb, fc});
        const std::string show_b = e63.charFormatAtJson();
        expectTrue(show_before_b != show_b,
                   "★ 两侧的格式确实不同（左边 12 磅 / 那一段 14 磅），下面的回显判据才有意义");
        e63.setCaret(0, 1);
        expectTrue(e63.charFormatAtJson() == show_before_b,
                   "★ 紧挨格式段左边的回显 = 左边那个字的回显（不是右边那一段的）");
        e63.setCaret(0, 2);
        expectTrue(e63.charFormatAtJson() == show_b,
                   "★ 紧挨格式段右边的回显 = 格式段自己的回显（回显与敲字同一个函数）");

        // (a) 左边界敲字：沿用左边那一段（12 磅），不是右边那一段（14 磅）。
        e63.setCaret(0, 1);
        expectTrue(e63.typeTextAndRender("X"), "在格式段的左边界敲一个字");
        expectTrue(e63.blocks()[0].cp_text == utf8ToCodepoints("aXbcd"),
                   "左边界敲字：文本接在中间");
        expectTrue(szAt(1) == 120,
                   "★ 左边界敲出来的字是 12 磅（沿用左边那一段，不是右边格式段的 14 磅）");
        expectTrue(szAt(2) == 140, "格式段自己没被带跑，仍是 14 磅");

        // (b) 连打第二下（不重新摆光标）：还在同一段上，不能因为光标落到了格式段之前而翻转。
        expectTrue(e63.typeTextAndRender("y"), "紧接着再敲一个字");
        expectTrue(szAt(1) == 120 && szAt(2) == 120,
                   "★ 连打第二个字仍是 12 磅（落点每格重新判，答案不随光标位置漂）");

        // (c) 反向一半：右边界敲字沿用格式段自己的 14 磅，不是右边那一段普通字的 12 磅。
        e63.setCaret(0, 4);  // 文本「aXybcd」里 b 之后
        expectTrue(e63.typeTextAndRender("Y"), "在格式段的右边界敲一个字");
        expectTrue(szAt(4) == 140,
                   "★ 右边界敲出来的字是 14 磅（接着左边格式段往下写），不是 12 磅");
        expectTrue(e63.blocks()[0].cp_text == utf8ToCodepoints("aXybYcd"),
                   "右边界敲字：文本接在中间");

        // (d) 段尾：左边是普通字，沿用 12 磅（这一条在改规则之前就是对的，留作对照）。
        e63.setCaret(0, 7);
        expectTrue(e63.typeTextAndRender("Z"), "在段尾敲一个字");
        expectTrue(szAt(7) == 120, "段尾敲字沿用左边那一段的 12 磅");

        // (e) 打字盖住选区：沿用选区第一个字（14 磅），不是左边的邻居（12 磅）。
        //   与 (a) 是两条不同的规则，混成一条会让"选中加粗的一段重打"掉格式。
        const FlowPos ew = e63.flowPosAt(0, 3);
        const FlowPos ex = e63.flowPosAt(0, 4);
        expectTrue(e63.setSelection(Range{ew, ex}), "选中格式段（现在在「aXybYcdZ」的第 4 格）");
        expectTrue(e63.typeTextAndRender("W"), "打字盖住格式段");
        expectTrue(szAt(3) == 140,
                   "★ 打字盖住格式段沿用 14 磅（取选区第一个字，不是左邻居的 12 磅）");
        expectTrue(e63.auditCompile() == 0 && e63.auditLayout() == 0,
                   "这几笔编辑之后增量与全量仍然逐字段一致");
    }

    if (runSection(64)) std::printf("\n[64] 编辑标记：只画进页位图，不动排版\n");
    if (runSection(64)) {
        // 三条判据：关掉之后像素真的少了（标记不是空开关）；开出"全部"之后又多出
        //   空格里的中点；三种档位的行盒与字形坐标逐位相同（标记不是内容，不进版面）。
        //   反向一半在最后：关回去的像素必须与从未打开时逐字节相同。
        EngineConfig c64;
        c64.zoom = 1.0f;
        c64.page.width_pt = 320.0f;
        c64.page.height_pt = 240.0f;
        c64.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts64 = {cjkPath, mnPath, latinPath};

        // 形状：两段。第一段中间有两个连续的空格（"全部"档下每一格都要有自己的中点），
        //   第二段是空段（段落标记落在行首）。
        const doc::Ref sec64 = doc::makeNode(
            2, NodeKind::Section, SectionSpec{},
            {doc::makeNode(3, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("甲乙  丙"), CharFormat{}, {}}, {}),
             doc::makeNode(4, NodeKind::ParagraphMark, doc::MarkPayload{}, {}),
             doc::makeNode(5, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const doc::Doc t64 = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {sec64}), 6);

        EditorEngine e64;
        if (!e64.init(fonts64)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e64.setConfig(c64);
        e64.setDocument(t64, StyleSheet{});
        e64.relayout();
        e64.repaint();

        // 比页位图，不比帧：帧里叠着光标与选区那一层瞬态，它跟标记无关，混进来就是
        //   把两件事的账记在一处（只读模式下光标不画，一关就把"标记没画"盖过去了）。
        auto snap = [&]() {
            const size_t n = e64.pageBytes(0);
            return std::vector<uint8_t>(e64.pagePixels(0), e64.pagePixels(0) + n);
        };
        // 两帧之间差多少个像素：按 RGBA 四字节一组比，比"差多少字节"更接近肉眼看到的变化。
        auto diffPx = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            if (a.size() != b.size()) return -1;
            int n = 0;
            for (size_t i = 0; i + 3 < a.size(); i += 4) {
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2] ||
                    a[i + 3] != b[i + 3])
                    ++n;
            }
            return n;
        };
        auto setMarks = [&](MarksMode m) {
            EngineConfig c = e64.config();
            c.marks = m;
            e64.setConfig(c);
            e64.relayout();
            e64.repaint();
        };
        // 版面几何：行盒的四个位置量、码点区间与每个字形的原点，全部逐位比。
        auto sameGeometry = [&](const LayoutResult& a, const LayoutResult& b) {
            if (a.lines.size() != b.lines.size() || a.glyphs.size() != b.glyphs.size())
                return false;
            for (size_t i = 0; i < a.lines.size(); ++i) {
                const LineBox& x = a.lines[i];
                const LineBox& y = b.lines[i];
                if (x.paragraph != y.paragraph || x.page != y.page) return false;
                if (x.block_lo_pt != y.block_lo_pt || x.block_hi_pt != y.block_hi_pt) return false;
                if (x.inline_origin_pt != y.inline_origin_pt) return false;
                if (x.inline_end_pt != y.inline_end_pt) return false;
                if (x.first_cp != y.first_cp || x.last_cp != y.last_cp) return false;
            }
            for (size_t i = 0; i < a.glyphs.size(); ++i) {
                if (a.glyphs[i].x_pt != b.glyphs[i].x_pt || a.glyphs[i].y_pt != b.glyphs[i].y_pt)
                    return false;
            }
            return true;
        };

        expectTrue(e64.config().marks == MarksMode::Paragraph,
                   "默认档 = 只显示段落标记（段落边界是编辑时最该看见的）");

        // 这一节比的是像素差，先把光标熄掉：只读模式下光标本来就不画（见 caretInsertable），
        //   留着它会往差值里多塞一条竖线的像素。
        e64.showCaret(false);
        const std::vector<uint8_t> with_para = snap();
        setMarks(MarksMode::None);
        const LayoutResult geom_off = e64.layout();
        const std::vector<uint8_t> bare = snap();
        const int ink_para = diffPx(with_para, bare);

        setMarks(MarksMode::All);
        const std::vector<uint8_t> all = snap();
        const int ink_all = diffPx(all, bare);
        const int ink_space = diffPx(all, with_para);

        expectTrue(ink_para > 0, "★ 段落标记真的画了墨迹（关掉之后少了 " +
                                     std::to_string(ink_para) + " 个像素）");
        expectTrue(ink_space > 0, "★ 全部标记档多出空格里的中点（比段落档多 " +
                                      std::to_string(ink_space) + " 个像素）");
        // 两个差值都以"无标记"那一版为基线：墨迹落在同一行上、像素集合互相压住，
        //   所以"两个差值相加"不成立。这里只钉两件事 —— 全部档确实画了墨迹，
        //   而且它比段落档多出来的正是空格里的中点（ink_space，上面那条）。
        expectTrue(ink_all > 0, "全部档确实画了墨迹（" + std::to_string(ink_all) + " 个像素）");
        expectTrue(sameGeometry(geom_off, e64.layout()),
                   "★ 三档的行盒与字形坐标逐位相同（标记不进版面：不占宽度、不改断行）");
        expectTrue(
            e64.blocks()[0].cp_text == utf8ToCodepoints("甲乙  丙") && e64.blocks().size() == 2,
            "正文与段数一个都没变（标记不是内容）");

        // 反向一半：关回去的像素必须与从未打开时逐字节相同 —— 开关不是累加的。
        setMarks(MarksMode::None);
        expectTrue(diffPx(snap(), bare) == 0, "★ 再关掉：帧逐字节回到没有标记的那一版");

        // 只读是呈现态：标记不画（与表单括号同一条）—— 画了会被印出来。
        setMarks(MarksMode::All);
        e64.setEditMode(EditMode::ReadOnly);
        e64.relayout();
        e64.repaint();
        const int ink_readonly = diffPx(snap(), bare);
        expectTrue(ink_readonly == 0, "★ 只读模式下不画标记（页位图等于关掉标记那一版，差 " +
                                          std::to_string(ink_readonly) + " 个像素）");
        e64.setEditMode(EditMode::Document);
        e64.relayout();
        e64.repaint();
        expectTrue(e64.auditCompile() == 0 && e64.auditLayout() == 0,
                   "切换档位之后增量与全量仍然逐字段一致");
    }

    if (runSection(65)) std::printf("\n[65] 编辑器内复制粘贴：样式跟着回来，高度不变\n");
    if (runSection(65)) {
        // 判据两半：粘回来的字必须还是粗的、还是 20 磅（片段带标记）；全选粘一遍之后
        //   段数与总高必须一模一样（片段不许多带一个段落标记，也不许把段落格式抹掉）。
        EngineConfig c65;
        c65.zoom = 1.0f;
        c65.page.width_pt = 320.0f;
        c65.page.height_pt = 240.0f;
        c65.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts65 = {cjkPath, mnPath, latinPath};

        std::vector<std::u32string> paras65;
        for (int i = 0; i < 3; ++i) {
            const std::vector<uint32_t> cps = utf8ToCodepoints("第" + std::to_string(i) + "段");
            paras65.emplace_back(cps.begin(), cps.end());
        }
        const doc::Doc t65 = doc::makeTextDoc(paras65);

        EditorEngine e65;
        if (!e65.init(fonts65)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e65.setConfig(c65);
        e65.setDocument(t65, StyleSheet{});
        e65.relayout();
        e65.repaint();

        // 把第一段整段改成粗体 20 磅，再全选复制。
        e65.setSelection(Range{e65.flowPosAt(0, 0), e65.flowPosAt(0, 3)});
        CharFormat big;
        big.bold = true;
        big.size = Length::abs(ptToEmu(20.0));
        e65.applyCharFormatAndRender(big);
        e65.relayout();
        e65.repaint();

        e65.selectAll();
        const ClipCopy cap = e65.copySelection();
        expectTrue(cap.ok && !cap.fragment.empty(), "全选复制拿得到片段");
        expectTrue(cap.html.find("<span") != std::string::npos,
                   "剪贴板载荷里带 HTML 样式（给别的应用那一份）");

        const int paras_before = static_cast<int>(e65.blocks().size());
        auto textOf = [&]() {
            std::string out;
            for (size_t i = 0; i < e65.blocks().size(); ++i) {
                if (i) out.push_back('\n');
                out += e65.blocks()[i].textUtf8();
            }
            return out;
        };
        const std::string text_before = textOf();
        const float h_before =
            e65.layout().lines.empty() ? 0.0f : e65.layout().lines.back().block_hi_pt;

        // 全选状态下粘贴：整篇被这一份片段替换掉。内容、段数、总高都不该变。
        e65.selectAll();
        expectTrue(e65.pasteFragment(cap.fragment).empty(), "全选状态粘回这一份片段");
        e65.relayout();
        e65.repaint();

        const int paras_after = static_cast<int>(e65.blocks().size());
        const std::string text_after = textOf();
        const float h_after =
            e65.layout().lines.empty() ? 0.0f : e65.layout().lines.back().block_hi_pt;
        expectTrue(text_after == text_before,
                   "★ 正文一字未变（多带一个段落标记会让末尾多一行空段）");
        expectTrue(paras_after == paras_before, "★ 段数不变（" + std::to_string(paras_before) +
                                                    " → " + std::to_string(paras_after) + "）");
        expectTrue(
            std::abs(h_after - h_before) < 0.01f,
            "★ 总高不变（" + std::to_string(h_before) + " → " + std::to_string(h_after) + " 磅）");

        // 样式：粘回来的第一段还是粗体 20 磅。
        e65.setSelection(Range{e65.flowPosAt(0, 0), e65.flowPosAt(0, 3)});
        const std::string eff = e65.charFormatAtJson(true);
        expectTrue(eff.find("\"b\":true") != std::string::npos,
                   "★ 粘回来的字还是粗的（" + eff + "）");
        expectTrue(eff.find("\"sz\":20") != std::string::npos,
                   "★ 粘回来的字还是 20 磅（" + eff + "）");
        expectTrue(e65.auditCompile() == 0 && e65.auditLayout() == 0,
                   "这一轮之后增量与全量仍然逐字段一致");
    }

    if (runSection(66)) std::printf("\n[66] 空段跨选有高亮 / 选区格式求交（三态缺席）\n");
    if (runSection(66)) {
        EngineConfig c66;
        c66.zoom = 1.0f;
        c66.page.width_pt = 320.0f;
        c66.page.height_pt = 240.0f;
        c66.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts66 = {cjkPath, mnPath, latinPath};
        const doc::Doc t66 = doc::makeTextDoc({U"甲乙", U"", U"丙丁"});
        EditorEngine e66;
        if (!e66.init(fonts66)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e66.setConfig(c66);
        e66.setDocument(t66, StyleSheet{});
        e66.relayout();
        e66.repaint();

        e66.setSelection(Range{e66.flowPosAt(0, 0), e66.flowPosAt(0, 2)});
        CharFormat b;
        b.bold = true;
        e66.applyCharFormatAndRender(b);

        // 跨选过空段：三行都要有矩形，中间那一条是高 17 磅、宽约 0.35 em 的窄条。
        e66.setSelection(Range{e66.flowPosAt(0, 1), e66.flowPosAt(2, 1)});
        const std::vector<EditorEngine::PageRect> rr = e66.selectionRects();
        expectTrue(rr.size() == 3,
                   "★ 跨选过空段：三条矩形都在（实际 " + std::to_string(rr.size()) + " 条）");
        bool has_narrow = false;
        for (const auto& pr : rr) {
            if (pr.r_pt.w > 0.0f && pr.r_pt.w < 6.0f && pr.r_pt.h > 10.0f) has_narrow = true;
        }
        expectTrue(has_narrow, "★ 空段那一条是窄条（段落标记的宽度），不是一个零宽矩形");

        // 求交：选区跨出加粗那一段，加粗这一项必须缺席（Word 的工具栏在这里会灭掉）。
        e66.setSelection(Range{e66.flowPosAt(0, 1), e66.flowPosAt(2, 1)});
        const std::string mixed = e66.effectiveFormatAtJson();
        expectTrue(mixed.find("\"b\"") == std::string::npos,
                   "★ 跨出加粗段之后加粗项缺席（" + mixed + "）");
        expectTrue(mixed.find("\"sz\"") != std::string::npos,
                   "字号两边一致，仍然要报（" + mixed + "）");
        // 反向一半：只选加粗那两个字，加粗项必须回来。
        e66.setSelection(Range{e66.flowPosAt(0, 0), e66.flowPosAt(0, 2)});
        const std::string bold_only = e66.effectiveFormatAtJson();
        expectTrue(bold_only.find("\"b\":true") != std::string::npos,
                   "★ 只选加粗段：加粗项报 true（" + bold_only + "）");
        expectTrue(e66.auditCompile() == 0 && e66.auditLayout() == 0,
                   "这一段之后增量与全量仍然逐字段一致");

        // 末尾：正向选到文档最末，最后一段也要有矩形。那枚段落标记占流里一位、不属于
        //   任何一块的内容，靠流区间选不到 —— 由 selectionRectsIn 按方向补上（见 [67]）。
        e66.setSelection(Range{e66.flowPosAt(0, 0), e66.flowPosAt(2, 2)});
        const auto all = e66.selectionRects();
        expectTrue(all.size() == 3, "★ 选到文档末尾：三条矩形都在（含最后一段）");

        // 末尾回车新建空段：行盒必须要能画那枚标记（有字体、size 正常、行末为 0）。
        e66.setCaret(2, 2);
        e66.splitParagraphAndRender();
        // 判据落在"页位图 == 从零重光栅"上，不比帧像素：帧里混着光标与选区，
        //   光标挪一格就能让两次读数不同，而新空段的标记一像素都没画。
        expectTrue(e66.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                   "★ 回车之后页位图与从零重光栅逐字节一致（新空段的标记在位图里）");
        const LineBox& born = e66.layout().lines.back();
        expectTrue(e66.blocks().size() == 4 && born.paragraph == 3,
                   "回车之后多出第 4 段，末行就是它");
        expectTrue(born.bracket_font != nullptr && born.last_cp == 0 && born.inline_end_pt == 0.0f,
                   "★ 新建空段的行盒具备画标记的条件（字体在、码点数 0、行末 0）");
        expectTrue(e66.auditCompile() == 0 && e66.auditLayout() == 0,
                   "回车之后增量与全量仍然逐字段一致");
    }

    if (runSection(67)) {
        std::printf("\n[67] 空段标记与文档末选区\n");
        EngineConfig c67;
        c67.zoom = 1.0f;
        c67.page.width_pt = 320.0f;
        c67.page.height_pt = 240.0f;
        c67.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts67 = {cjkPath, mnPath, latinPath};
        const doc::Doc t67 = doc::makeTextDoc({U"甲乙", U"丙丁"});
        EditorEngine e67;
        if (!e67.init(fonts67)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e67.setConfig(c67);
        e67.setDocument(t67, StyleSheet{});
        e67.relayout();
        e67.repaint();

        // 场景一：末尾连按三次回车。只多出三个行盒，一个字形都没多 —— 页指纹若只认
        //   字形，这几行会被判成"没变过"，那三枚标记一次都不重光栅。
        for (int i = 0; i < 3; ++i) {
            const int last = static_cast<int>(e67.blocks().size()) - 1;
            e67.setCaret(last, e67.blocks()[static_cast<size_t>(last)].cp_text.size());
            e67.splitParagraphAndRender();
        }
        // 光标挪回文档首：下面数墨迹时，瞬态不会落进末段那一格。
        e67.setCaret(0, 0);
        e67.repaint();

        const int pw67 = e67.pagePxWidth();
        const int ph67 = e67.pagePxHeight();
        const uint8_t* px67 = e67.pagePixels(0);
        auto inkIn = [&](float x, float y, float w, float h) {
            const int x0 = std::max(0, static_cast<int>(std::floor(x)) - 1);
            const int x1 = std::min(pw67, static_cast<int>(std::ceil(x + w)) + 1);
            const int y0 = std::max(0, static_cast<int>(std::floor(y)));
            const int y1 = std::min(ph67, static_cast<int>(std::ceil(y + h)));
            int n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                for (int xx = x0; xx < x1; ++xx) {
                    const size_t o = (static_cast<size_t>(yy) * static_cast<size_t>(pw67) +
                                      static_cast<size_t>(xx)) *
                                     4u;
                    if (px67[o] < 200 && px67[o + 1] < 200 && px67[o + 2] < 200) ++n;
                }
            }
            return n;
        };
        std::vector<int> ink67;
        for (size_t i = 0; i < e67.layout().lines.size(); ++i) {
            const LineBox& lb = e67.layout().lines[i];
            const float slot_x = lb.inline_origin_pt + lb.inline_end_pt;
            ink67.push_back(
                inkIn(slot_x, lb.block_lo_pt, lb.size_pt * 0.7f, lb.block_hi_pt - lb.block_lo_pt));
        }
        expectTrue(
            e67.layout().lines.size() == 5,
            "回车三次之后有五条行盒（实际 " + std::to_string(e67.layout().lines.size()) + "）");
        expectTrue(ink67.size() == 5 && ink67[2] > 0 && ink67[3] > 0 && ink67[4] > 0,
                   "★ 三个新空段的段落标记都画进了页位图（实际墨迹 " +
                       std::to_string(ink67.size() == 5 ? ink67[4] : -1) + " 像素）");
        expectTrue(e67.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                   "★ 页位图 == 从零重光栅（" + e67.auditPageJson(0) + "）");

        // 场景二：正向选到文档末（从上往下拖 = anchor 在首、focus 在末）。
        //   末段那枚标记在流里占一位却不属于任何一块的内容，要按"正向选到末位"补上。
        const int last67 = static_cast<int>(e67.blocks().size()) - 1;
        e67.selectAll();
        const std::vector<EditorEngine::PageRect> fwd67 = e67.selectionRects();
        expectTrue(fwd67.size() == 5, "★ 正向选到文档末：五段各一条矩形（含末段，实际 " +
                                          std::to_string(fwd67.size()) + " 条）");
        if (fwd67.size() == 5) {
            const LineBox& tail67 = e67.layout().lines.back();
            expectTrue(
                fwd67[4].r_pt.w > 0.0f && fwd67[4].r_pt.w <= tail67.size_pt * 0.4f + 0.01f,
                "★ 末段那一条就是那枚标记的窄条（宽 " + std::to_string(fwd67[4].r_pt.w) + " 磅）");
        }

        // 反向一半：锚点在文档末、焦点在首。末段的标记不在选区里 —— 与字处理软件一致，
        //   反过来选也高亮末段，就成了"方向不影响选中范围"的另一套语义。
        const FlowPos end67 = e67.flowPosAt(last67, 0);
        e67.setSelection(Range{end67, 0});
        const std::vector<EditorEngine::PageRect> back67 = e67.selectionRects();
        expectTrue(back67.size() == 4, "★ 反向选到文档末：末段没有那一条（实际 " +
                                           std::to_string(back67.size()) + " 条）");

        // 场景三：末段有字。正向全选时末行那一条要比正文宽出一枚标记，否则"选到文档末"
        //   与"选到正文末"在屏幕上没有区别。
        const doc::Doc t68 = doc::makeTextDoc({U"甲乙"});
        EditorEngine e68;
        if (!e68.init(fonts67)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e68.setConfig(c67);
        e68.setDocument(t68, StyleSheet{});
        e68.relayout();
        e68.repaint();
        e68.selectAll();
        const std::vector<EditorEngine::PageRect> one68 = e68.selectionRects();
        const float tail_w68 =
            e68.layout().lines.back().inline_end_pt + e68.layout().lines.back().size_pt * 0.35f;
        expectTrue(
            one68.size() == 1 && std::fabs(one68[0].r_pt.w - tail_w68) < 0.2f,
            "★ 末段有字：末行那一条宽出行末一枚标记（期望 " + std::to_string(tail_w68) + "，实际 " +
                (one68.empty() ? std::string("无") : std::to_string(one68[0].r_pt.w)) + "）");
        expectTrue(e67.auditCompile() == 0 && e67.auditLayout() == 0 && e68.auditCompile() == 0 &&
                       e68.auditLayout() == 0,
                   "这一段之后增量与全量仍然逐字段一致");

        // 场景四：竖排。标记与选区窄条都换到块轴上（列盒左右），是另一段加法 ——
        //   横排那一份过了不能代表这里。
        {
            BlockSpec v0;
            v0.font_index = 0;
            v0.size_pt = 12.0f;
            v0.wm = WritingMode::VerticalLR();
            v0.line_spacing.factor = 1.6f;
            v0.setText("甲乙");
            BlockSpec v1 = v0;
            v1.setText("丙丁");
            EditorEngine ev;
            expectTrue(ev.init(fonts67), "竖排引擎初始化");
            ev.setConfig(c67);
            loadBlocks(ev, {v0, v1});
            ev.relayout();
            ev.repaint();
            for (int i = 0; i < 2; ++i) {
                const int last = static_cast<int>(ev.blocks().size()) - 1;
                ev.setCaret(last, ev.blocks()[static_cast<size_t>(last)].cp_text.size());
                ev.splitParagraphAndRender();
            }
            ev.setCaret(0, 0);
            ev.repaint();

            const int pwv = ev.pagePxWidth();
            const int phv = ev.pagePxHeight();
            int marked = 0;
            for (const LineBox& lb : ev.layout().lines) {
                // 竖排段各占一列、每列一整页（块轴吃掉本页剩余宽度），所以逐行取它自己那一页。
                const uint8_t* pxv = ev.pagePixels(lb.page);
                if (pxv == nullptr) continue;
                // 竖排：行内轴是 y，标记坐在列底（inline_origin + inline_end）那一带上。
                const int y0 = std::max(
                    0, static_cast<int>(std::floor(lb.inline_origin_pt + lb.inline_end_pt)));
                const int y1 =
                    std::min(phv, static_cast<int>(std::ceil(
                                      lb.inline_origin_pt + lb.inline_end_pt + lb.size_pt * 0.8f)));
                const int x0 = std::max(0, static_cast<int>(std::floor(lb.block_lo_pt)) - 1);
                const int x1 = std::min(pwv, static_cast<int>(std::ceil(lb.block_hi_pt)) + 1);
                int n = 0;
                for (int yy = y0; yy < y1; ++yy) {
                    for (int xx = x0; xx < x1; ++xx) {
                        const size_t o = (static_cast<size_t>(yy) * static_cast<size_t>(pwv) +
                                          static_cast<size_t>(xx)) *
                                         4u;
                        if (pxv[o] < 200 && pxv[o + 1] < 200 && pxv[o + 2] < 200) ++n;
                    }
                }
                if (n > 0) ++marked;
            }
            expectTrue(ev.layout().lines.size() == 4 && marked == 4,
                       "★ 竖排：回车新生的两列也画出了段落标记（" + std::to_string(marked) + " / " +
                           std::to_string(ev.layout().lines.size()) + " 列有墨迹）");
            ev.selectAll();
            const std::vector<EditorEngine::PageRect> fwdv = ev.selectionRects();
            expectTrue(fwdv.size() == 4, "★ 竖排：正向选到文档末，末列也有矩形（实际 " +
                                             std::to_string(fwdv.size()) + " 条）");
            if (!fwdv.empty()) {
                const EditorEngine::PageRect& tailv = fwdv.back();
                expectTrue(tailv.r_pt.h > 0.0f && tailv.r_pt.w > 0.0f,
                           "★ 竖排末列那一条是竖直的窄条（" + std::to_string(tailv.r_pt.w) + " × " +
                               std::to_string(tailv.r_pt.h) + "）");
            }
            expectTrue(ev.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                       "竖排页位图 == 从零重光栅（" + ev.auditPageJson(0) + "）");
            expectTrue(ev.auditCompile() == 0 && ev.auditLayout() == 0,
                       "竖排这一路增量与全量也逐字段一致");
        }
    }

    if (runSection(68)) {
        std::printf("\n[68] 插入表格：块级落点、第一格光标、撤销与自审\n");
        EngineConfig c68;
        c68.zoom = 1.0f;
        c68.page.width_pt = 320.0f;
        c68.page.height_pt = 420.0f;
        c68.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts68 = {cjkPath, mnPath, latinPath};

        const doc::Doc t68 = doc::makeTextDoc({U"甲乙丙丁", U"第二段"});
        EditorEngine e68;
        if (!e68.init(fonts68)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e68.setConfig(c68);
        e68.setDocument(t68, StyleSheet{});
        e68.relayout();
        e68.repaint();
        expectTrue(e68.blocks().size() == 2, "装载之后两段");

        // 光标停在第 0 段中间：表格落在这一段之后，不能把这一段切成两半。
        e68.setCaret(0, 2);
        expectTrue(e68.insertTableAndRender(2, 3, true), "插入 2x3 表格（首行是标题行）");
        expectTrue(e68.blocks().size() == 8,
                   "段数 = 2 段正文 + 6 格（实际 " + std::to_string(e68.blocks().size()) + "）");
        expectTrue(e68.blocks()[0].textUtf8() == "甲乙丙丁",
                   "第 0 段内容没被表格切开（实际 " + e68.blocks()[0].textUtf8() + "）");
        expectTrue(e68.blocks()[7].textUtf8() == "第二段",
                   "原第 1 段后移到第 7 段（实际 " + e68.blocks()[7].textUtf8() + "）");

        // 表格结构：一张表、两行、三列，从第 1 段开始，每格一段。
        const LayoutResult& lay68 = e68.layout();
        expectTrue(lay68.placed_tables.size() == 1,
                   "产物里有一张表（实际 " + std::to_string(lay68.placed_tables.size()) + "）");
        bool shape_ok = false;
        if (lay68.placed_tables.size() == 1) {
            const PlacedTable& pt = lay68.placed_tables[0];
            shape_ok = pt.rows == 2 && pt.cols == 3 && pt.first_block == 1 && pt.cells.size() == 6;
        }
        expectTrue(shape_ok, "表是 2 行 3 列 6 格，起始段是第 1 段");
        expectTrue(!lay68.placed_tables.empty() && lay68.placed_tables[0].col_x.size() == 4 &&
                       lay68.placed_tables[0].col_x[3] > lay68.placed_tables[0].col_x[2],
                   "列边界按格数平分（4 个边界、递增）");

        // 光标落进第一格：接着打字就是填这一格，而不是落在表之前后。
        const TextPos after68 = e68.caret();
        bool in_first_cell = after68.para == 1;
        if (in_first_cell) {
            const std::vector<CellSlot>& path68 = e68.blocks()[1].cell_path;
            in_first_cell = path68.size() == 1 && path68[0].row == 0 && path68[0].cell == 0;
        }
        expectTrue(in_first_cell, "★ 光标落在第一格（段号 " + std::to_string(after68.para) + "）");

        // 格内打字：段数不变、文字进得去。
        expectTrue(e68.typeTextAndRender("甲"), "第一格里打一个字");
        expectTrue(e68.blocks().size() == 8, "格内打字不改段数");
        expectTrue(e68.blocks()[1].textUtf8() == "甲",
                   "字进了第一格（实际 " + e68.blocks()[1].textUtf8() + "）");

        // 两张表能共存：第二张插在光标所在段（第一格）之后，成为嵌套表或相邻块。
        e68.setCaret(0, 0);
        expectTrue(e68.insertTableAndRender(1, 2, false), "再插一张 1x2 表格");
        expectTrue(
            e68.layout().placed_tables.size() == 2,
            "两张表的几何都在（实际 " + std::to_string(e68.layout().placed_tables.size()) + "）");

        // 参数越界不落地：回来之后段数与表数都不动。
        const size_t blocks_before68 = e68.blocks().size();
        expectTrue(!e68.insertTableAndRender(0, 3, false), "rows = 0 被拒");
        expectTrue(!e68.insertTableAndRender(3, 0, false), "cols = 0 被拒");
        expectTrue(e68.blocks().size() == blocks_before68, "被拒时文档一个字节都没动");

        // 表单模式是填值态：不该长出新的表。
        e68.setEditMode(EditMode::Form);
        expectTrue(!e68.insertTableAndRender(2, 2, false), "表单模式下插入表格被拒");
        e68.setEditMode(EditMode::Document);

        // 撤销回到插入之前：两张表都没了，正文回到两段。
        expectTrue(e68.undo() && e68.undo() && e68.undo(), "撤销三次（两张表 + 一个格内字符）");
        e68.relayout();
        expectTrue(e68.layout().placed_tables.empty(), "撤销之后一张表都不剩");
        expectTrue(e68.blocks().size() == 2 && e68.blocks()[0].textUtf8() == "甲乙丙丁",
                   "正文回到插入之前（实际 " + std::to_string(e68.blocks().size()) + " 段）");

        // 重做回到三张表都在的状态，并且增量与全量、编译与全量都逐字段一致。
        expectTrue(e68.redo() && e68.redo() && e68.redo(), "重做三次");
        e68.relayout();
        e68.repaint();
        expectTrue(e68.auditCompile() == 0, "插入表格之后编译自审为 0");
        expectTrue(e68.auditLayout() == 0, "插入表格之后排版自审为 0");
        expectTrue(e68.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                   "插入表格之后页位图 == 从零重光栅");
    }

    if (runSection(69)) {
        std::printf("\n[69] 插入控件：行内落点、区间、取值与撤销\n");
        EngineConfig c69;
        c69.zoom = 1.0f;
        c69.page.width_pt = 320.0f;
        c69.page.height_pt = 420.0f;
        c69.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts69 = {cjkPath, mnPath, latinPath};

        const doc::Doc t69 = doc::makeTextDoc({U"甲乙丙丁"});
        EditorEngine e69;
        if (!e69.init(fonts69)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e69.setConfig(c69);
        e69.setDocument(t69, StyleSheet{});
        e69.relayout();
        e69.repaint();
        expectTrue(e69.controls().size() == 0, "装载之后没有控件");

        ControlSpec spec69;
        spec69.key = "patient_name";
        spec69.controlType = "text";
        e69.setCaret(0, 2);
        expectTrue(e69.insertControlAndRender(spec69, "张三"), "在第 2 个字后面插一个控件");
        expectTrue(e69.controls().size() == 1,
                   "控件集合里有一个（实际 " + std::to_string(e69.controls().size()) + "）");
        expectTrue(e69.blocks()[0].textUtf8() == "甲乙张三丙丁",
                   "控件不切断段落（实际 " + e69.blocks()[0].textUtf8() + "）");
        expectTrue(e69.blocks().size() == 1, "控件不产生新段");
        // 光标在文档模式下必须照旧画得出来：判"文档有没有内容"不能拿 blocks_ —— 它只是
        //   括号筛选后的临时副本，括号不显示（或刚重编译过）时是空的，拿它判会把整条光标判掉。
        expectTrue(e69.caretInsertable(),
                   "★ 插入控件之后文档模式的光标仍可插入（点哪儿都没反应的那种坏法）");
        {
            Rect cr69{};
            const bool has69 = e69.caretRectPt(&cr69);
            expectTrue(has69 && cr69.w > 0.0f, "光标矩形非零宽（画得出来）");
        }

        // 区间就是标签那一段文字：起止都落在同一个段落里。
        bool range_ok = false;
        if (e69.controls().size() == 1) {
            const ControlRange& cr = e69.controls().at(0).range;
            range_ok = cr.start.para == 0 && cr.start.offset == 2 && cr.end.para == 0 &&
                       cr.end.offset == 4;
        }
        expectTrue(range_ok, "控件区间盖住标签（2 到 4）");
        expectTrue(e69.controls().size() == 1 && e69.controls().at(0).id == "patient_name",
                   "控件带着业务编码");

        // 光标在控件内容末尾：接着打字就是填这个字段。
        const TextPos caret69 = e69.caret();
        expectTrue(caret69.para == 0 && caret69.offset == 4,
                   "光标在控件内容末尾（实际 " + std::to_string(caret69.offset) + "）");
        expectTrue(e69.typeTextAndRender("丰"), "接着打一个字");
        expectTrue(e69.blocks()[0].textUtf8() == "甲乙张三丰丙丁",
                   "字进了控件（实际 " + e69.blocks()[0].textUtf8() + "）");

        // 空标签的控件：区间零宽，也照样能被数到。
        e69.setCaret(0, 0);
        ControlSpec empty69;
        empty69.key = "blank";
        expectTrue(e69.insertControlAndRender(empty69, ""), "插一个空标签控件");
        expectTrue(e69.controls().size() == 2,
                   "两个控件（实际 " + std::to_string(e69.controls().size()) + "）");
        // 只读模式不许改结构。
        e69.setEditMode(EditMode::ReadOnly);
        expectTrue(!e69.insertControlAndRender(spec69, "不该出现"), "只读模式下插入控件被拒");
        e69.setEditMode(EditMode::Document);

        // 撤销一步步退：先退空标签控件、再退那个字、最后退第一个控件 —— 三步之后是纯文本。
        expectTrue(e69.undo(), "撤销一次（空标签控件）");
        expectTrue(e69.controls().size() == 1 && e69.blocks()[0].textUtf8() == "甲乙张三丰丙丁",
                   "撤销之后剩一个控件、文字不动（实际 " + e69.blocks()[0].textUtf8() + "）");
        expectTrue(e69.undo(), "再撤销一次（格内那个字）");
        expectTrue(e69.blocks()[0].textUtf8() == "甲乙张三丙丁",
                   "那个字退掉了（实际 " + e69.blocks()[0].textUtf8() + "）");
        expectTrue(
            e69.undo() && e69.controls().size() == 0 && e69.blocks()[0].textUtf8() == "甲乙丙丁",
            "第三次撤销回到纯文本（实际 " + e69.blocks()[0].textUtf8() + "、" +
                std::to_string(e69.controls().size()) + " 个控件）");
        // 重做把控件装回来：撤销靠旧修订、重做靠同一棵树挂回，两条路都要通。
        expectTrue(e69.redo() && e69.controls().size() == 1,
                   "重做把控件装回来（实际 " + std::to_string(e69.controls().size()) + "）");
        e69.relayout();
        e69.repaint();
        expectTrue(e69.auditCompile() == 0, "插入控件之后编译自审为 0");
        expectTrue(e69.auditLayout() == 0, "插入控件之后排版自审为 0");
    }

    if (runSection(70)) std::printf("\n[70] 表格：格内换行、跨页切片、连页切片与命中\n");
    if (runSection(70)) {
        // 一行里既有五行的格、也有空格：空格的行盒必须留在第一页。按数组后缀搬行时它
        //   会被一起搬到下一页 —— 表象就是"空格子的光标跑到下一页去了"。
        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（表格跨页）");
        EngineConfig c;
        c.zoom = 1.0f;
        c.page.width_pt = 300.0f;
        c.page.height_pt = 300.0f;
        c.page.margins = PageMargins::all(10.0f);
        e.setConfig(c);
        {
            int id = 3;
            auto run = [&](const std::string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::string long_text;
            for (int i = 0; i < 60; ++i) long_text += "字";
            const doc::Ref c00 =
                doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run(long_text), mark()});
            const doc::Ref c01 = doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()});
            const doc::Ref c10 =
                doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("第二行"), mark()});
            const doc::Ref c11 = doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()});
            const doc::Ref row0 = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c00, c01});
            const doc::Ref row1 = doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c10, c11});
            const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, {row0, row1});
            const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table});
            const NodeId root_id = id++;
            e.setDocument(doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                          StyleSheet{});
        }
        e.relayout();
        const LayoutResult& lay = e.layout();
        expectTrue(lay.lines.size() > 2,
                   "多行格排出了多行（实际 " + std::to_string(lay.lines.size()) + " 行）");
        // 空单元格 = 第 1 段；它的行盒落在第一页。
        int empty_line = -1;
        for (size_t i = 0; i < lay.lines.size(); ++i) {
            if (lay.lines[i].paragraph == 1) empty_line = static_cast<int>(i);
        }
        expectTrue(empty_line >= 0 && lay.lines[static_cast<size_t>(empty_line)].page == 0,
                   "同行的空单元格留在第一页（实际页 " +
                       std::to_string(
                           empty_line >= 0 ? lay.lines[static_cast<size_t>(empty_line)].page : -1) +
                       "）");
        // 命中：点在空单元格的中段与底部，都该落在第 1 段 —— 别跑到同一行里行盒更多的格。
        if (empty_line >= 0 && !lay.placed_tables.empty() && !lay.placed_tables[0].placed.empty()) {
            const LineBox& el = lay.lines[static_cast<size_t>(empty_line)];
            const float mid_x = (el.cell_box_lo_pt + el.cell_box_hi_pt) * 0.5f;
            const float row_bot =
                lay.placed_tables[0].placed[0].y + lay.placed_tables[0].placed[0].h;
            TextPos mid;
            const bool ok_mid = e.hitTestPt(mid_x, el.block_lo_pt + 20.0f, &mid);
            expectTrue(ok_mid && mid.para == 1, "点空单元格中段落在这一格（实际段 " +
                                                    std::to_string(ok_mid ? mid.para : -1) + "）");
            TextPos bot;
            const bool ok_bot = e.hitTestPt(mid_x, row_bot - 3.0f, &bot);
            expectTrue(ok_bot && bot.para == 1, "点空单元格底部仍落在这一格（实际段 " +
                                                    std::to_string(ok_bot ? bot.para : -1) + "）");
            TextPos in_text;
            const bool ok_text = e.hitTestPt(20.0f, row_bot - 3.0f, &in_text);
            expectTrue(ok_text && in_text.para == 0,
                       "点多行格底部落在多行格（实际段 " +
                           std::to_string(ok_text ? in_text.para : -1) + "）");
        }
        // 行片段的上下沿：单页里的行，片段正好包住格内容与上下内边距。
        if (!lay.placed_tables.empty()) {
            const PlacedTable& pt = lay.placed_tables[0];
            bool boxed = true;
            for (const PlacedRow& pr : pt.placed) {
                boxed =
                    boxed && pr.h > 1.0f && pr.y >= 10.0f - 0.01f && pr.y + pr.h <= 290.0f + 0.01f;
            }
            expectTrue(
                boxed && pt.placed.size() == 2,
                "两行的片段都在内容区里（实际 " + std::to_string(pt.placed.size()) + " 片）");
        }

        // 一行高过整页：必须切开，且每一行落在它自己的纵向位置所属的那一页。
        {
            EditorEngine e2;
            expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化（超高行）");
            EngineConfig c2;
            c2.zoom = 1.0f;
            c2.page.width_pt = 300.0f;
            c2.page.height_pt = 200.0f;
            c2.page.margins = PageMargins::all(10.0f);
            e2.setConfig(c2);
            int id = 3;
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::string huge;
            for (int i = 0; i < 300; ++i) huge += "字";
            const doc::Ref c00 = doc::makeNode(
                id++, NodeKind::TableCell, CellSpec{},
                {doc::makeNode(id++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints(huge), CharFormat{}, {}}, {}),
                 mark()});
            const doc::Ref c01 = doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()});
            const doc::Ref table =
                doc::makeNode(id++, NodeKind::Table, TableSpec{},
                              {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c00, c01})});
            const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table});
            const NodeId root_id = id++;
            e2.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            e2.relayout();
            const LayoutResult& l2 = e2.layout();
            const PlacedTable& pt2 = l2.placed_tables.at(0);
            expectTrue(l2.stats.page_count >= 3 && pt2.placed.size() >= 3,
                       "超高行切成多页（页 " + std::to_string(l2.stats.page_count) + " / 片 " +
                           std::to_string(pt2.placed.size()) + "）");
            // 空格那一行仍在第一页；每一页的片段高度为正、顶边在内容区里。
            int empty2 = -1;
            for (size_t i = 0; i < l2.lines.size(); ++i) {
                if (l2.lines[i].paragraph == 1) empty2 = static_cast<int>(i);
            }
            expectTrue(empty2 >= 0 && l2.lines[static_cast<size_t>(empty2)].page == 0,
                       "超高行切片之后，同行空格仍在第一页");
            bool sane = true;
            for (size_t i = 0; i < pt2.placed.size(); ++i) {
                const PlacedRow& pr = pt2.placed[i];
                sane = sane && pr.h > 1.0f && pr.page == static_cast<int>(i);
            }
            expectTrue(sane, "片段逐页递增、每片都非空");
            expectTrue(
                pt2.placed.front().cont_top == false && pt2.placed.back().cont_bottom == false,
                "首片不接上、末片不接下");
            // 中间那几片的两条边都是切出来的：绘制据此补线（分页）或不补（连页）。
            bool cuts = true;
            for (size_t i = 1; i + 1 < pt2.placed.size(); ++i) {
                cuts = cuts && pt2.placed[i].cont_top && pt2.placed[i].cont_bottom;
            }
            expectTrue(cuts, "中间片段的上下两边都是切出来的");
            // 行盒的页号与它自己的纵向位置一致：第一页的每一行都在页面内容区里。
            bool inside = true;
            for (const LineBox& lb : l2.lines) {
                if (lb.page != 0) continue;
                inside = inside && lb.block_lo_pt >= 9.99f && lb.block_hi_pt <= 190.01f;
            }
            expectTrue(inside, "第一页的行盒都在内容区里（没有溢到页外的行）");
            e2.repaint();
            // 跨页切口的两条边要补画：第一页的下边框、第二页的上边框。没有它们，
            //   表格在换页处看起来是被裁掉的；补上之后它才"接着长在下一页"。
            {
                const int pw2 = e2.pagePxWidth();
                const int ph2 = e2.pagePxHeight();
                /**
                 * 取一个像素是不是格线（深色）。
                 */
                auto ink = [&](int page, int x, int y) -> bool {
                    const uint8_t* px = e2.pagePixels(page);
                    if (px == nullptr) return false;
                    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(pw2) +
                                       static_cast<size_t>(x)) *
                                      4u;
                    return px[at] < 100 && px[at + 1] < 100 && px[at + 2] < 100;
                };
                /**
                 * 这一像素行是不是一条横线：格线横跨整张表，文字只占零星几列。
                 */
                auto lineLike = [&](int page, int y) -> bool {
                    if (y < 0 || y >= ph2) return false;
                    int dark = 0;
                    int total = 0;
                    for (int x = 16; x < 284; ++x) {
                        ++total;
                        if (ink(page, x, y)) ++dark;
                    }
                    return dark * 4 > total * 3;
                };
                const int cut0 =
                    static_cast<int>(std::lround((pt2.placed.front().y + pt2.placed.front().h)));
                const int cut1 = static_cast<int>(std::lround(pt2.placed[1].y));
                expectTrue(lineLike(0, cut0),
                           "第一页的下边框补上了（y=" + std::to_string(cut0) + "）");
                expectTrue(lineLike(1, cut1),
                           "第二页的上边框补上了（y=" + std::to_string(cut1) + "）");
                expectTrue(!lineLike(0, cut0 - 20), "切口上方是格内空白（不是整片黑）");
            }
            expectTrue(e2.auditLayout() == 0, "超高行之后排版自审为 0");
            expectTrue(e2.auditPageJson(0).find("\"diff\":0") != std::string::npos &&
                           e2.auditPageJson(2).find("\"diff\":0") != std::string::npos,
                       "页位图自审为 0（含最后一页）");
        }

        // 整行换页（行没被切开，是"这一行排不下就整个挪到下一页"）：换页处同样要补线。
        //   一类是页脚那一行的下边框 —— 没有人来画它（下一行在下一页）；一类是续页那一行的
        //   上边框。不然表格在换页处看着像被裁掉的，页脚最后一行没封口。
        {
            EditorEngine e4;
            expectTrue(e4.init({cjkPath, mnPath, latinPath}), "引擎初始化（整行换页）");
            EngineConfig c4;
            c4.zoom = 1.0f;
            c4.page.width_pt = 300.0f;
            c4.page.height_pt = 200.0f;
            c4.page.margins = PageMargins::all(10.0f);
            e4.setConfig(c4);
            int id = 3;
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            const char* rows4[] = {"甲", "乙", "丙", "丁", "戊", "己", "庚", "辛",
                                   "壬", "癸", "子", "丑", "寅", "卯", "辰", "巳"};
            std::vector<doc::Ref> table_rows;
            for (const char* txt : rows4) {
                const doc::Ref c0 = doc::makeNode(
                    id++, NodeKind::TableCell, CellSpec{},
                    {doc::makeNode(id++, NodeKind::TextRun,
                                   doc::RunPayload{utf8ToCodepoints(txt), CharFormat{}, {}}, {}),
                     mark()});
                const doc::Ref c1 = doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()});
                table_rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {c0, c1}));
            }
            const doc::Ref table4 = doc::makeNode(id++, NodeKind::Table, TableSpec{}, table_rows);
            const doc::Ref section4 =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table4});
            const NodeId root4 = id++;
            e4.setDocument(doc::adopt(doc::makeNode(root4, NodeKind::Document, {}, {section4}), id),
                           StyleSheet{});
            e4.relayout();
            const LayoutResult& l4 = e4.layout();
            const PlacedTable& pt4 = l4.placed_tables.at(0);
            expectTrue(pt4.placed.size() == 16, "十六行都是整片（没有被切开的行）");
            bool no_cut = true;
            for (const PlacedRow& pr : pt4.placed) {
                no_cut = no_cut && !pr.cont_top && !pr.cont_bottom;
            }
            expectTrue(no_cut, "★ 前提：这些行一整行地换页（不是切开的片段）");
            // 页脚最后一行 / 续页第一行。
            size_t last0 = pt4.placed.size();
            size_t first1 = pt4.placed.size();
            for (size_t i = 0; i < pt4.placed.size(); ++i) {
                if (pt4.placed[i].page == 0) last0 = i;
                if (pt4.placed[i].page == 1 && first1 == pt4.placed.size()) first1 = i;
            }
            expectTrue(last0 < pt4.placed.size() && first1 < pt4.placed.size(),
                       "★ 表格确实换页了（第一页最后一行 " + std::to_string(last0) +
                           " / 续页第一行 " + std::to_string(first1) + "）");
            e4.repaint();
            const int pw4 = e4.pagePxWidth();
            const int ph4 = e4.pagePxHeight();
            /**
             * 取一个像素是不是格线（深色）。
             */
            auto ink4 = [&](int page, int x, int y) -> bool {
                const uint8_t* px = e4.pagePixels(page);
                if (px == nullptr) return false;
                const size_t at =
                    (static_cast<size_t>(y) * static_cast<size_t>(pw4) + static_cast<size_t>(x)) *
                    4u;
                return px[at] < 100 && px[at + 1] < 100 && px[at + 2] < 100;
            };
            /**
             * 这一像素行是不是一条横线（同上一条判据）。
             */
            auto lineLike4 = [&](int page, int y) -> bool {
                if (y < 0 || y >= ph4) return false;
                int dark = 0;
                int total = 0;
                for (int x = 16; x < 284; ++x) {
                    ++total;
                    if (ink4(page, x, y)) ++dark;
                }
                return dark * 4 > total * 3;
            };
            if (last0 < pt4.placed.size() && first1 < pt4.placed.size()) {
                const int foot =
                    static_cast<int>(std::lround(pt4.placed[last0].y + pt4.placed[last0].h));
                const int head = static_cast<int>(std::lround(pt4.placed[first1].y));
                expectTrue(lineLike4(0, foot),
                           "★ 页脚最后一行的下边框补上了（y=" + std::to_string(foot) + "）");
                expectTrue(lineLike4(1, head), "★ 续页第一行有上边框（表格接着长，不是被裁掉）");
                expectTrue(!lineLike4(0, foot - 6), "页脚那条线上方是格内空白（不是整片黑）");
            }
            expectTrue(e4.auditLayout() == 0, "整行换页之后排版自审为 0");
            expectTrue(e4.auditPageJson(0).find("\"diff\":0") != std::string::npos &&
                           e4.auditPageJson(1).find("\"diff\":0") != std::string::npos,
                       "页位图自审为 0（含续页）");
        }

        // 回归：一页里的普通表格，三条横线（顶边、行分隔、底边）一条都不能少。
        //   补页脚那条下边时写成"只在切开的边上画"，顶边与行分隔会一起消失 ——
        //   一张两行四列的表于是只剩一个开口的方框（用户就是这么看到的）。
        {
            EditorEngine e5;
            expectTrue(e5.init({cjkPath, mnPath, latinPath}), "引擎初始化（一页表格）");
            EngineConfig c5;
            c5.zoom = 1.0f;
            c5.page.width_pt = 595.0f;
            c5.page.height_pt = 842.0f;
            c5.page.margins = PageMargins::all(56.0f);
            e5.setConfig(c5);
            int id = 3;
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::vector<doc::Ref> rows5;
            for (int r = 0; r < 2; ++r) {
                std::vector<doc::Ref> cells;
                for (int c = 0; c < 4; ++c) {
                    cells.push_back(doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()}));
                }
                rows5.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
            }
            const doc::Ref table5 = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows5);
            const doc::Ref section5 =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table5});
            const NodeId root5 = id++;
            e5.setDocument(doc::adopt(doc::makeNode(root5, NodeKind::Document, {}, {section5}), id),
                           StyleSheet{});
            e5.relayout();
            const PlacedTable& pt5 = e5.layout().placed_tables.at(0);
            expectTrue(pt5.placed.size() == 2, "两行都在第一页");
            e5.repaint();
            const int pw5 = e5.pagePxWidth();
            const int ph5 = e5.pagePxHeight();
            /**
             * 取一个像素是不是格线（深色）。
             */
            auto ink5 = [&](int x, int y) -> bool {
                const uint8_t* px = e5.pagePixels(0);
                if (px == nullptr) return false;
                const size_t at =
                    (static_cast<size_t>(y) * static_cast<size_t>(pw5) + static_cast<size_t>(x)) *
                    4u;
                return px[at] < 100 && px[at + 1] < 100 && px[at + 2] < 100;
            };
            /**
             * 这一像素行是不是一条横线（同上一条判据）。
             */
            auto lineLike5 = [&](int y) -> bool {
                if (y < 0 || y >= ph5) return false;
                int dark = 0;
                int total = 0;
                for (int x = 60; x < 530; ++x) {
                    ++total;
                    if (ink5(x, y)) ++dark;
                }
                return dark * 4 > total * 3;
            };
            const int top5 = static_cast<int>(std::lround(pt5.placed[0].y));
            const int mid5 = static_cast<int>(std::lround(pt5.placed[0].y + pt5.placed[0].h));
            const int bot5 = static_cast<int>(std::lround(pt5.placed[1].y + pt5.placed[1].h));
            expectTrue(lineLike5(top5), "★ 表格顶边画出来了（y=" + std::to_string(top5) + "）");
            expectTrue(lineLike5(mid5),
                       "★ 行与行之间的分隔线画出来了（y=" + std::to_string(mid5) + "）");
            expectTrue(lineLike5(bot5), "★ 表格底边画出来了（y=" + std::to_string(bot5) + "）");
        }

        {
            // 连页：表格行按条带切片，片段坐标是页内的，不按分页口径断开。
            EditorEngine e3;
            expectTrue(e3.init({cjkPath, mnPath, latinPath}), "引擎初始化（连页表格）");
            EngineConfig c3;
            c3.zoom = 1.0f;
            c3.page.width_pt = 300.0f;
            c3.page.height_pt = 200.0f;
            c3.page.margins = PageMargins::all(10.0f);
            // 连页是配置项（引擎按 page + page_gap 装配 PageMetrics），不是页度量上的开关。
            c3.continuous = true;
            c3.page_gap_pt = 20.0f;
            e3.setConfig(c3);
            int id = 3;
            std::string huge;
            for (int i = 0; i < 300; ++i) huge += "字";
            const doc::Ref cell3 = doc::makeNode(
                id++, NodeKind::TableCell, CellSpec{},
                {doc::makeNode(id++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints(huge), CharFormat{}, {}}, {}),
                 doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Ref table3 =
                doc::makeNode(id++, NodeKind::Table, TableSpec{},
                              {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell3})});
            const doc::Ref section3 =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table3});
            const NodeId root3 = id++;
            e3.setDocument(doc::adopt(doc::makeNode(root3, NodeKind::Document, {}, {section3}), id),
                           StyleSheet{});
            e3.relayout();
            const LayoutResult& l3 = e3.layout();
            const PlacedTable& pt3 = l3.placed_tables.at(0);
            expectTrue(pt3.placed.size() >= 2, "跨条带的行切成两片以上（实际 " +
                                                   std::to_string(pt3.placed.size()) + " 片）");
            // 首片的顶边 = 行顶的页内位置（不是块轴绝对位置）；续片的顶边在内容区上沿。
            expectTrue(pt3.placed[0].y >= 9.99f && pt3.placed[0].y <= 10.01f,
                       "首片顶边按页内坐标落（实际 " + std::to_string(pt3.placed[0].y) + "）");
            // 页内坐标的上界是条带高度（页高 + 页间隙）：贴在页底的那一截落在
            //   页间隙里，与文字同一条口径（宿主把间隙画在两张纸之间）。续片从条带
            //   上沿起，首片从行顶起 —— 两者都是页内坐标，不是块轴绝对位置。
            bool localized = true;
            for (size_t i = 0; i < pt3.placed.size(); ++i) {
                const PlacedRow& pr = pt3.placed[i];
                const float lo = (i == 0) ? 9.99f : -0.01f;
                localized = localized && pr.y >= lo &&
                            pr.y + pr.h <= c3.page.height_pt + c3.page_gap_pt + 0.01f;
            }
            expectTrue(localized, "每一片都在自己那条带的页内范围里");
            expectTrue(pt3.placed.front().cont_bottom && pt3.placed.back().cont_top,
                       "切出来的边都记了 cont 标记（绘制据此判断要不要补线）");
            e3.repaint();
            // 连页的条带切口不是页边界：那里不该有横线（有的话表格看上去就断成两页）。
            {
                const int pw3 = e3.pagePxWidth();
                /**
                 * 取一个像素是不是格线（深色）。
                 */
                auto ink = [&](int page, int x, int y) -> bool {
                    const uint8_t* px = e3.pagePixels(page);
                    if (px == nullptr) return false;
                    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(pw3) +
                                       static_cast<size_t>(x)) *
                                      4u;
                    return px[at] < 100 && px[at + 1] < 100 && px[at + 2] < 100;
                };
                /**
                 * 这一像素行是不是一条横线（同分页那一侧的判据）。
                 */
                auto lineLike = [&](int page, int y) -> bool {
                    const int ph3 = e3.pagePxHeight();
                    if (y < 0 || y >= ph3) return false;
                    int dark = 0;
                    int total = 0;
                    for (int x = 16; x < 284; ++x) {
                        ++total;
                        if (ink(page, x, y)) ++dark;
                    }
                    return dark * 4 > total * 3;
                };
                const int cut =
                    static_cast<int>(std::lround(pt3.placed.front().y + pt3.placed.front().h));
                const int cut_next = static_cast<int>(std::lround(pt3.placed[1].y));
                expectTrue(!lineLike(0, cut) && !lineLike(1, cut_next), "连页的条带切口不画横线");
            }
            expectTrue(e3.auditLayout() == 0, "连页表格的排版自审为 0");
            expectTrue(e3.auditBaseJson().find("\"diff\":0") != std::string::npos,
                       "连页底图自审为 0");
        }
    }

    if (runSection(71)) std::printf("\n[71] 空单元格的字符格式：落在段落标记上\n");
    if (runSection(71)) {
        // 空段落里没有文本节点，改格式只能落在段落标记上。全选之后改字重必须落到
        //   每一枚标记：命令返回"改了"、回显读得到、接着敲的字带着它、存盘读回还在。
        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（空单元格格式）");
        EngineConfig c;
        c.zoom = 1.0f;
        c.page.width_pt = 300.0f;
        c.page.height_pt = 300.0f;
        c.page.margins = PageMargins::all(10.0f);
        e.setConfig(c);
        e.setEditMode(EditMode::Document);
        e.setDocument(doc::makeTextDoc({U"甲乙"}), StyleSheet{});
        e.setCaret(0, 0);
        expectTrue(e.insertTableAndRender(2, 2, false), "插一张 2x2 表");
        e.selectAll();
        CharFormat patch;
        patch.bold = true;
        patch.italic = true;
        expectTrue(e.applyCharFormatAndRender(patch), "全选加粗 + 斜体");
        // 回显：落在空单元格里读到的是标记上那一份。
        e.setCaret(1, 0);
        const std::string at_empty = e.charFormatAtJson(true);
        expectTrue(at_empty.find("\"b\":true") != std::string::npos &&
                       at_empty.find("\"i\":true") != std::string::npos,
                   "空单元格的回显是粗斜体（实际 " + at_empty + "）");
        // 敲字沿用标记的格式。
        expectTrue(e.typeTextAndRender("字"), "在空单元格敲一个字");
        const BlockSpec& typed = e.blocks()[1];
        expectTrue(typed.textUtf8() == "字" && !typed.runs.empty() &&
                       typed.runs.front().weight >= 600.0f && typed.runs.front().italic,
                   "敲出来的字是粗斜体（实际 " + typed.textUtf8() + " / weight " +
                       std::to_string(typed.runs.empty() ? 0.0f : typed.runs.front().weight) +
                       "）");
        // 撤销：字退掉，格式留着（撤销的粒度是这一步操作）。
        expectTrue(e.undoAndRender() && e.blocks()[1].textUtf8().empty(),
                   "撤销回到空单元格（实际 \"" + e.blocks()[1].textUtf8() + "\"）");
        const std::string after_undo = e.charFormatAtJson(true);
        expectTrue(after_undo.find("\"b\":true") != std::string::npos,
                   "撤销之后格式仍在（实际 " + after_undo + "）");
        // 落盘：标记的字符格式是作者意图，读回来必须还在。
        const doc::SerializeResult saved = e.saveDocument();
        expectTrue(saved.ok, "序列化");
        EditorEngine e2;
        expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化（读回）");
        e2.setConfig(c);
        expectTrue(e2.loadDocument(saved.text).ok, "读回落盘文本");
        e2.relayout();
        e2.setCaret(1, 0);
        const std::string round = e2.charFormatAtJson(true);
        expectTrue(round.find("\"b\":true") != std::string::npos &&
                       round.find("\"i\":true") != std::string::npos,
                   "读回之后空单元格仍是粗斜体（实际 " + round + "）");
        expectTrue(e.auditCompile() == 0 && e.auditLayout() == 0, "改格式之后两份自审为 0");
    }

    if (runSection(72)) std::printf("\n[72] 表格跨页：续排片段的几何、命中、选中与自审\n");
    if (runSection(72)) {
        // 跨页切开的一行，续排片段上有些格的内容已经在上一页结束。这些格在这一页
        //   没有行盒 —— 命中、整格反白与字形归属都不能按行盒推，否则会出这三类错：
        //   点空格子光标跳到有字的格；空格子只在上一页有反白；切片那一行的字画在上一页。
        EngineConfig c72;
        c72.zoom = 1.0f;
        c72.page.width_pt = 400.0f;
        c72.page.height_pt = 300.0f;
        c72.page.margins = PageMargins::all(20.0f);

        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（跨页表格）");
        e.setConfig(c72);
        e.setEditMode(EditMode::Document);
        {
            // 表格之前先有一段正文：切片之后"第二页从内容区上沿起"才有对照物。
            e.setDocument(doc::makeTextDoc({U"首段"}), StyleSheet{});
        }
        e.setCaret(0, 2);
        expectTrue(e.insertTableAndRender(3, 3, false), "插一张 3x3 表");
        // 第一格撑满到跨页（每段一行，三十四段远超一页的内容高度，切出两片以上）。
        for (int i = 0; i < 34; ++i) {
            e.typeTextAndRender("第一格第" + std::to_string(i) + "行");
            e.splitParagraphAndRender();
        }
        e.relayout();
        const LayoutResult& lay = e.layout();
        const PlacedTable& pt = lay.placed_tables.at(0);
        expectTrue(pt.placed.size() >= 2,
                   "第一行被切成两片以上（实际 " + std::to_string(pt.placed.size()) + " 片）");
        expectTrue(pt.placed.front().page == 0 && !pt.placed.front().cont_top &&
                       pt.placed.front().cont_bottom,
                   "首片在第一页、下边是切出来的");
        expectTrue(pt.placed[1].page == 1 && pt.placed[1].cont_top,
                   "第二片在第二页、上边是切出来的");
        expectTrue(e.auditLayout() == 0, "跨页切片之后增量排版 == 全量");

        // 前提：续排片段上，另两格确实没有行盒。没有这一条，后面的判据可能是空转
        //   （比如它本来就还有行盒，那"点得到"根本不需要新逻辑）。
        // 段号从末尾数：最后一段是表后补的那段（表不能是文档的最后一块），再往前六段
        //   是行 1、行 2 的各三格，它们之前是行 0 的两个空格。
        const int n_blocks = static_cast<int>(e.blocks().size());
        const int empty_col1 = n_blocks - 9;
        const int empty_col2 = n_blocks - 8;
        expectTrue(e.blocks()[empty_col1].cp_text.empty() && e.blocks()[empty_col2].cp_text.empty(),
                   "前提：另两格是空段");
        bool col1_on_page1 = false;
        for (const LineBox& lb : lay.lines) {
            if (lb.paragraph == empty_col1 && lb.page == 1) col1_on_page1 = true;
        }
        expectTrue(!col1_on_page1, "前提：续排片段上空格的段落没有行盒（它的行盒在第一页）");

        // 命中：每一页的续排片段上，点第 2、3 列的空白区都落在各自那一格。
        //   坐标从片段自己取（页内坐标）：取片段的中段与列心，不写死页码。
        int pieces_checked = 0;
        int hits_ok = 0;
        int own_ok = 0;
        for (const PlacedRow& pr : pt.placed) {
            if (pr.row != 0 || pr.page == 0) continue;
            ++pieces_checked;
            const float local_y = pr.y + pr.h * 0.5f;
            const float y_doc = static_cast<float>(pr.page) * 320.0f + local_y;
            for (int col = 1; col <= 2; ++col) {
                const float x = pt.x + 0.5f * (pt.col_x[static_cast<size_t>(col)] +
                                               pt.col_x[static_cast<size_t>(col) + 1]);
                TextPos hit{};
                const int want = (col == 1) ? empty_col1 : empty_col2;
                if (e.hitTestPt(x, y_doc, &hit) && hit.para == want) ++hits_ok;
            }
            // 反向：同一行里那一格有内容，点在它上面仍落在它自己（不能因为"格框优先"
            //   把整行都判给第一列）。
            TextPos own{};
            if (e.hitTestPt(pt.x + 0.5f * pt.col_x[1], y_doc, &own) && own.para >= 9 &&
                own.para < empty_col1) {
                ++own_ok;
            }
        }
        expectTrue(pieces_checked >= 1,
                   "行 0 在第二页起有续排片段（实际 " + std::to_string(pieces_checked) + " 片）");
        expectTrue(hits_ok == pieces_checked * 2, "每一页的空格都点得中（" +
                                                      std::to_string(hits_ok) + "/" +
                                                      std::to_string(pieces_checked * 2) + "）");
        expectTrue(own_ok == pieces_checked, "每一页的有字格也点得中（" + std::to_string(own_ok) +
                                                 "/" + std::to_string(pieces_checked) + "）");

        // 整格反白：选第二页上的空格子，两页各一条，几何与 cellBoxOnPage 一致。
        {
            expectTrue(e.selectCellAtPara(empty_col1), "整格选中续排片段上的空格");
            const std::vector<PageRect> rects = e.selectionRects();
            expectTrue(rects.size() >= 2,
                       "空格子的反白铺到每一页（实际 " + std::to_string(rects.size()) + " 条）");
            const LayoutCell* cell = pt.cellAt(0, 1, nullptr, nullptr);
            bool shaped = cell != nullptr;
            if (cell != nullptr) {
                for (const PageRect& pr : rects) {
                    float bx = 0.0f;
                    float by = 0.0f;
                    float bw = 0.0f;
                    float bh = 0.0f;
                    const bool ok = pt.cellBoxOnPage(*cell, pr.page, &bx, &by, &bw, &bh);
                    shaped = shaped && ok && std::fabs(pr.r_pt.x - bx) < 0.01f &&
                             std::fabs(pr.r_pt.y - by) < 0.01f &&
                             std::fabs(pr.r_pt.w - bw) < 0.01f && std::fabs(pr.r_pt.h - bh) < 0.01f;
                }
            }
            expectTrue(shaped, "两条反白都正好是这一格在那一页上的框");
            bool counted = true;
            for (size_t i = 0; i < rects.size(); ++i)
                counted = counted && rects[i].page == static_cast<int>(i);
            expectTrue(
                rects.front().page == 0 && counted,
                "反白按页号从第一页逐页排开（实际 " + std::to_string(rects.size()) + " 条）");
        }

        // 字形页号必须等于它那一行所在页。行盒挪了页、字形没挪，字就画到上一页去了。
        auto glyphPageMismatch = [](const LayoutResult& lz) -> int {
            int bad = 0;
            for (const PlacedGlyph& g : lz.glyphs) {
                for (const LineBox& lb : lz.lines) {
                    if (lb.line != g.line) continue;
                    if (lb.page != g.page) ++bad;
                    break;
                }
            }
            return bad;
        };
        expectTrue(glyphPageMismatch(lay) == 0, "切片之后每个字形仍跟着它那一行所在页");
        // 每一片的高加起来等于这一行自己的高：切片不丢内容也不重复。
        {
            float sum = 0.0f;
            for (const PlacedRow& pr : pt.placed) {
                if (pr.row != 0) continue;
                sum += pr.h;
            }
            float span = 0.0f;
            for (const LineBox& lb : lay.lines) {
                if (lb.paragraph >= 1 && lb.paragraph <= 15) {
                    span = std::max(span, lb.block_hi_pt);
                }
            }
            expectTrue(sum > 0.0f && span > 0.0f && sum >= span, "片段之和覆盖整个多行格");
        }

        // 跨页之后在后面的行里敲字 + 回车：增量排版与页位图都必须跟得上。
        {
            const int last = static_cast<int>(e.blocks().size()) - 1;
            e.setCaret(last, 0);
            e.typeTextAndRender("新增");
            e.splitParagraphAndRender();
            e.typeTextAndRender("第二行");
            expectTrue(e.auditLayout() == 0, "跨页之后敲字：增量排版 == 全量");
            const int np = e.layout().stats.page_count;
            int diff_sum = 0;
            for (int pg = 0; pg < np; ++pg) {
                const std::string j = e.auditPageJson(pg);
                diff_sum += (j.find("\"diff\":0") != std::string::npos) ? 0 : 1;
            }
            if (diff_sum != 0) {
                for (int pg = 0; pg < np; ++pg) {
                    std::printf("      page%d %s\n", pg, e.auditPageJson(pg).c_str());
                }
            }
            expectTrue(diff_sum == 0, "跨页之后敲字：每一页的位图都与从零光栅相同");
            expectTrue(glyphPageMismatch(e.layout()) == 0, "敲字之后字形页号仍然一致");
            // 交叉判据：字真的在新行上，不是叠在旧行上。
            const BlockSpec& typed = e.blocks()[static_cast<size_t>(last)];
            expectTrue(typed.textUtf8() == "新增",
                       "新敲的字进了原来那一段（实际 " + typed.textUtf8() + "）");
        }

        // 连页：多行的表逐行下推。切片给的是页内坐标，拿它当块轴位置，第二行起整张表
        //   会塌到第一条带的顶上 —— 表象是"表格跑到页面外面去了"。
        {
            EditorEngine e2;
            expectTrue(e2.init({cjkPath, mnPath, latinPath}), "引擎初始化（连页多行表）");
            EngineConfig c2;
            c2.zoom = 1.0f;
            c2.page.width_pt = 400.0f;
            c2.page.height_pt = 300.0f;
            c2.page.margins = PageMargins::all(20.0f);
            c2.continuous = true;
            c2.page_gap_pt = 20.0f;
            e2.setConfig(c2);
            int id = 3;
            std::string txt;
            for (int i = 0; i < 40; ++i) txt += "字";
            std::vector<doc::Ref> rows;
            for (int r = 0; r < 3; ++r) {
                const doc::Ref cell = doc::makeNode(
                    id++, NodeKind::TableCell, CellSpec{},
                    {doc::makeNode(id++, NodeKind::TextRun,
                                   doc::RunPayload{utf8ToCodepoints(txt), CharFormat{}, {}}, {}),
                     doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
                rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell}));
            }
            const doc::Ref table = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows);
            const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {table});
            const NodeId root_id = id++;
            e2.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            e2.relayout();
            const PlacedTable& pt2 = e2.layout().placed_tables.at(0);
            // 每行的首片：顶边必须严格递增（行与行不重叠、也不塌回带顶）。
            std::vector<const PlacedRow*> firsts;
            for (const PlacedRow& pr : pt2.placed) {
                if (pr.cont_top) continue;
                if (!firsts.empty() && firsts.back()->row == pr.row) continue;
                firsts.push_back(&pr);
            }
            expectTrue(firsts.size() == 3,
                       "三行各有自己的首片（实际 " + std::to_string(firsts.size()) + " 片）");
            bool growing = firsts.size() == 3;
            for (size_t i = 1; i < firsts.size() && growing; ++i) {
                const PlacedRow& a = *firsts[i - 1];
                const PlacedRow& b = *firsts[i];
                const float a_bottom = a.y + (a.page - b.page == 0 ? a.h : 0.0f);
                growing = (a.page < b.page) || (a.page == b.page && b.y >= a_bottom - 0.01f);
            }
            expectTrue(growing, "连页下每一行都排在前一行下面（表没有塌回带顶）");
            expectTrue(e2.auditLayout() == 0, "连页多行表：增量排版 == 全量");
            // 先画一遍再比：底图自审读的是缓存里的像素，一页都没画过时它是空白。
            e2.repaint();
            int page_diffs = 0;
            for (int pg = 0; pg < e2.layout().stats.page_count; ++pg) {
                if (e2.auditPageJson(pg).find("\"diff\":0") == std::string::npos) ++page_diffs;
            }
            expectTrue(page_diffs == 0, "连页多行表：每一页的位图都与从零光栅相同");
            expectTrue(e2.auditBaseJson().find("\"diff\":0") != std::string::npos,
                       "连页多行表：底图自审为 0");
        }

        // 反向一半：没有跨页的小表里，点格子的空白处仍落在这一格，不会因为"格框优先"
        //   而把点字上的那一下也判成整格。
        {
            EditorEngine e3;
            expectTrue(e3.init({cjkPath, mnPath, latinPath}), "引擎初始化（小表对照）");
            e3.setConfig(c72);
            e3.setEditMode(EditMode::Document);
            e3.setDocument(doc::makeTextDoc({U"甲乙"}), StyleSheet{});
            e3.setCaret(0, 2);
            expectTrue(e3.insertTableAndRender(2, 2, false), "插一张 2x2 表");
            // 坐标从行盒取：两列都是空段，格的内框与行顶都在行盒上，比写死一个 y 稳。
            //   2x2 表占段 1..4：行 0 是 (0,0)=1 与 (0,1)=2。
            const LineBox* right_cell = nullptr;
            float row_top = 0.0f;
            for (const LineBox& lb : e3.layout().lines) {
                if (lb.paragraph == 2) {
                    right_cell = &lb;
                    row_top = lb.cell_top_pt;
                }
            }
            expectTrue(right_cell != nullptr, "前提：第一行第二格有行盒");
            if (right_cell != nullptr) {
                const float y = row_top + 3.0f;
                TextPos hit{};
                const bool ok = e3.hitTestPt(right_cell->cell_box_lo_pt + 20.0f, y, &hit);
                expectTrue(ok && hit.para == 2, "点第一行第二格落在它的段（实际 " +
                                                    std::to_string(ok ? hit.para : -1) + "）");
                TextPos own{};
                const bool ok_own = e3.hitTestPt(30.0f, y, &own);
                expectTrue(ok_own && own.para == 1, "点第一行第一格仍落在它自己那一格（实际 " +
                                                        std::to_string(ok_own ? own.para : -1) +
                                                        "）");
            }
        }
    }

    if (runSection(73)) std::printf("\n[73] 表格里的页索引与行带：标记、折行、逐字增量重画\n");
    if (runSection(73)) {
        // 表里有两件事不能按"数组相邻 = 版面相邻"推：
        //   一、行数组按流顺序排，跨页切开的那一行之后，同一行其余各格的行还在上一页
        //     —— 按页号二分取"这一页的行"会整段漏掉（标记不画、字形不画、页被判成新鲜）。
        //   二、高格的第二行之后，数组里紧跟着的是另一格的第一行（在它上面）——
        //     按它推行带边界会把带压成零高，那一行从此不再重画（屏幕上堆着旧标记）。
        // 这一节把两条都钉住：页索引自洽、每个空格在它自己那一页上都有标记的墨、
        //   格内逐字输入时每一步的页位图都等于从零光栅。
        EngineConfig c73;
        c73.zoom = 1.0f;
        c73.page.width_pt = 420.0f;
        c73.page.height_pt = 330.0f;
        c73.page.margins = PageMargins::all(20.0f);

        // 数一个矩形里的墨（页位图，只数深色像素）。坐标是页内设备像素。
        auto inkIn = [](const EditorEngine& eng, int page, int x0, int y0, int x1, int y1) {
            const uint8_t* px = eng.pagePixels(page);
            if (px == nullptr) return 0;
            const int pw = eng.pagePxWidth();
            const int ph = eng.pagePxHeight();
            int dark = 0;
            for (int y = std::max(0, y0); y < std::min(ph, y1); ++y) {
                for (int x = std::max(0, x0); x < std::min(pw, x1); ++x) {
                    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(pw) +
                                       static_cast<size_t>(x)) *
                                      4u;
                    if (px[at] < 200) ++dark;
                }
            }
            return dark;
        };

        // (a) 跨页表：行数组确实不按页单调（前提），页索引仍把每一行、每个字形
        //     归到它自己那一页，且一个不漏。
        {
            EditorEngine e;
            expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（页索引）");
            e.setConfig(c73);
            e.setEditMode(EditMode::Document);
            {
                e.setDocument(doc::makeTextDoc({U"首段"}), StyleSheet{});
            }
            e.setCaret(0, 2);
            expectTrue(e.insertTableAndRender(2, 5, false), "插 2x5 表");
            {
                // 第一格塞到跨页；第二行第一格放两行文字。
                e.setCaret(1, 0);
                e.typeTextAndRender("但是发个sdf");
                for (int i = 0; i < 15; ++i) e.splitParagraphAndRender();
            }
            e.relayout();
            const LayoutResult& lay = e.layout();
            int inversions = 0;
            int prev_page = -1;
            for (const LineBox& lb : lay.lines) {
                if (lb.page < prev_page) ++inversions;
                prev_page = lb.page;
            }
            expectTrue(inversions > 0, "前提：跨页表里行数组不按页单调（实际 " +
                                           std::to_string(inversions) + " 处逆序）");
            // 索引覆盖：每一行恰好出现一次，且出现的那一页就是它自己的页。
            int covered = 0;
            int misfiled = 0;
            for (size_t p = 0; p < lay.fragments.page_lines.size(); ++p) {
                for (int k : lay.fragments.page_lines[p]) {
                    ++covered;
                    if (lay.lines[static_cast<size_t>(k)].page != static_cast<int>(p)) ++misfiled;
                }
            }
            expectTrue(covered == static_cast<int>(lay.lines.size()) && misfiled == 0,
                       "页索引把每一行都归到它自己那一页（覆盖 " + std::to_string(covered) + "/" +
                           std::to_string(lay.lines.size()) + "，错位 " + std::to_string(misfiled) +
                           "）");
            int gcovered = 0;
            int gmisfiled = 0;
            for (size_t p = 0; p < lay.fragments.page_glyphs.size(); ++p) {
                for (int k : lay.fragments.page_glyphs[p]) {
                    ++gcovered;
                    if (lay.glyphs[static_cast<size_t>(k)].page != static_cast<int>(p)) ++gmisfiled;
                }
            }
            expectTrue(gcovered == static_cast<int>(lay.glyphs.size()) && gmisfiled == 0,
                       "页索引把每个字形都归到它自己那一页（覆盖 " + std::to_string(gcovered) +
                           "/" + std::to_string(lay.glyphs.size()) + "）");

            // 每一页、每一个空段的行，都要有段落标记的墨。空段没有字形，标记是
            //   它在这一页上唯一画得出来的东西 —— 缺了就是"空行看不见"。
            e.repaint();
            int empty_lines = 0;
            int marked = 0;
            std::string missing;
            for (const LineBox& lb : lay.lines) {
                if (lb.paragraph < 0 || lb.paragraph >= static_cast<int>(e.blocks().size()))
                    continue;
                const std::vector<uint32_t>& tx =
                    e.blocks()[static_cast<size_t>(lb.paragraph)].cp_text;
                if (!tx.empty() || lb.last_cp != 0) continue;
                ++empty_lines;
                const int x = static_cast<int>(lb.inline_origin_pt);
                const int y = static_cast<int>(lb.bracket_axis_pt - lb.size_pt);
                if (inkIn(e, lb.page, x, y, x + 14, y + 20) > 0) {
                    ++marked;
                } else if (missing.size() < 80) {
                    missing +=
                        " p" + std::to_string(lb.page) + "/para" + std::to_string(lb.paragraph);
                }
            }
            expectTrue(empty_lines >= 10,
                       "前提：表里有一批空段（实际 " + std::to_string(empty_lines) + " 条空行）");
            expectTrue(marked == empty_lines, "每条空行的段落标记都画在它自己那一页上（" +
                                                  std::to_string(marked) + "/" +
                                                  std::to_string(empty_lines) + missing + "）");
        }

        // (b) 格内两行文字：逐字输入，每一步的页位图都必须等于从零光栅。
        //     第二行之后行带若被压成零高，这一步会红（旧代码只红在第 14 个字那一步）。
        {
            EditorEngine e;
            expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（格内逐字）");
            e.setConfig(c73);
            e.setEditMode(EditMode::Document);
            {
                e.setDocument(doc::makeTextDoc({U"首段"}), StyleSheet{});
            }
            e.setCaret(0, 2);
            expectTrue(e.insertTableAndRender(2, 5, false), "插 2x5 表");
            e.setCaret(1, 0);
            const std::string line = "asdf asdf asdf";
            int stale = 0;
            std::string first_bad;
            for (size_t i = 0; i < line.size(); ++i) {
                e.typeTextAndRender(std::string(1, line[i]));
                const int pages = std::max(1, e.layout().stats.page_count);
                for (int pg = 0; pg < pages; ++pg) {
                    const std::string j = e.auditPageJson(pg);
                    if (j.find("\"diff\":0") != std::string::npos) continue;
                    ++stale;
                    if (first_bad.empty()) first_bad = j;
                }
            }
            expectTrue(stale == 0, "格内逐字输入：每一步每一页都与从零光栅相同（不自洽 " +
                                       std::to_string(stale) + " 次 " + first_bad + "）");
            // 折出来的第二行必须有墨：行带塌掉时这一条会红。
            e.relayout();
            e.repaint();
            int second_lines = 0;
            int inked = 0;
            for (const LineBox& lb : e.layout().lines) {
                if (lb.paragraph != 1 || lb.first_cp == 0) continue;
                ++second_lines;
                const int x0 = static_cast<int>(lb.inline_origin_pt);
                const int y0 = static_cast<int>(lb.bracket_axis_pt - lb.size_pt);
                const int x1 = static_cast<int>(lb.inline_origin_pt + lb.inline_end_pt);
                if (inkIn(e, lb.page, x0, y0, x1 + 2, y0 + 20) > 20) ++inked;
            }
            expectTrue(second_lines >= 1, "前提：格内文字折出了第二行（实际 " +
                                              std::to_string(second_lines) + " 行）");
            expectTrue(inked == second_lines, "折出来的每一行都有字（" + std::to_string(inked) +
                                                  "/" + std::to_string(second_lines) + "）");
        }

        // (c) 反向一半：不在表里的普通段落，逐字输入同样不许留旧墨迹。
        {
            EditorEngine e;
            expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（正文逐字）");
            e.setConfig(c73);
            e.setDocument(doc::makeTextDoc({U""}), StyleSheet{});
            e.setCaret(0, 0);
            int stale = 0;
            for (int i = 0; i < 12; ++i) {
                e.typeTextAndRender("字");
                const std::string j = e.auditPageJson(0);
                if (j.find("\"diff\":0") == std::string::npos) ++stale;
            }
            expectTrue(stale == 0,
                       "正文里逐字输入也一步不留旧墨迹（不自洽 " + std::to_string(stale) + " 次）");
        }
    }

    if (runSection(74)) std::printf("\n[74] 跨页表格：顺着同一列拖到页间，不许铺满整行\n");
    if (runSection(74)) {
        // 跨页切开的那一行，页底最靠下的行盒全在高的那一列上。指针落进页底边距或页间
        //   空隙时按"最近的一行"取落点，就永远取到那一列 —— 从别的列往下拖，矩形包络
        //   于是横跨整行，看着像"一不小心把整行都选中了"。落点该按指针的横坐标认列。
        EngineConfig c74;
        c74.zoom = 1.0f;
        c74.page.width_pt = 400.0f;
        c74.page.height_pt = 300.0f;
        c74.page.margins = PageMargins::all(20.0f);

        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（跨页拖选）");
        e.setConfig(c74);
        e.setEditMode(EditMode::Document);
        {
            e.setDocument(doc::makeTextDoc({U"首段"}), StyleSheet{});
        }
        e.setCaret(0, 2);
        expectTrue(e.insertTableAndRender(3, 3, false), "插一张 3x3 表");
        for (int i = 0; i < 34; ++i) {
            e.typeTextAndRender("第一格第" + std::to_string(i) + "行");
            e.splitParagraphAndRender();
        }
        e.relayout();
        const PlacedTable& pt = e.layout().placed_tables.at(0);
        const LayoutCell* c0 = pt.cellAt(0, 0, nullptr, nullptr);
        const LayoutCell* c2 = pt.cellAt(0, 2, nullptr, nullptr);
        expectTrue(c0 != nullptr && c2 != nullptr, "前提：第 1 行三格都能取到");
        const float col_w = pt.col_x[3] - pt.col_x[2];
        // 第三列的中心：指针从头到尾没离开过这一列。
        const float x_third = pt.x + 0.5f * (pt.col_x[2] + pt.col_x[3]);
        // 两个纵向落点：页底边距里、真正的页间空隙里（后者要靠 clamp 夹回来）。
        const float ys[] = {c74.page.height_pt - 4.0f, c74.page.height_pt + 7.0f};
        // 拖选一条：从第三格开头拖到 (x, y)。
        auto dragFromThird = [&](float x, float y) -> bool {
            e.setCaret(c2->first_block, 0);
            TextPos hit{};
            int side = 0;
            int line_aff = 0;
            if (!e.hitTestPt(x, y, &hit, nullptr, &side, &line_aff, nullptr, true)) return false;
            e.setFocusAtSideLine(hit.para, hit.offset, side, line_aff);
            return true;
        };
        int hits = 0;
        int stayed = 0;
        int whole_row = 0;
        std::string detail;
        for (float y : ys) {
            if (!dragFromThird(x_third, y)) continue;
            ++hits;
            // 焦点仍在第三格：第三格是一段空段，落点只能是它的开头。
            if (e.caret().para >= c2->first_block &&
                e.caret().para < c2->first_block + c2->block_count)
                ++stayed;
            // 整格选区不该出现（两端在同一格），更不该有一条横跨两列以上的矩形。
            bool wide = e.selectionIsCell();
            for (const PageRect& r : e.selectionRects())
                if (r.r_pt.w > col_w + 1.0f) wide = true;
            if (wide) {
                ++whole_row;
                detail += " y=" + std::to_string(static_cast<int>(y));
            }
        }
        expectTrue(hits == 2, "前提：两种纵向落点都命中了（实际 " + std::to_string(hits) + "）");
        expectTrue(stayed == hits, "★ 顺着同一列拖到页间，落点仍在这一列（" +
                                       std::to_string(stayed) + "/" + std::to_string(hits) + "）");
        expectTrue(whole_row == 0, "★ 页底边距与页间空隙都不把选区铺成整行（越界 " +
                                       std::to_string(whole_row) + " 次" + detail + "）");

        // 反向一半：横坐标落在第一列时，落点该回到第一列 —— 不能一律判给"最靠下的那一行"。
        int first_col_ok = 0;
        for (float y : ys) {
            if (!dragFromThird(pt.x + 10.0f, y)) continue;
            if (e.caret().para >= c0->first_block &&
                e.caret().para < c0->first_block + c0->block_count)
                ++first_col_ok;
        }
        expectTrue(first_col_ok == hits, "★ 落点按指针的列走：第一列上的点落回第一列（" +
                                             std::to_string(first_col_ok) + "/" +
                                             std::to_string(hits) + "）");
    }

    if (runSection(75)) std::printf("\n[75] 页边距裁剪标记：四角有墨、边上没有、只改光栅\n");
    if (runSection(75)) {
        // 标记标的是正文可用区（页边距的内沿）。它画进页位图，所以判据有三半：
        //   四角有墨、四条边的中段没有墨（不是"画了一个框"）、关掉之后墨消失。
        EngineConfig c75;
        c75.zoom = 1.0f;
        c75.page.width_pt = 400.0f;
        c75.page.height_pt = 300.0f;
        c75.page.margins = PageMargins::all(20.0f);

        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（裁剪标记）");
        e.setConfig(c75);
        {
            e.setDocument(doc::makeTextDoc({U"甲"}), StyleSheet{});
        }
        e.repaint();
        auto inkIn = [](const EditorEngine& eng, int page, int x0, int y0, int x1, int y1) {
            const uint8_t* px = eng.pagePixels(page);
            if (px == nullptr) return 0;
            const int pw = eng.pagePxWidth();
            const int ph = eng.pagePxHeight();
            int dark = 0;
            for (int y = std::max(0, y0); y < std::min(ph, y1); ++y) {
                for (int x = std::max(0, x0); x < std::min(pw, x1); ++x) {
                    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(pw) +
                                       static_cast<size_t>(x)) *
                                      4u;
                    if (px[at] < 200) ++dark;
                }
            }
            return dark;
        };
        // 四角的取样：只取页边距那一侧的两条臂（各 len 磅长）。窗口不能伸进正文 ——
        //   第一个字就贴在内容框的左上角上，伸进去会把字算成标记。
        auto cornerInk = [&](const EditorEngine& eng) {
            const int m = static_cast<int>(c75.page.margins.top);
            const int len = 18;  // kMarginMarkPt = 18 磅，本节的缩放是 1
            const int x1 = eng.pagePxWidth() - m;
            const int y1 = eng.pagePxHeight() - m;
            auto h = [&](int x, int y, int w) { return inkIn(eng, 0, x, y, x + w, y + 1); };
            auto v = [&](int x, int y, int hh) { return inkIn(eng, 0, x, y, x + 1, y + hh); };
            return std::array<int, 4>{h(m - len, m, len + 1) + v(m, m - len, len + 1),
                                      h(x1, m, len + 1) + v(x1, m - len, len + 1),
                                      h(m - len, y1, len + 1) + v(m, y1, len + 1),
                                      h(x1, y1, len + 1) + v(x1, y1, len + 1)};
        };
        {
            // 默认打开：四角有墨，边的中段没有（标的是四个角，不是整个框）。
            const auto ink = cornerInk(e);
            expectTrue(ink[0] > 0 && ink[1] > 0 && ink[2] > 0 && ink[3] > 0,
                       "★ 默认打开：四个角都有墨（实际 " + std::to_string(ink[0]) + "/" +
                           std::to_string(ink[1]) + "/" + std::to_string(ink[2]) + "/" +
                           std::to_string(ink[3]) + "）");
            const int w = e.pagePxWidth();
            const int h = e.pagePxHeight();
            const int mid_x = w / 2;
            const int mid_y = h / 2;
            const int m = static_cast<int>(c75.page.margins.top);
            const int edge = inkIn(e, 0, mid_x - 8, m - 2, mid_x + 8, m + 8) +
                             inkIn(e, 0, mid_x - 8, h - m - 8, mid_x + 8, h - m + 2) +
                             inkIn(e, 0, m - 2, mid_y - 8, m + 8, mid_y + 8) +
                             inkIn(e, 0, w - m - 8, mid_y - 8, w - m + 2, mid_y + 8);
            expectTrue(edge == 0, "★ 四条边的中段没有墨：标的是四个角，不是整个框（实际 " +
                                      std::to_string(edge) + "）");
            expectTrue(e.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                       "打开之后页位图 == 从零光栅（" + e.auditPageJson(0) + "）");
        }
        {
            // 关掉：签名一变，同一份缓存就不能再贴 —— 墨必须消失。
            EngineConfig off_cfg = c75;
            off_cfg.show_margin_marks = false;
            e.setConfig(off_cfg);
            e.repaint();
            const auto off = cornerInk(e);
            expectTrue(off[0] == 0 && off[1] == 0 && off[2] == 0 && off[3] == 0,
                       "★ 关掉之后四角的墨消失（缓存按光栅签名作废）（实际 " +
                           std::to_string(off[0]) + "/" + std::to_string(off[1]) + "/" +
                           std::to_string(off[2]) + "/" + std::to_string(off[3]) + "）");
        }
        {
            // 再打开：墨回来（同上，仍然是签名在管缓存）。
            e.setConfig(c75);
            e.repaint();
            const auto on = cornerInk(e);
            expectTrue(on[0] > 0 && on[1] > 0 && on[2] > 0 && on[3] > 0, "★ 再打开，四角的墨回来");
        }
        {
            // 连页下不画：那里没有页，四角没有意义。这一条用差分判 —— 正文与段落标记就画在
            //   左上角那一窗里，绝对值分不清"标记没画"与"语料本来有墨"。
            EngineConfig cont = c75;
            cont.continuous = true;
            cont.show_margin_marks = true;
            e.setConfig(cont);
            e.repaint();
            const auto on = cornerInk(e);
            cont.show_margin_marks = false;
            e.setConfig(cont);
            e.repaint();
            const auto off = cornerInk(e);
            bool same = true;
            for (size_t k = 0; k < on.size(); ++k)
                if (on[k] != off[k]) same = false;
            expectTrue(same, "连页模式下不画裁剪标记（开 " + std::to_string(on[0]) + "/" +
                                 std::to_string(on[1]) + "/" + std::to_string(on[2]) + "/" +
                                 std::to_string(on[3]) + "，关 " + std::to_string(off[0]) + "/" +
                                 std::to_string(off[1]) + "/" + std::to_string(off[2]) + "/" +
                                 std::to_string(off[3]) + "）");
        }
    }

    if (runSection(76)) std::printf("\n[76] 空控件嵌套：光标进得去每一层的内容区间\n");
    if (runSection(76)) {
        // 三个空控件套在一起（[ [ [ ] ] ]）：六枚括号压在同一处文字偏移上。
        //   内容区间是 (开括号之后, 闭括号之前)，所以外层跨到最外那枚 ] 之前、内层退化成
        //   一点。判据按"每一步都进得去"走 —— 只判终点会漏掉中间那几格。
        EngineConfig c76;
        c76.zoom = 1.0f;
        const std::vector<std::string> fonts76 = {cjkPath, mnPath, latinPath};

        const doc::Doc t76 = doc::makeTextDoc({U"ABC"});
        EditorEngine e76;
        if (!e76.init(fonts76)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e76.setConfig(c76);
        e76.setDocument(t76, StyleSheet{});
        e76.relayout();

        ControlSpec sp76;
        sp76.key = "outer";
        sp76.controlType = "text";
        e76.setCaret(0, 1);
        e76.insertControlAndRender(sp76, "");
        sp76.key = "mid";
        e76.insertControlAndRender(sp76, "");
        sp76.key = "inner";
        e76.insertControlAndRender(sp76, "");
        expectTrue(e76.controls().size() == 3,
                   "三个空控件（实际 " + std::to_string(e76.controls().size()) + "）");
        e76.setEditMode(EditMode::Form);
        e76.relayout();

        auto at76 = [&]() {
            return std::to_string(e76.caret().offset) + "/" + std::to_string(e76.caretSide());
        };
        // 流：A(0) [(1) [(2) [(3) ](4) ](5) ](6) B(7) C(8)；三层的内容区间分别是
        //   内层 [4,4]、中层 [3,5]、外层 [2,6]。光标读数里的侧别就是 cp 处的第几格，
        //   于是这一处的 1/3、1/4、1/5 分别对应流位置 4、5、6。
        //   方向键每一步都要进得去 —— 只判终点会漏掉中间那一格，而"进不去"正是从中间开始的。
        //   每条都先把结果落到字符串再判：断言的两个实参求值顺序不确定，把移动写进判据里
        //   会让失败信息印出移动之前的读数。
        // 归属只该有一个答案：宿主报的那个（caretFieldHint）与落点判据用的那个
        //   （innermostControlAt）必须是同一个。两者现在读同一份查询面（ControlSpans），
        //   这条断言钉的是"以后谁把其中一个改回自己那套反推"——分家的症状是
        //   "光标画在 A 里、宿主说在 B 里、编辑按 C 判"。
        auto ownerAgree = [&]() {
            return e76.caretFieldHint() == e76.innermostControlAt(e76.caretFlowPos());
        };
        expectTrue(ownerAgree(), "装载之后两套归属算法给出同一个控件");
        auto step76 = [&](int dcp) {
            const bool m = e76.moveCaretAndRender(dcp);
            return std::string(m ? "动" : "停") + at76();
        };
        const std::string s0 = at76();
        expectTrue(s0 == "1/3", "装载之后光标落在最内层的内容起点（实际 " + s0 + "）");
        expectTrue(e76.caretInsertable(), "那个位置可插入（三层里最内层的内容起点）");

        const std::string r1 = step76(1);
        const bool a1 = ownerAgree();
        const std::string r2 = step76(1);
        const bool a2 = ownerAgree();
        const std::string r3 = step76(1);
        const bool a3 = ownerAgree();
        expectTrue(a1 && a2 && a3, "右移三格的每一格上两套归属算法都一致");
        expectTrue(r1 == "动1/4", "★ 右移一格进中层的内容末尾（实际 " + r1 + "）");
        expectTrue(r2 == "动1/5", "★ 再右移进外层的内容末尾（实际 " + r2 + "）");
        expectTrue(r3 == "停1/5",
                   "★ 再右移：右邻是外层的闭括号（不可插入）→ 不动（实际 " + r3 + "）");

        const std::string l1 = step76(-1);
        const std::string l2 = step76(-1);
        const std::string l3 = step76(-1);
        const std::string l4 = step76(-1);
        expectTrue(l1 == "动1/4", "★ 反向：左移回到中层的内容末尾（实际 " + l1 + "）");
        expectTrue(l2 == "动1/3", "★ 再左移回到最内层的内容起点（实际 " + l2 + "）");
        expectTrue(l3 == "动1/2", "★ 再左移进中层的内容起点（实际 " + l3 + "）");
        expectTrue(l4 == "动1/1", "★ 再左移进外层的内容起点（实际 " + l4 + "）");
        const std::string l5 = step76(-1);
        expectTrue(l5 == "停1/1",
                   "★ 再左移：左邻是外层的开括号（不可插入）→ 不动（实际 " + l5 + "）");

        // 外层内容起点上退格：左邻是开括号，删不过去（表单模式的删除只在控件内容里）。
        const bool bs76 = e76.backspaceAndRender();
        const std::string after_bs = at76();
        expectTrue(!bs76 && after_bs == "1/1",
                   "★ 外层内容起点上退格被拒：删不到括号，光标也不动（实际 " + after_bs + "）");

        // 点击最内层的内容起点：进得去（三层共享同一处文字偏移，靠侧别分辨层级）。
        e76.setCaretAtSide(0, 1, 3);
        const std::string c1 = at76();
        expectTrue(c1 == "1/3", "★ 点击最内层的内容起点：进得去（实际 " + c1 + "）");

        // 只读内层：插入点按最内层判 —— 外层可编辑不能替内层作证。
        expectTrue(e76.setControlProp(2, ControlProp::Readonly, true), "把最内层设成只读");
        e76.relayout();
        e76.setCaretAtSide(0, 1, 2);
        const std::string c2 = at76();
        expectTrue(c2 == "1/2", "点击中层的内容起点（实际 " + c2 + "）");
        e76.setCaretAtSide(0, 1, 3);
        const std::string c3 = at76();
        expectTrue(c3 == "1/2", "★ 再点击只读内层：进不去，光标留在中层（实际 " + c3 + "）");
        const bool m1 = e76.moveCaretAndRender(1);
        const std::string c4 = at76();
        expectTrue(!m1 && c4 == "1/2",
                   "★ 从中层内容起点右移：下一格是只读内层 → 不动（实际 " + c4 + "）");
    }

    if (runSection(78)) std::printf("\n[78] 性能：增量对全量的耗时比（只判比值，独占跑）\n");
    if (runSection(78)) {
        // 这一节只判比值：分子分母来自同一次测量，机器快慢互相抵消。绝对值会被同机上的
        //   其它负载顶翻（同一份代码实测能从 0.5 翻到 0.9 ms），所以只打印、不断言。
        // 内容与第 15 节同源，只是那几段计时搬到了这里 —— 日常全量不再为它们付时间，
        //   也就不会因为隔壁在跑别的东西而变红。
        EditorEngine e;
        if (!e.init({cjkPath, mnPath, latinPath})) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        EngineConfig c;
        c.zoom = 1.0f;
        e.setConfig(c);

        const int kParas = 200;
        std::vector<BlockSpec> doc;
        for (int i = 0; i < kParas; ++i) {
            BlockSpec b;
            b.font_index = 0;
            b.size_pt = 14.0f;
            b.line_spacing.factor = 1.85f;
            b.space_after_pt = 6.0f;
            b.language = "zh";
            b.setText("第 " + std::to_string(i + 1) +
                      " 次病程记录：患者诉晨起后心悸明显，持续约十分钟，休息后缓解，"
                      "未见黑矇与晕厥。");
            doc.push_back(std::move(b));
        }
        loadBlocks(e, doc);
        e.relayout();
        e.repaint();
        expectTrue(e.editStats().layout_was_full, "前提：首次排版是全量（整篇都脏）");

        // 一、排版本身：全量、尾部敲字、中部敲字各测 5 次取中位数。
        //   单次 layout_ms 的噪声很大（系统调度与缓存抖动都能差出一倍以上），
        //   所以两边各测 5 次取中位数再比 —— 报出去的绝对值也必须是稳的。
        //   全量的求法：没有脏段时 relayout() 按全量处理（见 engine/engine.h 的说明）。
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
            "     中位数：全量重排 %.3f ms ｜ 尾部敲字 %.3f ms（%.1f%%）｜ 中部敲字 %.3f ms"
            "（%.1f%%）\n",
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

        // 二、端到端：排版 + 统计同步 + 页指纹 + 上屏。
        //   layout_ms 只量排版本身，看不到另外三项 —— 而用户感觉到的是这一整条。
        //   每次测完把文档改回原样，恢复不参与测量。
        const std::vector<BlockSpec> pristine = e.blocks();
        auto restore = [&]() {
            loadBlocks(e, pristine);
            e.relayout();
            e.repaint();
        };
        auto e2e = [&](int para, size_t off, const std::string& text) {
            e.setCaret(para, off);
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
            "             撑出一行（后缀要跟着动）：尾部 %.3f ms（%.1f%%）｜ 中部 %.3f ms"
            "（%.1f%%）\n",
            e_wt, 100.0 * e_wt / e_full, e_wm, 100.0 * e_wm / e_full);
        expectTrue(e_full > 0.0, "前提：端到端全量可以量");

        // 三、后缀平移复用对续排重排：后缀里塞一个竖排段就把平移路径关掉，于是后缀必须
        //   真重排。两边各测 3 次取最小值（最小值对负载尖峰最稳）。
        std::vector<BlockSpec> with_vertical = pristine;
        {
            BlockSpec v;
            v.font_index = 0;
            v.size_pt = 14.0f;
            v.line_spacing.factor = 1.9f;
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

            loadBlocks(e, with_vertical);
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
        const double shift_ms = *std::min_element(v_shift.begin(), v_shift.end());
        const double resume_ms = *std::min_element(v_resume.begin(), v_resume.end());
        std::printf("     后缀 100 段：平移复用 %.3f ms ｜ 续排重排 %.3f ms（%.0f%%）\n", shift_ms,
                    resume_ms, 100.0 * shift_ms / std::max(resume_ms, 1e-9));
        expectTrue(shift_ms * 1.5 < resume_ms,
                   "★ 后缀平移复用比续排重排快 1.5 倍以上（断行与字形放置全省掉）");
    }

    if (runSection(79)) std::printf("\n[79] 图片：资源注册、插入、命中、改尺寸\n");
    if (runSection(79)) {
        EngineConfig c79;
        c79.zoom = 1.0f;
        const std::vector<std::string> fonts79 = {cjkPath, mnPath, latinPath};

        const doc::Doc t79 = doc::makeTextDoc({U"ABCD"});
        EditorEngine e79;
        if (!e79.init(fonts79)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e79.setConfig(c79);
        e79.setDocument(t79, StyleSheet{});
        e79.relayout();

        // 单色块：缩放采样之后中心仍是同一个颜色，判据不必挑采样算法。
        const int iw = 96;
        const int ih = 48;
        std::vector<uint8_t> pixels(static_cast<size_t>(iw) * ih * 4u);
        for (int i = 0; i < iw * ih; ++i) {
            pixels[static_cast<size_t>(i) * 4u + 0] = 200;
            pixels[static_cast<size_t>(i) * 4u + 1] = 30;
            pixels[static_cast<size_t>(i) * 4u + 2] = 30;
            pixels[static_cast<size_t>(i) * 4u + 3] = 255;
        }
        std::string err79;
        expectTrue(e79.registerImageAndRepaint("pic", iw, ih, pixels.data(), pixels.size(), &err79),
                   "注册一张 96x48 的图（" + err79 + "）");
        expectTrue(e79.imageCount() == 1, "资源表里有一张");
        expectTrue(e79.imageBytes() == pixels.size(), "资源表按字节记账");

        e79.setCaret(0, 2);
        ImageSpec im79;
        im79.resource_id = "pic";
        im79.px_w = iw;
        im79.px_h = ih;
        im79.dpi = 96;
        expectTrue(e79.insertImageAndRender(im79), "插进文档");
        std::vector<EditorEngine::ImageBox> boxes79 = e79.images();
        expectTrue(boxes79.size() == 1, "文档里一张图");
        if (boxes79.size() == 1) {
            const float w = boxes79[0].w_pt;
            const float h = boxes79[0].h_pt;
            expectTrue(std::fabs(w - 72.0f) < 0.01f && std::fabs(h - 36.0f) < 0.01f,
                       "Auto 按 DPI 折算：96x48 像素 @96dpi = 72 x 36 磅（实际 " +
                           std::to_string(w) + " x " + std::to_string(h) + "）");
            expectTrue(boxes79[0].resource_ready, "像素在资源表里");
            expectTrue(!boxes79[0].rects.empty(), "它落在某一页上");
        }

        // 命中：图心在图身上，左上角在 0 号把手上。
        if (!boxes79.empty() && !boxes79[0].rects.empty()) {
            const Rect r79 = boxes79[0].rects[0].r_pt;
            const EditorEngine::ImageHit body79 =
                e79.imageAtPt(r79.x + r79.w * 0.5f, r79.y + r79.h * 0.5f);
            expectTrue(body79.index == 0 && body79.handle < 0, "点图身：命中这张图、不在把手上");
            const EditorEngine::ImageHit corner79 = e79.imageAtPt(r79.x, r79.y);
            expectTrue(corner79.index == 0 && corner79.handle == 0, "点左上角：命中 0 号把手");
            expectTrue(e79.imageAtPt(r79.x + r79.w + 40.0f, r79.y + r79.h + 40.0f).index < 0,
                       "点在图外：什么都没命中");
            // 把手命中区就是画出来的那个方块：顺着边线拉长会让整条右边线都变成把手，
            //   贴着边线想落光标的地方就再也落不进去。
            const EditorEngine::ImageHit mid79 = e79.imageAtPt(r79.x + r79.w, r79.y + r79.h * 0.5f);
            expectTrue(mid79.index == 0 && mid79.handle == 3, "右中把手中心命中 3 号把手");
            const EditorEngine::ImageHit edge79 =
                e79.imageAtPt(r79.x + r79.w, r79.y + r79.h * 0.5f + 10.0f);
            expectTrue(edge79.index == 0 && edge79.handle < 0,
                       "边线上离把手 10 磅处不是把手（只算图身）");
            expectTrue(e79.imageAtPt(r79.x + r79.w + 20.0f, r79.y + r79.h * 0.5f).index < 0,
                       "右把手的横向容差没有放大");
        }

        // 真画上去了没有：图心那个像素必须是图里的颜色。
        if (!boxes79.empty() && !boxes79[0].rects.empty()) {
            const Rect r79 = boxes79[0].rects[0].r_pt;
            const int fw = e79.frameWidth();
            const int step = e79.pagePxHeight() + e79.pageGapPx();
            const int cx = static_cast<int>(std::lround((r79.x + r79.w * 0.5f) * c79.zoom));
            const int cy = static_cast<int>(std::lround((r79.y + r79.h * 0.5f) * c79.zoom));
            const uint8_t* fp = e79.frame();
            const size_t at =
                (static_cast<size_t>(cy) * static_cast<size_t>(fw) + static_cast<size_t>(cx)) * 4u;
            (void)step;
            const bool red = fp[at] > 180 && fp[at + 1] < 90 && fp[at + 2] < 90;
            expectTrue(red, "图心像素是图里的颜色（不是占位框的底色）");
        }

        // 资源没注册时仍然插得进去，画面上是占位框。
        ImageSpec ghost79;
        ghost79.resource_id = "missing";
        ghost79.px_w = 20;
        ghost79.px_h = 10;
        expectTrue(e79.insertImageAndRender(ghost79), "没注册像素的图也插得进去");
        boxes79 = e79.images();
        expectTrue(boxes79.size() == 2 && !boxes79[1].resource_ready,
                   "第二张的像素不在资源表里（画面上是占位框）");
        expectTrue(e79.undoAndRender(), "撤销这次插入");
        expectTrue(e79.images().size() == 1, "撤销之后只剩一张");

        // 改尺寸：一次拖拽只留一步（合并提交）。
        expectTrue(e79.selectImageAndRender(0), "选中这张图");
        expectTrue(e79.selectedImage() == 0, "选中的是第 0 张");
        const int depth0 = e79.undoDepth();
        expectTrue(e79.resizeImageAndRender(0, 30.0f, 15.0f), "改成 30 x 15 磅");
        const int depth1 = e79.undoDepth();
        expectTrue(depth1 == depth0 + 1, "改尺寸在撤销栈里留下一步");
        expectTrue(e79.resizeImageAndRender(0, 60.0f, 30.0f, /*merge=*/true),
                   "拖拽的后续一帧（合并提交）");
        expectTrue(e79.undoDepth() == depth1, "合并提交不增加撤销步数");
        expectTrue(std::fabs(e79.images()[0].w_pt - 60.0f) < 0.01f, "尺寸落在 60 磅上");
        expectTrue(e79.undoAndRender(), "撤销");
        expectTrue(std::fabs(e79.images()[0].w_pt - 72.0f) < 0.01f,
                   "一次撤销回到拖拽之前（72 磅），不是回到中间那一帧");

        // 表单模式不放行：改尺寸是设计动作。
        e79.setEditMode(EditMode::Form);
        e79.relayout();
        expectTrue(!e79.resizeImage(0, 40.0f, 20.0f), "表单模式拒绝改图片尺寸");
        e79.setEditMode(EditMode::Document);
        e79.relayout();
        expectTrue(std::fabs(e79.images()[0].w_pt - 72.0f) < 0.01f, "被拒之后尺寸一个字都没动");
    }

    if (runSection(80)) std::printf("\n[80] 控件：八种类型、取值、切换与光标屏障\n");
    if (runSection(80)) {
        EngineConfig c80;
        c80.zoom = 1.0f;
        const std::vector<std::string> fonts80 = {cjkPath, mnPath, latinPath};

        const doc::Doc t80 = doc::makeTextDoc({U"ABCD"});
        EditorEngine e80;
        if (!e80.init(fonts80)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e80.setConfig(c80);
        e80.setDocument(t80, StyleSheet{});
        e80.relayout();

        const std::vector<ControlOption> opts80 = {
            {"a", "选项甲"}, {"b", "选项乙"}, {"c", "选项丙"}};
        auto insert80 = [&](const std::string& type, const std::string& label,
                            const std::vector<ControlOption>& options, const std::string& group) {
            ControlSpec spec = makeControlSpec(type);
            if (auto* c = std::get_if<ChoiceDetail>(&spec.detail)) {
                c->options = options;
                c->group = group;
            }
            if (auto* d = std::get_if<DropdownDetail>(&spec.detail)) d->options = options;
            // 插到文末：插在控件内容末尾会落进上一个控件里（容器允许嵌套，那是设计使然）。
            e80.moveCaretToDocEnd();
            return e80.insertControlAndRender(spec, label);
        };

        expectTrue(insert80("text", "文本", {}, ""), "插入文本输入");
        expectTrue(insert80("number", "0", {}, ""), "插入数值输入");
        // 插完光标就在内容末尾：正好用来验数值过滤。
        expectTrue(!e80.typeTextUtf8("abc"), "数值控件拒收字母");
        expectTrue(e80.typeTextUtf8(".5"), "数值控件收小数点与数字");
        expectTrue(!e80.typeTextUtf8("a"), "再打字母仍然被拒");
        expectTrue(!e80.typeTextUtf8(".."), "第二个小数点被拒");
        expectTrue(e80.controlViews()[1].content == "0.5", "内容只留下数字那部分");
        expectTrue(insert80("radioButton", "甲", {}, "g1"), "插入单选甲");
        expectTrue(insert80("radioButton", "乙", {}, "g1"), "插入单选乙");
        expectTrue(insert80("checkBox", "同意", {}, ""), "插入复选");
        expectTrue(insert80("radioGroup", "", opts80, ""), "插入单选组");
        expectTrue(insert80("checkBoxGroup", "", opts80, ""), "插入复选组");
        expectTrue(insert80("dropdownList", "", opts80, ""), "插入下拉");
        expectTrue(insert80("date", "", {}, ""), "插入时间选择");

        std::vector<EditorEngine::ControlView> v80 = e80.controlViews();
        expectTrue(v80.size() == 9, "文档里九个控件（实际 " + std::to_string(v80.size()) + "）");
        if (v80.size() == 9) {
            expectTrue(v80[0].controlType == "text" && v80[1].controlType == "number" &&
                           v80[2].controlType == "radioButton" &&
                           v80[3].controlType == "radioButton" &&
                           v80[4].controlType == "checkBox" && v80[5].controlType == "radioGroup" &&
                           v80[6].controlType == "checkBoxGroup" &&
                           v80[7].controlType == "dropdownList" && v80[8].controlType == "date",
                       "九种业务类型按插入顺序排好");
            expectTrue(v80[5].value.text.empty() && v80[6].value.items.empty() &&
                           v80[7].value.text.empty() && v80[8].value.text.empty(),
                       "选择类与下拉、日期插入时都没有值");
            expectTrue(v80[5].content.find("○ 选项甲") == 0, "单选组的内容是图形加标签");
            expectTrue(v80[5].options.size() == 3, "单选组带三个选项");
        }

        // 单选组的互斥：切到第二项，内容里的图形跟着换。
        expectTrue(e80.toggleControlAndRender(5, 1), "单选组切到第二项");
        v80 = e80.controlViews();
        expectTrue(v80[5].value.text == "b", "值落在 b 上");
        expectTrue(v80[5].content.find("◉ 选项乙") != std::string::npos, "第二项的图形变成选中态");
        expectTrue(v80[5].content.find("○ 选项甲") != std::string::npos, "第一项回到未选中");

        // 复选组：多选、可取消。
        expectTrue(e80.toggleControlAndRender(6, 0), "复选组选中第一项");
        expectTrue(e80.toggleControlAndRender(6, 2), "复选组再选中第三项");
        v80 = e80.controlViews();
        expectTrue(v80[6].value.items.size() == 2 && v80[6].value.items[0] == "a" &&
                       v80[6].value.items[1] == "c",
                   "复选组同时选中 a 与 c");
        expectTrue(e80.toggleControlAndRender(6, 0), "取消第一项");
        expectTrue(e80.controlViews()[6].value.items.size() == 1, "只剩一项");

        // 单选的互斥组：点第二个，第一个自动取消 —— 同一批提交。
        expectTrue(e80.toggleControlAndRender(2, -1), "选中单选甲");
        expectTrue(e80.controlViews()[2].value.flag, "甲是选中的");
        const int depth80 = e80.undoDepth();
        expectTrue(e80.toggleControlAndRender(3, -1), "选中单选乙");
        v80 = e80.controlViews();
        expectTrue(v80[3].value.flag && !v80[2].value.flag, "乙选中、甲取消");
        expectTrue(v80[2].content.find("○") == 0 && v80[3].content.find("◉") == 0,
                   "两个单选的图形各自跟着自己的值");
        expectTrue(e80.undoDepth() == depth80 + 1, "一次切换只留一步撤销");

        // 复选：点一下选中，再点一下取消。
        expectTrue(e80.toggleControlAndRender(4, -1), "勾上复选");
        expectTrue(e80.controlViews()[4].value.flag, "复选是勾上的");
        expectTrue(e80.toggleControlAndRender(4, -1), "再点一下");
        expectTrue(!e80.controlViews()[4].value.flag, "复选取消了");

        // 下拉：值落在记录上，内容是选中项的标签；不在选项表里的值被拒。
        ControlValue dv80;
        dv80.text = "b";
        expectTrue(e80.setControlValueAndRender(7, dv80), "下拉选中 b");
        expectTrue(e80.controlViews()[7].content == "选项乙", "内容是选项乙");
        dv80.text = "zzz";
        expectTrue(!e80.setControlValueAndRender(7, dv80), "下拉拒收选项表以外的值");
        expectTrue(e80.controlViews()[7].content == "选项乙", "被拒之后内容不变");

        // 日期：值存 ISO 串，内容按默认格式显示；坏日期被拒。
        ControlValue dv81;
        dv81.text = "2026-10-26T01:57:41";
        expectTrue(e80.setControlValueAndRender(8, dv81), "日期写入 2026-10-26 01:57:41");
        expectTrue(e80.controlViews()[8].content == "2026年10月26日 01时57分41秒",
                   "内容按默认格式显示（实际 " + e80.controlViews()[8].content + "）");
        dv81.text = "2026-02-30T00:00:00";
        expectTrue(!e80.setControlValueAndRender(8, dv81), "2 月 30 日被拒");
        dv81.text = "2026-10-26";
        expectTrue(!e80.setControlValueAndRender(8, dv81), "不完整的 ISO 串被拒");

        // 光标屏障：三档模式都进不去选择类的内容。
        const ControlRange r82 = e80.controls().at(5).range;
        e80.setCaret(r82.start.para, r82.start.offset + 2);
        expectTrue(!e80.caretInsertable(), "文档模式：光标停在单选组内容里不算可插入位置");
        e80.setEditMode(EditMode::Form);
        e80.relayout();
        expectTrue(!e80.caretInsertable(), "表单模式：同样进不去");
        e80.setEditMode(EditMode::Document);
        e80.relayout();
        // 方向键跨过整块：从内容里往外走一格，落点必须在块外。
        e80.moveCaretBy(1);
        expectTrue(e80.caretInsertable(), "方向键把光标带出了屏障");

        // 从控件一侧跨到另一侧：贴着闭括号按左键要落到开括号之前，反过来也一样。
        //   贴回"来的这一侧"会让光标在控件边上原地不动，两边的正文互相走不过去。
        //   这里单独造一个前后都有正文的单选，边界上只有它自己的一对括号，
        //   相邻控件共享偏移时位置要另外数括号，与这条判据无关。
        e80.moveCaretToDocEnd();
        expectTrue(e80.typeTextAndRender("Z"), "在正文末尾打一个字");
        expectTrue(insert80("checkBox", "隔开", {}, ""), "插入一个前后都有正文的复选");
        const int cross80 = static_cast<int>(e80.controls().size()) - 1;
        const ControlRange rc80 = e80.controls().at(cross80).range;
        const FlowPos after80 = e80.flowPosAtSide(rc80.end.para, rc80.end.offset, 1);
        const FlowPos before80 = e80.flowPosAtSide(rc80.start.para, rc80.start.offset, 0);
        expectTrue(after80 > before80, "控件右侧的流位置在左侧之后");
        expectTrue(e80.setCaretAtFlow(after80), "光标摆到复选右侧");
        expectTrue(e80.moveCaretBy(-1), "左方向键");
        expectTrue(e80.caretFlowPos() == before80, "落在控件左侧（实际 " +
                                                       std::to_string(e80.caretFlowPos()) +
                                                       "，期望 " + std::to_string(before80) + "）");
        expectTrue(e80.moveCaretBy(1), "右方向键");
        expectTrue(e80.caretFlowPos() == after80, "从左侧跨回复选右边");

        // 下拉的自由输入：打开之后选项表以外的值也能写。
        ControlSpec free80 = makeControlSpec("dropdownList");
        if (auto* d = std::get_if<DropdownDetail>(&free80.detail)) {
            d->options = opts80;
            d->free_input = true;
        }
        e80.moveCaretToDocEnd();
        expectTrue(e80.insertControlAndRender(free80, ""), "插入一个允许自由输入的下拉");
        ControlValue fv80;
        fv80.text = "手填的值";
        const int freeIndex = static_cast<int>(e80.controlViews().size()) - 1;
        expectTrue(e80.setControlValueAndRender(freeIndex, fv80), "自由输入收下选项表以外的值");
        expectTrue(e80.controlViews()[static_cast<size_t>(freeIndex)].content == "手填的值",
                   "内容就是那段输入");

        // 整块删除：贴着屏障控件退格，删掉的是整块，不是它内容里的一个字。
        //   它的内容是"图形 + 标签"的投影，删一半就再也重建不回来。
        //   有值的下拉先清值（用户想删的是"选的那一项"），再按一次才删控件本身。
        e80.moveCaretToDocEnd();
        const size_t n_before = e80.controlViews().size();
        expectTrue(e80.backspace(), "退格清掉下拉的值");
        expectTrue(e80.controlViews().size() == n_before, "这一下只清值，控件还在");
        expectTrue(e80.backspace(), "再退格一次删控件");
        expectTrue(e80.controlViews().size() == n_before - 1, "整块删掉了一个控件（不是删字）");
        expectTrue(e80.backspace(), "再退格一次");
        expectTrue(e80.controlViews().size() == n_before - 2, "又整块删掉一个");

        // 空内容控件：光标停在两枚括号之间，连打三个字都得留在里面。
        //   零宽内容最容易把"内容末尾"与"控件之后"混成同一格 —— 那一格一旦判错，
        //   第一个字还进得去，第二个字就落到括号外面了。
        e80.moveCaretToDocEnd();
        expectTrue(e80.insertControlAndRender(makeControlSpec("text"), ""), "插入一个空内容控件");
        const int emptyIndex = static_cast<int>(e80.controlViews().size()) - 1;
        // 走 AndRender 版：真实宿主每敲一个字都会重排一次，簇表因此是新的。
        expectTrue(
            e80.typeTextAndRender("a") && e80.typeTextAndRender("b") && e80.typeTextAndRender("c"),
            "空控件里连打三个字都成立");
        // 批量提交的宿主：不重排直接接着打。此时簇表记的是重排那一刻的码点数，
        //   拿它吸附会把落点吸到上一个字 —— 旧表不许用，这是那条判据钉的事。
        expectTrue(e80.typeTextUtf8("d") && e80.typeTextUtf8("e"), "不重排连打两个字也成立");
        expectTrue(e80.controlViews()[static_cast<size_t>(emptyIndex)].content == "abcde",
                   "五个字都留在空控件里（实际 " +
                       e80.controlViews()[static_cast<size_t>(emptyIndex)].content + "）");

        // 没配互斥组的单选各自独立：点谁选谁，再点一下取消，不牵连另一个。
        expectTrue(insert80("radioButton", "独立甲", {}, ""), "插入未配组的单选甲");
        expectTrue(insert80("radioButton", "独立乙", {}, ""), "插入未配组的单选乙");
        const int solo_a = static_cast<int>(e80.controlViews().size()) - 2;
        const int solo_b = solo_a + 1;
        expectTrue(e80.toggleControlAndRender(solo_a, -1), "点独立甲");
        expectTrue(e80.controlViews()[static_cast<size_t>(solo_a)].value.flag, "独立甲选中");
        expectTrue(e80.toggleControlAndRender(solo_b, -1), "点独立乙");
        v80 = e80.controlViews();
        expectTrue(v80[static_cast<size_t>(solo_b)].value.flag, "独立乙选中");
        expectTrue(v80[static_cast<size_t>(solo_a)].value.flag,
                   "独立甲仍然选中（没有组的单选不互斥）");
        expectTrue(e80.toggleControlAndRender(solo_a, -1), "再点一次独立甲");
        v80 = e80.controlViews();
        expectTrue(!v80[static_cast<size_t>(solo_a)].value.flag, "独立甲取消");
        expectTrue(v80[static_cast<size_t>(solo_b)].value.flag, "独立乙不受影响");
    }

    if (runSection(81)) std::printf("\n[81] 控件定义可改：选项、标签、格式、孤儿值与撤销\n");
    if (runSection(81)) {
        EngineConfig c81;
        c81.zoom = 1.0f;
        const std::vector<std::string> fonts81 = {cjkPath, mnPath, latinPath};
        const doc::Doc t81 = doc::makeTextDoc({U"ABCD"});
        EditorEngine e81;
        if (!e81.init(fonts81)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e81.setConfig(c81);
        e81.setDocument(t81, StyleSheet{});
        e81.relayout();

        const std::vector<ControlOption> opts81 = {
            {"a", "选项甲"}, {"b", "选项乙"}, {"c", "选项丙"}};
        // 四个控件：单选组、下拉、日期、文本。各挑一项它收的定义字段来改。
        auto insert81 = [&](const std::string& type, const std::string& label,
                            const std::vector<ControlOption>& options) {
            ControlSpec spec = makeControlSpec(type);
            if (auto* c = std::get_if<ChoiceDetail>(&spec.detail)) c->options = options;
            if (auto* d = std::get_if<DropdownDetail>(&spec.detail)) d->options = options;
            e81.moveCaretToDocEnd();
            return e81.insertControlAndRender(spec, label);
        };
        expectTrue(insert81("radioGroup", "", opts81), "插入单选组");
        expectTrue(insert81("dropdownList", "", opts81), "插入下拉");
        expectTrue(insert81("date", "", {}), "插入日期");
        expectTrue(insert81("text", "乙方", {}), "插入文本控件");

        expectTrue(e81.toggleControlAndRender(0, 1), "单选组选中第二项");
        ControlValue dv81;
        dv81.text = "b";
        expectTrue(e81.setControlValueAndRender(1, dv81), "下拉选中 b");
        ControlValue iso81;
        iso81.text = "2026-10-26T01:57:41";
        expectTrue(e81.setControlValueAndRender(2, iso81), "日期写入 2026-10-26 01:57:41");

        // 改选项表：换标签、删掉选中项。内容按新表重建，指向被删项的值跟着清掉。
        const int depth81 = e81.undoDepth();
        ControlPatch p81;
        p81.options = std::vector<ControlOption>{{"a", "甲（改）"}, {"c", "丙"}};
        expectTrue(e81.setControlSpecAndRender(0, p81), "改单选组的选项表");
        std::vector<EditorEngine::ControlView> v81 = e81.controlViews();
        expectTrue(v81[0].options.size() == 2, "选项表剩两项");
        expectTrue(v81[0].options[0].label == "甲（改）", "标签改成了甲（改）");
        expectTrue(v81[0].value.text.empty(), "选中的 b 被删掉，值跟着清空");
        expectTrue(v81[0].content.find("甲（改）") != std::string::npos &&
                       v81[0].content.find("选项乙") == std::string::npos,
                   "内容是新的选项表（实际 " + v81[0].content + "）");
        expectTrue(e81.undoDepth() == depth81 + 1, "改定义只留一步撤销");

        // 撤销：定义与值一起回到改之前（值与定义在同一步里）。
        expectTrue(e81.undoAndRender(), "撤销改定义");
        v81 = e81.controlViews();
        expectTrue(v81[0].options.size() == 3 && v81[0].value.text == "b",
                   "撤销之后选项与选中值都回来了");
        expectTrue(e81.redoAndRender(), "重做");
        expectTrue(e81.controlViews()[0].value.text.empty(), "重做之后值又是空的");

        // 幂等：同一份补丁再来一次不产生步骤。
        const int depth82 = e81.undoDepth();
        expectTrue(e81.setControlSpecAndRender(0, p81), "同一份补丁再来一次");
        expectTrue(e81.undoDepth() == depth82, "幂等：不产生步骤");

        // 类型不可改。
        ControlPatch p83;
        p83.expect_type = std::string("checkBoxGroup");
        expectTrue(!e81.setControlSpecAndRender(0, p83), "类型不可改（要求 checkBoxGroup）");
        expectTrue(e81.controlViews()[0].controlType == "radioGroup", "被拒之后类型没动");

        // 收不下的字段要拒：给单选组送 format、给日期送 options、给文本送 group。
        ControlPatch p84;
        p84.format = std::string("yyyy");
        expectTrue(!e81.setControlSpecAndRender(0, p84), "给单选组送日期格式被拒");
        ControlPatch p85;
        p85.options = opts81;
        expectTrue(!e81.setControlSpecAndRender(2, p85), "给日期送选项表被拒");
        ControlPatch p86;
        p86.group = std::string("g");
        expectTrue(!e81.setControlSpecAndRender(3, p86), "给文本控件送互斥组被拒");

        // 日期：改格式串，内容跟着换；单位一个不选被拒。
        ControlPatch p87;
        p87.format = std::string("yyyy/MM/dd");
        expectTrue(e81.setControlSpecAndRender(2, p87), "日期改成 yyyy/MM/dd");
        expectTrue(e81.controlViews()[2].content == "2026/10/26",
                   "内容按新格式显示（实际 " + e81.controlViews()[2].content + "）");
        expectTrue(e81.controlViews()[2].value.text == "2026-10-26T01:57:41",
                   "值还是那段 ISO，换格式不动数据");
        ControlPatch p88;
        p88.units = static_cast<uint16_t>(0);
        expectTrue(!e81.setControlSpecAndRender(2, p88), "日期单位一个都不选被拒");
        ControlPatch p89;
        p89.units = static_cast<uint16_t>(kDateUnitYear | kDateUnitMonth | kDateUnitDay);
        expectTrue(e81.setControlSpecAndRender(2, p89), "日期只留年月日");
        expectTrue(
            e81.controlViews()[2].date_units == (kDateUnitYear | kDateUnitMonth | kDateUnitDay),
            "单位落进记录");
        expectTrue(e81.controlViews()[2].content == "2026/10/26", "内容不受单位影响");

        // 只改业务编码：值、内容一个字都不动（文本控件尤其，它的内容不是投影）。
        ControlPatch p90;
        p90.key = std::string("party_b");
        expectTrue(e81.setControlSpecAndRender(3, p90), "改文本控件的业务编码");
        expectTrue(e81.controlViews()[3].key == "party_b", "编码落进记录");
        expectTrue(e81.controlViews()[3].content == "乙方", "内容没动（它不是定义的投影）");

        // 下拉：只改 free_input 时值不动；关掉自由输入后选项外的值要清掉。
        ControlPatch p91;
        p91.free_input = true;
        expectTrue(e81.setControlSpecAndRender(1, p91), "打开自由输入");
        ControlValue fv81;
        fv81.text = "手填的";
        expectTrue(e81.setControlValueAndRender(1, fv81), "自由输入收下选项外的值");
        ControlPatch p92;
        p92.free_input = false;
        expectTrue(e81.setControlSpecAndRender(1, p92), "关掉自由输入");
        expectTrue(e81.controlViews()[1].value.text.empty(), "选项外的值跟着清空");
        // 空值的下拉：内容是空的，提示是排版层画的那句灰字（与文本控件的 placeholder 同一条）。
        //   内容空掉不等于控件点不到 —— 提示槽占着宽度，几何由它给（见 [89]）。
        expectTrue(e81.controlViews()[1].content.empty(), "空值不投影成内容");

        // 改定义是设计动作：表单与只读模式都拒绝。
        e81.setEditMode(EditMode::Form);
        e81.relayout();
        ControlPatch p93;
        p93.key = std::string("other");
        expectTrue(!e81.setControlSpecAndRender(3, p93), "表单模式拒绝改定义");
        e81.setEditMode(EditMode::ReadOnly);
        e81.relayout();
        expectTrue(!e81.setControlSpecAndRender(3, p93), "只读模式拒绝改定义");
        e81.setEditMode(EditMode::Document);
        e81.relayout();
        expectTrue(e81.controlViews()[3].key == "party_b", "两次被拒之后编码没动");
    }

    if (runSection(82)) std::printf("\n[82] 选择类的记号：方框、对勾、颜色与重绘\n");
    if (runSection(82)) {
        EngineConfig c82;
        c82.zoom = 3.0f;
        const std::vector<std::string> fonts82 = {cjkPath, mnPath, latinPath};
        const doc::Doc t82 = doc::makeTextDoc({U"A"});
        EditorEngine e82;
        if (!e82.init(fonts82)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e82.setConfig(c82);
        e82.setDocument(t82, StyleSheet{});
        e82.relayout();

        auto ins82 = [&](const std::string& type, const std::string& label) {
            ControlSpec sp = makeControlSpec(type);
            e82.moveCaretToDocEnd();
            return e82.insertControlAndRender(sp, label);
        };
        const std::vector<ControlOption> opts82 = {
            {"a", "选项甲"}, {"b", "选项乙"}, {"c", "选项丙"}};
        auto insChoice82 = [&](const std::string& type, const std::string& label,
                               const std::vector<ControlOption>& options) {
            ControlSpec sp = makeControlSpec(type);
            if (auto* c = std::get_if<ChoiceDetail>(&sp.detail)) c->options = options;
            e82.moveCaretToDocEnd();
            return e82.insertControlAndRender(sp, label);
        };
        expectTrue(ins82("checkBox", "未选"), "插入未选中的复选");
        expectTrue(ins82("checkBox", "已选"), "插入要选中的复选");
        expectTrue(e82.toggleControlAndRender(1, -1), "选中第二个复选");

        // 记号落进内容：方框两种状态用 U+2610 / U+2611（语义正确，字形由引擎画）。
        const std::vector<EditorEngine::ControlView> v82 = e82.controlViews();
        expectTrue(v82.size() == 2, "文档里两个复选");
        if (v82.size() == 2) {
            expectTrue(!v82[0].content.empty() && static_cast<uint32_t>(v82[0].content[0]) != 0 &&
                           v82[0].content.substr(0, 3) == "\u2610",
                       "未选中的记号是 U+2610（实际 " + v82[0].content + "）");
            expectTrue(v82[1].content.substr(0, 3) == "\u2611",
                       "选中的记号是 U+2611（实际 " + v82[1].content + "）");
        }

        // 数一枚记号那一格里的墨：选中了要多个对勾，墨一定更多。位置取内容矩形的头一格。
        const int fw82 = e82.frameWidth();
        auto ink82 = [&](size_t index) -> int {
            const std::vector<EditorEngine::ControlRect> rs = e82.controlRects(index);
            if (rs.empty()) return -1;
            const Rect r = rs[0].r_pt;
            const float zoom = c82.zoom;
            const int x0 = static_cast<int>(std::floor(r.x * zoom));
            const int x1 = static_cast<int>(std::ceil((r.x + r.h) * zoom));
            const int y0 = static_cast<int>(std::floor(r.y * zoom));
            const int y1 = static_cast<int>(std::ceil((r.y + r.h) * zoom));
            const uint8_t* fp = e82.frame();
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(fw82) +
                                      static_cast<size_t>(x)) *
                                     4u;
                    if (fp[i] < 200 || fp[i + 1] < 200 || fp[i + 2] < 200) ++n;
                }
            }
            return n;
        };
        const int ink_off = ink82(0);
        const int ink_on = ink82(1);
        expectTrue(ink_off > 0, "未选中的方框画出来了（墨 " + std::to_string(ink_off) + "）");
        expectTrue(ink_on > ink_off, "选中的方框墨更多（对勾加进去了：" + std::to_string(ink_off) +
                                         " → " + std::to_string(ink_on) + "）");

        // 方框四条边一样粗：边长与线宽都对齐整数像素，右 / 下边不会多出一像素。
        auto frameRuns82 = [&](size_t index) -> std::array<int, 4> {
            std::array<int, 4> out{0, 0, 0, 0};
            const std::vector<EditorEngine::ControlRect> rs = e82.controlRects(index);
            if (rs.empty()) return out;
            const Rect r = rs[0].r_pt;
            const float zoom = c82.zoom;
            const int x0 = static_cast<int>(std::floor(r.x * zoom));
            const int x1 = static_cast<int>(std::ceil((r.x + r.h) * zoom));
            const int y0 = static_cast<int>(std::floor(r.y * zoom));
            const int y1 = static_cast<int>(std::ceil((r.y + r.h) * zoom));
            const uint8_t* fp = e82.frame();
            auto dark = [&](int x, int y) {
                const size_t i =
                    (static_cast<size_t>(y) * static_cast<size_t>(fw82) + static_cast<size_t>(x)) *
                    4u;
                return fp[i] < 128 && fp[i + 1] < 128 && fp[i + 2] < 128;
            };
            int minx = x1, maxx = x0 - 1, miny = y1, maxy = y0 - 1;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    if (!dark(x, y)) continue;
                    minx = std::min(minx, x);
                    maxx = std::max(maxx, x);
                    miny = std::min(miny, y);
                    maxy = std::max(maxy, y);
                }
            }
            if (maxx < minx || maxy < miny) return out;
            // 内容矩形里还有标签的字，不能拿整块墨的包围盒当框。框是最左边那一条
            //   连续的墨：先找最左的暗列与它最上面的暗点，再沿上边量出边长。
            int bx = -1;
            int by = -1;
            for (int x = x0; x <= maxx && bx < 0; ++x) {
                for (int y = y0; y <= maxy; ++y) {
                    if (!dark(x, y)) continue;
                    bx = x;
                    by = y;
                    break;
                }
            }
            if (bx < 0 || by < 0) return out;
            int side = 0;
            while (bx + side <= maxx && dark(bx + side, by)) ++side;
            if (side < 4) return out;
            const int ymid = by + side / 2;
            const int xmid = bx + side / 2;
            while (bx + out[0] < bx + side && dark(bx + out[0], ymid)) ++out[0];
            while (bx + side - 1 - out[1] >= bx && dark(bx + side - 1 - out[1], ymid)) ++out[1];
            while (by + out[2] < by + side && dark(xmid, by + out[2])) ++out[2];
            while (by + side - 1 - out[3] >= by && dark(xmid, by + side - 1 - out[3])) ++out[3];
            return out;
        };
        const std::array<int, 4> runs82 = frameRuns82(0);
        expectTrue(runs82[0] > 0, "框的左边量得到（" + std::to_string(runs82[0]) + " 像素）");
        expectTrue(runs82[0] == runs82[1] && runs82[0] == runs82[2] && runs82[0] == runs82[3],
                   "四条边一样粗（" + std::to_string(runs82[0]) + "/" + std::to_string(runs82[1]) +
                       "/" + std::to_string(runs82[2]) + "/" + std::to_string(runs82[3]) + "）");

        // 对勾的斜边有抗锯齿的中间灰：非黑即白说明边缘是台阶。
        auto shade82 = [&](size_t index) -> int {
            const std::vector<EditorEngine::ControlRect> rs = e82.controlRects(index);
            if (rs.empty()) return -1;
            const Rect r = rs[0].r_pt;
            const float zoom = c82.zoom;
            const int x0 = static_cast<int>(std::floor(r.x * zoom));
            const int x1 = static_cast<int>(std::ceil((r.x + r.h) * zoom));
            const int y0 = static_cast<int>(std::floor(r.y * zoom));
            const int y1 = static_cast<int>(std::ceil((r.y + r.h) * zoom));
            const uint8_t* fp = e82.frame();
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(fw82) +
                                      static_cast<size_t>(x)) *
                                     4u;
                    const int rr = fp[i];
                    const int gg = fp[i + 1];
                    const int bb = fp[i + 2];
                    if (rr > 40 && rr < 215 && std::abs(rr - gg) < 10 && std::abs(gg - bb) < 10) {
                        ++n;
                    }
                }
            }
            return n;
        };
        expectTrue(shade82(1) > 0, "对勾有抗锯齿的过渡像素（" + std::to_string(shade82(1)) + "）");

        // 记号色可配：配成纯蓝之后，那一格里必须出现蓝色像素。
        auto blue82 = [&](size_t index) -> int {
            const std::vector<EditorEngine::ControlRect> rs = e82.controlRects(index);
            if (rs.empty()) return -1;
            const Rect r = rs[0].r_pt;
            const float zoom = c82.zoom;
            const int x0 = static_cast<int>(std::floor(r.x * zoom));
            const int x1 = static_cast<int>(std::ceil((r.x + r.h) * zoom));
            const int y0 = static_cast<int>(std::floor(r.y * zoom));
            const int y1 = static_cast<int>(std::ceil((r.y + r.h) * zoom));
            const uint8_t* fp = e82.frame();
            int n = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(fw82) +
                                      static_cast<size_t>(x)) *
                                     4u;
                    if (fp[i] < 120 && fp[i + 2] > 150) ++n;
                }
            }
            return n;
        };
        expectTrue(blue82(1) == 0, "默认记号色跟随文字（不是蓝的）");
        ControlPatch cp82;
        cp82.glyph_color = Color::rgb(20, 110, 220);
        expectTrue(e82.setControlSpecAndRender(1, cp82), "把选中那个复选的记号色改成蓝");
        expectTrue(blue82(1) > 20, "记号变蓝了（蓝色像素 " + std::to_string(blue82(1)) + "）");
        expectTrue(blue82(0) == 0, "另一个控件不受影响（颜色是每个控件自己的）");

        // 只改颜色、不改一个字：画面照样要重绘（页指纹必须覆盖外观）。
        ControlPatch cp83;
        cp83.glyph_color = Color::rgb(220, 40, 40);
        expectTrue(e82.setControlSpecAndRender(1, cp83), "再改成红");
        expectTrue(blue82(1) == 0, "改成红之后蓝色像素没了");
        expectTrue(e82.controlViews()[1].glyph_color.r == 220, "记录里是红");

        // 记号色落到不收它的类型上：拒。
        expectTrue(ins82("text", "文本"), "插入一个文本控件");
        ControlPatch cp85;
        cp85.glyph_color = Color::rgb(1, 2, 3);
        expectTrue(!e82.setControlSpecAndRender(2, cp85), "给文本控件配记号色被拒");

        // 四种选择类的记号色都能配：单选、单选组、复选组与复选走同一条外观字段。
        expectTrue(insChoice82("radioButton", "单选", {}), "插入单选");
        expectTrue(insChoice82("radioGroup", "", opts82), "插入单选组");
        expectTrue(insChoice82("checkBoxGroup", "", opts82), "插入复选组");
        const int n82 = static_cast<int>(e82.controlViews().size());
        const int i_radio = n82 - 3;
        const int i_rgroup = n82 - 2;
        const int i_cgroup = n82 - 1;
        for (const int idx : {i_radio, i_rgroup, i_cgroup}) {
            ControlPatch cp;
            cp.glyph_color = Color::rgb(20, 110, 220);
            expectTrue(e82.setControlSpecAndRender(idx, cp), "给选择类配记号色");
            const int blue = blue82(static_cast<size_t>(idx));
            expectTrue(blue > 12, "记号变蓝（下标 " + std::to_string(idx) + "，蓝色像素 " +
                                      std::to_string(blue) + "）");
            expectTrue(e82.controlViews()[static_cast<size_t>(idx)].glyph_color.b == 220,
                       "颜色落进记录");
        }

        // 括号跟着内容字体走：把内容调成 24 磅，开括号的槽宽必须跟着变宽。
        expectTrue(ins82("text", "AB"), "插入一个文本控件量括号槽宽");
        const int i_text = static_cast<int>(e82.controlViews().size()) - 1;
        const ControlRange br82 = e82.controls().at(i_text).range;
        const FlowPos open82 = e82.flowPosAtSide(br82.start.para, br82.start.offset, 0);
        const FlowPos in0_82 = open82 + 1;
        const FlowPos in1_82 = e82.flowPosAtSide(br82.end.para, br82.end.offset, 0);
        expectTrue(in1_82 > in0_82, "内容区间在开括号之后、闭括号之前");
        auto slotWidth82 = [&]() -> float {
            if (!e82.setCaretAtFlow(open82)) return -1.0f;
            Rect cr{};
            if (!e82.caretRectPt(&cr)) return -1.0f;
            const std::vector<EditorEngine::ControlRect> rs = e82.controlRects(i_text);
            if (rs.empty()) return -1.0f;
            return rs[0].r_pt.x - cr.x;
        };
        const float slot_before82 = slotWidth82();
        expectTrue(slot_before82 > 0.0f, "开括号槽宽量得到");
        expectTrue(e82.setSelection(Range{in0_82, in1_82}), "选中控件内容");
        CharFormat big82;
        big82.size = Length::abs(ptToEmu(24.0));
        expectTrue(e82.applyCharFormatAndRender(big82), "内容改成 24 磅");
        const float slot_after82 = slotWidth82();
        expectTrue(slot_after82 > slot_before82 + 1.0f, "括号槽宽跟着字号变（" +
                                                            std::to_string(slot_before82) + " → " +
                                                            std::to_string(slot_after82) + "）");
    }

    if (runSection(83)) std::printf("\n[83] 折行与选框：右括号落行首、选区端点、勾选重绘\n");
    if (runSection(83)) {
        EngineConfig c83;
        c83.zoom = 1.0f;
        c83.page.width_pt = 110.0f;
        c83.page.height_pt = 400.0f;
        c83.page.margins = PageMargins::all(5.0f);  // 内容宽 100 磅
        const std::vector<std::string> fonts83 = {cjkPath, mnPath, latinPath};
        EditorEngine e83;
        if (!e83.init(fonts83)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e83.setConfig(c83);

        // 造一版"闭括号被挤到下一行行首"的文档：标签占满第一行，闭括号放不下。
        //   标签长度按字数试 —— 汉字是全角，但括号槽宽与空格宽由字体定，算不出来就试。
        auto buildCloseAtLineStart = [&](int chars) {
            int id = 3;
            ControlSpec sp = makeControlSpec("checkBox");
            std::string label;
            for (int i = 0; i < chars; ++i) label += "字";
            if (auto* c = std::get_if<ChoiceDetail>(&sp.detail)) c->label = label;
            const doc::Ref control = doc::makeNode(
                id++, NodeKind::ContentControl, sp,
                {doc::makeNode(
                    id++, NodeKind::TextRun,
                    doc::RunPayload{
                        utf8ToCodepoints(controlDisplayText(sp, sp.value)), CharFormat{}, {}},
                    {})});
            // 控件后面紧跟两个字：下一行就成了 "]甲乙"，与用户描述的场景一致。
            const doc::Ref section = doc::makeNode(
                id++, NodeKind::Section, SectionSpec{},
                {control,
                 doc::makeNode(id++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
                 doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const NodeId root_id = id++;
            e83.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            e83.relayout();
        };
        int close_line = -1;
        int total_lines = 0;
        for (int k = 4; k <= 12 && close_line < 0; ++k) {
            buildCloseAtLineStart(k);
            const std::vector<LineBox>& ls = e83.layout().lines;
            total_lines = static_cast<int>(ls.size());
            for (size_t i = 0; i < ls.size(); ++i) {
                if (ls[i].clusters.empty()) continue;
                const LineCluster& c0 = ls[i].clusters[0];
                if (c0.placeholder && c0.bracket_side == BracketSide::Close) {
                    close_line = static_cast<int>(i);
                    break;
                }
            }
        }
        expectTrue(close_line > 0,
                   "构造：闭括号被挤到第二行行首（共 " + std::to_string(total_lines) + " 行）");
        if (close_line > 0) {
            const LineBox& cl = e83.layout().lines[static_cast<size_t>(close_line)];
            FlowPos lo = 0;
            FlowPos hi = 0;
            expectTrue(e83.controlContentSpan(0, &lo, &hi), "控件内容区间量得到");
            // 闭括号之后那一格：内容末尾 + 1。
            const FlowPos after_close = hi + 1;
            expectTrue(e83.setCaretAtFlow(after_close), "光标摆到闭括号之后");
            Rect r_close{};
            expectTrue(e83.caretRectPt(&r_close), "闭括号之后的光标矩形量得到");
            expectTrue(std::fabs(r_close.y - cl.block_lo_pt) < 1.0f,
                       "闭括号之后的光标画在它自己那一行（y=" + std::to_string(r_close.y) +
                           "，行首 y=" + std::to_string(cl.block_lo_pt) + "）");

            // 从右边一格左移：落点必须是闭括号之后，而且仍画在这一行 ——
            //   行别若只按"贴上一行"投影，键盘一移就跳到上一行去了。
            expectTrue(e83.setCaretAtFlow(after_close + 1), "光标再往右一格（甲乙之间）");
            expectTrue(e83.moveCaretBy(-1), "左移一格");
            expectTrue(e83.caretFlowPos() == after_close, "落点是闭括号之后（不是内容末尾）");
            Rect r_after{};
            expectTrue(e83.caretRectPt(&r_after), "左移之后的光标矩形量得到");
            expectTrue(std::fabs(r_after.y - cl.block_lo_pt) < 1.0f,
                       "左移之后仍在这一行（y=" + std::to_string(r_after.y) + "）");

            // 选区与光标是两件事：端点可以落在控件内容里，光标仍不能停在那儿。
            expectTrue(e83.setSelection(Range{0, hi}), "选区端点落在控件内容末尾");
            expectTrue(e83.selection().focus == hi, "focus 原样落在内容里（没有被夹到块外）");
            expectTrue(!e83.insertableAtFlow(hi), "那个位置仍然不是插入点");
        }

        // 勾选状态变了页就得重画：内容只换一个码点（☐ → ☑），成形后是同一枚方框字形，
        //   行指纹若不比"值"，这一页会被判成没变 —— 画面停在旧帧，要等别处动一下才刷。
        {
            int id = 3;
            ControlSpec sp = makeControlSpec("checkBox");
            if (auto* c = std::get_if<ChoiceDetail>(&sp.detail)) c->label = "同意";
            const doc::Ref control = doc::makeNode(
                id++, NodeKind::ContentControl, sp,
                {doc::makeNode(
                    id++, NodeKind::TextRun,
                    doc::RunPayload{
                        utf8ToCodepoints(controlDisplayText(sp, sp.value)), CharFormat{}, {}},
                    {})});
            const doc::Ref section = doc::makeNode(
                id++, NodeKind::Section, SectionSpec{},
                {control, doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const NodeId root_id = id++;
            e83.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            e83.relayout();
            expectTrue(e83.controlViews().size() == 1, "构造：一个复选");

            const int fw = e83.frameWidth();
            const size_t bytes =
                static_cast<size_t>(fw) * static_cast<size_t>(e83.frameHeight()) * 4u;
            const uint8_t* fp = e83.frame();
            std::vector<uint8_t> before(fp, fp + bytes);
            expectTrue(e83.toggleControlAndRender(0, -1), "勾上这个复选");
            const uint8_t* fp2 = e83.frame();
            size_t diff = 0;
            for (size_t i = 0; i < bytes; i += 4u) {
                if (fp2[i] != before[i] || fp2[i + 1] != before[i + 1] ||
                    fp2[i + 2] != before[i + 2]) {
                    ++diff;
                }
            }
            expectTrue(diff > 0,
                       "切换选中之后页位图真的变了（差 " + std::to_string(diff) + " 个像素）");
            // 反向一半：再切回来，画面也要回去。
            std::vector<uint8_t> on(fp2, fp2 + bytes);
            expectTrue(e83.toggleControlAndRender(0, -1), "再点一下取消选中");
            const uint8_t* fp3 = e83.frame();
            size_t back = 0;
            for (size_t i = 0; i < bytes; i += 4u) {
                if (fp3[i] != on[i] || fp3[i + 1] != on[i + 1] || fp3[i + 2] != on[i + 2]) ++back;
            }
            expectTrue(back > 0, "取消选中也要重画（差 " + std::to_string(back) + " 个像素）");
        }
    }

    if (runSection(84))
        std::printf("\n[84] 控件落点：同一处点击不换边、选区从按下处起手、图片选中不画光标\n");
    if (runSection(84)) {
        EngineConfig c84;
        c84.zoom = 1.0f;
        c84.page.width_pt = 200.0f;
        c84.page.height_pt = 400.0f;
        c84.page.margins = PageMargins::all(10.0f);  // 内容宽 180 磅
        const std::vector<std::string> fonts84 = {cjkPath, mnPath, latinPath};
        EditorEngine e84;
        if (!e84.init(fonts84)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e84.setConfig(c84);

        // 造文档：前有两个字、中间一个复选、后面两个字。标签长度可调 —— 长标签会把
        //   控件挤成两行，跨行时"点边距"才有真实的落点可测。
        auto buildDoc = [&](const std::string& label, int tail_chars) {
            int id = 3;
            ControlSpec sp = makeControlSpec("checkBox");
            if (auto* ch = std::get_if<ChoiceDetail>(&sp.detail)) ch->label = label;
            std::string tail_text;
            for (int i = 0; i < tail_chars; ++i) tail_text += "丙";
            const doc::Ref control = doc::makeNode(
                id++, NodeKind::ContentControl, sp,
                {doc::makeNode(
                    id++, NodeKind::TextRun,
                    doc::RunPayload{
                        utf8ToCodepoints(controlDisplayText(sp, sp.value)), CharFormat{}, {}},
                    {})});
            const doc::Ref section = doc::makeNode(
                id++, NodeKind::Section, SectionSpec{},
                {doc::makeNode(id++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
                 control,
                 doc::makeNode(id++, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints(tail_text), CharFormat{}, {}}, {}),
                 doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const NodeId root_id = id++;
            e84.setDocument(
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), id),
                StyleSheet{});
            e84.relayout();
        };

        // 点画布：命中 → 落点，与宿主那一条路逐字相同。
        auto clickAt = [&](float x, float y) {
            TextPos p{};
            int owner = -1;
            int side = 0;
            int line_aff = 0;
            int cell_para = -1;
            if (!e84.hitTestPt(x, y, &p, &owner, &side, &line_aff, &cell_para, false)) return false;
            e84.setCaretAtSideLine(p.para, p.offset, side, line_aff);
            e84.repaint();
            return true;
        };

        buildDoc("选项甲", 2);
        expectTrue(e84.controlViews().size() == 1, "构造：一个复选");
        FlowPos lo84 = 0;
        FlowPos hi84 = 0;
        expectTrue(e84.controlContentSpan(0, &lo84, &hi84), "控件内容区间量得到");
        const std::vector<EditorEngine::ControlRect> rects84 = e84.controlRects(0);
        expectTrue(!rects84.empty(), "控件有一个页面矩形");
        if (!rects84.empty()) {
            const float y84 = rects84[0].r_pt.y + rects84[0].r_pt.h * 0.5f;
            const float x_left = rects84[0].r_pt.x + rects84[0].r_pt.w * 0.25f;
            const float x_right = rects84[0].r_pt.x + rects84[0].r_pt.w * 0.75f;

            // 同一处点两次：落点必须一样。按"来的那一侧"取对面的话，点同一处会在
            //   控件左右来回跳 —— 表象就是"光标在控件两边乱跑"。
            expectTrue(clickAt(x_left, y84), "点在控件左半边");
            const FlowPos left1 = e84.caretFlowPos();
            expectTrue(clickAt(x_left, y84), "同一处再点一次");
            const FlowPos left2 = e84.caretFlowPos();
            expectTrue(left1 == left2, "左半边点两次光标不换边（" + std::to_string(left1) + " / " +
                                           std::to_string(left2) + "）");
            expectTrue(left1 == lo84 - 1, "左半边的落点是开括号之前（实际 " +
                                              std::to_string(left1) + "，期望 " +
                                              std::to_string(lo84 - 1) + "）");

            expectTrue(clickAt(x_right, y84), "点在控件右半边");
            const FlowPos right1 = e84.caretFlowPos();
            expectTrue(clickAt(x_right, y84), "右半边再点一次");
            const FlowPos right2 = e84.caretFlowPos();
            expectTrue(right1 == right2, "右半边点两次光标不换边（" + std::to_string(right1) +
                                             " / " + std::to_string(right2) + "）");
            expectTrue(right1 == hi84 + 1, "右半边的落点是闭括号之后（实际 " +
                                               std::to_string(right1) + "，期望 " +
                                               std::to_string(hi84 + 1) + "）");

            // 从控件内部按下往右拖：锚端落在按下的那个字上，而不是控件边界。
            //   按下的那一点先在屏障里被夹到块外（光标不能停在控件里），锚端随后补回去。
            TextPos pf{};
            int of = -1;
            int sf = 0;
            int lf = 0;
            int cf = -1;
            const float x_after = rects84[0].r_pt.x + rects84[0].r_pt.w + 20.0f;
            expectTrue(e84.hitTestPt(x_after, y84, &pf, &of, &sf, &lf, &cf, false),
                       "控件右侧有一个可落点");
            expectTrue(clickAt(x_left, y84), "按下在控件内部");
            e84.setFocusAtSideLine(pf.para, pf.offset, sf, lf);
            e84.repaint();
            const Range sel84 = e84.selection();
            expectTrue(!sel84.empty(), "拖出了选区");
            expectTrue(sel84.anchor > lo84 - 1 && sel84.anchor < hi84 + 1,
                       "锚端落在按下的那个字上（实际 " + std::to_string(sel84.anchor) + "，块是 " +
                           std::to_string(lo84) + ".." + std::to_string(hi84) + "）");
            expectTrue(!e84.insertableAtFlow(sel84.anchor), "锚端在屏障里（光标本来停不到那儿）");
            expectTrue(sel84.focus > sel84.anchor, "焦点端在右边");
        }

        // 跨行控件：第二行上点左边距。命中会把点吸附到那一行最近的字（在控件里），
        //   落点于是又被屏障要求换到块外 —— 这里同样不能一左一右地跳。
        buildDoc("选项甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳", 2);
        const std::vector<EditorEngine::ControlRect> wrap84 = e84.controlRects(0);
        expectTrue(wrap84.size() >= 2,
                   "构造：控件跨行（矩形 " + std::to_string(wrap84.size()) + " 段）");
        if (wrap84.size() >= 2) {
            const float y2 = wrap84[1].r_pt.y + wrap84[1].r_pt.h * 0.5f;
            expectTrue(clickAt(2.0f, y2), "点控件第二行的左边距");
            const FlowPos m1 = e84.caretFlowPos();
            expectTrue(clickAt(2.0f, y2), "边距上再点一次");
            const FlowPos m2 = e84.caretFlowPos();
            expectTrue(m1 == m2, "边距上重复点击不换边（" + std::to_string(m1) + " / " +
                                     std::to_string(m2) + "）");
        }

        // 图片选中时不画文本光标：那条竖线正落在图的右边线上，会把右中把手盖住。
        {
            e84.setDocument(doc::makeTextDoc({U"甲乙丙丁"}), StyleSheet{});
            e84.relayout();

            const int iw = 96;
            const int ih = 48;
            std::vector<uint8_t> px(static_cast<size_t>(iw) * ih * 4u, 200);
            std::string err84;
            expectTrue(e84.registerImageAndRepaint("pic84", iw, ih, px.data(), px.size(), &err84),
                       "注册一张图（" + err84 + "）");
            e84.setCaret(0, 2);
            ImageSpec im84;
            im84.resource_id = "pic84";
            im84.px_w = iw;
            im84.px_h = ih;
            im84.dpi = 96;
            expectTrue(e84.insertImageAndRender(im84), "插进文档");
            expectTrue(e84.editStats().caret_visible, "插完图光标还在（在图片之后）");
            expectTrue(e84.selectImage(0), "选中这张图");
            e84.repaint();
            expectTrue(!e84.editStats().caret_visible, "选中图片时不画文本光标");
            expectTrue(e84.clearImageSelectionAndRender(), "取消选中");
            expectTrue(e84.editStats().caret_visible, "取消选中之后光标回来");
        }
    }

    if (runSection(85)) std::printf("\n[85] 表格命令：插删行列、合并拆分、三层记录与落点归属\n");
    if (runSection(85)) {
        EngineConfig c85;
        c85.zoom = 1.0f;
        c85.page.width_pt = 320.0f;
        c85.page.height_pt = 420.0f;
        c85.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts85 = {cjkPath, mnPath, latinPath};

        // 文档：一段正文 + 一张 2x2 空表 + 结尾一段。表直接建在树里（插入那条路另有一节）。
        int id = 3;
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        std::vector<doc::Ref> rows85;
        for (int r = 0; r < 2; ++r) {
            std::vector<doc::Ref> cells;
            for (int c = 0; c < 2; ++c) {
                cells.push_back(doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()}));
            }
            rows85.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
        }
        const doc::Ref table85 = doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows85);
        const doc::Ref sec85 = doc::makeNode(
            id++, NodeKind::Section, SectionSpec{},
            {doc::makeNode(id++, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
             mark(), table85, mark()});
        const NodeId root85 = id++;
        const doc::Doc t85 = doc::adopt(doc::makeNode(root85, NodeKind::Document, {}, {sec85}), id);

        EditorEngine e85;
        if (!e85.init(fonts85)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e85.setConfig(c85);
        e85.setDocument(t85, StyleSheet{});
        e85.relayout();
        e85.repaint();

        // 某一格的首段段号（格的坐标 → 块下标）。
        auto cellPara = [&](int row, int cell) {
            for (size_t i = 0; i < e85.blocks().size(); ++i) {
                const std::vector<CellSlot>& path = e85.blocks()[i].cell_path;
                if (!path.empty() && path.back().row == row && path.back().cell == cell) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        auto putCaret = [&](int row, int cell) {
            const int p = cellPara(row, cell);
            if (p < 0) return false;
            e85.setCaret(p, 0);
            return true;
        };
        auto caretCell = [&](int* row, int* cell) {
            const int para = e85.caret().para;
            if (para < 0 || para >= static_cast<int>(e85.blocks().size())) return false;
            const std::vector<CellSlot>& path = e85.blocks()[static_cast<size_t>(para)].cell_path;
            if (path.empty()) return false;
            if (row != nullptr) *row = path.back().row;
            if (cell != nullptr) *cell = path.back().cell;
            return true;
        };
        auto stateIs = [&](const char* needle) {
            return e85.tableStateJson().find(needle) != std::string::npos;
        };

        expectTrue(e85.tableSpecs().size() == 1, "装载：一张表");
        expectTrue(e85.tableSpecs()[0].rows == 2 && e85.tableSpecs()[0].cols == 2, "表是 2x2");
        expectTrue(putCaret(0, 0), "光标进第 0 行第 0 格");

        // 插行：行数 +1，光标跟着进新行的那一列（接着填是插入行之后最常见的下一步）。
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::InsertRowBelow),
                   "在第 0 行下方插一行");
        expectTrue(e85.tableSpecs()[0].rows == 3, "行数变 3");
        int cr85 = -1;
        int cc85 = -1;
        expectTrue(caretCell(&cr85, &cc85) && cr85 == 1 && cc85 == 0,
                   "光标落在新行第 0 列（实际 " + std::to_string(cr85) + "," +
                       std::to_string(cc85) + "）");
        expectTrue(e85.undo(), "撤销插行");
        e85.relayout();
        expectTrue(e85.tableSpecs()[0].rows == 2, "撤销之后行数回到 2");

        // 删除行：删到只剩一行时拒绝，而且文档一个字节都不动。
        expectTrue(putCaret(1, 0), "光标进第 1 行");
        const size_t blocks85 = e85.blocks().size();
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::RemoveRow), "删掉第 1 行");
        expectTrue(e85.tableSpecs()[0].rows == 1, "行数变 1");
        expectTrue(!e85.tableOpAndRender(EditorEngine::TableOp::RemoveRow), "删最后一行被拒");
        expectTrue(e85.blocks().size() == blocks85 - 2, "被拒时不再少块（拒前拒后一致）");
        expectTrue(e85.undo(), "撤销删行");
        e85.relayout();
        expectTrue(e85.tableSpecs()[0].rows == 2, "撤销之后行数回到 2");

        // 插列：列数 +1；插在右边时光标留在原格。
        expectTrue(putCaret(0, 0), "光标回第 0 行第 0 格");
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::InsertColumnRight),
                   "在第 0 列右侧插一列");
        expectTrue(e85.tableSpecs()[0].cols == 3, "列数变 3");
        expectTrue(caretCell(&cr85, &cc85) && cr85 == 0 && cc85 == 0, "光标还在原来那一格");
        expectTrue(e85.undo(), "撤销插列");
        e85.relayout();
        expectTrue(e85.tableSpecs()[0].cols == 2, "撤销之后列数回到 2");

        // 合并：拖选两格 → 整格选区 → 合并成 1 格（跨度 2），格子数少一个。
        const int pa85 = cellPara(0, 0);
        const int pb85 = cellPara(0, 1);
        expectTrue(pa85 >= 0 && pb85 >= 0, "找得到第 0 行的两格");
        e85.setCaretAtSide(pa85, 0, 0);
        e85.setFocusAtSide(pb85, 0, 0);
        expectTrue(e85.selectionIsCell(), "拖选跨格之后是整格选区");
        const size_t cells85 = e85.tableSpecs()[0].cells.size();
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::MergeCells), "合并这两格");
        expectTrue(e85.tableSpecs()[0].cells.size() == cells85 - 1, "格子数少一个");
        bool merged85 = false;
        for (const LayoutCell& lc : e85.tableSpecs()[0].cells) {
            if (lc.row == 0 && lc.cell == 0) merged85 = lc.col_span == 2;
        }
        expectTrue(merged85, "左上角那一格横跨两列");
        expectTrue(stateIs("\"colSpan\":2"), "tableState 报出这一格的跨度");
        expectTrue(e85.undo(), "撤销合并");
        e85.relayout();
        expectTrue(e85.tableSpecs()[0].cells.size() == cells85, "撤销之后格子数回来");

        // 没有整格选区时合并被拒：不替用户猜"和右边那一格并"。
        expectTrue(putCaret(0, 0), "光标（不是选区）在第 0 行第 0 格");
        expectTrue(!e85.tableOpAndRender(EditorEngine::TableOp::MergeCells), "单格不能合并");

        // 拆分：合并之后拆回去，格数与跨度都回到原样；没合并的格拆分被拒。
        e85.setCaretAtSide(cellPara(0, 0), 0, 0);
        e85.setFocusAtSide(cellPara(0, 1), 0, 0);
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::MergeCells), "再合并一次");
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::SplitCell), "拆开合并的那一格");
        expectTrue(e85.tableSpecs()[0].cells.size() == cells85, "拆分之后格子数回来");
        bool split85 = false;
        for (const LayoutCell& lc : e85.tableSpecs()[0].cells) {
            if (lc.row == 0 && lc.cell == 0) split85 = lc.col_span == 1;
        }
        expectTrue(split85, "左上角那一格回到 1x1");
        expectTrue(!e85.tableOpAndRender(EditorEngine::TableOp::SplitCell), "没合并的格不能拆分");

        // 行记录：标题行。上面一步撤销过，这里先确认起始状态是"不是标题行"。
        expectTrue(putCaret(0, 0), "光标回第 0 行第 0 格");
        RowPatch rp85;
        rp85.is_header = true;
        expectTrue(e85.setRowSpecAndRender(rp85), "把第 0 行设成标题行");
        expectTrue(e85.tableSpecs()[0].row_specs[0].is_header, "排版输入里这一行是标题行");
        expectTrue(stateIs("\"rowHeader\":1"), "tableState 报出标题行");
        // 幂等：同一个值再设一次也要成功，而且不产生新的一步撤销。
        expectTrue(e85.setRowSpecAndRender(rp85), "再设一次同样的标题行");
        expectTrue(e85.undo(), "撤销一次");
        e85.relayout();
        expectTrue(!e85.tableSpecs()[0].row_specs[0].is_header,
                   "★ 一次撤销就回到「不是标题行」（幂等没有多压一步）");

        // 格记录：垂直对齐。行列操作不许把它清回顶端（只写跨度的那种写法会）。
        CellPatch cp85;
        cp85.v_align = CellVAlign::Center;
        expectTrue(e85.setCellSpecAndRender(cp85), "把这一格设成垂直居中");
        expectTrue(stateIs("\"vAlign\":1"), "tableState 报出居中");
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::InsertColumnRight), "再插一列");
        expectTrue(stateIs("\"vAlign\":1"), "★ 插列之后对齐没被清回顶端");
        expectTrue(e85.undo(), "撤销插列");
        e85.relayout();

        // 表记录：宽度。
        TablePatch tp85;
        tp85.width_pt = 120.0f;
        expectTrue(e85.setTableSpecAndRender(tp85), "把表宽设成 120 磅");
        expectTrue(stateIs("\"widthPt\":120"), "tableState 报出 120 磅");
        expectTrue(e85.undo(), "撤销表宽");
        e85.relayout();

        // 删列到底：2 列 → 1 列之后拒绝。
        expectTrue(putCaret(0, 0), "光标回第 0 行第 0 格");
        expectTrue(e85.tableOpAndRender(EditorEngine::TableOp::RemoveColumn), "删掉一列");
        expectTrue(e85.tableSpecs()[0].cols == 1, "列数变 1");
        expectTrue(!e85.tableOpAndRender(EditorEngine::TableOp::RemoveColumn), "删最后一列被拒");
        expectTrue(e85.undo(), "撤销删列");
        e85.relayout();
        expectTrue(e85.tableSpecs()[0].cols == 2, "撤销之后列数回到 2");

        // 表单模式是填值态：结构命令与属性命令都不该落地。
        e85.setEditMode(EditMode::Form);
        expectTrue(!e85.tableOpAndRender(EditorEngine::TableOp::InsertRowBelow),
                   "表单模式下插行被拒");
        RowPatch rp85b;
        rp85b.is_header = true;
        expectTrue(!e85.setRowSpecAndRender(rp85b), "表单模式下改行记录被拒");
        expectTrue(!e85.setTableSpecAndRender(tp85), "表单模式下改表记录被拒");
        e85.setEditMode(EditMode::Document);

        // 落点归属：单元格里给表格，表格外的正文给文本。
        e85.setCaret(cellPara(0, 0), 0);
        Rect rc85{};
        expectTrue(e85.caretRectPt(&rc85), "读得到格内光标矩形");
        expectTrue(e85.contextTargetJson(rc85.x + 1.0f, rc85.y + 1.0f).find("\"kind\":1") !=
                       std::string::npos,
                   "★ 格内落点报的是表格");
        expectTrue(e85.contextTargetJson(rc85.x + 1.0f, rc85.y + 1.0f).find("\"cellRow\":0") !=
                       std::string::npos,
                   "落点带出格坐标");
        e85.setCaret(0, 1);
        expectTrue(e85.caretRectPt(&rc85), "读得到正文光标矩形");
        expectTrue(e85.contextTargetJson(rc85.x + 1.0f, rc85.y + 1.0f).find("\"kind\":0") !=
                       std::string::npos,
                   "★ 正文落点报的是文字");
        expectTrue(e85.contextTargetJson(1e6f, 1e6f).find("\"kind\":-1") != std::string::npos,
                   "页外落点报 none");

        // 收尾：三次自审都要为 0（结构、编译、排版），页位图与从零重光栅一致。
        e85.repaint();
        expectTrue(e85.auditTablesJson().find("\"diff\":0") != std::string::npos,
                   "表格结构自审为 0（" + e85.auditTablesJson() + "）");
        expectTrue(e85.auditCompile() == 0, "编译自审为 0");
        expectTrue(e85.auditLayout() == 0, "排版自审为 0");
        expectTrue(e85.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                   "页位图 == 从零重光栅");
    }

    if (runSection(86))
        std::printf("\n[86] 格内图片后敲字、整图选区、文字加控件混选删除、图片撤销\n");
    if (runSection(86)) {
        EngineConfig c86;
        c86.zoom = 1.0f;
        c86.page.width_pt = 320.0f;
        c86.page.height_pt = 300.0f;
        c86.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts86 = {cjkPath, mnPath, latinPath};

        const int iw86 = 96;
        const int ih86 = 48;
        std::vector<uint8_t> px86(static_cast<size_t>(iw86) * ih86 * 4u);
        for (int i = 0; i < iw86 * ih86; ++i) {
            px86[static_cast<size_t>(i) * 4u + 0] = 200;
            px86[static_cast<size_t>(i) * 4u + 1] = 30;
            px86[static_cast<size_t>(i) * 4u + 2] = 30;
            px86[static_cast<size_t>(i) * 4u + 3] = 255;
        }
        ImageSpec im86;
        im86.resource_id = "pic86";
        im86.px_w = iw86;
        im86.px_h = ih86;
        im86.dpi = 96;

        // 某一段排出了几个字形 / 几个行盒。
        auto paraGlyphs = [](const EditorEngine& e, int para) {
            const ParaIndex& pi = e.layout().para_index[static_cast<size_t>(para)];
            return pi.glyph_end - pi.glyph_begin;
        };
        auto paraLines = [](const EditorEngine& e, int para) {
            const ParaIndex& pi = e.layout().para_index[static_cast<size_t>(para)];
            return pi.line_end - pi.line_begin;
        };
        // 帧里某一页上某个页内坐标的像素。
        auto framePixel = [&](EditorEngine& e, int page, float x_pt, float y_pt, uint8_t* rgb) {
            const int fw = e.frameWidth();
            const int fh = e.frameHeight();
            const int step = e.pagePxHeight() + e.pageGapPx();
            const int px = static_cast<int>(std::lround(x_pt * c86.zoom));
            const int py = static_cast<int>(std::lround(y_pt * c86.zoom)) + page * step;
            rgb[0] = 0;
            rgb[1] = 0;
            rgb[2] = 0;
            if (px < 0 || py < 0 || px >= fw || py >= fh) return false;
            const uint8_t* fp = e.frame();
            const size_t at =
                (static_cast<size_t>(py) * static_cast<size_t>(fw) + static_cast<size_t>(px)) * 4u;
            rgb[0] = fp[at];
            rgb[1] = fp[at + 1];
            rgb[2] = fp[at + 2];
            return true;
        };

        // 一格宽 296 磅：图 72 磅，一行放得下二十来个字。
        int id = 3;
        auto mark = [&]() {
            return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
        };
        auto run = [&](const char* text) {
            return doc::makeNode(id++, NodeKind::TextRun,
                                 doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
        };
        const doc::Ref cell_a = doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {mark()});
        const doc::Ref cell_b =
            doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run("右格"), mark()});
        const doc::Ref table86 =
            doc::makeNode(id++, NodeKind::Table, TableSpec{},
                          {doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell_a, cell_b})});
        const doc::Ref sec86 = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                             {run("甲乙"), mark(), table86, run("尾段"), mark()});
        const NodeId root86 = id++;
        const doc::Doc t86 = doc::adopt(doc::makeNode(root86, NodeKind::Document, {}, {sec86}), id);

        EditorEngine e86;
        if (!e86.init(fonts86)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e86.setConfig(c86);
        e86.setDocument(t86, StyleSheet{});
        e86.relayout();
        e86.repaint();

        std::string err86;
        expectTrue(
            e86.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &err86),
            "注册一张 96x48 的图（" + err86 + "）");

        // 格坐标 → 块下标。
        auto blockInCell = [&](int row, int cell) {
            for (size_t i = 0; i < e86.blocks().size(); ++i) {
                const std::vector<CellSlot>& path = e86.blocks()[i].cell_path;
                if (!path.empty() && path.back().row == row && path.back().cell == cell) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        const int pa86 = blockInCell(0, 0);
        const int pb86 = blockInCell(0, 1);
        expectTrue(pa86 >= 0 && pb86 >= 0, "构造：一格一个段（" + std::to_string(pa86) + " / " +
                                               std::to_string(pb86) + "）");
        const int right_glyphs0 = (pb86 >= 0) ? paraGlyphs(e86, pb86) : 0;

        expectTrue(pa86 >= 0, "第一格有段落");
        if (pa86 >= 0) {
            e86.setCaret(pa86, 0);
            expectTrue(e86.insertImageAndRender(im86), "图插进格内那个空段");
            expectTrue(!e86.images().empty(), "文档里有一张图");
            expectTrue(e86.typeTextAndRender("戊己"), "在图后面敲两个字");

            // 内容真的进了这一段，字形也真的排出来了。
            expectTrue(e86.blocks()[static_cast<size_t>(pa86)].cp_text.size() == 2,
                       "格内那一段的内容是两个字（实际 " +
                           std::to_string(e86.blocks()[static_cast<size_t>(pa86)].cp_text.size()) +
                           "）");
            const int gl86 = paraGlyphs(e86, pa86);
            expectTrue(gl86 == 2, "★ 两个字都排出了字形（实际 " + std::to_string(gl86) + " 个）");
            expectTrue(
                paraLines(e86, pa86) == 1,
                "图与两个字在同一行（实际 " + std::to_string(paraLines(e86, pa86)) + " 行）");
            // 同一行另一格不受牵连。
            expectTrue(!e86.images().empty() && !e86.images()[0].rects.empty(), "图落在某一页上");
            if (pb86 >= 0) {
                expectTrue(
                    paraGlyphs(e86, pb86) == right_glyphs0,
                    "★ 右格的字形数没变（实际 " + std::to_string(paraGlyphs(e86, pb86)) + "）");
                expectTrue(paraLines(e86, pb86) == 1, "右格仍然只有一行");
            }
            // 增量结果与全量逐字段一致。
            expectTrue(e86.auditLayout() == 0, "★ 格内图片加文字：增量排版 == 全量重排");
            expectTrue(e86.auditCompile() == 0, "编译自审为 0");
            expectTrue(e86.auditPageJson(0).find("\"diff\":0") != std::string::npos,
                       "页位图 == 从零重光栅");

            // 再敲到跨行为止：新起的每一行都得有字，不能是一串空行。
            std::string more86;
            for (int i = 0; i < 60; ++i) more86 += "字";
            expectTrue(e86.typeTextAndRender(more86), "再敲六十个字（跨好几行）");
            const int gl86b = paraGlyphs(e86, pa86);
            expectTrue(gl86b == 62,
                       "★ 六十四个字全部排出字形（实际 " + std::to_string(gl86b) + " 个）");
            const int lines86 = paraLines(e86, pa86);
            expectTrue(lines86 >= 3, "真的跨行了（" + std::to_string(lines86) + " 行）");
            int empty_lines = 0;
            {
                const ParaIndex& pi = e86.layout().para_index[static_cast<size_t>(pa86)];
                for (int L = pi.line_begin; L < pi.line_end; ++L) {
                    const LineBox& lb = e86.layout().lines[static_cast<size_t>(L)];
                    int n = 0;
                    for (const PlacedGlyph& g : e86.layout().glyphs) {
                        if (g.line == lb.line && g.paragraph == pa86) ++n;
                    }
                    if (n == 0) ++empty_lines;
                }
            }
            expectTrue(empty_lines == 0,
                       "★ 每一行都有字形，没有空行（空行数 " + std::to_string(empty_lines) + "）");
            expectTrue(e86.auditLayout() == 0, "★ 跨行之后增量排版 == 全量重排");
        }

        // 整段只有一张图：选区要盖住图片，不是一条窄条。
        {
            const doc::Doc t87 = doc::makeTextDoc({U"", U""});
            EditorEngine e87;
            if (!e87.init(fonts86)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e87.setConfig(c86);
            e87.setDocument(t87, StyleSheet{});
            e87.relayout();
            e87.repaint();
            std::string err87;
            expectTrue(
                e87.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &err87),
                "注册一张图（" + err87 + "）");
            e87.setCaret(0, 0);
            expectTrue(e87.insertImageAndRender(im86), "插进第一段（那一段只有这张图）");
            const std::vector<EditorEngine::ImageBox> b87 = e87.images();
            expectTrue(b87.size() == 1 && !b87[0].rects.empty(), "图落在某一页上");
            const FlowPos a87 = e87.blockFlowStart(0);
            e87.setSelection(Range{a87, a87 + e87.blockFlowSpan(0)});
            e87.repaint();
            const std::vector<EditorEngine::PageRect> rr87 = e87.selectionRects();
            expectTrue(rr87.size() == 1,
                       "整图选区一条矩形（实际 " + std::to_string(rr87.size()) + " 条）");
            if (rr87.size() == 1) {
                expectTrue(std::fabs(rr87[0].r_pt.w - 72.0f) < 1.0f,
                           "★ 选区宽度盖住图片（实际 " + std::to_string(rr87[0].r_pt.w) +
                               " 磅，图宽 72）");
                expectTrue(rr87[0].r_pt.h > 30.0f,
                           "选区高度盖住图片（实际 " + std::to_string(rr87[0].r_pt.h) + " 磅）");
                if (!b87.empty() && !b87[0].rects.empty()) {
                    const Rect ir87 = b87[0].rects[0].r_pt;
                    expectTrue(std::fabs(rr87[0].r_pt.x - ir87.x) < 1.0f &&
                                   std::fabs(rr87[0].r_pt.y - ir87.y) < 2.0f,
                               "选区与图片同一位置");
                }
            }
        }

        // 选区里同时有文字与控件：删除要真的落地，不能整条被拒。
        {
            const doc::Doc t88 = doc::makeTextDoc({U"甲乙丙丁"});
            EditorEngine e88;
            if (!e88.init(fonts86)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e88.setConfig(c86);
            e88.setDocument(t88, StyleSheet{});
            e88.relayout();
            e88.repaint();
            e88.setCaret(0, 2);
            expectTrue(e88.insertControlAndRender(makeControlSpec("radioButton"), "单甲"),
                       "光标处插一个单选");
            expectTrue(e88.controlViews().size() == 1, "文档里一个控件");
            // 从第一个字选到最后（含控件那一块与末尾段落标记）。
            e88.setCaret(0, 0);
            expectTrue(e88.moveCaretToDocEnd(true), "扩选到文档末尾");
            const Range sel88 = e88.selection();
            expectTrue(!sel88.empty(), "确实有选区（" + std::to_string(sel88.begin()) + ".." +
                                           std::to_string(sel88.end()) + "）");
            const size_t blocks88 = e88.blocks().size();
            expectTrue(e88.deleteSelectionAndRender(), "★ 文字与控件一起删得掉");
            expectTrue(e88.controlViews().empty(),
                       "控件没了（还剩 " + std::to_string(e88.controlViews().size()) + " 个）");
            {
                size_t left = 0;
                for (const BlockSpec& b : e88.blocks()) left += b.cp_text.size();
                expectTrue(left == 0, "文档里一个字符都不剩（实际 " + std::to_string(left) +
                                          " 个，原 " + std::to_string(blocks88) + " 段）");
            }
            expectTrue(e88.auditCompile() == 0 && e88.auditLayout() == 0, "删完增量与全量一致");
            expectTrue(e88.undoAndRender(), "撤销这次删除");
            expectTrue(e88.controlViews().size() == 1, "★ 撤销把控件与文字一起恢复");
        }

        // 改尺寸之后撤销：图片仍选中（拖动框在），光标不冒到图右边，像素跟着回退。
        {
            const doc::Doc t89 = doc::makeTextDoc({U"ABCD", U""});
            EditorEngine e89;
            if (!e89.init(fonts86)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            e89.setConfig(c86);
            e89.setDocument(t89, StyleSheet{});
            e89.relayout();
            e89.repaint();
            std::string err89;
            expectTrue(
                e89.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &err89),
                "注册一张图（" + err89 + "）");
            e89.setCaret(1, 0);
            expectTrue(e89.insertImageAndRender(im86), "插进第二段（那一段只有这张图）");
            expectTrue(e89.selectImageAndRender(0), "选中这张图");
            expectTrue(!e89.editStats().caret_visible, "选中图片时不画文本光标");

            // 连改三次（每次一步），再撤销一步。
            expectTrue(e89.resizeImageAndRender(0, 60.0f, 30.0f), "第一次改尺寸");
            expectTrue(e89.resizeImageAndRender(0, 100.0f, 50.0f), "第二次改尺寸");
            expectTrue(e89.resizeImageAndRender(0, 240.0f, 120.0f), "第三次改尺寸（撑到第二页）");
            const int pages89 = e89.layout().stats.page_count;
            expectTrue(e89.undoAndRender(), "撤销第三次");
            expectTrue(std::fabs(e89.images()[0].w_pt - 100.0f) < 0.01f,
                       "尺寸回到 100 磅（实际 " + std::to_string(e89.images()[0].w_pt) + "）");
            expectTrue(e89.selectedImage() == 0, "★ 撤销之后图片仍然选中（拖动框在）");
            expectTrue(!e89.editStats().caret_visible, "★ 撤销之后光标没有冒到图片右边");
            // 像素与几何一致：图心那个点必须是图里的颜色。
            const std::vector<EditorEngine::ImageBox> b89 = e89.images();
            expectTrue(!b89.empty() && !b89[0].rects.empty(), "图仍在某一页上");
            if (!b89.empty() && !b89[0].rects.empty()) {
                e89.repaint();
                const EditorEngine::PageRect pr89 = b89[0].rects[0];
                uint8_t rgb89[3] = {0, 0, 0};
                const bool got89 = framePixel(e89, pr89.page, pr89.r_pt.x + pr89.r_pt.w * 0.5f,
                                              pr89.r_pt.y + pr89.r_pt.h * 0.5f, rgb89);
                expectTrue(got89 && rgb89[0] > 180 && rgb89[1] < 90,
                           "★ 撤销之后图心像素是图里的颜色（" +
                               std::to_string(static_cast<int>(rgb89[0])) + "," +
                               std::to_string(static_cast<int>(rgb89[1])) + "）");
                // 页缓存必须是本版排版光栅出来的那份像素。
                expectTrue(e89.auditPageJson(pr89.page).find("\"diff\":0") != std::string::npos,
                           "★ 撤销之后页位图 == 从零重光栅");
            }
            expectTrue(e89.layout().stats.page_count <= pages89,
                       "撤销之后页数没有反向增长（" +
                           std::to_string(e89.layout().stats.page_count) + " vs " +
                           std::to_string(pages89) + "）");
        }

        // 格内有图之后敲字的几种排法：都必须排出行盒与字形，且与全量重排一致。
        //   返回空串表示这一种没问题，否则是失败原因。
        //   一行没有字形可以接受 —— 那是"图单独占一行"（图比格宽时必然如此），
        //   但那一行必须真的有原子，否则就是凭空多出来的空行。
        auto cellVariant = [&](bool table_first, int rows, int cols, int at_row, int at_col,
                               const char* before, float w_pt, float h_pt, const char* typed,
                               float page_h, bool undo_after) -> std::string {
            int id = 3;
            auto text_run = [&](const std::string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::vector<doc::Ref> kids;
            if (!table_first) {
                kids.push_back(text_run("甲乙"));
                kids.push_back(mark());
            }
            std::vector<doc::Ref> table_rows;
            for (int r = 0; r < rows; ++r) {
                std::vector<doc::Ref> cells;
                for (int c = 0; c < cols; ++c) {
                    const bool target = (r == at_row && c == at_col);
                    std::vector<doc::Ref> cell_kids;
                    if (target) {
                        if (before[0] != '\0') cell_kids.push_back(text_run(before));
                    } else {
                        // 其他格填点字：行高与列宽才像一张真表，跨页也才有东西可切。
                        cell_kids.push_back(text_run("格内"));
                    }
                    cell_kids.push_back(mark());
                    cells.push_back(
                        doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, std::move(cell_kids)));
                }
                table_rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
            }
            kids.push_back(doc::makeNode(id++, NodeKind::Table, TableSpec{}, table_rows));
            kids.push_back(mark());
            const doc::Ref sec =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(kids));
            const NodeId root_id = id++;
            const doc::Doc tv =
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {sec}), id);

            EngineConfig cv = c86;
            cv.page.height_pt = page_h;
            EditorEngine ev;
            if (!ev.init(fonts86)) return "字体加载失败";
            ev.setConfig(cv);
            ev.setDocument(tv, StyleSheet{});
            ev.relayout();
            ev.repaint();
            std::string err;
            ev.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &err);
            int pa = -1;
            for (size_t i = 0; i < ev.blocks().size(); ++i) {
                const std::vector<CellSlot>& path = ev.blocks()[i].cell_path;
                if (!path.empty() && path.back().row == at_row && path.back().cell == at_col) {
                    pa = static_cast<int>(i);
                    break;
                }
            }
            if (pa < 0) return "找不到那一格的段";
            ev.setCaret(pa, ev.blocks()[static_cast<size_t>(pa)].cp_text.size());
            ImageSpec im;
            im.resource_id = "pic86";
            im.px_w = iw86;
            im.px_h = ih86;
            im.dpi = 96;
            im.width = Length::abs(ptToEmu(w_pt));
            im.height = Length::abs(ptToEmu(h_pt));
            if (!ev.insertImageAndRender(im)) return "插图失败";
            if (!ev.typeTextAndRender(typed)) return "敲字失败";
            if (undo_after) {
                // 撤销掉刚敲的字，再敲一遍：增量要在"图还在、文字来去"这条路上站得住。
                if (!ev.undoAndRender()) return "撤销敲字失败";
                if (!ev.typeTextAndRender(typed)) return "再敲一遍失败";
            }
            const size_t want = utf8ToCodepoints(before).size() + utf8ToCodepoints(typed).size();
            const size_t got = ev.blocks()[static_cast<size_t>(pa)].cp_text.size();
            if (got != want)
                return "内容 " + std::to_string(got) + " 个，应为 " + std::to_string(want);
            const ParaIndex& pi = ev.layout().para_index[static_cast<size_t>(pa)];
            const int glyphs = pi.glyph_end - pi.glyph_begin;
            if (glyphs != static_cast<int>(want)) {
                return "只排出 " + std::to_string(glyphs) + " 个字形，应为 " + std::to_string(want);
            }
            for (int L = pi.line_begin; L < pi.line_end; ++L) {
                const LineBox& lb = ev.layout().lines[static_cast<size_t>(L)];
                int n = 0;
                for (const PlacedGlyph& g : ev.layout().glyphs) {
                    if (g.line == lb.line && g.paragraph == pa) ++n;
                }
                if (n > 0) continue;
                bool holds_atom = false;
                for (const LineCluster& lc : lb.clusters) {
                    if (lc.atom != InlineAtom::None) holds_atom = true;
                }
                if (!holds_atom) return "第 " + std::to_string(L - pi.line_begin) + " 行没有字形";
            }
            if (ev.auditLayout() != 0) return "增量排版与全量重排不一致";
            // 三份自审都可能同错（见 TRAPS 210）：每个字形必须落在它那一行的页上、
            //   纵向也在那条行盒里。少这一条，"字画在上一页"这种错不会有人报。
            for (const PlacedGlyph& g : ev.layout().glyphs) {
                const LineBox* hit = nullptr;
                for (const LineBox& lb : ev.layout().lines) {
                    if (lb.line == g.line && lb.paragraph == g.paragraph) {
                        hit = &lb;
                        break;
                    }
                }
                if (hit == nullptr) return "有字形找不到它那一行的行盒";
                if (hit->page != g.page) {
                    return "字形画在第 " + std::to_string(g.page) + " 页，行盒在第 " +
                           std::to_string(hit->page) + " 页";
                }
                if (g.y_pt < hit->block_lo_pt - 1.0f || g.y_pt > hit->block_hi_pt + 1.0f) {
                    return "字形纵向落在行盒之外（行 " + std::to_string(hit->block_lo_pt) + ".." +
                           std::to_string(hit->block_hi_pt) + "，字形 " + std::to_string(g.y_pt) +
                           "）";
                }
            }
            ev.repaint();
            if (ev.auditPageJson(0).find("\"diff\":0") == std::string::npos)
                return "页位图与重光栅不一致";
            return std::string();
        };

        const std::string v1 =
            cellVariant(/*table_first=*/true, 1, 1, 0, 0, "", 72.0f, 36.0f, "戊己", 300.0f, false);
        expectTrue(v1.empty(), "★ 表在文首、空格插图后敲字：" + v1);
        const std::string v2 =
            cellVariant(false, 2, 2, 1, 1, "甲乙", 72.0f, 36.0f, "丙丁戊", 300.0f, false);
        expectTrue(v2.empty(), "★ 表格右下格、先有字再插图再敲字：" + v2);
        const std::string v3 =
            cellVariant(false, 1, 2, 0, 0, "", 260.0f, 60.0f, "戊己庚辛", 300.0f, false);
        expectTrue(v3.empty(), "★ 图比格还宽的格内敲字：" + v3);
        const std::string v4 =
            cellVariant(false, 1, 1, 0, 0, "", 60.0f, 200.0f, "戊己庚辛壬癸", 300.0f, false);
        expectTrue(v4.empty(), "★ 图几乎占满一页的格内敲字：" + v4);
        const std::string v5 =
            cellVariant(false, 8, 2, 5, 1, "", 72.0f, 36.0f, "戊己庚辛", 200.0f, false);
        expectTrue(v5.empty(), "★ 表格跨页时在格里的图后敲字：" + v5);
        const std::string v6 =
            cellVariant(false, 2, 2, 0, 0, "", 72.0f, 36.0f, "戊己庚辛", 300.0f, true);
        expectTrue(v6.empty(), "★ 格内图后敲字、撤销、再敲一遍：" + v6);
        const std::string v7 =
            cellVariant(false, 1, 1, 0, 0, "", 60.0f, 400.0f, "戊己庚辛", 300.0f, false);
        expectTrue(v7.empty(), "★ 图比整页还高的格内敲字：" + v7);
        const std::string v8 =
            cellVariant(true, 1, 1, 0, 0, "甲乙", 72.0f, 36.0f, "丙丁", 300.0f, false);
        expectTrue(v8.empty(), "★ 表在文首、格内先有字再插图再敲字：" + v8);

        // 图插在 (0,0) 格、字敲在 (0,1) 格 —— 敲字那格不是插图那格，一行里另一格变高
        //   之后，图片那一格的行盒与整行的行高都要跟着结算。图片照样走"先插原始尺寸、
        //   再缩小"这条真实路径（拖动把手松手就是它）。
        auto neighborVariant = [&](const char* typed) -> std::string {
            int id = 3;
            auto text_run = [&](const std::string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            // 与真实页面同构：表格前面有一段正文，表格从第 1 段起。
            std::vector<doc::Ref> kids;
            kids.push_back(text_run("首段"));
            kids.push_back(mark());
            std::vector<doc::Ref> table_rows;
            for (int r = 0; r < 3; ++r) {
                std::vector<doc::Ref> cells;
                for (int c = 0; c < 3; ++c) {
                    std::vector<doc::Ref> cell_kids;
                    // 与用户点出来的表一致：只有第一行前两格有东西（一格图、一格字），
                    //   其余格是空段 —— 行高就只由这两格里的高者结算。
                    if (r == 0 && c == 2) cell_kids.push_back(text_run("格内"));
                    cell_kids.push_back(mark());
                    cells.push_back(
                        doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, std::move(cell_kids)));
                }
                table_rows.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
            }
            kids.push_back(doc::makeNode(id++, NodeKind::Table, TableSpec{}, table_rows));
            kids.push_back(mark());
            const doc::Ref sec =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(kids));
            const NodeId root_id = id++;
            const doc::Doc tv =
                doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {sec}), id);

            EditorEngine ev;
            if (!ev.init(fonts86)) return "字体加载失败";
            // 与浏览器里那一页同一套度量：A4、72 磅边距、4/3 缩放 —— 行带与脏矩形的
            //   取整都吃缩放，1.0 下算不出来的接缝在 4/3 下才露。
            EngineConfig cv = c86;
            cv.zoom = 4.0f / 3.0f;
            cv.page.width_pt = 595.0f;
            cv.page.height_pt = 842.0f;
            cv.page.margins = PageMargins::all(72.0f);
            ev.setConfig(cv);
            ev.setDocument(tv, StyleSheet{});
            ev.relayout();
            ev.repaint();
            std::string err;
            ev.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &err);
            auto para_of = [&](int row, int col) -> int {
                for (size_t i = 0; i < ev.blocks().size(); ++i) {
                    const std::vector<CellSlot>& path = ev.blocks()[i].cell_path;
                    if (!path.empty() && path.back().row == row && path.back().cell == col) {
                        return static_cast<int>(i);
                    }
                }
                return -1;
            };
            const int p_img = para_of(0, 0);
            const int p_txt = para_of(0, 1);
            if (p_img < 0 || p_txt < 0) return "找不到那一格的段";
            ev.setCaret(p_img, 0);
            ImageSpec im;
            im.resource_id = "pic86";
            im.px_w = iw86;
            im.px_h = ih86;
            im.dpi = 96;
            im.width = Length::abs(ptToEmu(150.0f));
            im.height = Length::abs(ptToEmu(100.0f));
            if (!ev.insertImageAndRender(im)) return "插图失败";
            if (!ev.selectImageAndRender(0)) return "选不中这张图";
            if (!ev.resizeImageAndRender(0, 60.0f, 30.0f)) return "缩小图片失败";
            ev.setCaret(p_txt, 0);
            // 逐字敲，与真实击键同频：一次贴一串走的是一条批量路径，跨行只发生一次；
            //   逐字会反复把"这一格从一行变两行"这个中间态交给增量排版。
            const std::string one(typed);
            for (size_t k = 0; k < one.size(); ++k) {
                if (!ev.typeTextAndRender(std::string(1, one[k]))) return "敲字失败";
                const EditStats st = ev.editStats();
                if (st.layout_was_full) return "第 " + std::to_string(k) + " 键走了全量重排";
                // 每敲一个字都当场对一次页位图。漏画是"某一次局部重绘没盖住变化区"
                //   攒出来的：只在末尾对一次，看到的是几十次叠加的差异，定位不到那一刀。
                if (ev.auditPageJson(0).find("\"diff\":0") == std::string::npos) {
                    return "第 " + std::to_string(k) + " 键之后页位图与重光栅不一致";
                }
            }

            const size_t want = utf8ToCodepoints(typed).size();
            const size_t got = ev.blocks()[static_cast<size_t>(p_txt)].cp_text.size();
            if (got != want)
                return "敲字那格内容 " + std::to_string(got) + " 个，应为 " + std::to_string(want);
            const ParaIndex& pi = ev.layout().para_index[static_cast<size_t>(p_txt)];
            const int glyphs = pi.glyph_end - pi.glyph_begin;
            if (glyphs != static_cast<int>(want)) {
                return "敲字那格只排出 " + std::to_string(glyphs) + " 个字形，应为 " +
                       std::to_string(want);
            }
            if (pi.line_end - pi.line_begin < 2) return "没有折到第二行";
            for (int L = pi.line_begin; L < pi.line_end; ++L) {
                const LineBox& lb = ev.layout().lines[static_cast<size_t>(L)];
                int n = 0;
                for (const PlacedGlyph& g : ev.layout().glyphs) {
                    if (g.line == lb.line && g.paragraph == p_txt) ++n;
                }
                if (n == 0) return "第 " + std::to_string(L - pi.line_begin) + " 行没有字形";
            }
            // 图片那一格的行盒不能被敲字那格带歪：两格各算各的块坐标，能比的是行数 ——
            //   图缩小到 72x36 之后只占一行，敲字那格折行不该把它的行盒变多。
            const ParaIndex& pimg = ev.layout().para_index[static_cast<size_t>(p_img)];
            if (pimg.line_end - pimg.line_begin > 1) {
                return "图片那格凭空多出了 " + std::to_string(pimg.line_end - pimg.line_begin - 1) +
                       " 个行盒";
            }
            if (ev.auditLayout() != 0) return "增量排版与全量重排不一致";
            for (const PlacedGlyph& g : ev.layout().glyphs) {
                const LineBox* hit = nullptr;
                for (const LineBox& lb : ev.layout().lines) {
                    if (lb.line == g.line && lb.paragraph == g.paragraph) {
                        hit = &lb;
                        break;
                    }
                }
                if (hit == nullptr) return "有字形找不到它那一行的行盒";
                if (hit->page != g.page) {
                    return "字形画在第 " + std::to_string(g.page) + " 页，行盒在第 " +
                           std::to_string(hit->page) + " 页";
                }
            }
            {
                // 内容没变时再呈现一次：不该重画，也不该把上一次的结果带偏。
                ev.repaint();
                if (ev.auditPageJson(0).find("\"diff\":0") == std::string::npos)
                    return "重复呈现之后页位图与重光栅不一致";
            }
            ev.repaint();
            if (ev.auditPageJson(0).find("\"diff\":0") == std::string::npos)
                return "页位图与重光栅不一致";
            return std::string();
        };

        const std::string n1 =
            neighborVariant("asdfa asdf a a asdf asdf asdf asf asf as asdf asdf");
        expectTrue(n1.empty(), "★ 图在左格、字敲在右格并折行：" + n1);

        // 表格与图片段落之间的间距：两种次序各量一次。
        {
            const doc::Ref cell_tg =
                doc::makeNode(4, NodeKind::TableCell, CellSpec{},
                              {doc::makeNode(5, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Ref table_tg =
                doc::makeNode(3, NodeKind::Table, TableSpec{},
                              {doc::makeNode(6, NodeKind::TableRow, RowSpec{}, {cell_tg})});
            const doc::Ref sec_tg = doc::makeNode(
                2, NodeKind::Section, SectionSpec{},
                {doc::makeNode(7, NodeKind::ParagraphMark, doc::MarkPayload{}, {}), table_tg,
                 doc::makeNode(8, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Doc tg = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {sec_tg}), 9);
            EditorEngine eg;
            if (!eg.init(fonts86)) {
                std::fprintf(stderr, "字体加载失败\n");
                return 2;
            }
            eg.setConfig(c86);
            eg.setDocument(tg, StyleSheet{});
            eg.relayout();
            eg.repaint();
            std::string errg;
            eg.registerImageAndRepaint("pic86", iw86, ih86, px86.data(), px86.size(), &errg);
            eg.setCaret(0, 0);
            eg.insertImageAndRender(im86);
            const int lastg = static_cast<int>(eg.blocks().size()) - 1;
            eg.setCaret(lastg, 0);
            eg.insertImageAndRender(im86);
            const std::vector<EditorEngine::ImageBox> bg = eg.images();
            expectTrue(bg.size() == 2 && !eg.layout().placed_tables.empty(),
                       "构造：两张图夹一张表");
            if (bg.size() == 2 && !bg[0].rects.empty() && !bg[1].rects.empty() &&
                !eg.layout().placed_tables.empty()) {
                const PlacedTable& pt = eg.layout().placed_tables[0];
                const float img1_bottom = bg[0].rects[0].r_pt.y + bg[0].rects[0].r_pt.h;
                const float table_top = pt.placed.front().y;
                const float table_bottom = pt.placed.back().y + pt.placed.back().h;
                const float img2_top = bg[1].rects[0].r_pt.y;
                // 表前用前一段的段后距，表后补一段同样的间距：两边都不该贴住表格。
                expectTrue(table_top - img1_bottom >= 5.9f,
                           "★ 图与表格之间有间距（表顶 " + std::to_string(table_top) + " - 图底 " +
                               std::to_string(img1_bottom) + "）");
                expectTrue(img2_top - table_bottom >= 5.9f,
                           "★ 表格与下一段图之间有间距（图顶 " + std::to_string(img2_top) +
                               " - 表底 " + std::to_string(table_bottom) + "）");
            }
        }
    }

    if (runSection(87)) std::printf("\n[87] 字数口径与表格命令的可用性\n");
    if (runSection(87)) {
        EngineConfig c87;
        c87.zoom = 1.0f;
        c87.page.width_pt = 595.0f;
        c87.page.height_pt = 842.0f;
        c87.page.margins = PageMargins::all(56.0f);
        const std::vector<std::string> fonts87 = {cjkPath, mnPath, latinPath};

        // 中间放一个 ASCII 空格与一个全角空格：两个都只在码点数里出现，不在字数里。
        const doc::Doc t87 = doc::makeTextDoc({U"甲乙 丙\u3000丁"});
        EditorEngine e87;
        if (!e87.init(fonts87)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e87.setConfig(c87);
        e87.setDocument(t87, StyleSheet{});
        e87.relayout();

        expectTrue(e87.documentCodepoints() == 6,
                   "码点数 = 6（甲乙 + 空格 + 丙 + 全角空格 + 丁，实际 " +
                       std::to_string(e87.documentCodepoints()) + "）");
        expectTrue(
            e87.documentWordCount() == 4,
            "★ 字数 = 4：两个空白不计（实际 " + std::to_string(e87.documentWordCount()) + "）");

        // 空段落不产字：全空白的一段加到 0。
        const doc::Doc t87b = doc::makeTextDoc({U" \u3000 "});
        EditorEngine e87b;
        if (!e87b.init(fonts87)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e87b.setConfig(c87);
        e87b.setDocument(t87b, StyleSheet{});
        e87b.relayout();
        expectTrue(
            e87b.documentWordCount() == 0,
            "★ 只有空白的一段：字数 0（实际 " + std::to_string(e87b.documentWordCount()) + "）");

        // 不在表格里：六条结构命令一条都不该报可用。
        const bool none_ok = !e87.canTableOp(EditorEngine::TableOp::InsertRowAbove) &&
                             !e87.canTableOp(EditorEngine::TableOp::RemoveRow) &&
                             !e87.canTableOp(EditorEngine::TableOp::InsertColumnRight) &&
                             !e87.canTableOp(EditorEngine::TableOp::RemoveColumn) &&
                             !e87.canTableOp(EditorEngine::TableOp::MergeCells) &&
                             !e87.canTableOp(EditorEngine::TableOp::SplitCell);
        expectTrue(none_ok, "★ 光标在正文：六条表格命令全部不可用");

        // 1×1：插得进、删不掉；没有整格选区不能并，跨度 1 不能拆。
        e87.setCaret(0, 0);
        expectTrue(e87.insertTableAndRender(1, 1, false), "插一张 1×1 表");
        const bool one_by_one = e87.canTableOp(EditorEngine::TableOp::InsertRowAbove) &&
                                e87.canTableOp(EditorEngine::TableOp::InsertColumnRight) &&
                                !e87.canTableOp(EditorEngine::TableOp::RemoveRow) &&
                                !e87.canTableOp(EditorEngine::TableOp::RemoveColumn) &&
                                !e87.canTableOp(EditorEngine::TableOp::MergeCells) &&
                                !e87.canTableOp(EditorEngine::TableOp::SplitCell);
        expectTrue(one_by_one, "★ 1×1：只能插，不能删 / 并 / 拆");

        // 2×2：删得了（反向一半）。
        expectTrue(e87.insertTableAndRender(2, 2, false), "另插一张 2×2 表");
        expectTrue(e87.canTableOp(EditorEngine::TableOp::RemoveRow) &&
                       e87.canTableOp(EditorEngine::TableOp::RemoveColumn),
                   "★ 2×2：删行删列都可用");

        // 只读模式：结构命令全关（与 tableOp 同一条判定）。
        e87.setEditMode(EditMode::ReadOnly);
        expectTrue(!e87.canTableOp(EditorEngine::TableOp::InsertRowAbove) &&
                       !e87.canTableOp(EditorEngine::TableOp::RemoveRow) &&
                       !e87.canTableOp(EditorEngine::TableOp::RemoveColumn),
                   "★ 只读模式：一条都不报可用");
        e87.setEditMode(EditMode::Document);
    }

    if (runSection(88)) std::printf("\n[88] 业务只读：投影不再占用这个位\n");
    if (runSection(88)) {
        EngineConfig c88;
        c88.zoom = 1.0f;
        c88.page.width_pt = 595.0f;
        c88.page.height_pt = 842.0f;
        c88.page.margins = PageMargins::all(56.0f);
        const std::vector<std::string> fonts88 = {cjkPath, mnPath, latinPath};

        const doc::Doc t88 = doc::makeTextDoc({U"甲乙"});
        EditorEngine e88;
        if (!e88.init(fonts88)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e88.setConfig(c88);
        e88.setDocument(t88, StyleSheet{});
        e88.relayout();

        e88.setCaret(0, 1);
        ControlSpec d88;
        d88.controlType = "date";
        expectTrue(e88.insertControlAndRender(d88, ""), "插一个日期控件");
        const std::vector<EditorEngine::ControlView> v88 = e88.controlViews();
        expectTrue(v88.size() == 1 && !v88[0].readonly,
                   "★ 日期控件的业务只读位是假：\"内容只是投影\"不占用它");
        expectTrue(v88.size() == 1 && v88[0].shape == ValueShape::Enum,
                   "投影由值形态表达（枚举 = 值在记录上）");

        ControlValue first;
        first.text = "2026-10-26T14:30:00";
        expectTrue(e88.setControlValue(0, first), "默认可填：设值成功");

        expectTrue(e88.setControlProp(0, ControlProp::Readonly, true), "设成业务只读");
        ControlValue second;
        second.text = "2027-01-01T08:00:00";
        expectTrue(!e88.setControlValue(0, second),
                   "★ 只读之后拒写（这条原来返回真：只读只挡得住键盘）");
        expectTrue(e88.controlViews()[0].value.text == "2026-10-26T14:30:00", "值一个字符都没动");

        expectTrue(e88.setControlProp(0, ControlProp::Readonly, false), "取消只读");
        expectTrue(e88.setControlValue(0, second), "★ 反向：取消之后同一个写入成功");
        expectTrue(e88.controlViews()[0].value.text == "2027-01-01T08:00:00", "改到了新值");

        // 表单模式下光标停不进投影内容（值在记录上，那个位置没有可编辑正文）。
        FlowPos inner88 = -1;
        for (const InlineObject& o : e88.blocks()[0].objects) {
            if (o.kind == InlineObjectKind::ControlStart) {
                inner88 = o.cp + 1;
                break;
            }
        }
        e88.setEditMode(EditMode::Form);
        expectTrue(inner88 > 0 && !e88.insertableAtFlow(inner88),
                   "★ 表单模式下光标停不进日期的投影内容");
        // 插入只在文档模式放行（改结构是设计动作），插第二个控件之前要切回来。
        e88.setEditMode(EditMode::Document);

        // 复选：只读之后勾不动，且勾选状态留在原处。
        ControlSpec cb88;
        cb88.controlType = "checkBox";
        expectTrue(e88.insertControlAndRender(cb88, "知情同意"), "插一个复选");
        const int cbIndex = static_cast<int>(e88.controlViews().size()) - 1;
        expectTrue(e88.toggleControl(cbIndex, -1), "默认可勾");
        expectTrue(e88.setControlProp(cbIndex, ControlProp::Readonly, true), "复选设成只读");
        expectTrue(!e88.toggleControl(cbIndex, -1), "★ 只读的复选勾不动");
        expectTrue(e88.controlViews()[cbIndex].value.flag, "反向：勾选状态没被改掉");
        e88.setEditMode(EditMode::Document);
    }

    if (runSection(89)) std::printf("\n[89] 空控件的提示文字：占宽度不占位置、有内容就不画\n");
    if (runSection(89)) {
        EngineConfig c89;
        c89.zoom = 1.0f;
        c89.page.width_pt = 595.0f;
        c89.page.height_pt = 842.0f;
        c89.page.margins = PageMargins::all(56.0f);
        const std::vector<std::string> fonts89 = {cjkPath, mnPath, latinPath};

        const doc::Doc t89 = doc::makeTextDoc({U"甲乙"});
        EditorEngine e89;
        if (!e89.init(fonts89)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e89.setConfig(c89);
        e89.setDocument(t89, StyleSheet{});
        e89.relayout();

        ControlSpec spec89 = makeControlSpec("text");
        spec89.appearance.placeholder = "请输入姓名";
        e89.setCaret(0, 2);  // 段末
        expectTrue(e89.insertControlAndRender(spec89, ""), "插入一个空内容的文本控件");

        const std::vector<EditorEngine::ControlView> v89 = e89.controlViews();
        expectTrue(v89.size() == 1 && v89[0].content.empty(), "控件内容为空：提示不是内容");
        expectTrue(v89.size() == 1 && v89[0].placeholder == "请输入姓名", "提示文字记在记录上");
        // 正文里不能出现那句话的一半：它是画出来的，不是插进去的字。
        std::string body89;
        for (uint32_t cp : e89.blocks()[0].cp_text) body89 += codepointsToUtf8({cp});
        expectTrue(body89 == "甲乙", "正文仍是两个字（提示没进内容）");

        // 槽序：开括号、提示、闭括号，三枚挤在同一个 cp 上。
        std::vector<const LineCluster*> slots89;
        for (const LineBox& lb : e89.layout().lines) {
            for (const LineCluster& c : lb.clusters) {
                if (c.placeholder) slots89.push_back(&c);
            }
        }
        expectTrue(slots89.size() == 3,
                   "★ 三枚槽：两枚括号夹一句提示（实 " + std::to_string(slots89.size()) + " 枚）");
        if (slots89.size() == 3) {
            expectTrue(slots89[0]->bracket_side == BracketSide::Open, "槽序：先是开括号");
            expectTrue(slots89[1]->control_hint, "槽序：中间是提示文字");
            expectTrue(slots89[2]->bracket_side == BracketSide::Close, "槽序：最后是闭括号");
        }

        // 提示槽的宽度 = 它排出来的那一段，且它确实占着宽度（后面的括号被推开）。
        float hint_w89 = 0.0f;
        for (const LineBox& lb : e89.layout().lines) {
            for (size_t i = 0; i < lb.clusters.size(); ++i) {
                if (!lb.clusters[i].control_hint) continue;
                const float a = lb.clusters[i].inline_pt;
                const float b =
                    (i + 1 < lb.clusters.size()) ? lb.clusters[i + 1].inline_pt : lb.inline_end_pt;
                hint_w89 = b - a;
            }
        }
        expectTrue(hint_w89 > 20.0f, "★ 提示占了宽度（" + std::to_string(hint_w89) + " 磅）");

        // 光标：控件内部与控件之后差出这一句话的宽度 —— 提示在流里不是位置，但在版面上是宽度。
        FlowPos lo89 = 0;
        FlowPos hi89 = 0;
        expectTrue(e89.controlContentSpan(0, &lo89, &hi89), "内容区间量得到");
        expectTrue(lo89 == hi89, "空控件的内容区间退化成一个点（提示没有多造一个位置）");
        Rect r_in89{};
        Rect r_out89{};
        expectTrue(e89.setCaretAtFlow(lo89) && e89.caretRectPt(&r_in89), "光标停在控件内部");
        expectTrue(e89.setCaretAtFlow(hi89 + 1) && e89.caretRectPt(&r_out89), "光标停在控件之后");
        expectTrue(r_out89.x - r_in89.x > hint_w89 * 0.5f,
                   "★ 里面与后面差出一句话的宽（差 " + std::to_string(r_out89.x - r_in89.x) +
                       " 磅 / 提示 " + std::to_string(hint_w89) + " 磅）");

        // 有内容之后提示消失，光标停在内容里逐字编辑。
        expectTrue(e89.setCaretAtFlow(lo89), "光标回到空控件内部");
        expectTrue(e89.typeTextUtf8("张"), "在空控件里打入一个字");
        e89.relayout();
        expectTrue(e89.controlViews()[0].content == "张", "字进了控件内容");
        int hints89 = 0;
        for (const LineBox& lb : e89.layout().lines) {
            for (const LineCluster& c : lb.clusters) {
                if (c.control_hint) ++hints89;
            }
        }
        expectTrue(hints89 == 0, "★ 有内容之后不再画提示");

        // 清空回到空控件：换一句等宽的提示，槽宽一模一样而摘要必须变 ——
        //   不然画面会停在上一句上（行指纹只看几何）。
        expectTrue(e89.setControlValueAndRender(0, ControlValue{}), "清空内容");
        auto hintOf89 = [&]() -> const LineCluster* {
            for (const LineBox& lb : e89.layout().lines) {
                for (const LineCluster& c : lb.clusters) {
                    if (c.control_hint) return &c;
                }
            }
            return nullptr;
        };
        auto hintWidth89 = [&]() -> float {
            for (const LineBox& lb : e89.layout().lines) {
                for (size_t i = 0; i < lb.clusters.size(); ++i) {
                    if (!lb.clusters[i].control_hint) continue;
                    return ((i + 1 < lb.clusters.size()) ? lb.clusters[i + 1].inline_pt
                                                         : lb.inline_end_pt) -
                           lb.clusters[i].inline_pt;
                }
            }
            return 0.0f;
        };
        const LineCluster* h89a = hintOf89();
        const float w89a = hintWidth89();
        const uint64_t hash89a = (h89a != nullptr) ? h89a->content_hash : 0;
        ControlPatch ph89;
        ph89.placeholder = std::string("请输入名字");  // 与上一句同字数 = 同宽
        expectTrue(e89.setControlSpecAndRender(0, ph89), "改提示文字");
        const LineCluster* h89b = hintOf89();
        const float w89b = hintWidth89();
        expectTrue(h89b != nullptr && std::fabs(w89b - w89a) < 0.01f,
                   "换一句等宽的提示：槽宽一点没变（" + std::to_string(w89a) + " / " +
                       std::to_string(w89b) + "）");
        expectTrue(h89b != nullptr && h89b->content_hash != hash89a,
                   "★ 宽度不变而摘要变了：行指纹靠它判得出要重画");

        // 装不下就截断到容器宽：一句很长的话不该把正文推开一屏。
        ControlPatch long89;
        for (int i = 0; i < 40; ++i) long89.placeholder = *long89.placeholder + "字";
        expectTrue(e89.setControlSpecAndRender(0, long89), "换成一句很长的提示");
        const float w89c = hintWidth89();
        expectTrue(w89c > 0.0f && w89c <= c89.page.contentWidthPt() + 0.01f,
                   "★ 提示宽度压到容器宽（" + std::to_string(w89c) + " / " +
                       std::to_string(c89.page.contentWidthPt()) + "）");

        // 只读（呈现与打印）态：提示不画，也不占位 —— 与括号同一个口径。
        e89.setEditMode(EditMode::ReadOnly);
        e89.relayout();
        int hintsRo89 = 0;
        for (const LineBox& lb : e89.layout().lines) {
            for (const LineCluster& c : lb.clusters) {
                if (c.control_hint) ++hintsRo89;
            }
        }
        expectTrue(hintsRo89 == 0, "★ 只读态不画提示，也不给它占位");
        e89.setEditMode(EditMode::Document);
        e89.relayout();

        // 空判定仍按流跨度（两枚括号）：提示进排版不进流，所以贴它退格仍是"删整块"。
        expectTrue(e89.controlContentSpan(0, &lo89, &hi89) && lo89 == hi89, "又是空控件");
        expectTrue(e89.setCaretAtFlow(lo89), "光标在空控件内部");
        expectTrue(e89.backspace(), "贴空控件退格");
        expectTrue(e89.controlViews().empty(), "★ 空控件被整块删掉（提示没有改变空判定）");

        // 空的下拉与日期也走这一套：内容是空的、提示是排版画的那句灰字。
        //   这两类的提示如果投影成内容，字号与墨色就跟正文一样（看着不像提示），
        //   还会被算进字数、被复制带走。
        {
            const doc::Doc td = doc::makeTextDoc({U"科室"});
            EditorEngine e89d;
            expectTrue(e89d.init(fonts89), "引擎初始化（空下拉）");
            e89d.setConfig(c89);
            e89d.setDocument(td, StyleSheet{});
            e89d.relayout();

            // 提示槽在哪一条行盒上（同一条判据给下面两次用）。
            auto hintOf = [&]() -> const LineCluster* {
                for (const LineBox& lb : e89d.layout().lines) {
                    for (const LineCluster& c : lb.clusters) {
                        if (c.control_hint) return &c;
                    }
                }
                return nullptr;
            };

            ControlSpec dd = makeControlSpec("dropdownList");
            if (auto* d = std::get_if<DropdownDetail>(&dd.detail)) {
                d->options = {{"a", "内科"}, {"b", "外科"}};
            }
            dd.appearance.placeholder = "请选择科室";
            e89d.setCaret(0, 2);
            expectTrue(e89d.insertControlAndRender(dd, ""), "插入一个空的下拉");
            std::vector<EditorEngine::ControlView> vd = e89d.controlViews();
            expectTrue(vd.size() == 1 && vd[0].content.empty(),
                       "★ 空下拉的内容是空的（提示不进内容）");
            expectTrue(vd.size() == 1 && vd[0].placeholder == "请选择科室", "提示记在记录上");
            expectTrue(hintOf() != nullptr, "★ 空下拉照样有提示槽（不是零宽控件）");
            expectTrue(!e89d.controlRects(0).empty(), "空下拉有几何（点得中，才弹得出选项）");

            ControlValue dv;
            dv.text = "b";
            expectTrue(e89d.setControlValueAndRender(0, dv), "选中外科");
            expectTrue(e89d.controlViews()[0].content == "外科", "有值之后内容换成选项标签");
            expectTrue(hintOf() == nullptr, "★ 有值之后不再画提示");

            // 日期同一条；没写 placeholder 时退回内置的那一句（"选择日期"）。
            ControlSpec dt = makeControlSpec("date");
            e89d.moveCaretToDocEnd();
            expectTrue(e89d.insertControlAndRender(dt, ""), "插入一个空日期");
            const auto views = e89d.controlViews();
            expectTrue(views.size() == 2 && views[1].content.empty(), "★ 空日期的内容是空的");
            expectTrue(hintOf() != nullptr, "★ 空日期照样有提示槽");
        }
    }

    if (runSection(90)) std::printf("\n[90] 复制：控件只被盖住一半时只取选区那几段文字\n");
    if (runSection(90)) {
        EngineConfig c90;
        c90.zoom = 1.0f;
        c90.page.width_pt = 400.0f;
        c90.page.height_pt = 240.0f;
        c90.page.margins = PageMargins::all(12.0f);
        const std::vector<std::string> fonts90 = {cjkPath, mnPath, latinPath};

        // 一段文字中间夹一个控件：甲乙[丙丁]戊己。控件的两枚边界落在段中间。
        ControlSpec no;
        no.key = "no";
        no.controlType = "text";
        const doc::Ref control90 = doc::makeNode(
            4, NodeKind::ContentControl, no,
            {doc::makeNode(5, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("丙丁"), CharFormat{}, {}}, {})});
        const doc::Ref section90 = doc::makeNode(
            2, NodeKind::Section, SectionSpec{},
            {doc::makeNode(3, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
             control90,
             doc::makeNode(6, NodeKind::TextRun,
                           doc::RunPayload{utf8ToCodepoints("戊己"), CharFormat{}, {}}, {}),
             doc::makeNode(7, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const doc::Doc t90 = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section90}), 8);
        EditorEngine e90;
        if (!e90.init(fonts90)) {
            std::fprintf(stderr, "字体加载失败\n");
            return 2;
        }
        e90.setConfig(c90);
        e90.setDocument(t90, StyleSheet{});
        e90.relayout();

        // 选区盖住"乙"与控件里的前一段（"丙"）：复制的应当只有这两段文字，
        //   控件壳（括号）与没选中的"丁"都不进片段。
        expectTrue(e90.setSelection(Range{e90.flowPosAt(0, 1), e90.flowPosAt(0, 3)}),
                   "选中「乙」与控件里的一半");
        const ClipCopy half = e90.copySelection();
        expectTrue(half.ok, "半覆盖控件时复制成功（不是整块带走，也不是报错）");
        expectTrue(half.text == "乙丙", "★ 复制的纯文本只有选区那两段（实际 " + half.text + "）");
        expectTrue(half.fragment.find("ContentControl") == std::string::npos,
                   "★ 片段里没有控件（半个控件没有可粘贴的形状）");

        // 反向一半：控件被整个包住时照旧整块复制 —— 结构不能因为"半覆盖"这条而丢。
        expectTrue(e90.setSelection(Range{e90.flowPosAt(0, 0), e90.flowPosAt(0, 6)}),
                   "选中整段（控件完整在内）");
        const ClipCopy whole = e90.copySelection();
        expectTrue(whole.ok, "整段复制成功");
        expectTrue(whole.fragment.find("ContentControl") != std::string::npos,
                   "★ 完整包住的控件照旧整块进片段");
        expectTrue(whole.text == "甲乙丙丁戊己", "纯文本仍是整段（实际 " + whole.text + "）");
        expectTrue(e90.auditCompile() == 0 && e90.auditLayout() == 0,
                   "复制不产生改动：增量与全量仍然逐字段一致");

        // 剪切 = 复制 + 删选区：删的必须正好是复制走的那一段。跨控件边界按流区间删会被
        //   安全阀拒掉（症状是"剪切不好使"），这里钉住它真的删了、而且只删选中的那两段。
        expectTrue(e90.setSelection(Range{e90.flowPosAt(0, 1), e90.flowPosAt(0, 3)}),
                   "再选一次同一段");
        expectTrue(e90.deleteSelection(), "★ 跨控件边界删选区：执行成功（剪切能落地）");
        e90.relayout();
        expectTrue(e90.blocks()[0].textUtf8() == "甲丁戊己",
                   "★ 只删了选区那两段（实际 " + e90.blocks()[0].textUtf8() + "）");
        expectTrue(e90.controlViews().size() == 1 && e90.controlViews()[0].content == "丁",
                   "★ 控件留着，内容只剩没选中的那一段");
        expectTrue(e90.auditCompile() == 0 && e90.auditLayout() == 0,
                   "跨控件删除之后增量与全量仍然逐字段一致");

        // 反向一半：把控件连括号一起选上再删 —— 整个控件走掉（与复制"完整包住才复制"对称）。
        FlowPos lo90 = 0;
        FlowPos hi90 = 0;
        expectTrue(e90.controlContentSpan(0, &lo90, &hi90), "控件内容区间量得到");
        expectTrue(e90.setSelection(Range{lo90 - 1, hi90 + 1}), "连两枚括号一起选上");
        expectTrue(e90.deleteSelection(), "整块删");
        e90.relayout();
        expectTrue(e90.controlViews().empty(), "★ 完整选中的控件整块删掉了");
        expectTrue(e90.blocks()[0].textUtf8() == "甲戊己",
                   "★ 删完只剩控件外的字（实际 " + e90.blocks()[0].textUtf8() + "）");

        // 嵌套：外层只被盖住一部分、内层整个落在选区里 —— 内层照旧整块进片段，
        //   跳掉的只有被切开的那一层（"把控件全踢出去"就错在这里）。
        {
            ControlSpec outer_spec;
            outer_spec.key = "outer";
            outer_spec.controlType = "text";
            ControlSpec inner_spec;
            inner_spec.key = "inner";
            inner_spec.controlType = "text";
            const doc::Ref inner = doc::makeNode(
                6, NodeKind::ContentControl, inner_spec,
                {doc::makeNode(7, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("丁戊"), CharFormat{}, {}}, {})});
            const doc::Ref outer = doc::makeNode(
                4, NodeKind::ContentControl, outer_spec,
                {doc::makeNode(5, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("丙"), CharFormat{}, {}}, {}),
                 inner,
                 doc::makeNode(8, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("己"), CharFormat{}, {}}, {})});
            const doc::Ref sec_n = doc::makeNode(
                2, NodeKind::Section, SectionSpec{},
                {doc::makeNode(3, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("甲乙"), CharFormat{}, {}}, {}),
                 outer,
                 doc::makeNode(9, NodeKind::TextRun,
                               doc::RunPayload{utf8ToCodepoints("庚辛"), CharFormat{}, {}}, {}),
                 doc::makeNode(10, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Doc tn = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {sec_n}), 11);

            EditorEngine e90b;
            expectTrue(e90b.init(fonts90), "引擎初始化（嵌套控件）");
            e90b.setConfig(c90);
            e90b.setDocument(tn, StyleSheet{});
            e90b.relayout();
            // 从外层的第一段文字到外层的最后一个字：外层被切开，内层整个在内。
            expectTrue(e90b.setSelection(Range{e90b.flowPosAt(0, 2), e90b.flowPosAt(0, 6)}),
                       "选中外层内容（内层完整在内）");
            const ClipCopy nest = e90b.copySelection();
            expectTrue(nest.ok, "嵌套控件也能复制");
            size_t hits = 0;
            for (size_t at = nest.fragment.find("ContentControl"); at != std::string::npos;
                 at = nest.fragment.find("ContentControl", at + 1)) {
                ++hits;
            }
            expectTrue(nest.text == "丙丁戊己", "纯文本是选区里那几段（实际 " + nest.text + "）");
            expectTrue(hits == 1, "★ 被完整包住的内层控件进了片段，被切开的外层没有（实际 " +
                                      std::to_string(hits) + " 个）");
        }
    }

    if (runSection(91)) std::printf("\n[91] 连页增量：中段插一个空段，整条后缀跟着走\n");
    if (runSection(91)) {
        // 连页的块轴是整篇累加的绝对坐标，页号与页内坐标都由它现算；分页的块轴是页内
        //   坐标，放不下就翻页并把块轴挪回页顶。"后缀整体平移"那一趟只认分页那一套时，
        //   连页文档从第二页起的后缀每一行都判成放不下、整条被拽回条带顶部：中间空出
        //   一块、下面的接不上；后缀里带表时表从错误的块轴位置续排，整篇塌回第一页。
        //
        //   这里用"增量结果 vs 整篇重排"对拍（auditLayout）：那是唯一抓得住静默错位的
        //   判据 —— 塌下去的排版自己看着是自洽的，只有与全量比才露馅。
        EngineConfig c91;
        c91.zoom = 1.0f;
        c91.page.width_pt = 400.0f;
        c91.page.height_pt = 200.0f;
        c91.page.margins = PageMargins::all(20.0f);
        c91.continuous = true;
        c91.page_gap_pt = 20.0f;
        const float slab91 = c91.page.height_pt + c91.page_gap_pt;
        const std::vector<std::string> fonts91 = {cjkPath, mnPath, latinPath};

        std::vector<std::u32string> paras91;
        paras91.reserve(40);
        for (int i = 0; i < 40; ++i) {
            const std::vector<uint32_t> cps = utf8ToCodepoints("第" + std::to_string(i) + "行");
            paras91.emplace_back(cps.begin(), cps.end());
        }
        const doc::Doc t91 = doc::makeTextDoc(paras91);
        EditorEngine e91;
        expectTrue(e91.init(fonts91), "引擎初始化（连页增量）");
        e91.setConfig(c91);
        e91.setDocument(t91, StyleSheet{});
        e91.relayout();

        // 一段首行的全局块轴位置 = 页号 x 条带高 + 页内 y。
        auto axis91 = [&](int para) -> float {
            const LayoutResult& lay = e91.layout();
            if (para < 0 || para >= static_cast<int>(lay.para_index.size())) return -1.0f;
            const ParaIndex& pi = lay.para_index[static_cast<size_t>(para)];
            if (pi.line_begin < 0 || pi.line_begin >= pi.line_end) return -1.0f;
            const LineBox& lb = lay.lines[static_cast<size_t>(pi.line_begin)];
            return static_cast<float>(lb.page) * slab91 + lb.block_lo_pt;
        };
        const int pages91 = e91.layout().stats.page_count;
        expectTrue(pages91 >= 3,
                   "前提：这份文档跨了好几页（实际 " + std::to_string(pages91) + " 页）");
        const float line91 = axis91(21) - axis91(20);
        const float tail0 = axis91(21);
        expectTrue(line91 > 1.0f, "前提：段间距量得出来（实际 " + std::to_string(line91) + " 磅）");

        // 在第 20 段段首回车：多出一个空段，后面每一段整体下移一行。
        e91.setCaret(20, 0);
        expectTrue(e91.splitParagraph(), "中段回车");
        e91.relayout();
        expectTrue(e91.auditLayout() == 0, "★ 连页增量 == 整篇重排（逐字段）");
        const int pages91b = e91.layout().stats.page_count;
        expectTrue(pages91b >= pages91, "★ 页数没有塌回去（原 " + std::to_string(pages91) +
                                            " 页，现 " + std::to_string(pages91b) + " 页）");
        const float tail1 = axis91(22);
        expectTrue(std::abs((tail1 - tail0) - line91) < 0.51f,
                   "★ 后缀只让出一行（实际让出 " + std::to_string(tail1 - tail0) + " 磅，行距 " +
                       std::to_string(line91) + " 磅）—— 让出一条带就是块轴被抹回条带顶");
        // 一路走到底都不许回头：块轴必须严格递增。
        bool rising = true;
        for (int p = 1; p < static_cast<int>(e91.layout().para_index.size()); ++p) {
            const float a = axis91(p - 1);
            const float b = axis91(p);
            if (a < 0.0f || b < 0.0f) continue;
            if (!(b > a - 0.01f)) rising = false;
        }
        expectTrue(rising, "★ 整篇块轴一路递增（没有哪一段被拽到前面去）");

        // 后缀里有表：表跟着整体下移一行，增量与全量仍然逐字段一致。
        {
            EditorEngine e92;
            expectTrue(e92.init(fonts91), "引擎初始化（连页+表）");
            e92.setConfig(c91);
            int id = 3;
            auto run = [&](const std::string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{utf8ToCodepoints(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            std::vector<doc::Ref> kids92;
            for (int i = 0; i < 12; ++i) {
                kids92.push_back(run("甲" + std::to_string(i)));
                kids92.push_back(mark());
            }
            std::vector<doc::Ref> rows92;
            for (int r = 0; r < 2; ++r) {
                std::vector<doc::Ref> cells;
                for (int c = 0; c < 2; ++c) {
                    cells.push_back(
                        doc::makeNode(id++, NodeKind::TableCell, CellSpec{},
                                      {run("格" + std::to_string(r) + std::to_string(c)), mark()}));
                }
                rows92.push_back(doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, cells));
            }
            kids92.push_back(doc::makeNode(id++, NodeKind::Table, TableSpec{}, rows92));
            for (int i = 0; i < 12; ++i) {
                kids92.push_back(run("乙" + std::to_string(i)));
                kids92.push_back(mark());
            }
            const doc::Ref sec92 =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, std::move(kids92));
            const NodeId root92 = id++;
            const doc::Doc t92 =
                doc::adopt(doc::makeNode(root92, NodeKind::Document, {}, {sec92}), id);
            e92.setDocument(t92, StyleSheet{});
            e92.relayout();
            const auto tableTop = [&e92, slab91]() -> float {
                if (e92.layout().placed_tables.empty()) return -1.0f;
                const PlacedTable& pt = e92.layout().placed_tables.front();
                if (pt.placed.empty()) return -1.0f;
                return static_cast<float>(pt.placed.front().page) * slab91 + pt.placed.front().y;
            };
            const int pages_a = e92.layout().stats.page_count;
            const float top_a = tableTop();
            e92.setCaret(5, 0);
            expectTrue(e92.splitParagraph(), "表之前回车");
            e92.relayout();
            expectTrue(e92.auditLayout() == 0, "★ 后缀带表：连页增量 == 整篇重排");
            expectTrue(e92.layout().stats.page_count >= pages_a,
                       "★ 后缀带表：页数没有塌回一页（原 " + std::to_string(pages_a) + " 页，现 " +
                           std::to_string(e92.layout().stats.page_count) + " 页）");
            const float top_b = tableTop();
            expectTrue(top_b > top_a - 0.01f && top_b - top_a < 60.0f,
                       "★ 表跟着下移一行（实际下移 " + std::to_string(top_b - top_a) +
                           " 磅）—— 塌回条带顶时这个差是一整条带");
        }
    }

    if (runSection(92)) std::printf("\n[92] 行盒几何：行槽与字面盒分开、倍数可压行、段前距相加\n");
    if (runSection(92)) {
        // 一条行盒要同时回答三个问题，三者的边界必须清楚：
        //   字面盒（block_hi - block_lo）管字形与命中，行槽（advance_pt）管光标与反白，
        //   段前距与段后距各算各的、不折叠。混成一个数，表现出来就是
        //   "1.5 倍行距下反白一条一条断开、光标比行短一截"，或者"两段的 6 磅只让出 6 磅"。
        EngineConfig c92;
        c92.zoom = 1.0f;
        c92.page.width_pt = 400.0f;
        c92.page.height_pt = 400.0f;
        c92.page.margins = PageMargins::all(20.0f);
        const std::vector<std::string> fonts92 = {cjkPath, mnPath, latinPath};

        // 第一段写成两行：反白是否首尾相接要在同一段内量 —— 段与段之间本来就该有段后距。
        std::string longText;
        for (int i = 0; i < 40; ++i) longText += "甲";
        std::vector<std::u32string> paras92;
        for (const std::string& text : {longText, std::string("乙"), std::string("丙")}) {
            const std::vector<uint32_t> cps = utf8ToCodepoints(text);
            paras92.emplace_back(cps.begin(), cps.end());
        }
        const doc::Doc t92 = doc::makeTextDoc(paras92);
        EditorEngine e;
        expectTrue(e.init(fonts92), "引擎初始化（行盒几何）");
        e.setConfig(c92);
        e.setDocument(t92, StyleSheet{});
        e.relayout();

        const LayoutResult& lay = e.layout();
        expectTrue(lay.lines.size() == 4, "前提：三段共四行（第一段折成两行，实际 " +
                                              std::to_string(lay.lines.size()) + " 行）");
        if (lay.lines.size() < 4) return 1;
        auto ink = [&](size_t i) { return lay.lines[i].block_hi_pt - lay.lines[i].block_lo_pt; };
        auto slot = [&](size_t i) { return lay.lines[i].advance_pt; };
        const float adv0 = slot(0);
        const float ink0 = ink(0);
        expectTrue(adv0 > 1.0f, "前提：单倍行高量得出来（实际 " + std::to_string(adv0) + " 磅）");
        // 自带三款字体的 lineGap 都是 0，所以单倍行高恰好等于 ascent + descent：
        //   行槽与字面盒在这一档重合，这是"多出来的行距全在行底"的必然结果。
        expectNear(adv0, ink0, 0.01, "单倍行高等于字面盒（这些字体没有行间隙）");
        expectNear(adv0, lay.lines[0].ascent_pt + lay.lines[0].descent_pt, 0.01,
                   "反向：单倍行高就是 ascent + descent，没有多加一项");

        Rect cr{};
        expectTrue(e.caretRectPt(&cr), "光标矩形可查");
        expectNear(cr.h, adv0, 0.01, "单倍行距下光标高度等于行槽");

        // 1.5 倍：行槽跟着长，字面盒一动不动，光标跟的是行槽。
        ParaFormatPatch p15;
        p15.line_factor = 1.5f;
        e.setCaret(0, 0);
        expectTrue(e.applyParaFormatAndRender(p15), "第一段设 1.5 倍行距");
        expectNear(slot(0), adv0 * 1.5f, 0.01, "1.5 倍：行槽 = 单倍 x 1.5");
        expectNear(ink(0), ink0, 0.01, "反向：1.5 倍下字面盒纹丝不动（基线没被拽走）");
        expectTrue(slot(0) > ink(0) + 1.0f,
                   "行槽比字面盒高出一条行距（实际 " + std::to_string(slot(0) - ink(0)) + " 磅）");
        expectTrue(e.caretRectPt(&cr), "1.5 倍下光标矩形可查");
        expectNear(cr.h, slot(0), 0.01, "光标高度跟行槽走，不是字面盒");

        // 反白同理：一行一条矩形，纵向取行槽，于是相邻两行首尾相接、中间不留缝。
        e.selectAll();
        const std::vector<EditorEngine::PageRect> rects = e.selectionRects();
        expectTrue(rects.size() >= 3,
                   "全选之后至少三条反白（实际 " + std::to_string(rects.size()) + " 条）");
        if (rects.size() >= 3) {
            expectNear(rects[0].r_pt.h, slot(0), 0.01, "反白高度等于行槽");
            expectNear(rects[1].r_pt.y, rects[0].r_pt.y + rects[0].r_pt.h, 0.01,
                       "1.5 倍行距下两条反白首尾相接（中间不留缝）");
        }
        e.setCaret(0, 0);

        // 0.8 倍：真的压行。行高以单倍为基准乘倍数，倍数小于 1 时行槽跟着变矮。
        ParaFormatPatch p08;
        p08.line_factor = 0.8f;
        expectTrue(e.applyParaFormatAndRender(p08), "改成 0.8 倍");
        expectNear(slot(0), adv0 * 0.8f, 0.01, "0.8 倍：行槽真的压到单倍的八成");
        expectTrue(slot(0) < adv0 - 1.0f, "反向：0.8 倍确实比单倍矮");
        expectNear(ink(0), ink0, 0.01, "反向：压行只动行槽，字面盒不缩");
        expectTrue(lay.lines.size() == 4, "压行不改断行（仍是四行）");

        // 倍数为 0 时按下限 0.1 算：行高不能是 0，否则一页上所有行叠在一处。
        ParaFormatPatch p0;
        p0.line_factor = 0.0f;
        expectTrue(e.applyParaFormatAndRender(p0), "倍数设成 0");
        expectTrue(slot(0) > 0.5f,
                   "倍数 0 被下限顶住（实际行高 " + std::to_string(slot(0)) + " 磅）");
        expectNear(slot(0), adv0 * 0.1f, 0.01, "下限就是 0.1 倍");

        // 段前距：两段的空白相加，不折叠（CSS 的 margin 会折叠成 6，那是唯一不能照抄的地方）。
        ParaFormatPatch p1;
        p1.line_factor = 1.0f;
        expectTrue(e.applyParaFormatAndRender(p1), "第一段恢复单倍");
        auto lineTop = [&](int para) -> float {
            const ParaIndex& pi = lay.para_index[static_cast<size_t>(para)];
            return lay.lines[static_cast<size_t>(pi.line_begin)].block_lo_pt;
        };
        auto paraGap = [&](int upper) -> float {
            const ParaIndex& pi = lay.para_index[static_cast<size_t>(upper - 1)];
            const LineBox& prev = lay.lines[static_cast<size_t>(pi.line_end - 1)];
            return lineTop(upper) - (prev.block_lo_pt + prev.advance_pt);
        };
        expectNear(paraGap(1), 6.0, 0.01, "前提：段间只有段后距（6 磅）");

        ParaFormatPatch pbefore;
        pbefore.space_before = Length::abs(ptToEmu(10.0));
        e.setCaret(1, 0);
        expectTrue(e.applyParaFormatAndRender(pbefore), "第二段设段前距 10 磅");
        expectNear(paraGap(1), 16.0, 0.01, "段前距与上一段的段后距相加（6 + 10 = 16）");
        {
            const ParaIndex& pi0 = lay.para_index[0];
            const LineBox& last0 = lay.lines[static_cast<size_t>(pi0.line_end - 1)];
            expectNear(lineTop(1), last0.block_lo_pt + last0.advance_pt + 16.0, 0.01,
                       "第二段首行的行顶落在算出来的那一处");
        }
        // 增量与全量必须同一条规则：后缀平移按 advance 逐行累加，段前距只在全量里加的话，
        //   两边的块轴就差这一截，auditLayout 当场红。
        e.setCaret(0, 0);
        expectTrue(e.typeTextAndRender("丁"), "在第一段敲一个字（走增量续排）");
        expectTrue(e.auditLayout() == 0, "带段前距的增量续排等于整篇重排");

        ParaFormatPatch p_clear;
        p_clear.space_before = Length::abs(ptToEmu(0.0));
        e.setCaret(1, 0);
        expectTrue(e.applyParaFormatAndRender(p_clear), "清掉段前距");
        expectNear(paraGap(1), 6.0, 0.01, "反向：清掉之后段间回到 6 磅");

        // 页顶不加段前距：分页之后段首那一截属于页边距。
        {
            EditorEngine e2;
            expectTrue(e2.init(fonts92), "引擎初始化（页顶段前距）");
            EngineConfig c2 = c92;
            c2.page.height_pt = 200.0f;
            e2.setConfig(c2);
            e2.setDocument(doc::makeTextDoc({U"前", U"后"}), StyleSheet{});
            e2.relayout();
            ParaFormatPatch pbreak;
            pbreak.space_before = Length::abs(ptToEmu(10.0));
            pbreak.page_break_before = true;
            e2.setCaret(1, 0);
            expectTrue(e2.applyParaFormatAndRender(pbreak), "第二段：段前分页 + 段前距 10 磅");
            const LayoutResult& lay2 = e2.layout();
            const ParaIndex& pi2 = lay2.para_index[1];
            const LineBox& lb2 = lay2.lines[static_cast<size_t>(pi2.line_begin)];
            expectTrue(lb2.page == 1,
                       "第二段落在第二页（实际第 " + std::to_string(lb2.page) + " 页）");
            expectNear(lb2.block_lo_pt, c2.page.margins.top, 0.01,
                       "页顶不加段前距（行顶就是版心顶，不多 10 磅）");
            expectTrue(e2.auditLayout() == 0, "页顶那条规则在增量与全量之间一致");
        }

        // 文档第一段照样让出段前距：页首那一段与"分页之后的段首"不是一回事。
        {
            EditorEngine e3;
            expectTrue(e3.init(fonts92), "引擎初始化（首段段前距）");
            e3.setConfig(c92);
            e3.setDocument(t92, StyleSheet{});
            e3.relayout();
            ParaFormatPatch pfirst;
            pfirst.space_before = Length::abs(ptToEmu(12.0));
            e3.setCaret(0, 0);
            expectTrue(e3.applyParaFormatAndRender(pfirst), "第一段设段前距 12 磅");
            const LineBox& lb3 = e3.layout().lines.front();
            expectNear(lb3.block_lo_pt, c92.page.margins.top + 12.0, 0.01,
                       "文档首段照样让出段前距（与页顶那条规则分开）");
        }

        // 多倍行距多出来的空白上下对半分：字落在行槽中间，反白与插入点才包得住它。
        //   固定值 / 最小值是"行盒高度"，字从盒顶排起 —— 两档不能共用一个式子。
        {
            EditorEngine e6;
            expectTrue(e6.init(fonts92), "引擎初始化（字在行槽中间）");
            e6.setConfig(c92);
            e6.setDocument(t92, StyleSheet{});
            e6.relayout();
            expectNear(e6.layout().lines.front().slotLoPt(), e6.layout().lines.front().block_lo_pt,
                       0.01, "前提：单倍行距下行槽顶就是字面盒顶（没有多余空白）");

            ParaFormatPatch p15b;
            p15b.line_factor = 1.5f;
            e6.setCaret(0, 0);
            expectTrue(e6.applyParaFormatAndRender(p15b), "第一段设 1.5 倍");
            const LineBox& mid = e6.layout().lines.front();
            const float ink6 = mid.block_hi_pt - mid.block_lo_pt;
            const float slot6 = mid.slotSpanPt();
            expectNear(mid.lead_top_pt, (slot6 - ink6) * 0.5f, 0.01,
                       "★ 行距多出来的空白上下各一半");
            expectNear(mid.block_lo_pt - mid.slotLoPt(), mid.lead_top_pt, 0.01,
                       "★ 字面盒顶比行槽顶低半条行距（字在反白中间）");
            Rect cr6{};
            expectTrue(e6.caretRectPt(&cr6), "1.5 倍下光标矩形可查");
            expectNear(cr6.y, mid.slotLoPt(), 0.01, "★ 光标从行槽顶起");
            expectNear(cr6.y + cr6.h, mid.slotLoPt() + slot6, 0.01, "★ 光标到行槽底止");

            // 反向：固定值不从中间分。字从盒顶排起，多出来的空白全在下方 —— 与 Word 的
            //   "固定值"同一档，也是"字比盒高时要裁底"说得通的那个位置。
            ParaFormatPatch pex;
            pex.line_rule = LineSpacingRule::Exact;
            pex.line_exact = Length::abs(ptToEmu(30.0));
            e6.setCaret(0, 0);
            expectTrue(e6.applyParaFormatAndRender(pex), "改成固定值 30 磅");
            const LineBox& fx = e6.layout().lines.front();
            expectNear(fx.lead_top_pt, 0.0, 0.01, "★ 固定值不从中间分（字贴盒顶）");
            expectNear(fx.slotLoPt(), fx.block_lo_pt, 0.01, "固定值的字面盒顶就是行槽顶");
        }

        // 选区从软换行处起（或到某行行首止）时，本行一个字都没选中：上一行末尾不能凭空
        //   多出一个小方块。只有本行的段落标记真的被选中，零宽才要补一条窄条。
        {
            EditorEngine e7;
            expectTrue(e7.init(fonts92), "引擎初始化（从软换行处起选）");
            e7.setConfig(c92);
            e7.setDocument(t92, StyleSheet{});
            e7.relayout();
            const ParaIndex pi7 = e7.layout().para_index[0];
            expectTrue(pi7.line_end - pi7.line_begin >= 2, "前提：第一段折成两行");
            if (pi7.line_end - pi7.line_begin >= 2) {
                const uint32_t wrap =
                    e7.layout().lines[static_cast<size_t>(pi7.line_begin)].last_cp;
                const FlowPos at_wrap = e7.flowPosAtSide(0, static_cast<int>(wrap), 0);
                const FlowPos two_later = e7.flowPosAtSide(0, static_cast<int>(wrap) + 2, 0);
                e7.setSelection(Range{at_wrap, two_later});
                const std::vector<EditorEngine::PageRect> r7 = e7.selectionRects();
                expectTrue(r7.size() == 1,
                           "★ 从软换行处起选：上一行末尾不多出一个小方块（反白条数 " +
                               std::to_string(r7.size()) + "）");
                if (r7.size() == 1) {
                    const LineBox& l2 = e7.layout().lines[static_cast<size_t>(pi7.line_begin + 1)];
                    expectNear(r7[0].r_pt.y, l2.slotLoPt(), 0.01,
                               "那一条反白落在下一行、从行槽顶起");
                }
                // 反向：从上一行中间起选，两条反白都在（尺子没有把整行吞掉）。
                const FlowPos earlier = e7.flowPosAtSide(0, static_cast<int>(wrap) - 2, 0);
                e7.setSelection(Range{earlier, two_later});
                expectTrue(e7.selectionRects().size() == 2, "反向：真跨过换行处选，两条反白都在");
            }
        }
    }

    if (runSection(93)) {
        // 行距三态：auto 按倍数；exact 就是那个磅值（比墨迹矮时按行裁字）；atLeast 取固定值
        //   与内容高的较大者。三档共用同一个"内容高"基准，差别只在行高怎么定 —— 所以
        //   这里同时钉三件事：行高、字面盒不动（exact 只改推进不改基线以上）、裁不裁。
        EngineConfig c93;
        c93.zoom = 1.0f;
        c93.page.width_pt = 400.0f;
        c93.page.height_pt = 400.0f;
        c93.page.margins = PageMargins::all(20.0f);
        std::string long93;
        for (int i = 0; i < 40; ++i) long93 += "甲";
        std::vector<std::u32string> paras93;
        for (const std::string& text : {std::string("甲"), long93, std::string("乙")}) {
            const std::vector<uint32_t> cps = utf8ToCodepoints(text);
            paras93.emplace_back(cps.begin(), cps.end());
        }
        const doc::Doc t93 = doc::makeTextDoc(paras93);
        EditorEngine e;
        expectTrue(e.init({cjkPath, mnPath, latinPath}), "引擎初始化（行距三态）");
        e.setConfig(c93);
        e.setDocument(t93, StyleSheet{});
        e.relayout();
        const LayoutResult& lay = e.layout();
        expectTrue(lay.para_index.size() >= 3, "前提：文档有三段");
        if (lay.para_index.size() < 3) return 1;
        expectTrue(lay.para_index[1].line_end - lay.para_index[1].line_begin >= 2,
                   "前提：第二段折成两行");
        if (lay.para_index[1].line_end - lay.para_index[1].line_begin < 2) return 1;
        const float single = lay.lines.front().advance_pt;
        const float ink = lay.lines.front().block_hi_pt - lay.lines.front().block_lo_pt;
        expectNear(single, ink, 0.01, "前提：单倍行距下行槽等于字面盒（这些字体没有行间隙）");

        auto setSpacing = [&](LineSpacingRule rule, float exact) {
            ParaFormatPatch p;
            p.line_rule = rule;
            p.line_exact = Length::abs(ptToEmu(exact));
            e.setCaret(1, 0);
            return e.applyParaFormatAndRender(p);
        };
        auto lineOf = [&](int para, int nth) -> const LineBox& {
            const ParaIndex& pi = lay.para_index[static_cast<size_t>(para)];
            return lay.lines[static_cast<size_t>(pi.line_begin + nth)];
        };

        expectTrue(setSpacing(LineSpacingRule::Exact, 20.0f), "第二段设固定值 20 磅");
        expectNear(lineOf(1, 0).advance_pt, 20.0, 0.01, "★ 固定值 20：行高就是 20");
        expectTrue(!lineOf(1, 0).clip_text, " 20 磅装得下墨迹，不进裁剪");
        expectNear(lineOf(1, 0).block_hi_pt - lineOf(1, 0).block_lo_pt, ink, 0.01,
                   "反向：固定值只改行高，字面盒不动");
        expectNear(lineOf(1, 1).block_lo_pt - lineOf(1, 0).block_lo_pt, 20.0, 0.01,
                   "同一段第二行的行顶正好推进 20 磅");

        expectTrue(setSpacing(LineSpacingRule::Exact, 8.0f), "改成固定值 8 磅");
        expectNear(lineOf(1, 0).advance_pt, 8.0, 0.01, "★ 固定值 8：行高就是 8（不夹到单倍）");
        expectTrue(lineOf(1, 0).clip_text, "★ 8 磅比墨迹矮，这一行要裁");
        expectTrue(!lineOf(0, 0).clip_text, "反向：别的段不受影响，仍不裁");
        expectNear(lineOf(1, 1).block_lo_pt - lineOf(1, 0).block_lo_pt, 8.0, 0.01,
                   "要裁的时候行距照样是 8 磅（裁的是绘制，不是排版）");

        expectTrue(setSpacing(LineSpacingRule::AtLeast, 30.0f), "改成最小值 30 磅");
        expectNear(lineOf(1, 0).advance_pt, 30.0, 0.01, "★ 最小值 30：内容更矮，按 30 排");
        expectTrue(!lineOf(1, 0).clip_text, " 最小值那一档永远装得下，不裁");
        expectTrue(setSpacing(LineSpacingRule::AtLeast, 5.0f), "改成最小值 5 磅");
        expectNear(lineOf(1, 0).advance_pt, single, 0.01,
                   "★ 最小值 5：内容更高，退回单倍（不是 5）");
        expectNear(lineOf(1, 0).block_hi_pt - lineOf(1, 0).block_lo_pt, ink, 0.01,
                   " 最小值档的字面盒同样不动");

        expectTrue(setSpacing(LineSpacingRule::Exact, 0.0f), "固定值写成 0");
        expectNear(lineOf(1, 0).advance_pt, single, 0.01,
                   "★ 固定值 0 视为没设，退回 auto（不排零高的行）");

        // 裁字：固定值比字矮时墨迹要停在行槽底，别的档不许裁 —— 而且改完行距画面必须跟着
        //   重画。后半句不是多余的一条：页位图的新鲜度只比指纹，而"字形坐标一个都没动、
        //   只有裁切变了"是常态（单行段落换行距规则时基线不动），行指纹里必须带着裁切与
        //   行高，否则页被判成还新鲜，命令返回 true、读回也对，屏幕上仍是旧帧。
        //   段落标记要关掉：它由另一条绘制路径画，本来就不跟着字形一起裁。
        {
            EngineConfig c4 = c93;
            c4.zoom = 2.0f;
            c4.marks = MarksMode::None;
            EditorEngine e4;
            expectTrue(e4.init({cjkPath, mnPath, latinPath}), "引擎初始化（裁字）");
            e4.setConfig(c4);
            e4.setDocument(doc::makeTextDoc({U"甲乙丙丁"}), StyleSheet{});
            e4.relayout();
            auto setSpacing4 = [&](LineSpacingRule rule, float exact) {
                ParaFormatPatch p;
                p.line_rule = rule;
                p.line_exact = Length::abs(ptToEmu(exact));
                e4.setCaret(0, 0);
                return e4.applyParaFormatAndRender(p);
            };
            // 量"行槽顶往下多深还有墨"（磅）：固定值那一档的行槽底就是裁边，
            //   装得下时墨迹会落到基线下面。
            auto inkDepth4 = [&]() -> float {
                e4.repaint();
                const int fw = e4.frameWidth();
                const int fh = e4.frameHeight();
                const uint8_t* px = e4.frame();
                const LineBox& lb = e4.layout().lines.front();
                const int top = static_cast<int>(std::floor(lb.slotLoPt() * c4.zoom));
                const int x0 = static_cast<int>(std::floor(lb.inline_origin_pt * c4.zoom));
                const int x1 =
                    static_cast<int>(std::ceil((lb.inline_origin_pt + lb.inline_end_pt) * c4.zoom));
                int last = top;
                for (int y = std::max(0, top); y < std::min(fh, top + 80); ++y) {
                    int n = 0;
                    for (int x = std::max(0, x0); x < std::min(fw, x1); ++x) {
                        const size_t i = (static_cast<size_t>(y) * fw + x) * 4u;
                        if (px[i] < 128 && px[i + 1] < 128 && px[i + 2] < 128) ++n;
                    }
                    if (n >= 3) last = y;
                }
                return (last - top) / c4.zoom;
            };
            const float fullDepth = inkDepth4();
            expectTrue(fullDepth > 12.0f, "★ 单倍行距下墨迹落到基线下面（实际深 " +
                                              std::to_string(fullDepth) + " 磅）");
            expectTrue(setSpacing4(LineSpacingRule::Exact, 8.0f), "改固定值 8 磅");
            const float cutDepth = inkDepth4();
            expectTrue(cutDepth <= 8.5f, "★ 固定值 8 磅：墨迹在 8 磅处停住（实际深 " +
                                             std::to_string(cutDepth) + " 磅）");
            // 改回多倍：同一台引擎、同一份文档，字形坐标与改之前完全一样 —— 差别只有
            //   "裁不裁"。这一条量的就是页位图有没有跟着作废。
            expectTrue(setSpacing4(LineSpacingRule::Auto, 1.0f), "再改回单倍");
            const float backDepth = inkDepth4();
            expectTrue(backDepth > 12.0f,
                       "★ 改回单倍之后墨迹又落到基线下面（画面跟着重画了，实际深 " +
                           std::to_string(backDepth) + " 磅）");
            expectTrue(setSpacing4(LineSpacingRule::AtLeast, 30.0f), "再改最小值 30 磅");
            const float leastDepth = inkDepth4();
            expectTrue(leastDepth > 12.0f,
                       " 最小值档装得下，同样不裁（实际深 " + std::to_string(leastDepth) + " 磅）");
            // 4 磅与 8 磅都是"裁"，字形同样一个都不动，只有裁边的高度变了。
            expectTrue(setSpacing4(LineSpacingRule::Exact, 4.0f), "改固定值 4 磅");
            const float thinDepth = inkDepth4();
            expectTrue(thinDepth <= 4.5f, "★ 固定值 4 磅：裁边跟着行高走，不是一直按 8（实际深 " +
                                              std::to_string(thinDepth) + " 磅）");
        }

        // 增量与全量必须同一条规则：exact 只改每行的推进量，后缀平移按它累加。
        expectTrue(setSpacing(LineSpacingRule::Exact, 12.0f), "再设固定值 12 磅");
        e.setCaret(2, 0);
        expectTrue(e.typeTextAndRender("丙"), "末段敲一个字（走增量续排）");
        expectTrue(e.auditLayout() == 0, "固定值行距下的增量续排等于整篇重排");
    }

    checkSectionTable();
    printSectionTimes();

    std::printf("\n──────────────────────────────\n");
    std::printf("共 %d 项断言，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
