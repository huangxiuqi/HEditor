#include "core/compile/project.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "core/compile/atom_map.h"
#include "core/compile/format_map.h"
#include "core/doc/length.h"
#include "core/doc/table_grid.h"

namespace heditor::doc {

namespace {

/**
 * 一次遍历：按流顺序把修订摊成块，同时攒下控件边界与每块的哈希。
 *
 * full 为假时只算哈希（blockHashes 用）：不拼文本、不解析格式 —— 那两样才是投影的大头，
 * 而"哪几块变了"只需要节点哈希。
 */
class Walker {
public:

    Walker(const StyleSheet* sheet, const ViewParams& vp, bool full, std::vector<BlockSpec>* blocks,
           const BlockHistory* prev, const BlockDamage* dmg, int style_version)
        : sheet_(sheet), vp_(vp), full_(full), blocks_(blocks), prev_(prev) {
        style_version_ = style_version;
        if (blocks_ == nullptr) return;
        if (prev_ == nullptr || dmg == nullptr || !dmg->changed) return;
        // 样式表版本不同：块哈希一样、格式却要重新解析 —— 搬过来就是旧格式。
        if (prev_->style_version != style_version_) return;
        // 上一轮的身份账必须与块流同长，否则"第几块"这个坐标无处可落 —— 宁可全量。
        if (prev_->keys.size() != blocks_->size()) return;
        if (dmg->lo < 0 || dmg->hi < dmg->lo || dmg->hi > dmg->after_blocks) return;
        plan_.lo = dmg->lo;
        plan_.hi = dmg->hi;
        plan_.before_blocks = static_cast<int>(blocks_->size());
        plan_.after_blocks = dmg->after_blocks;
        plan_.active = true;
        fresh_.reserve(blocks_->size());
        out_.keys.reserve(blocks_->size());
        out_.extents.reserve(blocks_->size());
    }

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
        out_.style_version = style_version_;
        if (blocks_ == nullptr) {
            out_.blocks = std::move(fresh_);
            out_.keys = std::move(fresh_keys_);
            return std::move(out_);
        }
        if (plan_.active) {
            // 坐标与实际物化的块数对不上 ⇒ 这一轮不能按区间拼。整篇重来一次（不带复用），
            //   内容一定正确，只是慢一回。宁可慢，也不要把块拼到错位置。
            if (static_cast<int>(fresh_.size()) != plan_.hi - plan_.lo) {
                plan_.active = false;
                fresh_.clear();
                fresh_keys_.clear();
                block_index_ = 0;
                flow_ = 0;
                block_start_ = 0;
                prev_end_ = 0;
                out_ = Projection{};
                out_.style_version = style_version_;
                if (doc.root != nullptr) {
                    for (const Ref& c : doc.root->children) walk(c);
                    if (!runs_.empty() || !objects_.empty()) flush(nullptr);
                }
            }
        }
        if (plan_.active) {
            // 原地更新：区间之外的块与身份一个字都不动，只把中段换掉。
            //   旧块流里要删的那一段与 after 的区间长度不同 —— 两边块数差 shift，
            //   所以旧区间的右端是 hi - shift（分段时少删一块、合并时多删一块）。
            const int shift = plan_.after_blocks - plan_.before_blocks;
            const int old_lo = plan_.lo;
            const int old_hi = std::max(old_lo, plan_.hi - shift);
            blocks_->erase(blocks_->begin() + old_lo, blocks_->begin() + old_hi);
            blocks_->insert(blocks_->begin() + old_lo, std::make_move_iterator(fresh_.begin()),
                            std::make_move_iterator(fresh_.end()));
            out_.keys = prev_->keys;
            out_.keys.erase(out_.keys.begin() + old_lo, out_.keys.begin() + old_hi);
            out_.keys.insert(out_.keys.begin() + old_lo, fresh_keys_.begin(), fresh_keys_.end());
        } else {
            *blocks_ = std::move(fresh_);
            out_.keys = std::move(fresh_keys_);
        }
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
        // 序号在这里分配一次：同一偏移上按对象表顺序递增（闭括号先、开括号后），换偏移
        //   从零重数。排版与编辑两侧都读它，不再各自数格子。
        InlineObject item = o;
        if (item.cp != slot_cp_ || slot_next_ < 0) {
            slot_cp_ = item.cp;
            slot_next_ = 0;
        }
        item.slot = slot_next_++;
        objects_.push_back(item);
    }

