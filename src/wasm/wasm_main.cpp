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

#include "core/doc/para_format.h"
#include "core/doc/record_json.h"
#include "core/engine/engine.h"
#include "core/engine/payload_schema.h"
#include "core/rev/fragment.h"
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

/**
 * 已注册的字体清单：宿主拿它填字体下拉框，也拿它把"族名"换成下标。
 */
std::string fontsJson() { return g_engine.fontsJson(); }

/**
 * 三个角色指向哪张脸。负数是"这一档没给"：正文回落到第一张，蒙文跟随正文，
 *   拉丁关掉回退 —— 只装一份字体也能排版，不必凑齐三条。
 */
void setFontRoles(int cjk_index, int mongol_index, int latin_index) {
    g_cjk_font = cjk_index >= 0 ? cjk_index : 0;
    g_mn_font = mongol_index >= 0 ? mongol_index : g_cjk_font;
    g_latin_font = latin_index >= 0 ? latin_index : -1;
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
        b.line_factor = 1.275f;
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
            b.line_factor = 1.3f;
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
            b.line_factor = 1.175f;
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
            b.line_factor = 1.25f;
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
            b.line_factor = 1.3f;
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
            b.line_factor = 1.275f;
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
            b.line_factor = 1.275f;
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
            b2.line_factor = 1.275f;
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
    g_engine.setDocument(g_table_demo ? demoTableTree(g_cjk_font) : demoTree(g_blocks, g_fields),
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
/**
 * 页度量：页宽、页高、四边页边距（都是磅）。
 *
 * 纸张规格（A4 / A3 / Letter……）不在这里认：规格只是"一组宽高"，抄进引擎就是第二个
 *   真相。宿主按规格算出宽高再调用这一条，换纸就是换这四个数。
 */
void setPageMetrics(float w_pt, float h_pt, float top_pt, float right_pt, float bottom_pt,
                    float left_pt) {
    g_cfg.page.width_pt = w_pt;
    g_cfg.page.height_pt = h_pt;
    g_cfg.page.margins = PageMargins{top_pt, right_pt, bottom_pt, left_pt};
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
 * 走引擎的 setCaretAndSelectionStyle 而不是 applyConfig：后者会改光栅签名，
 *   页位图整份作废，窗口里每一页都要重新光栅（3 页约 32 ms）—— 取色器连续改时每一下都付。
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
    return pk::checked(pk::caret_style::kSpec, o.str());
}

/**
 * 选区样式的现状。形状与 caretStyleJson 一致，少一个宽度。
 */
std::string selectionStyleJson() {
    const Color& s = g_engine.config().selection_color;
    std::ostringstream o;
    o << "{\"color\":[" << static_cast<int>(s.r) << "," << static_cast<int>(s.g) << ","
      << static_cast<int>(s.b) << "," << static_cast<int>(s.a) << "]}";
    return pk::checked(pk::selection_style::kSpec, o.str());
}

/**
 * 预热一页的字形：只把 mask 光栅进缓存，画面不动、也不呈现。
 *
 * 宿主在滚动停下之后的空闲帧里调用它，把"下一页的字形"这笔钱先付掉 ——
 *   换窗那一拍于是只剩铺底色与帧搬运（见 EditorEngine::prewarmPageGlyphs）。
 * 返回本次预热触及的字形数，宿主据此判断"这一页还有没有要预热的"。
 */
int prewarmPageGlyphs(int page) { return g_engine.prewarmPageGlyphs(page); }

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

/**
 * 窗口导出图（内容 + 瞬态，窗口顺序拼一张）。给探针与诊断用：
 * 它现场拼一次，宿主上屏走的是 pageRgba / pageDirtyRects —— 那条路不需要窗口帧。
 */
val frameRgba() {
    return val(emscripten::typed_memory_view(g_engine.frameBytes(), g_engine.frame()));
}

/**
 * 一页"画到画布上应该是什么"（页内坐标，RGBA），零拷贝视图：内容 + 这一页的瞬态。
 *
 * 宿主把它整页或按脏矩形贴到该页的画布上 —— 页的位置由宿主按 (页号 x 步距) 定，
 *   引擎不再有槽，也不再拼"窗口帧"给宿主用。
 * 页不在缓存里或已过期时给一个空视图 —— 它不负责补一次光栅：
 *   补光栅会把字形账算到一次读命令头上，而账是"这一拍排了多少"的证据。宿主在
 *   present 之后读它，那时窗口内该光栅的页都已经光栅过了。
 */
val pageRgba(int page) {
    const size_t n = g_engine.pageBytes(page);
    if (n == 0) return val(emscripten::typed_memory_view(static_cast<size_t>(0), g_engine.frame()));
    return val(emscripten::typed_memory_view(n, g_engine.pagePixels(page)));
}

/**
 * 视口锚点：呈现只保证 [first, first+count) 这几页是新鲜的。count <= 0 表示整篇。
 * 宿主按"可见页 ± 余量"设它 —— 窗口越短，每拍要维护的页位图越少、脏矩形也越少。
 *
 * 不调它 = 整篇。长文档上差别是一个量级：2000 段实测每实例堆 82.6 MB（窗口 3 页）
 * 对 1031.6 MB（整篇），装载 115 ms 对 1583 ms。数字与拆解见该函数的注释。
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
 * 一页的脏矩形（页内像素坐标），读后清零。
 *
 * 宿主按这张清单决定 putImageData 哪几块：光标移动只传几十像素，不再是整页 1 Mpx。
 * 内容重光栅与瞬态（光标 / 选区 / 标记）都算在里面，帧的重建不算 —— 那只是落点变了。
 */
std::string pageDirtyRectsJson(int page) { return g_engine.takePageDirtyRectsJson(page); }

/**
 * 还欠着脏矩形的页号（升序）。宿主用它决定这一拍要贴哪几页 ——
 *   不能用 dirtyPages（那是"本拍写过哪几页"，矩形却是累积的）。
 */
std::string pageDirtyPagesJson() { return g_engine.pageDirtyPagesJson(); }

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
    //   其余是记账与差分。合成一个 paintMs 就只能得出"绘制慢"。
    o << "\"paintFillMs\":" << s.paint_fill_ms << ",";
    o << "\"paintGlyphMs\":" << s.paint_glyph_ms << ",";
    // 含在 paintGlyphMs 里的一笔：只为记账取字形包围盒。它不画像素，
    //   判"字形贵不贵"时必须先把它减掉，否则会把记账算成光栅。
    o << "\"paintGlyphBoundsMs\":" << s.paint_glyph_bounds_ms << ",";
    o << "\"paintedGlyphs\":" << s.painted_glyphs << ",";
    o << "\"paintPresentMs\":" << s.paint_present_ms << ",";
    // 残差那五段的分解。残差 = 总耗时减掉铺面积与字形，它装的是入口准备、内容光栅的
    //   杂项、瞬态差分、脏矩形记账、光标与上报 —— 合成一个数就只能猜该动哪一块。
    //   注意写页位图那两段（铺面积、字形）包在 paintSwapMs 里面，不是与它并列。
    o << "\"paintPreMs\":" << s.paint_pre_ms << ",";
    o << "\"paintSwapMs\":" << s.paint_swap_ms << ",";
    o << "\"paintTransientMs\":" << s.paint_transient_ms << ",";
    o << "\"paintOverlayMs\":" << s.paint_overlay_ms << ",";
    o << "\"paintLedgerMs\":" << s.paint_ledger_ms << ",";
    o << "\"paintReportMs\":" << s.paint_report_ms << ",";
    o << "\"totalMs\":" << s.total_ms << ",";
    o << "\"frameWidth\":" << s.frame_width << ",";
    o << "\"frameHeight\":" << s.frame_height << ",";
    o << "\"frameBytes\":" << g_engine.frameBytes() << ",";
    // 预算与占用（字节）：占用贴着上限就说明缓存随时会被整份抹掉 —— 那时
    //   "预热"与"命中"都无从谈起，而面板上只看命中率是看不出来的。
    o << "\"glyphCacheBytes\":" << s.glyph_cache_bytes << ",";
    o << "\"glyphCacheLimit\":" << s.glyph_cache_limit << ",";
    o << "\"zoom\":" << s.zoom;
    o << "}";
    return pk::checked(pk::stats::kSpec, o.str());
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
    return pk::checked(pk::bench_relayout_paint::kSpec, o.str());
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

/**
 * 在指定位置插入一段文本（模板填充）。与 typeText 一样自带重排与重绘，
 *   所以命令表里它的呈现归属是 'self'。
 */
bool insertTextAt(int para, int offset, const std::string& utf8) {
    return g_engine.insertTextAndRender(para, static_cast<size_t>(offset), utf8);
}
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

/**
 * 整格选区。段号来自 hitTest 的 cellPara 字段 —— 宿主原样搬回来，不自己算几何。
 * 与 setSelection 的区别只在粒度：它是"单元格选区"，渲染铺满整格（含内边距与显式行高）。
 */
bool selectCellAtPara(int para) { return g_engine.selectCellAtParaAndRender(para); }

/**
 * 按点选整格（三击）。点用文档坐标（磅），与 hitTest 同一个坐标系。
 * 点在表格之外返回 false，宿主据此退回落光标。
 */
bool cellSelectAt(float x_pt, float y_pt) {
    if (!g_engine.selectCellAtPt(x_pt, y_pt)) return false;
    g_engine.repaint();
    return true;
}

/**
 * 双击选词。点用文档坐标（磅），与 hitTest 同一个坐标系。
 * 落点不在正文上、或那一段是空的时返回 false，宿主据此退回落光标。
 */
bool selectWordAt(float x_pt, float y_pt) { return g_engine.selectWordAtPtAndRender(x_pt, y_pt); }

/**
 * 三击选段（表格内退化成选整格）。返回 false 表示没得选，宿主退回落光标。
 */
bool selectParaAt(float x_pt, float y_pt) { return g_engine.selectParaAtPtAndRender(x_pt, y_pt); }

/**
 * 把当前选区搬到点上（一次事务，撤销一步）。
 */
bool moveSelectionTo(float x_pt, float y_pt) {
    return g_engine.moveSelectionToPtAndRender(x_pt, y_pt);
}

/**
 * 拖动搬运的落点指示：这一点上松手会插在哪（页内磅）。只读，不动光标与选区。
 */
std::string dropSpot(float x_pt, float y_pt) { return g_engine.dropSpotJson(x_pt, y_pt); }

/**
 * 把一张图片搬到点上（一次事务，撤销一步）。两个坐标是落点，dx / dy 是拖动位移。
 */
bool moveImageTo(int index, float x_pt, float y_pt, int relative, float dx_pt, float dy_pt) {
    return g_engine.moveImageToPtAndRender(index, x_pt, y_pt, relative != 0, dx_pt, dy_pt);
}

/**
 * 改图片版式：0 行内、1 四周型、2 上下型、3 衬于文字下方、4 浮于文字上方。
 */
bool setImageLayout(int index, int layout) {
    return g_engine.setImageLayoutAndRender(index, layout);
}
/**
 * 段内位置 -> 全局流位置（码点）。
 *
 * 选区两端（setSelection）收的是全局流位置，而外部 API 收的是 { 段号, 段内偏移 }：
 *   两者之间的换算只在引擎里做 —— 段间分隔符算不算一个位置、表格格里怎么数，
 *   这些口径不该由宿主猜（猜错了只在跨段选区上显形）。
 */
int flowIndexAt(int para, int offset) {
    if (offset < 0) offset = 0;
    return static_cast<int>(g_engine.flowPosAt(para, static_cast<size_t>(offset)));
}

/**
 * 全局流位置 -> { 段号, 段内偏移 }。
 *
 * 与 flowIndexAt 是同一件事的两个方向。选区两端在引擎里是流位置（setSelection 收的
 *   就是它），而外部 API 收的是段内位置 —— 这个换算只能由引擎做：段间分隔符算不算
 *   一个位置、表格格里怎么数，宿主猜不出来。
 */
std::string flowTextPosJson(int flow) {
    TextPos p{};
    if (!g_engine.flowToTextPos(static_cast<FlowPos>(flow), &p)) return "null";
    std::ostringstream o;
    o << "{\"para\":" << p.para << ",\"offset\":" << p.offset << "}";
    return pk::checked(pk::flow_text_pos::kSpec, o.str());
}

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
    return pk::checked(pk::selection_report::kSpec, o.str());
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
 * clamp 非零时把落在页外/页间空隙的点夹到最近的页边再算：拖选那一路用 —— 指针拖出画布
 *   之后仍要能选，单击那一路不夹（点在灰底上本来就不该挪光标）。
 */
std::string hitTest(float x_pt, float y_pt, int clamp) {
    TextPos p{};
    int owner = -1;
    int side = 0;
    int line_aff = 0;
    int cell_para = -1;
    // 图片先算：点在图或把手上时宿主不落光标，而是选中它（把手命中还要起拖）。
    //   与落点共用同一份坐标，所以不进 hitTestPt 的两个出口，另起两个键带出去。
    const EditorEngine::ImageHit img = g_engine.imageAtPt(x_pt, y_pt);
    if (!g_engine.hitTestPt(x_pt, y_pt, &p, &owner, &side, &line_aff, &cell_para, clamp != 0)) {
        if (img.index < 0) return pk::checked(pk::hit_test::kSpec, "null");
        std::ostringstream bad;
        bad << "{\"para\":-1,\"offset\":0,\"field\":-1,\"side\":0,\"line\":0,\"img\":" << img.index
            << ",\"imgHandle\":" << img.handle;
        // 没有文字落点，但"按下去能不能拖"照样要回答（搬图那一半）。
        TextPos none{};
        none.para = -1;
        const int dk = g_engine.dragKindAt(none, 0, img);
        if (dk > 0) {
            bad << ",\"drag\":" << dk << ",\"dragImage\":" << img.index;
        }
        bad << "}";
        return pk::checked(pk::hit_test::kSpec, bad.str());
    }
    std::ostringstream o;
    o << "{\"para\":" << p.para << ",\"offset\":" << p.offset << ",\"field\":" << owner
      << ",\"side\":" << side << ",\"line\":" << line_aff;
    // cellPara：落点在某个格子的边距带上（格边框到文字左沿之间，空格则是整格）时给出
    //   这一格的段号。宿主看到它就调 selectCellAtPara —— 那是"选整格"这条操作线，
    //   与"落光标"不是同一件事。给段号而不是流区间：空格的内容长度为 0，它的流区间
    //   与下一格的第一段是同一个位置，按流区间分不出是哪一格。
    // 组控件：这一点落在第几个选项上。宿主拿它决定切换哪一项（互斥与多选规则在引擎里）。
    const int option = (owner >= 0) ? g_engine.controlOptionAtPos(p, side) : -1;
    if (option >= 0) o << ",\"opt\":" << option;
    if (cell_para >= 0) {
        o << ",\"cellPara\":" << cell_para;
        // 顺带给出这一格的内容区间：空格的两端相等（长度 0）。判据拿它区分"有内容"与
        //   "空格"两种整格选区；宿主不用它 —— 选格只需要段号。
        FlowPos lo = -1;
        FlowPos hi = -1;
        if (g_engine.cellFlowRangeAt(cell_para, &lo, &hi)) {
            o << ",\"cell\":[" << lo << "," << hi << "]";
        } else {
            o << ",\"cell\":[" << 0 << "," << 0 << "]";
        }
    }
    if (img.index >= 0) {
        o << ",\"img\":" << img.index << ",\"imgHandle\":" << img.handle;
    }
    // 按下这一下还要回答两件事，而且必须与上面那份落点出自同一次命中：
    //   能不能开始拖拽移动（drag / dragImage），以及是不是压在表格内部的格线上
    //   （gridTable / gridCol / gridRow）。分成三条命令问，宿主就要三次往返才轮得到
    //   落光标 —— 拖选与图片把手都来不及起手（按下到第一次移动只隔几毫秒）。
    const int drag_kind = g_engine.dragKindAt(p, side, img);
    if (drag_kind > 0) {
        o << ",\"drag\":" << drag_kind << ",\"dragImage\":" << img.index;
    }
    uint64_t grid_table = 0;
    int grid_col = 0;
    int grid_row = -1;
    if (g_engine.tableGridLineAt(x_pt, y_pt, &grid_table, &grid_col, &grid_row)) {
        o << ",\"gridTable\":" << grid_table << ",\"gridCol\":" << grid_col
          << ",\"gridRow\":" << grid_row;
    }
    o << "}";
    return pk::checked(pk::hit_test::kSpec, o.str());
}

/**
 * 带侧别与行别的定位（点画布的落点）。side 与 line 都来自 hitTest 的同一份结果。
 * 宿主不要自己拼这两个数：它只该原样搬运引擎算出来的结论。
 */
void setCaretAtSideLine(int para, int offset, int side, int line_aff) {
    g_engine.setCaretAtSideLine(para, static_cast<size_t>(offset < 0 ? 0 : offset), side, line_aff);
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
    g_engine.setFocusAtSideLine(para, static_cast<size_t>(offset < 0 ? 0 : offset), side, line_aff);
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
 * 显示 / 不显示表单括号（默认显示）。
 *
 * 括号进文档流（占宽度），所以改它会重排 —— setConfig 内部已经做了，宿主不必自己
 * 再排一次；下一次 present 的脏矩形自然会覆盖被挪动的正文。
 *
 * 只有"全显示 / 全不显示"两档：不做"只显示光标所在字段"那一档 —— 它会让跨字段
 * 移动光标变成整行重排，与"括号常显"的口径也打架。
 */
void setShowBrackets(bool on) {
    EngineConfig c = g_cfg;
    c.show_brackets = on;
    g_cfg = c;
    applyConfig();
}
bool showBrackets() { return g_cfg.show_brackets; }

/**
 * 小点（控件右下角的方块）。独立开关，开启时所有控件都画，与焦点无关。
 * 它不占宽度、不进文档流，所以改它不重排 —— 只让页位图重光栅（见 rasterSignature）。
 */
void setShowControlDots(bool on) {
    EngineConfig c = g_cfg;
    c.show_control_dots = on;
    g_cfg = c;
    applyConfig();
}
bool showControlDots() { return g_cfg.show_control_dots; }

/**
 * 不可见字符标记的档位：0 不显示 / 1 只显示段落标记 / 2 连空格与制表符一起显示。
 *
 * 标记不占宽度、不进流位置，改它不重排 —— 只让页位图重光栅（见 rasterSignature）。
 * 档位越界时按"只显示段落标记"处理：它是默认档，也是"看得见段落边界"这一档。
 */
void setShowMarks(int mode) {
    EngineConfig c = g_cfg;
    c.marks = (mode < 0 || mode > 2) ? MarksMode::Paragraph : static_cast<MarksMode>(mode);
    g_cfg = c;
    applyConfig();
}
int showMarks() { return static_cast<int>(g_cfg.marks); }

/**
 * 页边距裁剪标记（正文四角的短角线）。默认不画。
 *
 * 它只是页位图上的一层辅助线：不占宽度、不进流位置，改它不重排 —— 只让页位图重光栅
 * （见 rasterSignature）。连页下引擎自己不画：那里没有页，四角没有意义。
 */
void setShowMarginMarks(bool on) {
    EngineConfig c = g_cfg;
    c.show_margin_marks = on;
    g_cfg = c;
    applyConfig();
}
bool showMarginMarks() { return g_cfg.show_margin_marks; }

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
 * 插一张空表格：rows 行、每行 cols 格，header 把首行标成标题行。
 *
 * 表格落在光标所在段之后（块级元素不进段落内部），光标随之落进第一格。只在文档模式
 * 放行 —— 表单模式在填值、只读模式不许改，两者都不该长出新表。
 */
bool insertTableAt(int rows, int cols, bool header) {
    return g_engine.insertTableAndRender(rows, cols, header);
}

/**
 * 把宿主给的 opts 读成一份"要改的几项"。
 *
 * 插入与改定义共用它：两处各写一份解析，字段一多必然漂移（插入认 format、改定义不认），
 * 而那种漂移在宿主侧只表现为"某一项配了没生效"。没给的键保持 nullopt（= 这一项不动）。
 * @param o opts（键：key / label / options / group / free / format / units / type）
 * @return 补丁
 */
ControlPatch controlPatchFrom(val o) {
    ControlPatch p;
    if (!o["key"].isUndefined()) p.key = o["key"].as<std::string>();
    if (!o["label"].isUndefined()) p.label = o["label"].as<std::string>();
    if (!o["type"].isUndefined()) p.expect_type = o["type"].as<std::string>();
    if (o["options"].isArray()) {
        std::vector<ControlOption> options;
        const int n = o["options"]["length"].as<int>();
        for (int i = 0; i < n; ++i) {
            const val item = o["options"][i];
            ControlOption opt;
            if (!item["v"].isUndefined()) opt.value = item["v"].as<std::string>();
            if (!item["l"].isUndefined()) opt.label = item["l"].as<std::string>();
            options.push_back(std::move(opt));
        }
        p.options = std::move(options);
    }
    if (!o["group"].isUndefined()) p.group = o["group"].as<std::string>();
    if (!o["ph"].isUndefined()) p.placeholder = o["ph"].as<std::string>();
    if (!o["free"].isUndefined()) p.free_input = o["free"].as<bool>();
    if (!o["format"].isUndefined()) p.format = o["format"].as<std::string>();
    if (!o["units"].isUndefined()) p.units = static_cast<uint16_t>(o["units"].as<int>());
    if (o["glyph"].isArray() && o["glyph"]["length"].as<int>() >= 3) {
        // 记号色：[红, 绿, 蓝, 不透明度]。不给 alpha 就是不透明 —— 少写一个通道不该
        //   意外变成"全透明 = 跟随文字"。
        Color c;
        c.r = static_cast<uint8_t>(o["glyph"][0].as<int>());
        c.g = static_cast<uint8_t>(o["glyph"][1].as<int>());
        c.b = static_cast<uint8_t>(o["glyph"][2].as<int>());
        c.a = o["glyph"]["length"].as<int>() >= 4 ? static_cast<uint8_t>(o["glyph"][3].as<int>())
                                                  : 255;
        p.glyph_color = c;
    }
    return p;
}

/**
 * 跑一条表格结构命令。op 与 EditorEngine::TableOp 同序：
 * 0 上方插行 / 1 下方插行 / 2 删行 / 3 左侧插列 / 4 右侧插列 / 5 删列 / 6 合并 / 7 拆分 /
 * 8 删整表。
 *
 * 光标不在表格里、模式不对、或那一格本来就不支持这个动作（删最后一列、合并区域不成立）
 * 时整条命令被拒并返回 false —— 文档一个字节都不动。
 */
bool tableOpAt(int op) {
    if (op < 0 || op > 8) return false;
    return g_engine.tableOpAndRender(static_cast<EditorEngine::TableOp>(op));
}

/**
 * 把宿主给的 opts 读成一份格记录的补丁。
 * @param o opts（键：vAlign，0 顶端 / 1 居中 / 2 底端）
 * @return 补丁
 */
CellPatch cellPatchFrom(val o) {
    CellPatch p;
    if (!o["vAlign"].isUndefined()) {
        const int v = o["vAlign"].as<int>();
        if (v >= 0 && v <= 2) p.v_align = static_cast<CellVAlign>(v);
    }
    return p;
}

/**
 * 把宿主给的 opts 读成一份行记录的补丁。没给的键保持 nullopt（= 这一项不动）。
 * @param o opts（键：header / split / breakBefore / heightPt / heightRule）
 * @return 补丁
 */
RowPatch rowPatchFrom(val o) {
    RowPatch p;
    if (!o["header"].isUndefined()) p.is_header = o["header"].as<bool>();
    if (!o["split"].isUndefined()) p.can_split_by_page = o["split"].as<bool>();
    if (!o["breakBefore"].isUndefined()) p.page_break_before = o["breakBefore"].as<bool>();
    if (!o["heightPt"].isUndefined()) p.height_pt = static_cast<float>(o["heightPt"].as<double>());
    if (!o["heightRule"].isUndefined()) {
        const int r = o["heightRule"].as<int>();
        if (r >= 0 && r <= 2) p.height_rule = static_cast<HeightRule>(r);
    }
    return p;
}

/**
 * 把宿主给的 opts 读成一份表记录的补丁。
 * @param o opts（键：widthPt / align，align 与 Align 同序：0 左 / 1 中 / 2 右）
 * @return 补丁
 */
TablePatch tablePatchFrom(val o) {
    TablePatch p;
    if (!o["widthPt"].isUndefined()) p.width_pt = static_cast<float>(o["widthPt"].as<double>());
    if (!o["align"].isUndefined()) {
        const int a = o["align"].as<int>();
        if (a >= 0 && a <= 2) p.align = static_cast<Align>(a);
    }
    return p;
}

/**
 * 改光标所在格的记录（目前是垂直对齐）。
 */
bool setCellSpecAt(val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    return g_engine.setCellSpecAndRender(cellPatchFrom(o));
}

/**
 * 改光标所在行的记录（标题行、跨页断行、行前分页、行高）。
 */
bool setRowSpecAt(val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    return g_engine.setRowSpecAndRender(rowPatchFrom(o));
}

/**
 * 改光标所在表格的记录（宽度、水平位置）。
 */
bool setTableSpecAt(val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    return g_engine.setTableSpecAndRender(tablePatchFrom(o));
}

/**
 * 光标所在表格的现状（表 / 行 / 格三层记录 + 整格选区范围）。
 */
std::string tableStateJson() { return g_engine.tableStateJson(); }

/**
 * 表格栅格（拖拽列边界与行边界用）。点用文档坐标（磅）。
 */
std::string tableGrid(float x, float y) { return g_engine.tableGridJson(x, y); }

/**
 * 拖拽结束：整份列宽落地。widths 是磅值数组（按网格列序），<= 0 的那一列保持自动。
 */
bool setTableColWidths(int handle, val widths) {
    std::vector<float> ws;
    if (!widths.isUndefined() && !widths.isNull()) {
        const int n = widths["length"].as<int>();
        ws.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) ws.push_back(widths[i].as<float>());
    }
    return g_engine.setTableColWidthsAndRender(static_cast<uint64_t>(handle), ws);
}

/**
 * 拖拽结束：改一行的高度（磅）。<= 0 表示回到按内容。
 */
bool setTableRowHeight(int handle, int row, float h_pt) {
    return g_engine.setTableRowHeightAndRender(static_cast<uint64_t>(handle), row, h_pt);
}

/**
 * 一个点上压着什么（右键菜单的判定）：文档坐标（磅）进，JSON 出。
 */
std::string contextTargetJson(float x, float y) { return g_engine.contextTargetJson(x, y); }

/**
 * 插一个内容控件（表单字段），落在光标处，label 是它的初始内容。
 *
 * opts 的键都可省：type 是业务类型（默认 text）、key 是业务编码、label 是内容、
 * ro 是只读、req 是必填、shape 是值形态（0 文本 / 1 布尔 / 2 枚举 / 3 列表 / 4 资源）。
 * 只在文档模式放行（理由同上）。
 */
bool insertControlAt(val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    // 类型决定特有层：这里的 opts 只需要说"要哪一种"，默认值由 makeControlSpec 给。
    const std::string type =
        o["type"].isUndefined() ? std::string("text") : o["type"].as<std::string>();
    ControlSpec spec = makeControlSpec(type);
    ControlPatch patch = controlPatchFrom(o);
    // 插入时的 label 是"内容初值"，走 insertControlAndRender 的第二个参数；记录上的标签
    //   由那一路按类型决定写到哪儿。这里摘掉，免得同一句话写进两个地方。
    patch.label.reset();
    // 类型已经由 makeControlSpec 定了，patch 里那一项只剩"要对齐"的意思。
    patch.expect_type.reset();
    if (!applyControlPatch(&spec, patch, nullptr)) return false;
    if (!o["ro"].isUndefined()) spec.readonly = o["ro"].as<bool>();
    if (!o["req"].isUndefined()) spec.required = o["req"].as<bool>();
    if (!o["shape"].isUndefined()) {
        const int shape = o["shape"].as<int>();
        if (shape >= 0 && shape <= 4) spec.valueShape = static_cast<ValueShape>(shape);
    }
    const std::string label =
        o["label"].isUndefined() ? std::string() : o["label"].as<std::string>();
    return g_engine.insertControlAndRender(spec, label);
}

/**
 * 改一个控件的定义（选项表、标签、互斥组、自由输入、日期格式与单位、业务编码）。
 *
 * 只改 opts 里给出的键：没给的不动。type 给了就必须与当前一致（换类型请删掉重插）。
 * 改完内容按新定义重建；指向已删选项的值一起清掉 —— 都算在同一步撤销里。
 */
bool setControlSpecAt(int index, val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    return g_engine.setControlSpecAndRender(index, controlPatchFrom(o));
}

/**
 * 改一个控件的值。三项都传：flag / text / items —— 值形态决定读哪一项，多传的字段无害
 * （与 ControlValue 的三个字段并存同一个理由：自描述比带标签的联合好读）。
 * 值不合法时引擎拒绝并返回 false，文档不动。
 */
bool setControlValueAt(int index, bool flag, const std::string& text, val items) {
    ControlValue v;
    v.flag = flag;
    v.text = text;
    if (!items.isUndefined() && items.isArray()) {
        const int n = items["length"].as<int>();
        for (int i = 0; i < n; ++i) v.items.push_back(items[i].as<std::string>());
    }
    return g_engine.setControlValueAndRender(index, v);
}

/**
 * 切换一个控件的第 option 个选项（单控件传 -1）。互斥与多选规则在命令层落地。
 */
bool toggleControlAt(int index, int option) {
    return g_engine.toggleControlAndRender(index, option);
}

/**
 * 文档坐标落在组控件的第几个选项上（不在组控件上时 -1）。宿主据此决定点的是哪一项。
 */
int controlOptionAt(float x_pt, float y_pt) { return g_engine.controlOptionAtPt(x_pt, y_pt); }

/**
 * 注册一张图片的像素：宿主解码之后把 RGBA 交上来（文档只存资源 id）。
 *
 * 返回空串表示成功，否则是失败原因 —— 尺寸为零、字节数对不上、超出资源表上限都要说清，
 * 静默失败会让宿主以为图已经能画了。
 */
std::string registerImage(const std::string& id, int w, int h, uintptr_t ptr, size_t len) {
    std::string error;
    const bool ok = g_engine.registerImageAndRepaint(
        id, w, h, reinterpret_cast<const uint8_t*>(ptr), len, &error);
    if (ok) return std::string();
    return error.empty() ? std::string("图片注册失败") : error;
}

/**
 * 删掉一张图片的像素。文档里对它的引用保留，画面上退回占位框。
 */
bool dropImage(const std::string& id) { return g_engine.dropImageAndRepaint(id); }

/**
 * 文档里全部图片的清单：下标、资源 id、显示尺寸、落页矩形、像素在不在。
 * 宿主拿它画自己的辅助层（缩略图列表、属性面板）。
 */
std::string imageList() {
    const std::vector<EditorEngine::ImageBox> all = g_engine.images();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"count\":" << g_engine.imageCount() << ",\"bytes\":" << g_engine.imageBytes()
      << ",\"images\":[";
    for (size_t i = 0; i < all.size(); ++i) {
        if (i) o << ",";
        o << "{\"index\":" << all[i].index << ",\"node\":" << all[i].id << ",\"res\":\""
          << all[i].resource_id << "\",\"w\":" << all[i].w_pt << ",\"h\":" << all[i].h_pt
          << ",\"ready\":" << (all[i].resource_ready ? "true" : "false")
          << ",\"layout\":" << all[i].layout << ",\"rects\":[";
        for (size_t k = 0; k < all[i].rects.size(); ++k) {
            if (k) o << ",";
            o << "{\"page\":" << all[i].rects[k].page << ",\"x\":" << all[i].rects[k].r_pt.x
              << ",\"y\":" << all[i].rects[k].r_pt.y << ",\"w\":" << all[i].rects[k].r_pt.w
              << ",\"h\":" << all[i].rects[k].r_pt.h << "}";
        }
        o << "]}";
    }
    o << "]}";
    return pk::checked(pk::image_list::kSpec, o.str());
}

/**
 * 命中：文档坐标（磅）落在哪张图的哪个把手上。没点到图时 index 与 handle 都是 -1。
 */
std::string imageHit(float x_pt, float y_pt) {
    const EditorEngine::ImageHit hit = g_engine.imageAtPt(x_pt, y_pt);
    std::ostringstream o;
    o << "{\"index\":" << hit.index << ",\"handle\":" << hit.handle << "}";
    return pk::checked(pk::image_hit::kSpec, o.str());
}

/**
 * 选中一张图片（出拖拽框与八个把手）。-1 表示取消选中。
 */
bool selectImage(int index) {
    if (index < 0) return g_engine.clearImageSelectionAndRender();
    return g_engine.selectImageAndRender(index);
}

/**
 * 改一张图片的显示尺寸（磅）：拖把手的落点。
 */
bool resizeImageAt(int index, float w_pt, float h_pt, bool merge) {
    return g_engine.resizeImageAndRender(index, w_pt, h_pt, merge);
}

/**
 * 拖拽改尺寸的预览（不动文档、不重排）：只把瞬态层的框换成"松手会变成多大"。
 * handle 是正在拖的把手（0 左上顺时针），它决定框往哪个方向长。
 */
bool previewImageResizeAt(int index, float w_pt, float h_pt, int handle) {
    return g_engine.previewImageResizeAndRender(index, w_pt, h_pt, handle);
}

/**
 * 拖动浮动图的预览（不动文档、不重排）：图按位移画在新位置，文字松手才绕排。
 * 行内图没有预览：它在文字流里，挪一下就要重排。
 */
bool previewImageMoveAt(int index, float dx_pt, float dy_pt) {
    if (!g_engine.previewImageMove(index, dx_pt, dy_pt)) return false;
    g_engine.repaint();
    return true;
}

/**
 * 收起尺寸预览（松手后由宿主补一次真实 resizeImage；中途取消也走它）。
 *
 * 拖动浮动图的预览改的是版面本身：丢掉它要连带重排，图才回到文档里的位置。
 */
bool clearImagePreview() { return g_engine.clearImagePreviewAndRender(); }

/**
 * 在光标处插一张图片。opts 的键：res（资源 id）、w、h（显示尺寸，磅，可省）、
 * pw、ph（原始像素）、dpi（默认 96）。
 *
 * 尺寸两边都不给时按 pw/ph 与 dpi 折算（96 DPI 是行业惯例）；只给一边时按原图比例推
 * 另一边。res 为空表示先占位、之后由宿主补上资源 —— 画面上是一个带资源标识的空框。
 */
bool insertImageAt(val opts) {
    const val o = (opts.isUndefined() || opts.isNull()) ? val::object() : opts;
    ImageSpec spec;
    if (!o["res"].isUndefined()) spec.resource_id = o["res"].as<std::string>();
    if (!o["pw"].isUndefined()) spec.px_w = o["pw"].as<int>();
    if (!o["ph"].isUndefined()) spec.px_h = o["ph"].as<int>();
    if (!o["dpi"].isUndefined()) spec.dpi = o["dpi"].as<int>();
    if (!o["w"].isUndefined()) spec.width = Length::abs(ptToEmu(o["w"].as<double>()));
    if (!o["h"].isUndefined()) spec.height = Length::abs(ptToEmu(o["h"].as<double>()));
    return g_engine.insertImageAndRender(spec);
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
 * 帧自审：这一帧 vs "整帧重画一遍"，逐字节比对（0 = 一致）。
 * 它只能抓上屏与增量擦除的错，抓不到页位图本身写错（见 auditPage）。
 */
std::string auditFrame() { return g_engine.auditFrameJson(); }

/**
 * 内容自审：页位图按窗口拼起来 vs 从零重画，逐字节比对（0 = 一致）。
 * 与 auditFrame 的分工见 engine.h：那一条比"内容 + 瞬态"，只有内容那一层错时
 *   两条都红；拼装本身错时也只有这一条查得出来。
 */
std::string auditBase() { return g_engine.auditBaseJson(); }

/**
 * 页自审：缓存里第 p 页 vs 从零光栅同一页，逐字节比对（0 = 一致）。
 * 绕开缓存当参照：帧与底图两条自审读的都是缓存，缓存错了它们照样全绿。
 */
std::string auditPage(int page) { return g_engine.auditPageJson(page); }

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
 * Tab / Shift+Tab = 缩进 / 反缩进（不是跳字段）。
 * 走 *AndRender：入口改状态就必须自己收尾，理由见 engine.cpp 里那段说明。
 */
bool indentText() { return g_engine.indentAndRender(); }
bool outdentText() { return g_engine.outdentAndRender(); }
int controlAtPoint(float x_pt, float y_pt) { return g_engine.controlAtPt(x_pt, y_pt); }
std::string describeControls() { return g_engine.describeControls(); }

/**
 * 水印记录（第一节）。宿主用它回填页面设置面板。
 */
std::string watermarkJson() {
    const WatermarkSpec w = g_engine.watermark();
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(2);
    o << "{\"enabled\":" << (w.enabled ? "true" : "false") << ",\"text\":\"" << jsonEscape(w.text)
      << "\",\"sizePt\":" << w.size_pt << ",\"color\":[" << static_cast<int>(w.color.r) << ","
      << static_cast<int>(w.color.g) << "," << static_cast<int>(w.color.b) << ","
      << static_cast<int>(w.color.a) << "],\"angle\":" << w.angle_deg << "}";
    return pk::checked(pk::watermark::kSpec, o.str());
}

/**
 * 改水印。参数逐个给、不走 JSON：这几个值都是标量，拼一份 JSON 只多一层解析。
 * 颜色四个通道原样收（0~255），alpha 就是"半透明"的唯一表达。
 */
bool setWatermark(bool enabled, const std::string& text, float size_pt, int r, int g, int b, int a,
                  float angle_deg) {
    WatermarkSpec w;
    w.enabled = enabled;
    w.text = text;
    w.size_pt = size_pt > 0.0f ? size_pt : 48.0f;
    const auto clamp255 = [](int v) { return static_cast<uint8_t>(std::max(0, std::min(255, v))); };
    w.color = Color{clamp255(r), clamp255(g), clamp255(b), clamp255(a)};
    w.angle_deg = angle_deg;
    return g_engine.setWatermarkAndRender(w);
}

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
    // 类型、值、选项来自控件视图；区间与矩形来自控件集合 —— 两者按同一个下标对齐。
    const std::vector<EditorEngine::ControlView> views = g_engine.controlViews();
    std::vector<const EditorEngine::ControlView*> byIndex(static_cast<size_t>(cs.size()), nullptr);
    for (const EditorEngine::ControlView& v : views) {
        if (v.index >= 0 && v.index < cs.size()) byIndex[static_cast<size_t>(v.index)] = &v;
    }
    for (int i = 0; i < cs.size(); ++i) {
        const Control& c = cs.at(i);
        const EditorEngine::ControlView* v = byIndex[static_cast<size_t>(i)];
        if (i) o << ",";
        o << "{\"i\":" << i << ",\"kind\":\"" << jsonEscape(controlKindName(c.kind))
          << "\",\"type\":\"" << jsonEscape(v != nullptr ? v->controlType : std::string())
          << "\",\"key\":\"" << jsonEscape(v != nullptr ? v->key : std::string()) << "\",\"id\":\""
          << jsonEscape(c.id) << "\",\"label\":\""
          << jsonEscape(v != nullptr ? v->label : std::string()) << "\",\"group\":\""
          << jsonEscape(v != nullptr ? v->group : std::string()) << "\",\"text\":\""
          << jsonEscape(v != nullptr ? v->content : std::string())
          // 提示文字：属性面板要回填它，判据要读"空控件上画的是哪一句"。
          << "\",\"ph\":\"" << jsonEscape(v != nullptr ? v->placeholder : std::string())
          << "\",\"ro\":" << (c.read_only ? "true" : "false") << ",\"req\":"
          << ((v != nullptr && v->required) ? "true" : "false")
          // 日期的格式与可选单位、选择类的记号色：属性面板要按现状回填，缺了就只能显示默认值
          //   （改一次颜色之后面板再打开还是黑的，用户会以为没改上）。
          << ",\"fmt\":\"" << jsonEscape(v != nullptr ? v->format : std::string()) << "\""
          << ",\"du\":" << (v != nullptr ? v->date_units : static_cast<uint16_t>(0))
          << ",\"glyphColor\":[" << (v != nullptr ? static_cast<int>(v->glyph_color.r) : 0) << ","
          << (v != nullptr ? static_cast<int>(v->glyph_color.g) : 0) << ","
          << (v != nullptr ? static_cast<int>(v->glyph_color.b) : 0) << ","
          << (v != nullptr ? static_cast<int>(v->glyph_color.a) : 0) << "]"
          << ",\"multi\":" << ((v != nullptr && v->multi) ? "true" : "false")
          << ",\"free\":" << ((v != nullptr && v->free_input) ? "true" : "false")
          << ",\"flag\":" << ((v != nullptr && v->value.flag) ? "true" : "false") << ",\"sel\":\""
          << jsonEscape(v != nullptr ? v->value.text : std::string()) << "\",\"selItems\":[";
        if (v != nullptr) {
            for (size_t k = 0; k < v->value.items.size(); ++k) {
                if (k) o << ",";
                o << "\"" << jsonEscape(v->value.items[k]) << "\"";
            }
        }
        o << "],\"opts\":[";
        if (v != nullptr) {
            for (size_t k = 0; k < v->options.size(); ++k) {
                if (k) o << ",";
                o << "[\"" << jsonEscape(v->options[k].value) << "\",\""
                  << jsonEscape(v->options[k].label) << "\"]";
            }
        }
        o << "],\"depth\":" << c.depth << ",\"parent\":" << c.parent << ",\"start\":["
          << c.range.start.para << "," << c.range.start.offset << "]"
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
    return pk::checked(pk::controls::kSpec, o.str());
}

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
          << ",\"v\":" << (lb.vertical ? 1 : 0) << ",\"cp\":[" << lb.first_cp << "," << lb.last_cp
          << "],\"blo\":" << lb.block_lo_pt << ",\"bhi\":" << lb.block_hi_pt
          << ",\"iorg\":" << lb.inline_origin_pt << ",\"iend\":"
          << lb.inline_end_pt
          // 格范围（页内磅）：判据要拿它比"选区是不是整格"。不在表格里时两组都是 0。
          //   clo/chi 是内容区（命中用），cxlo/cxhi 是格外框（反白用它，含两侧内边距）。
          << ",\"clo\":" << lb.cell_lo_pt << ",\"chi\":" << lb.cell_hi_pt
          << ",\"cxlo\":" << lb.cell_box_lo_pt << ",\"cxhi\":" << lb.cell_box_hi_pt
          << ",\"ctop\":" << lb.cell_top_pt << ",\"cbot\":" << lb.cell_bottom_pt
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
    return pk::checked(pk::lines::kSpec, o.str());
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
              << ",\"owner\":"
              << g_engine.controlIndexOfOwner(c.owner_control)
              // 行内原子也在这张表里（它同样是"占了一个槽的占位簇"）：kind 0 不是原子，
              //   1 是图片、2 是公式。判据靠它区分"槽是括号还是原子" —— 两者的
              //   side / owner 都是 0 与 -1，光看那两个字段分不开。
              //   空控件的提示文字也在里面：side 0 且 atom 0 —— 括号不会出现 side 0，
              //   所以这一组取值唯一地标识"这是提示文字那一枚槽"（它占宽度不占位置）。
              << ",\"atom\":" << static_cast<int>(c.atom) << ",\"h\":" << c.atom_h_pt
              << ",\"x\":" << (lb.inline_origin_pt + c.inline_pt) << "}";
        }
    }
    o << "]}";
    return pk::checked(pk::bracket_slots::kSpec, o.str());
}

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
        b.line_factor = 1.05f;
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
void loadStressDocumentDiverse(int paras, int chars) { loadStressDocumentImpl(paras, chars, true); }

