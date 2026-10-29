#include "core/doc/serialize.h"
#include "core/doc/font_refs.h"
#include "core/utils/utf8.h"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "core/doc/record_json.h"
#include "nlohmann/json.hpp"

namespace heditor::doc {

namespace {

using nlohmann::ordered_json;

/**
 * 载荷的七个分支。种类到分支的对应是唯一一份，落盘、读盘与哈希都按它走。
 */
enum class Alt : uint8_t {
    None,
    Run,
    Mark,
    Control,
    Table,
    Row,
    Cell,
    Image,
    Formula,
    Field,
    Section,
};

/**
 * 属性键。写与读共用同一套常量：两处各写字面量，迟早有一处拼错，而拼错的表现是这个字段读回来
 * 是默认值，也就是静默丢数据。
 */
constexpr const char* kV = "v";
constexpr const char* kNext = "next";
constexpr const char* kStrings = "s";
constexpr const char* kFonts = "fonts";
constexpr const char* kRoot = "root";
constexpr const char* kId = "id";
constexpr const char* kKind = "k";
constexpr const char* kHidden = "h";
constexpr const char* kText = "t";
constexpr const char* kPara = "p";
constexpr const char* kStyle = "st";
constexpr const char* kMarks = "m";
constexpr const char* kControl = "c";
constexpr const char* kTable = "tb";
constexpr const char* kRow = "rw";
constexpr const char* kCell = "cs";
constexpr const char* kImage = "im";
constexpr const char* kFormula = "fm";
constexpr const char* kField = "fd";
constexpr const char* kSection = "sec";
constexpr const char* kChildren = "ch";
constexpr const char* kStyles = "styles";
constexpr const char* kStylesDefaultsPara = "dp";
constexpr const char* kStylesDefaultsRun = "dr";
constexpr const char* kStylesPara = "p";
constexpr const char* kStylesChar = "c";
constexpr const char* kStyleName = "n";
constexpr const char* kStyleBasedOn = "bo";
constexpr const char* kResources = "res";

/**
 * 载荷是哪一支。
 *
 * 最后一支写成带 static_assert 的 else 而不是裸 else：加一种载荷时，裸 else 会把它静默
 * 当成控件记录写出去，而这里要求编不过。
 */
Alt altOf(const Payload& p) {
    return std::visit(
        [](const auto& v) -> Alt {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return Alt::None;
            else if constexpr (std::is_same_v<T, RunPayload>)
                return Alt::Run;
            else if constexpr (std::is_same_v<T, MarkPayload>)
                return Alt::Mark;
            else if constexpr (std::is_same_v<T, ControlSpec>)
                return Alt::Control;
            else if constexpr (std::is_same_v<T, TableSpec>)
                return Alt::Table;
            else if constexpr (std::is_same_v<T, RowSpec>)
                return Alt::Row;
            else if constexpr (std::is_same_v<T, CellSpec>)
                return Alt::Cell;
            else if constexpr (std::is_same_v<T, ImageSpec>)
                return Alt::Image;
            else if constexpr (std::is_same_v<T, FormulaSpec>)
                return Alt::Formula;
            else if constexpr (std::is_same_v<T, FieldSpec>)
                return Alt::Field;
            else {
                static_assert(std::is_same_v<T, SectionSpec>, "新增载荷要在这里加一支");
                return Alt::Section;
            }
        },
        p);
}

/**
 * 种类与载荷的对应：文本带字符属性，段落标记带段落覆盖，控件带控件记录，其余不带载荷。
 *
 * 这条对应是树的不变量（flowLen 与 hashNode 都按它取 std::get），落盘与读盘都查一遍：
 * 一份种类与载荷对不上的文件读进来，第一次编辑就会在 std::get 上崩掉。默认分支落在
 * "不带载荷"上是准确的 —— 新加的结构种类本来就没有载荷，要给它载荷时是先改这张表。
 */
Alt altForKind(NodeKind kind) {
    switch (kind) {
        case NodeKind::TextRun:
            return Alt::Run;
        case NodeKind::ParagraphMark:
            return Alt::Mark;
        case NodeKind::ContentControl:
            return Alt::Control;
        case NodeKind::Table:
            return Alt::Table;
        case NodeKind::TableRow:
            return Alt::Row;
        case NodeKind::TableCell:
            return Alt::Cell;
        case NodeKind::Image:
            return Alt::Image;
        case NodeKind::Formula:
            return Alt::Formula;
        case NodeKind::Field:
            return Alt::Field;
        case NodeKind::Section:
            return Alt::Section;
        default:
            return Alt::None;
    }
}

/**
 * 字体表的键：非负十进制整数。只认这一种写法 —— 键是"第几号字体"的身份，允许 "+1"/" 1"
 * 只会让同一份表有两种合法写法。
 */
bool parseIndexKey(const std::string& s, int* out) {
    if (s.empty()) return false;
    int v = 0;
    for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        v = v * 10 + (ch - '0');
        if (v > 1000000) return false;
    }
    *out = v;
    return true;
}

/**
 * 文本转 UTF-8。非法码点（代理区 / 越界）当场报错，不替换成 U+FFFD —— 写端替换一个字符，
 * 读回来就是另一份文本，而所有字段都"看起来正常"。宁可整份写不出去。
 */
bool textToUtf8(const std::vector<uint32_t>& cps, std::string* out, std::string* error) {
    for (uint32_t cp : cps) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            *error = "文本里有非法码点 " + std::to_string(cp);
            return false;
        }
    }
    *out = codepointsToUtf8(cps);
    return true;
}

