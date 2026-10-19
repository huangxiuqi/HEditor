#include "core/rev/rev.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>

namespace heditor::rev {

namespace {

void hashLength(Hasher& h, const Length& v) {
    h.u8(static_cast<uint8_t>(v.kind));
    h.i64(static_cast<int64_t>(v.abs_emu));
    h.i64(v.num);
    h.i64(v.den);
}

void hashColor(Hasher& h, const Color& c) {
    h.u8(c.r);
    h.u8(c.g);
    h.u8(c.b);
    h.u8(c.a);
}

void hashCharFormat(Hasher& h, const CharFormat& f) {
    h.u64(f.bold.has_value() ? 1 : 0);
    if (f.bold) h.u64(*f.bold ? 1 : 0);
    h.u64(f.italic.has_value() ? 1 : 0);
    if (f.italic) h.u64(*f.italic ? 1 : 0);
    h.u64(f.underline.has_value() ? 1 : 0);
    if (f.underline) h.u64(*f.underline ? 1 : 0);
    h.u64(f.font_index.has_value() ? 1 : 0);
    if (f.font_index) h.i64(*f.font_index);
    h.u64(f.size.has_value() ? 1 : 0);
    if (f.size) hashLength(h, *f.size);
    h.u64(f.color.has_value() ? 1 : 0);
    if (f.color) hashColor(h, *f.color);
}

void hashParaPatch(Hasher& h, const ParaFormatPatch& p) {
    h.u64(p.font_index.has_value() ? 1 : 0);
    if (p.font_index) h.i64(*p.font_index);
    h.u64(p.size.has_value() ? 1 : 0);
    if (p.size) hashLength(h, *p.size);
    h.u64(p.wm.has_value() ? 1 : 0);
    if (p.wm) {
        h.u8(static_cast<uint8_t>(p.wm->inline_axis));
        h.u8(static_cast<uint8_t>(p.wm->block_flow));
        h.u8(static_cast<uint8_t>(p.wm->orientation));
    }
    h.u64(p.line_factor.has_value() ? 1 : 0);
    if (p.line_factor) h.f32(*p.line_factor);
    h.u64(p.space_after.has_value() ? 1 : 0);
    if (p.space_after) hashLength(h, *p.space_after);
    h.u64(p.page_break_before.has_value() ? 1 : 0);
    if (p.page_break_before) h.u64(*p.page_break_before ? 1 : 0);
    h.u64(p.language.has_value() ? 1 : 0);
    // 语言是驻留字符串的裸指针，哈希必须按内容算：同一个标签在不同进程里的地址不一样。
    if (p.language && *p.language) h.str(std::string(*p.language));
    h.u64(p.color.has_value() ? 1 : 0);
    if (p.color) hashColor(h, *p.color);
    h.u64(p.fallback_font_indices.has_value() ? 1 : 0);
    if (p.fallback_font_indices) {
        h.u64(p.fallback_font_indices->size());
        for (int idx : *p.fallback_font_indices) h.i64(idx);
    }
}

void hashBorderSide(Hasher& h, const BorderSide& s) {
    h.u64(s.on ? 1 : 0);
    h.u8(static_cast<uint8_t>(s.style));
    hashLength(h, s.width);
    hashColor(h, s.color);
}

void hashAppearance(Hasher& h, const Appearance& a) {
    hashLength(h, a.width);
    h.u8(static_cast<uint8_t>(a.overflow));
    h.u8(static_cast<uint8_t>(a.underline));
    hashBorderSide(h, a.border.left);
    hashBorderSide(h, a.border.right);
    hashBorderSide(h, a.border.top);
    hashBorderSide(h, a.border.bottom);
    h.str(a.glyph.kind);
    hashLength(h, a.glyph.width);
    hashLength(h, a.glyph.size);
    hashColor(h, a.glyph.color);
    hashColor(h, a.glyph.fill);
    h.u8(static_cast<uint8_t>(a.align));
    h.str(a.placeholder);
    h.str(a.styleRef);
    h.str(a.invalidStyleRef);
}

/**
 * 表格三类记录的哈希：表格自己是列定义与列宽，行是三个开关与行高，格是两个跨度与对齐。
 *
 * 这些是容器几何：它们不改任何一块的文字，却决定块排成什么样。
 * 投影把外层的这几份哈希并进每个块的哈希，损伤定位按"自身哈希变了就整段算损伤"处理 ——
 * 两处合起来才等于"容器几何变了，它下面所有块都要重排"。
 */
void hashTableColumn(Hasher& h, const TableColumn& c) {
    h.str(c.field_ref);
    h.str(c.title);
    h.str(c.data_type);
    h.str(c.editor_style);
    hashLength(h, c.width);
    h.u8(static_cast<uint8_t>(c.align));
}

void hashTableSpec(Hasher& h, const TableSpec& t) {
    h.u64(t.columns.size());
    for (const TableColumn& c : t.columns) hashTableColumn(h, c);
    h.u64(t.grid_cols.size());
    for (const Length& l : t.grid_cols) hashLength(h, l);
    hashLength(h, t.width);
    h.u8(static_cast<uint8_t>(t.align));
}

void hashRowSpec(Hasher& h, const RowSpec& r) {
    h.u8(r.can_split_by_page ? 1 : 0);
    h.u8(r.is_header ? 1 : 0);
    h.u8(r.page_break_before ? 1 : 0);
    hashLength(h, r.height);
    h.u8(static_cast<uint8_t>(r.height_rule));
}

void hashCellSpec(Hasher& h, const CellSpec& c) {
    h.i64(c.row_span);
    h.i64(c.col_span);
    h.u8(static_cast<uint8_t>(c.v_align));
}

/**
 * 图片、公式与域记录的哈希。
 *
 * 域的 cache 不在里面：它是求值结果，不是作者内容。把它算进内容哈希，会让"同一份文档求值
 * 前后"哈希不同 —— 撤销回到旧修订时对不上，而内容明明没变。它照样落盘（重新打开时先显示
 * 已有的值），往返判据对域另做一份逐字段比对。
 */
void hashImageSpec(Hasher& h, const ImageSpec& im) {
    h.str(im.resource_id);
    h.i64(im.px_w);
    h.i64(im.px_h);
    h.i64(im.dpi);
    hashLength(h, im.width);
    hashLength(h, im.height);
    h.u8(static_cast<uint8_t>(im.anchor));
    h.u8(static_cast<uint8_t>(im.wrap));
}

void hashFormulaSpec(Hasher& h, const FormulaSpec& f) {
    h.str(f.expr);
    h.u8(static_cast<uint8_t>(f.backend));
    hashLength(h, f.width);
    hashLength(h, f.height);
}

void hashFieldSpec(Hasher& h, const FieldSpec& f) {
    h.u8(static_cast<uint8_t>(f.kind));
    h.str(f.name);
}

/**
 * 控件记录的哈希。
 *
 * ControlSpec 的相等是默认生成的（加字段自动比），哈希不会自动跟着变 —— 漏一个字段的表现是
 * "改了这个属性，缓存照样命中"，画面上什么都不变。所以 rev_test 里逐字段构造只差一个字段的
 * 两份规格并断言哈希不同，那份清单要与这里的字段同步。
 */
void hashControlSpec(Hasher& h, const ControlSpec& c) {
    h.str(c.key);
    h.str(c.controlType);
    h.u8(static_cast<uint8_t>(c.valueShape));
    h.u64(c.value.flag ? 1 : 0);
    h.str(c.value.text);
    h.u64(c.value.items.size());
    for (const std::string& it : c.value.items) h.str(it);
    h.u64(c.readonly ? 1 : 0);
    h.u64(c.required ? 1 : 0);
    h.u64(c.structureLock ? 1 : 0);
    hashAppearance(h, c.appearance);
    h.str(c.validateRef);
}

/**
 * 节点的完整哈希：自身哈希加孩子哈希。身份不进哈希（见文件头的说明）。
 */
void hashNode(Hasher& h, const Node& n) {
    hashSelf(h, n);
    h.u64(n.children.size());
    for (const Ref& c : n.children) {
        h.u64(c->hash_lo);
        h.u64(c->hash_hi);
    }
}

Ref withChildren(const Ref& node, std::vector<Ref> children) {
    return makeNode(node->id, node->kind, node->payload, std::move(children), node->hidden);
}

/**
 * 换掉一棵子树里的一个节点，沿路径复制。没找到时原样返回同一个指针 —— 结构共享要真的共享，
 * 否则"改一段"就退化成"复制全篇"。
 */
Ref replaceNode(const Ref& node, NodeId target, const Ref& replacement, bool* found) {
    if (node->id == target) {
        *found = true;
        return replacement;
    }
    bool any = false;
    std::vector<Ref> kids;
    kids.reserve(node->children.size());
    for (const Ref& c : node->children) {
        bool hit = false;
        Ref nc = replaceNode(c, target, replacement, &hit);
        any = any || hit;
        kids.push_back(std::move(nc));
    }
    if (!any) {
        *found = false;
        return node;
    }
    *found = true;
    return withChildren(node, std::move(kids));
}

EditResult fail(std::string why) {
    EditResult r;
    r.ok = false;
    r.error = std::move(why);
    return r;
}

/**
 * 一个节点在流里贡献多少个可寻址码点。容器按内容累加，段落标记与控件边界都不占码点。
 */
int flowLen(const Ref& n) {
    if (n->kind == NodeKind::TextRun) {
        return static_cast<int>(std::get<RunPayload>(n->payload).text.size());
    }
    if (n->kind == NodeKind::ParagraphMark) return 0;
    int sum = 0;
    for (const Ref& c : n->children) sum += flowLen(c);
    return sum;
}

/**
 * 块级容器：它的内容自成段落序列，不从属于外面那一段。
 *
 * 内容控件不是（它是行内的，文字与边界都落在当前段里）；表格与它的行、格是 —— 格里的段落
 * 与表外的段落是两段，中间没有"同一段"这回事。
 */
bool isBlockContainer(NodeKind kind) {
    return kind == NodeKind::Table || kind == NodeKind::TableRow || kind == NodeKind::TableCell;
}

/**
 * 段落内容在流里的落点。
 *
 * InRun 落在某个 run 内部；BeforeChild 落在两项之间（段首、控件之前、段末标记之前）。
 * 只有这两种：容器内部不是另一种落点，只是 path 更长 —— 见 Site。
 */
enum class SiteKind : uint8_t { InRun, BeforeChild };

/**
 * (段号, 段内码点) 落在哪儿。
 *
 * 流顺序按深度优先：容器内部的文字就是那一段的文字，内部的段落标记一样切段。位置可能有两种
 * 落法（上一段末尾与下一段开头是同一处边界），这里取"靠前的那一个"—— 两种落法对文本结果
 * 等价，但必须定死一种，否则同一份编辑会产出两棵不同的树，哈希也就对不上了。
 *
 * path 既是递归栈也是结果：从正文那一节往下走到"落点所在的那一层孩子表"的下标链
 * （空 = 就是节的直接孩子）。成功时栈里留着整条链，失败时逐层弹干净。
 * 有了它，容器内部与顶层就是同一种落点：改的永远是某一层孩子表里的某处，改动再沿路径换回去。
 * 没有"容器内另行处理"的分支 —— 那种分支要把位置语义写两遍，两份迟早对不上。
 */
struct Site {
    SiteKind kind = SiteKind::BeforeChild;
    int child = -1;
    int off = 0;
};

bool locate(const std::vector<Ref>& kids, int para, int cp, std::vector<int>* path, Site* out) {
    int p = 0;
    int off = 0;
    for (int i = 0; i < static_cast<int>(kids.size()); ++i) {
        const Ref& n = kids[i];
        if (n->kind == NodeKind::ParagraphMark) {
            if (p == para) {
                // 段落已经结束而还没找到落点：位置只能是段末（空段也走这里）。
                if (cp != off) return false;
                out->kind = SiteKind::BeforeChild;
                out->child = i;
                out->off = 0;
                return true;
            }
            ++p;
            off = 0;
            continue;
        }
        if (n->kind == NodeKind::TextRun) {
            const int len = static_cast<int>(std::get<RunPayload>(n->payload).text.size());
            if (p == para && cp >= off && cp <= off + len) {
                out->kind = SiteKind::InRun;
                out->child = i;
                out->off = cp - off;
                return true;
            }
            off += len;
            continue;
        }
        // 块级容器（表格与它的行、格）自成段落边界：它们不从属于当前段，所以没有"在该段 cp 处
        // 进入它"这种落点 —— 只有"这一段在它里面"，此时段号减掉容器的起点，码点本来就是段内
        // 坐标。少了这一支，格里的段落会按"上一格累积了多少码点"去算偏移，永远错位。
        if (isBlockContainer(n->kind)) {
            const int marks = n->paras;
            if (p <= para && para < p + marks) {
                path->push_back(i);
                const bool found = locate(n->children, para - p, cp, path, out);
                if (!found) path->pop_back();
                return found;
            }
            p += marks;
            off = 0;
            continue;
        }
        const int len = flowLen(n);
        const int marks = n->paras;
        if (p == para && cp == off) {
            // 容器之前：cp 正好在这一项之前。这一段里已经有内容时它仍然是合法的落点。
            out->kind = SiteKind::BeforeChild;
            out->child = i;
            out->off = 0;
            return true;
        }
        if (p < para && para < p + marks) {
            // 段落落在容器内部：段号减掉容器的起点，码点本来就是段内坐标。
            path->push_back(i);
            const bool found = locate(n->children, para - p, cp, path, out);
            if (!found) path->pop_back();
            return found;
        }
        if (p == para && cp > off && cp <= off + len) {
            // 段号相同、码点已经进了容器的流：容器从段落中间开始，内容是这一段的延续。
            path->push_back(i);
            const bool found = locate(n->children, 0, cp - off, path, out);
            if (!found) path->pop_back();
            return found;
        }
        off += len;
        p += marks;
    }
    return false;
}

/**
 * 第 para 段的结束标记落在哪一层：path 指到那一层，*at 是它在该层孩子表里的下标。
 */
bool markOf(const std::vector<Ref>& kids, int para, std::vector<int>* path, int* at) {
    int p = 0;
    for (int i = 0; i < static_cast<int>(kids.size()); ++i) {
        const Ref& n = kids[i];
        if (n->kind == NodeKind::ParagraphMark) {
            if (p == para) {
                *at = i;
                return true;
            }
            ++p;
            continue;
        }
        const int marks = n->paras;
        if (p <= para && para < p + marks) {
            path->push_back(i);
            const bool found = markOf(n->children, para - p, path, at);
            if (!found) path->pop_back();
            return found;
        }
        p += marks;
    }
    return false;
}

/**
 * 按路径取到某一层的孩子表。路径越界时返回 nullptr：路径由同一棵树上的 locate 给出，
 * 越界不该发生，但这一层读的是别人的指针，宁可返回空也不越界读。
 */
const std::vector<Ref>* kidsAt(const Doc& doc, const std::vector<int>& path) {
    const std::vector<Ref>* layer = bodyOf(doc);
    if (layer == nullptr) return nullptr;
    for (int idx : path) {
        if (idx < 0 || idx >= static_cast<int>(layer->size())) return nullptr;
        const Ref& child = (*layer)[idx];
        if (!child) return nullptr;
        layer = &child->children;
    }
    return layer;
}

/**
 * 沿路径复制：把第 path[0] 个孩子换成 fn(它) 的结果，逐层换到根。
 * 走到终点才调用 fn；中间没命中的分支原样返回同一个指针 —— 结构共享就落在这一行上。
 */
Ref applyAtPath(const Ref& node, const std::vector<int>& path, size_t k,
                const std::function<Ref(const Ref&)>& fn) {
    if (k == path.size()) return fn(node);
    const int idx = path[k];
    if (idx < 0 || idx >= static_cast<int>(node->children.size())) return node;
    std::vector<Ref> kids = node->children;
    Ref updated = applyAtPath(kids[idx], path, k + 1, fn);
    if (updated == kids[idx]) return node;
    kids[idx] = std::move(updated);
    return withChildren(node, std::move(kids));
}

/**
 * 把"落点那一层的新孩子表"装回去，返回新的根。
 *
 * 顶层（path 为空）走正文那一节，容器内部沿路径换上去 —— 两个分支只差"叶子是谁"，
 * 编辑本身不关心自己在第几层。
 */
Ref withLeafChildren(const Doc& doc, const std::vector<int>& path, std::vector<Ref> kids) {
    const Ref& section = doc.root->children.front();
    if (path.empty()) {
        Ref updated = withChildren(section, std::move(kids));
        return withChildren(doc.root, {std::move(updated)});
    }
    // 路径的每一格都是"那一层孩子表里的下标"，起点是正文那一节而不是根：根下面只有一节，
    // 把根也算进路径会让 path 与 locate 的口径差一层。
    // 捕获的是引用：路径是一条链，fn 只会被调用一次，挪走孩子表之后不再读它。
    Ref updated = applyAtPath(section, path, 0,
                              [&kids](const Ref& n) { return withChildren(n, std::move(kids)); });
    return withChildren(doc.root, {std::move(updated)});
}

Ref runNode(NodeId id, std::vector<uint32_t> text, const CharFormat& marks,
            const std::string& style) {
    RunPayload p;
    p.text = std::move(text);
    p.marks = marks;
    p.style = style;
    return makeNode(id, NodeKind::TextRun, std::move(p), {});
}

/**
 * 一段文本追加到 cur；遇到段落标记收一段。
 */
void collectParas(const Ref& n, std::u32string* cur, std::vector<std::u32string>* out) {
    if (n->kind == NodeKind::ParagraphMark) {
        out->push_back(std::move(*cur));
        *cur = std::u32string();
        return;
    }
    if (n->kind == NodeKind::TextRun) {
        const RunPayload& rp = std::get<RunPayload>(n->payload);
        for (uint32_t cp : rp.text) cur->push_back(static_cast<char32_t>(cp));
        return;
    }
    for (const Ref& c : n->children) collectParas(c, cur, out);
}

}  // namespace

void Hasher::f32(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}

void Hasher::str(const std::string& s) {
    u64(s.size());
    for (unsigned char c : s) u64(c);
}

void hashSelf(Hasher& h, const Node& n) {
    h.u8(static_cast<uint8_t>(n.kind));
    h.u8(n.hidden ? 1 : 0);
    // 载荷用 std::visit 且最后一支带 static_assert：加一种载荷时裸 else 会把它静默按控件
    // 记录哈希，而这里要的是编不过。
    std::visit(
        [&h](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                h.u64(0);
            } else if constexpr (std::is_same_v<T, RunPayload>) {
                h.u64(1);
                h.u64(p.text.size());
                for (uint32_t cp : p.text) h.u64(cp);
                hashCharFormat(h, p.marks);
                h.str(p.style);
            } else if constexpr (std::is_same_v<T, MarkPayload>) {
                h.u64(2);
                hashParaPatch(h, p.para);
                h.str(p.style);
            } else if constexpr (std::is_same_v<T, ControlSpec>) {
                h.u64(3);
                hashControlSpec(h, p);
            } else if constexpr (std::is_same_v<T, TableSpec>) {
                h.u64(4);
                hashTableSpec(h, p);
            } else if constexpr (std::is_same_v<T, RowSpec>) {
                h.u64(5);
                hashRowSpec(h, p);
            } else if constexpr (std::is_same_v<T, CellSpec>) {
                h.u64(6);
                hashCellSpec(h, p);
            } else if constexpr (std::is_same_v<T, ImageSpec>) {
                h.u64(7);
                hashImageSpec(h, p);
            } else if constexpr (std::is_same_v<T, FormulaSpec>) {
                h.u64(8);
                hashFormulaSpec(h, p);
            } else {
                static_assert(std::is_same_v<T, FieldSpec>, "新增载荷要在这里加一支");
                h.u64(9);
                hashFieldSpec(h, p);
            }
        },
        n.payload);
}

