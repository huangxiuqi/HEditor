#include "core/rev/project.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "core/doc/atom_map.h"
#include "core/doc/format_map.h"
#include "core/doc/length.h"
#include "core/doc/table.h"

namespace heditor::rev {

namespace {

/**
 * 一次遍历：按流顺序把修订摊成块，同时攒下控件边界与每块的哈希。
 *
 * full 为假时只算哈希（blockHashes 用）：不拼文本、不解析格式 —— 那两样才是投影的大头，
 * 而"哪几块变了"只需要节点哈希。
 */
class Walker {
public:
    Walker(const StyleSheet* sheet, const ViewParams& vp, bool full)
        : sheet_(sheet), vp_(vp), full_(full) {}

    Projection run(const Doc& doc) {
        // 从根的孩子表走：每一节都在里面。段落号在节之间连续，投影按流顺序攒块，
        //   所以"跳过第二节"不会报错，只会少几块 —— 只走第一节是静默丢内容。
        if (doc.root != nullptr) {
            for (const Ref& c : doc.root->children) walk(c);
            // 结尾还没有结束标记的那一段：累积过文本、或只攒下过控件边界的，都要落成一段，
            // 用默认段落格式。少了后半个条件，"控件从文档开头就开、内容为空"那种文书会连
            // 一段都产不出来，控件边界于是凭空消失。
            if (!runs_.empty() || !objects_.empty()) flush(nullptr);
        }
        out_.style_version = (sheet_ != nullptr) ? sheet_->version() : 0;
        return std::move(out_);
    }

private:
    /**
     * 一个节点自身（不含孩子）的哈希。容器几何进块哈希与损伤判定都用它。
     */
    static std::pair<uint64_t, uint64_t> selfOf(const Ref& n) {
        Hasher h;
        hashSelf(h, *n);
        return {h.lo(), h.hi()};
    }

    /**
     * 进入一个"影响孩子怎么排"的容器（表格、行、格）。它的记录进这一层往里每个块的哈希：
     * 列宽与跨度变了，块里的字一个都没动，但块要重排 —— 只比内容会让排版缓存命中旧结果。
     */
    void pushContainer(const Ref& n) {
        const std::pair<uint64_t, uint64_t> h = selfOf(n);
        containers_.push_back(h);
    }

    void popContainer() { containers_.pop_back(); }

    /**
     * 当前段已经累积了多少个码点 —— 行内对象的锚点就是它。run 按流顺序攒在 runs_ 里
     * （控件内部的文字也在其中），所以这个数与落段时逐段拼进去的结果一致。
     */
    int cpCount() const {
        int n = 0;
        for (const Ref& r : runs_) {
            n += static_cast<int>(std::get<RunPayload>(r->payload).text.size());
        }
        return n;
    }

    /**
     * 攒一枚行内对象，同时把它喂进累加哈希。
     *
     * 边界本身只带位置与身份，控件记录（键、类型、外观……）不在边界上 —— 所以这里把控件节点的
     * 哈希也喂进去。少了这一句，改控件的任何一个属性都不会动块哈希，画面就停在旧内容上。
     */
    void emitObject(const InlineObject& o, const Ref& node) {
        obj_hash_.u64(o.cp);
        obj_hash_.u8(static_cast<uint8_t>(o.kind));
        obj_hash_.i64(o.owner);
        obj_hash_.u8(o.read_only ? 1 : 0);
        obj_hash_.u64(node != nullptr ? node->hash_lo : 0);
        obj_hash_.u64(node != nullptr ? node->hash_hi : 0);
        objects_.push_back(o);
    }

    /**
     * 记下本块第一个吃进内容的元素 —— 只在"这一块还没有过任何内容"时生效。
     */
    void noteFirst(NodeId id) {
        if (cur_first_ == kInvalidNodeId && runs_.empty() && objects_.empty()) cur_first_ = id;
    }

