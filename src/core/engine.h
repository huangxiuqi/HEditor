/**
 * 引擎门面：原生宿主与 WASM 宿主共用同一份实现。
 * 这一层就是"平台边界"——向上只暴露三样东西：一个内存里的 RGBA 帧（frame()），
 * 宿主负责把它交给 PNG 编码器或 canvas.putImageData；一组度量数字
 * （EngineStats / EditStats），用于性能取证；一组编辑与光标查询入口
 * （insertTextUtf8 / backspace / caretRectPt / hitTestPt）。
 * 坐标一律是"段 + 码点偏移"，不暴露任何内部指针。
 * 边界上流的是"一大块位图"，不是几千条绘制指令——这是 Figma 那条纪律
 * （"循环留在引擎内，跨边界只传大块"）的最小可用形态。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_policy.h"
#include "core/layout/layout.h"
#include "core/render/bitmap.h"
#include "core/text/font.h"
#include "core/writing_mode.h"

namespace ced {

/**
 * 表单模式下哪些控件显示方括号。
 *
 * 括号既提示"这里可以输入"，又占宽度（进文档流，见 layout.h），所以显示范围决定
 * 排版成本：All 常显，占位集合与光标无关，光标移动不重排；Focused 只显示光标所在
 * 字段，代价是跨字段移动光标会重排整行。默认 All。
 */
enum class FormBracketScope : uint8_t {
    All = 0,      // 默认：所有控件一直显示括号
    Focused = 1,  // 只有光标所在字段显示括号，其余画小点
};

struct EngineConfig {
    PageMetrics page = PageMetrics::A4();
    /**
     * Tab 的缩进宽度（空格数）。
     *
     * Tab 在编辑器里表示缩进，不表示跳字段（跳字段是表单/网页的约定）；字段之间用
     * 鼠标点。用空格而非 U+0009：行盒只认簇的 advance，制表符会变成零宽度字符。
     */
    int tab_spaces = 4;

    float zoom = 2.0f;  // 设备像素 / 磅。2.0 ≈ 144dpi
    float page_gap_pt = 14.0f;
    Color background = Color::rgb(238, 240, 243);
    Color page_fill = Color::rgb(255, 255, 255);
    Color page_border = Color::rgb(206, 212, 218);
    Color caret_color = Color::rgb(31, 111, 235);
    float caret_width_pt = 1.4f;
    bool draw_page_border = true;

    /**
     * 表单控件标记。
     *
     * 不画底色也不画边框：控件内容就是普通正文，加底纹会让它看起来像另一种东西。
     * 括号常驻（form_brackets = All），光标只决定是否额外高亮；只读字段用灰色。
     * 标记画在帧上而非底图，因为要跟随光标状态，见 presentInternal。
     */
    FormBracketScope form_brackets = FormBracketScope::All;
    Color control_mark = Color::rgb(56, 139, 253);      // 可编辑：亮蓝
    Color control_mark_ro = Color::rgb(168, 168, 168);  // 只读：灰（否则看着能改）
    /**
     * 方括号用的是字体里的字形（'[' / ']'），所以没有"线宽""臂长"这类参数 ——
     * 粗细与形状跟着正文字体走，换字体/换字号都不用改配置。
     * 也没有"间距"参数：括号的间隙由排版给（控件边界会占一个括号宽度，
     * 见 layout.cpp 的占位簇），不再由绘制层补。
     */
    float control_dot_pt = 3.0f;  // 小点边长（磅）
};

