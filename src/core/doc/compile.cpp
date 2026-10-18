#include "core/doc/compile.h"

#include <algorithm>

#include "core/doc/document.h"
#include "core/utils/hash.h"

namespace heditor {

namespace {

/**
 * 一段待解析的文本：码点 + 它在模型里的格式来源。
 *
 * 为什么字符格式要等到段落结束才解析：三层解析得先知道"本段的段落样式"，而段落样式挂在
 * 结束这一段的那枚段落标记上 —— 走到文本节点时还看不到它。所以这里把格式来源一起缓存，
 * 解析推迟到 flush。
 */
struct PendingText {
    std::vector<uint32_t> cps;
    CharFormat marks;              // 元素上的内联覆盖
    std::string style_ref;         // 字符样式引用
    NodeId node = kInvalidNodeId;  // 记诊断用
};

/**
 * 两个 run 的字符格式是否相同（不比区间）。
 * 逐字段比较：漏比一个字段会把两个不同格式的文本段并成一个，而合并是不可逆的静默丢格式。
 * 语言按指针比即可 —— 它来自同一份段落格式，是同一个驻留字符串。
 */
bool sameRunFormat(const FormatRun& a, const FormatRun& b) {
    return a.font_index == b.font_index && a.size_pt == b.size_pt && a.color == b.color &&
           a.language == b.language && a.fallback_font_indices == b.fallback_font_indices;
}

/**
 * 一次摊平：按流顺序遍历，边累积文本边切段。
 *
 * 不支持的元素记一条诊断，容器仍下钻取文本；诊断让 ok() 变假，调用方不能把产物当完整结果。
 * 内容控件的两个边界落成段内的行内对象（BlockSpec::objects），锚点是当前段已累积的码点数。
 * 段落格式与字符格式都按同一套三层解析（文档默认值、样式链、内联覆盖）算出来写进产物 ——
 * 引擎只吃解析后的结果，它不认识样式表。
 *
 * 分节强制分页（1.8）在这里落成引擎已认识的一个标记：进入第二节及以后时，若它的节类型
 * 不是连续，就把下一个落下的段落标上"段前分页"。于是分节不引入新的排版路径，与全量的
 * 一致性由引擎既有的那条机制（段前分页 + 增量与全量等价）保证。
 */
class Compiler {
public:
    Compiler(const NodeTree& tree, FlowVisibility vis, const StyleSheet& sheet)
        : tree_(tree), vis_(vis), sheet_(sheet) {}

    CompileResult run(NodeId start) {
        walk(start, false);
        // 结尾还没有结束标记的那一段：累积过文本、或只攒下过控件边界的，都要落成一段，
        // 用默认段落格式。少了后半个条件，"控件从文档开头就开、内容为空"那种文书会连
        // 一段都产不出来，控件边界于是凭空消失。
        if (!pending_.empty() || !obj_.empty()) flush(nullptr);
        return std::move(out_);
    }

    /**
     * 只走一个容器的 children[begin..end]。增量编译用它（见 compileChildRange）。
     * 区间不完整时（begin 越界等）返回空产物，让调用方退回全量而不是拿半份结果。
     */
    CompileResult runRange(NodeId container, int begin, int end) {
        const Node* c = tree_.find(container);
        if (c == nullptr || !isContainer(c->kind)) return CompileResult{};
        if (begin < 0 || end < begin || end >= static_cast<int>(c->children.size())) {
            return CompileResult{};
        }
        for (int i = begin; i <= end; ++i) walk(c->children[static_cast<size_t>(i)]->id, false);
        if (!pending_.empty() || !obj_.empty()) flush(nullptr);
        return std::move(out_);
    }

private:
    /**
     * 当前段已经累积了多少个码点 —— 行内对象的锚点就是它。
     *
     * 文本在 flush 之前一直是待解析的（PendingText 只缓存不拼），所以这里按各段长度求和，
     * 与 flush 里逐段 insert 之后的位置是同一个数。控件边界因此与文本共用一把尺子，
     * 挂在哪一段、跨了几段都按同一条规则算。
     */
    uint32_t cpCount() const {
        uint32_t n = 0;
        for (const PendingText& t : pending_) n += static_cast<uint32_t>(t.cps.size());
        return n;
    }
    /**
     * 记下本块第一个吃进内容的元素 —— 只在"这一块还没有过任何内容"时生效。
     *
     * 内容指会被算进产物的东西：待解析的文本、控件边界。控件被包在未编译的子树里时不产边界，
     * 那时不该记它，否则来源身份会指向一个在产物里没有位置的节点。
     */
    void noteFirst(NodeId id) {
        if (cur_first_ == kInvalidNodeId && pending_.empty() && obj_.empty()) cur_first_ = id;
    }

