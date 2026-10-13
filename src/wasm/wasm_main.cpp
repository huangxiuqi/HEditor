/**
 * WASM 宿主。
 * 只做三件事：接收 JS fetch 下来的字体字节（字体不进 wasm 包）、暴露布局与绘制
 * 入口、把帧缓冲以"整块 typed_memory_view"交出去 —— 跨边界传的是位图，
 * 不是绘制指令。
 * 关于文本字面量：直接用 UTF-8 中文，不用 \xNN 十六进制转义。十六进制转义有个
 * 陷阱："\xE6\xB8\xA9"、"36.8" 单独看都没问题，但 "\xE6\xB8\xA936.8" 会被编译器
 * 贪婪解析成 \xA93 加 "6.8"（hex escape sequence out of range）。凡是 \xNN 后面
 * 紧跟数字或字母的都得拆字符串，不如直接写中文。
 */
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "demo/demo_documents.h"

#ifndef HEDITOR_BUILD_STAMP
#define HEDITOR_BUILD_STAMP "dev"
#endif

using namespace heditor;
using emscripten::val;

namespace {

EditorEngine g_engine;
EngineConfig g_cfg;
int g_cjk_font = 0;
int g_mn_font = 1;
int g_latin_font = -1;  // 回退字体；未提供时为 -1（关闭回退）

void applyConfig() { g_engine.setConfig(g_cfg); }

WritingMode writingModeFromCode(int code) {
    switch (code) {
        case 1:
            return WritingMode::VerticalLR();
        case 2:
            return WritingMode::VerticalRL();
        default:
            return WritingMode::HorizontalTB();
    }
}

/**
 * 字体传入：JS 侧 fetch 到字节后在这里申请堆内存，再把指针交给引擎。
 */
uintptr_t allocBuffer(size_t n) { return reinterpret_cast<uintptr_t>(std::malloc(n)); }
void freeBuffer(uintptr_t p) { std::free(reinterpret_cast<void*>(p)); }

int loadFont(const std::string& name, uintptr_t ptr, size_t len) {
    return g_engine.fonts().addFaceFromMemory(name, reinterpret_cast<const uint8_t*>(ptr), len);
}

void setFontRoles(int cjk_index, int mongol_index, int latin_index) {
    g_cjk_font = cjk_index;
    g_mn_font = mongol_index;
    g_latin_font = latin_index;
}

/**
 * 文本素材：样例里的蒙文段落与中文文书。
 */
std::string mongolianParagraph(int repeats) {
    // ᠮᠣᠩᠭᠣᠯ ᠪᠢᠴᠢᠭ（"蒙古文"）重复若干次，用于撑满一列看列序
    static const char kWord[] =
        "\xE1\xA0\xAE\xE1\xA0\xA3\xE1\xA0\xA9\xE1\xA0\xAD\xE1\xA0\xA3\xE1\xA0\xAF"
        " "
        "\xE1\xA0\xAA\xE1\xA0\xA2\xE1\xA0\xB4\xE1\xA0\xA2\xE1\xA0\xAD";
    std::string s;
    for (int i = 0; i < repeats; ++i) {
        if (i) s += " ";
        s += kWord;
    }
    return s;
}

std::string chineseNote() {
    return "患者主诉：反复心悸3天，加重1天。现病史：患者3天前无明显诱因出现心悸，"
           "伴胸闷、气短，活动后加重，休息后稍缓解。体格检查：体温36.8℃，脉搏92次/分，"
           "呼吸20次/分，血压138/86mmHg。初步诊断：心律失常、高血压。";
}

/**
 * 长文档样例：paragraphs 段可编辑病历。
 *
 * 单段文档没有可复用的前缀，增量排版与全量重排必然相等 —— 只有多段文档才能把
 * "只重排脏段"这件事量出来（见 scripts/edit_probe.mjs 的长文档一节）。
 */
std::vector<BlockSpec> longDocument(int paragraphs, int font_index) {
    static const char* kSentences[] = {
        "患者诉晨起后心悸明显，持续约十分钟，休息后缓解，未见黑矇与晕厥，"
        "夜间睡眠尚可，饮食正常，二便如常。",
        "查体：神志清楚，双肺呼吸音清，未闻及干湿啰音，心率齐，各瓣膜听诊区未闻及杂音，"
        "双下肢无水肿。",
        "复查心电图提示窦性心律，偶发房性早搏，ST-T 改变较前无动态演变，"
        "继续当前治疗方案并观察。",
        "医嘱：低盐低脂饮食，避免情绪激动与剧烈活动，规律服药，一周后门诊随访，"
        "如有胸痛加重随时就诊。",
    };
    const int kCount = static_cast<int>(sizeof(kSentences) / sizeof(kSentences[0]));

    std::vector<BlockSpec> blocks;
    blocks.reserve(static_cast<size_t>(std::max(0, paragraphs)));
    for (int i = 0; i < paragraphs; ++i) {
        BlockSpec b;
        b.font_index = font_index;
        b.size_pt = 14.0f;
        b.wm = WritingMode::HorizontalTB();
        b.line_factor = 1.85f;
        b.space_after_pt = 6.0f;
        b.language = "zh";
        b.color = Color::rgb(24, 24, 24);
        b.setText("第 " + std::to_string(i + 1) + " 次病程记录：" + kSentences[i % kCount]);
        blocks.push_back(std::move(b));
    }
    return blocks;
}

/**
 * 当前文档的两份输入：块流（内容与块级格式）与表单字段。
 *
 * 它们是"重新建树"的全部材料 —— 换场景、换字段、增删控件都只是改这两份再重编一次，
 * 于是控件永远长在树上，不再有"区间是相对旧文本算的"那类错配。
 */
std::vector<BlockSpec> g_blocks;
std::vector<DemoFieldSpec> g_fields;

/**
 * 按场景号装配块流。只造数据、不动引擎 —— 装载统一走 rebuildDocument。
 */
std::vector<BlockSpec> buildScenarioBlocks(int scenario) {
    std::vector<BlockSpec> blocks;
    switch (scenario) {
        case 1: {  // 蒙文竖排 · 列序从左往右（侧倒，正确形态）
            BlockSpec b;
            b.font_index = g_mn_font;
            b.size_pt = 20.0f;
            b.wm = WritingMode::VerticalLR();
            b.line_factor = 1.9f;
            b.language = "mn";
            b.color = Color::rgb(20, 60, 150);
            b.setText(mongolianParagraph(18));
            blocks.push_back(std::move(b));
            break;
        }
        case 2: {  // 中文竖排 · 列序从右往左（字形正立）
            BlockSpec b;
            b.font_index = g_cjk_font;
            b.size_pt = 14.0f;
            b.wm = WritingMode::VerticalRL();
            b.line_factor = 1.7f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText(chineseNote());
            blocks.push_back(std::move(b));
            break;
        }
        case 3: {  // 中文长文本横排，看断行与分页
            BlockSpec b;
            b.font_index = g_cjk_font;
            b.size_pt = 13.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.8f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText(chineseNote() + chineseNote());
            blocks.push_back(std::move(b));
            break;
        }
        case 4: {  // 中英文混排 + 拉丁回退（验证 10⁹ 这类上标不再出豆腐块）
            blocks.push_back(mixedParagraphBlock(g_cjk_font, g_latin_font));
            break;
        }
        case 5: {  // 蒙文竖排 · 列序从右往左（对照：故意错的组合）
            BlockSpec b;
            b.font_index = g_mn_font;
            b.size_pt = 20.0f;
            b.wm = WritingMode::VerticalRL();
            b.line_factor = 1.9f;
            b.language = "mn";
            b.color = Color::rgb(150, 60, 30);
            b.setText(mongolianParagraph(18));
            blocks.push_back(std::move(b));
            break;
        }
        case 6: {  // 档 1：可编辑文档（短文本 —— 一页放得下，便于数增量）
            BlockSpec b;
            b.font_index = g_cjk_font;
            b.size_pt = 14.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.85f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText("患者主诉：反复心悸三天，加重一天。");
            blocks.push_back(std::move(b));
            break;
        }
        case 7: {  // 档 2：长文档（200 段）—— 多段才有"可复用的前缀"可谈
            blocks = longDocument(200, g_cjk_font);
            break;
        }
        default:
            blocks = sampleDocument(g_cjk_font, g_mn_font, g_latin_font);
            break;
    }

    return blocks;
}

/**
 * 按当前的块流与字段重建文档树，一次性交给引擎。
 *
 * 演示页从这里起走"建树"那条路（setDocument）：控件长在树上、编辑落在树上，与单测里
 *   那条路是同一份代码。换文档不再需要"清控件"那一步 —— 控件是树的产物，树换了它自然
 *   跟着换，不存在"区间指向旧文本"的错配。
 *
 * keep_caret 为真时把光标原样搬回去（文本没变才搬）：换控件、改只读标记、增删字段都不该
 *   动光标，而 setDocument 会把光标归零。所以"改控件"这条路上必须补这一步，否则光标会自己
 *   在原地跳回段首。
 */
void rebuildDocument(bool keep_caret) {
    const TextPos before = g_engine.caret();
    // 侧别也要一起搬回来：同一个偏移上有好几个位置（括号之间、控件内外），
    //   只搬偏移等于把光标挪到那一处的"默认"位置上 —— 用户会看到它自己跳了一格。
    const int side = keep_caret ? g_engine.caretSide() : -1;
    std::string text_before;
    if (keep_caret) {
        for (const BlockSpec& b : g_engine.blocks()) text_before += b.textUtf8();
    }
    g_engine.setDocument(demoTree(g_blocks, g_fields), StyleSheet{});
    if (!keep_caret) return;
    std::string text_after;
    for (const BlockSpec& b : g_engine.blocks()) text_after += b.textUtf8();
    if (text_before != text_after) return;  // 换了文档：光标归零才是对的
    if (before.para < 0 || before.para >= static_cast<int>(g_engine.blocks().size())) return;
    if (before.offset > g_engine.blocks()[static_cast<size_t>(before.para)].cp_text.size()) return;
    g_engine.setCaretAtSide(before.para, before.offset, side);
}

void setScenario(int scenario) {
    g_blocks = buildScenarioBlocks(scenario);
    g_fields.clear();
    rebuildDocument(false);
}

/**
 * 把当前文档从引擎取回来，作为下一次重建的底稿。
 *
 * 树上编辑之后引擎里的文本已经变了，而 g_blocks 还是装载时那一份 —— 不刷新的话，
 *   紧接着"改控件再重建"（setFormControls）会把用户改过的内容整个倒回装载时的
 *   样子：光标跟着归零，字也打不进任何字段（区间还在原处，文本却已经对不上了）。
 * 只取文本与块级字段：demoTree 不读 runs / objects，重新编译会自己再产一份。
 */
void syncBlocksFromEngine() { g_blocks = g_engine.blocks(); }

void setZoom(float z) {
    g_cfg.zoom = z;
    applyConfig();
}
void setPageMetrics(float w_pt, float h_pt, float margin_pt) {
    g_cfg.page.width_pt = w_pt;
    g_cfg.page.height_pt = h_pt;
    g_cfg.page.margin_pt = margin_pt;
    applyConfig();
}

void relayoutAndPaint() {
    g_engine.relayout();
    g_engine.repaint();
}

void layoutOnly() { g_engine.relayout(); }

/**
 * 这份 wasm 自己的构建时间（由构建脚本经 HEDITOR_BUILD_STAMP 注入）。
 * 用途单一但关键：验证台把它显示出来，"页面跑的到底哪一版"就不用猜了 ——
 *   磁盘上的文件时间只说明"编过"，说明不了"浏览器加载的是它"（缓存会让两者不一致）。
 */
std::string buildStamp() { return HEDITOR_BUILD_STAMP; }

int frameWidth() { return g_engine.frameWidth(); }
int frameHeight() { return g_engine.frameHeight(); }
size_t frameByteSize() { return g_engine.frameBytes(); }

val frameRgba() {
    return val(emscripten::typed_memory_view(g_engine.frameBytes(), g_engine.frame()));
}

/**
 * 视口锚点：帧只装 [first, first+count) 这几页。count <= 0 表示整篇。
 * 宿主按"可见页 ± 1"设它，帧高于是与文档长度无关 —— 帧高就是画布高度，
 * 而画布越大浏览器每帧要重新合成的图层越大（见 EditorEngine::setViewportAnchor）。
 */
void setViewportAnchor(int first_page, int page_count) {
    g_engine.setViewportAnchor(first_page, page_count);
}

int windowFirstPage() { return g_engine.windowFirstPage(); }
int windowPageCount() { return g_engine.windowPageCount(); }
int pagePxWidth() { return g_engine.pagePxWidth(); }
int pagePxHeight() { return g_engine.pagePxHeight(); }
int pageGapPx() { return g_engine.pageGapPx(); }

/**
 * 本次呈现写过的页（绝对页号，升序）。宿主据此决定哪几张页画布要重贴。
 */
std::string dirtyPagesJson() {
    std::ostringstream o;
    o << "[";
    const std::vector<int>& v = g_engine.dirtyPages();
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o << ",";
        o << v[i];
    }
    o << "]";
    return o.str();
}

