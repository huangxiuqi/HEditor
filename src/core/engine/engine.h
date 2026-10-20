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
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/edit_mode.h"
#include "core/doc/flow.h"
#include "core/engine/caret.h"
#include "core/engine/edit_command.h"
#include "core/engine/engine_config.h"
#include "core/engine/engine_stats.h"
#include "core/layout/layout.h"
#include "core/render/bitmap.h"
#include "core/rev/capability.h"
#include "core/rev/flow.h"
#include "core/rev/project.h"
#include "core/rev/rev.h"
#include "core/rev/session.h"
#include "core/text/font.h"
#include "core/text/writing_mode.h"

namespace heditor {

class EditorEngine : private EditHost {
public:
    /**
     * fontPaths 的顺序决定字体索引
     */
    bool init(const std::vector<std::string>& fontPaths);

    void setConfig(const EngineConfig& cfg);
    const EngineConfig& config() const { return cfg_; }

    /**
     * 只改光标与选区的样式（颜色、光标宽度）。
     *
     * 为什么不复用 setConfig：那一处会作废底图，而底图全建要把窗口里的每一页重新光栅
     *   （3 页约 32 ms）。样式由界面上的取色器连续改时，每一下都付这笔钱。
     *   底图的内容与样式无关 —— 光标与选区都画在帧上，见 presentInternal。
     *
     * 不动底图，也不清 caret_rect_px_prev_ 与 selection_rects_prev_：那两份正是下一次
     *   呈现用来把旧样式的像素从帧上擦掉的依据，清了就会在帧上留下旧颜色。
     *   调用方改完要呈现一次（引擎不自己重绘）。
     */
    void setCaretAndSelectionStyle(const Color& caret, float caret_width_pt,
                                   const Color& selection);

    /**
     * 预热一页的字形：把它的 mask 光栅一遍送进字形缓存，不呈现、不改画面。
     *
     * 为什么要有它：滚动换窗那一拍最贵的部分是冷字形光栅 —— 2000 段长文档 @1.5 倍
     *   一页约 1500 个字形，冷缓存下实测 50~90 ms（热缓存只要 8~12 ms），而它必须
     *   在换窗那一拍里做完，于是"滚动时那几页还没画出来"。把这笔钱挪到当前画面还
     *   够用的空闲帧里先付掉，换窗就只剩铺底色与帧搬运。
     * 返回本次预热触及的字形数（0 = 这一页没有字形或还没排版）。
     */
    int prewarmPageGlyphs(int page);

    /**
     * 页度量：断行依赖内容宽度，所以改它必须整篇重排（影响面不可定位）。
     */
    void setPage(const PageMetrics& page) {
        if (page != cfg_.page) markAllLayoutDirty();
        cfg_.page = page;
    }
    /**
     * 排版输入。名单覆盖全部控件时它就是 blocks_full_（同一份，不拷贝）；
     * 只有真的筛掉了某些括号时才有另一份。
     */
    const std::vector<BlockSpec>& blocks() const { return filter_active_ ? blocks_ : blocks_full_; }
    /**
     * 与块流同一层的产物：表格结构（宽度、列宽、行规则、每格的跨度与块区间）。
     *
     * 单列一个读口是因为它与 blocks() 是两种形状：块流描述"排什么"，这里描述"框在哪"。
     * 只比块流看不出列宽与跨度的错，正如只比表格看不出文字。
     */
    const std::vector<LayoutTable>& tableSpecs() const { return tables_; }

    /**
     * 文档装载：把模型树换成一份修订（rev::fromTree），此后引擎只持修订，不再持树。
     *
     * 表示不了的形状（目前是多节）整份拒绝、状态停在"没有装载"——装一半的结果是一份看起来
     * 完整的修订，而它与模型不是同一份文档，调用方没有别的办法知道。控件边界、段落格式与
     * 字符格式因此仍只有一个产处（投影），引擎不自己拼。
     *
     * 隐藏控件的可见性口径与投影同一处取值（见 rebuildFromTree），所以 setEditMode 必须重建。
     * 装载会复位光标、作废底图并整篇标脏。
     */
    void setDocument(const NodeTree& tree, const StyleSheet& sheet);
    /**
     * 持有中的文档。引擎只认这一条装载入口，所以它一直有效（未装载时是一份空修订）。
     */
    const rev::Doc& document() const;

    /**
     * 段落坐标与模型流位置的桥接 —— 第十四节那条接缝的另一半。
     *
     * 引擎对外一直说 (段, 段内码点偏移)，模型层说流位置；两者不能靠算式互换 —— 控件与节这些
     * 容器在流里各占起止两位，而段的码点里没有它们。所以换算只认一个权威：编译给出的块来源
     * 身份（BlockSource）加 FlowIndex。桥是单向的：模型是真相，段坐标是它的一个视图。
     * 树是唯一的装载入口，所以桥一直存在；没装载过（段流为空）时入口返回 -1 与 false。
     *
     * owner_hint 是宿主与探针的说话方式（它们只认控件下标）：同一处边界上"控件之前 /
     * 进入控件"是两个位置，提示把它投影成一个控件下标。引擎内部一律说侧别，见 flowPosAtSide。
     */
    FlowPos flowPosAt(int para, size_t cp_offset, int owner_hint = -1) const;
    /**
     * 桥接的底层：位置 = (段, 码点偏移, 侧别)。侧别见 caretSide 的注释。
     */
    FlowPos flowPosAtSide(int para, size_t cp_offset, int side) const;
    /**
     * 反向：流位置 → (段, 段内码点偏移)。落在段间（段落标记、节边界）上时归到相邻段的端点上
     * —— 模型里的位置只有"内容上"的，没有"段间"的。不持树时返回 false。
     */
    bool flowToTextPos(FlowPos flow, TextPos* out) const;

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
     * 视口锚点：帧只呈现 [first_page, first_page+page_count) 这几页。
     * 帧是一张位图，高度就是"宿主那张画布有多高"—— 画布越大，浏览器每次上屏要重新
     * 合成的图层就越大。实测（scripts/diag_perf.mjs）：18.3 Mpx 的帧比 1.13 Mpx 的帧
     * 每帧多花整整一个 vsync，这一笔既不在引擎计时里、也不在 putImageData 里。
     *
     * page_count <= 0 表示"整篇"（默认）：默认值下行为与没有这个机制时逐字节相同，
     * 单测与既有探针一行都不用改。但产品宿主必须记得设它 —— 整篇帧在长文档上是内存与
     * 耗时的双重放大：2000 段（65 页）实测每实例堆 82.6 MB、装载 115 ms（窗口 3 页），
     * 对 1031.6 MB、1583 ms（整篇帧）。同一份文档，差一个量级。
     */
    void setViewportAnchor(int first_page, int page_count);

    /**
     * 本次呈现覆盖到的页（绝对页号，升序去重）。
     * 宿主据此决定"哪几张页画布要重贴" —— 它比"脏页数"更准：光标与表单标记
     * 画在帧上而不进底图，它们所在的那一页同样要重贴，而页指纹里没有这两样。
     */
    const std::vector<int>& dirtyPages() const { return last_dirty_pages_; }
    /**
     * 帧当前覆盖的首页（= 宿主算页偏移与命中换算的基准）。
     */
    int windowFirstPage() const { return base_first_page_; }
    /**
     * 帧槽数：页在帧内的行偏移 = (页号 mod 槽数) x 步距。由宿主侧算好传进来
     *   （它等于窗口页数上限 maxWindowPages，与宿主的画布槽数是同一个数）。
     * 为什么不是引擎自己算：引擎不知道视口尺寸；而两侧各算一次会分叉 ——
     *   画布槽那边实测出过分叉（宿主按"已建槽数"只增、引擎按"当前几何"可减，
     *   视口从很高变矮时槽号整体错位、画布上是别的页）。
     */
    void setFrameSlots(int n);
    int frameSlots() const { return frame_slot_count_; }
    /**
     * 帧当前覆盖的页数。宿主按它把窗口位图切成几块贴给页画布。
     */
    int windowPageCount() const { return base_page_count_; }