    /**
     * 记下本块第一个吃进内容的元素 —— 只在"这一块还没有过任何内容"时生效。
     *
     * 同时钉住这一块的起点：内容的第一位在哪儿，块就从哪儿开始。这一步与流游标是同一件事的
     * 两半 —— 游标给出"现在走到第几位"，这里把它记成"这一块的第一位"。
     */
    void noteFirst(NodeId id) {
        if (cur_first_ == kInvalidNodeId && runs_.empty() && objects_.empty()) {
            cur_first_ = id;
            block_start_ = flow_;
        }
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
        ++flow_;  // 进表：容器的一端
        int row_no = 0;
        for (const Ref& row : t->children) {
            if (row->kind != NodeKind::TableRow) continue;
            pushContainer(row);
            ++flow_;  // 进行
            int cell_no = 0;
            for (const Ref& cell : row->children) {
                if (cell->kind != NodeKind::TableCell) continue;
                path_.push_back(CellSlot{static_cast<uint64_t>(t->id), row_no, cell_no});
                flushIfPending();
                ++flow_;  // 进格
                const CellSpec& cs = std::get<CellSpec>(cell->payload);
                LayoutCell lc;
                lc.row = row_no;
                lc.cell = cell_no;
                lc.row_span = cs.row_span;
                lc.col_span = cs.col_span;
                lc.v_align = mapCellVAlign(cs.v_align);
                lc.first_block = block_index_;
                const int ci = static_cast<int>(out_.tables[ti].cells.size());
                out_.tables[ti].cells.push_back(lc);
                pushContainer(cell);
                for (const Ref& c : cell->children) walk(c);
                flushIfPending();
                popContainer();
                ++flow_;  // 出格：格边界是两位，这是其中第二位
                out_.tables[ti].cells[static_cast<size_t>(ci)].block_count =
                    block_index_ - lc.first_block;
                path_.pop_back();
                ++cell_no;
            }
            popContainer();
            ++flow_;  // 出行
            ++row_no;
        }
        popContainer();
        ++flow_;  // 出表
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
            flow_ += static_cast<int>(std::get<RunPayload>(n->payload).text.size());
            runs_.push_back(n);
            return;
        }
        if (n->kind == NodeKind::ParagraphMark) {
            flush(n);
            ++flow_;  // 标记自己占一位，它是块与块之间那道界
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
            // 浮动图仍然进对象表（流位置要能与树对齐），几何在落段时另记一份 floats。
            o.floating = (is_img && img.anchor == ImageAnchor::Float);
            AtomGeometry g;
            g.is_image = is_img;
            g.width = is_img ? img.width : fml.width;
            g.height = is_img ? img.height : fml.height;
            g.px_w = img.px_w;
            g.px_h = img.px_h;
            g.dpi = img.dpi;
            g.expr_cp = is_img ? 0 : utf8ToCodepoints(fml.expr).size();
            g.wrap = is_img ? img.wrap : ImageWrap::None;
            g.dx_pt = is_img ? img.dx_pt : 0.0f;
            g.dy_pt = is_img ? img.dy_pt : 0.0f;
            emitObject(o, n);
            atoms_.emplace_back(objects_.size() - 1, g);
            ++flow_;  // 原子占一位
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
            // 进控件时就占一个槽：跨度要等孩子走完才知道末尾，但顺序必须按进入的先后定 ——
            //   离开时才 push 的话，嵌套时内层会排到外层前面，而对外下标是"第几个字段"，
            //   它得是文档顺序（与对象表里 ControlStart 的出现顺序同一套）。槽里存下标而不是
            //   引用：孩子里还有控件，push 会让 vector 搬家。
            const size_t slot = out_.controls.size();
            out_.controls.push_back(ControlSpan{});
            out_.controls[slot].node = n;
            out_.controls[slot].block = block_index_;
            out_.controls[slot].cp = cpCount();
            noteFirst(n->id);
            // 内容指纹带外观与值（与 compile 同一个函数）：勾选状态变化时字形不动，
            //   行指纹得靠它才判得出"这一行要重画"。
            const uint64_t look = controlPaintDigest(spec);
            // 空控件的提示文字要在孩子走完之后才判得出来（"空"= 一个码点都没吃进来），
            //   所以先记下自己在对象表里的位置，回来再补。
            const size_t start_slot = objects_.size();
            const int cp_lo = cpCount();
            emitObject(
                InlineObject{static_cast<uint32_t>(cpCount()), InlineObjectKind::ControlStart,
                             owner, spec.readonly, 0.0f, 0.0f, InlineAtom::None, look},
                n);
            ++flow_;  // 进控件：这一位就是上面那枚起始边界
            for (const Ref& c : n->children) walk(c);
            // 提示是哪一句由控件类型定（下拉 / 日期有内置的，文本 / 数值只认 placeholder）。
            const std::string hint = controlEmptyHint(spec);
            if (cpCount() == cp_lo && start_slot < objects_.size() &&
                objects_.size() == start_slot + 1 && objects_[start_slot].owner == owner &&
                !hint.empty()) {
                objects_[start_slot].hint = hint;
            }
            out_.controls[slot].end_block = block_index_;
            out_.controls[slot].end_cp = cpCount();
            noteFirst(n->id);
            emitObject(InlineObject{static_cast<uint32_t>(cpCount()), InlineObjectKind::ControlEnd,
                                    owner, spec.readonly, 0.0f, 0.0f, InlineAtom::None, look},
                       n);
            ++flow_;  // 出控件：结束边界
            return;
        }
        // 结构外壳：文档、节、页眉页脚只提供层级，边界不进内容，下去即可（与编译同一口径）。
        //   但节与页眉页脚在流里仍是容器 —— 它们的两端各占一位，与 FlowIndex 同一口径。
        if (n->kind == NodeKind::Section || n->kind == NodeKind::Document ||
            n->kind == NodeKind::HeaderFooter) {
            const bool boundary = (n->kind != NodeKind::Document);
            if (boundary) ++flow_;
            for (const Ref& c : n->children) walk(c);
            if (boundary) ++flow_;
            return;
        }
        // 其余元素一律写成诊断：静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。
        // 容器仍照常下钻，内部文本按流顺序并入当前段。
        out_.diagnostics.push_back(CompileDiagnostic{
            n->kind, n->id, std::string("尚未进入排版输入的元素：") + nodeKindName(n->kind)});
        const bool boundary = isContainer(n->kind);
        if (boundary) ++flow_;
        for (const Ref& c : n->children) walk(c);
        if (boundary) ++flow_;
    }