    /**
     * 记一条诊断，同一条原因只记一次。
     *
     * 为什么要去重：同一段里十个文本节点引用同一个未知样式会刷出十条一模一样的消息，
     * 把真正的信号淹掉。与编译那一侧同一条纪律（诊断按 detail 去重）。
     */
    void note(NodeKind kind, NodeId node, const std::string& detail) {
        for (const CompileDiagnostic& d : out_.diagnostics) {
            if (d.detail == detail) return;
        }
        out_.diagnostics.push_back(CompileDiagnostic{kind, node, detail});
    }

    /**
     * 进入一张表：填结构表里与内容无关的那一半（尺寸、列、行属性、列宽）。
     *
     * 网格由跨度推（规则只有 buildGridFromSpans 一份，与模型层同一实现），推不出来时记诊断、
     * 列数按第一行的跨度之和给出：内容不能因此丢，但 ok() 已经为假。
     */
    void beginTable(const Ref& t) {
        const TableSpec& ts = std::get<TableSpec>(t->payload);
        LayoutTable lt;
        lt.handle = static_cast<uint64_t>(t->id);
        const WidthMapping w = mapWidth(ts.width);
        if (w.unresolved) {
            note(NodeKind::Table, t->id, "表格的宽度是相对长度，投影期算不出参照值，已按自动处理");
        }
        lt.width = w.spec;
        switch (ts.align) {
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

        std::vector<const Ref*> rows;
        for (const Ref& c : t->children) {
            if (c->kind != NodeKind::TableRow) {
                note(NodeKind::Table, t->id,
                     std::string("表格的直接孩子里有") + nodeKindName(c->kind) + "，不是行");
                continue;
            }
            rows.push_back(&c);
        }
        lt.rows = static_cast<int>(rows.size());

        std::vector<std::vector<GridCellSpan>> spans;
        spans.reserve(rows.size());
        for (const Ref* row : rows) {
            std::vector<GridCellSpan> line;
            for (const Ref& cell : (*row)->children) {
                if (cell->kind != NodeKind::TableCell) {
                    note(NodeKind::TableRow, (*row)->id,
                         std::string("行的直接孩子里有") + nodeKindName(cell->kind) +
                             "，不是单元格");
                    continue;
                }
                const CellSpec& cs = std::get<CellSpec>(cell->payload);
                line.push_back(GridCellSpan{cell->id, cs.row_span, cs.col_span});
            }
            spans.push_back(std::move(line));
        }
        TableGrid grid;
        std::string why;
        if (buildGridFromSpans(spans, &grid, &why)) {
            lt.cols = grid.cols;
        } else {
            note(NodeKind::Table, t->id, "表格结构不成立：" + why);
            lt.cols = 0;
            if (!spans.empty()) {
                for (const GridCellSpan& c : spans.front()) lt.cols += c.col_span;
            }
        }
        lt.col_widths.assign(static_cast<size_t>(lt.cols), WidthSpec{});
        for (int i = 0; i < lt.cols && i < static_cast<int>(ts.grid_cols.size()); ++i) {
            const WidthMapping cw = mapWidth(ts.grid_cols[static_cast<size_t>(i)]);
            if (cw.unresolved) {
                note(NodeKind::Table, t->id,
                     "表格的列宽是相对长度，投影期算不出参照值，已按自动处理");
            }
            lt.col_widths[static_cast<size_t>(i)] = cw.spec;
        }

        lt.row_specs.reserve(rows.size());
        for (const Ref* row : rows) {
            const RowSpec& rs = std::get<RowSpec>((*row)->payload);
            LayoutRow lr;
            lr.can_split_by_page = rs.can_split_by_page;
            lr.is_header = rs.is_header;
            lr.page_break_before = rs.page_break_before;
            lr.height_rule = mapRowHeightRule(rs.height_rule);
            const bool has_value = (rs.height.kind != LengthKind::Auto);
            if (lr.height_rule == RowHeightRule::Auto) {
                if (has_value) {
                    note(NodeKind::TableRow, (*row)->id,
                         "行高的规则是自动，但作者给了值（值不被解释）");
                }
            } else if (!has_value) {
                note(NodeKind::TableRow, (*row)->id, "行高规则要求一个值，但给的是自动");
            } else if (rs.height.kind == LengthKind::Abs) {
                lr.height_pt = static_cast<float>(emuToPt(rs.height.abs_emu));
            } else {
                note(NodeKind::TableRow, (*row)->id, "行高必须是绝对长度");
            }
            lt.row_specs.push_back(lr);
        }
        out_.tables.push_back(std::move(lt));
    }

    /**
     * 一张表：按行按格遍历，把格里的内容投影成块。
     *
     * 每一格是独立的段落上下文：进格前把格外的残留落掉，格结束时落掉格内最后一段 ——
     * 格内结尾常常没有段落标记，不落它这一格就会与下一格并成一段。
     */
    void walkTable(const Ref& t) {
        beginTable(t);
        const int ti = static_cast<int>(out_.tables.size()) - 1;
        flushIfPending();
        pushContainer(t);
        int row_no = 0;
        for (const Ref& row : t->children) {
            if (row->kind != NodeKind::TableRow) continue;
            pushContainer(row);
            int cell_no = 0;
            for (const Ref& cell : row->children) {
                if (cell->kind != NodeKind::TableCell) continue;
                path_.push_back(CellSlot{static_cast<uint64_t>(t->id), row_no, cell_no});
                flushIfPending();
                const CellSpec& cs = std::get<CellSpec>(cell->payload);
                LayoutCell lc;
                lc.row = row_no;
                lc.cell = cell_no;
                lc.row_span = cs.row_span;
                lc.col_span = cs.col_span;
                lc.v_align = mapCellVAlign(cs.v_align);
                lc.first_block = static_cast<int>(out_.blocks.size());
                const int ci = static_cast<int>(out_.tables[ti].cells.size());
                out_.tables[ti].cells.push_back(lc);
                pushContainer(cell);
                for (const Ref& c : cell->children) walk(c);
                flushIfPending();
                popContainer();
                out_.tables[ti].cells[static_cast<size_t>(ci)].block_count =
                    static_cast<int>(out_.blocks.size()) - lc.first_block;
                path_.pop_back();
                ++cell_no;
            }
            popContainer();
            ++row_no;
        }
        popContainer();
    }

    /**
     * 落掉还没结束的一段（表格是块级元素，进入与离开它的时候都不该有残留）。
     */
    void flushIfPending() {
        if (!runs_.empty() || !objects_.empty()) flush(nullptr);
    }

    void walk(const Ref& n) {
        // 与 FlowIndex 同一口径：呈现态下隐藏的子树整棵不在流里，也就不在内容里。
        if (vp_.vis == FlowVisibility::Display && n->hidden) return;

        if (n->kind == NodeKind::TextRun) {
            noteFirst(n->id);
            runs_.push_back(n);
            return;
        }
        if (n->kind == NodeKind::ParagraphMark) {
            flush(n);
            return;
        }
        // 行内原子：图片与公式各占一个位置、各是一个不可断的盒子。盒子几何要等块级字号
        // （公式的默认框按字号估），所以这里只记下"这里有一枚原子"，落段时再定。
        if (n->kind == NodeKind::Image || n->kind == NodeKind::Formula) {
            noteFirst(n->id);
            const bool is_img = (n->kind == NodeKind::Image);
            const ImageSpec& img = is_img ? std::get<ImageSpec>(n->payload) : ImageSpec{};
            const FormulaSpec& fml = is_img ? FormulaSpec{} : std::get<FormulaSpec>(n->payload);
            InlineObject o;
            o.cp = static_cast<uint32_t>(cpCount());
            o.kind = InlineObjectKind::Atom;
            o.owner = static_cast<int>(n->id);  // 绘制要靠它回节点取"框里写什么"
            o.atom = is_img ? InlineAtom::Image : InlineAtom::Formula;
            o.content_hash = is_img ? atomContentHash(img) : atomContentHash(fml);
            AtomGeometry g;
            g.is_image = is_img;
            g.width = is_img ? img.width : fml.width;
            g.height = is_img ? img.height : fml.height;
            g.px_w = img.px_w;
            g.px_h = img.px_h;
            g.dpi = img.dpi;
            g.expr_cp = is_img ? 0 : utf8ToCodepoints(fml.expr).size();
            emitObject(o, n);
            atoms_.emplace_back(objects_.size() - 1, g);
            return;
        }
        if (n->kind == NodeKind::Section) {
            // 第一节前面没有节，它的分页行为无意义；从第二节起，非连续节把下一个段落标上
            //   段前分页。标记只落地一次（见 flush）。
            if (sections_seen_ > 0) {
                const SectionSpec& sec = std::get<SectionSpec>(n->payload);
                if (sec.break_kind != SectionBreak::Continuous) pending_section_break_ = true;
            }
            ++sections_seen_;
        }
        if (n->kind == NodeKind::Table) {
            walkTable(n);
            return;
        }
        // 内容控件：两个边界进内容，其余一切照旧下钻。进它写一枚起始边界、离开时写一枚结束
        // 边界，锚点都是那一刻"当前段已累积的码点数"—— 所以控件挂在哪一段、跨了几段都是
        // 同一条规则。同一处边界上"到此结束"的闭括号先出现（离开得早），"从此开始"的开括号
        // 后出现（进入得晚），产出端因此不需要排序规则。
        if (n->kind == NodeKind::ContentControl) {
            const ControlSpec& spec = std::get<ControlSpec>(n->payload);
            const int owner = static_cast<int>(n->id);
            ControlSpan span;
            span.node = n;
            span.block = static_cast<int>(out_.blocks.size());
            span.cp = cpCount();
            noteFirst(n->id);
            emitObject(InlineObject{static_cast<uint32_t>(cpCount()),
                                    InlineObjectKind::ControlStart, owner, spec.readonly},
                       n);
            for (const Ref& c : n->children) walk(c);
            span.end_block = static_cast<int>(out_.blocks.size());
            span.end_cp = cpCount();
            noteFirst(n->id);
            emitObject(InlineObject{static_cast<uint32_t>(cpCount()), InlineObjectKind::ControlEnd,
                                    owner, spec.readonly},
                       n);
            out_.controls.push_back(std::move(span));
            return;
        }
        // 结构外壳：文档、节、页眉页脚只提供层级，边界不进内容，下去即可（与编译同一口径）。
        if (n->kind == NodeKind::Section || n->kind == NodeKind::Document ||
            n->kind == NodeKind::HeaderFooter) {
            for (const Ref& c : n->children) walk(c);
            return;
        }
        // 其余元素一律写成诊断：静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。
        // 容器仍照常下钻，内部文本按流顺序并入当前段。
        out_.diagnostics.push_back(CompileDiagnostic{
            n->kind, n->id, std::string("尚未进入排版输入的元素：") + nodeKindName(n->kind)});
        for (const Ref& c : n->children) walk(c);
    }

    /**
     * 落一段。mark 为空表示这一段没有结束标记（末尾未结束），用默认段落格式。
     */
    void flush(const Ref& mark) {
        ProjectedBlock pb;
        pb.source.mark = (mark != nullptr) ? mark->id : kInvalidNodeId;
        pb.source.first = (cur_first_ != kInvalidNodeId) ? cur_first_ : pb.source.mark;
        cur_first_ = kInvalidNodeId;

        // 块哈希只从贡献这个块的节点与对象算。节点哈希本身已经是内容哈希，所以"改一段"只
        // 改动那一段的块哈希 —— 增量排版据此判断损伤，不需要任何人上报改了什么。
        Hasher h;
        h.u64(runs_.size());
        for (const Ref& r : runs_) {
            h.u64(r->hash_lo);
            h.u64(r->hash_hi);
        }
        h.u64(mark ? 1 : 0);
        if (mark) {
            h.u64(mark->hash_lo);
            h.u64(mark->hash_hi);
        }
        // 行内对象在产出时就喂进了 obj_hash_（连同产生它的节点哈希），这里把它并进来。
        h.u64(obj_hash_.lo());
        h.u64(obj_hash_.hi());
        // 容器几何：外层表格的列宽、行高、跨度决定这一块排成什么形状，所以它们进块哈希 ——
        // 少了这一段，"只改了列宽"会让每一块都命中排版缓存，损伤报得再对也没用。
        h.u64(containers_.size());
        for (const std::pair<uint64_t, uint64_t>& c : containers_) {
            h.u64(c.first);
            h.u64(c.second);
        }
        // 格路径：同一张表里两格内容相同、列宽不同，排版结果不一样，所以位置也进键。
        h.u64(path_.size());
        for (const CellSlot& slot : path_) {
            h.u64(slot.table);
            h.i64(slot.row);
            h.i64(slot.cell);
        }
        pb.hash = BlockHash{h.lo(), h.hi()};
        obj_hash_ = Hasher{};

        if (full_) {
            // 原子的盒子在这里定：公式的默认框按段落字号估，而字号是段落属性 —— 走到原子
            // 那一刻还看不到它。emit 那一刻只记下"这里有一枚原子"。
            for (const std::pair<std::size_t, AtomGeometry>& a : atoms_) {
                const AtomBox box = resolveAtomBox(a.second, pb.spec.size_pt);
                if (box.width_relative) {
                    note(NodeKind::Image, objects_[a.first].owner, atomRelativeDiag(true));
                }
                if (box.height_relative) {
                    note(NodeKind::Image, objects_[a.first].owner, atomRelativeDiag(false));
                }
                objects_[a.first].w_pt = box.w_pt;
                objects_[a.first].h_pt = box.h_pt;
            }
            pb.spec.cell_path = path_;
            pb.spec.objects = std::move(objects_);
            if (mark != nullptr) {
                const MarkPayload& mp = std::get<MarkPayload>(mark->payload);
                applyParaFormatToBlock(
                    *sheet_, mp.style, mp.para, mark->id, &pb.spec,
                    [this](const CompileDiagnostic& d) { out_.diagnostics.push_back(d); });
            }
            // 段落默认字符格式就是刚解析出来的那组块级字段。run 以它为底、三层解析盖在上面。
            const FormatRun para_default = pb.spec.wholeRun();
            const std::string para_style =
                (mark != nullptr) ? std::get<MarkPayload>(mark->payload).style : std::string();
            const CharFormat base = paragraphRunBase(*sheet_, para_style, nullptr);

            for (const Ref& r : runs_) {
                const RunPayload& rp = std::get<RunPayload>(r->payload);
                FormatRun fr = para_default;
                fr.cp_begin = static_cast<uint32_t>(pb.spec.cp_text.size());
                pb.spec.cp_text.insert(pb.spec.cp_text.end(), rp.text.begin(), rp.text.end());
                fr.cp_end = static_cast<uint32_t>(pb.spec.cp_text.size());

                std::vector<std::string> why;
                const CharFormat cf = resolveRunFormatFrom(*sheet_, base, rp.style, rp.marks, &why);
                for (const std::string& w : why) note(NodeKind::TextRun, r->id, w);
                applyCharFormatToRun(cf, r->id, &fr, [this](const CompileDiagnostic& d) {
                    note(d.kind, d.node, d.detail);
                });
                if (!pb.spec.runs.empty() && pb.spec.runs.back().sameFormat(fr)) {
                    pb.spec.runs.back().cp_end = fr.cp_end;
                } else {
                    pb.spec.runs.push_back(fr);
                }
            }
        }
        // 分节强制分页：标记只在下一个段落上落地一次，与段落自己的段前分页是或的关系。
        //   必须放在段落格式落地之后 —— 那一步会按段落覆盖面把这一位重置成作者的取值。
        if (pending_section_break_) {
            pb.spec.page_break_before = true;
            pending_section_break_ = false;
        }

        objects_.clear();
        atoms_.clear();
        runs_.clear();
        out_.blocks.push_back(std::move(pb));
    }

    const StyleSheet* sheet_;
    ViewParams vp_;
    bool full_;
    std::vector<Ref> runs_;              // 当前段里按流顺序攒下的 run
    std::vector<InlineObject> objects_;  // 当前段里攒下的行内对象（控件边界与原子）
    /**
     * 当前段里待定几何的原子：对象下标 + 它需要的输入。落段时才定得下来（要块级字号）。
     */
    std::vector<std::pair<std::size_t, AtomGeometry>> atoms_;
    int sections_seen_ = 0;                                  // 走过几节（第一节的分页行为无意义）
    bool pending_section_break_ = false;                     // 分节强制分页还没落到段上
    std::vector<CellSlot> path_;                             // 当前所在的格（空 = 正文）
    std::vector<std::pair<uint64_t, uint64_t>> containers_;  // 外层容器的自身哈希
    NodeId cur_first_ = kInvalidNodeId;
    Hasher obj_hash_;
    Projection out_;
};

}  // namespace

Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params) {
    return Walker(&sheet, params, true).run(doc);
}

std::vector<BlockHash> blockHashes(const Doc& doc, FlowVisibility vis) {
    ViewParams vp;
    vp.vis = vis;
    const Projection p = Walker(nullptr, vp, false).run(doc);
    std::vector<BlockHash> out;
    out.reserve(p.blocks.size());
    for (const ProjectedBlock& b : p.blocks) out.push_back(b.hash);
    return out;
}

BlockDamage diffBlocks(const std::vector<BlockHash>& before, const std::vector<BlockHash>& after) {
    const int n = static_cast<int>(before.size());
    const int m = static_cast<int>(after.size());
    int p = 0;
    while (p < n && p < m && before[p] == after[p]) ++p;
    int s = 0;
    // 后缀不越过前缀已匹配的那一段 —— 否则一个大删除会被算成"删了又插"。
    while (s < n - p && s < m - p && before[n - 1 - s] == after[m - 1 - s]) ++s;
    bool changed = (n != m);
    for (int i = 0; !changed && i < n; ++i) {
        if (!(before[i] == after[i])) changed = true;
    }
    return BlockDamage{p, m - s, changed};
}

namespace {

/**
 * 按哈希下降找损伤。
 *
 * 每一层先比哈希：相同就整棵剪掉（"整篇没变"能是 O(1) 的原因就在这里）；不同才往下走。
 * 孩子表按 NodeId 对齐 —— 没有身份就只能按下标猜，而中间插一段之后下标全错位。
 *
 * 一种容易被漏掉的情形：改的是叶子（run 里的文字）。它自己没有孩子、也不含段落标记，
 * 递归到它就是终点，若不在这里记一笔，损伤会空着报出去 —— 画面停在旧内容上，而所有
 * 断言都是绿的。所以"两边都不含段落标记"的差异一律记成它所在的那一块。
 */
class DamageWalker {
public:
    void walk(const Ref& a, const Ref& b, int& off) {
        ++visited;
        if (a == b || (a->hash_lo == b->hash_lo && a->hash_hi == b->hash_hi)) {
            off += b->paras;
            return;
        }
        // 容器自己的记录变了（列宽、行高、跨度、控件外观）：它覆盖的块整段算损伤。
        // 这一条不能省 —— 改列宽时每一块的内容哈希都没变，只比内容会报"没有损伤"，
        // 而整张表的行高与分页都可能跟着变。
        if (selfDiffers(a, b)) {
            mark(off, b->paras > 0 ? b->paras : 1);
            off += b->paras;
            return;
        }
        // 自身没变、两边都不含段落标记 ⇒ 这点差异落在某一块的内部，那一段就是它的归属。
        if (a->paras == 0 && b->paras == 0) {
            mark(off, 1);
            return;
        }
        const std::vector<Ref>& x = a->children;
        const std::vector<Ref>& y = b->children;
        // 快路径：孩子在原位被换掉（改字、改格式、改控件属性都是这种）。
        bool same_shape = x.size() == y.size();
        for (size_t k = 0; same_shape && k < x.size(); ++k) {
            ++scanned;
            if (x[k]->id != y[k]->id) same_shape = false;
        }
        if (same_shape) {
            for (size_t k = 0; k < x.size(); ++k) walk(x[k], y[k], off);
            return;
        }
        alignChildren(x, y, off);
    }

