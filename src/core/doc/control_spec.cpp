#include "core/doc/control_spec.h"

#include "core/utils/hash.h"

#include "core/doc/document.h"

#include <string>

namespace heditor {

const char* valueShapeName(ValueShape shape) {
    switch (shape) {
        case ValueShape::Text:
            return "text";
        case ValueShape::Bool:
            return "bool";
        case ValueShape::Enum:
            return "enum";
        case ValueShape::List:
            return "list";
        case ValueShape::Resource:
            return "resource";
    }
    return "（未知）";
}

const char* controlPropName(ControlProp prop) {
    switch (prop) {
        case ControlProp::Hidden:
            return "hidden";
        case ControlProp::Readonly:
            return "readonly";
        case ControlProp::Required:
            return "required";
        case ControlProp::StructureLock:
            return "structureLock";
    }
    return "（未知）";
}

const char* overflowName(Overflow o) {
    switch (o) {
        case Overflow::Expand:
            return "expand";
        case Overflow::Clip:
            return "clip";
        case Overflow::Spill:
            return "spill";
    }
    return "（未知）";
}

const char* underlineName(Underline u) {
    switch (u) {
        case Underline::None:
            return "none";
        case Underline::Whole:
            return "whole";
        case Underline::BlankOnly:
            return "blankOnly";
    }
    return "（未知）";
}

const char* alignName(Align a) {
    switch (a) {
        case Align::Start:
            return "start";
        case Align::Center:
            return "center";
        case Align::End:
            return "end";
    }
    return "（未知）";
}

const char* borderStyleName(BorderStyle s) {
    switch (s) {
        case BorderStyle::Solid:
            return "solid";
        case BorderStyle::Dashed:
            return "dashed";
        case BorderStyle::Dotted:
            return "dotted";
    }
    return "（未知）";
}

bool borderStyleFromName(const std::string& name, BorderStyle* out) {
    if (out == nullptr) return false;
    const BorderStyle all[] = {BorderStyle::Solid, BorderStyle::Dashed, BorderStyle::Dotted};
    for (BorderStyle s : all) {
        if (name == borderStyleName(s)) {
            *out = s;
            return true;
        }
    }
    return false;
}

bool valueShapeFromName(const std::string& name, ValueShape* out) {
    if (out == nullptr) return false;
    const ValueShape all[] = {ValueShape::Text, ValueShape::Bool, ValueShape::Enum,
                              ValueShape::List, ValueShape::Resource};
    for (ValueShape s : all) {
        if (name == valueShapeName(s)) {
            *out = s;
            return true;
        }
    }
    return false;
}

bool overflowFromName(const std::string& name, Overflow* out) {
    if (out == nullptr) return false;
    const Overflow all[] = {Overflow::Expand, Overflow::Clip, Overflow::Spill};
    for (Overflow o : all) {
        if (name == overflowName(o)) {
            *out = o;
            return true;
        }
    }
    return false;
}

bool underlineFromName(const std::string& name, Underline* out) {
    if (out == nullptr) return false;
    const Underline all[] = {Underline::None, Underline::Whole, Underline::BlankOnly};
    for (Underline u : all) {
        if (name == underlineName(u)) {
            *out = u;
            return true;
        }
    }
    return false;
}

bool alignFromName(const std::string& name, Align* out) {
    if (out == nullptr) return false;
    const Align all[] = {Align::Start, Align::Center, Align::End};
    for (Align a : all) {
        if (name == alignName(a)) {
            *out = a;
            return true;
        }
    }
    return false;
}

namespace {

/**
 * 选项之间的分隔符。命中折选项时按同一份宽度算（见 controlOptionSpans），改这里要一起改。
 */
constexpr const char* kOptionGap = "  ";

/**
 * 两位补零的数字。日期格式里只有 2 位与 4 位两种宽度，不引 iostream 的格式化。
 * @param v 数值（0..9999）
 * @param width 宽度
 * @return 补零后的文本
 */
std::string padded(int v, int width) {
    std::string s = std::to_string(v);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}

/**
 * 把 ISO 串拆成六个字段。解析不了返回 false —— 调用方要么原样输出、要么拒绝。
 * @param iso ISO 串
 * @param y 年
 * @param mo 月
 * @param d 日
 * @param h 时
 * @param mi 分
 * @param se 秒
 * @return true = 形状与范围都对
 */
bool splitIso(const std::string& iso, int* y, int* mo, int* d, int* h, int* mi, int* se) {
    if (iso.size() != 19) return false;
    if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':') {
        return false;
    }
    int* outs[6] = {y, mo, d, h, mi, se};
    // 年占 4 位，其余各 2 位；位置按 ISO 串的固定布局取。
    const int starts[6] = {0, 5, 8, 11, 14, 17};
    const int widths[6] = {4, 2, 2, 2, 2, 2};
    for (int i = 0; i < 6; ++i) {
        int v = 0;
        for (int k = 0; k < widths[i]; ++k) {
            const char c = iso[static_cast<size_t>(starts[i] + k)];
            if (c < '0' || c > '9') return false;
            v = v * 10 + (c - '0');
        }
        *outs[i] = v;
    }
    return *mo >= 1 && *mo <= 12 && *d >= 1 && *d <= 31 && *h <= 23 && *mi <= 59 && *se <= 59;
}

/**
 * 这个月有几天。闰年按公历规则算 —— 日期控件最常见的坏值是 2 月 30 日。
 * @param y 年
 * @param mo 月
 * @return 天数
 */
int daysInMonth(int y, int mo) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (mo < 1 || mo > 12) return 0;
    if (mo == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return kDays[mo - 1];
}

}  // namespace