    /**
     * 试着从上一轮搬这一块。返回 true 表示已搬进产物。
     *
     * 坐标只用来定位，不当作正确性依据：区间给的是"上一轮第几块与之对应"，
     *   搬之前还要比内容哈希与来源身份。坐标报偏了最多是少搬一块（这一块重新物化），
     *   不会把旧内容顶上去 —— 那种失效会静默地让画面停在旧内容上，而内容判据全绿。
     *
     * 区间之内一律不复用：那里本来就是"要重算"的那一段。
     */
    bool canReuse(const BlockHash& hash, NodeId mark, NodeId first) {
        if (!plan_.active || blocks_ == nullptr || prev_ == nullptr) return false;
        const bool in_prefix = block_index_ < plan_.lo;
        if (!in_prefix && block_index_ < plan_.hi) return false;
        const int shift = plan_.after_blocks - plan_.before_blocks;
        const int src = in_prefix ? block_index_ : block_index_ - shift;
        if (src < 0 || src >= static_cast<int>(blocks_->size())) return false;
        const BlockKey& k = prev_->keys[static_cast<size_t>(src)];
        // 两半都要过：内容没变（哈希）、身份没换（同一枚标记与同一个首个内容节点）。
        //   只比哈希会把"内容相同的另一段"搬过来，于是来源身份错位、光标落偏一个字。
        if (!(k.hash == hash)) return false;
        if (k.source.mark != mark || k.source.first != first) return false;
        // 命中之后什么都不用做：块与身份就留在调用方的块流里，遍历结束只替换中段。
        ++out_.reused;
        return true;
    }