    /**
     * 页在帧像素下的几何。宿主用它把"第 k 张页画布 + 页内 y"换算成帧坐标，
     * 以及给页画布定位。三者都是 cfg_ 与 zoom 的纯函数。
     */
    int pagePxWidth() const;
    int pagePxHeight() const;
    int pageGapPx() const;

    /**
     * 光标。对外坐标仍是 (段, 段内码点偏移)，任何设置入口都会先夹到合法范围、
     * 再吸附到字素簇边界 —— 所以光标永远不会停在半个 é（e + U+0301）中间。
     *
     * 引擎内部的真源是一对全局流位置（selection_ 的 anchor 与 focus，见它的注释），
     * 段号与段内流位置都由 focus 投影出来。下面这些入口都是"把别的坐标系翻成它"，
     * 对外接口一个字没变。
     */
    void setCaret(int para, size_t offset);
    void setCaret(TextPos p) { setCaret(p.para, p.offset); }
    /**
     * 带"归属"的定位：点画布时用户点的是某个字段的括号，位置本身（共享边界上的
     * 那个偏移）分不出是哪一侧，于是把结论一起带进来。归属是侧别的投影，见 setCaretAtSide。
     */
    void setCaretAt(int para, size_t offset, int field);
    void setCaretAt(TextPos p, int field) { setCaretAt(p.para, p.offset, field); }
    /**
     * 带侧别的定位：点击（宿主把 hitTestPt 量到的侧别带回来）与文档重建后搬回光标走它。
     * 侧别见 caretSide 的注释。
     */
    void setCaretAtSide(int para, size_t offset, int side);
    void setCaretAtSide(TextPos p, int side) { setCaretAtSide(p.para, p.offset, side); }
    /**
     * 带侧别与行别的定位（点击落点）。
     *
     * 行别说的是"折行处贴哪一行"：一行排满折到下一行时，上一行的末尾与下一行的开头
     * 是同一个位置（偏移与侧别都一样），光凭位置分不开。默认贴上一行行尾；点下一行的
     * 行首时点明贴下一行，光标才画在点的地方。行别见 caretLineAffinity。
     */
    void setCaretAtSideLine(int para, size_t offset, int side, int line_aff);
    void setFocusAtSideLine(int para, size_t offset, int side, int line_aff);
    /**
     * 拖选：把焦点端搬到点击位置，anchor 端不动。
     *
     * 与 setCaretAtSide 只差收尾方式：那边两端一起摆到新位置（定位），这边只动 focus。
     * 按下时宿主先用 setCaretAtSide 定位，于是 anchor 落在按下点；拖动过程中的每一次
     * 移动都走这里，选区就是从按下点到当前位置那一段。侧别的含义与 setCaretAtSide 相同。
     */
    void setFocusAtSide(int para, size_t offset, int side);
    /**
     * 把光标摆到一个流位置上（绝对）。编辑路径全用它：删除后的落点是删除区间的起点、
     * 插入后是插入区间的终点、Tab 的目标、撤销恢复的位置，它们都已经是流位置。
     */
    bool setCaretAtFlow(FlowPos flow);

    /**
     * 选区：一对流位置，带方向（anchor 是不动的那一端）。
     *
     * 引擎从第一天起就只存这一个值（见 selection_ 的注释），此前对外只露它的 focus；
     * 下面这几个入口把它按选区露出来。空选区（两端重合）就是光标 —— 所以
     * hasSelection() 为假时 caret() 给出的就是它的位置。有选区时 caret() 是插入点：
     * 文字选区在 focus 那一端，整格选区在焦点那一格的末尾。两者始终只有一份状态。
     */
    Range selection() const { return caret_.selection(); }
    /**
     * 选区非空：两端不重合。它判的是"有没有选中东西"，不是"光标在不在"。
     */
    bool hasSelection() const { return caret_.hasSelection(); }
    /**
     * 程序化设选区（宿主恢复、探针摆位）。两端各自夹到文档流的合法范围内。
     * 传空区间就等于定位。
     *
     * 这里不做端点规范化（外扩到控件边界）：那是"要取出片段"时才需要的性质
     * （见 range.h 的 normalizeRange），而拖选出来的端点本来就该停在用户指的地方。
     */
    bool setSelection(Range r);
    /**
     * 取消选区：anchor 收到 focus，焦点端不动 —— 光标留在原地。
     */
    void clearSelection();
    /**
     * 全选：文档首到文档末。
     *
     * 末端的口径是"最后一块的段内末端"，不是最后一块之后：段间那个段落标记不属于
     * 任何一段的内容，够不到它。
     */
    bool selectAll();

    /**
     * 整格选区：按"段号"选它所在的那一格（渲染整格铺满，删除逐格清空）。
     *
     * 收段号而不是流区间：一格的身份是它占的块区间（见 CaretResolver::CellBlocks）——
     * 空格的内容长度为 0，流区间退化成点，与下一格的第一段共用同一个流位置，分不开。
     * 段号来自命中（hitTestPt 的 out_cell_para），因为"哪一格"是几何问题 —— 引擎不提供
     * 按表号行号列号选格那一层：跨页拆行时行的逻辑号与画面对不上，而用户点的是画面。
     */
    bool selectCellAtPara(int para);
    /**
     * 选区是不是"单元格选区"。文字选区与它是两条独立的操作线，靠这个标记分开 ——
     * 不靠"选中了多少"去猜（见 CaretResolver::selectionIsCell）。
     */
    bool selectionIsCell() const { return caret_.selectionIsCell(); }
    /**
     * 选区是否吸附到了格边界上（跨格拖选、单击边距带、三击）。全选为假：它按整篇删。
     */
    bool hasCellRange() const { return caret_.hasCellRange(); }

    /**
     * 这一段内容所在那一格的内容区间（全局流位置，左闭右开）。不在表格里返回假。
     *
     * 空格的内容长度是 0（两端相等），所以它同时也是"这一格有没有字"的判据。
     * 嵌套表格取"包含这一段的最短区间" = 最内层那一格 —— 外层格把内层表的段也算进去，
     * 按最短挑才不会被外层截走。
     */
    bool cellFlowRangeAt(int para, FlowPos* lo, FlowPos* hi) const;