bool numberAccepts(const std::string& current, uint32_t add) {
    if (add >= '0' && add <= '9') return true;
    if (add == '.') return current.find('.') == std::string::npos;
    // 负号只能打在最前面：中间出现负号不是数值的任何中间态。
    if (add == '-') return current.empty();
    return false;
}

bool numberTextValid(const std::string& text) {
    if (text.empty()) return true;
    size_t i = 0;
    if (text[0] == '-') i = 1;
    bool digit = false;
    bool dot = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            digit = true;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digit;
}

bool isoDateTimeValid(const std::string& iso) {
    if (iso.empty()) return true;
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int se = 0;
    if (!splitIso(iso, &y, &mo, &d, &h, &mi, &se)) return false;
    return d <= daysInMonth(y, mo);
}

std::string formatControlDate(const std::string& iso, const std::string& pattern) {
    if (iso.empty()) return std::string();
    const std::string& fmt = pattern.empty() ? std::string(kDefaultDateFormat) : pattern;
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int se = 0;
    if (!splitIso(iso, &y, &mo, &d, &h, &mi, &se)) return iso;
    std::string out;
    size_t i = 0;
    while (i < fmt.size()) {
        const std::string rest = fmt.substr(i);
        if (rest.rfind("yyyy", 0) == 0) {
            out += padded(y, 4);
            i += 4;
        } else if (rest.rfind("MM", 0) == 0) {
            out += padded(mo, 2);
            i += 2;
        } else if (rest.rfind("dd", 0) == 0) {
            out += padded(d, 2);
            i += 2;
        } else if (rest.rfind("HH", 0) == 0) {
            out += padded(h, 2);
            i += 2;
        } else if (rest.rfind("mm", 0) == 0) {
            out += padded(mi, 2);
            i += 2;
        } else if (rest.rfind("ss", 0) == 0) {
            out += padded(se, 2);
            i += 2;
        } else {
            out += fmt[i];
            ++i;
        }
    }
    return out;
}

const ChoiceDetail* choiceOf(const ControlSpec& spec) {
    return std::get_if<ChoiceDetail>(&spec.detail);
}

const DropdownDetail* dropdownOf(const ControlSpec& spec) {
    return std::get_if<DropdownDetail>(&spec.detail);
}

const DateDetail* dateOf(const ControlSpec& spec) { return std::get_if<DateDetail>(&spec.detail); }

const TextDetail* textOf(const ControlSpec& spec) { return std::get_if<TextDetail>(&spec.detail); }

const CustomDetail* customOf(const ControlSpec& spec) {
    return std::get_if<CustomDetail>(&spec.detail);
}