Ref makeNode(NodeId id, NodeKind kind, Payload payload, std::vector<Ref> children, bool hidden) {
    auto node = std::make_shared<Node>();
    node->id = id;
    node->kind = kind;
    node->hidden = hidden;
    node->payload = std::move(payload);
    node->children = std::move(children);
    // 段落数与哈希同一趟算出来：两者都只依赖孩子，分两趟走没有意义。它不进哈希 ——
    //   由内容唯一确定的量再哈希一遍，只会让"改一个看不见的字段"看起来像改了内容。
    int paras = (node->kind == NodeKind::ParagraphMark) ? 1 : 0;
    for (const Ref& c : node->children) paras += c->paras;
    node->paras = paras;
    Hasher h;
    hashNode(h, *node);
    node->hash_lo = h.lo();
    node->hash_hi = h.hi();
    return node;
}

Doc adopt(Ref root, NodeId next_id) {
    Doc d;
    if (!root) return d;
    d.root = std::move(root);
    d.next_id = next_id;
    d.lo = d.root->hash_lo;
    d.hi = d.root->hash_hi;
    return d;
}

const std::vector<Ref>* bodyOf(const Doc& doc) {
    if (!doc.root || doc.root->children.empty()) return nullptr;
    return &doc.root->children.front()->children;
}