    /**
     * 记一条诊断，同一条原因只记一次。
     *
     * 为什么要去重：字符格式的诊断说的是"这段文字引用的样式有问题"，同一段里十个文本节点
     * 引用同一个未知样式会刷出十条一模一样的消息，把真正的信号淹掉。
     */
    void note(NodeKind kind, NodeId node, const std::string& detail) {
        for (const CompileDiagnostic& d : out_.diagnostics) {
            if (d.detail == detail) return;
        }
        out_.diagnostics.push_back(CompileDiagnostic{kind, node, detail});
    }

    /**
     * 落一段。mark 为空表示这一段没有结束标记（末尾未结束），用默认段落格式。
     */
    void flush(const Node* mark) {
        const NodeId mark_id = (mark != nullptr) ? mark->id : kInvalidNodeId;
        // 本块的来源身份：第一个吃进内容的元素；一段空空如也时就是它自己的结束标记。
        //   在 obj_ 被搬走之前取，之后没法再判"这一段到底有没有过内容"。
        const NodeId first_id = (cur_first_ != kInvalidNodeId) ? cur_first_ : mark_id;
        cur_first_ = kInvalidNodeId;

        BlockSpec b;
        if (mark != nullptr) applyParaFormat(*mark, &b);
        // 本段的行内对象表：进出控件边界时就地攒下（见 walk 里控件那一段），落段时一起带走。
        //   它与 cp_text 共用同一个码点坐标，同一次落段，于是不可能出现"文本进了这一段、
        //   对象却留在上一段"的错配。
        b.objects = std::move(obj_);
        obj_.clear();
        // 分节强制分页：标记只在下一个段落上落地一次（标记被消费掉），所以一节一段也只会
        // 分一次页。它与段落自己的段前分页是或的关系。
        if (pending_section_break_) {
            b.page_break_before = true;
            pending_section_break_ = false;
        }

        // 段落默认字符格式就是刚解析出来的那组块级字段。run 以它为底、三层解析盖在上面 ——
        // 于是"没有字符格式的文本"的 run 恒等于它，与加 run 之前逐字段相同。
        // 此刻 cp_text 还是空，wholeRun 只借它的属性，区间随后按各段文本现填。
        const FormatRun para_default = b.wholeRun();

        // 段落样式链的字符格式基线：一段解析一次，本段所有文本节点共用。
        // 诊断传 nullptr —— 这条链的问题已经由上面的 applyParaFormat 报过，而且两者是同一根
        // 链；再报一次只会按文本节点数重复。
        const std::string para_style = (mark != nullptr) ? mark->styleRef : std::string();
        const CharFormat base = paragraphRunBase(sheet_, para_style, nullptr);

        for (const PendingText& t : pending_) {
            FormatRun r = para_default;
            r.cp_begin = static_cast<uint32_t>(b.cp_text.size());
            b.cp_text.insert(b.cp_text.end(), t.cps.begin(), t.cps.end());
            r.cp_end = static_cast<uint32_t>(b.cp_text.size());

            std::vector<std::string> why;
            const CharFormat cf = resolveRunFormatFrom(sheet_, base, t.style_ref, t.marks, &why);
            for (const std::string& w : why) note(NodeKind::TextRun, t.node, w);
            applyRunFormat(cf, t.node, &r);

            // 与上一个 run 的格式完全相同就并进去：相邻同格式的文本段在引擎里是一个 run。
            // 不并也对（切分不改产出，这一条在引擎侧有等价判据），但并起来让 runs 是规范的，
            // 不随模型里文本节点的切分方式变化。
            if (!b.runs.empty() && sameRunFormat(b.runs.back(), r)) {
                b.runs.back().cp_end = r.cp_end;
            } else {
                b.runs.push_back(r);
            }
        }
        pending_.clear();
        // 原子的盒子几何在这里定：公式的默认框要按段落字号估，而字号是段落属性 ——
        //   走到文本节点时还看不到它。emit 那一刻只记下"这里有一枚原子"。
        for (InlineObject& o : b.objects) {
            if (o.kind != InlineObjectKind::Atom) continue;
            const Node* atom = tree_.find(static_cast<NodeId>(o.owner));
            if (atom == nullptr) continue;
            resolveAtomBox(*atom, b, &o);
        }
        out_.blocks.push_back(std::move(b));
        out_.sources.push_back(BlockSource{first_id, mark_id});
    }