    /**
     * 落一段。mark 为空表示这一段没有结束标记（末尾未结束），用默认段落格式。
     */
    void flush(const Ref& mark) {
        // 身份与哈希先用局部变量算，块本身等到"确定要物化"时才构造 —— 复用路径上
        //   构造一个 BlockSpec 再丢掉，等于把省下来的那一份又花回去一半。
        const bool had_content = (cur_first_ != kInvalidNodeId);
        const NodeId mark_id = (mark != nullptr) ? mark->id : kInvalidNodeId;
        const NodeId first_id = had_content ? cur_first_ : mark_id;
        cur_first_ = kInvalidNodeId;

        // 流的两个局部量：本块占几位、它前面那道边界占几位。绝对位置由调用方累加，这里
        //   一个绝对数都不写 —— 写了就得在上游变长之后平移，那是另一套需要维护的账。
        //   空段（只有标记、没有内容）的起点就是自己的标记，与块流起点的老口径一致。
        if (!had_content) block_start_ = flow_;
        BlockExtent extent;
        extent.span = flow_ - block_start_;
        extent.gap = block_start_ - prev_end_;
        out_.extents.push_back(extent);
        // 上一块内容的末尾就是标记那一位（标记属于两块之间的界，不属于任何一块），
        //   所以这里记 flow_ 而不是 flow_+1 —— 加了 1 就等于把标记吞进下一块的 gap 起点，
        //   下一块的 gap 会变成 0。
        prev_end_ = flow_;
        block_start_ = flow_ + (mark != nullptr ? 1 : 0);

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
        const BlockHash hash{h.lo(), h.hi()};
        obj_hash_ = Hasher{};

        // 增量复用：坐标给出"与上一轮哪一块对应"，哈希与来源身份确认"它真的没变"。
        //   分节强制分页这一位不在块哈希里，所以那一块不复用。
        // 含行内原子（图片、公式）或浮动图的块不复用：对象表里存着原子的节点身份，
        //   而块哈希只哈希内容 —— "删掉一张图再在同一处插一张"文本与图片内容都没变，
        //   复用就会把对象表里那个已经删掉的节点号一直带下去（行盒里的图片簇指向幽灵）。
        //   浮动图还有第二个理由：它的纵向位置取决于前面排了多高，复用会把它整条漏掉。
        bool has_atom = false;
        for (const InlineObject& o : objects_) {
            if (o.kind == InlineObjectKind::Atom) {
                has_atom = true;
                break;
            }
        }
        if (!pending_section_break_ && !has_atom && canReuse(hash, mark_id, first_id)) {
            objects_.clear();
            atoms_.clear();
            runs_.clear();
            ++block_index_;
            return;
        }

        ProjectedBlock pb;
        pb.source.mark = mark_id;
        pb.source.first = first_id;
        pb.hash = hash;
        // 要不要物化这一块：段落格式、字符格式、原子盒子都要查三层样式，样式表缺席就什么都
        //   查不了。只算哈希的那一路（blockHashes）传空指针，走到这里只留哈希与来源身份 ——
        //   哈希在上面已经算完，那一趟本来也不需要块的内容。
        const bool materialize = (sheet_ != nullptr) && (full_ || has_atom);

        // 段落格式先落：浮动图要不要成立取决于这一段是不是横排正文（见下面那一支）。
        if (materialize && mark != nullptr) {
            const MarkPayload& mp = std::get<MarkPayload>(mark->payload);
            applyParaFormatToBlock(
                *sheet_, mp.style, mp.para, mark->id, &pb.spec,
                [this](const CompileDiagnostic& d) { out_.diagnostics.push_back(d); });
        }

        if (materialize) {
            const bool float_ok = !pb.spec.wm.isVertical() && path_.empty();
            // 原子的盒子在这里定：公式的默认框按段落字号估，而字号是段落属性 —— 走到原子
            // 那一刻还看不到它。emit 那一刻只记下"这里有一枚原子"。
            for (const std::pair<std::size_t, AtomGeometry>& a : atoms_) {
                InlineObject& obj = objects_[a.first];
                if (!full_ && !obj.floating) continue;
                const AtomBox box = resolveAtomBox(a.second, pb.spec.size_pt);
                if (box.width_relative) {
                    note(NodeKind::Image, obj.owner, atomRelativeDiag(true));
                }
                if (box.height_relative) {
                    note(NodeKind::Image, obj.owner, atomRelativeDiag(false));
                }
                if (!obj.floating || !float_ok) {
                    // 行内（含竖排与表格格里退回行内的浮动图）：宽度照常占版面。
                    obj.floating = false;
                    obj.w_pt = box.w_pt;
                    obj.h_pt = box.h_pt;
                    continue;
                }
                // 浮动：版面上一寸不占，几何另记一份 —— 排版层按 floats 排。
                obj.w_pt = 0.0f;
                obj.h_pt = 0.0f;
                LayoutFloat lf;
                lf.handle = static_cast<uint64_t>(obj.owner);
                lf.para = static_cast<int>(block_index_);
                lf.cp = obj.cp;
                lf.w_pt = box.w_pt;
                lf.h_pt = box.h_pt;
                lf.wrap = a.second.wrap;
                lf.dx_pt = a.second.dx_pt;
                lf.dy_pt = a.second.dy_pt;
                lf.content_hash = obj.content_hash;
                out_.floats.push_back(lf);
            }
            pb.spec.objects = std::move(objects_);
            if (mark != nullptr) {
                const MarkPayload& mp = std::get<MarkPayload>(mark->payload);
                // 空段落的字符格式只挂在段落标记上（理由见 compile.cpp 同一处）。
                if (pb.spec.cp_text.empty()) {
                    const CharFormat& m = mp.marks;
                    if (m.font_index.has_value()) pb.spec.font_index = *m.font_index;
                    if (m.size.has_value() && m.size->kind == LengthKind::Abs) {
                        pb.spec.size_pt = static_cast<float>(emuToPt(m.size->abs_emu));
                    }
                    if (m.color.has_value()) pb.spec.color = *m.color;
                    if (m.bold.has_value()) {
                        pb.spec.weight = *m.bold ? kWeightBold : kWeightRegular;
                    }
                    if (m.italic.has_value()) pb.spec.italic = *m.italic;
                    if (m.underline.has_value()) pb.spec.underline = *m.underline;
                }
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
                if (pb.spec.cp_text.empty()) {
                    pb.spec.cp_text = doc::Text::slice(rp.text, 0, rp.text.size());
                } else {
                    pb.spec.cp_text.insert(pb.spec.cp_text.end(), rp.text.begin(), rp.text.end());
                }
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
        slot_next_ = -1;  // 新的一段从零开始数
        fresh_.push_back(std::move(pb.spec));
        fresh_keys_.push_back(BlockKey{pb.hash, pb.source});
        ++block_index_;
    }

    const StyleSheet* sheet_;
    ViewParams vp_;
    bool full_;
    std::vector<Ref> runs_;              // 当前段里按流顺序攒下的 run
    std::vector<InlineObject> objects_;  // 当前段里攒下的行内对象（控件边界与原子）
    /**
     * 上一次分配序号时的码点偏移，与这个偏移上已分配出去的枚数（-1 = 本段还没有过对象）。
     */
    uint32_t slot_cp_ = 0;
    int slot_next_ = -1;
    /**
     * 当前段里待定几何的原子：对象下标 + 它需要的输入。落段时才定得下来（要块级字号）。
     */
    std::vector<std::pair<std::size_t, AtomGeometry>> atoms_;
    int sections_seen_ = 0;                                  // 走过几节（第一节的分页行为无意义）
    bool pending_section_break_ = false;                     // 分节强制分页还没落到段上
    std::vector<CellSlot> path_;                             // 当前所在的格（空 = 正文）
    std::vector<std::pair<uint64_t, uint64_t>> containers_;  // 外层容器的自身哈希
    NodeId cur_first_ = kInvalidNodeId;
    /**
     * 流游标：投影自己数的"现在走到第几位"，与 FlowIndex::measure 同一套展开规则 ——
     *   容器两端各一位、文本按码点、原子与标记各一位、根不占位、呈现态下隐藏子树整棵不占。
     *
     * 位置在这里产生，而不是事后再拿节点去流索引里查一遍：走的是同一次遍历，两种说法
     *   从源头上就没有分叉的机会。
     */
    int flow_ = 0;
    /**
     * 当前这一块内容的第一位。noteFirst 钉住；空段在 flush 里钉成它自己那枚标记。
     */
    int block_start_ = 0;
    /**
     * 上一块内容末尾那一位（它的标记之前）。两块之差就是中间那道界占了几位。
     */
    int prev_end_ = 0;
    Hasher obj_hash_;
    Projection out_;

    /**
     * 调用方持有的块流：既是被复用的上一轮，也是这一轮的输出。
     */
    std::vector<BlockSpec>* blocks_ = nullptr;
    /**
     * 上一轮的身份账与样式表版本。空表示全量投影。
     */
    const BlockHistory* prev_ = nullptr;
    /**
     * 这一轮物化出来的块（只有损伤区间那一段）。
     */
    std::vector<BlockSpec> fresh_;
    std::vector<BlockKey> fresh_keys_;
    int style_version_ = 0;
    /**
     * 损伤区间与两边的块数。active 为假时整篇物化。
     */
    struct Plan {
        int lo = 0;
        int hi = 0;
        int before_blocks = 0;
        int after_blocks = 0;
        bool active = false;
    } plan_;
    /**
     * 正在攒第几块（按产物顺序）。复用与否只由它落在 lo/hi 的哪一侧决定。
     */
    int block_index_ = 0;
};

void fillSlots(Projection* out, const std::vector<BlockSpec>& blocks) {
    for (int b = 0; b < static_cast<int>(blocks.size()); ++b) {
        for (const InlineObject& o : blocks[static_cast<size_t>(b)].objects) {
            out->slots.push_back(Slot{b, o.cp, o.kind, o.owner, o.slot});
        }
    }
}

}  // namespace

Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params,
                   std::vector<BlockSpec>* blocks, const BlockHistory* prev,
                   const BlockDamage* dmg) {
    const int v = sheet.version();
    Projection out = Walker(&sheet, params, true, blocks, prev, dmg, v).run(doc);
    fillSlots(&out, blocks != nullptr ? *blocks : out.blocks);
    return out;
}

