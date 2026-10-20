#include "core/rev/serialize.h"

#include <cstdint>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "core/doc/record_json.h"
#include "nlohmann/json.hpp"

namespace heditor::rev {

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
        if (!p.text.empty()) j[kText] = p.text;
        if (!p.marks.empty()) j[kMarks] = recjson::marksJson(p.marks);
        if (!p.style.empty()) j[kStyle] = strings->add(p.style);
    } else if (alt == Alt::Mark) {
        const MarkPayload& p = std::get<MarkPayload>(n->payload);
        if (!p.para.empty()) j[kPara] = recjson::paraJson(p.para);
        if (!p.style.empty()) j[kStyle] = strings->add(p.style);
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
 * 读端环境：字符串池 + 错误串 + 已见节点号。重复的节点号必须拒掉 —— 那种树里改一个节点会
 * 连带改到另一个同号节点，而且是静默的。
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

    std::vector<std::string> strings_;
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
    if (!recjson::readU32Array(j, kText, &p.text)) return r.fail("文本不是码点数组");
    if (!recjson::readMarks(j, kMarks, &p.marks)) return r.fail("字符属性读不出来");
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
    if (!recjson::readPara(j, kPara, &p.para)) return r.fail("段落覆盖读不出来");
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
    const bool has_text = j.contains(kText) || j.contains(kMarks);
    if (alt != Alt::Run && has_text) return r.fail("这一节点种类不带字符属性");
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

SerializeResult serialize(const Doc& doc) {
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
    j[kV] = kRevFormatVersion;
    j[kNext] = doc.next_id;
    if (!strings.list().empty()) j[kStrings] = strings.list();
    j[kRoot] = std::move(root);

    // 先自查一遍再写：库对非法 UTF-8 的处理是抛异常或终止进程（wasm 关掉了异常），
    // 两条路都不该走到。查到就返回失败，调用方知道这份修订没写出去。
    if (!recjson::validateStrings(j, &result.error)) return result;
    result.text = j.dump();
    result.ok = true;
    return result;
}

DeserializeResult deserialize(const std::string& text, Doc* out) {
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
    if (doc[kV].get<int>() != kRevFormatVersion) {
        result.error = "版本号不是 " + std::to_string(kRevFormatVersion);
        return result;
    }

    Reader reader(&result.error);
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

    *out = adopt(std::move(root), next);
    result.ok = true;
    return result;
}

}  // namespace heditor::rev