    /**
     * 把解析出来的字符格式写进 run：没设置的字段保留段落默认值（未设置即继承）。
     *
     * 粗体、斜体、下划线在引擎输入里还没有对应字段。设了却不报出来等于静默丢格式 ——
     * 排版看着没错、只是样式没了，比"不支持"难查得多。所以显式记诊断，不伪造成功路径。
     */
    void applyRunFormat(const CharFormat& cf, NodeId node, FormatRun* r) {
        if (cf.font_index.has_value()) r->font_index = *cf.font_index;
        if (cf.color.has_value()) r->color = *cf.color;
        if (cf.size.has_value()) {
            if (cf.size->kind != LengthKind::Abs) {
                note(NodeKind::TextRun, node, "字符格式的字号必须是绝对长度");
            } else {
                r->size_pt = static_cast<float>(emuToPt(cf.size->abs_emu));
            }
        }
        if (cf.bold.value_or(false) || cf.italic.value_or(false) || cf.underline.value_or(false)) {
            note(NodeKind::TextRun, node,
                 "字符格式的粗体、斜体、下划线尚未进入编译（引擎输入还没有这些字段）");
        }
    }

    /**
     * 段落格式 → 引擎的块级字段。
     *
     * 模型存的是"内联覆盖"加上一个样式引用，所以这里先按三层解析出完整格式。解析的诊断
     * （未知样式名、继承成环）原样收进产物 —— 那两种情况都会让这一段的格式与作者以为的不一致，
     * 不能只让它们停在解析层。
     *
     * 长度在这里做唯一一次换算：模型存 EMU 整数，引擎吃磅值。非绝对长度（百分比 / em /
     * 字符数）要等排版给出参照才知道多长，编译期拒绝并记诊断 —— 静默取零会让一整段文字
     * 凭空没了字号。
     */
    void applyParaFormat(const Node& mark, BlockSpec* b) {
        std::vector<std::string> why;
        const ParaFormat f = resolveParagraphFormat(sheet_, mark.styleRef, mark.para, &why);
        for (const std::string& w : why) {
            out_.diagnostics.push_back(CompileDiagnostic{mark.kind, mark.id, w});
        }
        b->font_index = f.font_index;
        absToPt(f.size, "字号", mark.id, &b->size_pt);
        b->wm = f.wm;
        b->line_factor = f.line_factor;
        absToPt(f.space_after, "段后距", mark.id, &b->space_after_pt);
        b->page_break_before = f.page_break_before;
        b->language = f.language;
        b->color = f.color;
        b->fallback_font_indices = f.fallback_font_indices;
    }