std::string statsJson() {
    const EngineStats& s = g_engine.stats();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "{";
    o << "\"pageCount\":" << s.page_count << ",";
    o << "\"glyphCount\":" << s.glyph_count << ",";
    o << "\"placedLines\":" << s.placed_line_count << ",";
    o << "\"clusters\":" << s.cluster_count << ",";
    o << "\"faces\":" << s.face_count << ",";
    o << "\"fontInstances\":" << s.font_instance_count << ",";
    o << "\"fontRunCount\":" << s.font_run_count << ",";
    o << "\"missingGlyphCount\":" << s.missing_glyph_count << ",";
    o << "\"shapeMs\":" << s.shape_ms << ",";
    o << "\"layoutMs\":" << s.layout_ms << ",";
    o << "\"pageFpMs\":" << s.page_fp_ms << ",";
    o << "\"paintMs\":" << s.paint_ms << ",";
    // 三段的用途是判"该优化哪一段"：铺面积走 memcpy、字形走扫描线光栅、
    //   帧搬运走 copyRectFrom。合成一个 paintMs 就只能得出"绘制慢"。
    o << "\"paintFillMs\":" << s.paint_fill_ms << ",";
    o << "\"paintGlyphMs\":" << s.paint_glyph_ms << ",";
    o << "\"paintPresentMs\":" << s.paint_present_ms << ",";
    o << "\"totalMs\":" << s.total_ms << ",";
    o << "\"frameWidth\":" << s.frame_width << ",";
    o << "\"frameHeight\":" << s.frame_height << ",";
    o << "\"frameBytes\":" << g_engine.frameBytes() << ",";
    o << "\"glyphCacheHits\":" << s.glyph_cache_hits << ",";
    o << "\"glyphCacheMisses\":" << s.glyph_cache_misses << ",";
    o << "\"glyphCacheEntries\":" << s.glyph_cache_entries << ",";
    o << "\"zoom\":" << s.zoom;
    o << "}";
    return o.str();
}

