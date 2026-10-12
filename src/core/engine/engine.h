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

#include "core/engine/engine_config.h"
#include "core/engine/engine_stats.h"
#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_policy.h"
#include "core/layout/layout.h"
#include "core/render/bitmap.h"
#include "core/text/font.h"
#include "core/text/writing_mode.h"

namespace heditor {

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
    /**
     * 带"归属"的定位：点画布时用户点的是某个字段的括号，位置本身（共享边界上的
     * 那个偏移）分不出是哪一侧，于是把结论一起带进来。
     *
     * 适用条件写死成一条：只有当光标仍然停在这里给的位置上时，这个归属才有效
     * （见 caretFieldHint）。于是所有会移动光标的编辑自动让它失效 ——
     * 不需要在每个编辑入口记得清一下，忘了清正是这类"陈旧状态"bug 的来源。
     */
    void setCaretAt(int para, size_t offset, int field);
    void setCaretAt(TextPos p, int field) { setCaretAt(p.para, p.offset, field); }
    const TextPos& caret() const { return caret_; }
    /**
     * 光标当前的归属提示（-1 = 没有；按位置是否仍旧吻合判有效性）。
     * 仅供编辑归属与光标几何使用。
     */
    int caretFieldHint() const {
        return (caret_field_ >= 0 && caret_field_at_ == caret_ && caret_field_ < controls_.size())
                   ? caret_field_
                   : -1;
    }
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

    /**
     * 编辑入口：坐标是 (段, 段内码点偏移)，落地前吸附到字素簇边界。
     */
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
     * 跨段编辑。
     *
     * 回车在光标处把这一段拆成两段；新段整体继承原段的样式，只把 page_break_before
     * 清掉（段落分隔符不该附带一次翻页）。段首退格、段末 Delete 分别合并前后段 ——
     * 它们是 backspace() / deleteForward() 的自然延伸，不是另外的编辑命令。
     */
    bool splitParagraph();
    bool mergeWithPreviousParagraph();
    bool mergeWithNextParagraph();

    /**
     * 编辑 + 重排 + 呈现的组合入口
     * 为什么要把三步合成一次调用：这三步之间没有任何需要宿主参与的信息。
     * 让宿主分别调三次，就是三次跨界 + 三次对象封送 —— 那才是 WASM 的减速带。
     * 组合在 C++ 里，宿主一次调用拿到"有没有变"和一组统计。
     */
    bool typeTextAndRender(const std::string& utf8);
    bool backspaceAndRender();
    bool deleteForwardAndRender();
    bool splitParagraphAndRender();
    bool mergeWithPreviousParagraphAndRender();
    bool mergeWithNextParagraphAndRender();
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
     * 边界落成一项"行内对象"（见 BlockSpec::objects），排版据此插入占位簇（占宽度、
     * 不产生字形，见 layout.h）。因此挂控件、改控件范围不必重排（正文没变），而改显示
     * 括号的范围（form_brackets）必须重排 —— 对象表变了，排版输入就变了。
     *
     * 结构与绘制共用同一份落点：括号画在哪由占位簇的几何给出（owner_control 指回
     * 控件），不再两边各记一套 —— "占位一次、不占位一次"那类不一致没有立足处。
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
     * 用 include_end = true：光标停在字段末尾也算在字段里。
     * 例外是"点画布带来的归属提示"：共享边界上它比最内层更准（见 setCaretAt），
     *   有提示时以提示为准 —— 界面上的"当前字段"要跟用户的点击一致。
     */
    int focusedControlIndex() const {
        const int hint = caretFieldHint();
        if (hint >= 0) return hint;
        return controls_.empty() ? -1 : controls_.innermostAt(caret_, true);
    }

    /**
     * 要显示括号（= 要在排版里占宽度）的控件下标 —— 产生端的名单，只有引擎认识
     * 「聚焦」与编辑模式。syncInlineObjects() 按它决定往对象表里写哪些边界。
     * 空 = 一个占位都不插，排版与"没有表单"逐字节一致。
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
     * 把控件边界写进块流的行内对象表 —— 元素流在引擎侧的落点，也是排版层唯一的
     * 括号来源（layoutDocument 不再收控件集合）。幂等，每次 relayout 之前调用。
     *
     * 产出端暂时还是引擎：只有它知道编辑模式与 form_brackets，也就知道哪些字段要
     * 显示括号。等编译层能把内容控件直接编成行内对象，这里就退化成一次过滤。
     */
    void syncInlineObjects();

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