    /**
     * 绝对长度 → 磅。非绝对长度记诊断并保持引擎的默认值（诊断已经让 ok() 变假，
     * 调用方不会把这份产物当完整结果）。
     */
    void absToPt(const Length& len, const char* field, NodeId mark, float* out) {
        if (len.kind != LengthKind::Abs) {
            out_.diagnostics.push_back(
                CompileDiagnostic{NodeKind::ParagraphMark, mark,
                                  std::string("段落格式的") + field + "必须是绝对长度"});
            return;
        }
        *out = static_cast<float>(emuToPt(len.abs_emu));
    }

    /**
     * 绝对长度 → 磅。Auto 与非绝对都不是编译期能换算的，返回 false 由调用方兜。
     *
     * 非绝对（百分比 / em / 字符位）是这一层真的做不到的事，所以显式记一条诊断 ——
     * 静默按 Auto 处理会让"作者给了 5 厘米宽"变成"按原始像素算"，而画面看着又是合理的。
     * Auto 是作者声明"由排版方决定"，不记。
     */
    bool absLenToPt(const Length& len, const Node& n, const char* field, float* out) {
        if (len.kind == LengthKind::Auto) return false;
        if (len.kind != LengthKind::Abs) {
            note(n.kind, n.id,
                 std::string("原子的") + field + "是相对长度，编译期算不出参照值，已按原始尺寸折算");
            return false;
        }
        *out = static_cast<float>(emuToPt(len.abs_emu));
        return true;
    }

    /**
     * 原子的内容指纹：绘制要看的那些字节（图片是资源标识，公式是表达式与后端）。
     *
     * 尺寸不进它 —— 尺寸改了行盒几何跟着改，行指纹自己就变了。它管的是几何看不见的那一类：
     * 换了一张同样大的图、改了公式表达式。
     */
    uint64_t atomContentHash(const Node& n) {
        uint64_t h = kFnvBasis;
        h = fnvMix(h, static_cast<uint64_t>(n.kind == NodeKind::Image ? 1 : 2));
        if (n.kind == NodeKind::Image) {
            h = fnvMixCStr(h, n.image.resource_id.c_str());
        } else {
            h = fnvMixCStr(h, n.formula.expr.c_str());
            h = fnvMix(h, static_cast<uint64_t>(n.formula.backend));
        }
        return h;
    }

    /**
     * 定出原子盒子的显示尺寸（磅）。
     *
     * 优先用作者给的绝对显示尺寸；只给一边时按原始宽高比推另一边 —— 原图比例是"给了尺寸却
     * 不给另一边"唯一说得通的解释。两边都没有时按原始像素与 DPI 折算（96 DPI 是行业惯例，
     * 见 image_spec.h）。三条都走不通（图片既没有像素也没有显示尺寸，或者给的是编译期算不出
     * 的相对长度）时用默认框并记诊断 —— 排版必须有一份几何，而诊断让调用方知道这不是作者
     * 要的结果。
     */
    void resolveAtomBox(const Node& n, const BlockSpec& b, InlineObject* o) {
        const bool is_img = (n.kind == NodeKind::Image);
        const Length& lw = is_img ? n.image.width : n.formula.width;
        const Length& lh = is_img ? n.image.height : n.formula.height;
        const float dpi =
            (is_img && n.image.dpi > 0) ? static_cast<float>(n.image.dpi) : 96.0f;
        const float px_w = is_img ? static_cast<float>(n.image.px_w) : 0.0f;
        const float px_h = is_img ? static_cast<float>(n.image.px_h) : 0.0f;
        const bool has_px = (px_w > 0.0f && px_h > 0.0f);
        const float nat_w = has_px ? px_w * 72.0f / dpi : 0.0f;
        const float nat_h = has_px ? px_h * 72.0f / dpi : 0.0f;

        // 默认框：图片给一个 4:3 的空框（尺寸本该来自像素或作者，走到这儿说明两者都缺），
        //   公式按表达式的码点数估宽、高取一行。估的是框，不是公式的排版结果 ——
        //   本期不做伪渲染（见 formula_spec.h）。
        float def_w = 0.0f;
        float def_h = 0.0f;
        if (is_img) {
            def_w = 4.0f * b.size_pt * 4.0f;
            def_h = 4.0f * b.size_pt * 3.0f;
        } else {
            const size_t n_cp = utf8ToCodepoints(n.formula.expr).size();
            def_w = 2.0f * b.size_pt + static_cast<float>(n_cp) * 0.5f * b.size_pt;
            def_w = std::min(std::max(def_w, 4.0f * b.size_pt), 40.0f * b.size_pt);
            def_h = b.size_pt * 2.0f;
        }

        float w = 0.0f;
        float h = 0.0f;
        const bool w_abs = absLenToPt(lw, n, "显示宽度", &w);
        const bool h_abs = absLenToPt(lh, n, "显示高度", &h);
        if (w_abs && !h_abs) {
            h = has_px ? w * (nat_h / nat_w) : def_h;
        } else if (!w_abs && h_abs) {
            w = has_px ? h * (nat_w / nat_h) : def_w;
        } else if (!w_abs && !h_abs) {
            w = has_px ? nat_w : def_w;
            h = has_px ? nat_h : def_h;
        }
        o->w_pt = (w > 0.0f) ? w : def_w;
        o->h_pt = (h > 0.0f) ? h : def_h;
    }

