#include "core/doc/compile.h"

namespace heditor {

namespace {

/**
 * 一次摊平：按流顺序遍历，边累积码点边切段。
 *
 * 只做 0.4 的窄口径（见 compile.h 顶部）。不支持的元素记一条诊断，容器仍下钻取文本，
 * 所以产出的段落文本可能只在"样式与边界语义"上不完整；诊断必须让 ok() 变假，
 * 调用方不能把产物当完整结果用。
 */
class Compiler {
public:
    Compiler(const NodeTree& tree, FlowVisibility vis) : tree_(tree), vis_(vis) {}

    CompileResult run() {
        walk(tree_.rootId(), false);
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
        b.cp_text = std::move(cps_);
        cps_.clear();
        out_.blocks.push_back(std::move(b));
    }

    /**
     * 段落格式 → 引擎的块级字段。
     *
     * 长度在这里做唯一一次换算：模型存 EMU 整数，引擎吃磅值。非绝对长度（百分比 / em /
     * 字符数）要等排版给出参照才知道多长，编译期拒绝并记诊断 —— 静默取零会让一整段文字
     * 凭空没了字号。
     */
    void applyParaFormat(const Node& mark, BlockSpec* b) {
        const ParaFormat& f = mark.para;
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

        if (isText(n->kind)) {
            // 字符样式还没进编译（要等 1.6 的样式表）。设了样式却不报出来，等于静默丢格式：
            // 排版结果看着没错，只是样式没了，比"不支持"难查得多。
            if (!n->marks.empty() && !flagged) {
                out_.diagnostics.push_back(CompileDiagnostic{
                    n->kind, n->id, "文本的字符样式尚未进入编译（等 1.6 样式表），marks 被忽略"});
            }
            cps_.insert(cps_.end(), n->text.begin(), n->text.end());
            return;
        }
        if (isParagraphMark(n->kind)) {
            flush(n);
            return;
        }
        // 只有 Document 与 Section 是 0.4 认的容器；其余容器与全部原子都只记诊断。
        const bool supported = (n->kind == NodeKind::Document || n->kind == NodeKind::Section);
        if (!supported && !flagged) {
            out_.diagnostics.push_back(CompileDiagnostic{
                n->kind, n->id,
                std::string("0.4 只支持文本与段落：") + nodeKindName(n->kind) + " 未编译"});
        }
        // 容器照常下钻，内部文本按流顺序并入当前段 —— 0.4 的产物因此是"能编的那部分"，
        // 由 diagnostics 把不完整标出来（ok() 变假）。
        for (const Node* c : n->children) walk(c->id, flagged || !supported);
    }

    const NodeTree& tree_;
    FlowVisibility vis_;
    std::vector<uint32_t> cps_;  // 正在累积的那一段的码点
    CompileResult out_;
};

}  // namespace

CompileResult compileDocument(const NodeTree& tree, FlowVisibility vis) {
    return Compiler(tree, vis).run();
}

}  // namespace heditor