/**
 * 引擎内循环基准。
 * 这个函数存在的意义就是"别让循环跨边界"：
 * 12 次重排+重绘全在 C++ 里跑完，JS 只下一次调用、收一个 JSON。
 * 同样的 12 次如果摆在 JS 里，就是 12 次跨界 + 12 次对象封送 + 12 次
 * typed_memory_view 重建 —— 那才是 WASM 真正的减速带。
 */
std::string benchRelayoutPaint(int runs) {
    if (runs < 1) runs = 1;
    if (runs > 200) runs = 200;

    // 预热一次：首轮包含冷缓存（cmap 查询、outline 解析）
    g_engine.relayout();
    g_engine.repaint();

    double shape = 0.0, layout = 0.0, paint = 0.0, total = 0.0;
    for (int i = 0; i < runs; ++i) {
        g_engine.relayout();
        g_engine.repaint();
        const EngineStats& s = g_engine.stats();
        shape += s.shape_ms;
        layout += s.layout_ms;
        paint += s.paint_ms;
        total += s.total_ms;
    }
    const double n = static_cast<double>(runs);

    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "{";
    o << "\"runs\":" << runs << ",";
    o << "\"shapeMs\":" << shape / n << ",";
    o << "\"layoutMs\":" << layout / n << ",";
    o << "\"paintMs\":" << paint / n << ",";
    o << "\"totalMs\":" << total / n;
    o << "}";
    return o.str();
}

std::string shapeReport(int font_index, float size_pt, const std::string& text, int wm_code,
                        const std::string& language) {
    return g_engine.describeShaping(font_index, size_pt, text, writingModeFromCode(wm_code),
                                    language.empty() ? nullptr : language.c_str());
}