    /**
     * flagged 表示"已经在某个未编译的子树里"：那种情况下不再重复记诊断，
     * 只继续按流顺序取文本，免得一棵表格子树刷出几十条同因诊断。
     */
    void walk(NodeId id, bool flagged) {
        const Node* n = tree_.find(id);
        if (n == nullptr) return;
        // 与 FlowIndex 同一口径：呈现态下隐藏的子树整棵不在流里，也就不在内容里。
        if (vis_ == FlowVisibility::Display && n->hidden) return;

        if (n->kind == NodeKind::Section) {
            // 第一节前面没有节，它的分页行为无意义；从第二节起，非连续节把下一个段落
            // 标上段前分页。
            if (sections_seen_ > 0 && n->section.break_kind != SectionBreak::Continuous) {
                pending_section_break_ = true;
            }
            ++sections_seen_;
        }

        if (isText(n->kind)) {
            // 只缓存，不解析：三层解析要拿本段的段落样式，而它挂在这一段末尾的标记上。
            noteFirst(n->id);
            PendingText t;
            t.cps = n->text;
            t.marks = n->marks;
            t.style_ref = n->styleRef;
            t.node = n->id;
            pending_.push_back(std::move(t));
            return;
        }
        if (isParagraphMark(n->kind)) {
            flush(n);
            return;
        }
        // 内容控件：两个边界进内容（第七节），其余一切照旧下钻。进它写一枚起始边界、
        // 离开时写一枚结束边界，锚点都是那一刻"当前段已累积的码点数"—— 所以控件挂在哪一段、
        // 跨了几段都是同一条规则，不必为跨段控件另写一条。
        //
        // 次序是遍历的自然产物：同一处边界上"到此结束"的闭括号先出现（离开得早），
        // "从此开始"的开括号后出现（进入得晚），空控件的一对紧挨着。产出端因此没有排序规则。
        //
        // flagged 为真（控件在某个未编译的子树里，比如表格）时不产边界：那段内容本身没进
        // 产物，凭空多一对括号只会指向一段被诊断标出来的文字。子树照常下钻。
        if (n->kind == NodeKind::ContentControl) {
            // owner 给的是控件的节点 id（收窄成 int：水位从 1 起递增，远不到 2^31）—— 它是
            // 身份而不是序号，于是排版与命中测试拿到的句柄在重排之后仍指同一个控件。
            const int owner = static_cast<int>(n->id);
            const bool ro = n->control.readonly;
            if (!flagged) {
                noteFirst(n->id);
                obj_.push_back(InlineObject{cpCount(), InlineObjectKind::ControlStart, owner, ro});
            }
            for (const Node* c : n->children) walk(c->id, flagged);
            if (!flagged) {
                // 闭括号同样吃进内容，也要记来源。段落从一枚闭括号起是正常形态（前一段的
                //   控件跨到了这一段，它的结束边界落在段首）。不记的话 first 会退化成这一段
                //   自己的结束标记，而结束标记必然在容器内 —— 增量编译的形状判据据此认为起点
                //   就在容器里，于是编出缺掉这枚边界的半段，界面上表现为整段的控件一起消失。
                noteFirst(n->id);
                obj_.push_back(InlineObject{cpCount(), InlineObjectKind::ControlEnd, owner, ro});
            }
            return;
        }
        // 行内原子：图片与公式在流里各占一个位置、各是一个不可断的盒子。它没有孩子、
        //   也不切段，所以走的是与控件边界同一条路 —— 就地攒一枚行内对象，落段时一起带走。
        //   盒子几何留到 flush 再定：公式的默认框要按段落字号估，而字号是段落属性。
        if (n->kind == NodeKind::Image || n->kind == NodeKind::Formula) {
            if (!flagged) {
                noteFirst(n->id);
                InlineObject o;
                o.cp = cpCount();
                o.kind = InlineObjectKind::Atom;
                o.owner = static_cast<int>(n->id);  // 绘制要靠它回树里取"框里写什么"
                o.atom = (n->kind == NodeKind::Image) ? InlineAtom::Image : InlineAtom::Formula;
                o.content_hash = atomContentHash(*n);
                obj_.push_back(o);
            }
            return;
        }
        // 只把分组遍历用的容器与片段根当"支持"：它们是结构外壳，边界不进内容；其余容器与
        // 尚未进编译的原子（域、制表符、分页符）都只记诊断。域（Field）是原子，本期引擎
        // 输入还没有域这一项，所以同样记诊断而不静默丢 —— 它的缓存值留给页眉页脚渲染时再用。
        const bool supported = (n->kind == NodeKind::Document || n->kind == NodeKind::Section ||
                                n->kind == NodeKind::HeaderFooter);
        if (!supported && !flagged) {
            out_.diagnostics.push_back(CompileDiagnostic{
                n->kind, n->id,
                std::string("尚未进入编译的元素：") + nodeKindName(n->kind) + " 未编译"});
        }
        // 容器照常下钻，内部文本按流顺序并入当前段 —— 产物因此是"能编的那部分"，
        // 由 diagnostics 把不完整标出来（ok() 变假）。
        for (const Node* c : n->children) walk(c->id, flagged || !supported);
    }

