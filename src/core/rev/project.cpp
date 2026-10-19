#include "core/rev/project.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

#include "core/doc/format_map.h"

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
        const std::vector<Ref>* body = bodyOf(doc);
        if (body != nullptr) {
            for (const Ref& c : *body) walk(c);
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
        // 其余元素一律写成诊断：静默放过比"还没做"更危险（调用方会以为拿到了完整结果）。
        // 容器仍照常下钻，内部文本按流顺序并入当前段。
        out_.diagnostics.push_back(CompileDiagnostic{
            n->kind, n->id,
            std::string("尚未进入投影的元素：") + nodeKindName(n->kind) + " 未投影"});
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
        pb.hash = BlockHash{h.lo(), h.hi()};
        obj_hash_ = Hasher{};

        if (full_) {
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

        objects_.clear();
        runs_.clear();
        out_.blocks.push_back(std::move(pb));
    }

    const StyleSheet* sheet_;
    ViewParams vp_;
    bool full_;
    std::vector<Ref> runs_;              // 当前段里按流顺序攒下的 run
    std::vector<InlineObject> objects_;  // 当前段里攒下的行内对象（控件边界）
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
        // 两边都不含段落标记 ⇒ 这点差异落在某一块的内部，那一段就是它的归属。
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