/**
 * 档 1：编辑与光标
 * 每一次编辑都是一次跨越边界：文本进去、布尔+统计出来。
 * JS 侧不保留文档、不索引字符、不算位置 —— 它只负责把键盘事件翻译成
 * "插一串、退一格、光标右移"，其余全在 C++ 里。
 * 注意这里没有 caretRectPt / hitTestPt 的逐次查询接口：
 * 光标矩形随每次编辑一起返回（见 editStatsJson），因为宿主每次都要用它。
 * 让宿主"改一次、问一次"就是两次跨界，而这两次之间有依赖。
 */
bool typeText(const std::string& utf8) { return g_engine.typeTextAndRender(utf8); }
bool backspaceText() { return g_engine.backspaceAndRender(); }
bool deleteForwardText() { return g_engine.deleteForwardAndRender(); }
/**
 * 跨段编辑：段首退格 / 段末 Delete 已经自动走合并（见 engine.cpp 的 backspace/deleteForward），
 * 这三个是显式入口（回车分段、以及从任一方向合并）。
 */
bool splitParagraph() { return g_engine.splitParagraphAndRender(); }
bool mergeWithPreviousParagraph() { return g_engine.mergeWithPreviousParagraphAndRender(); }
bool mergeWithNextParagraph() { return g_engine.mergeWithNextParagraphAndRender(); }
bool moveCaret(int dcp) { return g_engine.moveCaretAndRender(dcp); }
bool caretToLineStart() { return g_engine.moveCaretToLineStartAndRender(); }
bool caretToLineEnd() { return g_engine.moveCaretToLineEndAndRender(); }
void setCaret(int para, int offset) { g_engine.setCaret(para, static_cast<size_t>(offset)); }
void showCaret(bool on) { g_engine.showCaret(on); }
void present() { g_engine.repaint(); }

/**
 * 引擎实时的光标偏移（直读，不经过编辑统计）。
 * 为什么需要它：editStats 是 presentInternal 里写下来的账 ——
 * 任何"改了状态却没重绘"的入口之后，这份账都停在上一次呈现上。
 * 于是"宿主持着过期脏矩形上屏"这类 bug 有一个精确的探测器：
 * editStats.caretOffset  !=  caretOffsetLive()  ⇒  有入口没给自己收尾
 * 探针用这一条把整类问题一次性钉住（Tab 跳字段就是栽在这上面）。
 */
int caretOffsetLive() { return static_cast<int>(g_engine.caret().offset); }

/**
 * 命中测试：帧坐标（磅）→ 位置。返回 "null" 或 {"para":n,"offset":n,"field":n}。
 * 反查与正查共用同一份行盒几何，所以点选落点和画出来的光标必然一致。
 *
 * side = 落点的侧别（见引擎的 caretSide）。它是落点的完整身份：相邻两个字段共享边界时，
 * 前一个字段的末尾与后一个字段的开头是同一个偏移，同一个偏移上还可能压着好几枚括号 ——
 * 只给偏移，宿主就表达不了用户点的是哪一个位置，于是"点后一个字段的开头"只能按最内层
 * 规则归给前一个字段（前一个字段只读时那次点击直接失效，相邻空字段更是整段点不进去）。
 *
 * field = "该次点击属于哪个控件"（-1 = 判不出）。它是 side 的投影，给人看与给判据断言用；
 * 宿主搬运落点必须搬 side —— 投影会把"同处好几枚括号"的细节抹平。
 */
std::string hitTest(float x_pt, float y_pt) {
    TextPos p{};
    int owner = -1;
    int side = 0;
    if (!g_engine.hitTestPt(x_pt, y_pt, &p, &owner, &side)) return "null";
    std::ostringstream o;
    o << "{\"para\":" << p.para << ",\"offset\":" << p.offset << ",\"field\":" << owner
      << ",\"side\":" << side << "}";
    return o.str();
}

/**
 * 带侧别的定位（点画布的落点）。side 来自 hitTest 的同一个字段。
 * 宿主不要自己拼这个侧别：它只该原样搬运引擎算出来的结论。
 */
void setCaretAtSide(int para, int offset, int side) {
    g_engine.setCaretAtSide(para, static_cast<size_t>(offset < 0 ? 0 : offset), side);
}

/**
 * 按控件归属定位（-1 = 最内层，LineBox::kInlineOffsetOuter = 贴外侧）。
 * 给探针与诊断用：它们手上有的是控件下标，不知道侧别。点击路径请用 setCaretAtSide ——
 * 归属会抹平"同一偏移上好几枚括号"的细节。
 */
void setCaretAt(int para, int offset, int field) {
    g_engine.setCaretAt(para, static_cast<size_t>(offset < 0 ? 0 : offset), field);
}

/**
 * 表单：控件是区间（模型见 core/doc/control.h）。
 *
 * 这一层只做"配数据加报状态"，不含任何策略判断：判断全在 C++ 侧的编辑能力表，
 * 前端改不了它。这是刻意的 —— 如果"能不能改"由 JS 决定，那它就只是 UI 提示，
 * 而不是边界。
 */

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char ch : s) {
        if (ch == '"' || ch == '\\') {
            o.push_back('\\');
            o.push_back(ch);
        } else if (ch == '\n') {
            o += "\\n";
        } else {
            o.push_back(ch);
        }
    }
    return o;
}

