#include "core/doc/steps.h"

#include <algorithm>

namespace heditor {

namespace {

int indexInParent(const Node* n) {
    if (n == nullptr || n->parent == nullptr) return -1;
    const std::vector<Node*>& kids = n->parent->children;
    for (size_t i = 0; i < kids.size(); ++i) {
        if (kids[i] == n) return static_cast<int>(i);
    }
    return -1;
}

FlowIndex indexOf(const NodeTree& tree) { return FlowIndex(tree, FlowVisibility::Design); }

bool inRange(FlowPos pos, int size) { return pos >= 0 && pos <= size; }

/**
 * 把一段码点插到流位置 at 上。
 *
 * 标记与紧邻的文本节点相同就并入它：插入不该平白造出一个"同标记的邻居"，那正是规范化要合并
 * 掉的形状；并入也让"插入之后撤销"能精确回到原样。标记不同才新建节点，而落在文本中间时还要
 * 把那个节点切开 —— 否则新内容会整段跑到原有文本的一侧去。
 */
void insertTextAt(NodeTree& tree, const FlowIndex& idx, FlowPos at,
                  const std::vector<uint32_t>& cps, const CharFormat& marks) {
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return;

    if (isText(n->kind)) {
        if (n->marks == marks) {
            std::vector<uint32_t> grown = n->text;
            grown.insert(grown.begin() + p.offset, cps.begin(), cps.end());
            tree.setText(n->id, std::move(grown));
            return;
        }
        const NodeId parent = n->parent->id;
        const int ix = indexInParent(n);
        const bool has_left = p.offset > 0;
        const bool has_right = p.offset < static_cast<int>(n->text.size());
        if (has_left && has_right) {
            const std::vector<uint32_t> right(n->text.begin() + p.offset, n->text.end());
            const std::vector<uint32_t> left(n->text.begin(), n->text.begin() + p.offset);
            const CharFormat kept = n->marks;
            tree.setText(n->id, left);
            const NodeId tail = tree.insertChild(parent, ix + 1, NodeKind::TextRun);
            tree.setText(tail, right);
            tree.find(tail)->marks = kept;
        }
        const NodeId fresh = tree.insertChild(parent, ix + (has_left ? 1 : 0), NodeKind::TextRun);
        tree.setText(fresh, cps);
        tree.find(fresh)->marks = marks;
        return;
    }

    if (isContainer(n->kind)) {
        // 先试前一个孩子（插在末尾那种输入），再试后一个孩子（插在开头那种）。
        if (p.offset > 0) {
            Node* prev = n->children[static_cast<size_t>(p.offset) - 1];
            if (isText(prev->kind) && prev->marks == marks) {
                std::vector<uint32_t> grown = prev->text;
                grown.insert(grown.end(), cps.begin(), cps.end());
                tree.setText(prev->id, std::move(grown));
                return;
            }
        }
        if (p.offset < static_cast<int>(n->children.size())) {
            Node* next = n->children[static_cast<size_t>(p.offset)];
            if (isText(next->kind) && next->marks == marks) {
                std::vector<uint32_t> grown = cps;
                grown.insert(grown.end(), next->text.begin(), next->text.end());
                tree.setText(next->id, std::move(grown));
                return;
            }
        }
        const NodeId fresh = tree.insertChild(p.container, p.offset, NodeKind::TextRun);
        tree.setText(fresh, cps);
        tree.find(fresh)->marks = marks;
        return;
    }

    // 落在段落标记或原子之前：插在它前面。标记与原子只占一个流位置，只有"之前"这一个位置。
    const NodeId parent = n->parent->id;
    const NodeId fresh = tree.insertChild(parent, indexInParent(n), NodeKind::TextRun);
    tree.setText(fresh, cps);
    tree.find(fresh)->marks = marks;
}

/**
 * 把一个段落标记插到流位置 at 上。落在文本中间时先把文本切开，标记插在两半之间。
 */
void insertParaMarkAt(NodeTree& tree, const FlowIndex& idx, FlowPos at, const ParaFormat& para) {
    const Pos p = idx.posAt(at);
    const Node* n = tree.find(p.container);
    if (n == nullptr) return;

    NodeId parent = kInvalidNodeId;
    int ix = 0;
    if (isText(n->kind)) {
        parent = n->parent->id;
        ix = indexInParent(n);
        if (p.offset > 0 && p.offset < static_cast<int>(n->text.size())) {
            const std::vector<uint32_t> right(n->text.begin() + p.offset, n->text.end());
            const std::vector<uint32_t> left(n->text.begin(), n->text.begin() + p.offset);
            const CharFormat kept = n->marks;
            tree.setText(n->id, left);
            const NodeId tail = tree.insertChild(parent, ix + 1, NodeKind::TextRun);
            tree.setText(tail, right);
            tree.find(tail)->marks = kept;
            // 切开之后右半在 ix + 1，标记要插在两半之间。
            ++ix;
        } else if (p.offset > 0) {
            ++ix;
        }
    } else if (isContainer(n->kind)) {
        parent = p.container;
        ix = p.offset;
    } else {
        parent = n->parent->id;
        ix = indexInParent(n);
    }

    const NodeId mark = tree.insertChild(parent, ix, NodeKind::ParagraphMark);
    tree.find(mark)->para = para;
}

/**
 * 删掉流区间 [begin, end)。
 *
 * 覆盖到的是同一层孩子，且都是文本或段落标记（构造删除步骤时已经验过）。两端被部分覆盖的
 * 文本节点截断，中间被完整覆盖的整个删掉。截断先做（它不改孩子下标），删除从后往前做
 * （从后往前删，前面的下标才不会失效）。
 */
bool eraseRange(NodeTree& tree, FlowPos begin, FlowPos end, std::string* error) {
    if (begin >= end) return true;
    std::vector<Cover> covers;
    std::string err;
    if (!collectCovers(tree, Range{begin, end}, &covers, &err)) {
        if (error != nullptr) *error = err;
        return false;
    }

    std::vector<NodeId> fully;
    for (const Cover& c : covers) {
        const Node* n = tree.find(c.node);
        if (n == nullptr) continue;
        const int span = isText(n->kind) ? static_cast<int>(n->text.size()) : 1;
        if (c.from == c.first && c.to == c.first + span) {
            fully.push_back(c.node);
            continue;
        }
        // 部分覆盖只可能是文本：段落标记与原子各占一个流位置，要么整覆盖要么不覆盖。
        const std::vector<uint32_t>& src = n->text;
        std::vector<uint32_t> kept(src.begin(), src.begin() + (c.from - c.first));
        kept.insert(kept.end(), src.begin() + (c.to - c.first), src.end());
        tree.setText(c.node, std::move(kept));
    }

    for (auto it = fully.rbegin(); it != fully.rend(); ++it) {
        const Node* n = tree.find(*it);
        const int ix = indexInParent(n);
        if (ix < 0) continue;
        tree.eraseChild(n->parent->id, ix);
    }
    return true;
}

template <typename... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};
template <typename... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

}  // namespace

