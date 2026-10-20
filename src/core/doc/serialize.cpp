#include "core/doc/serialize.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/doc/record_json.h"
#include "nlohmann/json.hpp"

namespace heditor {

namespace {

using nlohmann::ordered_json;
using namespace recjson;

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
constexpr const char* kRow = "rw";
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
 *
 * 记录级的编解码与池的实现在 record_json，与修订层共用一份。
 */
using Strings = recjson::StringPool;

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
    if (!(n.cell == CellSpec{})) j[kCell] = cellJson(n.cell);
    if (!(n.row == RowSpec{})) j[kRow] = rowJson(n.row);
    if (!(n.table == TableSpec{})) j[kTable] = tableJson(n.table);
    if (!(n.field == FieldSpec{})) j[kField] = recjson::fieldJson(n.field);
    if (!n.section.sameAs(SectionSpec{})) j[kSection] = sectionJson(n.section);
    if (!(n.image == ImageSpec{})) j[kImage] = recjson::imageJson(n.image);
    if (!(n.formula == FormulaSpec{})) j[kFormula] = recjson::formulaJson(n.formula);
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
class Reader : public recjson::Context {
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

    bool fail(const std::string& what) const override {
        if (err_ != nullptr && err_->empty()) *err_ = what;
        return false;
    }

    const ordered_json& nodeAt(std::size_t i) const { return nodes_.at(i); }
    std::size_t nodeCount() const { return nodes_.size(); }
    const std::vector<NodeKind>& kinds() const { return kinds_; }
    NodeId nextId() const { return next_id_; }
    const ordered_json& roots() const { return roots_; }

    bool stringAt(int index, std::string* out) const override {
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
    if (!recjson::readField(entry, kField, &n->field)) return r.fail("域记录读不出来");
    if (!readSection(entry, kSection, &n->section)) return r.fail("节记录读不出来");
    if (!recjson::readImage(entry, kImage, &n->image)) return r.fail("图片记录读不出来");
    if (!recjson::readFormula(entry, kFormula, &n->formula)) return r.fail("公式记录读不出来");
    if (entry.contains(kStyle)) {
        int index = 0;
        if (!readInt(entry, kStyle, &index)) return r.fail("样式引用不是下标");
        if (!r.stringAt(index, &n->styleRef)) return false;
    }
    if (!readCell(entry, kCell, &n->cell)) {
        return r.fail("单元格记录不是两个整数（加可选的垂直对齐名）");
    }
    if (!readRow(entry, kRow, &n->row)) return r.fail("行记录读不出来");
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
