#include "core/doc/serialize.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace heditor {

namespace {

using nlohmann::ordered_json;

/**
 * 属性键。写与读共用同一套常量：两处各写一遍字面量，迟早会有一处拼错，
 * 而拼错的表现是"这个字段读回来是默认值"，也就是静默丢数据。
 */
constexpr const char* kV = "v";
constexpr const char* kSym = "sym";
constexpr const char* kKinds = "k";
constexpr const char* kStrings = "s";
constexpr const char* kRoots = "roots";
constexpr const char* kNodes = "nodes";
constexpr const char* kNext = "next";

constexpr const char* kId = "id";
constexpr const char* kKind = "k";
constexpr const char* kCount = "n";
constexpr const char* kHidden = "h";
constexpr const char* kText = "t";
constexpr const char* kPara = "p";
constexpr const char* kStyle = "s";
constexpr const char* kMarks = "m";
constexpr const char* kControl = "c";
constexpr const char* kCell = "cs";
constexpr const char* kTable = "tb";
constexpr const char* kField = "f";
constexpr const char* kSection = "sec";
constexpr const char* kImage = "im";
constexpr const char* kFormula = "fm";

/**
 * 名字池：写的时候按首次出现顺序编号，正文里只留下标。
 *
 * 进池的是重复出现的开放集合（样式名、控件业务类型与 key、校验规则名、语言标签）——
 * 属性名本身是封闭的一小组短键，为它们建表只会多一层间接，省不下字节。
 */
class Strings {
public:
    int add(const std::string& s) {
        const auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        const int id = static_cast<int>(list_.size());
        list_.push_back(s);
        index_.emplace(s, id);
        return id;
    }
    const std::vector<std::string>& list() const { return list_; }

private:
    std::vector<std::string> list_;
    std::unordered_map<std::string, int> index_;
};

/**
 * 浮点的十进制文本。9 位有效数字是 float 的往返充分条件，所以写出去再读回来是同一个值。
 */
std::string floatText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    return buf;
}

bool parseFloatText(const std::string& text, float* out) {
    if (out == nullptr || text.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

/**
 * 严格 UTF-8 校验：拒绝过长编码、代理区与超出 U+10FFFF 的码点。
 *
 * 为什么要自己查一遍：JSON 库在遇到非法字节时，有异常的环境下抛异常、没有异常的环境下
 * 直接终止进程（wasm 构建关掉了异常）。两者都不是这里想要的 —— 前者要调用方接住，
 * 后者是一次崩溃。写出之前自己查一遍，查到就返回失败并说明是哪一处。
 */
bool isValidUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        uint32_t cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            cp = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            cp = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            cp = lead & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= s.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((extra == 1 && cp < 0x80u) || (extra == 2 && cp < 0x800u) ||
            (extra == 3 && cp < 0x10000u)) {
            return false;
        }
        if (cp >= 0xD800u && cp <= 0xDFFFu) return false;
        if (cp > 0x10FFFFu) return false;
        i += extra + 1;
    }
    return true;
}

/**
 * 把要写出去的 JSON 树里的每个字符串查一遍（值与原对象的键都查）。
 */
bool validateStrings(const ordered_json& j, std::string* error) {
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (!isValidUtf8(s)) {
            *error = "字符串不是合法 UTF-8（" + std::to_string(s.size()) + " 字节）";
            return false;
        }
        return true;
    }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!isValidUtf8(it.key())) {
                *error = "键不是合法 UTF-8（" + std::to_string(it.key().size()) + " 字节）";
                return false;
            }
            if (!validateStrings(it.value(), error)) return false;
        }
        return true;
    }
    if (j.is_array()) {
        for (const ordered_json& item : j) {
            if (!validateStrings(item, error)) return false;
        }
    }
    return true;
}

ordered_json colorJson(const Color& c) { return ordered_json::array({c.r, c.g, c.b, c.a}); }

ordered_json writingModeJson(const WritingMode& wm) {
    return ordered_json::array({static_cast<int>(wm.inline_axis), static_cast<int>(wm.block_flow),
                                static_cast<int>(wm.orientation)});
}

ordered_json paraJson(const ParaFormatPatch& p) {
    ordered_json j = ordered_json::object();
    if (p.font_index.has_value()) j["fi"] = *p.font_index;
    if (p.size.has_value()) j["sz"] = printLength(*p.size);
    if (p.wm.has_value()) j["wm"] = writingModeJson(*p.wm);
    if (p.line_factor.has_value()) j["lf"] = floatText(*p.line_factor);
    if (p.space_after.has_value()) j["sa"] = printLength(*p.space_after);
    if (p.page_break_before.has_value()) j["pb"] = *p.page_break_before;
    if (p.language.has_value()) {
        j["lang"] = *p.language == nullptr ? std::string() : std::string(*p.language);
    }
    if (p.color.has_value()) j["col"] = colorJson(*p.color);
    if (p.fallback_font_indices.has_value()) j["fb"] = *p.fallback_font_indices;
    return j;
}

ordered_json marksJson(const CharFormat& m) {
    ordered_json j = ordered_json::object();
    if (m.bold.has_value()) j["b"] = *m.bold;
    if (m.italic.has_value()) j["i"] = *m.italic;
    if (m.underline.has_value()) j["u"] = *m.underline;
    if (m.font_index.has_value()) j["fi"] = *m.font_index;
    if (m.size.has_value()) j["sz"] = printLength(*m.size);
    if (m.color.has_value()) j["col"] = colorJson(*m.color);
    return j;
}

ordered_json borderSideJson(const BorderSide& b) {
    ordered_json j = ordered_json::object();
    if (b.on) j["on"] = true;
    if (b.style != BorderStyle::Solid) j["st"] = borderStyleName(b.style);
    if (!(b.width == Length::abs(ptToEmu(0.5)))) j["w"] = printLength(b.width);
    if (!(b.color == Color::rgb(0, 0, 0))) j["col"] = colorJson(b.color);
    return j;
}

