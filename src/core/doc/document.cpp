#include "core/doc/document.h"

#include "core/doc/internal.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <type_traits>
#include <utility>

namespace heditor::doc {

/**
 * 模块内部工具。用命名空间而不是匿名命名空间，是因为步骤层要复用同一份路径复制与定位：
 * 两处各写一份的话，"改一段"就会有两条构造路径，哈希迟早对不上。
 */
namespace detail {

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
    h.u64(p.line_rule.has_value() ? 1 : 0);
    if (p.line_rule) h.u64(static_cast<uint64_t>(*p.line_rule));
    h.u64(p.line_factor.has_value() ? 1 : 0);
    if (p.line_factor) h.f32(*p.line_factor);
    h.u64(p.line_exact.has_value() ? 1 : 0);
    if (p.line_exact) hashLength(h, *p.line_exact);
    h.u64(p.space_before.has_value() ? 1 : 0);
    if (p.space_before) hashLength(h, *p.space_before);
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
 * 节记录的哈希：页面设置里每一样都会改排版（纸张、页边距、分节分页、页码重编、书写方向），
 * 所以全部进内容哈希。
 *
 * 页眉页脚的 root 是身份（指向本节的子树），不进哈希 —— 那棵子树本身是节的子节点，
 * 它的内容已经在节哈希里了。distance 与 linked 是记录本身，进。
 */
void hashSectionSpec(Hasher& h, const SectionSpec& sec) {
    h.u8(static_cast<uint8_t>(sec.paper.orient));
    hashLength(h, sec.paper.width);
    hashLength(h, sec.paper.height);
    hashLength(h, sec.margins.top);
    hashLength(h, sec.margins.bottom);
    hashLength(h, sec.margins.left);
    hashLength(h, sec.margins.right);
    hashLength(h, sec.gutter.size);
    h.u8(static_cast<uint8_t>(sec.gutter.side));
    h.u8(sec.mirror_margins ? 1 : 0);
    h.u8(static_cast<uint8_t>(sec.break_kind));
    h.u8(static_cast<uint8_t>(sec.valign));
    h.u8(static_cast<uint8_t>(sec.writing_mode.inline_axis));
    h.u8(static_cast<uint8_t>(sec.writing_mode.block_flow));
    h.u8(static_cast<uint8_t>(sec.writing_mode.orientation));
    h.u8(static_cast<uint8_t>(sec.numbering.format));
    h.i64(sec.numbering.start);
    h.u8(sec.numbering.restart ? 1 : 0);
    for (int v = 0; v < kHeaderFooterVariantCount; ++v) {
        hashLength(h, sec.headers[v].distance);
        h.u8(sec.headers[v].linked_to_previous ? 1 : 0);
        hashLength(h, sec.footers[v].distance);
        h.u8(sec.footers[v].linked_to_previous ? 1 : 0);
    }
    // 属性袋按序遍历：std::map 本身有序，所以两个相等的袋子哈希相同。
    h.u64(sec.extra.size());
    for (const auto& kv : sec.extra) {
        h.str(kv.first);
        h.str(kv.second);
    }
    // 水印画在每一页的像素上，但不进任何块的内容 —— 它只能靠节哈希把页指纹带动起来。
    h.u8(sec.watermark.enabled ? 1 : 0);
    h.str(sec.watermark.text);
    h.f32(sec.watermark.size_pt);
    hashColor(h, sec.watermark.color);
    h.f32(sec.watermark.angle_deg);
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
    h.f32(im.dx_pt);
    h.f32(im.dy_pt);
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
 * 控件特有层的哈希：一个控件只带自己那一组属性，但每一组都要整个进哈希。
 *
 * 变体下标先混进去，再按族逐字段混 —— 两个不同的族即使字段值撞了也不会得到同一个哈希。
 * @param h 哈希器
 * @param d 特有层
 */
void hashControlDetail(Hasher& h, const ControlDetail& d) {
    h.u8(static_cast<uint8_t>(d.index()));
    if (const auto* t = std::get_if<TextDetail>(&d)) {
        h.u8(static_cast<uint8_t>(t->filter));
        return;
    }
    if (const auto* c = std::get_if<ChoiceDetail>(&d)) {
        h.str(c->label);
        h.u64(c->options.size());
        for (const ControlOption& o : c->options) {
            h.str(o.value);
            h.str(o.label);
        }
        h.str(c->group);
        h.u8(static_cast<uint8_t>(c->multi ? 1 : 0));
        h.u8(static_cast<uint8_t>(c->caret_barrier ? 1 : 0));
        return;
    }
    if (const auto* dp = std::get_if<DropdownDetail>(&d)) {
        h.u64(dp->options.size());
        for (const ControlOption& o : dp->options) {
            h.str(o.value);
            h.str(o.label);
        }
        h.u8(static_cast<uint8_t>(dp->free_input ? 1 : 0));
        return;
    }
    if (const auto* dt = std::get_if<DateDetail>(&d)) {
        h.str(dt->format);
        h.u64(static_cast<uint64_t>(dt->units));
        return;
    }
    if (const auto* cu = std::get_if<CustomDetail>(&d)) {
        // props 是 std::map：遍历顺序由键定，跨进程稳定。
        h.u64(cu->props.size());
        for (const auto& kv : cu->props) {
            h.str(kv.first);
            h.str(kv.second);
        }
    }
}

/**
 * 控件记录的哈希。
 *
 * ControlSpec 的相等是默认生成的（加字段自动比），哈希不会自动跟着变 —— 漏一个字段的表现是
 * "改了这个属性，缓存照样命中"，画面上什么都不变。所以 doc_test 里逐字段构造只差一个字段的
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
    hashControlDetail(h, c.detail);
}

/**
 * 节点的完整哈希：自身哈希加孩子哈希。身份不进哈希（见文件头的说明）。
 */
void hashNode(Hasher& h, const Node& n) {
    hashSelf(h, n);
    h.u64(n.children.size());
    if (!n.children.empty()) {
        h.u64(n.children.aggregateLo());
        h.u64(n.children.aggregateHi());
    }
}

Ref withChildren(const Ref& node, std::vector<Ref> children) {
    return makeNodeFromChildren(node->id, node->kind, node->payload,
                                node->children.rebuilt(std::move(children)), node->hidden);
}

Ref withChildren(const Ref& node, Children children) {
    return makeNodeFromChildren(node->id, node->kind, node->payload, std::move(children),
                                node->hidden);
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
    return kind == NodeKind::Table || kind == NodeKind::TableRow || kind == NodeKind::TableCell ||
           kind == NodeKind::Section;
}

/**
 * 段落内容在流里的落点：定义在 internal.h，本体与步骤层共用。
 */

bool locate(const Children& kids, int para, int cp, std::vector<int>* path, Site* out) {
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
bool markOf(const Children& kids, int para, std::vector<int>* path, int* at) {
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
const Children* kidsAt(const Doc& doc, const std::vector<int>& path) {
    if (!doc.root) return nullptr;
    const Children* layer = &doc.root->children;
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
    const Ref updated = applyAtPath(node->children[static_cast<size_t>(idx)], path, k + 1, fn);
    if (updated == node->children[static_cast<size_t>(idx)]) return node;
    return withChildren(node, node->children.withReplaced(static_cast<size_t>(idx), updated));
}

/**
 * 把"落点那一层的新孩子表"装回去，返回新的根。
 *
 * 顶层（path 为空）走正文那一节，容器内部沿路径换上去 —— 两个分支只差"叶子是谁"，
 * 编辑本身不关心自己在第几层。
 */
Ref withLeafChildren(const Doc& doc, const std::vector<int>& path, std::vector<Ref> kids) {
    // 路径的每一格都是"那一层孩子表里的下标"，起点是根：第一格是节，往后才是节里的内容。
    // 捕获的是引用：路径是一条链，fn 只会被调用一次，挪走孩子表之后不再读它。
    return applyAtPath(doc.root, path, 0,
                       [&kids](const Ref& n) { return withChildren(n, std::move(kids)); });
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

}  // namespace detail

/**
 * 本文件后半是公开实现。逐处写 detail:: 只会让"这是内部工具"变成噪声；命名空间仍在，
 * 跨文件仍认 detail，这里只是省掉同一文件内的限定。
 */
using namespace detail;

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
                hashCharFormat(h, p.marks);
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
            } else if constexpr (std::is_same_v<T, FieldSpec>) {
                h.u64(9);
                hashFieldSpec(h, p);
            } else {
                static_assert(std::is_same_v<T, SectionSpec>, "新增载荷要在这里加一支");
                h.u64(10);
                hashSectionSpec(h, p);
            }
        },
        n.payload);
}

Ref makeNode(NodeId id, NodeKind kind, Payload payload, std::vector<Ref> children, bool hidden) {
    return makeNodeFromChildren(id, kind, std::move(payload), Children(std::move(children)),
                                hidden);
}

Ref makeNodeFromChildren(NodeId id, NodeKind kind, Payload payload, Children children,
                         bool hidden) {
    auto node = std::make_shared<Node>();
    node->id = id;
    node->kind = kind;
    node->hidden = hidden;
    node->payload = std::move(payload);
    node->children = std::move(children);
    // 段落数与哈希同一趟算出来：两者都只依赖孩子，分两趟走没有意义。它不进哈希 ——
    //   由内容唯一确定的量再哈希一遍，只会让"改一个看不见的字段"看起来像改了内容。
    node->paras = ((node->kind == NodeKind::ParagraphMark) ? 1 : 0) + node->children.paras();
    // 呈现态的那一份：隐藏子树整棵不产生块，所以它自己记 0，孩子也不必再数。
    int paras_display = 0;
    if (!node->hidden) {
        paras_display =
            ((node->kind == NodeKind::ParagraphMark) ? 1 : 0) + node->children.parasDisplay();
    }
    node->paras_display = paras_display;
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

const Children* bodyOf(const Doc& doc) {
    if (!doc.root || doc.root->children.empty()) return nullptr;
    return &doc.root->children.front()->children;
}

const Node* findNode(const Doc& doc, NodeId id) {
    if (!doc.root) return nullptr;
    const Node* found = nullptr;
    // 显式栈：深层嵌套的文档不该把递归深度交给调用栈。根也要在栈里 —— 流位置 0 常常
    //   落在根上（各节之前），少了它，按节点号找根的调用一律拿到空指针。
    std::vector<const Node*> stack;
    stack.push_back(doc.root.get());
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

const Node* parentOf(const Doc& doc, NodeId id) {
    if (!doc.root) return nullptr;
    std::vector<const Node*> stack;
    stack.push_back(doc.root.get());
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        for (const Ref& c : n->children) {
            if (c->id == id) return n;
            stack.push_back(c.get());
        }
    }
    return nullptr;
}

NodeSpec toSpec(const Node& node) {
    NodeSpec s;
    s.kind = node.kind;
    s.hidden = node.hidden;
    if (const auto* run = std::get_if<RunPayload>(&node.payload)) {
        s.text = run->text;
        s.marks = run->marks;
        s.styleRef = run->style;
    } else if (const auto* mark = std::get_if<MarkPayload>(&node.payload)) {
        s.para = mark->para;
        s.styleRef = mark->style;
        s.marks = mark->marks;
    } else if (const auto* c = std::get_if<ControlSpec>(&node.payload)) {
        s.control = *c;
    } else if (const auto* t = std::get_if<TableSpec>(&node.payload)) {
        s.table = *t;
    } else if (const auto* r = std::get_if<RowSpec>(&node.payload)) {
        s.row = *r;
    } else if (const auto* c = std::get_if<CellSpec>(&node.payload)) {
        s.cell = *c;
    } else if (const auto* im = std::get_if<ImageSpec>(&node.payload)) {
        s.image = *im;
    } else if (const auto* f = std::get_if<FormulaSpec>(&node.payload)) {
        s.formula = *f;
    } else if (const auto* fd = std::get_if<FieldSpec>(&node.payload)) {
        s.field = *fd;
    }
    s.children.reserve(node.children.size());
    for (const Ref& c : node.children) s.children.push_back(toSpec(*c));
    return s;
}

Doc makeTextDoc(const std::vector<std::u32string>& paragraphs) {
    return makeTextDoc(paragraphs, ParaFormatPatch{});
}

Doc makeTextDoc(const std::vector<std::u32string>& paragraphs, const ParaFormatPatch& para) {
    NodeId next = 1;
    std::vector<Ref> kids;
    for (const std::u32string& p : paragraphs) {
        if (!p.empty()) {
            std::vector<uint32_t> cps;
            cps.reserve(p.size());
            for (char32_t c : p) cps.push_back(static_cast<uint32_t>(c));
            kids.push_back(runNode(next++, std::move(cps), CharFormat{}, std::string()));
        }
        kids.push_back(makeNode(next++, NodeKind::ParagraphMark, MarkPayload{para, "", {}}, {}));
    }
    Ref section = makeNode(next++, NodeKind::Section, SectionSpec{}, std::move(kids));
    Ref root = makeNode(next++, NodeKind::Document, {}, {std::move(section)});
    return adopt(std::move(root), next);
}

int paraCount(const Doc& doc) { return doc.root ? doc.root->paras : 0; }

std::vector<std::u32string> paraTexts(const Doc& doc) {
    std::vector<std::u32string> out;
    if (!doc.root) return out;
    std::u32string cur;
    for (const Ref& c : doc.root->children) collectParas(c, &cur, &out);
    // 末尾没有结束标记的那一段：有内容就落一段。
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

EditResult insertText(const Doc& doc, int para, int cp, const std::u32string& text) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0) return fail("码点偏移为负");
    if (text.empty()) return EditResult{true, doc, {}};

    if (!doc.root || doc.root->children.empty()) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    // 起点是根的孩子表（各节都在里面）：节是块级容器，段落号在节之间连续，
    //   于是"第几段"与"第几节"是两件事，定位只认前者。
    if (!locate(doc.root->children, para, cp, &path, &site)) return fail("码点偏移越界");
    const Children* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = layer->toVector();

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

    if (!doc.root || doc.root->children.empty()) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    // 起点是根的孩子表（各节都在里面）：节是块级容器，段落号在节之间连续，
    //   于是"第几段"与"第几节"是两件事，定位只认前者。
    if (!locate(doc.root->children, para, cp, &path, &site)) return fail("码点偏移越界");
    const Children* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = layer->toVector();
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

    if (!doc.root || doc.root->children.empty()) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    // 起点是根的孩子表（各节都在里面）：节是块级容器，段落号在节之间连续，
    //   于是"第几段"与"第几节"是两件事，定位只认前者。
    if (!locate(doc.root->children, para, cp, &path, &site)) return fail("码点偏移越界");
    const Children* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = layer->toVector();

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

    if (!doc.root || doc.root->children.empty()) return fail("文档没有正文");
    std::vector<int> path;
    int at = -1;
    if (!markOf(doc.root->children, para, &path, &at)) return fail("段落标记丢失");
    const Children* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = layer->toVector();
    kids.erase(kids.begin() + at);

    Ref root = withLeafChildren(doc, path, std::move(kids));
    return EditResult{true, adopt(std::move(root), doc.next_id), {}};
}

EditResult insertControl(const Doc& doc, int para, int cp, const ControlSpec& spec,
                         const std::u32string& label, NodeId* created) {
    if (!doc.root) return fail("空文档");
    if (para < 0 || para >= paraCount(doc)) return fail("段落越界");
    if (cp < 0) return fail("码点偏移为负");

    if (!doc.root || doc.root->children.empty()) return fail("文档没有正文");
    std::vector<int> path;
    Site site;
    // 起点是根的孩子表（各节都在里面）：节是块级容器，段落号在节之间连续，
    //   于是"第几段"与"第几节"是两件事，定位只认前者。
    if (!locate(doc.root->children, para, cp, &path, &site)) return fail("码点偏移越界");
    const Children* layer = kidsAt(doc, path);
    if (layer == nullptr) return fail("落点路径失效");
    std::vector<Ref> kids = layer->toVector();

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

bool checkSupportedLayout(const Doc& doc, std::string* why) {
    if (!doc.root) return true;

    const auto fail = [why](std::string message) {
        if (why != nullptr) *why = std::move(message);
        return false;
    };

    bool has_hf_node = false;
    bool has_field = false;
    std::function<void(const Ref&)> scan = [&](const Ref& n) {
        if (!n || has_hf_node) return;
        if (n->kind == NodeKind::HeaderFooter) {
            has_hf_node = true;
            return;
        }
        if (n->kind == NodeKind::Field) has_field = true;
        for (const Ref& c : n->children) scan(c);
    };
    scan(doc.root);
    if (has_hf_node) return fail("页眉页脚内容尚未参与排版");
    if (has_field) return fail("域元素尚未进入排版输入");

    // 第一节的纸张与页边距：后面的节必须与它相同（整篇一份页度量）。
    const SectionSpec* first_spec = nullptr;

    for (const Ref& section : doc.root->children) {
        if (!section || section->kind != NodeKind::Section) continue;
        const auto* spec = std::get_if<SectionSpec>(&section->payload);
        if (spec == nullptr) return fail("节缺少节记录");

        // 纸张与页边距按"整篇一份"支持：引擎只有一套页度量，文档里记的那一份由装载时
        //   采用成配置（见 EditorEngine::adoptPageFromDocument）。多节各写各的仍拒绝 ——
        //   那是按节排版，不是现在的支持范围。
        if (first_spec == nullptr) {
            first_spec = spec;
        } else if (!(spec->paper == first_spec->paper) || !(spec->margins == first_spec->margins)) {
            return fail("按节页度量尚未参与排版");
        }

        for (const HeaderFooter& hf : spec->headers) {
            if (hf.root != kInvalidNodeId) return fail("页眉内容尚未参与排版");
            if (!(hf.distance == Length{}) || !hf.linked_to_previous) {
                return fail("页眉距离或链接设置尚未参与排版");
            }
        }
        for (const HeaderFooter& hf : spec->footers) {
            if (hf.root != kInvalidNodeId) return fail("页脚内容尚未参与排版");
            if (!(hf.distance == Length{}) || !hf.linked_to_previous) {
                return fail("页脚距离或链接设置尚未参与排版");
            }
        }

        // 分节分页与第一节水印是支持的；其余节字段（纸张、页边距、装订线、页码等）
        // 还没有被排版消费。
        SectionSpec baseline;
        baseline.break_kind = spec->break_kind;
        baseline.watermark = spec->watermark;
        // 纸张与页边距在上面按"全篇相同"判过了，这里取本节的同一份，等于不再要求它们是默认值。
        baseline.paper = spec->paper;
        baseline.margins = spec->margins;
        if (!spec->sameAs(baseline)) return fail("按节页度量尚未参与排版");
    }
    return true;
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

}  // namespace heditor::doc