ControlSpec makeControlSpec(const std::string& controlType) {
    ControlSpec spec;
    spec.controlType = controlType.empty() ? std::string("text") : controlType;
    const std::string& t = spec.controlType;
    if (t == "number") {
        TextDetail d;
        d.filter = ContentFilter::Number;
        spec.detail = d;
        return spec;
    }
    if (t == "radioButton" || t == "checkBox" || t == "radioGroup" || t == "checkBoxGroup") {
        ChoiceDetail d;
        d.multi = (t == "checkBox" || t == "checkBoxGroup");
        spec.detail = d;
        spec.valueShape = d.multi ? (t == "checkBox" ? ValueShape::Bool : ValueShape::List)
                                  : (t == "radioButton" ? ValueShape::Bool : ValueShape::Enum);
        // 选择类的内容是"图形 + 标签"的投影，不是作者打的字：内容只读，光标也不进。
        spec.readonly = true;
        return spec;
    }
    if (t == "dropdownList") {
        spec.detail = DropdownDetail{};
        spec.valueShape = ValueShape::Enum;
        spec.readonly = true;
        return spec;
    }
    if (t == "date") {
        spec.detail = DateDetail{};
        spec.valueShape = ValueShape::Enum;
        spec.readonly = true;
        return spec;
    }
    if (t == "text") return spec;
    // 不认识的名字：原样透传，按文本值形态画通用输入框，不假装认识。
    spec.detail = CustomDetail{};
    return spec;
}

uint64_t controlAppearanceDigest(const Appearance& a) {
    uint64_t h = kFnvBasis;
    const auto mixLen = [&h](const Length& len) {
        h = fnvMix(h, static_cast<uint64_t>(len.kind));
        h = fnvMix(h, static_cast<uint64_t>(len.abs_emu));
    };
    const auto mixColor = [&h](const Color& c) {
        h = fnvMix(h, static_cast<uint64_t>(c.r) | (static_cast<uint64_t>(c.g) << 8) |
                          (static_cast<uint64_t>(c.b) << 16) | (static_cast<uint64_t>(c.a) << 24));
    };
    const auto mixSide = [&](const BorderSide& b) {
        h = fnvMix(h, b.on ? 1ull : 0ull);
        h = fnvMix(h, static_cast<uint64_t>(b.style));
        mixLen(b.width);
        mixColor(b.color);
    };
    mixLen(a.width);
    h = fnvMix(h, static_cast<uint64_t>(a.overflow));
    h = fnvMix(h, static_cast<uint64_t>(a.underline));
    mixSide(a.border.left);
    mixSide(a.border.right);
    mixSide(a.border.top);
    mixSide(a.border.bottom);
    mixLen(a.glyph.width);
    mixLen(a.glyph.size);
    mixColor(a.glyph.color);
    mixColor(a.glyph.fill);
    h = fnvMix(h, static_cast<uint64_t>(a.align));
    return h;
}

uint64_t controlPaintDigest(const ControlSpec& spec) {
    uint64_t h = controlAppearanceDigest(spec.appearance);
    // 值按"喂进去的字节"混，不按结构：flag / text / items 三个字段的分隔由 0xFFFF
    //   这种值域外的哨兵负责，免得 ("a","b") 与 ("ab",) 折成同一个摘要。
    const auto mixText = [&h](const std::string& s) {
        for (const char ch : s)
            h = fnvMix(h, static_cast<uint64_t>(static_cast<unsigned char>(ch)));
        h = fnvMix(h, 0xFFFFull);
    };
    h = fnvMix(h, spec.value.flag ? 1ull : 0ull);
    mixText(spec.value.text);
    for (const std::string& item : spec.value.items) mixText(item);
    return h;
}

