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
#include "core/utils/utf8.h"
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

/**
 * 按需装一张脸：字体已由宿主写进 MEMFS，这里只给路径。引擎按需读它的块，
 * 内存里只留一张骨架（小表）与块缓存，不再整份常驻。
 */
int loadFontFromFile(const std::string& path) { return g_engine.fonts().addFaceLazy(path); }

/**
 * 改字体块缓存的配置（块大小与块数），并清空已有块与统计。
 *   给"块大小该取多少"做实验用 —— 它一直是个没有依据的参数（见 font_source.h 的说明）。
 */
void setFontCacheConfig(int block_size, int max_blocks) {
    g_engine.fonts().setCacheConfig(block_size > 0 ? static_cast<size_t>(block_size) : 0u,
                                    max_blocks > 0 ? static_cast<size_t>(max_blocks) : 0u);
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
 * 表格场景：这一档不走块流建树（表格结构不在 BlockSpec 里），直接建一棵含表格的树。
 */
bool g_table_demo = false;

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
        case 7: {  // 档 2：长文档 —— 多段才有"可复用的前缀"可谈（段数见下面这行）
            blocks = longDocument(2000, g_cjk_font);
            break;
        }
        case 9: {  // 行内原子：图片与公式的显式占位框（阶段 4）
            BlockSpec b;
            b.font_index = g_cjk_font;
            b.size_pt = 14.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.85f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            // 先设文本：setText 会清空对象表（锚点是按旧文本算的）。
            b.setText("心电图：图三张，节律规整。");
            InlineObject img;
            img.cp = 4;
            img.kind = InlineObjectKind::Atom;
            img.atom = InlineAtom::Image;
            img.w_pt = 60.0f;
            img.h_pt = 45.0f;
            b.objects.push_back(img);
            InlineObject fm;
            fm.cp = 9;
            fm.kind = InlineObjectKind::Atom;
            fm.atom = InlineAtom::Formula;
            fm.w_pt = 90.0f;
            fm.h_pt = 28.0f;
            b.objects.push_back(fm);
            blocks.push_back(std::move(b));

            // 第二段纯文本：用来看原子把后面的字挤开、以及长到换行时原子整枚走。
            BlockSpec b2;
            b2.font_index = g_cjk_font;
            b2.size_pt = 14.0f;
            b2.wm = WritingMode::HorizontalTB();
            b2.line_factor = 1.85f;
            b2.space_after_pt = 6.0f;
            b2.language = "zh";
            b2.color = Color::rgb(24, 24, 24);
            b2.setText(chineseNote() + chineseNote());
            blocks.push_back(std::move(b2));
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
    // 行别同一待遇：折行处的光标贴哪一行是用户点出来的，重建之后不该变。
    const int line_aff = keep_caret ? g_engine.caretLineAffinity() : 0;
    std::string text_before;
    if (keep_caret) {
        for (const BlockSpec& b : g_engine.blocks()) text_before += b.textUtf8();
    }
    g_engine.setDocument(g_table_demo ? demoTableTree(g_cjk_font)
                                      : demoTree(g_blocks, g_fields),
                         StyleSheet{});
    if (!keep_caret) return;
    std::string text_after;
    for (const BlockSpec& b : g_engine.blocks()) text_after += b.textUtf8();
    if (text_before != text_after) return;  // 换了文档：光标归零才是对的
    if (before.para < 0 || before.para >= static_cast<int>(g_engine.blocks().size())) return;
    if (before.offset > g_engine.blocks()[static_cast<size_t>(before.para)].cp_text.size()) return;
    g_engine.setCaretAtSideLine(before.para, before.offset, side, line_aff);
}

void setScenario(int scenario) {
    g_table_demo = (scenario == 10);
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

/**
 * 页间隙（磅）。连页模式把它设成 0：页首尾相接，一屏内容在帧里是连续的一段，
 *   于是"一张视口画布"可以按文档坐标直接贴，页与页之间不会留缝。
 * 与页度量分开成一条门面，是因为它只影响页的落点，不影响页本身的宽高与内容区。
 */
void setPageGap(float gap_pt) {
    g_cfg.page_gap_pt = gap_pt < 0.0f ? 0.0f : gap_pt;
    applyConfig();
}

/**
 * 连页模式开关（不分页，内容一列连到底）。
 *
 * 它改的是排版的形状，所以走 applyConfig（setConfig 会判"页度量变了"并整篇重排）。
 *   宿主配合的两件事：页间隙设成 0（条带首尾相接）、滚动高度取 contentPx。
 */
void setContinuous(bool on) {
    g_cfg.continuous = on;
    applyConfig();
}

/**
 * 内容底边（帧像素）。连页下宿主拿它当滚动高度（见 EditorEngine::contentBottomPx）。
 */
int contentPx() { return g_engine.contentBottomPx(); }

/**
 * 撤销栈的深度上限（条数，0 = 不限制）。与 EngineConfig::undo_limit 是同一处值。
 * 它不影响呈现，所以走 applyConfig 那条路就够了（不重排、不重绘）。
 */
void setUndoLimit(int n) {
    g_cfg.undo_limit = n < 0 ? 0 : n;
    applyConfig();
}
int undoLimit() { return g_cfg.undo_limit; }

/**
 * 色通道夹到 0~255：宿主的面板递进来的可能越界，而截断的失败方式不明显。
 */
uint8_t channelClamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/**
 * 光标样式：颜色与宽度。色值是各通道 0~255，宽度单位是磅。
 *
 * 走引擎的 setCaretAndSelectionStyle 而不是 applyConfig：后者会作废底图，
 *   而底图全建要把窗口里的每一页重新光栅（3 页约 32 ms）—— 取色器连续改时每一下都付。
 * 不在这里呈现：与 setZoom 同一档，由宿主决定何时上屏（两个宿主的门面都会补一次 present）。
 */
void setCaretStyle(int r, int g, int b, int a, float width_pt) {
    g_cfg.caret_color = Color{channelClamp(r), channelClamp(g), channelClamp(b), channelClamp(a)};
    g_engine.setCaretAndSelectionStyle(g_cfg.caret_color, width_pt, g_cfg.selection_color);
    // 引擎会把过窄的宽度夹到下限，取回来才是真正的现状（宿主面板要读它）。
    g_cfg.caret_width_pt = g_engine.config().caret_width_pt;
}

/**
 * 选区样式：底色与 alpha。alpha 不足 255 是刻意的，见 EngineConfig::selection_color。
 */
void setSelectionStyle(int r, int g, int b, int a) {
    g_cfg.selection_color =
        Color{channelClamp(r), channelClamp(g), channelClamp(b), channelClamp(a)};
    g_engine.setCaretAndSelectionStyle(g_cfg.caret_color, g_cfg.caret_width_pt,
                                       g_cfg.selection_color);
}

/**
 * 光标样式的现状。宿主面板用它初始化控件 —— 默认值只在 EngineConfig 里有一份。
 */
std::string caretStyleJson() {
    const EngineConfig& c = g_engine.config();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"color\":[" << static_cast<int>(c.caret_color.r) << ","
      << static_cast<int>(c.caret_color.g) << "," << static_cast<int>(c.caret_color.b) << ","
      << static_cast<int>(c.caret_color.a) << "],\"widthPt\":" << c.caret_width_pt << "}";
    return o.str();
}

/**
 * 选区样式的现状。形状与 caretStyleJson 一致，少一个宽度。
 */
std::string selectionStyleJson() {
    const Color& s = g_engine.config().selection_color;
    std::ostringstream o;
    o << "{\"color\":[" << static_cast<int>(s.r) << "," << static_cast<int>(s.g) << ","
      << static_cast<int>(s.b) << "," << static_cast<int>(s.a) << "]}";
    return o.str();
}

/**
 * 预热一页的字形：只把 mask 光栅进缓存，画面不动、也不呈现。
 *
 * 宿主在滚动停下之后的空闲帧里调用它，把"下一页的字形"这笔钱先付掉 ——
 *   换窗那一拍于是只剩铺底色与帧搬运（见 EditorEngine::prewarmPageGlyphs）。
 * 返回本次预热触及的字形数，宿主据此判断"这一页还有没有要预热的"。
 */
int prewarmPageGlyphs(int page) { return g_engine.prewarmPageGlyphs(page); }

void relayoutAndPaint() {    g_engine.relayout();
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
 *
 * 不调它 = 整篇帧。长文档上差别是一个量级：2000 段实测每实例堆 82.6 MB（窗口 3 页）
 * 对 1031.6 MB（整篇），装载 115 ms 对 1583 ms。数字与拆解见该函数的注释。
 */
void setViewportAnchor(int first_page, int page_count) {
    g_engine.setViewportAnchor(first_page, page_count);
}

int windowFirstPage() { return g_engine.windowFirstPage(); }

/**
 * 帧槽数：页在帧内的行偏移 = (页号 mod 槽数) x 步距。宿主切帧用它算源行，
 *   所以两侧必须是同一个数 —— 它由 worker 按窗口页数上限设一次，宿主读回同一个值。
 */
int frameSlots() { return g_engine.frameSlots(); }
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
    // 字体按需读的账。整份装载时这些数全是零 —— 那就说明按需这条路没被走到。
    //   实时取而不是用快照：字形光栅化在排版之后，那一段的读不进排版时的快照。
    const FontCacheTotals fc = g_engine.fonts().cacheTotals();
    o << "\"fontCacheHits\":" << fc.hits << ",";
    o << "\"fontCacheMisses\":" << fc.misses << ",";
    o << "\"fontStorageReads\":" << fc.storage_reads << ",";
    o << "\"fontStorageBytes\":" << fc.storage_bytes << ",";
    o << "\"fontIoMs\":" << fc.io_ms << ",";
    o << "\"fontResidentBytes\":" << fc.resident_bytes << ",";
    o << "\"fontStorageFailures\":" << fc.read_failures << ",";
    o << "\"fontStorageEofs\":" << fc.eof_reads << ",";
    o << "\"shapeMs\":" << s.shape_ms << ",";
    o << "\"layoutMs\":" << s.layout_ms << ",";
    o << "\"pageFpMs\":" << s.page_fp_ms << ",";
    o << "\"paintMs\":" << s.paint_ms << ",";
    // 三段的用途是判"该优化哪一段"：铺面积走 memcpy、字形走扫描线光栅、
    //   帧搬运走 copyRectFrom。合成一个 paintMs 就只能得出"绘制慢"。
    o << "\"paintFillMs\":" << s.paint_fill_ms << ",";
    o << "\"paintGlyphMs\":" << s.paint_glyph_ms << ",";
    // 含在 paintGlyphMs 里的一笔：只为记账取字形包围盒。它不画像素，
    //   判"字形贵不贵"时必须先把它减掉，否则会把记账算成光栅。
    o << "\"paintGlyphBoundsMs\":" << s.paint_glyph_bounds_ms << ",";
    o << "\"paintedGlyphs\":" << s.painted_glyphs << ",";
    o << "\"paintPresentMs\":" << s.paint_present_ms << ",";
    // 残差那五段的分解。残差 = 总耗时减掉铺面积与字形，它装的是入口准备、换窗与页循环的
    //   杂项、帧贴回、上屏记账、光标与上报 —— 合成一个数就只能猜该动哪一块。
    //   注意写底图那两段（铺面积、字形）包在 paintSwapMs 里面，不是与它并列。
    o << "\"paintPreMs\":" << s.paint_pre_ms << ",";
    o << "\"paintSwapMs\":" << s.paint_swap_ms << ",";
    o << "\"paintBlitMs\":" << s.paint_blit_ms << ",";
    o << "\"paintOverlayMs\":" << s.paint_overlay_ms << ",";
    o << "\"paintLedgerMs\":" << s.paint_ledger_ms << ",";
    o << "\"paintReportMs\":" << s.paint_report_ms << ",";
    o << "\"totalMs\":" << s.total_ms << ",";
    o << "\"frameWidth\":" << s.frame_width << ",";
    o << "\"frameHeight\":" << s.frame_height << ",";
    o << "\"frameBytes\":" << g_engine.frameBytes() << ",";
    o << "\"glyphCacheHits\":" << s.glyph_cache_hits << ",";
    o << "\"glyphCacheMisses\":" << s.glyph_cache_misses << ",";
    o << "\"glyphCacheEntries\":" << s.glyph_cache_entries << ",";
    // 预算与占用（字节）：占用贴着上限就说明缓存随时会被整份抹掉 —— 那时
    //   "预热"与"命中"都无从谈起，而面板上只看命中率是看不出来的。
    o << "\"glyphCacheBytes\":" << s.glyph_cache_bytes << ",";
    o << "\"glyphCacheLimit\":" << s.glyph_cache_limit << ",";
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
bool moveCaret(int dcp, bool extend) { return g_engine.moveCaretAndRender(dcp, extend); }
bool caretToLineStart(bool extend) { return g_engine.moveCaretToLineStartAndRender(extend); }
bool caretToLineEnd(bool extend) { return g_engine.moveCaretToLineEndAndRender(extend); }
/**
 * 到文档首 / 末（Ctrl+Home / Ctrl+End）。
 */
bool caretToDocStart(bool extend) { return g_engine.moveCaretToDocStartAndRender(extend); }
bool caretToDocEnd(bool extend) { return g_engine.moveCaretToDocEndAndRender(extend); }
/**
 * 上下键：dir < 0 向上、> 0 向下。列位沿行内轴保持（跨短行不丢列）。
 */
bool moveCaretVertical(int dir, bool extend) {
    return g_engine.moveCaretVerticalAndRender(dir, extend);
}
/**
 * 选区三个入口。两端是全局流位置，不是段内偏移 —— 与事务里那两份选区快照
 * 同一个坐标系，所以探针拿到的数与模型层的 Range 是同一把尺子。
 */
bool selectAll() { return g_engine.selectAllAndRender(); }
bool setSelection(int anchor, int focus) {
    return g_engine.setSelectionAndRender(
        Range{static_cast<FlowPos>(anchor), static_cast<FlowPos>(focus)});
}
bool clearSelection() {
    g_engine.clearSelection();
    g_engine.repaint();
    return true;
}
void setCaret(int para, int offset) { g_engine.setCaret(para, static_cast<size_t>(offset)); }
void showCaret(bool on) { g_engine.showCaret(on); }
void present() { g_engine.repaint(); }

/**
 * 撤销 / 重做一条事务（栈里没有可撤销的条目时返回 false）。
 * 走 AndRender 版 —— 与其它编辑入口同一口径，宿主一次调用就收尾。
 * web 宿主的默认键位是 Ctrl/Cmd+Z 撤、Ctrl+Y 与 Ctrl+Shift+Z 重做（在 app.js 的
 *   onKeyDown 里）；这里这一层是给探针与自动化用的同一件事，不必造键盘事件。
 */
bool undoEdit() { return g_engine.undoAndRender(); }
bool redoEdit() { return g_engine.redoAndRender(); }

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
 * 光标是否落在竖排的段里（直读）。宿主的方向键映射要用它 ——
 * 竖排的行内轴是竖直的，上下走字、左右换列（见宿主侧的键位表）。
 * 键位映射必须同步问，所以它不能走 editStats 那条异步回传。
 */
bool caretVerticalNow() { return g_engine.caretVertical(); }

/**
 * 光标所在段的列序是否反向（直读）。与 caretVerticalNow 同一条路：
 * 竖排的左右是换列，而往左还是往右换由列序决定（中日竖排左、蒙文右）。
 */
bool caretBlockReversedNow() { return g_engine.caretBlockReversed(); }

/**
 * 选区取证：两端（全局流位置）、是否为空，以及它在页面坐标下的矩形。
 *
 * 矩形是判据要的正面证据 —— "选中了哪一段"最终要落到"哪几行被盖住了"。空选区给空数组，
 * 与"光标没有尺寸"是同一条：选区的可视范围就是矩形，没矩形就没选中。
 *
 * 矩形只给当前窗口内的那部分（selectionRectsVisible），与"这一帧画了什么"同一口径。
 *   整篇给的话，2000 段全选就是 4000 条 / 197 KB，而 Worker 宿主每一拍状态都要推一份
 *   （实测每次交互的墙钟从 3.4 ms 涨到 50.3 ms，拖选直接不跟手）。
 *   要看"选区到底盖了多少行"，用两端 anchor..focus，它与窗口无关。
 */
std::string selectionReport() {
    const Range r = g_engine.selection();
    const std::vector<EditorEngine::PageRect> rects = g_engine.selectionRectsVisible();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"anchor\":" << r.anchor << ",\"focus\":" << r.focus
      << ",\"empty\":" << (r.empty() ? "true" : "false") << ",\"rects\":[";
    for (size_t i = 0; i < rects.size(); ++i) {
        if (i) o << ",";
        o << "{\"page\":" << rects[i].page << ",\"x\":" << rects[i].r_pt.x
          << ",\"y\":" << rects[i].r_pt.y << ",\"w\":" << rects[i].r_pt.w
          << ",\"h\":" << rects[i].r_pt.h << "}";
    }
    o << "]}";
    return o.str();
}

/**
 * 命中测试：文档坐标（磅）→ 位置。返回 "null" 或 {para, offset, field, side, line}。
 *
 * side 是落点的完整身份：相邻字段共享边界、同一偏移上压着几枚括号时，只给偏移分不开
 *   "前一个字段的末尾"与"后一个字段的开头"（前一个字段只读时那次点击直接失效）。
 *   field 是它的投影，只给人看 —— 宿主搬运落点必须搬 side。
 * line 是行别（见引擎的 caretLineAffinity）：折行处"上一行行尾"与"下一行行首"是同一个
 *   偏移与侧别，只有它分得开；宿主必须原样带回去（setCaretAtSideLine），
 *   否则点下一行行首会画到上一行行尾。
 */
std::string hitTest(float x_pt, float y_pt) {
    TextPos p{};
    int owner = -1;
    int side = 0;
    int line_aff = 0;
    if (!g_engine.hitTestPt(x_pt, y_pt, &p, &owner, &side, &line_aff)) return "null";
    std::ostringstream o;
    o << "{\"para\":" << p.para << ",\"offset\":" << p.offset << ",\"field\":" << owner
      << ",\"side\":" << side << ",\"line\":" << line_aff << "}";
    return o.str();
}

/**
 * 带侧别与行别的定位（点画布的落点）。side 与 line 都来自 hitTest 的同一份结果。
 * 宿主不要自己拼这两个数：它只该原样搬运引擎算出来的结论。
 */
void setCaretAtSideLine(int para, int offset, int side, int line_aff) {
    g_engine.setCaretAtSideLine(para, static_cast<size_t>(offset < 0 ? 0 : offset), side,
                                line_aff);
}

/**
 * 带侧别的定位（点画布的落点）。side 来自 hitTest 的同一个字段。
 * 宿主不要自己拼这个侧别：它只该原样搬运引擎算出来的结论。
 */
void setCaretAtSide(int para, int offset, int side) {
    g_engine.setCaretAtSide(para, static_cast<size_t>(offset < 0 ? 0 : offset), side);
}

/**
 * 拖选的移动端：只搬焦点端，anchor 不动。side 与 setCaretAtSide 同一来源。
 * 定位与拖选是两个动作，这里刻意不合并：合并之后"按下"这件事就没有落点了。
 */
void setFocusAtSide(int para, int offset, int side) {
    g_engine.setFocusAtSide(para, static_cast<size_t>(offset < 0 ? 0 : offset), side);
}

/**
 * 拖选的移动端（带行别）。line 与 setCaretAtSideLine 同一来源。
 * 拖到下一行行首时也要贴着那一行，否则选区末端的矩形停在上一行行尾。
 */
void setFocusAtSideLine(int para, int offset, int side, int line_aff) {
    g_engine.setFocusAtSideLine(para, static_cast<size_t>(offset < 0 ? 0 : offset), side,
                                line_aff);
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
 * 删控件：拆壳（保留内容）或连内容一起删。走 AndRender 版 —— 宿主一次调用就收尾，
 * 不必自己记得再排一次（漏掉那步的症状是"控件没了但画布上括号还在"）。
 * 表单模式默认被 structureLock 拒（返回 false），探针要删就得先切文档模式。
 */
bool removeControlAt(int index, bool keep_content) {
    return g_engine.removeControlAndRender(index, keep_content);
}

/**
 * 改控件的运行时属性：0 = hidden、1 = readonly、2 = required、3 = structureLock
 * （与 ControlProp 同序）。hidden 改的是呈现态的流结构，重排不能省。
 */
bool setControlPropAt(int index, int prop, bool value) {
    if (prop < 0 || prop > 3) return false;
    return g_engine.setControlPropAndRender(index, static_cast<ControlProp>(prop), value);
}

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
 * 底图自审：平移复用得到的底图 vs 从零全建，逐字节比对（0 = 一致）。
 * 与 auditFrame 的分工见 engine.h：那一条比帧，帧是从底图搬的，盖不住底图本身出错。
 */
std::string auditBase() { return g_engine.auditBaseJson(); }

/**
 * 排版自审：增量结果 vs 全量重排，逐字段比对（0 = 一致）。
 */
std::string auditLayout() { return g_engine.auditLayoutJson(); }

/**
 * 字形页索引自审：二分出来的页区间 vs 全篇扫一遍的归属，逐页比对（0 = 一致）。
 * 绘制侧按页二分之后必须有这一条：auditBase 是"强制全建再比"，而全建走同一条二分，
 *   两边错得一样，逐字节比照样全绿（同一个代码路径当判据必然恒绿）。
 */
std::string auditGlyphPages() { return g_engine.auditGlyphPagesJson(); }

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
/**
 * 某几段的行盒与字形（只读，供诊断"字画到哪儿去了"）。
 *
 * 为什么要有这个口子：行盒与字形是两份产物（一份给光标/选区/命中测试，一份给光栅器），
 *   它们一旦不一致，屏幕上就是"选中了一块但那里没有字"或"字跑到别的条带去"——
 *   而截图分不出是行盒错了还是字形错了。逐条列出来才判得动。
 * 范围由参数框住（一段几十行，够用），不做整篇导出。
 */
std::string linesJson(int from_para, int to_para) {
    const LayoutResult& lay = g_engine.layout();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"lines\":[";
    bool first = true;
    for (const LineBox& lb : lay.lines) {
        if (lb.paragraph < from_para || lb.paragraph > to_para) continue;
        if (!first) o << ",";
        first = false;
        o << "{\"para\":" << lb.paragraph << ",\"line\":" << lb.line << ",\"page\":" << lb.page
          << ",\"v\":" << (lb.vertical ? 1 : 0) << ",\"cp\":[" << lb.first_cp << ","
          << lb.last_cp << "],\"blo\":" << lb.block_lo_pt << ",\"bhi\":" << lb.block_hi_pt
          << ",\"iorg\":" << lb.inline_origin_pt << ",\"iend\":" << lb.inline_end_pt
          << ",\"ncl\":" << lb.clusters.size() << "}";
    }
    o << "],\"glyphs\":[";
    first = true;
    for (const PlacedGlyph& pg : lay.glyphs) {
        if (pg.paragraph < from_para || pg.paragraph > to_para) continue;
        if (!first) o << ",";
        first = false;
        o << "{\"para\":" << pg.paragraph << ",\"line\":" << pg.line << ",\"page\":" << pg.page
          << ",\"cl\":" << pg.cluster << ",\"x\":" << pg.x_pt << ",\"y\":" << pg.y_pt << "}";
    }
    o << "]}";
    return o.str();
}

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
              // 行内原子也在这张表里（它同样是"占了一个槽的占位簇"）：kind 0 不是原子，
              //   1 是图片、2 是公式。判据靠它区分"槽是括号还是原子" —— 两者的
              //   side / owner 都是 0 与 -1，光看那两个字段分不开。
              << ",\"atom\":" << static_cast<int>(c.atom)
              << ",\"h\":" << c.atom_h_pt
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
 *
 * diverse 为真时按一批不同的常用字轮着填，否则整篇是同一个字。这个开关是为字体
 * 按需读的账设的：同一个字重复时字形集合只有一个，字形缓存与块缓存都会命中到失真，
 * 落存储的次数与字节都量不出来。真实中文的重复率本来就高，所以"有重复但集合有
 * 规模"才像真文档。
 */
void loadStressDocumentImpl(int paras, int chars, bool diverse) {
    if (paras < 1) paras = 1;
    if (chars < 1) chars = 1;


    std::vector<BlockSpec> blocks;
    for (int i = 0; i < paras; ++i) {
        std::string text;
        for (int k = 0; k < chars; ++k) {
            if (diverse) {
                // 用一段连续的基本汉字码点当池子：只为了让字形集合有规模。
                //   真实文档的不同字数在几百到几千之间，这里取 800 个。
                appendUtf8(&text, 0x4E00u + (static_cast<uint32_t>(i) * chars + k) % 800u);
            } else {
                text += "字";
            }
        }
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

void loadStressDocument(int paras, int chars) { loadStressDocumentImpl(paras, chars, false); }

/**
 * 同上，但每段用一批不同的常用字 —— 给"字形集合要有规模"的判据用。
 */
void loadStressDocumentDiverse(int paras, int chars) {
    loadStressDocumentImpl(paras, chars, true);
}

/**
 * 用一段真实文本造文档：按换行切段，每段一个块，整体重复 repeat 遍。
 *
 * 判据要用真文本才准：合成文本（一批码点循环）的字形集合与用字分布都不像真病历，
 * 量出来的缓存账会偏。文本由宿主读进来并清洗掉 markdown 记号后传进来。
 */
void loadTextDocument(const std::string& text, int repeat) {
    if (text.empty()) return;
    if (repeat < 1) repeat = 1;

    std::vector<std::string> paras;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        if (nl > pos) paras.emplace_back(text, pos, nl - pos);
        pos = nl + 1;
    }
    if (paras.empty()) return;

    std::vector<BlockSpec> blocks;
    blocks.reserve(paras.size() * static_cast<size_t>(repeat));
    for (int r = 0; r < repeat; ++r) {
        for (const std::string& t : paras) {
            BlockSpec b;
            b.font_index = g_cjk_font;
            b.size_pt = 12.0f;
            b.wm = WritingMode::HorizontalTB();
            b.line_factor = 1.5f;
            b.language = "zh";
            b.color = Color::rgb(24, 24, 24);
            b.setText(t);
            blocks.push_back(std::move(b));
        }
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
    o << "\"bandY0\":" << e.band_y0 << ",";
    o << "\"bandY1\":" << e.band_y1 << ",";
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
    // 这一轮走了哪条重建路径，以及分段局部重建被第几条判据否决 —— 只报耗时的话，
    //   抖动能把"走到了局部路径但没省下来"与"根本没走到"混成同一个读数。
    o << "\"rebuildPath\":" << e.rebuild_path << ",";
    o << "\"rebuildReject\":" << e.rebuild_reject << ",";
    o << "\"pageFpMs\":" << e.page_fp_ms << ",";
    o << "\"paintMs\":" << e.paint_ms << ",";
    o << "\"totalMs\":" << e.total_ms << ",";
    o << "\"caretPara\":" << e.caret_para << ",";
    o << "\"caretOffset\":" << e.caret_offset << ",";
    // 字体按需读的账在这里也带一份：探针取的是编辑统计，排版口那份（statsJson）
    //   它读不到。这些数实时取，不用 EngineStats 的快照 —— 快照是排版结束那一刻的，
    //   而字形光栅化发生在排版之后，正是按需读的大头，用快照会把它整个漏掉。
    const EngineStats& es = g_engine.stats();
    o << "\"glyphCount\":" << es.glyph_count << ",";
    o << "\"missingGlyphCount\":" << es.missing_glyph_count << ",";
    const FontCacheTotals fc = g_engine.fonts().cacheTotals();
    o << "\"fontCacheHits\":" << fc.hits << ",";
    o << "\"fontCacheMisses\":" << fc.misses << ",";
    o << "\"fontStorageReads\":" << fc.storage_reads << ",";
    o << "\"fontStorageBytes\":" << fc.storage_bytes << ",";
    o << "\"fontIoMs\":" << fc.io_ms << ",";
    o << "\"fontResidentBytes\":" << fc.resident_bytes << ",";
    o << "\"fontStorageFailures\":" << fc.read_failures << ",";
    o << "\"fontStorageEofs\":" << fc.eof_reads << ",";
    o << "\"caretIndex\":" << e.caret_index << ",";
    o << "\"docCodepoints\":" << e.doc_codepoints << ",";
    o << "\"caretSide\":" << e.caret_side << ",";
    // 竖排标记：宿主靠它把方向键按物理方向映射（竖排上下走字、左右换列）。
    o << "\"caretVertical\":" << (e.caret_vertical ? "true" : "false") << ",";
    // 列序：中日竖排往左换列、蒙文往右换列。只给"是竖排"，左右就会按反。
    o << "\"caretBlockReversed\":" << (e.caret_block_reversed ? "true" : "false") << ",";
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
    emscripten::function("loadFontFromFile", &loadFontFromFile);
    emscripten::function("setFontCacheConfig", &setFontCacheConfig);
    emscripten::function("setFontRoles", &setFontRoles);

    emscripten::function("setScenario", &setScenario);
    emscripten::function("setZoom", &setZoom);
    emscripten::function("setPageMetrics", &setPageMetrics);
    emscripten::function("setPageGap", &setPageGap);
    emscripten::function("setContinuous", &setContinuous);
    emscripten::function("contentPx", &contentPx);
    emscripten::function("setUndoLimit", &setUndoLimit);
    emscripten::function("undoLimit", &undoLimit);
    emscripten::function("setCaretStyle", &setCaretStyle);
    emscripten::function("setSelectionStyle", &setSelectionStyle);
    emscripten::function("caretStyleJson", &caretStyleJson);
    emscripten::function("selectionStyleJson", &selectionStyleJson);
    emscripten::function("prewarmPageGlyphs", &prewarmPageGlyphs);

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
    emscripten::function("caretToDocStart", &caretToDocStart);
    emscripten::function("caretToDocEnd", &caretToDocEnd);
    emscripten::function("moveCaretVertical", &moveCaretVertical);
    emscripten::function("selectAll", &selectAll);
    emscripten::function("setSelection", &setSelection);
    emscripten::function("clearSelection", &clearSelection);
    emscripten::function("selectionReport", &selectionReport);
    emscripten::function("setCaret", &setCaret);
    emscripten::function("setCaretAt", &setCaretAt);
    emscripten::function("setCaretAtSide", &setCaretAtSide);
    emscripten::function("setCaretAtSideLine", &setCaretAtSideLine);
    emscripten::function("setFocusAtSideLine", &setFocusAtSideLine);
    emscripten::function("setFocusAtSide", &setFocusAtSide);
    emscripten::function("showCaret", &showCaret);
    emscripten::function("present", &present);
    emscripten::function("caretOffsetLive", &caretOffsetLive);
    emscripten::function("caretVerticalNow", &caretVerticalNow);
    emscripten::function("caretBlockReversedNow", &caretBlockReversedNow);
    emscripten::function("hitTest", &hitTest);
    emscripten::function("editStatsJson", &editStatsJson);
    emscripten::function("caretReport", &caretReport);
    emscripten::function("documentText", &documentText);
    emscripten::function("undoEdit", &undoEdit);
    emscripten::function("redoEdit", &redoEdit);

    // 表单（控件是区间 + 编辑策略）
    emscripten::function("setFormControls", &setFormControls);
    emscripten::function("setEditMode", &setEditMode);
    emscripten::function("editMode", &editMode);
    emscripten::function("setBracketScope", &setBracketScope);
    emscripten::function("bracketScope", &bracketScope);
    emscripten::function("removeControlAt", &removeControlAt);
    emscripten::function("setControlPropAt", &setControlPropAt);
    emscripten::function("focusControl", &focusControl);
    emscripten::function("tabCaret", &tabCaret);
    emscripten::function("auditFrame", &auditFrame);
    emscripten::function("auditBase", &auditBase);
    emscripten::function("auditLayout", &auditLayout);
    emscripten::function("auditGlyphPages", &auditGlyphPages);
    emscripten::function("auditCompile", &auditCompile);
    emscripten::function("takeBlitBox", &takeBlitBoxJson);
    emscripten::function("indent", &indentText);
    emscripten::function("outdent", &outdentText);
    emscripten::function("controlAtPoint", &controlAtPoint);
    emscripten::function("controlsJson", &controlsJson);
    emscripten::function("bracketSlotsJson", &bracketSlotsJson);
    emscripten::function("linesJson", &linesJson);
    emscripten::function("describeControls", &describeControls);
    emscripten::function("blockedEdits", &blockedEdits);
    emscripten::function("loadFormSample", &loadFormSample);
    emscripten::function("loadStressDocument", &loadStressDocument);
    emscripten::function("loadStressDocumentDiverse", &loadStressDocumentDiverse);
    emscripten::function("loadTextDocument", &loadTextDocument);

    emscripten::function("frameWidth", &frameWidth);
    emscripten::function("frameHeight", &frameHeight);
    emscripten::function("frameByteSize", &frameByteSize);
    emscripten::function("frameRgba", &frameRgba);

    emscripten::function("setViewportAnchor", &setViewportAnchor);
    emscripten::function("windowFirstPage", &windowFirstPage);
    emscripten::function("frameSlots", &frameSlots);
    emscripten::function("setFrameSlots",
                         +[](int n) { g_engine.setFrameSlots(n); });
    emscripten::function("windowPageCount", &windowPageCount);
    emscripten::function("pagePxWidth", &pagePxWidth);
    emscripten::function("pagePxHeight", &pagePxHeight);
    emscripten::function("pageGapPx", &pageGapPx);
    emscripten::function("dirtyPagesJson", &dirtyPagesJson);

    emscripten::function("statsJson", &statsJson);
    emscripten::function("buildStamp", &buildStamp);
    emscripten::function("shapeReport", &shapeReport);
}