DeleteBuild makeDeleteRange(const NodeTree& tree, Range range) {
    DeleteBuild out;
    const ExtractResult ex = extractRange(tree, range);
    if (!ex.ok) {
        out.error = ex.error;
        return out;
    }
    const Range norm = normalizeRange(tree, range);
    DeleteRange s;
    s.begin = norm.begin();
    s.end = norm.end();
    s.removed = ex.fragment;
    out.step = Step{s};
    out.ok = true;
    return out;
}

StepResult canApply(const NodeTree& tree, const Step& step) {
    const FlowIndex idx = indexOf(tree);
    const int size = idx.size();
    StepResult r;
    std::visit(Overloaded{
                   [&](const InsertText& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "插入位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertFragment& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "插入位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const DeleteRange& s) {
                       if (s.begin > s.end || !inRange(s.begin, size) || !inRange(s.end, size)) {
                           r.error = "删除区间越界";
                           return;
                       }
                       std::vector<Cover> covers;
                       std::string err;
                       if (!collectCovers(tree, Range{s.begin, s.end}, &covers, &err)) {
                           r.error = err;
                           return;
                       }
                       int span = 0;
                       for (const Cover& c : covers) span += c.to - c.from;
                       if (span != s.end - s.begin) {
                           r.error = "删除区间与文档对不上（步骤过期了）";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const InsertParaMark& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "分段位置越界";
                           return;
                       }
                       r.ok = true;
                   },
                   [&](const RemoveParaMark& s) {
                       if (!inRange(s.at, size)) {
                           r.error = "取消分段的位置越界";
                           return;
                       }
                       const Pos p = idx.posAt(s.at);
                       const Node* n = tree.find(p.container);
                       if (n == nullptr || !isParagraphMark(n->kind) || p.offset != 0) {
                           r.error = "该位置上没有段落标记";
                           return;
                       }
                       r.ok = true;
                   },
               },
               step);
    return r;
}