ordered_json glyphJson(const GlyphStyle& g) {
    ordered_json j = ordered_json::object();
    if (!g.kind.empty()) j["kind"] = g.kind;
    if (!(g.width == Length::abs(ptToEmu(0.75)))) j["w"] = printLength(g.width);
    if (g.size != Length{}) j["sz"] = printLength(g.size);
    if (!(g.color == Color::rgb(0, 0, 0))) j["col"] = colorJson(g.color);
    if (!(g.fill == Color::rgb(0, 0, 0))) j["fill"] = colorJson(g.fill);
    return j;
}

ordered_json appearanceJson(const Appearance& a, Strings* strings) {
    ordered_json j = ordered_json::object();
    if (a.width != Length{}) j["w"] = printLength(a.width);
    if (a.overflow != Overflow::Expand) j["ov"] = overflowName(a.overflow);
    if (a.underline != Underline::None) j["ul"] = underlineName(a.underline);
    ordered_json border = ordered_json::object();
    if (!(a.border.left == BorderSide{})) border["l"] = borderSideJson(a.border.left);
    if (!(a.border.right == BorderSide{})) border["r"] = borderSideJson(a.border.right);
    if (!(a.border.top == BorderSide{})) border["t"] = borderSideJson(a.border.top);
    if (!(a.border.bottom == BorderSide{})) border["b"] = borderSideJson(a.border.bottom);
    if (!border.empty()) j["bd"] = std::move(border);
    if (!(a.glyph == GlyphStyle{})) j["gl"] = glyphJson(a.glyph);
    if (a.align != Align::Start) j["al"] = alignName(a.align);
    if (!a.placeholder.empty()) j["ph"] = a.placeholder;
    if (!a.styleRef.empty()) j["sr"] = strings->add(a.styleRef);
    if (!a.invalidStyleRef.empty()) j["isr"] = strings->add(a.invalidStyleRef);
    return j;
}

ordered_json controlValueJson(const ControlValue& v) {
    ordered_json j = ordered_json::object();
    if (v.flag) j["flag"] = true;
    if (!v.text.empty()) j["text"] = v.text;
    if (!v.items.empty()) j["items"] = v.items;
    return j;
}

ordered_json controlJson(const ControlSpec& c, Strings* strings) {
    ordered_json j = ordered_json::object();
    if (!c.key.empty()) j["key"] = strings->add(c.key);
    if (!c.controlType.empty()) j["ct"] = strings->add(c.controlType);
    if (c.valueShape != ValueShape::Text) j["vs"] = valueShapeName(c.valueShape);
    if (!(c.value == ControlValue{})) j["val"] = controlValueJson(c.value);
    if (c.readonly) j["ro"] = true;
    if (c.required) j["rq"] = true;
    // structureLock 的默认是 true，所以只在放开时写。默认值不写这条在这里最容易漏。
    if (!c.structureLock) j["lk"] = false;
    if (!(c.appearance == Appearance{})) j["ap"] = appearanceJson(c.appearance, strings);
    if (!c.validateRef.empty()) j["vr"] = strings->add(c.validateRef);
    return j;
}

ordered_json tableJson(const TableSpec& t) {
    ordered_json j = ordered_json::object();
    if (!t.columns.empty()) {
        ordered_json cols = ordered_json::array();
        for (const TableColumn& c : t.columns) {
            ordered_json col = ordered_json::object();
            if (!c.field_ref.empty()) col["fr"] = c.field_ref;
            if (!c.title.empty()) col["ti"] = c.title;
            if (!c.data_type.empty()) col["dt"] = c.data_type;
            if (!c.editor_style.empty()) col["es"] = c.editor_style;
            if (c.width != Length{}) col["w"] = printLength(c.width);
            if (c.align != Align::Start) col["al"] = alignName(c.align);
            cols.push_back(std::move(col));
        }
        j["cols"] = std::move(cols);
    }
    if (!t.grid_cols.empty()) {
        ordered_json grid = ordered_json::array();
        for (const Length& l : t.grid_cols) grid.push_back(printLength(l));
        j["grid"] = std::move(grid);
    }
    if (t.header_rows != 0) j["hr"] = t.header_rows;
    return j;
}

ordered_json fieldJson(const FieldSpec& f) {
    ordered_json j = ordered_json::object();
    if (f.kind != FieldKind::PageNumber) j["kind"] = fieldKindName(f.kind);
    if (!f.name.empty()) j["name"] = f.name;
    if (!f.cache.empty()) j["cache"] = f.cache;
    return j;
}

ordered_json headerFooterJson(const HeaderFooter& hf) {
    ordered_json j = ordered_json::object();
    if (hf.root != kInvalidNodeId) j["root"] = hf.root;
    if (hf.distance != Length{}) j["dist"] = printLength(hf.distance);
    // linked_to_previous 的默认是 true，只在断链时写。
    if (!hf.linked_to_previous) j["link"] = false;
    return j;
}