    /**
     * 光标位置（派生）。段号 + 段内码点偏移，由光标的流位置投影出来 —— 不是独立状态。
     * 整格选区时它是焦点那一格的末尾（插入点不跟着鼠标在格内跑）。
     */
    TextPos caret() const;
    /**
     * 光标当前的归属（-1 = 不在任何控件里；LineBox::kInlineOffsetOuter = 在所有控件之外）。
     * 由 focus 投影出来的段内流位置直接查，不是独立状态。
     */
    int caretFieldHint() const;
    /**
     * 光标的侧别（派生）：在它那个码点偏移上跨过了几枚边界括号（0 = 在所有括号之前）。
     * 与 caret() 合起来才是光标的完整位置 —— 同一个偏移上可能有好几个插入点
     * （控件之前 / 进入控件 / 内容末尾 / 控件之后），光有偏移表达不了。
     * 它是段内流位置与码点偏移的差，而两者都由 focus 投影 —— 这一路没有中间状态，
     * 所以不可能与光标位置不同步。这就是它取代 caret_side_ 与 caret_side_at_ 那两个
     * 字段的理由（那两个字面量天然可能失配，才需要失效标记兜）。
     */
    int caretSide() const;
    /**
     * 光标当前的行别：折行处贴上一行行尾（0）还是贴下一行行首（1）。
     *
     * 它只解决一件事：折行把一个位置落在两行之间。段内第 k 个位置既是上一行的末尾，
     * 也是下一行的开头，偏移与侧别都一样 —— 只按位置找行，找回来的必然是上一行，
     * 于是"点下一行的行首"变成"光标跳到上一行行尾"。行别是那个被丢掉的自由度。
     *
     * 它不是文档状态：任何不带行别的定位都会把它清成 0（贴上一行），与上下键的列位
     * 同一待遇（见 vertical_goal_valid_）。写完字之后光标本来就该贴在前一个字后面，
     * 而那个位置在上一行的末尾。
     */
    int caretLineAffinity() const { return caret_.lineAffinity(); }
    /**
     * 光标是否落在竖排的段里（宿主据此把方向键按物理方向映射，见 keyToOp）。
     */
    bool caretVertical() const;
    /**
     * 光标所在段的列序是否反向（中日竖排为真：第一列在最右；蒙文为假：第一列在最左）。
     *
     * 方向键要用它：竖排下左右是换列，而"下一列在哪一边"由列序决定 ——
     * 中日竖排往左换列，蒙文往右换列。只按"是竖排吗"映射会把蒙文的左右按反。
     */
    bool caretBlockReversed() const;
    /**
     * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
     * 这是文本坐标投影到流位置的唯一约定 —— 只给"没有侧别信息"的入口用（宿主程序化定位、
     * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel，侧别由操作自己算出来。
     */
    void setCaretByText(TextPos p);
    /**
     * 按"用户感知字符"（簇）移动。dcp > 0 向后、< 0 向前。
     * 返回 true 表示光标真的动了（没动就别重绘）。
     *
     * extend 为真时锚点不动：Shift+方向 是从锚点那一端继续扩展，而不是每次重新起头。
     * 锚点不需要"进入扩展态"这样一个额外状态 —— 第一次按 Shift 时它自然就是那时的
     * 光标位置（此前所有非扩展移动都把两端收到一起）。
     */
    bool moveCaretBy(int dcp, bool extend = false);
    /**
     * 到行首/行尾，不是段首/段尾 —— 这才是 Home/End 该有的语义。
     */
    bool moveCaretToLineStart(bool extend = false);
    bool moveCaretToLineEnd(bool extend = false);
    /**
     * 到文档首 / 文档末（Ctrl+Home / Ctrl+End）。
     *
     * 与行首行尾分开：段与行是两个概念，而"整篇的开头"还要再退过节点边界。
     */
    bool moveCaretToDocStart(bool extend = false);
    bool moveCaretToDocEnd(bool extend = false);
    /**
     * 上下键：沿块轴走到相邻行，列位沿行内轴保持。
     *
     * dir < 0 向上、> 0 向下。跨短行时列位不丢（列位在第一次按时取自光标当前的行内
     * 坐标，之后连续按都沿用）。列位是会话状态不是文档状态，任何非扩展的定位都会清掉它
     * （见 setCaretRel）—— 不清的症状是"鼠标点去别处，再按上还跳回那一列"。
     */
    bool moveCaretVertical(int dir, bool extend = false);
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
    bool moveCaretAndRender(int dcp, bool extend = false);
    /**
     * Home / End：到行首 / 行尾。
     *
     * 不用 moveCaret(±1e9) 实现：那样依赖 moveCaretBy 走不动时提前 break，且语义不同。
     */
    bool moveCaretToLineStartAndRender(bool extend = false);
    bool moveCaretToLineEndAndRender(bool extend = false);
    bool moveCaretToDocStartAndRender(bool extend = false);
    bool moveCaretToDocEndAndRender(bool extend = false);
    bool moveCaretVerticalAndRender(int dir, bool extend = false);
    bool selectAllAndRender();
    bool setSelectionAndRender(Range r);
    bool selectCellAtParaAndRender(int para);

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

    void setEditMode(EditMode m);
    EditMode editMode() const { return mode_; }

    /**
     * 当前光标所在的最内层控件下标（-1 = 不在任何控件内）。
     * 由光标位置与侧别推出来（见 caretFieldHint）；推不出来时（光标落在所有控件之外）
     * 回落成"最内层含端点"—— 与编辑归属同一口径。
     *
     * 这条回落不只是为了好看：bracketedControls() 在聚焦档下要问它，而边界对象表
     * 又是由 bracketedControls() 决定的 —— 少了回落，首次排版前会互相等成死结
     * （一个括号都排不出来，于是更推不出归属）。
     */
    int focusedControlIndex() const {
        const int hint = caretFieldHint();
        if (hint >= 0) return hint;
        return controls_.empty() ? -1 : controls_.innermostAt(caret(), true);
    }

    /**
     * 占位簇上的 owner 句柄 → 对外控件下标。
     *
     * owner 是控件的节点 id（第五刀：序号会在插入之后整体平移，而节点 id 是身份）。
     * 对外（宿主、探针、既有判据）的口径始终是下标，所以"拿下标去和占位簇的
     * owner_control 比"的地方都要先过这里 —— 宿主报"这枚括号归谁"正是其中一处。
     */
    int controlIndexOfOwner(int owner) const {
        const auto it = owner_index_.find(owner);
        return it == owner_index_.end() ? -1 : it->second;
    }

    /**
     * 反方向：对外控件下标 → 块流对象表里的 owner 句柄。
     *
     * caretRectFor 拿它把光标归属（下标）换成句柄再去问排版 —— 比反了的表现是"共享边界上
     * 光标画到另一侧去"，位置差一格，只有像素级判据看得出来。
     * 负数（没有归属、贴外侧哨兵）原样传下去：它们在排版侧是同一种含义的另一种取值。
     */
    int slotOwnerOf(int control_index) const {
        if (control_index < 0) return control_index;
        if (control_index >= static_cast<int>(control_node_ids_.size())) return -1;
        return static_cast<int>(control_node_ids_[static_cast<size_t>(control_index)]);
    }