struct EngineStats {
    int page_count = 0;
    int glyph_count = 0;
    int placed_line_count = 0;
    int cluster_count = 0;
    int face_count = 0;
    int font_instance_count = 0;
    /**
     * 字体回退取证：跑完整条链后仍是 .notdef 的字形数 / 切出的 run 数。
     */
    int missing_glyph_count = 0;
    int font_run_count = 0;
    double shape_ms = 0.0;
    double layout_ms = 0.0;
    double paint_ms = 0.0;
    /**
     * 绘制的三段分解。存在的理由：它是"要不要引入 GPU / 第三方光栅器"的判据。
     * 实测（4 页、1022 字形、帧面积从 0.51 Mpx 到 8.12 Mpx 共 16 倍）：
     * paint 只从 110 ms 涨到 173 ms（1.57 倍）—— 成本不随面积走。
     * 所以"加像素吞吐"（GPU、更快的 fill/memcpy）在这里是打空的；
     * 必须先把这三段分开，才知道钱到底花在哪一段。
     */
    double paint_fill_ms = 0.0;     // 铺面积：页间隙背景 + 页底 + 页边框
    double paint_glyph_ms = 0.0;    // 字形：hb 抽轮廓 + 扫描线光栅（或命中字形缓存后只做 blit）
    double paint_present_ms = 0.0;  // 帧缓冲搬移：reset + 从底图贴回 + 光标

    /**
     * 字形缓存取证
     * 这两个数字是"字形缓存到底有没有用"的唯一判据，缺一不可：
     * 命中率高 + 耗时降   → 缓存吃掉了真正的成本；
     * 命中率高 + 耗时没降 → 瓶颈根本不在字形光栅，别再往这儿投资；
     * 命中率低            → 亚像素相位太散（键设计问题），
     * 这时"再快的光栅器"能帮的也有限。
     */
    long long glyph_cache_hits = 0;
    long long glyph_cache_misses = 0;
    int glyph_cache_entries = 0;
    double total_ms = 0.0;
    int frame_width = 0;
    int frame_height = 0;
    float zoom = 1.0f;
};

/**
 * 一次编辑/呈现之后的可证伪计数。
 * 这一批数字存在的唯一理由：让"到底有没有做增量"变成一个可以当场证伪的
 * 事实，而不是一句"感觉挺流畅"。
 * 两层增量各有自己的判据：
 * 光栅（页指纹决定重画哪几页）看 repainted_pixels / repainted_pages；
 * 排版（段级续排）看 relaid_paragraphs / relaid_lines 与 layout_was_full ——
 * 在全量重排时 relaid_lines 等于 total_lines，做增量之后才真的小于它。
 */
struct EditStats {
    /**
     * 布局
     * 这一组只在"本轮真的重排过"时才有意义。
     * layout_ran 是那个开关：光标移动、光标闪烁这两条路径不重排，
     * 此时 relaid_lines / layout_ms 都必须是 0 —— 否则面板上会出现
     * "移动光标重排了 12 行、耗时 3ms"这种假账（数字是真的，归属是错的）。
     */
    bool layout_ran = false;
    int relaid_paragraphs = 0;
    int reused_paragraphs = 0;  // 后缀原样搬回的段数（内容一个字节都没动）
    int total_paragraphs = 0;
    int relaid_lines = 0;
    int total_lines = 0;
    bool layout_was_full = false;
    int dirty_page_count = 0;  // 内容指纹变了、需要重画的页数

    /** 上屏粒度：本次真正重画的页数，以及文档总页数。 */
    int repainted_pages = 0;
    int total_pages = 0;
    /**
     * 本次真正写过的像素数（由 Bitmap 记账，非上层估算），以及写入区域的包围盒（设备像素）。
     */
    long long repainted_pixels = 0;
    int repainted_x = 0;
    int repainted_y = 0;
    int repainted_w = 0;
    int repainted_h = 0;
    int frame_w = 0;
    int frame_h = 0;

    /** 耗时拆分（毫秒）：本次排版、本次绘制、以及这一次操作的总计。 */
    double layout_ms = 0.0;
    double paint_ms = 0.0;
    double total_ms = 0.0;

    /** 光标位置与文档规模（逻辑坐标，与视觉行列无关）。 */
    int caret_para = 0;
    size_t caret_offset = 0;  // 段内码点偏移
    size_t caret_index = 0;   // 全文码点下标
    size_t doc_codepoints = 0;
    bool caret_visible = false;
    /**
     * 光标在帧像素坐标下的矩形。宿主据此摆 IME 候选框、
     * 或把原生 textarea 覆盖层挪到光标处（档 3）。
     */
    int caret_x_px = 0;
    int caret_y_px = 0;
    int caret_w_px = 0;
    int caret_h_px = 0;