/**
 * base64 编码。
 * @param data 字节
 * @returns 文本
 */
std::string base64Encode(const std::vector<uint8_t>& data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) |
                           static_cast<uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
        i += 3;
    }
    if (i < data.size()) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? kAlphabet[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

/**
 * base64 解码。
 * @param text 文本
 * @param out 字节
 * @returns true = 解码成功
 */
bool base64Decode(const std::string& text, std::vector<uint8_t>* out) {
    uint32_t acc = 0;
    int bits = 0;
    out->clear();
    for (char c : text) {
        if (c == '=') break;
        int v = -1;
        if (c >= 'A' && c <= 'Z')
            v = c - 'A';
        else if (c >= 'a' && c <= 'z')
            v = c - 'a' + 26;
        else if (c >= '0' && c <= '9')
            v = c - '0' + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
}
/**
 * 这棵子树用到哪些字体下标。字体表只列点过名的那些：把整个库写进文件，等于把装载环境
 * 一起打包（系统字体几十上百张，与这份文档无关）。
 */
void collectFonts(const Ref& n, std::unordered_set<int>* out) {
    const Alt alt = altOf(n->payload);
    if (alt == Alt::Run) {
        const RunPayload& p = std::get<RunPayload>(n->payload);
        if (p.marks.font_index.has_value()) out->insert(*p.marks.font_index);
    } else if (alt == Alt::Mark) {
        const MarkPayload& p = std::get<MarkPayload>(n->payload);
        if (p.marks.font_index.has_value()) out->insert(*p.marks.font_index);
        if (p.para.font_index.has_value()) out->insert(*p.para.font_index);
        if (p.para.fallback_font_indices.has_value()) {
            for (int index : *p.para.fallback_font_indices) out->insert(index);
        }
    }
    for (const Ref& c : n->children) collectFonts(c, out);
}

/**
 * 样式表用到哪些字体下标。
 */
void collectStyleFonts(const StyleSheet& sheet, std::unordered_set<int>* out) {
    const auto add_para = [out](const ParaFormatPatch& p) {
        if (p.font_index.has_value()) out->insert(*p.font_index);
        if (p.fallback_font_indices.has_value()) {
            for (int index : *p.fallback_font_indices) out->insert(index);
        }
    };
    const auto add_run = [out](const CharFormat& r) {
        if (r.font_index.has_value()) out->insert(*r.font_index);
    };
    add_para(sheet.docDefaultsPara());
    add_run(sheet.docDefaultsRun());
    for (StyleKind kind : {StyleKind::Paragraph, StyleKind::Character}) {
        for (const std::string& name : sheet.names(kind)) {
            const StyleDef* def = sheet.find(kind, name);
            if (def == nullptr) continue;
            add_para(def->para);
            add_run(def->run);
        }
    }
}

/**
 * 一条样式写成 JSON。名字直接写成字符串，不进字符串池：样式表很小，也便于人读。
 */
ordered_json styleJson(const StyleDef& def) {
    ordered_json j = ordered_json::object();
    j[kStyleName] = def.name;
    if (!def.based_on.empty()) j[kStyleBasedOn] = def.based_on;
    j["p"] = recjson::paraJson(def.para);
    j["r"] = recjson::marksJson(def.run);
    return j;
}

ordered_json stylesJson(const StyleSheet& sheet) {
    ordered_json j = ordered_json::object();
    j[kStylesDefaultsPara] = recjson::paraJson(sheet.docDefaultsPara());
    j[kStylesDefaultsRun] = recjson::marksJson(sheet.docDefaultsRun());
    for (StyleKind kind : {StyleKind::Paragraph, StyleKind::Character}) {
        ordered_json list = ordered_json::array();
        for (const std::string& name : sheet.names(kind)) {
            const StyleDef* def = sheet.find(kind, name);
            if (def != nullptr) list.push_back(styleJson(*def));
        }
        j[kind == StyleKind::Paragraph ? kStylesPara : kStylesChar] = std::move(list);
    }
    return j;
}

bool writeNode(const Ref& n, recjson::StringPool* strings, NodeId* max_id,
               std::unordered_set<NodeId>* seen, ordered_json* out, std::string* error) {
    const Alt alt = altOf(n->payload);
    if (alt != altForKind(n->kind)) {
        *error = "节点 " + std::to_string(n->id) + " 的种类与载荷不符";
        return false;
    }
    if (n->id == kInvalidNodeId) {
        *error = "节点号无效";
        return false;
    }
    // 号重复的树读不回来（读端会拒），所以写端也不交出一份读不回来的文件。
    if (!seen->insert(n->id).second) {
        *error = "节点号重复：" + std::to_string(n->id);
        return false;
    }
    if (n->id > *max_id) *max_id = n->id;

    ordered_json j = ordered_json::object();
    j[kId] = n->id;
    j[kKind] = nodeKindName(n->kind);
    if (n->hidden) j[kHidden] = true;
    if (alt == Alt::Run) {
        const RunPayload& p = std::get<RunPayload>(n->payload);
        if (!p.text.empty()) {
            std::string utf8;
            if (!textToUtf8(p.text, &utf8, error)) {
                *error = "节点 " + std::to_string(n->id) + "：" + *error;
                return false;
            }
            j[kText] = std::move(utf8);
        }
        if (!p.marks.empty()) j[kMarks] = recjson::marksJson(p.marks);
        if (!p.style.empty()) j[kStyle] = strings->add(p.style);
    } else if (alt == Alt::Mark) {
        const MarkPayload& p = std::get<MarkPayload>(n->payload);
        if (!p.para.empty()) j[kPara] = recjson::paraJson(p.para);
        if (!p.style.empty()) j[kStyle] = strings->add(p.style);
        if (!p.marks.empty()) j[kMarks] = recjson::marksJson(p.marks);
    } else if (alt == Alt::Control) {
        const ControlSpec& p = std::get<ControlSpec>(n->payload);
        if (!(p == ControlSpec{})) j[kControl] = recjson::controlJson(p, strings);
    } else if (alt == Alt::Table) {
        const TableSpec& p = std::get<TableSpec>(n->payload);
        if (!(p == TableSpec{})) j[kTable] = recjson::tableJson(p);
    } else if (alt == Alt::Row) {
        const RowSpec& p = std::get<RowSpec>(n->payload);
        if (!(p == RowSpec{})) j[kRow] = recjson::rowJson(p);
    } else if (alt == Alt::Cell) {
        const CellSpec& p = std::get<CellSpec>(n->payload);
        if (!(p == CellSpec{})) j[kCell] = recjson::cellJson(p);
    } else if (alt == Alt::Image) {
        const ImageSpec& p = std::get<ImageSpec>(n->payload);
        if (!(p == ImageSpec{})) j[kImage] = recjson::imageJson(p);
    } else if (alt == Alt::Formula) {
        const FormulaSpec& p = std::get<FormulaSpec>(n->payload);
        if (!(p == FormulaSpec{})) j[kFormula] = recjson::formulaJson(p);
    } else if (alt == Alt::Field) {
        const FieldSpec& p = std::get<FieldSpec>(n->payload);
        // 域记录的三项都有非零默认的可能（种类、属性名、缓存值），所以三项全默认时不写；
        // 读回来仍是默认值，与别组的"默认值不写"同一条。
        if (p.kind != FieldKind::PageNumber || !p.name.empty() || !p.cache.empty()) {
            j[kField] = recjson::fieldJson(p);
        }
    } else if (alt == Alt::Section) {
        const SectionSpec& p = std::get<SectionSpec>(n->payload);
        // 节记录的默认值是"一节 A4 纵向、页边距 2cm、连续分节"，全默认时不写。
        if (!p.sameAs(SectionSpec{})) j[kSection] = recjson::sectionJson(p);
    }
    if (!n->children.empty()) {
        ordered_json kids = ordered_json::array();
        for (const Ref& c : n->children) {
            ordered_json kid;
            if (!writeNode(c, strings, max_id, seen, &kid, error)) return false;
            kids.push_back(std::move(kid));
        }
        j[kChildren] = std::move(kids);
    }
    *out = std::move(j);
    return true;
}

/**
 * 节点 id 的两个方向：只认正整数。0 是保留的无效值，负数与小数都不是 id —— 不做隐式取整。
 */
bool readNodeId(const ordered_json& j, const char* key, NodeId* out) {
    if (!j.contains(key)) return false;
    if (j[key].is_number_unsigned()) {
        *out = j[key].get<NodeId>();
        return *out != kInvalidNodeId;
    }
    if (j[key].is_number_integer()) {
        const long long v = j[key].get<long long>();
        if (v <= 0) return false;
        *out = static_cast<NodeId>(v);
        return true;
    }
    return false;
}

/**
 * 读端环境：字符串池 + 字体映射 + 错误串 + 已见节点号。重复的节点号必须拒掉 —— 那种树里改一个
 * 节点会连带改到另一个同号节点，而且是静默的。
 */
class Reader : public recjson::Context {
public:

    explicit Reader(std::string* err) : err_(err) {}

    bool stringAt(int index, std::string* out) const override {
        if (index < 0 || index >= static_cast<int>(strings_.size())) return fail("字符串下标越界");
        *out = strings_[static_cast<std::size_t>(index)];
        return true;
    }

    bool fail(const std::string& what) const override {
        if (err_ != nullptr && err_->empty()) *err_ = what;
        return false;
    }

    /**
     * 文件里的下标换成本机的。没写字体表（v1 或手改过的文件）时映射是空的，下标原样保留 ——
     * 那正是 v1 的语义。
     */
    int mapFontIndex(int index) const override {
        if (font_map_.empty()) return index;
        const auto it = font_map_.find(index);
        return it == font_map_.end() ? index : it->second;
    }

    std::vector<std::string> strings_;
    /**
     * 文件下标 → 本机下标。只给文件里点过名的那些下标建条目。
     */
    std::unordered_map<int, int> font_map_;
    std::unordered_set<NodeId> seen_;
    NodeId max_id_ = 0;

private:

    std::string* err_ = nullptr;
};

/**
 * 嵌套上限。深度由文件决定，不能交给调用栈：一份特意构造的深链文件不该把进程打崩。
 */
constexpr int kMaxDepth = 128;

bool readNode(Reader& r, const ordered_json& j, int depth, Ref* out);

bool readChildren(Reader& r, const ordered_json& j, int depth, std::vector<Ref>* out) {
    if (!j.contains(kChildren)) return true;
    if (!j[kChildren].is_array()) return r.fail("孩子表不是数组");
    for (const ordered_json& item : j[kChildren]) {
        Ref child;
        if (!readNode(r, item, depth + 1, &child)) return false;
        out->push_back(std::move(child));
    }
    return true;
}

bool readRunPayload(Reader& r, const ordered_json& j, Payload* out) {
    RunPayload p;
    // v2 写字符串，v1 写码点数组。按类型分派而不是按版本号分派：两份文件混在一起时，
    //   类型是自证的，版本号只是一个声明。非法 UTF-8 一律拒掉（它读回来会变成别的文本）。
    if (j.contains(kText)) {
        if (j[kText].is_string()) {
            const std::string utf8 = j[kText].get<std::string>();
            if (!recjson::isValidUtf8(utf8)) return r.fail("文本不是合法 UTF-8");
            p.text = utf8ToCodepoints(utf8);
        } else {
            std::vector<uint32_t> cps;
            if (!recjson::readU32Array(j, kText, &cps)) {
                return r.fail("文本既不是 UTF-8 字符串，也不是码点数组");
            }
            p.text = std::move(cps);
        }
    }
    if (!recjson::readMarks(r, j, kMarks, &p.marks)) return r.fail("字符属性读不出来");
    if (j.contains(kStyle)) {
        int index = 0;
        if (!recjson::readInt(j, kStyle, &index)) return r.fail("字符样式引用不是下标");
        if (!r.stringAt(index, &p.style)) return false;
    }
    *out = std::move(p);
    return true;
}

bool readMarkPayload(Reader& r, const ordered_json& j, Payload* out) {
    MarkPayload p;
    if (!recjson::readPara(r, j, kPara, &p.para)) return r.fail("段落覆盖读不出来");
    if (!recjson::readMarks(r, j, kMarks, &p.marks)) return r.fail("标记的字符属性读不出来");
    if (j.contains(kStyle)) {
        int index = 0;
        if (!recjson::readInt(j, kStyle, &index)) return r.fail("段落样式引用不是下标");
        if (!r.stringAt(index, &p.style)) return false;
    }
    *out = std::move(p);
    return true;
}

bool readControlPayload(Reader& r, const ordered_json& j, Payload* out) {
    ControlSpec c;
    if (!recjson::readControl(r, j, kControl, &c)) return false;
    *out = std::move(c);
    return true;
}

bool readNode(Reader& r, const ordered_json& j, int depth, Ref* out) {
    if (depth > kMaxDepth) return r.fail("节点嵌套超过上限");
    if (!j.is_object()) return r.fail("节点条目不是对象");

    NodeId id = kInvalidNodeId;
    if (!readNodeId(j, kId, &id)) return r.fail("节点 id 不是正整数");
    if (!r.seen_.insert(id).second) {
        return r.fail("节点 id 重复：" + std::to_string(id));
    }
    if (id > r.max_id_) r.max_id_ = id;

    std::string kind_name;
    if (!recjson::readString(j, kKind, &kind_name) || kind_name.empty()) {
        return r.fail("节点缺少种类名");
    }
    NodeKind kind = NodeKind::TextRun;
    if (!nodeKindFromName(kind_name, &kind)) return r.fail("不认识的节点种类名 " + kind_name);

    bool hidden = false;
    if (!recjson::readBool(j, kHidden, &hidden)) return r.fail("hidden 不是布尔");

    Payload payload;
    const Alt alt = altForKind(kind);
    // 每种载荷只认自己那几个键。把段落覆盖写进文本节点这种错，宁可读失败，也不要读成
    // "文本节点加一份被忽略的段落覆盖"：那种文档看起来正常，下一次编辑就对它做错事。
    // 字符属性两个种类都带：文本节点用 m，段落标记也用 m（空段落里没有文本节点，
    //   字符格式只能挂在标记上）。
    const bool has_text = j.contains(kText) || (alt == Alt::Run && j.contains(kMarks));
    if (alt != Alt::Run && has_text) return r.fail("这一节点种类不带字符属性");
    if (alt != Alt::Run && alt != Alt::Mark && j.contains(kMarks)) {
        return r.fail("这一节点种类不带字符属性");
    }
    if (alt != Alt::Run && alt != Alt::Mark && j.contains(kStyle)) {
        return r.fail("这一节点种类不带样式引用");
    }
    if (alt != Alt::Mark && j.contains(kPara)) return r.fail("这一节点种类不带段落覆盖");
    if (alt != Alt::Control && j.contains(kControl)) return r.fail("这一节点种类不带控件记录");
    if (alt != Alt::Table && j.contains(kTable)) return r.fail("这一节点种类不带表格记录");
    if (alt != Alt::Row && j.contains(kRow)) return r.fail("这一节点种类不带行记录");
    if (alt != Alt::Cell && j.contains(kCell)) return r.fail("这一节点种类不带单元格记录");
    if (alt != Alt::Image && j.contains(kImage)) return r.fail("这一节点种类不带图片记录");
    if (alt != Alt::Formula && j.contains(kFormula)) return r.fail("这一节点种类不带公式记录");
    if (alt != Alt::Field && j.contains(kField)) return r.fail("这一节点种类不带域记录");
    if (alt != Alt::Section && j.contains(kSection)) return r.fail("这一节点种类不带节记录");

    if (alt == Alt::Run) {
        if (!readRunPayload(r, j, &payload)) return false;
    } else if (alt == Alt::Mark) {
        if (!readMarkPayload(r, j, &payload)) return false;
    } else if (alt == Alt::Control) {
        if (!readControlPayload(r, j, &payload)) return false;
    } else if (alt == Alt::Table) {
        TableSpec ts;
        if (!recjson::readTable(j, kTable, &ts)) return r.fail("表格记录读不出来");
        payload = std::move(ts);
    } else if (alt == Alt::Row) {
        RowSpec rs;
        if (!recjson::readRow(j, kRow, &rs)) return r.fail("行记录读不出来");
        payload = std::move(rs);
    } else if (alt == Alt::Cell) {
        CellSpec cs;
        if (!recjson::readCell(j, kCell, &cs)) return r.fail("单元格记录读不出来");
        payload = std::move(cs);
    } else if (alt == Alt::Image) {
        ImageSpec im;
        if (!recjson::readImage(j, kImage, &im)) return r.fail("图片记录读不出来");
        payload = std::move(im);
    } else if (alt == Alt::Formula) {
        FormulaSpec fm;
        if (!recjson::readFormula(j, kFormula, &fm)) return r.fail("公式记录读不出来");
        payload = std::move(fm);
    } else if (alt == Alt::Field) {
        FieldSpec fd;
        if (!recjson::readField(j, kField, &fd)) return r.fail("域记录读不出来");
        payload = std::move(fd);
    } else if (alt == Alt::Section) {
        SectionSpec sec;
        if (!recjson::readSection(j, kSection, &sec)) return r.fail("节记录读不出来");
        payload = std::move(sec);
    }

    std::vector<Ref> kids;
    if (!readChildren(r, j, depth, &kids)) return false;
    *out = makeNode(id, kind, std::move(payload), std::move(kids), hidden);
    return true;
}

}  // namespace

SerializeResult serializeImpl(const Doc& doc, const SerializeOptions& opt,
                              const StyleSheet* sheet) {
    SerializeResult result;
    if (!doc.root) {
        result.error = "空文档";
        return result;
    }
    recjson::StringPool strings;
    ordered_json root;
    NodeId max_id = 0;
    std::unordered_set<NodeId> seen;
    if (!writeNode(doc.root, &strings, &max_id, &seen, &root, &result.error)) return result;
    if (doc.next_id <= max_id) {
        result.error = "分配器水位不大于现有最大节点号";
        return result;
    }

    ordered_json j = ordered_json::object();
    j[kV] = kDocFormatVersion;
    j[kNext] = doc.next_id;
    if (!strings.list().empty()) j[kStrings] = strings.list();
    // 字体表：下标 → 文件名。写不出来的时候就当这一侧不认识字体库（文件里没有这一项，
    //   读回来退化成 v1 的"下标即下标"），不因此让整份文档存不下去。
    if (opt.font_ref) {
        std::unordered_set<int> used;
        collectFonts(doc.root, &used);
        if (sheet != nullptr) collectStyleFonts(*sheet, &used);
        recjson::ordered_json fonts = fontRefsJson(used, opt.font_ref);
        if (!fonts.empty()) j[kFonts] = std::move(fonts);
    }
    if (sheet != nullptr) j[kStyles] = stylesJson(*sheet);
    if (opt.resources) {
        const std::vector<ResourceBlob> blobs = opt.resources();
        if (!blobs.empty()) {
            ordered_json resources = ordered_json::array();
            for (const ResourceBlob& blob : blobs) {
                ordered_json item = ordered_json::object();
                item["id"] = blob.id;
                item["w"] = blob.width;
                item["h"] = blob.height;
                item["d"] = base64Encode(blob.rgba);
                resources.push_back(std::move(item));
            }
            j[kResources] = std::move(resources);
        }
    }
    j[kRoot] = std::move(root);

    // 先自查一遍再写：库对非法 UTF-8 的处理是抛异常或终止进程（wasm 关掉了异常），
    // 两条路都不该走到。查到就返回失败，调用方知道这份修订没写出去。
    if (!recjson::validateStrings(j, &result.error)) return result;
    result.text = j.dump();
    result.ok = true;
    return result;
}

SerializeResult serialize(const Doc& doc, const SerializeOptions& opt) {
    return serializeImpl(doc, opt, nullptr);
}

SerializeResult serialize(const Doc& doc, const StyleSheet& sheet, const SerializeOptions& opt) {
    return serializeImpl(doc, opt, &sheet);
}

bool readStyleList(Reader& r, const ordered_json& j, const char* key, StyleKind kind,
                   StyleSheet* sheet) {
    if (!j.contains(key)) return true;
    if (!j[key].is_array()) return r.fail("样式列表不是数组");
    for (const ordered_json& item : j[key]) {
        if (!item.is_object()) return r.fail("样式条目不是对象");
        if (!item.contains(kStyleName) || !item[kStyleName].is_string()) {
            return r.fail("样式缺少名字");
        }
        StyleDef def;
        def.kind = kind;
        def.name = item[kStyleName].get<std::string>();
        if (item.contains(kStyleBasedOn) && item[kStyleBasedOn].is_string()) {
            def.based_on = item[kStyleBasedOn].get<std::string>();
        }
        if (!recjson::readPara(r, item, "p", &def.para)) return r.fail("样式段落格式读不出来");
        if (!recjson::readMarks(r, item, "r", &def.run)) return r.fail("样式字符格式读不出来");
        sheet->add(def);
    }
    return true;
}

bool readStyleSheet(Reader& r, const ordered_json& j, StyleSheet* sheet) {
    if (!j.is_object()) return r.fail("样式表不是对象");
    ParaFormatPatch para;
    CharFormat run;
    if (!recjson::readPara(r, j, kStylesDefaultsPara, &para))
        return r.fail("样式默认段落格式读不出来");
    if (!recjson::readMarks(r, j, kStylesDefaultsRun, &run))
        return r.fail("样式默认字符格式读不出来");
    sheet->setDocDefaults(para, run);
    if (!readStyleList(r, j, kStylesPara, StyleKind::Paragraph, sheet)) return false;
    return readStyleList(r, j, kStylesChar, StyleKind::Character, sheet);
}

DeserializeResult deserializeImpl(const std::string& text, Doc* out, StyleSheet* sheet,
                                  const DeserializeOptions& opt) {
    DeserializeResult result;
    if (out == nullptr) {
        result.error = "目标修订为空";
        return result;
    }

    const ordered_json doc = ordered_json::parse(text, nullptr, false);
    if (doc.is_discarded()) {
        result.error = "JSON 解析失败（不是合法 JSON，或含无法解析的内容）";
        return result;
    }
    if (!doc.is_object()) {
        result.error = "顶层不是对象";
        return result;
    }
    if (!doc.contains(kV) || !doc[kV].is_number_integer()) {
        result.error = "缺少版本号";
        return result;
    }
    const int version = doc[kV].get<int>();
    if (version < kMinReadableVersion || version > kDocFormatVersion) {
        result.error = "版本号 " + std::to_string(version) + " 不在可读范围（" +
                       std::to_string(kMinReadableVersion) + "~" +
                       std::to_string(kDocFormatVersion) + "）";
        return result;
    }

    Reader reader(&result.error);
    // 字体表：文件说的"第几号是哪张脸"换成本机的下标。文件没带（v1）或某一侧没有这张脸
    //   时，那一个下标原样保留 —— 前者是 v1 的语义，后者交给回退链。
    if (doc.contains(kFonts)) {
        if (!doc[kFonts].is_object()) {
            result.error = "字体表不是对象";
            return result;
        }
        for (const auto& [key, item] : doc[kFonts].items()) {
            FontRef ref;
            if (item.is_string()) {
                // v2 的形态：值是文件名。照旧当"只认识文件名的那一侧"读。
                ref.file = item.get<std::string>();
            } else if (item.is_object()) {
                if (!recjson::readString(item, "ps", &ref.postscript)) {
                    result.error = "字体表里的 ps 不是字符串";
                    return result;
                }
                if (!recjson::readString(item, "family", &ref.family)) {
                    result.error = "字体表里的 family 不是字符串";
                    return result;
                }
                if (!recjson::readString(item, "file", &ref.file)) {
                    result.error = "字体表里的 file 不是字符串";
                    return result;
                }
            } else {
                result.error = "字体表里有既不是对象也不是字符串的项";
                return result;
            }
            int index = 0;
            if (!parseIndexKey(key, &index)) {
                result.error = "字体表的键不是下标：" + key;
                return result;
            }
            if (!opt.resolve_font) continue;
            const int local = opt.resolve_font(ref);
            if (local >= 0) reader.font_map_[index] = local;
        }
    }
    if (doc.contains(kStrings)) {
        if (!doc[kStrings].is_array()) {
            result.error = "字符串表不是数组";
            return result;
        }
        for (const ordered_json& item : doc[kStrings]) {
            if (!item.is_string()) {
                result.error = "字符串表里有非字符串";
                return result;
            }
            reader.strings_.push_back(item.get<std::string>());
        }
    }
    if (!doc.contains(kRoot)) {
        result.error = "缺少根节点";
        return result;
    }

    Ref root;
    if (!readNode(reader, doc[kRoot], 0, &root)) return result;
    if (!root) {
        result.error = "根节点没有建出来";
        return result;
    }
    // 根下只许有节，且至少一节：段落号在节之间连续，定位与编辑按根的孩子表走，
    // 混进别的种类会让"第几段"与"第几节"两套编号对不上。
    if (root->kind != NodeKind::Document || root->children.empty()) {
        result.error = "根必须是 Document，其下至少一节";
        return result;
    }
    for (const Ref& c : root->children) {
        if (c->kind != NodeKind::Section) {
            result.error = std::string("文档的直接孩子里有") + nodeKindName(c->kind) + "，不是节";
            return result;
        }
    }

    NodeId next = 0;
    if (!readNodeId(doc, kNext, &next)) {
        result.error = "缺少分配器水位";
        return result;
    }
    if (next <= reader.max_id_) {
        result.error = "分配器水位不大于文件里的最大节点号";
        return result;
    }

    if (doc.contains(kStyles)) {
        StyleSheet styles;
        if (!readStyleSheet(reader, doc[kStyles], &styles)) return result;
        if (sheet != nullptr) *sheet = std::move(styles);
        result.has_styles = true;
    }

    if (doc.contains(kResources)) {
        if (!doc[kResources].is_array()) {
            result.error = "资源表不是数组";
            return result;
        }
        if (opt.on_resource) {
            for (const ordered_json& item : doc[kResources]) {
                if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
                    !item.contains("w") || !item.contains("h") || !item.contains("d") ||
                    !item["d"].is_string()) {
                    result.error = "资源条目字段不全";
                    return result;
                }
                ResourceBlob blob;
                blob.id = item["id"].get<std::string>();
                blob.width = item["w"].get<int>();
                blob.height = item["h"].get<int>();
                if (!base64Decode(item["d"].get<std::string>(), &blob.rgba)) {
                    result.error = "资源像素不是合法 base64";
                    return result;
                }
                if (!opt.on_resource(blob)) {
                    result.error = "资源回调拒绝了一张图片";
                    return result;
                }
            }
        }
    }

    *out = adopt(std::move(root), next);
    result.ok = true;
    return result;
}

DeserializeResult deserialize(const std::string& text, Doc* out, const DeserializeOptions& opt) {
    return deserializeImpl(text, out, nullptr, opt);
}

DeserializeResult deserialize(const std::string& text, Doc* out, StyleSheet* sheet,
                              const DeserializeOptions& opt) {
    return deserializeImpl(text, out, sheet, opt);
}

}  // namespace heditor::doc