const Node* findNode(const Doc& doc, NodeId id) {
    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return nullptr;
    const Node* found = nullptr;
    // 显式栈：深层嵌套的文档不该把递归深度交给调用栈。
    std::vector<const Node*> stack;
    for (const Ref& c : *body) stack.push_back(c.get());
    while (!stack.empty() && found == nullptr) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n->id == id) {
            found = n;
            break;
        }
        for (const Ref& c : n->children) stack.push_back(c.get());
    }
    return found;
}

Doc makeTextDoc(const std::vector<std::u32string>& paragraphs) {
    NodeId next = 1;
    std::vector<Ref> kids;
    for (const std::u32string& p : paragraphs) {
        if (!p.empty()) {
            std::vector<uint32_t> cps;
            cps.reserve(p.size());
            for (char32_t c : p) cps.push_back(static_cast<uint32_t>(c));
            kids.push_back(runNode(next++, std::move(cps), CharFormat{}, std::string()));
        }
        kids.push_back(makeNode(next++, NodeKind::ParagraphMark, MarkPayload{}, {}));
    }
    Ref section = makeNode(next++, NodeKind::Section, {}, std::move(kids));
    Ref root = makeNode(next++, NodeKind::Document, {}, {std::move(section)});
    return adopt(std::move(root), next);
}