bool controlMarkerAt(const ControlSpec& spec, int cp_in_content, bool* checked) {
    if (checked != nullptr) *checked = false;
    if (cp_in_content < 0) return false;
    const ChoiceDetail* c = choiceOf(spec);
    if (c == nullptr) return false;
    // 单控件：整个内容就是一个选项，记号在第一位。
    if (c->options.empty()) {
        if (cp_in_content != 0) return false;
        if (checked != nullptr) *checked = spec.value.flag;
        return true;
    }
    std::vector<std::pair<int, int>> spans;
    if (!controlOptionSpans(spec, &spans)) return false;
    for (size_t i = 0; i < spans.size(); ++i) {
        if (spans[i].first != cp_in_content) continue;
        if (checked == nullptr) return true;
        bool on = false;
        if (c->multi) {
            for (const std::string& v : spec.value.items) {
                if (!c->options[i].value.empty() && v == c->options[i].value) {
                    on = true;
                    break;
                }
            }
        } else {
            on = !spec.value.text.empty() && spec.value.text == c->options[i].value;
        }
        *checked = on;
        return true;
    }
    return false;
}

std::string controlDisplayText(const ControlSpec& spec, const ControlValue& value) {
    if (const auto* c = choiceOf(spec)) {
        if (c->options.empty()) {
            const char* glyph =
                c->multi ? (value.flag ? "\u2611" : "\u2610") : (value.flag ? "\u25C9" : "\u25CB");
            return std::string(glyph) + " " + c->label;
        }
        std::string out;
        for (size_t i = 0; i < c->options.size(); ++i) {
            const ControlOption& opt = c->options[i];
            bool on = false;
            if (c->multi) {
                for (const std::string& v : value.items) {
                    if (v == opt.value) {
                        on = true;
                        break;
                    }
                }
            } else {
                on = (!value.text.empty() && value.text == opt.value);
            }
            const char* glyph = c->multi ? (on ? "\u2611" : "\u2610") : (on ? "\u25C9" : "\u25CB");
            if (i > 0) out += kOptionGap;
            out += glyph;
            out += " ";
            out += opt.label;
        }
        return out;
    }
    if (const auto* d = dropdownOf(spec)) {
        if (value.text.empty()) return std::string();
        for (const ControlOption& opt : d->options) {
            if (opt.value == value.text) return opt.label;
        }
        // 自由输入的值没有选项可查，原样显示 —— 显示与值是同一段文字。
        return d->free_input ? value.text : std::string();
    }
    if (const auto* d = dateOf(spec)) return formatControlDate(value.text, d->format);
    return std::string();
}

bool controlOptionSpans(const ControlSpec& spec, std::vector<std::pair<int, int>>* out) {
    if (out == nullptr) return false;
    out->clear();
    const ChoiceDetail* c = choiceOf(spec);
    if (c == nullptr || c->options.empty()) return false;
    int pos = 0;
    for (size_t i = 0; i < c->options.size(); ++i) {
        if (i > 0) pos += static_cast<int>(std::char_traits<char>::length(kOptionGap));
        const int begin = pos;
        // 图形 1 个码点 + 一个空格 + 标签的码点数。
        pos += 2 + static_cast<int>(utf8ToCodepoints(c->options[i].label).size());
        out->emplace_back(begin, pos);
    }
    return true;
}