    /**
     * 同上的 JSON 版，供宿主取证（含两侧规模、首个不符位置与两个耗时）。
     */
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
     *
     * @param out_owner 非空时写出"该次点击属于哪个控件"（-1 = 判不出）。
     *   与偏移分开返回是必须的：共享边界上两个相邻字段落在同一个偏移上
     *   （前一个字段的末尾 = 后一个字段的开头），光有偏移无法表达用户点的是哪一侧。
     *   判据全部来自排版自己插的占位簇（它记着 owner_control）：
     *     点到占位簇 → 就是那个占位的主人；
     *     点到真实簇 → 该位置的内容所属的控件（半开区间，于是"点字段第一个字的左半"
     *       落在它的开头、归它，而不是归前一个字段）。
     */
    bool hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out,
                   int* out_owner = nullptr) const;

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
     * 页指纹：本次 relayout 的结果 / 已画进底图的那一版。
     *
     * 只重算受影响页（@p dirty_lo 到 @p dirty_hi，含端点）：没有脏页信息时
     * 传 -1 走整篇重算。增量成立的前提是"没动过的页，字形一个字节都没动" ——
     * 它由排版那侧的前缀复用保证，而不是靠这里猜。
     */
    void computePageFingerprints(int dirty_lo = -1, int dirty_hi = -1);
    void syncStatsFromLayout();

    /**
     * 标记"从第 para 段起需要重排"（取最小值，一次操作跨多段也只需一次标记）。
     * 凡是改段内容或改段数的入口都要调它，否则增量会拿旧前缀去接新后缀。
     */
    void markLayoutDirty(int para);

    /**
     * 全量记号：段数、页度量、控件集合这类"影响面不可定位"的改动用它。
     * 与 markLayoutDirty(0) 的区别在于它禁止走后缀复用快路：整篇都脏时
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

    /**
     * 控件列表里最靠前的段下标；没有控件时返回 -1。
     */
    int minControlParagraph() const;

    /**
     * 把 head + 1 段并进 head 段（段首退格 / 段末 Delete / 显式调用共用的实现）。
     * 光标落在合并点上。策略拒绝时返回 false 并计入 blocked_edits_。
     */
    bool mergeParagraphInto(int head);

    /**
     * 把 layout_ 截断到第 para 段之前：丢掉 [para, ...) 的行盒、字形与切片，
     * 保留 [0, para) 原样，供 layoutDocumentRange 从 para 续排。
     * para <= 0 时清空整个结果。
     */
    void truncateLayoutAt(int para);

    /**
     * 后缀复用的三种结局。
     * 三种都要求脏段自身的排版已重做完，差别只在后缀要不要动。
     */
    enum class SuffixReuse {
        None,       // 复用不成立，调用方走常规续排（从脏段排到末尾）
        Unchanged,  // 后缀一个字节都不用动（续排状态逐位相同）
        Shifted,    // 后缀整体沿块轴平移 + 重编页码/行号（行结构变了，但只有块轴位置变）
    };

    /**
     * 后缀复用：只重排第 from 段，然后试着不动后缀。
     *
     * 两级判据，从严到宽：一是同状态，后缀的断页、行号与坐标都会与上一轮逐位相同，整块搬回即可；
     * 二是后缀是纯横排，只有块轴起点变了，于是每一行都只是"平移 + 改页码"（见 shiftSuffixBlockAxis）。
     * 值得单开这条路，是因为后缀即使全部命中整形缓存，仍要把断行与字形放置重跑一遍，
     * 成本与后缀长度成正比（还会给每个行盒分配一次簇向量），而"在原行里多打一个字"
     * 根本不改变行结构。
     *
     * @param page_lo,page_hi 出参：脏段新旧两版覆盖的页区间，供页指纹只重算这几页，拿不到时写 -1。
     * @return None 时后缀未被动过（调用方可以安全地走续排），其余两种排版已完成。
     */
    SuffixReuse trySuffixReuse(int from, int* page_lo, int* page_hi);

    /**
     * 后缀整体平移：把 [from+1, 末段] 的每一行沿块轴挪到新的位置，并重编页码与全局行号，
     * 字形跟着它的行一起挪。断行结果（每行的簇与行内位置）完全不重算。
     *
     * 为什么可以这样做：横排的块轴推进是一条纯折线 —— 每行占 blocks[p].size_pt *
     * line_factor、段间再让出 space_after_pt，放不下就换页。所以只要知道后缀开始时的那一个
     * 续排状态（页号 + cursor_y + 行号），整个后缀的新位置就被唯一确定。
     * 这条规则与 layoutDocumentRange 横排分支里的断页判据同源；两者一旦分叉，
     * 单测 [15] 的 auditLayout（增量 vs 全量逐字段比）会立刻报红。
     *
     * 竖排段不吃这条规则（列与"块吃掉本页剩余高度"是另一套），所以后缀里出现竖排就返回 false。
     */
    bool shiftSuffixBlockAxis(int from, const ReflowState& after);

    /**
     * 后缀（[from+1, 末段]）里有没有竖排段 —— 竖排不能走平移复用。
     */
    bool suffixIsHorizontalOnly(int from) const;

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
     * 整篇都脏（换文档、改页度量、页度量变化）。与"从第 0 段起脏"不同：
     * 后者只是第一段的内容变了，后缀仍有可能原样复用（见 trySuffixReuse）。
     */
    bool layout_needs_full_ = true;

    /**
     * 段数变了（回车分段 / 退格合并段），尚未重排。
     *
     * 它必须禁掉后缀复用：复用判据是"第 from+1 段开始时的续排状态与上一轮相同"，
     * 而段号一变，"第 from+1 段"已经不是同一段了 —— 拿两个无关段落的状态去比，
     * 碰巧相等就会把整个后缀错当成没变（那会静默地产生错位的排版）。
     *
     * 但"段数变了"不等于"必须整篇重排"：前缀 [0, dirty_from) 仍然一个字都没动，
     * 所以照走常规续排（从脏段排到末尾）。重排完成即清零。
     */
    bool para_count_changed_ = false;

    /**
     * 上一次 relayout 的粒度，由 presentInternal 上报到 EditStats。
     */
    int last_relaid_paragraphs_ = 0;
    int last_reused_paragraphs_ = 0;
    /**
     * 后缀是被"平移复用"的（见 SuffixReuse::Shifted）—— presentInternal 上报用。
     */
    bool last_suffix_shifted_ = false;
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
     * 光标归属提示：点画布时定下来的"这个光标属于哪个控件"。
     * 共享边界上两侧字段落在同一个偏移上，光看位置判不出侧别 —— 见 setCaretAt。
     * 与 caret_field_at_ 配套：只有光标还停在这个位置上，提示才有效
     * （位置一变就自动失效，不需要在每个编辑入口手工清）。
     */
    int caret_field_ = -1;
    TextPos caret_field_at_{-1, 0};

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

    /**
     * 表单：控件区间集合与编辑策略（策略是纯函数，见 core/doc/edit_policy.h）。
     */
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


}  // namespace heditor
