#include "core/doc/compile.h"

#include "core/doc/atom_map.h"

#include <algorithm>

#include "core/doc/document.h"
#include "core/doc/format_map.h"
#include "core/doc/table.h"
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
        // 这一块属于哪一格。路径栈在遍历格时压/弹，落段时照抄一份 ——
        // 块的归属与它的内容在同一次落段里定下，不可能出现"内容进了这一段、归属还是上一格"。
        b.cell_path = path_;
        if (mark != nullptr) {
            // 段落格式的三层解析与换算在 format_map 里，投影层用的是同一份。
            applyParaFormatToBlock(
                sheet_, mark->styleRef, mark->para, mark->id, &b,
                [this](const CompileDiagnostic& d) { out_.diagnostics.push_back(d); });
            // 空段落没有文本节点，字符格式只挂在段落标记上：字号决定这一行的高度，
            //   字色决定它将来打出来的字。粗体、斜体、下划线在块级没有字段，跳过 ——
            //   它们经由 marksAtFlow 在敲字时带进文本 run。
            if (b.cp_text.empty()) {
                const CharFormat& m = mark->marks;
                if (m.font_index.has_value()) b.font_index = *m.font_index;
                if (m.size.has_value() && m.size->kind == LengthKind::Abs) {
                    b.size_pt = static_cast<float>(emuToPt(m.size->abs_emu));
                }
                if (m.color.has_value()) b.color = *m.color;
                if (m.bold.has_value()) b.weight = *m.bold ? kWeightBold : kWeightRegular;
                if (m.italic.has_value()) b.italic = *m.italic;
                if (m.underline.has_value()) b.underline = *m.underline;
            }
        }
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
        // 诊断传 nullptr —— 这条链的问题已经由上面的段落格式解析报过，而且两者是同一根
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
            applyCharFormatToRun(cf, t.node, &r, [this](const CompileDiagnostic& d) {
                note(d.kind, d.node, d.detail);
            });

            // 与上一个 run 的格式完全相同就并进去：相邻同格式的文本段在引擎里是一个 run。
            // 不并也对（切分不改产出，这一条在引擎侧有等价判据），但并起来让 runs 是规范的，
            // 不随模型里文本节点的切分方式变化。
            if (!b.runs.empty() && b.runs.back().sameFormat(r)) {
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
            const bool is_img = (atom->kind == NodeKind::Image);
            AtomGeometry g;
            g.is_image = is_img;
            g.width = is_img ? atom->image.width : atom->formula.width;
            g.height = is_img ? atom->image.height : atom->formula.height;
            g.px_w = is_img ? atom->image.px_w : 0;
            g.px_h = is_img ? atom->image.px_h : 0;
            g.dpi = is_img ? atom->image.dpi : 96;
            g.expr_cp = is_img ? 0 : utf8ToCodepoints(atom->formula.expr).size();
            const AtomBox box = resolveAtomBox(g, b.size_pt);
            if (box.width_relative) note(atom->kind, atom->id, atomRelativeDiag(true));
            if (box.height_relative) note(atom->kind, atom->id, atomRelativeDiag(false));
            o.w_pt = box.w_pt;
            o.h_pt = box.h_pt;
        }
        out_.blocks.push_back(std::move(b));
        out_.sources.push_back(BlockSource{first_id, mark_id});
    }

    /**
     * 把"待落的那一段"落掉。没有待落内容时什么也不做 —— 与 flush 的差别在这里：
     * 表格的格边界要的是"如果还有内容就落"，而不是"无论如何产一段空段落"。
     */
    void flushIfPending() {
        if (!pending_.empty() || !obj_.empty()) flush(nullptr);
    }

    /**
     * 模型长度 → 引擎宽度。Auto 保持自动，绝对长度换成磅，百分比换成比例。
     *
     * em 与字符位在这一层算不出参照（要等字号与列宽），记诊断后按自动处理 —— 静默取零会让
     * "作者给了 3 个字宽"变成零宽列，而画面看着又是合理的。
     */
    WidthSpec toWidthSpec(const Length& len, const Node& n, const char* field) {
        const WidthMapping m = mapWidth(len);
        if (m.unresolved) {
            note(n.kind, n.id,
                 std::string("表格的") + field + "是相对长度，编译期算不出参照值，已按自动处理");
        }
        return m.spec;
    }

    /**
     * 进入一张表：填结构表里与内容无关的那一半（尺寸、行属性、列宽），返回它的下标。
     *
     * 网格尺寸由跨度推（与模型层 buildGrid 同一口径），不用"行记录条数乘格数"那种算法 ——
     * 后者在合并单元格上必然错。推不出来时记诊断、列数按第一行的跨度之和给出：内容不能
     * 因此丢，但 ok() 已经为假。
     *
     * 返回下标而不是引用：格内容编译期间可能再 push 进嵌套表格，引用会失效。
     */
    size_t beginTable(const Node& t) {
        LayoutTable lt;
        lt.handle = static_cast<uint64_t>(t.id);
        lt.width = toWidthSpec(t.table.width, t, "宽度");
        switch (t.table.align) {
            case Align::Start:
                lt.align = TableHAlign::Start;
                break;
            case Align::Center:
                lt.align = TableHAlign::Center;
                break;
            case Align::End:
                lt.align = TableHAlign::End;
                break;
        }
        int rows = 0;
        int first_row_cols = 0;
        for (const Node* c : t.children) {
            if (c->kind != NodeKind::TableRow) continue;
            ++rows;
            if (rows == 1) {
                for (const Node* cell : c->children) {
                    if (cell->kind == NodeKind::TableCell) first_row_cols += cell->cell.col_span;
                }
            }
        }
        lt.rows = rows;
        TableGrid grid;
        std::string why;
        if (buildGrid(tree_, t.id, &grid, &why)) {
            lt.cols = grid.cols;
        } else {
            note(NodeKind::Table, t.id, "表格结构不成立：" + why);
            lt.cols = first_row_cols;
        }
        lt.col_widths.assign(static_cast<size_t>(lt.cols), WidthSpec{});
        for (int i = 0; i < lt.cols && i < static_cast<int>(t.table.grid_cols.size()); ++i) {
            lt.col_widths[static_cast<size_t>(i)] =
                toWidthSpec(t.table.grid_cols[static_cast<size_t>(i)], t, "列宽");
        }
        lt.row_specs.reserve(static_cast<size_t>(rows));
        for (const Node* c : t.children) {
            if (c->kind != NodeKind::TableRow) continue;
            LayoutRow lr;
            lr.can_split_by_page = c->row.can_split_by_page;
            lr.is_header = c->row.is_header;
            lr.page_break_before = c->row.page_break_before;
            lr.height_rule = mapRowHeightRule(c->row.height_rule);
            const bool has_value = (c->row.height.kind != LengthKind::Auto);
            if (lr.height_rule == RowHeightRule::Auto) {
                if (has_value) {
                    note(NodeKind::TableRow, c->id, "行高的规则是自动，但作者给了值（值不被解释）");
                }
            } else if (!has_value) {
                note(NodeKind::TableRow, c->id, "行高规则要求一个值，但给的是自动");
            } else if (c->row.height.kind == LengthKind::Abs) {
                lr.height_pt = static_cast<float>(emuToPt(c->row.height.abs_emu));
            } else {
                note(NodeKind::TableRow, c->id, "行高必须是绝对长度");
            }
            lt.row_specs.push_back(lr);
        }
        out_.tables.push_back(std::move(lt));
        return out_.tables.size() - 1;
    }

    /**
     * 一张表：按行按格遍历，把格里的内容编成段落。
     *
     * 每一格是独立的段落上下文：进格前把格外的残留落掉（表格是块级元素，不与正文共用一段），
     * 格结束时落掉格内最后一段 —— 格内结尾常常没有段落标记，不落它这一格就会与下一格并成
     * 一段。空格的块数为 0，由排版层按"一个空段落占一行"处理。
     *
     * 用 out_.tables 的下标而不是引用：格内容里可能再 push 进嵌套表格，vector 会重新分配。
     */
    void walkTable(const Node& t) {
        const size_t ti = beginTable(t);
        flushIfPending();
        int row_no = 0;
        for (const Node* row : t.children) {
            if (row->kind != NodeKind::TableRow) continue;
            int cell_no = 0;
            for (const Node* cell : row->children) {
                if (cell->kind != NodeKind::TableCell) continue;
                path_.push_back(CellSlot{static_cast<uint64_t>(t.id), row_no, cell_no});
                flushIfPending();
                LayoutCell lc;
                lc.row = row_no;
                lc.cell = cell_no;
                lc.row_span = cell->cell.row_span;
                lc.col_span = cell->cell.col_span;
                lc.v_align = mapCellVAlign(cell->cell.v_align);
                lc.first_block = static_cast<int>(out_.blocks.size());
                const size_t ci = out_.tables[ti].cells.size();
                out_.tables[ti].cells.push_back(lc);
                for (const Node* c : cell->children) walk(c->id, false);
                flushIfPending();
                out_.tables[ti].cells[ci].block_count =
                    static_cast<int>(out_.blocks.size()) - out_.tables[ti].cells[ci].first_block;
                path_.pop_back();
                ++cell_no;
            }
            ++row_no;
        }
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
            // 边界的内容指纹带外观摘要：改记号色 / 边框粗细时位置一个都不动，行指纹
            //   只能靠它判"这一段要重画"（与投影入口同一个函数，别各写一份）。
            const uint64_t look = controlAppearanceDigest(n->control.appearance);
            if (!flagged) {
                noteFirst(n->id);
                InlineObject o;
                o.cp = cpCount();
                o.kind = InlineObjectKind::ControlStart;
                o.owner = owner;
                o.read_only = ro;
                o.content_hash = look;
                obj_.push_back(o);
            }
            for (const Node* c : n->children) walk(c->id, flagged);
            if (!flagged) {
                // 闭括号同样吃进内容，也要记来源。段落从一枚闭括号起是正常形态（前一段的
                //   控件跨到了这一段，它的结束边界落在段首）。不记的话 first 会退化成这一段
                //   自己的结束标记，而结束标记必然在容器内 —— 增量编译的形状判据据此认为起点
                //   就在容器里，于是编出缺掉这枚边界的半段，界面上表现为整段的控件一起消失。
                noteFirst(n->id);
                InlineObject o;
                o.cp = cpCount();
                o.kind = InlineObjectKind::ControlEnd;
                o.owner = owner;
                o.read_only = ro;
                o.content_hash = look;
                obj_.push_back(o);
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
                o.content_hash = (n->kind == NodeKind::Image) ? atomContentHash(n->image)
                                                              : atomContentHash(n->formula);
                obj_.push_back(o);
            }
            return;
        }
        // 表格：扁平化进块流。格里的内容照常编成段落（带单元格路径），结构表另记一份 ——
        // 排版要按格取内容，光标要按块找格，两个方向各留一条索引。
        //
        // flagged 为真（表格本身在某个未编译的子树里）时只下钻不产结构：那部分内容没进产物，
        // 凭空多一张结构表只会指向一段被诊断标出来的文字。
        if (n->kind == NodeKind::Table) {
            if (!flagged) {
                walkTable(*n);
            } else {
                for (const Node* c : n->children) walk(c->id, true);
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
                n->kind, n->id, std::string("尚未进入排版输入的元素：") + nodeKindName(n->kind)});
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
     * 当前所在的单元格路径（外层到内层），空表示正文。遍历格时压/弹，落段时抄进 BlockSpec。
     */
    std::vector<CellSlot> path_;
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