ordered_json sectionJson(const SectionSpec& sec) {
    ordered_json j = ordered_json::object();
    const SectionMargins kDefaultMargins;
    if (!(sec.paper == Paper{})) {
        ordered_json p = ordered_json::object();
        if (sec.paper.name != PaperName::A4) p["name"] = paperNameName(sec.paper.name);
        if (sec.paper.orient != Orientation::Portrait) {
            p["orient"] = orientationName(sec.paper.orient);
        }
        if (sec.paper.width != Length{}) p["w"] = printLength(sec.paper.width);
        if (sec.paper.height != Length{}) p["h"] = printLength(sec.paper.height);
        j["paper"] = std::move(p);
    }
    if (!(sec.margins == kDefaultMargins)) {
        ordered_json m = ordered_json::object();
        if (!(sec.margins.top == kDefaultMargins.top)) m["t"] = printLength(sec.margins.top);
        if (!(sec.margins.bottom == kDefaultMargins.bottom)) {
            m["b"] = printLength(sec.margins.bottom);
        }
        if (!(sec.margins.left == kDefaultMargins.left)) m["l"] = printLength(sec.margins.left);
        if (!(sec.margins.right == kDefaultMargins.right)) m["r"] = printLength(sec.margins.right);
        j["mar"] = std::move(m);
    }
    if (!(sec.gutter == Gutter{})) {
        ordered_json g = ordered_json::object();
        if (sec.gutter.size != Length{}) g["sz"] = printLength(sec.gutter.size);
        if (sec.gutter.side != Gutter::Side::Left) g["side"] = static_cast<int>(sec.gutter.side);
        j["gut"] = std::move(g);
    }
    if (sec.mirror_margins) j["mir"] = true;
    if (sec.break_kind != SectionBreak::Continuous) j["brk"] = sectionBreakName(sec.break_kind);
    if (sec.valign != VerticalAlign::Top) j["va"] = verticalAlignName(sec.valign);
    if (sec.writing_mode.inline_axis != InlineAxis::Horizontal ||
        sec.writing_mode.block_flow != BlockFlow::TopToBottom ||
        sec.writing_mode.orientation != TextOrientation::Mixed) {
        j["wm"] = writingModeJson(sec.writing_mode);
    }
    if (!(sec.numbering == PageNumbering{})) {
        ordered_json n = ordered_json::object();
        if (sec.numbering.format != PageNumberFormat::Arabic) {
            n["fmt"] = pageNumberFormatName(sec.numbering.format);
        }
        if (sec.numbering.start != 1) n["start"] = sec.numbering.start;
        if (sec.numbering.restart) n["restart"] = true;
        j["num"] = std::move(n);
    }
    ordered_json heads = ordered_json::array();
    ordered_json feet = ordered_json::array();
    bool has_hf = false;
    for (int v = 0; v < kHeaderFooterVariantCount; ++v) {
        heads.push_back(headerFooterJson(sec.headers[v]));
        feet.push_back(headerFooterJson(sec.footers[v]));
        if (!(sec.headers[v] == HeaderFooter{}) || !(sec.footers[v] == HeaderFooter{})) {
            has_hf = true;
        }
    }
    if (has_hf) {
        j["hd"] = std::move(heads);
        j["ft"] = std::move(feet);
    }
    if (!sec.extra.empty()) {
        ordered_json x = ordered_json::object();
        for (const auto& kv : sec.extra) x[kv.first] = kv.second;
        j["x"] = std::move(x);
    }
    return j;
}

ordered_json imageJson(const ImageSpec& im) {
    ordered_json j = ordered_json::object();
    if (!im.resource_id.empty()) j["res"] = im.resource_id;
    if (im.px_w != 0) j["pw"] = im.px_w;
    if (im.px_h != 0) j["ph"] = im.px_h;
    if (im.dpi != 96) j["dpi"] = im.dpi;
    if (im.width != Length{}) j["w"] = printLength(im.width);
    if (im.height != Length{}) j["h"] = printLength(im.height);
    if (im.anchor != ImageAnchor::Inline) j["anchor"] = imageAnchorName(im.anchor);
    if (im.wrap != ImageWrap::None) j["wrap"] = imageWrapName(im.wrap);
    return j;
}

ordered_json formulaJson(const FormulaSpec& f) {
    ordered_json j = ordered_json::object();
    if (!f.expr.empty()) j["e"] = f.expr;
    if (f.backend != FormulaBackend::Placeholder) j["be"] = formulaBackendName(f.backend);
    if (f.width != Length{}) j["w"] = printLength(f.width);
    if (f.height != Length{}) j["h"] = printLength(f.height);
    return j;
}

/**
 * 一个节点的条目。只写与默认不同的字段，孩子个数永远写（它是结构）。
 */
ordered_json nodeJson(const Node& n, Strings* kinds, Strings* strings) {
    ordered_json j = ordered_json::object();
    j[kId] = n.id;
    j[kKind] = kinds->add(nodeKindName(n.kind));
    j[kCount] = n.children.size();
    if (n.hidden) j[kHidden] = true;
    if (!n.text.empty()) j[kText] = n.text;
    if (!n.para.empty()) j[kPara] = paraJson(n.para);
    if (!n.styleRef.empty()) j[kStyle] = strings->add(n.styleRef);
    if (!n.marks.empty()) j[kMarks] = marksJson(n.marks);
    if (!(n.control == ControlSpec{})) j[kControl] = controlJson(n.control, strings);
    if (!(n.cell == CellSpan{})) j[kCell] = ordered_json::array({n.cell.row_span, n.cell.col_span});
    if (!(n.table == TableSpec{})) j[kTable] = tableJson(n.table);
    if (!(n.field == FieldSpec{})) j[kField] = fieldJson(n.field);
    if (!n.section.sameAs(SectionSpec{})) j[kSection] = sectionJson(n.section);
    if (!(n.image == ImageSpec{})) j[kImage] = imageJson(n.image);
    if (!(n.formula == FormulaSpec{})) j[kFormula] = formulaJson(n.formula);
    return j;
}

/**
 * 收集顺序：正文根优先，然后逐节的页眉页脚片段（按变体顺序）。顺序固定，
 * 于是同一棵树写出来的文本逐字节相同。
 */
void collectRoots(const NodeTree& tree, std::vector<NodeId>* out) {
    out->push_back(tree.rootId());
    for (int i = 0; i < tree.sectionCount(); ++i) {
        const Node* sec = tree.find(tree.sectionAt(i));
        if (sec == nullptr) continue;
        for (int v = 0; v < kHeaderFooterVariantCount; ++v) {
            const NodeId h = sec->section.headers[v].root;
            const NodeId f = sec->section.footers[v].root;
            if (h != kInvalidNodeId) out->push_back(h);
            if (f != kInvalidNodeId) out->push_back(f);
        }
    }
}