/**
 * 整批换上一份字段名单，重建一次文档树（探针用）。
 *
 * 为什么是"整批"而不是"清空 + 逐个加"：控件是树的产物，改控件就是换一份字段名单再建一次
 *   树。拆成两步做的话，中间态里一枚括号都没有 —— 那一刻"侧别"没有对应物，引擎搬光标只
 *   能抹成 0（实测 7/2 变 7/0，接着打字就进了左边那个字段）。合成一次调用，中间态不存在，
 *   宿主也不必自己存光标再摆回去（见 docs/正式化架构与迁移路线.md 4.2）。
 * 每项形如 {start:[段,偏移], end:[段,偏移], kind, id, ro}。不收 label：树里不存它，
 *   界面上显示的那份是引擎从树里导出的（见 controlsJson）。
 */
void setFormControls(val list) {
    syncBlocksFromEngine();  // 底稿要是引擎里当前那一份（树上编辑可能已经改过文本）
    g_fields.clear();
    const int n = list["length"].as<int>();
    g_fields.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const val it = list[i];
        DemoFieldSpec f;
        f.para = it["start"][0].as<int>();
        f.so = it["start"][1].as<int>();
        f.epara = it["end"][0].as<int>();  // 跨段字段（终段 != 起段）照建：中间那些段落在它里面
        f.eo = it["end"][1].as<int>();
        if (!it["kind"].isUndefined()) f.kind = controlKindFromCode(it["kind"].as<int>());
        if (!it["id"].isUndefined()) f.key = it["id"].as<std::string>();
        f.readonly = !it["ro"].isUndefined() && it["ro"].as<bool>();
        g_fields.push_back(std::move(f));
    }
    rebuildDocument(true);
}
void setEditMode(int mode) { g_engine.setEditMode(editModeFromCode(mode)); }
int editMode() { return static_cast<int>(g_engine.editMode()); }

/**
 * 表单括号的显示范围：0 = All（默认，所有字段常驻括号）、1 = Focused（旧行为：
 * 只有光标所在字段显示括号，其余画小点）。
 * 改它会改变占位（括号进文档流），所以 setConfig 内部会重排 —— 宿主不必
 * 自己再排一次；下一次 present 的脏矩形自然会覆盖被挪动的正文。
 */
void setBracketScope(int scope) {
    EngineConfig c = g_cfg;
    c.form_brackets = (scope == 1) ? FormBracketScope::Focused : FormBracketScope::All;
    g_cfg = c;
    applyConfig();
}
int bracketScope() { return static_cast<int>(g_cfg.form_brackets); }

/**
 * 必须走 AndRender 版：Tab 会让占位跟着换字段、整行重排，
 * 不重绘的话宿主会拿上一次的脏矩形上屏（见 engine.cpp 里那段说明）。
 */
bool focusControl(bool forward) { return g_engine.focusControlAndRender(forward); }

/**
 * Tab / Shift+Tab 的策略版：表单模式在可编辑控件之间前进 / 后退，到最后一个可编辑控件
 * 就返回 false（不消费该 Tab，让宿主把焦点移出编辑器）；文档与只读模式一律返回 false。
 *
 * 与 focusControl 的差别不只是"到末尾绕不绕回"：focusControl 认控件区间的起点，
 * 这里认"内容起点落在可编辑集合内"这个谓词 —— 于是只读控件里嵌的可编辑控件不会成为
 * 落点，反之亦然，与编辑判定共用同一套口径（见 engine.h）。
 * 默认键位不绑它：web 演示页把裸 Tab 留给了缩进，绑什么由宿主决定（模型文档第七节）。
 */
bool tabCaret(bool backward) { return g_engine.tabCaretAndRender(backward); }

/**
 * Tab / Shift+Tab = 缩进 / 反缩进（不是跳字段）。
 * 走 *AndRender：入口改状态就必须自己收尾，理由见 engine.cpp 里那段说明。
 */
std::string auditFrame() { return g_engine.auditFrameJson(); }

/**
 * 排版自审：增量结果 vs 全量重排，逐字段比对（0 = 一致）。
 */
std::string auditLayout() { return g_engine.auditLayoutJson(); }

/**
 * 编译自审：本轮实际编译耗时与一次整树全量编译的耗时同源对照。
 * 判"编译这一层有没有做到增量"只能靠它 —— 没有分母的耗时数说明不了任何事。
 */
std::string auditCompile() { return g_engine.auditCompileJson(); }

/**
 * 上屏矩形：宿主必须用它，不能用 editStats 的 repaintedX/Y/W/H。
 * 前者是"自上次上屏以来写过哪些区域"（读后清零），后者是"这一轮写了多少"。
 * 一次用户操作可能触发两次 present（见 afterEdit 那条补救路径），
 * 用后一轮的账上屏会把前一轮整个丢掉 —— 症状正是
 * "光标走了、文字没动、方括号还留在原地"。
 */
std::string takeBlitBoxJson() {
    const Rect r = g_engine.takeBlitBox();
    std::ostringstream o;
    o << "{\"x\":" << static_cast<int>(r.x) << ",\"y\":" << static_cast<int>(r.y)
      << ",\"w\":" << static_cast<int>(r.w) << ",\"h\":" << static_cast<int>(r.h) << "}";
    return o.str();
}

bool indentText() { return g_engine.indentAndRender(); }
bool outdentText() { return g_engine.outdentAndRender(); }
int controlAtPoint(float x_pt, float y_pt) { return g_engine.controlAtPt(x_pt, y_pt); }
std::string describeControls() { return g_engine.describeControls(); }

/**
 * 累计被拒次数。必须有这个直读接口（而不是只走 editStatsJson）：
 * 编辑被拒时不会触发 repaint，而 EditStats 是在 presentInternal 里刷新的 ——
 * 于是"刚刚被拒的"在那个 JSON 里读不到（要等下一次呈现才出现）。
 * 探针按键之后立刻要读它，走 EditStats 会永远差一拍。
 */