Projection project(const Doc& doc, const StyleSheet& sheet, const ViewParams& params,
                   std::vector<BlockSpec>* blocks) {
    const int v = sheet.version();
    Projection out = Walker(&sheet, params, true, blocks, nullptr, nullptr, v).run(doc);
    fillSlots(&out, blocks != nullptr ? *blocks : out.blocks);
    return out;
}

std::vector<BlockHash> blockHashes(const Doc& doc, FlowVisibility vis) {
    ViewParams vp;
    vp.vis = vis;
    const Projection p = Walker(nullptr, vp, false, nullptr, nullptr, nullptr, 0).run(doc);
    std::vector<BlockHash> out;
    out.reserve(p.keys.size());
    for (const BlockKey& k : p.keys) out.push_back(k.hash);
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
    BlockDamage out{p, m - s, changed};
    out.after_blocks = m;
    return out;
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

    explicit DamageWalker(FlowVisibility vis) : vis_(vis) {}

    /**
     * 这棵子树在给定可见性下产生几块。paras 与 paras_display 是两个数，按 vis 取。
     */
    int vp(const Ref& n) const {
        return (vis_ == FlowVisibility::Display) ? n->paras_display : n->paras;
    }

    void walk(const Ref& a, const Ref& b, int& off) {
        ++visited;
        // 两边都不可见：里面的内容怎么变都不产生块。少了这一条，哈希下降会走进隐藏子树，
        //   把它内部的变化记成"某一块的损伤" —— 而那一块在呈现态根本不存在。
        if (vis_ == FlowVisibility::Display && a->hidden && b->hidden) return;
        if (a == b || (a->hash_lo == b->hash_lo && a->hash_hi == b->hash_hi)) {
            off += vp(b);
            return;
        }
        // 容器自己的记录变了（列宽、行高、跨度、控件外观）：它覆盖的块整段算损伤。
        // 这一条不能省 —— 改列宽时每一块的内容哈希都没变，只比内容会报"没有损伤"，
        // 而整张表的行高与分页都可能跟着变。
        if (selfDiffers(a, b)) {
            mark(off, vp(b) > 0 ? vp(b) : 1);
            off += vp(b);
            return;
        }
        // 自身没变、两边都不含段落标记 ⇒ 这点差异落在某一块的内部，那一段就是它的归属。
        if (vp(a) == 0 && vp(b) == 0) {
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

    FlowVisibility vis_ = FlowVisibility::Design;

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
                mark(off, vp(y[j]) > 0 ? vp(y[j]) : 1);
                off += vp(y[j]);
                ++j;
                continue;
            }
            if (j >= y.size()) {
                // 删除：它占的块没了，位置在当前 off（后面的块会往前挪一格）。
                mark(off, vp(x[i]) > 0 ? vp(x[i]) : 1);
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
                mark(off, vp(x[i]) > 0 ? vp(x[i]) : 1);
                ++i;
                continue;
            }
            if (in_x.count(y[j]->id) == 0) {
                // 插入。插进来的若是段落标记，它结束的就是"当前这一块" —— 而当前块的
                //   起点可能在 off 之前（walk 对内容叶子只标块、不结算 off），所以这里
                //   连当前块一起标两格。多报一块只会多物化一块，漏报会让新段沿用旧内容。
                const int span = (vp(y[j]) > 0) ? vp(y[j]) : 1;
                mark(off, y[j]->kind == NodeKind::ParagraphMark ? span + 1 : span);
                off += vp(y[j]);
                ++j;
                continue;
            }
            mark(off, vp(x[i]) > 0 ? vp(x[i]) : 1);
            ++i;
        }
    }
};