/**
 * 按深度优先序把一棵子树拍平。孩子个数已写进条目，读的时候按它切分。
 */
void flatten(const NodeTree& tree, NodeId id, Strings* kinds, Strings* strings,
             ordered_json* nodes) {
    const Node* n = tree.find(id);
    if (n == nullptr) return;
    nodes->push_back(nodeJson(*n, kinds, strings));
    for (const Node* c : n->children) flatten(tree, c->id, kinds, strings, nodes);
}

/**
 * 读端：符号表 + 错误串。任何一处读不下去都返回 false 并写明位置，不猜、不截断。
 *
 * 缺失即默认值（这是"默认值不写"的另一面），类型不对才是错 —— 这个区分贯穿下面所有
 * read 函数：contains 为假就跳过，存在但类型不符就报错。
 */
class Reader {
public:
    bool bind(const ordered_json& root, std::string* error) {
        err_ = error;
        if (!root.is_object()) return fail("顶层不是对象");
        if (!root.contains(kV) || !root[kV].is_number_integer()) return fail("缺少版本号");
        if (root[kV].get<int>() != kSerializeFormatVersion) {
            return fail("版本号不是 " + std::to_string(kSerializeFormatVersion));
        }
        if (!root.contains(kSym) || !root[kSym].is_object()) return fail("缺少符号表");
        const ordered_json& sym = root[kSym];
        if (!sym.contains(kKinds) || !sym[kKinds].is_array()) return fail("符号表缺少种类名");
        if (!sym.contains(kStrings) || !sym[kStrings].is_array()) return fail("符号表缺少字符串");
        std::vector<std::string> kind_names;
        if (!readStringArray(sym[kKinds], &kind_names)) return fail("种类名表读不出来");
        if (!readStringArray(sym[kStrings], &strings_)) return fail("字符串表读不出来");
        for (const std::string& name : kind_names) {
            NodeKind kind = NodeKind::TextRun;
            if (!nodeKindFromName(name, &kind)) return fail("不认识的节点种类名 " + name);
            kinds_.push_back(kind);
        }
        if (!root.contains(kNodes) || !root[kNodes].is_array()) return fail("缺少节点表");
        if (!root.contains(kRoots) || !root[kRoots].is_array()) return fail("缺少根列表");
        if (!root.contains(kNext) || !root[kNext].is_number_integer()) {
            return fail("缺少分配器水位");
        }
        nodes_ = root[kNodes];
        roots_ = root[kRoots];
        next_id_ = root[kNext].get<NodeId>();
        return true;
    }

    bool fail(const std::string& what) const {
        if (err_ != nullptr && err_->empty()) *err_ = what;
        return false;
    }

    const ordered_json& nodeAt(std::size_t i) const { return nodes_.at(i); }
    std::size_t nodeCount() const { return nodes_.size(); }
    const std::vector<NodeKind>& kinds() const { return kinds_; }
    NodeId nextId() const { return next_id_; }
    const ordered_json& roots() const { return roots_; }

    bool stringAt(int index, std::string* out) const {
        if (index < 0 || index >= static_cast<int>(strings_.size())) return fail("字符串下标越界");
        *out = strings_[static_cast<std::size_t>(index)];
        return true;
    }

private:
    bool readStringArray(const ordered_json& arr, std::vector<std::string>* out) {
        for (const ordered_json& item : arr) {
            if (!item.is_string()) return false;
            out->push_back(item.get<std::string>());
        }
        return true;
    }

    std::string* err_ = nullptr;
    ordered_json nodes_;
    ordered_json roots_;
    std::vector<NodeKind> kinds_;
    std::vector<std::string> strings_;
    NodeId next_id_ = 1;
};

bool readInt(const ordered_json& j, const char* key, int* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) return false;
    *out = j[key].get<int>();
    return true;
}

bool readSize(const ordered_json& j, const char* key, std::size_t* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer() && !j[key].is_number_unsigned()) return false;
    const long long v = j[key].get<long long>();
    if (v < 0) return false;
    *out = static_cast<std::size_t>(v);
    return true;
}

bool readBool(const ordered_json& j, const char* key, bool* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_boolean()) return false;
    *out = j[key].get<bool>();
    return true;
}

bool readString(const ordered_json& j, const char* key, std::string* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    *out = j[key].get<std::string>();
    return true;
}

bool readIntArray(const ordered_json& j, const char* key, std::vector<int>* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return false;
    std::vector<int> got;
    for (const ordered_json& item : j[key]) {
        if (!item.is_number_integer()) return false;
        got.push_back(item.get<int>());
    }
    *out = std::move(got);
    return true;
}

bool readU32Array(const ordered_json& j, const char* key, std::vector<uint32_t>* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return false;
    std::vector<uint32_t> got;
    for (const ordered_json& item : j[key]) {
        if (!item.is_number_integer()) return false;
        const long long v = item.get<long long>();
        if (v < 0) return false;
        got.push_back(static_cast<uint32_t>(v));
    }
    *out = std::move(got);
    return true;
}

bool readStringArrayValue(const ordered_json& j, const char* key, std::vector<std::string>* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return false;
    std::vector<std::string> got;
    for (const ordered_json& item : j[key]) {
        if (!item.is_string()) return false;
        got.push_back(item.get<std::string>());
    }
    *out = std::move(got);
    return true;
}

bool readLength(const ordered_json& j, const char* key, Length* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    return parseLength(j[key].get<std::string>(), out);
}

bool readColor(const ordered_json& j, const char* key, Color* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array() || j[key].size() != 4) return false;
    int parts[4] = {0, 0, 0, 255};
    for (int i = 0; i < 4; ++i) {
        if (!j[key][static_cast<std::size_t>(i)].is_number_integer()) return false;
        parts[i] = j[key][static_cast<std::size_t>(i)].get<int>();
    }
    for (int i = 0; i < 4; ++i) {
        if (parts[i] < 0 || parts[i] > 255) return false;
    }
    *out = Color{static_cast<uint8_t>(parts[0]), static_cast<uint8_t>(parts[1]),
                 static_cast<uint8_t>(parts[2]), static_cast<uint8_t>(parts[3])};
    return true;
}