int paraCount(const Doc& doc) { return doc.root ? doc.root->paras : 0; }

std::vector<std::u32string> paraTexts(const Doc& doc) {
    std::vector<std::u32string> out;
    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return out;
    std::u32string cur;
    for (const Ref& c : *body) collectParas(c, &cur, &out);
    // 末尾没有结束标记的那一段：有内容就落一段。
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

EditResult insertText(const Doc& doc, int para, int cp, const std::u32string& text) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0) return fail("码点偏移为负");
    if (text.empty()) return EditResult{true, doc, {}};

    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    if (!locate(*body, para, cp, &path, &site)) return fail("码点偏移越界");
    const std::vector<Ref>* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = *layer;

    std::vector<uint32_t> add;
    add.reserve(text.size());
    for (char32_t c : text) add.push_back(static_cast<uint32_t>(c));

    NodeId next = doc.next_id;
    if (site.kind == SiteKind::InRun) {
        RunPayload run = std::get<RunPayload>(kids[site.child]->payload);
        // 插入的文本继承插入点的字符属性：打字落在一个加粗的 run 里就该是加粗的。
        run.text.insert(run.text.begin() + site.off, add.begin(), add.end());
        kids[site.child] = runNode(kids[site.child]->id, std::move(run.text), run.marks, run.style);
    } else {
        kids.insert(kids.begin() + site.child,
                    runNode(next++, std::move(add), CharFormat{}, std::string()));
    }

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), next), {}};
}

