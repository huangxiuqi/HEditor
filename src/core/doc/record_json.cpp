#include "core/doc/record_json.h"

#include <cstdio>
#include <cstdlib>

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
    return true;
}

}  // namespace heditor::recjson