    /**
     * 表单（区间标注 + 编辑策略）
     * blocked_edits 是累计值，其余是本轮快照。
     * 它存在的理由：策略生效时编辑会静默返回 false，
     * 宿主看到的现象是"打字没反应" —— 那和"程序卡了/坏了"在观感上无法区分。
     * 有了这个计数，"按了 5 次键一个字没进去"就有了一个可断言的说法：
     * 文档长度不变 且 blocked_edits += 5。
     */
    int edit_mode = 0;  // 0=文档模式 1=表单模式 2=只读模式
    int control_count = 0;
    int caret_control = -1;  // 光标最内层所在的控件；-1 = 不在任何控件内
    long long blocked_edits = 0;
};

class EditorEngine {
public:
    /**
     * fontPaths 的顺序决定字体索引
     */
    bool init(const std::vector<std::string>& fontPaths);

    void setConfig(const EngineConfig& cfg);
    const EngineConfig& config() const { return cfg_; }

    /**
     * 页度量：断行依赖内容宽度，所以改它必须整篇重排（影响面不可定位）。
     */
    void setPage(const PageMetrics& page) {
        if (page != cfg_.page) markAllLayoutDirty();
        cfg_.page = page;
    }
    void setBlocks(std::vector<BlockSpec> blocks);
    const std::vector<BlockSpec>& blocks() const { return blocks_; }

    /**
     * 重排版：从 layout_dirty_from_ 起的段重排，之前的段原样复用（续排契约见
     * core/layout/layout.h）。没有脏段时按全量处理 —— 宁可多排一次，也不拿旧前缀
     * 去接新后缀。底图内容随之作废，呈现时按页指纹增量重画。
     */
    void relayout();
    /**
     * 全量：底图 + 光标一次性提交到帧缓冲。
     */
    void repaint();
    /**
     * 只渲染 [first_page, first_page+count) 这几页（连续视图）。
     */
    void paintPageRange(int first_page, int count);

    /**
     * 光标。坐标是 (段, 段内码点偏移)，任何设置入口都会先夹到合法范围、
     * 再吸附到字素簇边界 —— 所以光标永远不会停在半个 é（e + U+0301）中间。
     */
    void setCaret(int para, size_t offset);
    void setCaret(TextPos p) { setCaret(p.para, p.offset); }
    const TextPos& caret() const { return caret_; }
    /**
     * 按"用户感知字符"（簇）移动。dcp > 0 向后、< 0 向前。
     * 返回 true 表示光标真的动了（没动就别重绘）。
     */
    bool moveCaretBy(int dcp);
    /**
     * 到行首/行尾，不是段首/段尾 —— 这才是 Home/End 该有的语义。
     */
    bool moveCaretToLineStart();
    bool moveCaretToLineEnd();
    void showCaret(bool on);

    /** 编辑入口：坐标是 (段, 段内码点偏移)，落地前吸附到字素簇边界。 */
    bool insertTextUtf8(int para, size_t offset, const std::string& utf8);
    bool insertCodepoints(int para, size_t offset, const std::vector<uint32_t>& cps);
    bool insertCodepoint(int para, size_t offset, uint32_t cp);
    /**
     * 在光标处插字 / 删字（删除按簇，不是按码点）
     */
    bool typeTextUtf8(const std::string& utf8);
    bool backspace();
    bool deleteForward();

