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
#include "core/doc/edit_capability.h"
#include "core/doc/edit_policy.h"
#include "core/doc/flow.h"
#include "core/doc/transaction.h"
#include "core/engine/engine_config.h"
#include "core/engine/engine_stats.h"
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
     * 用文档模型树装载文档：引擎从此持有那棵树，块流由编译产出（第十四节那条接缝）。
     * 与 setBlocks 的分工是"来源"而不是"形状"：两条路给排版的都是同一份块流，差别在它是
     * 调用方拼好递进来，还是由模型树编译出来 —— 后者才是这一层的目标形态，控件边界、
     * 段落格式与字符格式因此都只有一个产处（compile.cpp），引擎不再自己拼。
     *
     * 隐藏控件的可见性口径与编译同一处取值（见 rebuildFromTree），所以 setEditMode 落在
     * 这条路上时必须重编译。装载会复位光标、作废底图并整篇标脏。
     *
     * 接 const 引用、内部走 assignFrom 深拷贝：树刻意不给拷贝构造（见 NodeTree 的说明），
     * 所以"按值收一棵树"在这里编译不过，那是对的。
     */
    void setDocument(const NodeTree& tree, const StyleSheet& sheet);
    /**
     * 块流是不是树的编译产物（而不是 setBlocks 递进来的）。
     */
    bool hasDocumentTree() const { return tree_valid_; }
    /**
     * 持有中的模型树。hasDocumentTree() 为假时它的内容没有意义。
     */
    const NodeTree& documentTree() const { return tree_; }

    /**
     * 段落坐标与模型流位置的桥接 —— 第十四节那条接缝的另一半。
     *
     * 引擎对外一直说 (段, 段内码点偏移)，模型层说流位置；两者不能靠算式互换 —— 控件与节这些
     * 容器在流里各占起止两位，而段的码点里没有它们。所以换算只认一个权威：编译给出的块来源
     * 身份（BlockSource）加 FlowIndex。桥是单向的：模型是真相，段坐标是它的一个视图。
     * 只有持树时（hasDocumentTree）桥才存在；旧路径的块流没有对应的树，入口返回 -1 与 false。
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
     * 光标。对外坐标仍是 (段, 段内码点偏移)，任何设置入口都会先夹到合法范围、
     * 再吸附到字素簇边界 —— 所以光标永远不会停在半个 é（e + U+0301）中间。
     *
     * 引擎内部只存一个数：caret_rel_（段内流位置，括号占位算在里面）。下面这些入口
     * 都是"把别的坐标系翻成它"，对外接口一个字没变。
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
     * 把光标摆到一个流位置上（绝对）。编辑路径全用它：删除后的落点是删除区间的起点、
     * 插入后是插入区间的终点、Tab 的目标、撤销恢复的位置，它们都已经是流位置。
     */
    bool setCaretAtFlow(FlowPos flow);
    /**
     * 光标位置（派生）。段号 + 段内码点偏移，由 caret_rel_ 算出来 —— 不再是独立状态。
     */
    TextPos caret() const;
    /**
     * 光标当前的归属（-1 = 不在任何控件里；LineBox::kInlineOffsetOuter = 在所有控件之外）。
     * 由 caret_rel_ 直接查，不是独立状态。
     */
    int caretFieldHint() const;
    /**
     * 光标的侧别（派生）：在它那个码点偏移上跨过了几枚边界括号（0 = 在所有括号之前）。
     * 与 caret() 合起来才是光标的完整位置 —— 同一个偏移上可能有好几个插入点
     * （控件之前 / 进入控件 / 内容末尾 / 控件之后），光有偏移表达不了。
     * 它是 rel 与偏移的差，所以不可能与光标位置不同步 —— 这就是它取代 caret_side_
     * 与 caret_side_at_ 那两个字段的理由（那两个字段天然可能失配，才需要失效标记兜）。
     */
    int caretSide() const;
    /**
     * 按对外坐标（段, 段内码点偏移）写光标，侧别按"最内层含端点"投影一次。
     * 这是文本坐标投影到流位置的唯一约定 —— 只给"没有侧别信息"的入口用（宿主程序化定位、
     * 排版收口）；编辑路径一律走 setCaretAtFlow / setCaretRel，侧别由操作自己算出来。
     */
    void setCaretByText(TextPos p);
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
     * 由光标位置与侧别推出来（见 caretFieldHint）；推不出来时（旧路径的块流还没有
     * 边界对象表、或光标落在所有控件之外）回落成"最内层含端点"—— 与编辑归属同一口径。
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
     * 块流由编译产出时 owner 是控件的节点 id（第五刀：序号会在插入之后整体平移，而节点
     * id 是身份），setBlocks 旧路径给的一直是下标。对外（宿主、探针、既有判据）的口径始终
     * 是下标，所以"拿下标去和占位簇的 owner_control 比"的地方都要先过这里 ——
     * 宿主报"这枚括号归谁"正是其中一处。
     */
    int controlIndexOfOwner(int owner) const {
        if (owner_index_.empty()) return owner;  // 旧路径给的本来就是下标，原样返回
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
        if (!tree_valid_) return control_index;
        if (control_index >= static_cast<int>(control_node_ids_.size())) return -1;
        return static_cast<int>(control_node_ids_[static_cast<size_t>(control_index)]);
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
     * 撤销与重做：树上编辑走事务，栈由引擎持有。
     *
     * 撤销按事务逆序应用逆操作，不做文档快照 —— 快照回退会让整篇变脏，直接击穿增量排版。
     * 撤销之后按事务记的编辑前选区恢复光标（选区在模型坐标里，经桥换回段坐标）。
     * 不持树时（旧路径）没有事务可言：四个入口都返回 false，不假装能做。
     */
    bool canUndo() const { return undo_ != nullptr && undo_->canUndo(); }
    bool canRedo() const { return undo_ != nullptr && undo_->canRedo(); }
    bool undo();
    bool redo();
    bool undoAndRender();
    bool redoAndRender(); /**
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
     * Tab / Shift+Tab 的策略版：持树时走模型层的能力表（edit_capability 的 tabFrom），
     * 到最后一个可编辑控件就放行（返回 false，不消费该 Tab），不绕回第一个。
     *
     * 与 focusControl 的差别不只是循环与否：focusControl 认的是控件集合的区间起点，
     * 而 tabFrom 认的是"内容起点落在可编辑集合内"这个谓词 —— 于是"只读控件里嵌的
     * 可编辑控件"不会成为落点，反之亦然，两处口径不会各说各话。
     * 不持树时退回 focusControl（旧路径没有流位置，能力表也用不上）。
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
     * 编译自审：拿"整树全量编译"做一份对照，与当前产物逐字段比对，返回不符项数。
     *
     * 与 auditLayout 同一个理由：编译这一层的增量的正确性由"与全量一致"定义。
     * 只比段数看不出来 —— 增量最典型的错法是"某一段的产物还是旧的"，
     * 而段数、行数全都对得上。无树（旧路径）时返回 0：那条路没有全量编译这个对照物。
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
     * 反查：帧坐标（磅）→ (段, 偏移) + 侧别。点落在页间空隙时返回 false。
     *
     * @param out_side 非空时写出落点的侧别（见 caretSide）。它是落点的完整身份：
     *   共享边界上"前一个字段的末尾"与"后一个字段的开头"是同一个偏移，同一个偏移上
     *   还可能压着好几枚括号，光有偏移表达不了用户点的是哪一个位置。
     * @param out_owner 非空时写出"该次点击属于哪个控件"（-1 = 判不出，
     *   LineBox::kInlineOffsetOuter = 在所有控件之外）。它是侧别的投影，供宿主显示用；
     *   搬运落点请用 out_side —— 投影会丢掉"同处好几枚括号"的细节。
     */
    bool hitTestPt(float x_pt_frame, float y_pt_frame, TextPos* out, int* out_owner = nullptr,
                   int* out_side = nullptr) const;

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
     * 落点判定的唯一入口：持树时走模型层的能力表（树 + 流位置），否则走旧的控件集合策略。
     * 两条路只在这一处分叉，调用方拿到的是同一种回执（能不能 + 为什么不能）。
     *
     * 为什么不在调用处各写一遍：编辑入口有六个（插入、退格、前删、分段、合并、反缩进），
     * 每处各写一遍必然分叉，而分叉的表现是"某一条路上打得进字、另一条路上打不进"，极难查。
     *
     * owner_hint 只对持树那条路有意义：它把落点贴到共享边界的内侧（见 flowPosAt）。
     * 旧路径的归属提示由 EditPolicy 自己消化。
     */
    EditCheck policyAt(TextPos p, int owner_hint) const;
    /**
     * 删除区间的判定。跨段删除两条路都拒（档 1 的删除不跨段，段落标记的删除走合并）。
     */
    EditCheck policyErase(TextPos a, TextPos b) const;
    /**
     * 表单模式的安全阀：一个可编辑控件都没有时，表单退化为文档模式（编辑一律放行），
     * 而不是"什么都不让改"。两条编辑路径共用它 —— 判据见 edit_policy.cpp 顶上那段。
     */
    bool formDegraded() const;
    /**
     * 导出后的控件集合里有没有可编辑的那一个。formDegraded 用它。
     */
    bool hasEditableControl() const;
    /**
     * 编辑落点的归属提示：p 就是当前光标时用点击带来的侧别（两条路都要用：旧路径把它交给
     * EditPolicy 的 preferred，持树那条路用它把桥的落点贴到控件内侧）；没有点击提示且持树时，
     * 回落成"最内层含端点的控件"—— 旧路径的归属规则。不持树且没有提示时返回 -1。
     *
     * 为什么持树时要回落而不是直接给 -1：桥上"无提示"取最外侧，于是光标停在字段起点会被
     * 读成"在控件之前"，编辑被拒 —— 而光标看起来正贴在开括号之后。回落让两侧口径
     * 一致：归属按"最内层含端点"定，与旧路径逐字相同。
     */
    int editOwnerHintAt(TextPos p) const;

    /**
     * 从持有的树重新编译出块流，并把控件集合从产物的边界里导出来。
     * 隐藏控件的可见性口径按当前编辑模式取（见 setDocument 的说明）。
     */
    void rebuildFromTree();

    /**
     * 只重编译一段（增量）。条件不满足时返回 false，由调用方退回全量。
     *
     * 为什么值得单开一条路：整树编译是 O(文档)，3000 段实测约 1.8ms —— 而那是每次按键
     * 都要付的一笔。条件刻意保守：段数变了、内容区间里嵌着段落标记或节边界、
     * 或者是非首节的第一段（分节强制分页是跨段状态），一律退回全量。
     * 返回 false 时状态一个字节都没改 —— 先判条件再动手。
     */
    bool rebuildParagraphFromTree(int para);

    /**
     * 重建的收尾：流索引、段的流区间递推、块流副本、控件视图。两个入口共用 ——
     * 分两处写就会留下"这一半按新树、那一半按旧树"的错配，而那种错配只表现为光标落偏。
     */
    void finishRebuildFromTree(FlowVisibility vis);

    /**
     * 隐藏控件的可见性口径。编译与流索引必须同源（非编辑模式下隐藏的子树整棵摘除），
     * 所以只在这里取一次，两个入口都用它。
     */
    FlowVisibility flowVisibility() const;
    /**
     * 在树上提交一个事务，然后把块流按新树重编译一次。
     *
     * 重编译是必须的而不是图省事：块流、流索引、块起点、来源身份、控件集合全是树的派生量，
     * 少重建任何一个，下一轮就会拿旧树的位置去对新树的块（症状是光标落偏一个字、控件区间
     * 慢一拍）。脏区只标到 dirty_para —— 同一段内的编辑不影响它前面的段，增量因此照旧成立；
     * para_count_changed 为真时禁止后缀复用，理由见那个成员的说明。
     */
    bool commitOnTree(Transaction tx, int dirty_para, bool para_count_changed);
    /**
     * 树上的一段删除：把同一段内 [a, b) 这段（已吸附到簇边界）删掉。
     *
     * 退格、前删、反缩进三个入口只是算区间的方式不同，落到文档上的操作是同一个 —— 与旧路径
     * 里它们共用 adjustControlsOnErase 同一个道理。策略判定留在调用方：被拒时要报的是
     * "用户按的那一下没生效"，与"这一步做不成"不是一回事。
     */
    bool eraseOnTree(TextPos a, TextPos b, const char* label);
    /**
     * 落点处应当沿用的字符格式：取紧挨在它左边的那个文本节点的标记与样式引用。
     *
     * 不取"块级默认"：那样在粗体字中间敲一个字会得到不粗的新字。取邻居是"输入沿用落点处
     * 已有样式"这条约定的最小实现；左边没有文本节点时返回空标记（= 全继承）。
     */
    CharFormat marksAtFlow(FlowPos at, std::string* style_ref) const;
    /**
     * 从块流的行内对象表导出控件集合：按 owner 配对起始与结束两枚边界，
     * 得到控件的 (起, 止) 区间。
     *
     * 为什么不从树直接遍历：编译已经把边界写进产物了（第五刀），而产物就是这一层
     * 拿到的全部输入 —— 再从树走一遍等于把"哪一段、第几个码点"重算一次，两处一旦
     * 分叉就会出现"排版按产物画括号、策略按另一套区间判可编辑"的错配。
     * 没有配对的那一枚整体丢掉：宁可少一个控件，也不要一个区间反了的控件。
     */
    void exportControlsFromBlocks();
    /**
     * 块内流偏移 → 段内码点偏移（flowToTextPos 的其中一段）。落在边界位上时归到那个
     * 边界锚着的码点 —— 于是桥的两端互逆，不会因为中间隔着括号而越走越偏。
     */
    int cpOffsetInBlock(int para, int rel) const;

    /**
     * 把偏移吸附到簇边界（用当前 layout 的簇表）
     */
    size_t snapToCluster(int para, size_t offset) const;

    /**
     * 某偏移上的边界括号统计。侧别与流位置都从它推出来（两者是同一件事的两种说法）。
     */
    struct BracketCounts {
        int closes = 0;  // 闭括号数 = "所有控件之外"那一侧的侧别
        int opens = 0;
        int before = 0;  // 排在它前面的边界枚数（对象表按 cp 升序，所以这也就是它的下标）
    };
    BracketCounts bracketCountsAt(int para, size_t cp) const;
    /**
     * 这个段的边界对象表。两条路各存一处：持树时产物在 blocks_full_（编译给的、
     * 不筛显示名单的那一份，流位置就是按它摊开的），旧路径的块流没有编译产物，
     * 对象由 syncInlineObjects 直接写进 blocks_。
     */
    const std::vector<InlineObject>& objectsFor(int para) const;
    /**
     * 光标唯一的写入点：段内流位置。其余入口都只是把它换算成 rel 再调这里。
     */
    void setCaretRel(int para, int rel);
    /**
     * 把"文本坐标 + 侧别"投影成 rel 并摆好（含重排）。side < 0 = 按"最内层含端点"投影。
     */
    void projectCaret(int para, size_t offset, int side);
    /**
     * 段内相对流位置 = 码点偏移 + 排在它前面的边界数 + 侧别。
     */
    int relAt(int para, size_t cp, int side) const;
    /**
     * 提示（控件下标 / 贴外侧哨兵 / -1）→ 侧别。宿主与探针给的是提示，排版与流要的是侧别。
     */
    int sideFromHint(int para, size_t cp, int hint) const;
    /**
     * 文本码点区间 [a, b) 在流里占的那一段（段内相对，半开）。括号不是文本，不进这个区间。
     * 判定（policyErase）与实际删除（eraseOnTree）共用它 —— 两处分叉就会出现
     * "判据说能删、真删却越界"。
     */
    bool textSpanAt(int para, size_t a, size_t b, int* from, int* to) const;
    /**
     * 不带侧别时的默认侧别：按"最内层含端点"归一次（见 defaultSideAt 的实现）。
     */
    int defaultSideAt(int para, size_t cp) const;
    /**
     * 某个位置（不是光标）的侧别：按"最内层含端点"归一次，归不到就是"所有控件之外"。
     */
    int sideForPos(TextPos p) const;
    /**
     * 段内相对流位置 → 最内层包含它的控件下标（LineBox::kInlineOffsetOuter = 在所有控件之外）。
     */
    int hintFromRel(int para, int rel) const;
    /**
     * 跨段控件的归属补判（见实现处的注释）：段内对象表只有本段那一半边界，
     * 控件从上一段跨进来时配对不上，这里用控件集合的跨段区间判。
     */
    int hintFromCrossSegment(int para, int rel) const;
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

    /**
     * 文档模型树与样式表（setDocument 装载）。tree_valid_ 为真时 blocks_ 是它们的
     * 编译产物；为假时 blocks_ 由调用方拼好递进来（setBlocks 旧路径）。
     */
    NodeTree tree_;
    StyleSheet sheet_;
    bool tree_valid_ = false;
    /**
     * 编译的完整产物。"按要显示括号的名单筛一遍"这一处读它 —— blocks_ 是筛过之后的
     * 副本。筛是破坏性的（对象表会被删项），而名单会来回变（Focused 档与 All 档之间切），
     * 没有这份原件，被删掉的边界就回不来了。
     */
    std::vector<BlockSpec> blocks_full_;
    /**
     * 控件集合下标 → 控件的节点 id（导出时按同一顺序建）。旧路径下为空 —— 那条路的
     * 边界句柄本来就是下标。
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
    std::optional<FlowIndex> flow_;
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

    int caret_para_ = 0;
    /**
     * 光标的唯一真源：段内流位置（括号占位算在里面）。
     *
     * 为什么不是一个码点偏移：同一个偏移上可能压着好几枚括号（相邻空字段就是 ][][]），
     * "控件之前 / 进入控件 / 内容末尾 / 控件之后"是互不相同的插入点，而码点偏移不含括号、
     * 表达不了它们 —— 每次要问"这是流里的哪个位置"都得猜，猜的入口越多、漏在接缝上的
     * bug 越多（"删一个字光标跳一格""字打进了隔壁字段"）。
     * rel 就是"数格子"：文本字与边界括号各占一位，于是
     *   偏移 = cpOffsetInBlock(para, rel)（派生）
     *   侧别 = rel - 偏移 - 排在它前面的边界数（派生）
     * 两个派生值不可能与它不同步 —— 这就是它取代 caret_side_ / caret_side_at_ 的理由。
     */
    int caret_rel_ = 0;
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

    /**
     * 表单：控件区间集合与编辑策略（策略是纯函数，见 core/doc/edit_policy.h）。
     */
    ControlSet controls_;
    EditPolicy policy_;
    long long blocked_edits_ = 0;
    /**
     * 撤销栈。只在持树时存在（setDocument 建、setBlocks 与 setControls 拆）——
     * 旧路径的编辑由调用方自己改块流，引擎没有"改动由哪几步构成"可言，也就没有可撤销的单位。
     * 它持的是 tree_ 的引用，而 tree_ 的地址在整个生命周期里不变。
     */
    std::unique_ptr<UndoStack> undo_;

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