/**
 * 用一段真实文本造文档：按换行切段，每段一个块，整体重复 repeat 遍。
 *
 * 判据要用真文本才准：合成文本（一批码点循环）的字形集合与用字分布都不像真病历，
 * 量出来的缓存账会偏。文本由宿主读进来并清洗掉 markdown 记号后传进来。
 */
void loadTextDocument(const std::string& text, int repeat) {
    // 空串 = 新建一份"只有一个空段落"的文档。
    //   这条必须有：空串若什么都不做，"清空文档"在引擎里就没有入口 ——
    //   setText('') 只能静默保留旧内容，而那是没法向使用者解释的一种语义。
    if (text.empty()) {
        g_blocks.assign(1, BlockSpec{});
        BlockSpec& only = g_blocks.front();
        only.font_index = g_cjk_font;
        only.size_pt = 12.0f;
        only.wm = WritingMode::HorizontalTB();
        only.line_factor = 1.0f;
        only.language = "zh";
        only.color = Color::rgb(24, 24, 24);
        only.setText(std::string());
        g_fields.clear();
        rebuildDocument(false);
        return;
    }
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
            b.line_factor = 1.0f;
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

/**
 * 保存：当前文档 → JSON 文本（格式见 rev::serialize）。
 *
 * 失败只可能是"还没装载文档"，那时返回空串 —— 不返回一段半截 JSON：
 *   宿主拿空串就知道没存成，而半截 JSON 会被写进文件、下次打开才炸。
 */
std::string serializeDocument() {
    const rev::SerializeResult r = g_engine.saveDocument();
    return r.ok ? r.text : std::string();
}

/**
 * 打开：JSON 文本 → 当前文档。返回空串 = 成功，否则是给人看的原因。
 *
 * 解析失败时引擎一个字节都不动（rev::deserialize 的契约），所以这里的失败
 *   不会把用户手上那份文档弄丢 —— 宿主把这句话显示出来就够了。
 * 成功之后要把 demo 那份"装载底稿"同步过来：后续改控件会按它重建，
 *   不同步的话用户打开的文件会被装载时那份旧底稿覆盖回去。
 */
std::string loadDocument(const std::string& text) {
    const rev::DeserializeResult r = g_engine.loadDocument(text);
    if (!r.ok) return r.error;
    syncBlocksFromEngine();
    g_fields.clear();
    return std::string();
}

/**
 * 复制：返回剪贴板载荷（{"f": 片段, "t": 纯文本} 或 {"e": 原因}）。
 *
 * 它是查询而不是命令：不改进文档、不上屏。宿主把它整份写进系统剪贴板的两个类型 ——
 *   自家格式给本编辑器（跨实例也行），纯文本给别的应用。
 */
std::string copySelection() {
    const ClipCopy c = g_engine.copySelection();
    return rev::clipboardPayload(c.fragment, c.text, c.error, c.html);
}

/**
 * 粘贴片段：空串 = 成功，否则是给人看的原因。
 *
 * 片段是从剪贴板读回来的字符串，这一层不解释它 —— 解析、校验、建节点全在引擎里，
 *   host 只搬字节。这样"能粘成什么样"只有一个真相。
 */
std::string pasteFragment(const std::string& text) { return g_engine.pasteFragmentAndRender(text); }

/**
 * 粘贴纯文本：从别的应用粘过来的文字走这条（按行拆段）。
 */
std::string pastePlainText(const std::string& text) {
    return g_engine.pastePlainTextAndRender(text);
}

/**
 * 工具栏递过来的字符格式补丁：键名与落盘（record_json 的 marksJson）同一套。
 *
 * 尺寸收"磅数"而不是 Length 对象：调用方是界面，写内部表示只会把它泄漏到 UI 层。
 * 这一层没有异常（wasm 构建关掉了），失败走返回串 —— 与 pastFragment 同一口径。
 * @param text JSON 文本
 * @param out 解析结果
 * @return 空串 = 成功；否则是给人看的原因
 */
std::string parseCharPatch(const std::string& text, CharFormat* out) {
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "字符格式必须是一个 JSON 对象";
    if (j.contains("b") && j["b"].is_boolean()) out->bold = j["b"].get<bool>();
    if (j.contains("i") && j["i"].is_boolean()) out->italic = j["i"].get<bool>();
    if (j.contains("u") && j["u"].is_boolean()) out->underline = j["u"].get<bool>();
    if (j.contains("fi") && j["fi"].is_number_integer()) out->font_index = j["fi"].get<int>();
    if (j.contains("sz") && j["sz"].is_number())
        out->size = Length::abs(ptToEmu(j["sz"].get<double>()));
    if (j.contains("col")) {
        const auto& c = j["col"];
        if (!c.is_array() || c.size() < 3) return "col 必须是 [r,g,b]";
        out->color = Color::rgb(c[0].get<int>(), c[1].get<int>(), c[2].get<int>());
    }
    return std::string();
}

/**
 * 段落格式补丁：fi（字体）/ sz（字号磅）/ lf（行距倍数）/ sa（段后距磅）/ pbb（段前分页）。
 * @param text JSON 文本
 * @param out 解析结果
 * @return 空串 = 成功；否则是给人看的原因
 */
std::string parseParaPatch(const std::string& text, ParaFormatPatch* out) {
    const nlohmann::ordered_json j = nlohmann::ordered_json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return "段落格式必须是一个 JSON 对象";
    if (j.contains("fi") && j["fi"].is_number_integer()) out->font_index = j["fi"].get<int>();
    if (j.contains("sz") && j["sz"].is_number())
        out->size = Length::abs(ptToEmu(j["sz"].get<double>()));
    if (j.contains("lf") && j["lf"].is_number()) out->line_factor = j["lf"].get<float>();
    if (j.contains("sa") && j["sa"].is_number())
        out->space_after = Length::abs(ptToEmu(j["sa"].get<double>()));
    if (j.contains("pbb") && j["pbb"].is_boolean()) out->page_break_before = j["pbb"].get<bool>();
    return std::string();
}

/**
 * 对选区应用字符格式（粗体 / 斜体 / 下划线 / 字体 / 字号 / 字色）。
 *
 * 返回串是三态：空 = 改了；noop = 格式本来就一样（不提交、不进撤销栈）；
 *   其余 = 解析失败的原因。
 * @param text 补丁 JSON
 * @return 见上
 */
std::string applyCharFormat(const std::string& text) {
    CharFormat patch;
    std::string why = parseCharPatch(text, &patch);
    if (!why.empty()) return why;
    if (!g_engine.applyCharFormatAndRender(patch)) return "noop";
    syncBlocksFromEngine();
    return std::string();
}

/**
 * 光标处的字符标记（工具栏回显）：键名与落盘同一套。
 * @param effective 非零 = 报实际生效的那一份（算上样式层与默认值）
 * @return JSON 文本
 */
std::string charFormatAt(int effective) { return g_engine.charFormatAtJson(effective != 0); }

/**
 * 光标所在段落的格式覆盖（工具栏回显）。
 * @return JSON 文本
 */
std::string paraFormatAt() { return g_engine.paraFormatAtJson(); }

/**
 * 对选区覆盖的段落应用段落格式（字体 / 字号 / 行距 / 段后距 / 段前分页）。
 * @param text 补丁 JSON
 * @return 空 = 改了；noop = 没变；其余 = 原因
 */
std::string applyParaFormat(const std::string& text) {
    ParaFormatPatch patch;
    std::string why = parseParaPatch(text, &patch);
    if (!why.empty()) return why;
    if (!g_engine.applyParaFormatAndRender(patch)) return "noop";
    syncBlocksFromEngine();
    return std::string();
}

/**
 * 删掉选区（剪切的后一半）。没有选区时返回 false。
 */
bool deleteSelectionCommand() { return g_engine.deleteSelectionAndRender(); }

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
    o << "\"rebuildReused\":" << e.rebuild_reused << ",";
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
    // 字数：非空白码点数。口径在引擎里（engine.cpp 的 isSpaceCodepoint），宿主只显示。
    o << "\"wordCount\":" << e.word_count << ",";
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
    return pk::checked(pk::edit_stats::kSpec, o.str());
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
 * 整篇文本（段间 \n）。
 *
 * 与 documentText 分开，不是图省事：宿主每敲一个字都要一份全文（change 事件要带它），
 *   而"逐段取再拼"要 N 次跨界调用 —— 2000 段时那一圈是毫秒级的，还是每拍。
 *   这一条在引擎里拼完只回一个字符串。
 */
std::string documentTextAll() {
    const auto& blocks = g_engine.blocks();
    std::string out;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (i) out.push_back('\n');
        out += blocks[i].textUtf8();
    }
    return out;
}