bool applyControlPatch(ControlSpec* spec, const ControlPatch& patch, std::string* error) {
    if (error != nullptr) error->clear();
    if (spec == nullptr) return false;
    const auto reject = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };

    if (patch.expect_type.has_value() && *patch.expect_type != spec->controlType) {
        return reject("改定义：类型不可改（现在 " + spec->controlType + "，要求 " +
                      *patch.expect_type + "）；换类型请删掉重插");
    }
    if (patch.key.has_value()) spec->key = *patch.key;

    // 特有层是变体：每一项只能落到收它的那一层。落到别处就是调用方把字段送错了控件，
    //   说出来比默默无视强 —— 后者会让调用方以为配置生效了。
    //   选项表两族都收（选择类的组控件与下拉各有一张表），所以它单独判一次。
    if (patch.label.has_value() || patch.group.has_value()) {
        auto* c = std::get_if<ChoiceDetail>(&spec->detail);
        if (c == nullptr) {
            return reject("改定义：这个控件不收标签 / 互斥组（它不是选择类）");
        }
        if (patch.label.has_value()) c->label = *patch.label;
        if (patch.group.has_value()) c->group = *patch.group;
    }
    if (patch.options.has_value()) {
        if (auto* c = std::get_if<ChoiceDetail>(&spec->detail)) {
            c->options = *patch.options;
        } else if (auto* d = std::get_if<DropdownDetail>(&spec->detail)) {
            d->options = *patch.options;
        } else {
            return reject("改定义：这个控件不收选项表（它不是选择类或下拉）");
        }
    }
    if (patch.free_input.has_value()) {
        auto* d = std::get_if<DropdownDetail>(&spec->detail);
        if (d == nullptr) return reject("改定义：这个控件不收自由输入（它不是下拉）");
        d->free_input = *patch.free_input;
    }
    if (patch.glyph_color.has_value()) {
        // 记号色只对选择类有意义：别的控件没有记号，配了也只是空转。
        if (choiceOf(*spec) == nullptr) {
            return reject("改定义：这个控件不收记号颜色（它不是选择类）");
        }
        spec->appearance.glyph.color = *patch.glyph_color;
    }
    if (patch.format.has_value() || patch.units.has_value()) {
        auto* d = std::get_if<DateDetail>(&spec->detail);
        if (d == nullptr) return reject("改定义：这个控件不收格式 / 单位（它不是日期）");
        if (patch.format.has_value()) d->format = *patch.format;
        if (patch.units.has_value()) {
            // 六个单位一个都不选等于"选不了任何东西"，而显示格式仍然能画出完整日期 ——
            //   那种组合只会在界面上表现为"点了没反应"。
            if (*patch.units == 0 || (*patch.units & ~kDateUnitsAll) != 0) {
                return reject("改定义：日期单位至少要选一个，且不能超出年月日时分秒");
            }
            d->units = *patch.units;
        }
    }
    return true;
}

void pruneControlValue(const ControlSpec& spec, ControlValue* value) {
    if (value == nullptr) return;
    const auto known = [](const std::vector<ControlOption>& options, const std::string& v) {
        for (const ControlOption& o : options) {
            if (o.value == v) return true;
        }
        return false;
    };
    if (const auto* c = choiceOf(spec)) {
        // 单控件（单选 / 复选）的值是一个布尔，与选项表无关 —— 表删空也仍然成立。
        //   组控件按值形态认，不按类型名认：它的值必须落在选项表里，表空了就没有合法值。
        if (spec.valueShape == ValueShape::Bool) return;
        if (c->multi) {
            std::vector<std::string> kept;
            for (const std::string& v : value->items) {
                if (known(c->options, v)) kept.push_back(v);
            }
            value->items = std::move(kept);
            return;
        }
        if (!value->text.empty() && !known(c->options, value->text)) value->text.clear();
        return;
    }
    if (const auto* d = dropdownOf(spec)) {
        // 自由输入的值本来就可以不在表里，删选项不该把它一起清掉。
        if (d->free_input) return;
        if (!value->text.empty() && !known(d->options, value->text)) value->text.clear();
    }
}

bool controlValueValid(const ControlSpec& spec, const ControlValue& value, std::string* error) {
    if (error != nullptr) error->clear();
    if (const auto* c = choiceOf(spec)) {
        if (c->options.empty()) return true;
        if (!c->multi) {
            if (value.text.empty()) return true;
            for (const ControlOption& opt : c->options) {
                if (opt.value == value.text) return true;
            }
            if (error != nullptr) *error = "值不在选项表里：" + value.text;
            return false;
        }
        for (const std::string& v : value.items) {
            bool found = false;
            for (const ControlOption& opt : c->options) found = found || (opt.value == v);
            if (!found) {
                if (error != nullptr) *error = "值不在选项表里：" + v;
                return false;
            }
        }
        return true;
    }
    if (const auto* d = dropdownOf(spec)) {
        if (value.text.empty() || d->free_input) return true;
        for (const ControlOption& opt : d->options) {
            if (opt.value == value.text) return true;
        }
        if (error != nullptr) *error = "值不在选项表里：" + value.text;
        return false;
    }
    if (dateOf(spec) != nullptr) {
        if (isoDateTimeValid(value.text)) return true;
        if (error != nullptr) *error = "日期不是合法的 ISO 串（YYYY-MM-DDTHH:MM:SS）";
        return false;
    }
    return true;
}

}  // namespace heditor