    /**
     * 要显示括号（= 要在排版里占宽度）的控件下标 —— 产生端的名单，只有引擎认识
     * 「聚焦」与编辑模式。syncInlineObjects() 按它决定往对象表里写哪些边界。
     * 空 = 一个占位都不插，排版与"没有表单"逐字节一致。
     *
     * 文档与表单都要（前者是设计态：看不见字段就无从增删改），只有只读不画 ——
     * 它是呈现与打印态，括号落进版面就印出来了。
     */
    std::vector<int> bracketedControls() const {
        std::vector<int> out;
        if (mode_ == EditMode::ReadOnly || controls_.empty()) return out;
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
     * 为什么不能用 focusedControlIndex() 代替它：只读模式下下标着同一份文档、括号根本不画，
     * 却会照样把正文推开一截 —— 凭空多出来的空隙，谁也没要。
     */
    int bracketPlanId() const {
        if (mode_ == EditMode::ReadOnly || controls_.empty()) return -1;
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
     * 撤销与重做：历史由会话持有，每格是一份旧修订。
     *
     * 撤销不是"反向应用逆操作"，而是把旧修订拿回来 —— 修订不可变，所以拿回来的是同一份内容，
     * 不是重放出来的近似。两份修订一减就得到损伤区间，投影据此只重算那一段。
     * 修订不记光标，所以撤销栈旁边另有一份选区快照、与它同步裁剪（见 recordSelection）。
     * 没装载过文档时没有会话，四个入口一律返回 false，不假装能做。
     */
    bool canUndo() const { return session_ && session_->canUndo(); }
    bool canRedo() const { return session_ && session_->canRedo(); }
    bool undo();
    bool redo();
    bool undoAndRender();
    bool redoAndRender(); /**
     * 文档坐标（磅）落在哪个控件上（取最内层）。-1 = 没有。
     * 它走的是 hitTestPt 的同一份几何，所以"视觉上在框里"与
     * "逻辑上在控件里"永远一致。
     */
    int controlAtPt(float x_pt_doc, float y_pt_doc) const;

    /**
     * 页面坐标下的一个矩形（页号 + 页面坐标磅矩形）。
     *
     * 定义已提到 utils/types.h：光标与选区那一层（CaretResolver）也要用它，
     * 而那一层不该反过来依赖引擎头。这里留一个同名别名，既有调用点不必动。
     */
    using PageRect = heditor::PageRect;
    /**
     * 控件矩形的原名。改名只是把"页号 + 页面矩形"从控件里提出来，调用点不必动。
     */
    using ControlRect = PageRect;
    std::vector<ControlRect> controlRects(int index) const;
    /**
     * 选区 → 页面坐标矩形（跨行拆、跨页分段）。空选区是空的 —— 没有选区就没有要画的。
     *
     * 只返回落在页范围 [first_page, first_page + count) 里的那部分；count <= 0 表示不限页。
     *
     * 为什么要能限页：整篇选中的矩形条数是"文档行数"这个量级（2000 段 ≈ 4000 条），
     *   而呈现与状态推送每拍都要过一遍。不限页时，全选之后每一拍都要造 4000 条矩形、
     *   把它们全刻进擦除清单、再逐条混合一遍 —— 其中九成以上落在当前帧之外，
     *   画不到也擦不着（量到的是单线程宿主 present 从 3.6 ms 涨到 9.8 ms，
     *   Worker 宿主每拍状态消息里那一份 197 KB）。
     *   限页之后这两笔都只随"看得见的行数"走，与选区多大无关。
     */
    std::vector<PageRect> selectionRectsIn(int first_page, int count) const;

    /**
     * 整篇的选区矩形。判据与审计用它（要看"选区到底盖了多少行"），
     * 呈现与状态推送不要用它 —— 用 selectionRectsIn。
     */
    std::vector<PageRect> selectionRects() const;

    /**
     * 当前视口窗口内的选区矩形。宿主取证入口用它，口径与"这一帧画了什么"一致。
     * 视口没钉过（vp_count_ <= 0）时限页失效，退化成整篇。
     */
    std::vector<PageRect> selectionRectsVisible() const;

    /**
     * 删除一个控件。keep_content 为真时拆壳留内容（那段字符变成普通正文），假时连内容一起删。
     *
     * 策略走 canDoStructureOp（structureLock 默认锁住）：表单模式默认拒绝、文档模式放行 ——
     * 增删字段是设计期的事。删完段数可能变，所以走全量重编译，不进增量那条路。
     *
     * @param index        控件下标（对外一律用下标，节点 id 不外露）
     * @param keep_content 真 = 保留内容，假 = 连内容一起删
     * @param caret_before 删除之前光标在哪（流位置），撤销恢复的就是它。键盘那一路必须传；不传
     *                     （-1）时按"选中整个控件"记，撤销会把控件重新选中、光标落到闭括号之后。
     * @return 是否真的改了文档
     */
    bool removeControl(int index, bool keep_content, FlowPos caret_before = -1);

    /**
     * 删除控件 + 重排 + 呈现的组合入口（理由同上面那组 AndRender）。
     * 宿主侧一次调用就收尾，不必自己记得再排一次 —— 漏掉那一步的症状是
     * "控件没了但括号还在画布上"。
     */
    bool removeControlAndRender(int index, bool keep_content);

    /**
     * 改控件的一个运行时属性（hidden / readonly / required / structureLock）。
     * 与 removeControl 同一套策略门；设计期属性（类型、格式、选项）不走这里。
     */
    bool setControlProp(int index, ControlProp prop, bool value);

    /**
     * 改控件属性 + 重排 + 呈现的组合入口。hidden 会改呈现态的流结构（整棵摘除），
     * 所以这一次重排不能省。幂等（值没变）时它和 setControlProp 一样返回 true，
     * 但不会白跑一次重排与重绘。
     */
    bool setControlPropAndRender(int index, ControlProp prop, bool value);

    /**
     * Tab / Shift+Tab：把光标移到下一个（上一个）可编辑控件的起点。
     * 到头会绕回第一个 —— 表单里 Tab 就该是循环的。
     */
    bool focusControl(bool forward);

    /**
     * Tab / Shift+Tab 的策略版：走模型层的能力表（edit_capability 的 tabFrom），
     * 到最后一个可编辑控件就放行（返回 false，不消费该 Tab），不绕回第一个。
     *
     * 与 focusControl 的差别不只是循环与否：focusControl 认的是控件集合的区间起点，
     * 而 tabFrom 认的是"内容起点落在可编辑集合内"这个谓词 —— 于是"只读控件里嵌的
     * 可编辑控件"不会成为落点，反之亦然，两处口径不会各说各话。
     */
    bool tabCaret(bool backward);
    /**
     * 同上，带收尾重绘。
     */
    bool tabCaretAndRender(bool backward);

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
     * 底图自审：把底图按"从零全建"重来一遍，与现在这一份逐字节比，返回
     * {"diff":N,"frameW":..,"frameH":..,"box":[x0,y0,x1,y1]}。
     *
     * 为什么必须单独有一条（auditFrameJson 盖不住）：帧是从底图搬过来的，
     *   底图本身错了，帧与"整帧重贴"当然一致 —— 两条都绿，画面却是错的。
     *   而"换窗时平移复用重叠页"这条优化只动底图，正是这一条要看的对象。
     *   判据：同一窗口下，平移复用得到的底图必须与全建逐字节相同。
     * 跑完顺便把底图修成真值（与 auditFrameJson 同一口径：自审不掩盖问题）。
     */
    std::string auditBaseJson();

    /**
     * 排版自审：用"不用缓存的全量重排"做一份对照，与当前排版结果逐字段比对，
     * 返回不符项数（0 = 一致）。
     * 理由与 auditFrameJson 相同：增量的正确性由"与全量一致"定义，
     * 而不是由"看起来对"定义。逐字段而不是只比行数：增量最典型的错法是
     * "少排了一段"或"前缀多留了一段"，两者都只差几行。
     */
    int auditLayout();

    /**
     * 表格结构自审：块与格这两份记录是否互相撑得住，返回不符项数（0 = 一致）。
     *
     * 为什么不并进 auditLayout：那一条比的是"增量排版与全量排版"，两边吃的是同一份
     *   blocks_ 与 tables_，结构本身坏了它照样全绿 —— 一格连一个块都不剩时，两边都排不出
     *   那一格的行盒，逐字段比对完全一致。而"格没有块""块没有结束标记"这类损坏恰恰是表格
     *   编辑最典型的失败方式：后果是那一格连行盒都没有，光标点不进去，画面上只是少了几行字。
     *
     * 查六条：格至少一个块；格的块区间在表内且互不重叠；格里的块都有结束标记；块的
     *   cell_path 与格给出的区间一致（两份记录互为对照）；格行列跨度不越界；表句柄在块
     *   路径里指同一张表。
     */
    int auditTables() const;

    /**
     * 同上的 JSON 版，供宿主与探针取证（含各类不符的计数与首个位置）。
     */
    std::string auditTablesJson() const;

    /**
     * 同上的 JSON 版，供宿主取证（含两侧规模、首个不符位置与两个耗时）。
     */
    std::string auditLayoutJson();

    /**
     * 字形页索引自审：逐页验"二分出来的区间"与"全篇筛一遍"是否给出同一批字形。
     *
     * 为什么要单开一条：绘制侧改成按页二分之后，auditBaseJson 变得盖不住它了 ——
     *   那条是"强制底图全建再与现在比"，而全建走的是同一条二分路径，两边错得一样，
     *   逐字节比照样全绿（这正是本条存在的理由：同一个代码路径当判据必然恒绿）。
     * 判据取双向：区间内每个字形的 page 必须等于该页（不漏），并且全篇里属于该页的
     *   字形数必须等于区间长度（不多不少）—— 只查前者会漏掉"区间里混进别页"，
     *   只查后者会漏掉"区间错位但长度对"。
     * 没有比对对象时返回空结果（diff 0、checked 0），不做假绿。
     */
    std::string auditGlyphPagesJson() const;

    /**
     * 编译自审：拿"整树全量编译"做一份对照，与当前产物逐字段比对，返回不符项数。
     *
     * 与 auditLayout 同一个理由：编译这一层的增量的正确性由"与全量一致"定义。
     * 只比段数看不出来 —— 增量最典型的错法是"某一段的产物还是旧的"，
     * 而段数、行数全都对得上。没装载过文档时返回 0：那时没有可比的东西。
     */
    int auditCompile() const;

    /**
     * 同上的 JSON 版：给出本轮实际编译耗时与一次全量编译耗时，两者同源。
     * 这是"增量到底省了多少"的唯一证据 —— 没有分母的耗时数说明不了任何事。
     */
    std::string auditCompileJson();

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
     * 反查：文档坐标（磅）→ (段, 偏移) + 侧别 + 行别；点落在页间空隙时返回 false。
     * 文档坐标以第 0 页顶边为原点，与帧怎么摆无关（帧按槽号装页，拿槽号反查会整体错位）。
     * out_side 是落点侧别（同一偏移上可能压着几枚括号），out_owner 是它的投影，只给人看。
     * out_line_aff 是折行处的行别：点下一行行首时大于 0，不带回去会画到上一行行尾。
     * out_cell_para：落在格子的边距带上时给出这一格的段号（否则 -1）。
     *   边距带 = 格边框到文字左沿之间那一条：点这里选整格，点字上是文字光标。
     *   空格没有文字可挡，整格都是边距带 —— 点空格的任何位置都选中这一格。
     *   给段号而不是流区间：一格的身份是它占的块区间，空格的流区间退化成一个点，
     *   与下一格的第一段共用同一个流位置（见 CaretResolver::CellBlocks）。
     */
    bool hitTestPt(float x_pt_doc, float y_pt_doc, TextPos* out, int* out_owner = nullptr,
                   int* out_side = nullptr, int* out_line_aff = nullptr,
                   int* out_cell_para = nullptr) const;

    /**
     * 按点选整格（三击的入口）：点在格里的任何位置都选中这一格。
     *
     * 单击的"整格"只发生在格左侧的边距带上（见 hitTestPt 的 out_cell_para）—— 那是一条
     * 独立的操作线；三击则是用户明确说"我要这一格"，点字中间也算。点不在表格里返回假。
     */
    bool selectCellAtPt(float x_pt_doc, float y_pt_doc);

    const uint8_t* frame() const { return bitmap_.data(); }
    int frameWidth() const { return bitmap_.width(); }
    int frameHeight() const { return bitmap_.height(); }
    size_t frameBytes() const;

    /**
     * 内容底边（帧像素）：最后一行墨迹的底，再加一个下边距。
     *
     * 连页模式下宿主拿它当滚动高度 —— 分页下滚动高度是"页数 x 步距"（页是实在的），
     *   连页下没有页可数，只有内容。分页下这个值同样成立（页号 x 条带高 + 行底），
     *   只是没人用它。
     */
    int contentBottomPx() const;

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

    /**
     * 落点判定的唯一入口：一次编辑能不能落在这里 / 能不能删掉这个区间。
     *
     * 六个编辑入口（插入、退格、前删、分段、合并、反缩进）全部走它，所以"判定与动作
     * 一致"是结构上的事实，不是一条要另外维护的约定。对外也开放：宿主拿它做禁用态提示，
     * 判据拿它把"能不能编辑"与"光标能不能到"分开 —— 表单模式下 setCaret 会把光标
     * 投影进最近的字段，"控件之外"那个位置根本到不了，只有直接问它才问得到。
     *
     * 落点的侧别由位置自己推（见 sideForPos）：光标那一处用光标自己的侧别，别处按
     * "最内层含端点"归一次。不另收归属提示 —— 那是"控件下标"这套说话方式，只有
     * 区间判定需要它，而落点判定已经改说侧别了。
     */
    rev::EditCheck policyAt(TextPos p) const;
    /**
     * 删除区间 [a, b) 的判定。跨段删除一律拒（档 1 的删除不跨段，段落标记的删除走合并）。
     */
    rev::EditCheck policyErase(TextPos a, TextPos b) const;

private:
    /**
     * 按当前修订重算产物：投影出块流与控件跨度，再把流索引、位置与控件视图带上。
     * 隐藏控件的可见性口径按当前编辑模式取（见 flowVisibility）。
     */
    void rebuildFromTree(const rev::BlockDamage* dmg = nullptr);

    /**
     * 重建的收尾：流索引、段的流区间递推、控件视图。
     *
     * 控件跨度由投影给（它走树时记下的），不在这里重扫对象表 —— 见 exportControls。
     */
    void finishRebuildFromTree(FlowVisibility vis, const std::vector<rev::ControlSpan>& controls);

    /**
     * 隐藏控件的可见性口径。编译与流索引必须同源（非编辑模式下隐藏的子树整棵摘除），
     * 所以只在这里取一次，两个入口都用它。
     */
    FlowVisibility flowVisibility() const;
    /**
     * 命令层的出口（EditHost）：把一批步骤落到修订上，再把产物按新修订重算一次。
     *
     * 重编译是必须的而不是图省事：块流、流索引、块起点、来源身份、控件集合全是修订的派生量，
     * 少重建任何一个，下一轮就会拿旧修订的位置去对新修订的块。失败时状态一个字节都不动 ——
     * 会话层保证"被拒的编辑不占撤销栈"，这里再保证产物不跟着半途改。
     */
    bool commitSteps(std::vector<Step> steps, int dirty_para, Range selection_before,
                     Range selection_after) override;

    /**
     * 撤销上限（会话要的那个数）。配置里的 0 是"不限制"，与"不留历史"不是一回事。
     */
    int undoLimit() const;

    /**
     * 记下这一批步骤对应的两份选区，并与撤销栈同步裁剪。
     */
    void recordSelection(Range before, Range after);
    /**
     * 从投影给出的跨度建控件集合。
     *
     * 跨度是投影走树时记下的（起止块号与段内码点，见 rev::ControlSpan），业务标识与锁定态
     * 就在节点载荷里 —— 所以这一层不再扫块流的对象表，也不再对每个控件 findNode 一次。
     * 两边各算一遍的代价不只是慢：区间一旦分叉，界面上只表现为"某个字段锁不住"或"括号画在
     * 别处"，而两份产物各自看都"对"。
     */
    void exportControls(const std::vector<rev::ControlSpan>& spans);
    /**
     * 选区矩形（帧像素坐标，与 caretRectPx 同口径）。只取落在页范围
     * 页范围 [first_page, first_page + count) 里的那部分；count <= 0 表示不限页。
     */
    std::vector<Rect> selectionRectsPxIn(int first_page, int count) const;
    /**
     * 整篇的选区矩形（帧像素坐标）。判据/取证用它，呈现不要用 —— 用 selectionRectsPxIn。
     */
    std::vector<Rect> selectionRectsPx() const;
    /**
     * 把"文本坐标 + 侧别"投影成 rel 并摆好（含重排）。side < 0 = 按"最内层含端点"投影。
     * extend 为真时不动锚点（Shift+Home/End 用）。line_aff 见 caretLineAffinity。
     */
    void projectCaret(int para, size_t offset, int side, bool extend = false, int line_aff = 0);
    /**
     * 光标矩形（帧像素坐标）。caret_visible 为 false 或找不到行盒时返回零矩形。
     */
    Rect caretRectPx() const; /**
     * 光标所在页（绝对页号；-1 = 找不到行盒）。
     * 窗口化之后这是必需的一个数：帧只装窗口内的页，光标不在窗口里就既画不出来、
     * 也点不中它 —— 宿主拿它决定窗口该锚在哪。
     */
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
    /**
     * 行指纹：与页指纹同一次遍历里算出来，只是分桶到行（见 line_fp_new_）。
     * @p dirty_lo / @p dirty_hi 是脏页范围，与页指纹同一口径；同样只重算落在这几页里的行。
     */
    void computeLineFingerprints(int dirty_lo, int dirty_hi, bool full);
    /**
     * 把 [first_page, 末页) 直接标成"与底图那一版不同"，不去哈希。
     *
     * 用在后缀整体平移那一级：后缀的每一行都沿块轴挪了位，于是它在的每一页内容都变了 ——
     * 这个答案在进这里之前就已经确定，再把几百页的字形哈希一遍纯属白花
     * （实测 161 页 = 21 ms，比排版本身贵一个数量级，而它在"回车/合并/敲到换行"上每一下都付）。
     * 页数变多时新页也落在 [first_page, 末页) 里，一并标脏，不必退回整篇重算。
     */
    void markPagesChangedFrom(int first_page);
    /**
     * 把当前行指纹与行盒几何存成"底图里那一版"。
     * 与 page_fp_in_base_ 同一时机调用：底图变了就要跟着换。
     */
    void snapshotLineGeometry();
    /**
     * 行带边界：第 k 行独占的块轴区间（页内磅），上下都取与相邻行盒的中线。
     * 见 engine_paint.cpp 的实现注释（为什么不能用行盒自己的 block_lo/hi）。
     */
    void lineBand(int k, float* lo, float* hi) const;
    /**
     * 第 @p page 页在帧内的首行偏移（行）。
     *
     * 一维布局下就是 (页号 - 底图首页) x 步距。页在帧里的位置只有这一个算法 ——
     *   原先同一个算式散在七处（页矩形、画入底图、只重贴一条带、表单括号、焦点小点、
     *   光标矩形、控件矩形），改布局时漏掉任何一处都只表现为"某一层偏几页"，而其余全对。
     * 帧改造（按页号分槽，见 out/_design_slotframe.md）只需要换这一个函数的实现。
     */
    int frameOffsetOfPage(int page) const;
    /**
     * 第 @p page 页上的字形下标区间（半开区间 [begin, end)）；这一页没有字形时两者相等。
     *
     * 依据：排版按段流顺序 push 字形，页号只前进不后退，所以 layout_.glyphs 的 page
     *   单调不降 —— 两端各二分一次即可。为什么要二分而不能整篇筛：绘制是按页进行的，
     *   而整篇筛是 O(全篇字形)，每画一页就付一遍；2000 段文档约 8 万个字形，
     *   反推约 2 ms/页，比真正画那一页的字形还贵。
     *   单调性是推理 + 运行期审计（auditGlyphPagesJson）双重保证的，不是假设。
     */
    void glyphRangeOnPage(int page, int* begin, int* end) const;
    /**
     * 第 p 段的第一行落在哪一页；这一段没有行盒时返回 -1。
     * 截断之前读一次、重排之后再读一次，就能框出"这一轮可能动到的最靠前那一页"。
     */
    int firstPageOfParagraph(int p) const;
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
     * 后缀复用：只重排脏区，然后试着不动后缀。
     *
     * 两级判据从严到宽：一是同状态 —— 后缀的断页、行号与坐标都与上一轮逐位相同，整块搬回；
     * 二是后缀纯横排 —— 只有块轴起点变了，于是每行只是"平移 + 改页码"。段数变化（回车分段 /
     * 退格合并）几乎总落到第二级：段落边界一变就多让或少让一次段间距，后缀必然整体下移。
     *
     * @param page_lo,page_hi 出参：脏区新旧两版覆盖的页区间，供页指纹只重算这几页，-1 = 整篇。
     * @param relaid_paragraphs 出参：脏区在新版里占几段。
     * @param all_pages_changed 出参：Shifted 那一级专用 —— 后缀在的每一页都变了，直接标脏。
     * @return None 时后缀未被动过（调用方可以安全地走续排），其余两种排版已完成。
     */
    SuffixReuse trySuffixReuse(int from, int* page_lo, int* page_hi, int* relaid_paragraphs,
                               bool* all_pages_changed);

    /**
     * 后缀整体平移：把 [from, 末段] 的每一行沿块轴挪到新的位置，并重编页码与全局行号，
     * 字形跟着它的行一起挪。断行结果（每行的簇与行内位置）完全不重算。
     *
     * @param from 后缀的第一段（含）；段数变化时它与脏区首段相差不止一（脏区可能占几段）。
     * @param first_glyph 后缀的第一个字形在 glyphs 里的下标（脏区的字形之后）。
     *
     * 为什么可以这样做：横排的块轴推进是一条纯折线 —— 每行占 blocks[p].size_pt *
     * line_factor、段间再让出 space_after_pt，放不下就换页。所以只要知道后缀开始时的那一个
     * 续排状态（页号 + cursor_y + 行号），整个后缀的新位置就被唯一确定；这条规则与
     * layoutDocumentRange 横排分支里的断页判据同源，分叉会被 auditLayout 抓住。
     * 竖排段不吃这条规则，所以后缀里出现竖排就返回 false。
     */
    bool shiftSuffixBlockAxis(int from, size_t first_glyph, const ReflowState& after);

    /**
     * 后缀（[from+1, 末段]）里有没有竖排段 —— 竖排不能走平移复用。
     */
    bool suffixIsHorizontalOnly(int from) const;

    /**
     * 画表单标记（方括号；Focused 档下还给非聚焦字段画小点）到帧上。
     * 几何全部从排版的行盒派生（占位簇的位置 + 行盒的括号参考字体/基线）：
     * 排版说槽在哪，就画在哪 —— 空字段（一个字形都没有）也画得出来。
     * 不进底图：见 presentInternal 里那段说明。
     *
     * first_page / count 是这一帧的窗口。标记只画窗口内的行：帧坐标按窗口首页折算，
     *   窗口外的行落在帧外，画不进去也擦不着。
     */
    void drawControlMarks(int first_page, int count);

    /**
     * 画一枚行内原子的显式占位框到底图上。
     *
     * 与表单标记分开：原子是页面内容（它是流里的一项，进页指纹与行指纹），所以画在底图里、
     * 不随光标状态擦除；标记是瞬态的，画在帧上。
     *
     * 几何全部从行盒的占位簇派生：沿行内轴的两端取簇的 inline_pt 与下一个簇的 inline_pt，
     * 底边坐在行盒的基线上，高度取簇自带的盒高。于是"占了位"与"画了框"共用同一份数据。
     *
     * @param a1_pt 框在行内轴上的末端（页内磅）；a0 由 c.inline_pt 与行盒的 inline_origin 给
     * @param off_y 这一页在帧里的纵向偏移（帧像素）
     */
    void drawAtomBox(const LineBox& lb, const LineCluster& c, float a1_pt, float off_y);

    /**
     * 光标 / 编辑模式动过之后调用：括号占位计划变了就重排。
     * 传的是改动之前的 bracketPlanId()。All 档下它恒为常数 →
     * 光标在同一/不同字段间移动都不会重排（"光标移动 = 真增量"的地基）。
     */
    void relayoutIfBracketPlanChanged(int prev_plan);

    /**
     * 这一段内容所在的最内层那一格（连它所属的表一起给出）。不在表格里返回空。
     *
     * 与 cellFlowRangeAt 同一把尺子（最短区间 = 最内层），只是把"哪一格"也交出去 ——
     * 跨行拖选要按行列取矩形包络，光有流区间是取不出来的。
     */
    const LayoutCell* layoutCellAt(int para, const LayoutTable** table_out = nullptr) const;

    /**
     * 一格的内容流区间（左闭右开）。空格子或坐标与块表对不上时返回假。
     */
    bool cellFlowRangeOf(const LayoutCell& c, FlowPos* lo, FlowPos* hi) const;

    /**
     * 拖选 / Shift 扩展之后重算选区粒度：两端落在两个不同的格里就是单元格选区。
     *
     * 为什么放在引擎而不是宿主：这是"选区盖住了什么"的判断，与表格结构有关，而宿主手里
     * 只有命中测试的落点。放在引擎里还有一条好处 —— 拖选、Shift 加方向键、程序化扩展
     * 走的是同一条判定，不会出现"拖行、按方向键不行"。
     *
     * 两端都在同一个格（或有一端不在格内）时不吸附，仍是文字选区：格内把字选到最后一个
     * 不该整格反白 —— 那是两套操作，不是粗细两档。
     *
     * 选中的是两格之间的矩形（行包络 x 列包络），不是"首格首到末格末"那条流区间：
     * 文档顺序是逐行的，按流区间取会把两端那两行里没选中的格一起反白（跨行就整行）。
     */
    void syncCellSelection();

    /**
     * 表格自审的分项计数（见 auditTables）。逐项分开记是为了取证：同一条判据下，
     * "某格没有块"与"块没有结束标记"是两种完全不同的坏法。
     */
    struct TableAudit {
        int cells = 0;               // 扫过的格数
        int empty_cell = 0;          // 一个块都没有的格（这种格没有行盒，光标进不去）
        int bad_range = 0;           // 块的区间越出块表
        int overlap = 0;             // 两个格认领同一块（同表）
        int block_without_mark = 0;  // 格里的块没有结束标记
        int path_mismatch = 0;       // 块的 cell_path 与格给出的区间对不上
        int bad_span = 0;            // 行的跨度越出表尺寸
        int blocks_in_no_cell = 0;   // 块说自己在正文，格却认领了它
        int first_cell = -1;         // 首个出问题的格（跨表连续编号），-1 = 没有
        int total() const {
            return empty_cell + bad_range + overlap + block_without_mark + path_mismatch +
                   bad_span + blocks_in_no_cell;
        }
    };

    TableAudit auditTablesDetail() const;

    EngineConfig cfg_;
    FontLibrary fonts_;
    /**
     * 筛过括号的那一份，只在 filter_active_ 为真时有效。
     */
    std::vector<BlockSpec> blocks_;

    /**
     * 文档（唯一真相）：一份修订加一条历史。blocks_ 与 blocks_full_ 都是它的投影产物，
     * 没有第二份可写的树 —— 编辑只产步骤，产物每轮由新修订重算。
     */
    std::optional<rev::Session> session_;
    StyleSheet sheet_;
    /**
     * 上一轮每块的身份（内容哈希 + 来源）与样式表版本，与 blocks_full_ 同长同序。
     *
     * 它只是"上一轮的账"，不是第二个真相：块本身就在 blocks_full_ 里，投影按损伤区间
     * 原地保留区间之外的那些 —— 所以既没有第二份块流，也没有整份拷贝。装载新文档时清掉。
     */
    rev::BlockHistory proj_hist_;
    /**
     * 装载过文档没有（setDocument 置位）。没装载过时没有树、没有流、也没有事务栈 ——
     * 编辑入口一律拒绝，审计入口报"没有可比的东西"，而不是拿一棵空树去编出 0 段来。
     */
    bool loaded_ = false;
    /**
     * 块流（唯一一份）：投影就地更新它，排版也吃它。
     *
     * "按要显示括号的名单筛一遍"是破坏性的（对象表会被删项），而名单会来回变，所以
     * 只有真的需要筛时才把筛过的结果放进 blocks_ —— 名单覆盖全部控件时 blocks_ 是空的，
     * blocks() 直接给这一份，不存在副本。
     */
    std::vector<BlockSpec> blocks_full_;
    /**
     * 筛过括号的块流。只为"名单真的排除了某些控件"而存在，其余时候为空。
     */
    bool filter_active_ = false;
    /**
     * 表格结构（编译产物的一种投影，与 blocks_full_ 同源）。排版吃它来按格布局；
     * 它也是"这张表跨了几页、格线画在哪"的输入。没有表格时为空。
     */
    std::vector<LayoutTable> tables_;
    /**
     * 控件集合下标 → 控件的节点 id（导出时按同一顺序建）。
     */
    std::vector<NodeId> control_node_ids_;
    /**
     * 产物的 owner（节点 id）→ 控件集合下标。导出时一并建好，命中测试与编辑归属
     * 靠它把句柄翻译成本层对外的口径。
     */
    std::unordered_map<int, int> owner_index_;

    /**
     * 树口径的流索引。可见性与编译同一处取（编辑态是设计态，呈现态摘掉隐藏子树），
     * 于是"块流里数得到的东西"与"流位置里数得到的位数"始终是同一份文档的两种说法。
     *
     * 它是派生量：树一改就过期，所以只在重建块流时一起重建，不单独维护。用 optional
     * 而不是常驻对象，是因为 FlowIndex 没有默认构造 —— 它一造出来就要一棵树。
     */
    std::optional<rev::FlowIndex> flow_;
    /**
     * 每块内容起点的流位置，与 blocks_full_ 同长同序。取自 BlockSource::first 那枚元素在
     * 流里的起点（见 compile.h）—— 空段取的是它自己的结束标记，"标记之前"就是内容起点。
     */
    std::vector<FlowPos> block_flow_;
    /**
     * 每块内容在流里占的位数：文本码点数 + 控件边界数。段间那个段落标记不在这里 ——
     * 它是段与段之间的位置，不属于任何一段的内容。
     */
    std::vector<int> block_flow_span_;
    /**
     * 每块的来源身份（编译给出，与 blocks_full_ 同长同序）。编辑要在树上落笔时靠它找到
     * "这一段是哪一棵子树里的那一段、被哪枚标记结束"。
     */
    std::vector<BlockSource> block_src_;

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
     * 预热字形用的临时靶子：一条页宽的窄带，画进去的像素随用随丢。
     *
     * 为什么不另做一条"只光栅不画"的路径：mask 进缓存这件事由绘制路径负责
     *   （Skia 的 strike 只认真正走过的 drawGlyphs）。另起一条就要再写一遍
     *   缓存键的算法，而两条路径的键只要差一点，预热就毫无效果，且不会报错。
     *   靶子够宽够高，字形不会被裁，缓存才存得下。
     */
    Bitmap prewarm_;

    EngineStats stats_;
    EditStats edit_stats_;

    /**
     * 与光栅后端有关的、只能设一次的那些参数（目前是 Skia 的字形缓存预算）。
     *
     * 为什么要有这么一个"懒做一次"的入口，而不是摆在 init() 里：wasm 宿主不走 init() ——
     *   它是 g_engine.fonts().addFaceFromMemory(...) 直接装字体，于是摆在 init() 里的
     *   设置在那条路上一次都不会执行（预算写成 48 MB，面板读回来仍是默认的 2 MB）。
     *   present 与预热是所有宿主都必经的两条路，而它们又都在"要光栅字形"之前，
     *   所以放在那儿是唯一保证生效的位置。
     */
    void ensureRenderSetup();
    bool render_setup_done_ = false;

    /**
     * 把两份视图摆给它们各自的类：位置解析读 PositionView，命令层读 EditView。
     *
     * 只在文档重建之后与重排之后各摆一次就够：视图里存的是这些容器的地址，而容器都是引擎的
     * 成员、不换对象（重建是 assign、重排是就地改），地址一直有效。
     * 谁要是把某个容器换成了另一个对象（move 走、重新 new），这里就是唯一的失效点。
     */
    void refreshViews();

    /**
     * 光标与位置解析。选区两端、行别、上下键的列位、光标条宽度全在它里面 ——
     * 引擎这一层只留"读文档产物"与"协调重排"（见 projectCaret 与移动族的转发）。
     */
    CaretResolver caret_;
    /**
     * 命令层：一次编辑怎么变成一条事务（插入、删除、分段、合并、缩进、结构操作）。
     *
     * 它只认 EditView 与 EditHost 两个接口 —— 引擎在这里的角色是"事务的出口"与
     * "视图的持有者"，编辑本身的做法（策略、步骤、撤销快照、光标落点）全在它里面。
     */
    EditCommand edit_;
    bool caret_visible_ = true;
    /**
     * 上一帧画过的选区矩形（帧像素）。下次呈现先按它擦掉 —— 与 caret_rect_px_prev_
     * 同一个理由：选区画在帧上、不进底图，不擦就会留下上一帧的一片蓝。
     */
    std::vector<Rect> selection_rects_prev_;

    /**
     * 底图当前显示的范围与有效性
     */
    int base_first_page_ = 0;
    /**
     * 帧槽数（页号取模的模数），0 = 还没被设过。
     *   它一变，所有页的槽号都跟着变，所以帧里的内容整体作废（见 setFrameSlots）。
     */
    int frame_slot_count_ = 0;
    /**
     * 槽 -> 该槽当前画着哪一页（-1 = 未知）。它是"要不要重画"的第二个判据：
     *   页指纹只说明"这一页的内容变了"，而窗口平移/换缩放会让某个槽里装的换成别的页 ——
     *   那件事页指纹看不见，必须由这张表回答。
     */
    std::vector<int> slot_page_;
    /**
     * 页号 -> 槽号。取模处理负页号，所以先加一轮再取。
     */
    int slotOfPage(int page) const {
        const int n = frame_slot_count_;
        if (n <= 0) return 0;
        return ((page % n) + n) % n;
    }
    int base_page_count_ = 0;
    bool base_valid_ = false;

    /**
     * 视口锚点（见 setViewportAnchor）。count <= 0 = 整篇。
     */
    int vp_first_page_ = 0;
    int vp_count_ = 0;

    /**
     * 本次呈现写过的页（绝对页号，升序去重）。见 dirtyPages()。
     */
    std::vector<int> last_dirty_pages_;

    /**
     * 页内容指纹：本次 relayout 的结果 / 已画进底图的那一版。
     *
     * 真指纹一律清掉 kFpDirtyBit —— 那一位留给"已知变了"的哨兵（markPagesChangedFrom），
     * 于是"这一页变了没有"永远只有一个判据：new != base。
     */
    std::vector<uint64_t> page_fp_new_;
    std::vector<uint64_t> page_fp_in_base_;
    int page_fp_new_count_ = 0;
    int page_fp_base_count_ = 0;

    /**
     * 行内容指纹：与页指纹同源、同一把尺子，只是分桶到行（下标 = LineBox::line）。
     *
     * 它存在的唯一理由是把"这一页要重画"细化成"这一页的哪几行要重画"：
     * 一页约 1000 个字形，而敲一个字只动其中一两行的字形 —— 页指纹粒度下，
     * 改一行与改整页同价（实测字形光栅 10~14 ms，其中九成是白画的）。
     *
     * 指纹必须含行盒几何（block_lo/hi、inline_origin）：字形一样但位置挪了
     * 也算变了 —— 那是"后缀整体平移"那一类。
     */
    std::vector<uint64_t> line_fp_new_;
    std::vector<uint64_t> line_fp_in_base_;
    int line_fp_base_count_ = 0;

    /**
     * 底图里每一行的块轴范围（页内磅），按行下标。
     * 重画一条行带时要靠它算"擦到哪儿"：上边界取新旧两版里更靠上的那个，
     * 否则行往下挪之后旧墨迹会留在原地（幽灵字）。下边界同理取更靠下的。
     */
    std::vector<float> line_lo_in_base_;
    std::vector<float> line_hi_in_base_;

    /**
     * 本次重画那条行带的页内磅范围（computeBand 写、paintPageIntoBase 读）。
     * 放成员是为了不把两个 float 在 lambda 之间传来传去。
     */
    float band_lo_scratch_ = 0.0f;
    float band_hi_scratch_ = 0.0f;

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
     * 表单：控件区间集合与编辑模式。
     *
     * 模式只决定"光标能落在哪、哪些命令可用"这一层策略（见 edit_capability），它属于
     * 控件这一侧的口径而不是文档：同一棵树切到不同模式，块流与可见性都可能跟着变。
     */
    ControlSet controls_;
    EditMode mode_ = EditMode::Document;
    long long blocked_edits_ = 0;
    /**
     * 每一份修订对应的两份选区快照（编辑前 / 编辑后）。历史本体在 session_ 里，
     * 这里只补它记不了的那一半 —— 撤销与重做都要把光标摆回按下去之前的位置。
     *
     * 与 session_ 的撤销栈同步裁剪：超上限时丢最旧的，与"丢更早的状态"同一口径。
     */
    struct SelectionRecord {
        Range before;
        Range after;
    };
    std::vector<SelectionRecord> sel_history_;
    size_t sel_pos_ = 0;

    /**
     * 表单标记（小点/方括号）在帧像素下的矩形，用来在下一帧把它们擦掉。
     * 与 caret_rect_px_prev_ 同一个思路：瞬态的东西必须记账，否则擦不干净。
     */
    std::vector<Rect> control_marks_prev_;

    /**
     * 要求下一次呈现整帧重贴（而不是只贴几个脏矩形）。
     * 用在"帧上画的东西变了、但底图没变"的时候 —— 切编辑模式、改控件集合。
     * （底图不含控件标记，所以那两件事不会让页指纹变；不显式置位就会出现
     * "切到只读模式后括号还在"这种残留。）
     */
    bool frame_needs_full_refresh_ = false;

    Rect caret_rect_px_prev_{};  // 上一帧光标矩形（帧像素）
};

}  // namespace heditor
