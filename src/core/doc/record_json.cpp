#include "core/doc/record_json.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "core/doc/length.h"

namespace heditor::recjson {

int StringPool::add(const std::string& s) {
    const auto it = index_.find(s);
    if (it != index_.end()) return it->second;
    const int id = static_cast<int>(list_.size());
    list_.push_back(s);
    index_.emplace(s, id);
    return id;
}

std::string floatText(float v) {
    // 最短表示而不是 %.9g：同一个 float 的十进制文本只有一种，而且是最短的那一种。
    //   往返成立由 to_chars 保证（它取的是能唯一还原这个 float 的最短串）。
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

bool parseFloatText(const std::string& text, float* out) {
    if (out == nullptr || text.empty()) return false;
    char* end = nullptr;
    const float v = std::strtof(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    *out = v;
    return true;
}

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
    if (p.line_rule.has_value()) j["lr"] = lineSpacingRuleName(*p.line_rule);
    if (p.line_240.has_value()) j["lf"] = *p.line_240;
    if (p.line_exact.has_value()) j["lx"] = printLength(*p.line_exact);
    if (p.space_before.has_value()) j["sb"] = printLength(*p.space_before);
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

ordered_json appearanceJson(const Appearance& a, StringPool* strings) {
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

/**
 * 选项表写成 [[值, 标签], ...]。数组对而不是对象：两个字段、顺序固定，落盘更短。
 * @param options 选项表
 * @return JSON 数组
 */
ordered_json optionsJson(const std::vector<ControlOption>& options) {
    ordered_json arr = ordered_json::array();
    for (const ControlOption& o : options) {
        ordered_json pair = ordered_json::array();
        pair.push_back(o.value);
        pair.push_back(o.label);
        arr.push_back(std::move(pair));
    }
    return arr;
}

/**
 * 类型特有层写成一个带 k 标签的对象。没有特有层时返回空对象（调用方不写这个键）。
 * @param c 控件记录
 * @return JSON 对象
 */
ordered_json detailJson(const ControlSpec& c) {
    ordered_json j = ordered_json::object();
    if (const auto* d = textOf(c)) {
        // 有文本特有层就写下来。Any 也要写：不写的话"有这一层但不过滤"落盘会变成
        //   "没有这一层"，读回来 operator== 就不等了 —— 往返不变的要求当场破掉。
        j["k"] = (d->filter == ContentFilter::Number) ? "number" : "text";
        return j;
    }
    if (const auto* d = choiceOf(c)) {
        j["k"] = "choice";
        if (!d->label.empty()) j["lbl"] = d->label;
        if (!d->options.empty()) j["opt"] = optionsJson(d->options);
        if (!d->group.empty()) j["grp"] = d->group;
        if (d->multi) j["multi"] = true;
        // caret_barrier 的默认是 true，只在关掉时写（默认值不写这条最容易漏）。
        if (!d->caret_barrier) j["cb"] = false;
        return j;
    }
    if (const auto* d = dropdownOf(c)) {
        j["k"] = "dropdown";
        if (!d->options.empty()) j["opt"] = optionsJson(d->options);
        if (d->free_input) j["fi"] = true;
        return j;
    }
    if (const auto* d = dateOf(c)) {
        j["k"] = "date";
        if (!d->format.empty()) j["fmt"] = d->format;
        if (d->units != kDateUnitsAll) j["u"] = static_cast<int>(d->units);
        return j;
    }
    if (const auto* d = customOf(c)) {
        j["k"] = "custom";
        if (!d->props.empty()) {
            ordered_json p = ordered_json::object();
            for (const auto& kv : d->props) p[kv.first] = kv.second;
            j["p"] = std::move(p);
        }
        return j;
    }
    return ordered_json::object();
}

ordered_json controlJson(const ControlSpec& c, StringPool* strings) {
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
    const ordered_json d = detailJson(c);
    if (!d.empty()) j["d"] = d;
    return j;
}

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

bool readFloat(const ordered_json& j, const char* key, float* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    return parseFloatText(j[key].get<std::string>(), out);
}

/**
 * 读倍数行距的刻度值（240 分之一行，240 = 单倍）。
 *
 * 只收整数：倍数与 OOXML 的 w:line 同口径，1.3 倍的线上形式是 312。收十进制小数会让
 * 同一个值有两种写法，而小数一旦落进浮点就回不到整数刻度，再写出去就是另一个数。
 * @param j 对象
 * @param key 键
 * @param out 写回刻度值
 * @return true = 缺键或合法；false = 类型不对或超出范围
 */
bool readLineFactor(const ordered_json& j, const char* key, int* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) return false;
    const long long v = j[key].get<long long>();
    if (v < 0 || v > std::numeric_limits<int>::max()) return false;
    *out = static_cast<int>(v);
    return true;
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
    if (t.width != Length{}) j["w"] = printLength(t.width);
    if (t.align != Align::Start) j["al"] = alignName(t.align);
    return j;
}

/**
 * 行记录。三个开关里 can_split_by_page 的默认是真，所以只在关掉时写；
 * 另外两个默认是假，只在打开时写。行高与规则各自只在非默认时写。
 */
ordered_json rowJson(const RowSpec& r) {
    ordered_json j = ordered_json::object();
    if (!r.can_split_by_page) j["sp"] = false;
    if (r.is_header) j["hd"] = true;
    if (r.page_break_before) j["pb"] = true;
    if (!(r.height == Length{})) j["ht"] = printLength(r.height);
    if (r.height_rule != HeightRule::Auto) j["hr"] = heightRuleName(r.height_rule);
    return j;
}

ordered_json cellJson(const CellSpec& c) {
    ordered_json j = ordered_json::array({c.row_span, c.col_span});
    if (c.v_align != CellVAlign::Top) j.push_back(cellVAlignName(c.v_align));
    return j;
}

bool readCell(const ordered_json& j, const char* key, CellSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array() || j[key].size() < 2 || j[key].size() > 3) return false;
    for (int i = 0; i < 2; ++i) {
        if (!j[key][static_cast<std::size_t>(i)].is_number_integer()) return false;
    }
    out->row_span = j[key][0].get<int>();
    out->col_span = j[key][1].get<int>();
    if (j[key].size() == 3) {
        if (!j[key][2].is_string()) return false;
        if (!cellVAlignFromName(j[key][2].get<std::string>(), &out->v_align)) return false;
    }
    return true;
}

ordered_json fieldJson(const FieldSpec& f) {
    ordered_json j = ordered_json::object();
    if (f.kind != FieldKind::PageNumber) j["kind"] = fieldKindName(f.kind);
    if (!f.name.empty()) j["name"] = f.name;
    if (!f.cache.empty()) j["cache"] = f.cache;
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
    if (im.dx_pt != 0.0f) j["dx"] = floatText(im.dx_pt);
    if (im.dy_pt != 0.0f) j["dy"] = floatText(im.dy_pt);
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

bool readPara(const Context& ctx, const ordered_json& j, const char* key, ParaFormatPatch* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& p = j[key];
    if (p.contains("fi")) {
        int v = 0;
        if (!readInt(p, "fi", &v)) return false;
        out->font_index = ctx.mapFontIndex(v);
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
    if (p.contains("lr")) {
        std::string name;
        LineSpacingRule rule = LineSpacingRule::Auto;
        if (!readString(p, "lr", &name)) return false;
        if (!lineSpacingRuleFromName(name, &rule)) return false;
        out->line_rule = rule;
    }
    if (p.contains("lf")) {
        int v = 0;
        if (!readLineFactor(p, "lf", &v)) return false;
        out->line_240 = v;
    }
    if (p.contains("lx")) {
        Length len;
        if (!readLength(p, "lx", &len)) return false;
        out->line_exact = len;
    }
    if (p.contains("sb")) {
        Length len;
        if (!readLength(p, "sb", &len)) return false;
        out->space_before = len;
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
        // 回退链也是一串字体下标，与主字体同一个映射。
        for (int& index : fb) index = ctx.mapFontIndex(index);
        out->fallback_font_indices = std::move(fb);
    }
    return true;
}

bool readMarks(const Context& ctx, const ordered_json& j, const char* key, CharFormat* out) {
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
        out->font_index = ctx.mapFontIndex(v);
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

bool readAppearance(const Context& ctx, const ordered_json& j, const char* key, Appearance* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return ctx.fail("control.ap 不是对象");
    const ordered_json& a = j[key];
    if (a.contains("w")) {
        Length len;
        if (!readLength(a, "w", &len)) return ctx.fail("外观宽度读不出来");
        out->width = len;
    }
    if (!readEnum(a, "ov", overflowFromName, &out->overflow)) {
        return ctx.fail("不认识的 overflow 名");
    }
    if (!readEnum(a, "ul", underlineFromName, &out->underline)) {
        return ctx.fail("不认识的 underline 名");
    }
    if (a.contains("bd")) {
        if (!a["bd"].is_object()) return ctx.fail("外观边框不是对象");
        const ordered_json& b = a["bd"];
        if (!readBorderSide(b, "l", &out->border.left)) return ctx.fail("左边框读不出来");
        if (!readBorderSide(b, "r", &out->border.right)) return ctx.fail("右边框读不出来");
        if (!readBorderSide(b, "t", &out->border.top)) return ctx.fail("上边框读不出来");
        if (!readBorderSide(b, "b", &out->border.bottom)) return ctx.fail("下边框读不出来");
    }
    if (a.contains("gl")) {
        if (!a["gl"].is_object()) return ctx.fail("图元不是对象");
        const ordered_json& g = a["gl"];
        if (!readString(g, "kind", &out->glyph.kind)) return ctx.fail("图元名不是字符串");
        if (g.contains("w")) {
            Length len;
            if (!readLength(g, "w", &len)) return ctx.fail("图元线宽读不出来");
            out->glyph.width = len;
        }
        if (g.contains("sz")) {
            Length len;
            if (!readLength(g, "sz", &len)) return ctx.fail("图元尺寸读不出来");
            out->glyph.size = len;
        }
        if (g.contains("col")) {
            Color c;
            if (!readColor(g, "col", &c)) return ctx.fail("图元颜色读不出来");
            out->glyph.color = c;
        }
        if (g.contains("fill")) {
            Color c;
            if (!readColor(g, "fill", &c)) return ctx.fail("图元填充读不出来");
            out->glyph.fill = c;
        }
    }
    if (!readEnum(a, "al", alignFromName, &out->align)) return ctx.fail("不认识的 align 名");
    if (!readString(a, "ph", &out->placeholder)) return ctx.fail("提示文字不是字符串");
    int index = 0;
    if (a.contains("sr")) {
        if (!readInt(a, "sr", &index)) return ctx.fail("外观样式引用不是下标");
        if (!ctx.stringAt(index, &out->styleRef)) return false;
    }
    if (a.contains("isr")) {
        if (!readInt(a, "isr", &index)) return ctx.fail("外观无效样式引用不是下标");
        if (!ctx.stringAt(index, &out->invalidStyleRef)) return false;
    }
    return true;
}

/**
 * 读选项表。每一项必须是 [值, 标签] 两个字符串。
 * @param j JSON
 * @param out 写回选项表
 * @return true = 形状对
 */
bool readOptions(const ordered_json& j, std::vector<ControlOption>* out) {
    if (!j.is_array()) return false;
    std::vector<ControlOption> got;
    for (const ordered_json& item : j) {
        if (!item.is_array() || item.size() != 2) return false;
        if (!item[0].is_string() || !item[1].is_string()) return false;
        ControlOption o;
        o.value = item[0].get<std::string>();
        o.label = item[1].get<std::string>();
        got.push_back(std::move(o));
    }
    *out = std::move(got);
    return true;
}

/**
 * 读类型特有层。
 *
 * k 不认识时整份读入失败：特有层的形状由 k 决定，猜一个形状填进去等于伪造数据。
 * 与"不认识的 controlType 原样透传"不是一回事 —— 那个透传的是属性袋（CustomDetail），
 * 而这里连袋子的形状都还不知道。
 * @param ctx 读取上下文（报错用）
 * @param j 控件对象
 * @param out 写回控件记录
 * @return true = 读成功
 */
bool readDetail(const Context& ctx, const ordered_json& j, ControlSpec* out) {
    if (!j.contains("d")) return true;
    if (!j["d"].is_object()) return ctx.fail("control.d 不是对象");
    const ordered_json& d = j["d"];
    if (!d.contains("k") || !d["k"].is_string()) return ctx.fail("control.d 缺少类型标签 k");
    const std::string k = d["k"].get<std::string>();
    if (k == "number") {
        TextDetail t;
        t.filter = ContentFilter::Number;
        out->detail = t;
        return true;
    }
    if (k == "text") {
        out->detail = TextDetail{};
        return true;
    }
    if (k == "choice") {
        ChoiceDetail c;
        if (!readString(d, "lbl", &c.label)) return ctx.fail("choice.lbl 不是字符串");
        if (d.contains("opt") && !readOptions(d["opt"], &c.options)) {
            return ctx.fail("choice.opt 不是 [值, 标签] 数组");
        }
        if (!readString(d, "grp", &c.group)) return ctx.fail("choice.grp 不是字符串");
        if (!readBool(d, "multi", &c.multi)) return ctx.fail("choice.multi 不是布尔");
        if (!readBool(d, "cb", &c.caret_barrier)) return ctx.fail("choice.cb 不是布尔");
        out->detail = c;
        return true;
    }
    if (k == "dropdown") {
        DropdownDetail c;
        if (d.contains("opt") && !readOptions(d["opt"], &c.options)) {
            return ctx.fail("dropdown.opt 不是 [值, 标签] 数组");
        }
        if (!readBool(d, "fi", &c.free_input)) return ctx.fail("dropdown.fi 不是布尔");
        out->detail = c;
        return true;
    }
    if (k == "date") {
        DateDetail c;
        if (!readString(d, "fmt", &c.format)) return ctx.fail("date.fmt 不是字符串");
        int units = static_cast<int>(kDateUnitsAll);
        if (!readInt(d, "u", &units)) return ctx.fail("date.u 不是整数");
        if (units < 0 || units > 0x3F) return ctx.fail("date.u 超出六个单位的范围");
        c.units = static_cast<uint16_t>(units);
        out->detail = c;
        return true;
    }
    if (k == "custom") {
        CustomDetail c;
        if (d.contains("p")) {
            if (!d["p"].is_object()) return ctx.fail("custom.p 不是对象");
            for (auto it = d["p"].begin(); it != d["p"].end(); ++it) {
                if (!it.value().is_string()) return ctx.fail("custom.p 的值不是字符串");
                c.props[it.key()] = it.value().get<std::string>();
            }
        }
        out->detail = c;
        return true;
    }
    return ctx.fail("不认识的类型特有层标签：" + k);
}

bool readControl(const Context& ctx, const ordered_json& j, const char* key, ControlSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return ctx.fail("control 不是对象");
    const ordered_json& c = j[key];
    int index = 0;
    if (c.contains("key")) {
        if (!readInt(c, "key", &index)) return ctx.fail("control.key 不是下标");
        if (!ctx.stringAt(index, &out->key)) return false;
    }
    if (c.contains("ct")) {
        if (!readInt(c, "ct", &index)) return ctx.fail("control.ct 不是下标");
        if (!ctx.stringAt(index, &out->controlType)) return false;
    }
    if (!readEnum(c, "vs", valueShapeFromName, &out->valueShape)) {
        return ctx.fail("不认识的值形态名");
    }
    if (c.contains("val")) {
        if (!c["val"].is_object()) return ctx.fail("control.val 不是对象");
        const ordered_json& v = c["val"];
        if (!readBool(v, "flag", &out->value.flag)) return ctx.fail("val.flag 不是布尔");
        if (!readString(v, "text", &out->value.text)) return ctx.fail("val.text 不是字符串");
        if (!readStringArrayValue(v, "items", &out->value.items)) {
            return ctx.fail("val.items 不是字符串数组");
        }
    }
    if (!readBool(c, "ro", &out->readonly)) return ctx.fail("control.ro 不是布尔");
    if (!readBool(c, "rq", &out->required)) return ctx.fail("control.rq 不是布尔");
    if (!readBool(c, "lk", &out->structureLock)) return ctx.fail("control.lk 不是布尔");
    if (!readAppearance(ctx, c, "ap", &out->appearance)) return false;
    if (c.contains("vr")) {
        if (!readInt(c, "vr", &index)) return ctx.fail("control.vr 不是下标");
        if (!ctx.stringAt(index, &out->validateRef)) return false;
    }
    if (!readDetail(ctx, c, out)) return false;
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
    if (!readLength(t, "w", &out->width)) return false;
    if (!readEnum(t, "al", alignFromName, &out->align)) return false;
    return true;
}

/**
 * 行记录。键与 rowJson 一一对应；缺键即默认值（can_split_by_page 缺键是 true）。
 */
bool readRow(const ordered_json& j, const char* key, RowSpec* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_object()) return false;
    const ordered_json& r = j[key];
    if (!readBool(r, "sp", &out->can_split_by_page)) return false;
    if (!readBool(r, "hd", &out->is_header)) return false;
    if (!readBool(r, "pb", &out->page_break_before)) return false;
    if (!readLength(r, "ht", &out->height)) return false;
    if (!readEnum(r, "hr", heightRuleFromName, &out->height_rule)) return false;
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
    if (!readFloat(im, "dx", &out->dx_pt)) return false;
    if (!readFloat(im, "dy", &out->dy_pt)) return false;
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
    if (sec.watermark.enabled) {
        ordered_json w = ordered_json::object();
        w["on"] = true;
        if (!sec.watermark.text.empty()) w["t"] = sec.watermark.text;
        const WatermarkSpec kDefault;
        if (sec.watermark.size_pt != kDefault.size_pt) w["sz"] = floatText(sec.watermark.size_pt);
        if (!(sec.watermark.color == kDefault.color)) w["col"] = colorJson(sec.watermark.color);
        if (sec.watermark.angle_deg != kDefault.angle_deg)
            w["ang"] = floatText(sec.watermark.angle_deg);
        j["wat"] = std::move(w);
    }
    return j;
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
    if (s.contains("wat")) {
        if (!s["wat"].is_object()) return false;
        const ordered_json& w = s["wat"];
        if (!readBool(w, "on", &out->watermark.enabled)) return false;
        if (!readString(w, "t", &out->watermark.text)) return false;
        if (!readFloat(w, "sz", &out->watermark.size_pt)) return false;
        if (!readColor(w, "col", &out->watermark.color)) return false;
        if (!readFloat(w, "ang", &out->watermark.angle_deg)) return false;
    }
    return true;
}

}  // namespace heditor::recjson