    /**
     * 编辑 + 重排 + 呈现的组合入口
     * 为什么要把三步合成一次调用：这三步之间没有任何需要宿主参与的信息。
     * 让宿主分别调三次，就是三次跨界 + 三次对象封送 —— 那才是 WASM 的减速带。
     * 组合在 C++ 里，宿主一次调用拿到"有没有变"和一组统计。
     */
    bool typeTextAndRender(const std::string& utf8);
    bool backspaceAndRender();
    bool deleteForwardAndRender();
    bool moveCaretAndRender(int dcp);
    /**
     * Home / End：到行首 / 行尾。
     *
     * 不用 moveCaret(±1e9) 实现：那样依赖 moveCaretBy 走不动时提前 break，且语义不同。
     */
    bool moveCaretToLineStartAndRender();
    bool moveCaretToLineEndAndRender();

    /**
     * 表单控件：文档流里的区间标注，不是矩形。模型与"区间为什么是活的"见
     * core/doc/control.h 顶部。
     *
     * 控件本身不参与断行/分页，参与排版的是它的括号：要显示括号的字段会在自己的
     * 边界插入"占位簇"（占宽度、不产生字形，见 layout.h）。因此挂控件、改控件范围
     * 不必重排（正文没变），而改显示括号的范围（form_brackets）必须重排；两侧共用
     * bracketedControls() 这一张名单，结构上不会一边占位一边不画。
     */

    ControlSet& controls() { return controls_; }
    const ControlSet& controls() const { return controls_; }
    /**
     * 传空 vector = 清空。内部会 normalize（建区间树）。
     */
    void setControls(std::vector<Control> list);
    void clearControls();

    void setEditMode(EditMode m);
    EditMode editMode() const { return policy_.mode(); }

    /**
     * 当前光标所在的最内层控件下标（-1 = 不在任何控件内）。
     * 口径与 canPlaceCaret 一致（include_end = true）：光标停在字段末尾也算在字段里。
     */
    int focusedControlIndex() const {
        return controls_.empty() ? -1 : controls_.innermostAt(caret_, true);
    }

    /**
     * 要显示括号（= 要在排版里占宽度）的控件下标。排版只认这张表。
     * 空表 = 一个占位都不插，排版与"没有表单"逐字节一致。
     */
    std::vector<int> bracketedControls() const {
        std::vector<int> out;
        if (policy_.mode() != EditMode::Form || controls_.empty()) return out;
        if (cfg_.form_brackets == FormBracketScope::All) {
            out.reserve(static_cast<size_t>(controls_.size()));
            for (int i = 0; i < controls_.size(); ++i) {
                out.push_back(i);
            }
            return out;
        }
        const int f = focusedControlIndex();
        if (f >= 0) out.push_back(f);
        return out;
    }

    /**
     * 「括号占位计划」的指纹：变了就必须重排（占位进文档流，集合一变排版就变）。
     * 编码：-1 = 没有占位；-2 = 全部控件（All 档）；>= 0 = 那一个控件（Focused 档）。
     * 为什么 All 档返回常数：占位集合与光标无关 → 光标跨字段移动不再重排
     * （Focused 档下那是一次整行重排，是"显示才占位"的真实代价）。
     * 为什么不能用 focusedControlIndex() 代替它：文档模式下标着同一份文档、
     * 括号根本不画，却会照样把正文推开一截 —— 凭空多出来的空隙，谁也没要。
     */
    int bracketPlanId() const {
        if (policy_.mode() != EditMode::Form || controls_.empty()) return -1;
        if (cfg_.form_brackets == FormBracketScope::All) return -2;
        return focusedControlIndex();
    }

    /**
     * 累计被编辑策略拒掉的次数。
     *
     * 用 getter 直读成员，不走 EditStats：被拒时不触发 repaint，而 EditStats 要等下一次
     * 呈现才刷新，走它会差一拍。
     */
    long long blockedEdits() const { return blocked_edits_; }

    /**
     * 帧坐标（磅）落在哪个控件上（取最内层）。-1 = 没有。
     * 它走的是 hitTestPt 的同一份几何，所以"视觉上在框里"与
     * "逻辑上在控件里"永远一致。
     */
    int controlAtPt(float x_pt_frame, float y_pt_frame) const;