bool readWritingMode(const ordered_json& j, const char* key, WritingMode* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array() || j[key].size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!j[key][static_cast<std::size_t>(i)].is_number_integer()) return false;
    }
    const int axis = j[key][0].get<int>();
    const int flow = j[key][1].get<int>();
    const int orient = j[key][2].get<int>();
    if (axis < 0 || axis > 1 || flow < 0 || flow > 2 || orient < 0 || orient > 2) return false;
    out->inline_axis = static_cast<InlineAxis>(axis);
    out->block_flow = static_cast<BlockFlow>(flow);
    out->orientation = static_cast<TextOrientation>(orient);
    return true;
}

template <typename T>
bool readEnum(const ordered_json& j, const char* key, bool (*from)(const std::string&, T*),
              T* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    return from(j[key].get<std::string>(), out);
}

bool readFloat(const ordered_json& j, const char* key, float* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    return parseFloatText(j[key].get<std::string>(), out);
}

bool readPara(const ordered_json& j, const char* key, ParaFormatPatch* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& p = j[key];
    if (p.contains("fi")) {
        int v = 0;
        if (!readInt(p, "fi", &v)) return false;
        out->font_index = v;
    }
    if (p.contains("sz")) {
        Length len;
        if (!readLength(p, "sz", &len)) return false;
        out->size = len;
    }
    if (p.contains("wm")) {
        WritingMode wm;
        if (!readWritingMode(p, "wm", &wm)) return false;
        out->wm = wm;
    }
    if (p.contains("lf")) {
        float v = 0.0f;
        if (!readFloat(p, "lf", &v)) return false;
        out->line_factor = v;
    }
    if (p.contains("sa")) {
        Length len;
        if (!readLength(p, "sa", &len)) return false;
        out->space_after = len;
    }
    if (p.contains("pb")) {
        bool v = false;
        if (!readBool(p, "pb", &v)) return false;
        out->page_break_before = v;
    }
    if (p.contains("lang")) {
        std::string tag;
        if (!readString(p, "lang", &tag)) return false;
        out->language = internLanguage(tag);
    }
    if (p.contains("col")) {
        Color c;
        if (!readColor(p, "col", &c)) return false;
        out->color = c;
    }
    if (p.contains("fb")) {
        std::vector<int> fb;
        if (!readIntArray(p, "fb", &fb)) return false;
        out->fallback_font_indices = fb;
    }
    return true;
}

bool readMarks(const ordered_json& j, const char* key, CharFormat* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& m = j[key];
    if (m.contains("b")) {
        bool v = false;
        if (!readBool(m, "b", &v)) return false;
        out->bold = v;
    }
    if (m.contains("i")) {
        bool v = false;
        if (!readBool(m, "i", &v)) return false;
        out->italic = v;
    }
    if (m.contains("u")) {
        bool v = false;
        if (!readBool(m, "u", &v)) return false;
        out->underline = v;
    }
    if (m.contains("fi")) {
        int v = 0;
        if (!readInt(m, "fi", &v)) return false;
        out->font_index = v;
    }
    if (m.contains("sz")) {
        Length len;
        if (!readLength(m, "sz", &len)) return false;
        out->size = len;
    }
    if (m.contains("col")) {
        Color c;
        if (!readColor(m, "col", &c)) return false;
        out->color = c;
    }
    return true;
}

bool readBorderSide(const ordered_json& j, const char* key, BorderSide* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& s = j[key];
    if (s.contains("on")) {
        bool v = false;
        if (!readBool(s, "on", &v)) return false;
        out->on = v;
    }
    if (!readEnum(s, "st", borderStyleFromName, &out->style)) return false;
    if (s.contains("w")) {
        Length len;
        if (!readLength(s, "w", &len)) return false;
        out->width = len;
    }
    if (s.contains("col")) {
        Color c;
        if (!readColor(s, "col", &c)) return false;
        out->color = c;
    }
    return true;
}

bool readAppearance(const Reader& r, const ordered_json& j, const char* key, Appearance* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return r.fail("control.ap 不是对象");
    const ordered_json& a = j[key];
    if (a.contains("w")) {
        Length len;
        if (!readLength(a, "w", &len)) return r.fail("外观宽度读不出来");
        out->width = len;
    }
    if (!readEnum(a, "ov", overflowFromName, &out->overflow)) {
        return r.fail("不认识的 overflow 名");
    }
    if (!readEnum(a, "ul", underlineFromName, &out->underline)) {
        return r.fail("不认识的 underline 名");
    }
    if (a.contains("bd")) {
        if (!a["bd"].is_object()) return r.fail("外观边框不是对象");
        const ordered_json& b = a["bd"];
        if (!readBorderSide(b, "l", &out->border.left)) return r.fail("左边框读不出来");
        if (!readBorderSide(b, "r", &out->border.right)) return r.fail("右边框读不出来");
        if (!readBorderSide(b, "t", &out->border.top)) return r.fail("上边框读不出来");
        if (!readBorderSide(b, "b", &out->border.bottom)) return r.fail("下边框读不出来");
    }
    if (a.contains("gl")) {
        if (!a["gl"].is_object()) return r.fail("图元不是对象");
        const ordered_json& g = a["gl"];
        if (!readString(g, "kind", &out->glyph.kind)) return r.fail("图元名不是字符串");
        if (g.contains("w")) {
            Length len;
            if (!readLength(g, "w", &len)) return r.fail("图元线宽读不出来");
            out->glyph.width = len;
        }
        if (g.contains("sz")) {
            Length len;
            if (!readLength(g, "sz", &len)) return r.fail("图元尺寸读不出来");
            out->glyph.size = len;
        }
        if (g.contains("col")) {
            Color c;
            if (!readColor(g, "col", &c)) return r.fail("图元颜色读不出来");
            out->glyph.color = c;
        }
        if (g.contains("fill")) {
            Color c;
            if (!readColor(g, "fill", &c)) return r.fail("图元填充读不出来");
            out->glyph.fill = c;
        }
    }
    if (!readEnum(a, "al", alignFromName, &out->align)) return r.fail("不认识的 align 名");
    if (!readString(a, "ph", &out->placeholder)) return r.fail("提示文字不是字符串");
    int index = 0;
    if (a.contains("sr")) {
        if (!readInt(a, "sr", &index)) return r.fail("外观样式引用不是下标");
        if (!r.stringAt(index, &out->styleRef)) return false;
    }
    if (a.contains("isr")) {
        if (!readInt(a, "isr", &index)) return r.fail("外观无效样式引用不是下标");
        if (!r.stringAt(index, &out->invalidStyleRef)) return false;
    }
    return true;
}

