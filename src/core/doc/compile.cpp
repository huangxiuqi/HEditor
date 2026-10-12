#include "core/doc/compile.h"

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
        out_.blocks.push_back(std::move(b));
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
                obj_.push_back(InlineObject{cpCount(), InlineObjectKind::ControlStart, owner, ro});
            }
            for (const Node* c : n->children) walk(c->id, flagged);
            if (!flagged) {
                obj_.push_back(InlineObject{cpCount(), InlineObjectKind::ControlEnd, owner, ro});
            }
            return;
        }
        // 只把分组遍历用的容器与片段根当"支持"：它们是结构外壳，边界不进内容；其余容器与
        // 全部原子都只记诊断。域（Field）是原子，本期引擎输入还没有域这一项，所以同样记
        // 诊断而不静默丢 —— 它的缓存值留给页眉页脚渲染时再用。
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

}  // namespace heditor