/**
 * 当前帧到底是哪一套光栅器画出来的。
 * 为什么值得专门暴露一个入口：这两个后端的输出在"帧尺寸、重画页数、写入像素数、
 * 甚至两遍渲染逐字节相等"这些指标上完全一样 —— 就是因为面板上写死了
 * "软件光栅"，而实际跑的是另一套，排查时对着一张空白的帧去怀疑几何。
 * 把来源标在面板上，是这类"指标全绿但画面上什么都没有"唯一廉价的解药。
 */
std::string rasterBackend() { return "Skia（SkCanvas + FreeType）"; }

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
    emscripten::function("fontsJson", &fontsJson);

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
    emscripten::function("insertTextAt", &insertTextAt);
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
    emscripten::function("selectCellAtPara", &selectCellAtPara);
    emscripten::function("cellSelectAt", &cellSelectAt);
    emscripten::function("selectWordAt", &selectWordAt);
    emscripten::function("selectParaAt", &selectParaAt);
    emscripten::function("moveSelectionTo", &moveSelectionTo);
    emscripten::function("moveImageTo", &moveImageTo);
    emscripten::function("dropSpot", &dropSpot);
    emscripten::function("setImageLayout", &setImageLayout);
    emscripten::function("watermarkJson", &watermarkJson);
    emscripten::function("setWatermark", &setWatermark);
    emscripten::function("setSelection", &setSelection);
    emscripten::function("flowIndexAt", &flowIndexAt);
    emscripten::function("flowTextPosJson", &flowTextPosJson);
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
    emscripten::function("documentTextAll", &documentTextAll);
    emscripten::function("undoEdit", &undoEdit);
    emscripten::function("redoEdit", &redoEdit);

    // 表单（控件是区间 + 编辑策略）
    emscripten::function("setFormControls", &setFormControls);
    emscripten::function("setEditMode", &setEditMode);
    emscripten::function("editMode", &editMode);
    emscripten::function("setShowBrackets", &setShowBrackets);
    emscripten::function("showBrackets", &showBrackets);
    emscripten::function("setShowControlDots", &setShowControlDots);
    emscripten::function("setShowMarks", &setShowMarks);
    emscripten::function("showMarks", &showMarks);
    emscripten::function("setShowMarginMarks", &setShowMarginMarks);
    emscripten::function("showMarginMarks", &showMarginMarks);
    emscripten::function("showControlDots", &showControlDots);
    emscripten::function("removeControlAt", &removeControlAt);
    emscripten::function("setControlPropAt", &setControlPropAt);
    emscripten::function("insertTableAt", &insertTableAt);
    emscripten::function("tableOpAt", &tableOpAt);
    emscripten::function("setCellSpecAt", &setCellSpecAt);
    emscripten::function("setRowSpecAt", &setRowSpecAt);
    emscripten::function("setTableSpecAt", &setTableSpecAt);
    emscripten::function("tableStateJson", &tableStateJson);
    emscripten::function("tableGrid", &tableGrid);
    emscripten::function("setTableColWidths", &setTableColWidths);
    emscripten::function("setTableRowHeight", &setTableRowHeight);
    emscripten::function("contextTargetJson", &contextTargetJson);
    emscripten::function("insertControlAt", &insertControlAt);
    emscripten::function("setControlSpecAt", &setControlSpecAt);
    emscripten::function("setControlValueAt", &setControlValueAt);
    emscripten::function("toggleControlAt", &toggleControlAt);
    emscripten::function("controlOptionAt", &controlOptionAt);
    emscripten::function("registerImage", &registerImage);
    emscripten::function("dropImage", &dropImage);
    emscripten::function("insertImageAt", &insertImageAt);
    emscripten::function("imageList", &imageList);
    emscripten::function("imageHit", &imageHit);
    emscripten::function("selectImage", &selectImage);
    emscripten::function("resizeImageAt", &resizeImageAt);
    emscripten::function("previewImageResizeAt", &previewImageResizeAt);
    emscripten::function("previewImageMoveAt", &previewImageMoveAt);
    emscripten::function("clearImagePreview", &clearImagePreview);
    emscripten::function("focusControl", &focusControl);
    emscripten::function("tabCaret", &tabCaret);
    emscripten::function("auditFrame", &auditFrame);
    emscripten::function("auditBase", &auditBase);
    emscripten::function("auditPage", &auditPage);
    emscripten::function("auditLayout", &auditLayout);
    emscripten::function("auditGlyphPages", &auditGlyphPages);
    emscripten::function("auditCompile", &auditCompile);
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
    // 保存 / 打开：文档进出的唯一一条路（阶段 6 的落点）。序列化不经过宿主拼字符串 ——
    //   格式由 rev::serialize 一份实现说了算，宿主只搬字节。
    emscripten::function("serializeDocument", &serializeDocument);
    emscripten::function("loadDocument", &loadDocument);
    // 剪贴板：复制是查询（不动文档），粘贴与删除是命令（自带重排与呈现）。
    //   自家格式只在本编辑器之间流转，纯文本给别的应用 —— 两条路都在引擎里落定，
    //   宿主不解释片段的内容。
    emscripten::function("copySelection", &copySelection);
    emscripten::function("pasteFragment", &pasteFragment);
    emscripten::function("pastePlainText", &pastePlainText);
    emscripten::function("deleteSelection", &deleteSelectionCommand);
    emscripten::function("applyCharFormat", &applyCharFormat);
    emscripten::function("applyParaFormat", &applyParaFormat);
    emscripten::function("charFormatAt", &charFormatAt);
    emscripten::function("paraFormatAt", &paraFormatAt);

    emscripten::function("frameWidth", &frameWidth);
    emscripten::function("frameHeight", &frameHeight);
    emscripten::function("frameByteSize", &frameByteSize);
    emscripten::function("frameRgba", &frameRgba);
    emscripten::function("pageRgba", &pageRgba);
    emscripten::function("pageDirtyRectsJson", &pageDirtyRectsJson);
    emscripten::function("pageDirtyPagesJson", &pageDirtyPagesJson);

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
