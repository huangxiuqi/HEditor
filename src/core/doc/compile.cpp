#include "core/doc/compile.h"

namespace heditor {

namespace {

/**
 * 一次摊平：按流顺序遍历，边累积码点边切段。
 *
 * 不支持的元素记一条诊断，容器仍下钻取文本；诊断让 ok() 变假，调用方不能把产物当完整结果。
 * 段落格式按文档默认值 → 段落样式链 → 标记上的内联覆盖解析出来，引擎只吃解析后的结果。
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
        // 结尾还没有结束标记的那一段：累积过码点就落一段，用默认段落格式。
        if (!cps_.empty()) flush(nullptr);
        return std::move(out_);
    }

private:
    /**
     * 落一段。mark 为空表示这一段没有结束标记（末尾未结束），用 BlockSpec 的默认格式。
     */
    void flush(const Node* mark) {
        BlockSpec b;
        if (mark != nullptr) applyParaFormat(*mark, &b);
        // 分节强制分页：标记只在下一个段落上落地一次（标记被消费掉），所以一节一段也只会
        // 分一次页。它与段落自己的段前分页是或的关系。
        if (pending_section_break_) {
            b.page_break_before = true;
            pending_section_break_ = false;
        }
        b.cp_text = std::move(cps_);
        cps_.clear();
        out_.blocks.push_back(std::move(b));
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
            // 字符格式已经有了三层解析（1.6），但引擎的输入还没有 run 级字段 —— BlockSpec 一段
            // 只有一套字体与字号。设了却不报出来等于静默丢格式：排版看着没错、只是样式没了，
            // 比"不支持"难查得多。等阶段 2 给引擎加行内区间时，这里换成把解析结果写进去。
            if ((!n->marks.empty() || !n->styleRef.empty()) && !flagged) {
                out_.diagnostics.push_back(CompileDiagnostic{
                    n->kind, n->id,
                    "文本的字符格式尚未进入编译（引擎输入只有块级字段），按段落格式排版"});
            }
            cps_.insert(cps_.end(), n->text.begin(), n->text.end());
            return;
        }
        if (isParagraphMark(n->kind)) {
            flush(n);
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
    std::vector<uint32_t> cps_;  // 正在累积的那一段的码点
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