bool readControl(const Reader& r, const ordered_json& j, const char* key, ControlSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return r.fail("control 不是对象");
    const ordered_json& c = j[key];
    int index = 0;
    if (c.contains("key")) {
        if (!readInt(c, "key", &index)) return r.fail("control.key 不是下标");
        if (!r.stringAt(index, &out->key)) return false;
    }
    if (c.contains("ct")) {
        if (!readInt(c, "ct", &index)) return r.fail("control.ct 不是下标");
        if (!r.stringAt(index, &out->controlType)) return false;
    }
    if (!readEnum(c, "vs", valueShapeFromName, &out->valueShape)) {
        return r.fail("不认识的值形态名");
    }
    if (c.contains("val")) {
        if (!c["val"].is_object()) return r.fail("control.val 不是对象");
        const ordered_json& v = c["val"];
        if (!readBool(v, "flag", &out->value.flag)) return r.fail("val.flag 不是布尔");
        if (!readString(v, "text", &out->value.text)) return r.fail("val.text 不是字符串");
        if (!readStringArrayValue(v, "items", &out->value.items)) {
            return r.fail("val.items 不是字符串数组");
        }
    }
    if (!readBool(c, "ro", &out->readonly)) return r.fail("control.ro 不是布尔");
    if (!readBool(c, "rq", &out->required)) return r.fail("control.rq 不是布尔");
    if (!readBool(c, "lk", &out->structureLock)) return r.fail("control.lk 不是布尔");
    if (!readAppearance(r, c, "ap", &out->appearance)) return false;
    if (c.contains("vr")) {
        if (!readInt(c, "vr", &index)) return r.fail("control.vr 不是下标");
        if (!r.stringAt(index, &out->validateRef)) return false;
    }
    return true;
}

bool readTable(const ordered_json& j, const char* key, TableSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& t = j[key];
    if (t.contains("cols")) {
        if (!t["cols"].is_array()) return false;
        for (const ordered_json& cj : t["cols"]) {
            if (!cj.is_object()) return false;
            TableColumn col;
            if (!readString(cj, "fr", &col.field_ref)) return false;
            if (!readString(cj, "ti", &col.title)) return false;
            if (!readString(cj, "dt", &col.data_type)) return false;
            if (!readString(cj, "es", &col.editor_style)) return false;
            if (cj.contains("w")) {
                Length len;
                if (!readLength(cj, "w", &len)) return false;
                col.width = len;
            }
            if (!readEnum(cj, "al", alignFromName, &col.align)) return false;
            out->columns.push_back(std::move(col));
        }
    }
    if (t.contains("grid")) {
        if (!t["grid"].is_array()) return false;
        for (const ordered_json& item : t["grid"]) {
            if (!item.is_string()) return false;
            Length len;
            if (!parseLength(item.get<std::string>(), &len)) return false;
            out->grid_cols.push_back(len);
        }
    }
    if (!readInt(t, "hr", &out->header_rows)) return false;
    return true;
}

bool readField(const ordered_json& j, const char* key, FieldSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& f = j[key];
    if (!readEnum(f, "kind", fieldKindFromName, &out->kind)) return false;
    if (!readString(f, "name", &out->name)) return false;
    if (!readString(f, "cache", &out->cache)) return false;
    return true;
}

bool readHeaderFooter(const ordered_json& arr, int variant, HeaderFooter* out) {
    if (!arr.is_array() || variant < 0 || variant >= static_cast<int>(arr.size())) return false;
    const ordered_json& j = arr[static_cast<std::size_t>(variant)];
    if (!j.is_object()) return false;
    if (j.contains("root")) {
        if (!j["root"].is_number_integer() && !j["root"].is_number_unsigned()) return false;
        out->root = j["root"].get<NodeId>();
    }
    if (j.contains("dist")) {
        Length len;
        if (!readLength(j, "dist", &len)) return false;
        out->distance = len;
    }
    if (!readBool(j, "link", &out->linked_to_previous)) return false;
    return true;
}