    const NodeTree& tree_;
    FlowVisibility vis_;
    const StyleSheet& sheet_;
    std::vector<PendingText> pending_;  // 当前段落里待解析的文本段
    std::vector<InlineObject> obj_;     // 当前段落里已攒下的行内对象（控件边界）
    /**
     * 本块第一个吃进内容的元素（见 noteFirst）。落段时收进 sources，随后复位。
     */
    NodeId cur_first_ = kInvalidNodeId;
    CompileResult out_;
    int sections_seen_ = 0;
    bool pending_section_break_ = false;
};

}  // namespace

CompileResult compileDocument(const NodeTree& tree, FlowVisibility vis, const StyleSheet& sheet) {
    return Compiler(tree, vis, sheet).run(tree.rootId());
}

CompileResult compileFragment(const NodeTree& tree, NodeId root, FlowVisibility vis,
                              const StyleSheet& sheet) {
    const Node* n = tree.find(root);
    if (n == nullptr || !isContainer(n->kind)) return CompileResult{};
    return Compiler(tree, vis, sheet).run(root);
}

CompileResult compileChildRange(const NodeTree& tree, NodeId container, int begin, int end,
                                FlowVisibility vis, const StyleSheet& sheet) {
    return Compiler(tree, vis, sheet).runRange(container, begin, end);
}

}  // namespace heditor