int blockedEdits() { return static_cast<int>(g_engine.blockedEdits()); }

/**
 * 控件列表 + 每个控件在页面坐标下的矩形（跨行会是多个）。
 * 前端拿它画 hover 提示、或者把 DOM 控件贴到正确位置。
 */
std::string controlsJson() {
    const ControlSet& cs = g_engine.controls();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"mode\":" << static_cast<int>(g_engine.editMode()) << ",\"count\":"
      << cs.size()
      // 归属与落笔口径一致：点画布定下来的提示优先（见 EditorEngine::focusedControlIndex）。
      //   不这么写的话，界面上显示"光标在左边那个字段"，而打出来的字进了右边那个。
      << ",\"caret\":" << g_engine.focusedControlIndex() << ",\"items\":[";
    for (int i = 0; i < cs.size(); ++i) {
        const Control& c = cs.at(i);
        if (i) o << ",";
        o << "{\"i\":" << i << ",\"kind\":\"" << jsonEscape(controlKindName(c.kind))
          << "\",\"id\":\"" << jsonEscape(c.id) << "\",\"label\":\"" << jsonEscape(c.label)
          << "\",\"ro\":" << (c.read_only ? "true" : "false") << ",\"depth\":" << c.depth
          << ",\"parent\":" << c.parent << ",\"start\":[" << c.range.start.para << ","
          << c.range.start.offset << "]"
          << ",\"end\":[" << c.range.end.para << "," << c.range.end.offset << "],\"rects\":[";
        const std::vector<EditorEngine::ControlRect> rs = g_engine.controlRects(i);
        for (size_t k = 0; k < rs.size(); ++k) {
            if (k) o << ",";
            o << "{\"page\":" << rs[k].page << ",\"x\":" << rs[k].r_pt.x
              << ",\"y\":" << rs[k].r_pt.y << ",\"w\":" << rs[k].r_pt.w << ",\"h\":" << rs[k].r_pt.h
              << "}";
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

/**
 * 占位槽清单：括号插进文档流之后的每一个槽（偏移、开闭、归属、所在行、行内 x）。
 *
 * 为什么必须有这个口子：括号是排版阶段插进簇序列的占位簇，"同一处边界上的五枚
 * 括号到底排成 ][][] 还是 [[ ]]" 这件事只存在于排版结果里。截图分不出来（两者
 * 都是五枚括号），区间数据也分不出来（区间一模一样）。探针要钉住"两个零宽字段
 * 的括号交替出现、而不是互相嵌套"，就只能读这份结构。
 * 只读接口，不改任何状态。
 */
std::string bracketSlotsJson() {
    const LayoutResult& lay = g_engine.layout();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"slots\":[";
    bool first = true;
    for (const LineBox& lb : lay.lines) {
        for (const LineCluster& c : lb.clusters) {
            if (!c.placeholder) continue;
            if (!first) o << ",";
            first = false;
            o << "{\"para\":" << lb.paragraph << ",\"line\":" << lb.line << ",\"cp\":" << c.cp
              << ",\"side\":" << static_cast<int>(c.bracket_side) << ",\"ro\":"
              << (c.read_only ? "true" : "false")
              // 占位簇上记的是产生端的句柄（持树时是控件节点 id），宿主要的是控件下标 ——
              //   差这一道翻译，报出来的归属就与 controls.items 的下标对不上。
              << ",\"owner\":" << g_engine.controlIndexOfOwner(c.owner_control)
              << ",\"x\":" << (lb.inline_origin_pt + c.inline_pt) << "}";
        }
    }
    o << "]}";
    return o.str();
}

/**
 * 一处演示数据，覆盖表单的四条关键语义（区间之外是没有控件的普通正文）：
 * variant 0（短文档）：[5,9)「反复心悸」可编辑 ｜ [9,11)「三天」只读 ｜ [12,16)「加重一天」可编辑
 * → 0..4、11、16..17 打不进字；从 8 往右移跨过只读的 9~11 落到 12。
 * variant 1（长文档）：一个跨行的长字段，里面嵌一个只读小字段
 * → 演示"控件不是矩形"（矩形块数 > 1）与"内层策略优先"。
 * variant 2（短文档）：三个可编辑字段紧挨着 —— [5,7)、[7,9)、[9,11) 共享两条边界
 * → 边界两侧都可写，"在边界上打字 / 把中间那个删空 / 在两段之间退格"都能当场试出来。
 * variant 3（短文档）：三层嵌套、三层都可编辑 —— [5,11) ⊃ [6,10) ⊃ [7,9)
 * → 归属取最内层，且改内层时中/外层必须一起长（区间不许互相穿过）；
 *   括号读作 [[[心悸]悸三]，三个开括号在左、闭括号由内到外依次收。
 */

/**
 * 压力文档：造 paras 段、每段 chars 个汉字的横排中文。
 *
 * 为什么必须有这个口子：演示场景只有一段，量出来的编译与索引耗时全是 0.0x ms ——
 * 既看不出 O(文档) 的那一层，也证明不了"增量到底省了多少"。性能判据只能在大文档上做，
 * 而且要能调长度：同一份代码在 10 段与 1000 段上的差别，才是可证伪的证据。
 */
void loadStressDocument(int paras, int chars) {
    if (paras < 1) paras = 1;
    if (chars < 1) chars = 1;
    std::string text;
    for (int k = 0; k < chars; ++k) text += "字";
    std::vector<BlockSpec> blocks;
    for (int i = 0; i < paras; ++i) {
        BlockSpec b;
        b.font_index = g_cjk_font;
        b.size_pt = 12.0f;
        b.wm = WritingMode::HorizontalTB();
        b.line_factor = 1.5f;
        b.language = "zh";
        b.color = Color::rgb(24, 24, 24);
        b.setText(text);
        blocks.push_back(std::move(b));
    }
    g_blocks = std::move(blocks);
    g_fields.clear();
    rebuildDocument(false);
}

void loadFormSample(int variant) {
    g_blocks = buildScenarioBlocks(variant == 1 ? 3 : 6);
    g_fields.clear();
    auto push = [&](int so, int eo, ControlKind k, const char* id, bool ro) {
        DemoFieldSpec f;
        f.para = 0;
        f.so = so;
        f.eo = eo;
        f.kind = k;
        f.key = id;
        f.readonly = ro;
        g_fields.push_back(std::move(f));
    };
    if (variant == 1) {
        push(5, 60, ControlKind::TextField, "history", false);
        push(20, 26, ControlKind::TextField, "onset", true);
    } else if (variant == 2) {
        push(5, 7, ControlKind::TextField, "part1", false);
        push(7, 9, ControlKind::TextField, "part2", false);
        push(9, 11, ControlKind::TextField, "part3", false);
    } else if (variant == 3) {
        // 三层嵌套：外层盖住整串「反复心悸三天」，中层剥掉两端的字，
        //   内层只留中间两个字「心悸」。三层都可编辑 —— 这是"嵌套"最需要覆盖的一格：
        //   归属必须取最内层（否则改内层会改成外层），而内层一动，中/外层都要跟着长。
        push(5, 11, ControlKind::TextField, "outer", false);
        push(6, 10, ControlKind::TextField, "middle", false);
        push(7, 9, ControlKind::TextField, "inner", false);
    } else {
        push(5, 9, ControlKind::TextField, "complaint", false);
        push(9, 11, ControlKind::NumberField, "duration", true);
        push(12, 16, ControlKind::TextField, "aggravate", false);
    }
    rebuildDocument(false);
    g_engine.setEditMode(EditMode::Form);
    // 必须重排：上面换过文档，而 layout_ 还停在上一次的行盒上。
    //   不重排的话控件会投影到旧几何上 —— 症状是"字段框比文本偏了一截、
    //   或者在错误的行上被切成两块"，而控件数据本身看着完全正常。
    g_engine.relayout();
}

/**
 * 编辑统计。这几个数字是档 1 的验收依据：
 * 重排 N 行 / 重画 W×H 像素 —— 都是引擎自己记账出来的，不是估算。
 */
std::string editStatsJson() {
    const EditStats& e = g_engine.editStats();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << "{";
    o << "\"layoutRan\":" << (e.layout_ran ? "true" : "false") << ",";
    o << "\"relaidLines\":" << e.relaid_lines << ",";
    o << "\"totalLines\":" << e.total_lines << ",";
    o << "\"relaidParagraphs\":" << e.relaid_paragraphs << ",";
    o << "\"reusedParagraphs\":" << e.reused_paragraphs << ",";
    o << "\"suffixShifted\":" << (e.suffix_shifted ? "true" : "false") << ",";
    o << "\"totalParagraphs\":" << e.total_paragraphs << ",";
    o << "\"layoutWasFull\":" << (e.layout_was_full ? "true" : "false") << ",";
    o << "\"dirtyPageCount\":" << e.dirty_page_count << ",";
    o << "\"repaintedPages\":" << e.repainted_pages << ",";
    o << "\"totalPages\":" << e.total_pages << ",";
    o << "\"windowPages\":" << e.window_pages << ",";
    o << "\"windowFirstPage\":" << e.window_first_page << ",";
    o << "\"repaintedPixels\":" << e.repainted_pixels << ",";
    o << "\"repaintedX\":" << e.repainted_x << ",";
    o << "\"repaintedY\":" << e.repainted_y << ",";
    o << "\"repaintedW\":" << e.repainted_w << ",";
    o << "\"repaintedH\":" << e.repainted_h << ",";
    o << "\"frameW\":" << e.frame_w << ",";
    o << "\"frameH\":" << e.frame_h << ",";
    o << "\"framePixels\":" << (static_cast<long long>(e.frame_w) * e.frame_h) << ",";
    o << "\"layoutMs\":" << e.layout_ms << ",";
    o << "\"rebuildCompileMs\":" << e.rebuild_compile_ms << ",";
    o << "\"rebuildFlowMs\":" << e.rebuild_flow_ms << ",";
    o << "\"rebuildFlowPosMs\":" << e.rebuild_flowpos_ms << ",";
    o << "\"rebuildCopyMs\":" << e.rebuild_copy_ms << ",";
    o << "\"rebuildControlsMs\":" << e.rebuild_controls_ms << ",";
    o << "\"pageFpMs\":" << g_engine.stats().page_fp_ms << ",";
    o << "\"paintMs\":" << e.paint_ms << ",";
    o << "\"totalMs\":" << e.total_ms << ",";
    o << "\"caretPara\":" << e.caret_para << ",";
    o << "\"caretOffset\":" << e.caret_offset << ",";
    o << "\"caretIndex\":" << e.caret_index << ",";
    o << "\"docCodepoints\":" << e.doc_codepoints << ",";
    o << "\"caretSide\":" << e.caret_side << ",";
    o << "\"caretPage\":" << e.caret_page << ",";
    o << "\"caretVisible\":" << (e.caret_visible ? "true" : "false") << ",";
    o << "\"caretX\":" << e.caret_x_px << ",";
    o << "\"caretY\":" << e.caret_y_px << ",";
    o << "\"caretW\":" << e.caret_w_px << ",";
    o << "\"caretH\":" << e.caret_h_px << ",";
    // 表单：模式、控件数、光标所在控件、累计被拒次数
    o << "\"editMode\":" << e.edit_mode << ",";
    o << "\"controlCount\":" << e.control_count << ",";
    o << "\"caretControl\":" << e.caret_control << ",";
    o << "\"blockedEdits\":" << e.blocked_edits;
    o << "}";
    return o.str();
}

/**
 * 诊断：把当前光标的行盒几何、簇表摊平。用于"光标为什么在这儿"这类问题的取证。
 */
std::string caretReport() { return g_engine.describeCaret(); }

/**
 * 当前文档全文（UTF-8），供宿主显示/调试。注意：宿主拿到它只用于显示，
 * 绝不能用它自己的字符串索引去算位置 —— 偏移的单位在引擎里是码点，
 * 而 JS 字符串是 UTF-16 码元，两者在中文/上标/emoji 上必然错位。
 */
std::string documentText(int para) {
    const auto& blocks = g_engine.blocks();
    if (para < 0 || para >= static_cast<int>(blocks.size())) return std::string();
    return blocks[static_cast<size_t>(para)].textUtf8();
}

/**
 * 当前帧到底是哪一套光栅器画出来的。
 * 为什么值得专门暴露一个入口：这两个后端的输出在"帧尺寸、重画页数、写入像素数、
 * 甚至两遍渲染逐字节相等"这些指标上完全一样 —— 就是因为面板上写死了
 * "软件光栅"，而实际跑的是另一套，排查时对着一张空白的帧去怀疑几何。
 * 把来源标在面板上，是这类"指标全绿但画面上什么都没有"唯一廉价的解药。
 */
std::string rasterBackend() {
#if defined(HEDITOR_USE_SKIA)
    return "Skia（SkCanvas + FreeType）";
#else
    return "自写扫描线光栅器";
#endif
}

}  // namespace

int main() { return 0; }

EMSCRIPTEN_BINDINGS(heditor) {
    emscripten::function("rasterBackend", &rasterBackend);
    emscripten::function("allocBuffer", &allocBuffer);
    emscripten::function("freeBuffer", &freeBuffer);
    emscripten::function("loadFont", &loadFont);
    emscripten::function("setFontRoles", &setFontRoles);

    emscripten::function("setScenario", &setScenario);
    emscripten::function("setZoom", &setZoom);
    emscripten::function("setPageMetrics", &setPageMetrics);

    emscripten::function("relayout", &layoutOnly);
    emscripten::function("relayoutAndPaint", &relayoutAndPaint);
    emscripten::function("benchRelayoutPaint", &benchRelayoutPaint);

    // 档 1：编辑与光标
    emscripten::function("typeText", &typeText);
    emscripten::function("backspace", &backspaceText);
    emscripten::function("deleteForward", &deleteForwardText);
    emscripten::function("splitParagraph", &splitParagraph);
    emscripten::function("mergeWithPreviousParagraph", &mergeWithPreviousParagraph);
    emscripten::function("mergeWithNextParagraph", &mergeWithNextParagraph);
    emscripten::function("moveCaret", &moveCaret);
    emscripten::function("caretToLineStart", &caretToLineStart);
    emscripten::function("caretToLineEnd", &caretToLineEnd);
    emscripten::function("setCaret", &setCaret);
    emscripten::function("setCaretAt", &setCaretAt);
    emscripten::function("setCaretAtSide", &setCaretAtSide);
    emscripten::function("showCaret", &showCaret);
    emscripten::function("present", &present);
    emscripten::function("caretOffsetLive", &caretOffsetLive);
    emscripten::function("hitTest", &hitTest);
    emscripten::function("editStatsJson", &editStatsJson);
    emscripten::function("caretReport", &caretReport);
    emscripten::function("documentText", &documentText);

    // 表单（控件是区间 + 编辑策略）
    emscripten::function("setFormControls", &setFormControls);
    emscripten::function("setEditMode", &setEditMode);
    emscripten::function("editMode", &editMode);
    emscripten::function("setBracketScope", &setBracketScope);
    emscripten::function("bracketScope", &bracketScope);
    emscripten::function("focusControl", &focusControl);
    emscripten::function("tabCaret", &tabCaret);
    emscripten::function("auditFrame", &auditFrame);
    emscripten::function("auditLayout", &auditLayout);
    emscripten::function("auditCompile", &auditCompile);
    emscripten::function("takeBlitBox", &takeBlitBoxJson);
    emscripten::function("indent", &indentText);
    emscripten::function("outdent", &outdentText);
    emscripten::function("controlAtPoint", &controlAtPoint);
    emscripten::function("controlsJson", &controlsJson);
    emscripten::function("bracketSlotsJson", &bracketSlotsJson);
    emscripten::function("describeControls", &describeControls);
    emscripten::function("blockedEdits", &blockedEdits);
    emscripten::function("loadFormSample", &loadFormSample);
    emscripten::function("loadStressDocument", &loadStressDocument);

    emscripten::function("frameWidth", &frameWidth);
    emscripten::function("frameHeight", &frameHeight);
    emscripten::function("frameByteSize", &frameByteSize);
    emscripten::function("frameRgba", &frameRgba);

    emscripten::function("setViewportAnchor", &setViewportAnchor);
    emscripten::function("windowFirstPage", &windowFirstPage);
    emscripten::function("windowPageCount", &windowPageCount);
    emscripten::function("pagePxWidth", &pagePxWidth);
    emscripten::function("pagePxHeight", &pagePxHeight);
    emscripten::function("pageGapPx", &pageGapPx);
    emscripten::function("dirtyPagesJson", &dirtyPagesJson);

    emscripten::function("statsJson", &statsJson);
    emscripten::function("buildStamp", &buildStamp);
    emscripten::function("shapeReport", &shapeReport);
}