    /**
     * 控件在页面坐标下的矩形。
     * 返回的是 vector —— 因为区间跨行/跨页时它本来就该是多个矩形。
     * "控件是一个矩形"这个直觉只在单行短字段上成立；一旦内容换行就不成立了，
     * 所以接口从第一天就按"多个"来设计，免得以后改签名。
     */
    struct ControlRect {
        int page = 0;
        Rect r_pt{};
    };
    std::vector<ControlRect> controlRects(int index) const;

    /**
     * Tab / Shift+Tab：把光标移到下一个（上一个）可编辑控件的起点。
     * 到头会绕回第一个 —— 表单里 Tab 就该是循环的。
     */
    bool focusControl(bool forward);

    /**
     * 跨字段移动光标必须用这一个（Tab / Shift+Tab 走的就是它）。
     * focusControl 只改状态、不重绘，而宿主随后会拿上一次呈现的脏矩形去上屏，
     * 画布于是成了新旧混合体；收尾重绘不可省，理由见 engine.cpp 的实现。
     */
    bool focusControlAndRender(bool forward);

    /**
     * 帧自审：把当前这一帧与"整帧重画一遍"的结果逐字节比，返回
     * {"diff":N,"frameW":..,"frameH":..,"box":[x0,y0,x1,y1]}。
     * diff > 0 就等于"增量路径漏掉了什么"（典型：某个旧表单标记没擦干净）。
     * 跑完顺便把这一帧修成真值。理由见 engine.cpp 的实现。
     */
    std::string auditFrameJson();

    /**
     * 排版自审：用"不用缓存的全量重排"做一份对照，与当前排版结果逐字段比对，
     * 返回不符项数（0 = 一致）。
     * 理由与 auditFrameJson 相同：增量的正确性由"与全量一致"定义，
     * 而不是由"看起来对"定义。逐字段而不是只比行数：增量最典型的错法是
     * "少排了一段"或"前缀多留了一段"，两者都只差几行。
     */
    int auditLayout();

    /** 同上的 JSON 版，供宿主取证（含两侧规模、首个不符位置与两个耗时）。 */
    std::string auditLayoutJson();

    /**
     * 上屏契约：引擎写过的区域累积到宿主读取为止，读后清零。
     *
     * 与 EditStats 的 repaintedX/Y/W/H 不同：那是"本轮写了多少"（面板用），这里要的是
     * "自上次上屏以来写过哪些区域"。一次用户操作可能触发两次 present（见
     * repaintAfterEdit），用后一轮的脏矩形上屏会丢掉前一轮的整帧重绘，此时引擎帧正确、
     * 画布却是旧的（auditFrame 差 0、selfCheck 差上千像素，正是这个形状）。
     * 累加器让"引擎写了、宿主没贴"在结构上不可能发生，也不再依赖每个调用点都用对
     * 那一轮的账。代价是并集可能大于严格必要：多贴几个矩形只是性能问题，漏贴是
     * 正确性问题。
     */
    Rect takeBlitBox();  // 读并清零；无待上屏时返回 w/h = 0

    /**
     * 编辑类入口的统一收尾：先让光标回到可见相位，再重绘。
     *
     * 闪烁可能正停在隐藏半拍，此时 editStats.caretVisible 为 false，宿主会走补救路径
     * 再 present 一次。把"编辑后光标可见"这条不变量落在引擎里，该路径就不会被触发。
     */
    void repaintAfterEdit();

    /**
     * Tab 缩进：在光标处插入 tab_spaces 个空格，走与敲字相同的策略与重排路径。
     * Shift+Tab 反缩进：删掉光标前最多 tab_spaces 个连续空格。
     * 不做"跳到下一个字段"：Tab 在正文编辑器里表示缩进，换字段用鼠标点。
     */
    bool indentAndRender();
    bool outdentAndRender();
    bool indent();
    bool outdent();

    /**
     * 诊断：把模式、控件树、每个控件的区间与矩形块数摊平。
     */
    std::string describeControls() const;