StepResult applyStep(NodeTree& tree, const Step& step) {
    StepResult r = canApply(tree, step);
    if (!r.ok) return r;

    const FlowIndex idx = indexOf(tree);
    std::visit(Overloaded{
                   [&](const InsertText& s) { insertTextAt(tree, idx, s.at, s.cps, s.marks); },
                   [&](const InsertFragment& s) {
                       FlowPos pos = s.at;
                       for (const Piece& p : s.content.pieces) {
                           const FlowIndex here = indexOf(tree);
                           if (p.kind == PieceKind::ParaMark) {
                               insertParaMarkAt(tree, here, pos, p.para);
                           } else {
                               insertTextAt(tree, here, pos, p.cps, p.marks);
                           }
                           pos += p.span();
                       }
                   },
                   [&](const DeleteRange& s) {
                       std::string err;
                       eraseRange(tree, s.begin, s.end, &err);
                   },
                   [&](const InsertParaMark& s) { insertParaMarkAt(tree, idx, s.at, s.para); },
                   [&](const RemoveParaMark& s) {
                       const Pos p = idx.posAt(s.at);
                       const Node* n = tree.find(p.container);
                       tree.eraseChild(n->parent->id, indexInParent(n));
                   },
               },
               step);
    return r;
}

Step invertStep(const Step& step) {
    return std::visit(Overloaded{
                          [](const InsertText& s) -> Step {
                              DeleteRange d;
                              d.begin = s.at;
                              d.end = s.at + static_cast<FlowPos>(s.cps.size());
                              Piece piece;
                              piece.kind = PieceKind::Text;
                              piece.cps = s.cps;
                              piece.marks = s.marks;
                              d.removed.pieces.push_back(std::move(piece));
                              return Step{d};
                          },
                          [](const InsertFragment& s) -> Step {
                              DeleteRange d;
                              d.begin = s.at;
                              d.end = s.at + s.content.span();
                              d.removed = s.content;
                              return Step{d};
                          },
                          [](const DeleteRange& s) -> Step {
                              InsertFragment f;
                              f.at = s.begin;
                              f.content = s.removed;
                              return Step{f};
                          },
                          [](const InsertParaMark& s) -> Step {
                              RemoveParaMark x;
                              x.at = s.at;
                              x.para = s.para;
                              return Step{x};
                          },
                          [](const RemoveParaMark& s) -> Step {
                              InsertParaMark x;
                              x.at = s.at;
                              x.para = s.para;
                              return Step{x};
                          },
                      },
                      step);
}

FlowPos mapPos(const Step& step, FlowPos pos, Assoc assoc) {
    return std::visit(Overloaded{
                          [&](const InsertText& s) -> FlowPos {
                              const FlowPos n = static_cast<FlowPos>(s.cps.size());
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const InsertFragment& s) -> FlowPos {
                              const FlowPos n = s.content.span();
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + n;
                              return assoc == Assoc::After ? s.at + n : s.at;
                          },
                          [&](const DeleteRange& s) -> FlowPos {
                              if (pos <= s.begin) return pos;
                              if (pos >= s.end) return pos - (s.end - s.begin);
                              return s.begin;
                          },
                          [&](const InsertParaMark& s) -> FlowPos {
                              if (pos < s.at) return pos;
                              if (pos > s.at) return pos + 1;
                              return assoc == Assoc::After ? s.at + 1 : s.at;
                          },
                          [&](const RemoveParaMark& s) -> FlowPos {
                              if (pos <= s.at) return pos;
                              if (pos >= s.at + 1) return pos - 1;
                              return s.at;
                          },
                      },
                      step);
}

}  // namespace heditor