    void mark(int from, int count) {
        if (count <= 0) count = 0;
        if (!any) {
            lo = from;
            hi = from + count;
            any = true;
            return;
        }
        lo = std::min(lo, from);
        hi = std::max(hi, from + count);
    }

    bool any = false;
    int lo = 0;
    int hi = 0;
    int visited = 0;
    int scanned = 0;

private:
    static bool selfDiffers(const Ref& a, const Ref& b) {
        Hasher ha;
        Hasher hb;
        hashSelf(ha, *a);
        hashSelf(hb, *b);
        return ha.lo() != hb.lo() || ha.hi() != hb.hi();
    }

    /**
     * 孩子表真的增删了才走这里。只在这一层建身份集合，不整篇建。
     *
     * 次序被打乱（两边都有、但位置不同）时按"删一个再插一个"处理：多报比漏报安全，
     * 而且本切片的编辑原语不产生次序变化。
     */
    void alignChildren(const std::vector<Ref>& x, const std::vector<Ref>& y, int& off) {
        std::unordered_set<NodeId> in_x;
        std::unordered_set<NodeId> in_y;
        for (const Ref& c : x) in_x.insert(c->id);
        for (const Ref& c : y) in_y.insert(c->id);

        size_t i = 0;
        size_t j = 0;
        while (i < x.size() || j < y.size()) {
            ++scanned;
            if (i >= x.size()) {
                // 插入：它自己带来的块都算损伤，位置就在当前 off。
                mark(off, y[j]->paras > 0 ? y[j]->paras : 1);
                off += y[j]->paras;
                ++j;
                continue;
            }
            if (j >= y.size()) {
                // 删除：它占的块没了，位置在当前 off（后面的块会往前挪一格）。
                mark(off, x[i]->paras > 0 ? x[i]->paras : 1);
                ++i;
                continue;
            }
            if (x[i]->id == y[j]->id) {
                walk(x[i], y[j], off);
                ++i;
                ++j;
                continue;
            }
            if (in_y.count(x[i]->id) == 0) {
                mark(off, x[i]->paras > 0 ? x[i]->paras : 1);
                ++i;
                continue;
            }
            if (in_x.count(y[j]->id) == 0) {
                mark(off, y[j]->paras > 0 ? y[j]->paras : 1);
                off += y[j]->paras;
                ++j;
                continue;
            }
            mark(off, x[i]->paras > 0 ? x[i]->paras : 1);
            ++i;
        }
    }
};

}  // namespace

Damage damage(const Doc& before, const Doc& after) {
    Damage out;
    if (!before.root || !after.root) {
        out.range.changed = (before.root != after.root);
        return out;
    }
    DamageWalker w;
    int off = 0;
    w.walk(before.root, after.root, off);
    out.visited = w.visited;
    out.scanned = w.scanned;
    out.range.changed = w.any || (before.root->paras != after.root->paras);
    // 区间夹到 after 的块数：尾部删段时"落点"会落在末尾之外，而调用方要重排的块不存在。
    out.range.lo = std::min(w.lo, off);
    out.range.hi = std::min(w.hi, off);
    return out;
}

}  // namespace heditor::rev