    /**
     * 几何：同一份行盒几何的正反查
     * 正查：(段, 偏移) → 页面坐标下的光标矩形（磅）
     */
    bool caretRectPt(Rect* out) const;
    /**
     * 反查：帧坐标（磅）→ (段, 偏移)。点落在页间空隙时返回 false。
     */
    bool hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out) const;

    const uint8_t* frame() const { return bitmap_.data(); }
    int frameWidth() const { return bitmap_.width(); }
    int frameHeight() const { return bitmap_.height(); }
    size_t frameBytes() const;

    /**
     * 诊断用：关掉字形缓存。
     * 存在的唯一理由：让"缓存路径与直接光栅路径逐字节相同"变成一条可证伪的
     * 断言 —— 同一个文档、同样的缩放下渲染两遍（一次带缓存、一次不带），
     * 帧缓冲必须逐字节相等。没有这个开关，这条等价性就只能靠推理声称。
     */
    void disableGlyphCache();

    const EngineStats& stats() const { return stats_; }
    const EditStats& editStats() const { return edit_stats_; }
    const LayoutResult& layout() const { return layout_; }
    FontLibrary& fonts() { return fonts_; }

    /**
     * 诊断用：把一串文本的整形结果摊成可读文本（字形 id / 推进 / cluster），
     * 让"整形到底有没有发生"这件事可被断言，而不是只看能不能编译。
     */
    std::string describeShaping(int font_index, float size_pt, const std::string& text,
                                const WritingMode& wm, const char* language);

    /**
     * 诊断用：把当前光标的行盒几何、簇表、命中结果摊成可读文本。
     */
    std::string describeCaret() const;

    /**
     * 页坐标 -> 设备像素坐标
     */
    float ptToPx(float pt) const { return pt * cfg_.zoom; }

    /**
     * 全文码点总数（不含块间分隔）
     */
    size_t documentCodepoints() const;