/**
 * 给定可见性下，文档流的最后一个叶子。呈现态跳过隐藏子树 —— 一棵隐藏的子树即使长在末尾，
 * 它的内容也不在流里，末尾由它前面那个可见元素决定。
 *
 * 返回空表示"这棵子树在流里没有任何可见叶子"（整棵被隐藏，或只剩空容器）。
 */
const Node* lastVisibleLeaf(const Ref& n, bool display) {
    for (size_t i = n->children.size(); i > 0; --i) {
        const Ref& child = n->children[i - 1];
        if (display && child->hidden) continue;
        if (const Node* r = lastVisibleLeaf(child, display)) return r;
    }
    return n->children.empty() ? n.get() : nullptr;
}

/**
 * 文档流的最后一个叶子是不是"内容"。是的话，末尾就有一段没有结束标记 —— 投影会给它补
 * 一块，而 paras 不计这一块。叶子是段落标记、或者整棵树只有空容器时，都不补。
 */
bool endsOpen(const Ref& n, FlowVisibility vis) {
    const Node* leaf = lastVisibleLeaf(n, vis == FlowVisibility::Display);
    if (leaf == nullptr) return false;
    const NodeKind k = leaf->kind;
    return k == NodeKind::TextRun || k == NodeKind::Image || k == NodeKind::Formula ||
           k == NodeKind::ContentControl;
}

}  // namespace