EditResult eraseText(const Doc& doc, int para, int cp, int count) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0 || count < 0) return fail("偏移或长度为负");
    if (count == 0) return EditResult{true, doc, {}};

    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    if (!locate(*body, para, cp, &path, &site)) return fail("码点偏移越界");
    const std::vector<Ref>* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = *layer;
    if (site.kind == SiteKind::BeforeChild) return fail("该位置没有可删的内容");

    int remaining = count;
    bool touched = false;
    for (int i = site.child; i < static_cast<int>(kids.size()) && remaining > 0; ++i) {
        if (kids[i]->kind == NodeKind::ParagraphMark) break;  // 不跨段
        if (kids[i]->kind != NodeKind::TextRun) continue;     // 容器不在这里删
        RunPayload run = std::get<RunPayload>(kids[i]->payload);
        const int len = static_cast<int>(run.text.size());
        const int start = (i == site.child) ? site.off : 0;
        const int take = std::min(remaining, len - start);
        if (take <= 0) continue;
        run.text.erase(run.text.begin() + start, run.text.begin() + start + take);
        remaining -= take;
        touched = true;
        if (run.text.empty()) {
            // 零宽 run 不留在树里：它与"这里什么都没有"不可区分，留着只会让哈希与规范化
            // 之后的形状对不上。
            kids.erase(kids.begin() + i);
            --i;
        } else {
            kids[i] = runNode(kids[i]->id, std::move(run.text), run.marks, run.style);
        }
    }
    if (!touched) return fail("没有可删的内容");
    if (remaining > 0) return fail("删除区间超出本段，未跨段删除");

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), doc.next_id), {}};
}