bool readSection(const ordered_json& j, const char* key, SectionSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& s = j[key];
    if (s.contains("paper")) {
        if (!s["paper"].is_object()) return false;
        const ordered_json& p = s["paper"];
        if (!readEnum(p, "name", paperNameFromName, &out->paper.name)) return false;
        if (!readEnum(p, "orient", orientationFromName, &out->paper.orient)) return false;
        if (p.contains("w")) {
            Length len;
            if (!readLength(p, "w", &len)) return false;
            out->paper.width = len;
        }
        if (p.contains("h")) {
            Length len;
            if (!readLength(p, "h", &len)) return false;
            out->paper.height = len;
        }
    }
    if (s.contains("mar")) {
        if (!s["mar"].is_object()) return false;
        const ordered_json& m = s["mar"];
        if (m.contains("t")) {
            if (!readLength(m, "t", &out->margins.top)) return false;
        }
        if (m.contains("b")) {
            if (!readLength(m, "b", &out->margins.bottom)) return false;
        }
        if (m.contains("l")) {
            if (!readLength(m, "l", &out->margins.left)) return false;
        }
        if (m.contains("r")) {
            if (!readLength(m, "r", &out->margins.right)) return false;
        }
    }
    if (s.contains("gut")) {
        if (!s["gut"].is_object()) return false;
        const ordered_json& g = s["gut"];
        if (g.contains("sz")) {
            if (!readLength(g, "sz", &out->gutter.size)) return false;
        }
        if (g.contains("side")) {
            int side = 0;
            if (!readInt(g, "side", &side)) return false;
            if (side != 0 && side != 1) return false;
            out->gutter.side = static_cast<Gutter::Side>(side);
        }
    }
    if (!readBool(s, "mir", &out->mirror_margins)) return false;
    if (!readEnum(s, "brk", sectionBreakFromName, &out->break_kind)) return false;
    if (!readEnum(s, "va", verticalAlignFromName, &out->valign)) return false;
    if (!readWritingMode(s, "wm", &out->writing_mode)) return false;
    if (s.contains("num")) {
        if (!s["num"].is_object()) return false;
        const ordered_json& n = s["num"];
        if (!readEnum(n, "fmt", pageNumberFormatFromName, &out->numbering.format)) return false;
        if (!readInt(n, "start", &out->numbering.start)) return false;
        if (!readBool(n, "restart", &out->numbering.restart)) return false;
    }
    if (s.contains("hd")) {
        if (!readHeaderFooter(s["hd"], 0, &out->headers[0])) return false;
        if (!readHeaderFooter(s["hd"], 1, &out->headers[1])) return false;
        if (!readHeaderFooter(s["hd"], 2, &out->headers[2])) return false;
    }
    if (s.contains("ft")) {
        if (!readHeaderFooter(s["ft"], 0, &out->footers[0])) return false;
        if (!readHeaderFooter(s["ft"], 1, &out->footers[1])) return false;
        if (!readHeaderFooter(s["ft"], 2, &out->footers[2])) return false;
    }
    if (s.contains("x")) {
        if (!s["x"].is_object()) return false;
        for (auto it = s["x"].begin(); it != s["x"].end(); ++it) {
            if (!it.value().is_string()) return false;
            out->extra[it.key()] = it.value().get<std::string>();
        }
    }
    return true;
}

bool readImage(const ordered_json& j, const char* key, ImageSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& im = j[key];
    if (!readString(im, "res", &out->resource_id)) return false;
    if (!readInt(im, "pw", &out->px_w)) return false;
    if (!readInt(im, "ph", &out->px_h)) return false;
    if (!readInt(im, "dpi", &out->dpi)) return false;
    if (!readLength(im, "w", &out->width)) return false;
    if (!readLength(im, "h", &out->height)) return false;
    if (!readEnum(im, "anchor", imageAnchorFromName, &out->anchor)) return false;
    if (!readEnum(im, "wrap", imageWrapFromName, &out->wrap)) return false;
    return true;
}

bool readFormula(const ordered_json& j, const char* key, FormulaSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& f = j[key];
    if (!readString(f, "e", &out->expr)) return false;
    if (!readEnum(f, "be", formulaBackendFromName, &out->backend)) return false;
    if (!readLength(f, "w", &out->width)) return false;
    if (!readLength(f, "h", &out->height)) return false;
    return true;
}

/**
 * 一个节点的全部字段（结构之外的部分）。返回 false 时 error 已写明位置。
 */
bool readNodeFields(const Reader& r, const ordered_json& entry, NodeTree* out, NodeId id) {
    Node* n = out->find(id);
    if (n == nullptr) return r.fail("节点没有建出来");
    if (!readBool(entry, kHidden, &n->hidden)) return r.fail("hidden 不是布尔");
    if (!readPara(entry, kPara, &n->para)) return r.fail("段落格式读不出来");
    if (!readMarks(entry, kMarks, &n->marks)) return r.fail("字符属性读不出来");
    if (!readControl(r, entry, kControl, &n->control)) return false;
    if (!readTable(entry, kTable, &n->table)) return r.fail("表格记录读不出来");
    if (!readField(entry, kField, &n->field)) return r.fail("域记录读不出来");
    if (!readSection(entry, kSection, &n->section)) return r.fail("节记录读不出来");
    if (!readImage(entry, kImage, &n->image)) return r.fail("图片记录读不出来");
    if (!readFormula(entry, kFormula, &n->formula)) return r.fail("公式记录读不出来");
    if (entry.contains(kStyle)) {
        int index = 0;
        if (!readInt(entry, kStyle, &index)) return r.fail("样式引用不是下标");
        if (!r.stringAt(index, &n->styleRef)) return false;
    }
    if (entry.contains(kCell)) {
        if (!entry[kCell].is_array() || entry[kCell].size() != 2) {
            return r.fail("跨度不是两个整数");
        }
        int spans[2] = {1, 1};
        for (int i = 0; i < 2; ++i) {
            if (!entry[kCell][static_cast<std::size_t>(i)].is_number_integer()) {
                return r.fail("跨度不是两个整数");
            }
            spans[i] = entry[kCell][static_cast<std::size_t>(i)].get<int>();
        }
        n->cell.row_span = spans[0];
        n->cell.col_span = spans[1];
    }
    if (entry.contains(kText)) {
        if (!isText(n->kind)) return r.fail("非文本节点带文本");
        std::vector<uint32_t> cps;
        if (!readU32Array(entry, kText, &cps)) return r.fail("文本不是码点数组");
        out->setText(id, std::move(cps));
    }
    return true;
}

}  // namespace