private:
    /**
     * 把偏移吸附到簇边界（用当前 layout 的簇表）
     */
    size_t snapToCluster(int para, size_t offset) const;
    /**
     * 找到包含该位置的行盒（先按行内范围命中，再退化为"同段最近行"）
     */
    const LineBox* findLineFor(const TextPos& pos) const;
    /**
     * 给定位置的行盒 + 页号 + 页面坐标矩形
     */
    bool caretRectFor(const TextPos& pos, int* page_out, Rect* out) const;
    /**
     * 光标矩形（帧像素坐标）。caret_visible 为 false 或找不到行盒时返回零矩形。
     */
    Rect caretRectPx() const;
    /**
     * 统一呈现入口。内部按"页指纹是否变了"决定重建底图的哪几页。
     */
    void presentInternal(int first_page, int count, bool force_full);
    /**
     * 算每页内容的廉价指纹，用来跳过没变的页
     */
    void computePageFingerprints();
    void syncStatsFromLayout();

    /**
     * 把 pos 投影到策略允许的位置（就近，认距离不认方向）。
     *
     * 只用于鼠标点击与程序化定位：点击给的是粗略的点，投影到最近的可编辑处是合理的猜测。
     * 键盘移动不能用它 —— 方向键是明确指令，投影会把光标送到另一个控件（在 #2 内按一次
     * 左键会跨过只读字段落到 #0）。键盘走 moveCaretBy 的逐格停，Home/End 走
     * clampToFieldEdge。dir > 0 向文档后方找、< 0 向前找，找不到保持原值。
     */
    bool clampToAllowed(TextPos* pos, int dir) const;

    /**
     * 标记"从第 para 段起需要重排"（取最小值，一次操作跨多段也只需一次标记）。
     * 凡是改段内容或改段数的入口都要调它，否则增量会拿旧前缀去接新后缀。
     */
    void markLayoutDirty(int para);

    /**
     * 全量记号：段数、页度量、控件集合这类"影响面不可定位"的改动用它。
     * 与 markLayoutDirty(0) 的区别在于它**禁止**走后缀复用快路：整篇都脏时
     * 后缀该不该变是不知道的（页宽变了、段数变了），只有重排一遍才算数。
     */
    void markAllLayoutDirty() {
        layout_dirty_from_ = 0;
        layout_needs_full_ = true;
    }

    /**
     * 括号占位计划变了（编辑模式切换、控件集合替换）时标脏。
     * 只标到"最靠前的那个控件所在段"：占位只插在控件边界所在的段里，
     * 更靠前的段排版结果不变。没有控件时不动 —— 那时占位集合恒为空。
     */
    void markLayoutDirtyForBrackets();

    /** 控件列表里最靠前的段下标；没有控件时返回 -1。 */
    int minControlParagraph() const;

    /**
     * 把 layout_ 截断到第 para 段之前：丢掉 [para, ...) 的行盒、字形与切片，
     * 保留 [0, para) 原样，供 layoutDocumentRange 从 para 续排。
     * para <= 0 时清空整个结果。
     */
    void truncateLayoutAt(int para);

    /**
     * 快路：只重排第 from 段，把后缀（[from+1, 末段]）原样保留。
     *
     * 判据只有一条 —— 脏段排完之后，续排状态（页号 / cursor_y / 行号）与上一轮
     * "后缀开始时"的那个状态逐位相同。三个标量一致，就意味着后缀里每一行的断页、
     * 全局行号与坐标都会和上一轮一模一样：可以整块搬回去，只有脏段自己的行盒与
     * 字形要换掉（erase + insert 两次 memmove）。
     *
     * 为什么值得单开一条路：后缀即使命中整形缓存，仍要把断行与字形放置重跑一遍，
     * 成本与后缀长度成正比。而"在原行里多打一个字"这类编辑根本不改变段落的行结构 ——
     * 那之后，后缀一个字节都不该动。
     *
     * @return true = 排版已完成；false = 条件不满足，调用方走常规续排。
     */
    bool trySpliceOnly(int from, const std::vector<int>& bracketed);

    /**
     * 画表单标记（方括号；Focused 档下还给非聚焦字段画小点）到帧上。
     * 几何全部从排版的行盒派生（占位簇的位置 + 行盒的括号参考字体/基线）：
     * 排版说槽在哪，就画在哪 —— 空字段（一个字形都没有）也画得出来。
     * 不进底图：见 presentInternal 里那段说明。
     */
    void drawControlMarks();

    /**
     * 光标 / 编辑模式动过之后调用：括号占位计划变了就重排。
     * 传的是改动之前的 bracketPlanId()。All 档下它恒为常数 →
     * 光标在同一/不同字段间移动都不会重排（"光标移动 = 真增量"的地基）。
     */
    void relayoutIfBracketPlanChanged(int prev_plan);

    /**
     * Home/End 的收边：目标在当前可编辑字段内则照常，否则退到本字段起点/终点；光标不在
     * 任何字段内时返回 false。
     *
     * 不用 clampToAllowed：那是投影到最近处，会把光标送进另一个控件。
     */
    bool clampToFieldEdge(TextPos* pos, bool to_start) const;

    EngineConfig cfg_;
    FontLibrary fonts_;
    std::vector<BlockSpec> blocks_;
    LayoutResult layout_;

    /**
     * 段级整形缓存（HarfBuzz 结果 + 簇推进 + 断点）。
     * 整形是唯一"必须摸字体"的一步，也是最贵的；它的输入只有文本与字体参数，
     * 所以改页宽、切编辑模式之后照样命中（见 layout.cpp 的 shapingFingerprint）。
     */
    ShapingCache shaping_cache_;

    /**
     * 需要重排的最小段下标；>= blocks_.size() 表示没有脏段。
     * 只有一个数就够，因为排版是"前缀保留 + 后缀重排"：要重排的段必然连成一段后缀。
     */
    int layout_dirty_from_ = 0;

    /**
     * 整篇都脏（换文档、改页度量、段数变化）。与"从第 0 段起脏"不同：
     * 后者只是第一段的内容变了，后缀仍有可能原样复用（见 trySpliceOnly）。
     */
    bool layout_needs_full_ = true;

    /** 上一次 relayout 的粒度，由 presentInternal 上报到 EditStats。 */
    int last_relaid_paragraphs_ = 0;
    int last_reused_paragraphs_ = 0;
    int last_relaid_lines_ = 0;
    bool last_layout_full_ = true;

    Bitmap bitmap_;  // 给宿主的帧（= 底图 + 光标）
    Bitmap base_;    // 底图（无光标）—— 局部重绘的全部依据就是这个缓存

    /**
     * 帧缓冲与底图共享的字形光栅缓存。见 core/render/bitmap.h 里的
     * GlyphMask / GlyphCache 注释（为什么必须有亚像素相位、为什么两条路径
     * 必须算出同一串浮点数）。
     */
    std::shared_ptr<GlyphCache> glyph_cache_;
    EngineStats stats_;
    EditStats edit_stats_;

    TextPos caret_{0, 0};
    bool caret_visible_ = true;

    /**
     * 底图当前显示的范围与有效性
     */
    int base_first_page_ = 0;
    int base_page_count_ = 0;
    bool base_valid_ = false;

    /**
     * 页内容指纹：本次 relayout 的结果 / 已画进底图的那一版。
     */
    std::vector<uint64_t> page_fp_new_;
    std::vector<uint64_t> page_fp_in_base_;
    int page_fp_new_count_ = 0;
    int page_fp_base_count_ = 0;

    /**
     * relayout() 置位、presentInternal() 消费。
     * 用它把"排版记账"和"光栅记账"分开：一次 relayout 后面可能跟很多次
     * repaint（光标闪烁每 530ms 就一次），只有第一次该认领那笔排版成本。
     * 表单标记"计划"的指纹（编辑模式 + 聚焦字段 + 各控件的投影矩形）。
     * presentInternal 用它判断"这一帧要画的标记跟上一帧不一样"→ 整帧重贴。
     * 理由（脆弱前提）写在 engine.cpp 的实现与 presentInternal 里。
     */
    uint64_t markPlanSignature() const;
    uint64_t mark_sig_prev_ = 0;

    /**
     * 待上屏脏矩形累加器（见 takeBlitBox 的注释）。
     */
    void noteBlitDirty(const Rect& r);
    Rect blit_acc_{};
    bool blit_acc_any_ = false;

    bool layout_pending_ = false;

    /** 表单：控件区间集合与编辑策略（策略是纯函数，见 core/doc/edit_policy.h）。 */
    ControlSet controls_;
    EditPolicy policy_;
    long long blocked_edits_ = 0;

    /**
     * 表单标记（小点/方括号）在帧像素下的矩形，用来在下一帧把它们擦掉。
     * 与 caret_rect_px_prev_ 同一个思路：瞬态的东西必须记账，否则擦不干净。
     */
    std::vector<Rect> control_marks_prev_;

    /**
     * 要求下一次呈现整帧重贴（而不是只贴几个脏矩形）。
     * 用在"帧上画的东西变了、但底图没变"的时候 —— 切编辑模式、改控件集合。
     * （底图不含控件标记，所以那两件事不会让页指纹变；不显式置位就会出现
     * "切回文档模式后括号还在"这种残留。）
     */
    bool frame_needs_full_refresh_ = false;

    Rect caret_rect_px_prev_{};  // 上一帧光标矩形（帧像素）
};

/**
 * 样例文书：中文横排 → 蒙文 vertical-lr（列序从左往右）→ 蒙文 vertical-rl（对照）
 * latin_font_index >= 0 时，中文块挂上拉丁字体作为回退（上标数字等中文子集缺字）
 */
std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                      int latin_font_index = -1);

/**
 * 中英文混排块（病历里最常见的形态：中文叙述 + 英文缩写/数值/单位/拉丁术语）。
 * 用于验证同一段落里 CJK 与 Latin 的断行与度量是否正确。
 * latin_font_index 给出后，把它作为回退字体挂上 —— "10⁹/L" 的上标 9 正是靠它兜住。
 */
BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index = -1);

}  // namespace ced