EditResult splitPara(const Doc& doc, int para, int cp) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0) return fail("码点偏移为负");

    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    if (!locate(*body, para, cp, &path, &site)) return fail("码点偏移越界");
    const std::vector<Ref>* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = *layer;

    NodeId next = doc.next_id;
    if (site.kind == SiteKind::BeforeChild) {
        kids.insert(kids.begin() + site.child,
                    makeNode(next++, NodeKind::ParagraphMark, MarkPayload{}, {}));
    } else {
        RunPayload run = std::get<RunPayload>(kids[site.child]->payload);
        std::vector<uint32_t> head(run.text.begin(), run.text.begin() + site.off);
        std::vector<uint32_t> tail(run.text.begin() + site.off, run.text.end());
        std::vector<Ref> repl;
        if (!head.empty()) {
            repl.push_back(runNode(kids[site.child]->id, std::move(head), run.marks, run.style));
        }
        repl.push_back(makeNode(next++, NodeKind::ParagraphMark, MarkPayload{}, {}));
        if (!tail.empty()) repl.push_back(runNode(next++, std::move(tail), run.marks, run.style));
        kids.erase(kids.begin() + site.child);
        kids.insert(kids.begin() + site.child, repl.begin(), repl.end());
    }

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), next), {}};
}

EditResult mergePara(const Doc& doc, int para) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para + 1 >= paraCount(doc)) return fail("没有下一段可合并");

    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return fail("文档没有正文");
    std::vector<int> path;
    int at = -1;
    if (!markOf(*body, para, &path, &at)) return fail("段落标记丢失");
    const std::vector<Ref>* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = *layer;
    kids.erase(kids.begin() + at);

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), doc.next_id), {}};
}