SerializeResult serializeDocument(const NodeTree& tree) {
    SerializeResult result;
    std::vector<NodeId> roots;
    collectRoots(tree, &roots);

    Strings kinds;
    Strings strings;
    ordered_json nodes = ordered_json::array();
    for (NodeId id : roots) flatten(tree, id, &kinds, &strings, &nodes);

    ordered_json sym = ordered_json::object();
    sym[kKinds] = kinds.list();
    sym[kStrings] = strings.list();

    ordered_json root_ids = ordered_json::array();
    for (NodeId id : roots) root_ids.push_back(id);

    ordered_json doc = ordered_json::object();
    doc[kV] = kSerializeFormatVersion;
    doc[kSym] = std::move(sym);
    doc[kRoots] = std::move(root_ids);
    doc[kNodes] = std::move(nodes);
    doc[kNext] = tree.maxAllocatedId() + 1;

    // 先自查一遍再写：库对非法 UTF-8 的处理是抛异常或终止进程，两条路都不该走到。
    // 查到就返回失败，调用方知道这份文档没写出去。
    if (!validateStrings(doc, &result.error)) return result;
    result.text = doc.dump();
    result.ok = true;
    return result;
}

DeserializeResult deserializeDocument(const std::string& text, NodeTree* out) {
    DeserializeResult result;
    if (out == nullptr) {
        result.error = "目标树为空";
        return result;
    }

    // 解析失败不靠异常：wasm 构建关掉了异常，库在那种环境下的错误处理是终止进程。
    // allow_exceptions 传 false 时它把失败记成 discarded 值，由这里判。
    const ordered_json doc = ordered_json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        result.error = "JSON 解析失败（不是合法 JSON，或含无法解析的内容）";
        return result;
    }

    Reader reader;
    if (!reader.bind(doc, &result.error)) return result;

    /**
     * 节点表按深度优先序拍平，所以一个节点自己占一条、它后面紧跟的是它的整棵子树。
     * 切分靠孩子个数递归下去，切完一段就知道这一段里每个节点的孩子是谁。
     */
    struct Plan {
        NodeId id = kInvalidNodeId;
        NodeKind kind = NodeKind::TextRun;
        std::size_t index = 0;
        std::vector<NodeId> children;
    };
    std::vector<Plan> plan;
    std::size_t cursor = 0;

    std::function<bool(NodeId)> slice = [&](NodeId expected) -> bool {
        if (cursor >= reader.nodeCount()) return reader.fail("节点表在读完之前就结束了");
        const ordered_json& entry = reader.nodeAt(cursor);
        if (!entry.is_object()) return reader.fail("节点条目不是对象");
        if (!entry.contains(kId)) return reader.fail("节点条目缺少 id");
        if (!entry[kId].is_number_integer() && !entry[kId].is_number_unsigned()) {
            return reader.fail("节点 id 不是整数");
        }
        const NodeId id = entry[kId].get<NodeId>();
        if (id != expected) {
            return reader.fail("节点表的顺序与根列表对不上（期望 id " + std::to_string(expected) +
                               "，读到 " + std::to_string(id) + "）");
        }
        int kind_index = -1;
        if (!readInt(entry, kKind, &kind_index)) return reader.fail("节点种类下标不是整数");
        if (kind_index < 0 || kind_index >= static_cast<int>(reader.kinds().size())) {
            return reader.fail("节点种类下标越界");
        }
        std::size_t count = 0;
        if (!readSize(entry, kCount, &count)) return reader.fail("孩子个数不是整数");

        const std::size_t here = plan.size();
        Plan p;
        p.id = id;
        p.kind = reader.kinds()[static_cast<std::size_t>(kind_index)];
        p.index = cursor;
        plan.push_back(std::move(p));
        ++cursor;

        for (std::size_t i = 0; i < count; ++i) {
            if (cursor >= reader.nodeCount()) return reader.fail("孩子个数超出了节点表");
            const ordered_json& child = reader.nodeAt(cursor);
            if (!child.is_object() || !child.contains(kId)) {
                return reader.fail("孩子条目缺少 id");
            }
            if (!child[kId].is_number_integer() && !child[kId].is_number_unsigned()) {
                return reader.fail("孩子 id 不是整数");
            }
            const NodeId child_id = child[kId].get<NodeId>();
            if (!slice(child_id)) return false;
            plan[here].children.push_back(child_id);
        }
        return true;
    };

    for (const ordered_json& item : reader.roots()) {
        if (!item.is_number_integer() && !item.is_number_unsigned()) {
            result.error = "根列表里有非整数";
            return result;
        }
        if (!slice(item.get<NodeId>())) return result;
    }
    if (cursor != reader.nodeCount()) {
        result.error = "节点表里有多余的条目";
        return result;
    }
    if (plan.empty()) {
        result.error = "节点表是空的";
        return result;
    }

    // 先在临时树上装好，成功了再整体交给 out —— 读坏了 out 一个字节都不动，
    // 调用方拿到的失败回执与"文档没变"是同一件事。
    NodeTree built;
    built.reset();
    if (built.rootId() != plan[0].id) {
        result.error = "根 id 与新建文档的根不符";
        return result;
    }
    for (const Plan& p : plan) {
        if (p.id == built.rootId()) continue;
        if (built.createDetachedWithId(p.id, p.kind) == kInvalidNodeId) {
            result.error = "建节点失败（id 撞车或无效）：" + std::to_string(p.id);
            return result;
        }
    }
    for (const Plan& p : plan) {
        for (std::size_t i = 0; i < p.children.size(); ++i) {
            if (built.attachChild(p.id, static_cast<int>(i), p.children[i]) == kInvalidNodeId) {
                result.error = "挂孩子失败（成环或已在树上）：" + std::to_string(p.children[i]);
                return result;
            }
        }
    }
    for (const Plan& p : plan) {
        if (!readNodeFields(reader, reader.nodeAt(p.index), &built, p.id)) return result;
    }
    // 文件里的水位可能比现存的最大 id 还大（保存前删过节点），所以照记录推进分配器，
    // 否则重新打开之后新分配的 id 会与文件里引用过的 id 撞上。
    built.reserveIdWatermark(reader.nextId());

    out->assignFrom(built);
    result.ok = true;
    return result;
}

}  // namespace heditor