Damage damage(const Doc& before, const Doc& after, FlowVisibility vis) {
    Damage out;
    if (!before.root || !after.root) {
        out.range.changed = (before.root != after.root);
        return out;
    }
    DamageWalker w(vis);
    int off = 0;
    w.walk(before.root, after.root, off);
    out.visited = w.visited;
    out.scanned = w.scanned;
    out.range.after_blocks = off;
    // 末尾那一段没有结束标记时，投影在遍历结束后补一块，而 paras 不计它。坐标要跟着补，
    //   否则按区间搬块会把最后一块搬到它前面去（判据 [22] 的"末尾开放"形状守着）。
    const bool open = endsOpen(after.root, vis);
    if (open) ++out.range.after_blocks;
    const int before_blocks =
        (vis == FlowVisibility::Display) ? before.root->paras_display : before.root->paras;
    const int after_blocks =
        (vis == FlowVisibility::Display) ? after.root->paras_display : after.root->paras;
    out.range.changed =
        w.any || (before_blocks != after_blocks) || (endsOpen(before.root, vis) != open);
    // 区间夹到 after 的块数：尾部删段时"落点"会落在末尾之外，而调用方要重排的块不存在。
    out.range.lo = std::min(w.lo, out.range.after_blocks);
    out.range.hi = std::min(w.hi + ((open && w.any) ? 1 : 0), out.range.after_blocks);
    return out;
}

}  // namespace heditor::doc