EditResult insertControl(const Doc& doc, int para, int cp, const ControlSpec& spec,
                         const std::u32string& label, NodeId* created) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0) return fail("码点偏移为负");

    const std::vector<Ref>* body = bodyOf(doc);
    if (!body) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    if (!locate(*body, para, cp, &path, &site)) return fail("码点偏移越界");
    const std::vector<Ref>* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = *layer;

    NodeId next = doc.next_id;
    std::vector<Ref> inner;
    if (!label.empty()) {
        std::vector<uint32_t> cps;
        cps.reserve(label.size());
        for (char32_t c : label) cps.push_back(static_cast<uint32_t>(c));
        inner.push_back(runNode(next++, std::move(cps), CharFormat{}, std::string()));
    }
    Ref control = makeNode(next++, NodeKind::ContentControl, spec, std::move(inner));
    if (created != nullptr) *created = control->id;

    if (site.kind == SiteKind::BeforeChild) {
        kids.insert(kids.begin() + site.child, control);
    } else {
        RunPayload run = std::get<RunPayload>(kids[site.child]->payload);
        std::vector<uint32_t> head(run.text.begin(), run.text.begin() + site.off);
        std::vector<uint32_t> tail(run.text.begin() + site.off, run.text.end());
        std::vector<Ref> repl;
        if (!head.empty()) {
            repl.push_back(runNode(kids[site.child]->id, std::move(head), run.marks, run.style));
        }
        repl.push_back(control);
        if (!tail.empty()) repl.push_back(runNode(next++, std::move(tail), run.marks, run.style));
        kids.erase(kids.begin() + site.child);
        kids.insert(kids.begin() + site.child, repl.begin(), repl.end());
    }

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), next), {}};
}

EditResult setHidden(const Doc& doc, NodeId id, bool hidden) {
    if (!doc.root) return fail("空文档");
    const Node* n = findNode(doc, id);
    if (n == nullptr) return fail("节点不存在");
    if (n->hidden == hidden) return EditResult{true, doc, {}};

    Ref repl = makeNode(n->id, n->kind, n->payload, n->children, hidden);
    bool found = false;
    Ref root = replaceNode(doc.root, id, repl, &found);
    if (!found) return fail("节点不存在");
    return EditResult{true, adopt(std::move(root), doc.next_id), {}};
}

}  // namespace heditor::rev
