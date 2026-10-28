#include "core/rev/fragment.h"
#include "core/rev/utf8.h"
#include "core/utils/utf8.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "core/rev/flow.h"
#include "core/rev/internal.h"
#include "core/rev/record_json.h"
#include "core/rev/utf8.h"
#include "core/utils/utf8.h"

namespace heditor::rev {
namespace {

using recjson::ordered_json;

constexpr const char* kV = "v";
constexpr const char* kStrings = "s";
constexpr const char* kFonts = "fonts";
constexpr const char* kNodes = "n";
constexpr int kFragmentVersion = 1;

/**
 * 外壳容器：只表达"在哪条流里"，不复制成节点。碰到就下沉一层。
 */
bool isShell(NodeKind kind) {
    return kind == NodeKind::Document || kind == NodeKind::Section ||
           kind == NodeKind::HeaderFooter;
}

/**
 * 把区间加宽到"整块容器"的边界。文本节点按码点截，外壳下沉，其余容器整块进来。
 *
 * 内容控件不加宽：它被盖住一半时不该把整块带走（复制一半控件比用户选的多），
 *   那一半由 collect 下沉成"选区里那几段文字"（控件壳不进片段）。
 */
void widenToWholeBlocks(const Node& parent, const FlowIndex& idx, FlowPos* lo, FlowPos* hi) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Ref& c : parent.children) {
            int f = 0;
            int e = 0;
            if (!idx.intervalOf(c->id, &f, &e)) continue;
            if (e <= *lo || f >= *hi) continue;
            if (f >= *lo && e <= *hi) continue;
            if (isShell(c->kind) || isText(c->kind)) continue;
            if (c->kind == NodeKind::ContentControl) continue;
            if (f < *lo) {
                *lo = f;
                changed = true;
            }
            if (e > *hi) {
                *hi = e;
                changed = true;
            }
        }
    }
}

/**
 * 按已加宽的区间取孩子。外壳下沉、文本截断、其余整棵。
 */
bool collect(const Node& parent, const FlowIndex& idx, FlowPos lo, FlowPos hi,
             std::vector<NodeSpec>* out, std::string* error) {
    for (const Ref& c : parent.children) {
        int f = 0;
        int e = 0;
        if (!idx.intervalOf(c->id, &f, &e)) continue;
        if (e <= lo || f >= hi) continue;
        if (isShell(c->kind)) {
            const FlowPos clo = std::max(lo, f);
            const FlowPos chi = std::min(hi, e);
            if (!collect(*c, idx, clo, chi, out, error)) return false;
            continue;
        }
        if (f >= lo && e <= hi) {
            out->push_back(toSpec(*c));
            continue;
        }
        if (isText(c->kind)) {
            const auto& run = std::get<RunPayload>(c->payload);
            const size_t a = static_cast<size_t>(std::max(f, lo) - f);
            const size_t b = static_cast<size_t>(std::min(e, hi) - f);
            NodeSpec s;
            s.kind = c->kind;
            s.hidden = c->hidden;
            s.text.assign(run.text.begin() + static_cast<long>(a),
                          run.text.begin() + static_cast<long>(b));
            s.marks = run.marks;
            s.styleRef = run.style;
            out->push_back(std::move(s));
            continue;
        }
        // 控件只被盖住一部分：下沉成"选区里那几段文字"，控件壳（括号、值、选项表）不进片段。
        //   整块复制会比用户选的多 —— 选中的是半句话，粘出来的却是一整块带括号的字段；
        //   而半个控件既没有可粘贴的形状，也不该因为选区蹭到边角就被整块带走。
        if (c->kind == NodeKind::ContentControl) {
            const FlowPos clo = std::max(lo, f);
            const FlowPos chi = std::min(hi, e);
            if (!collect(*c, idx, clo, chi, out, error)) return false;
            continue;
        }
        // 加宽之后不该再出现半覆盖的容器：出现了说明加宽漏了某一层，宁可失败。
        *error = "取片段：容器被切开（加宽没有覆盖到它）";
        return false;
    }
    return true;
}

/**
 * 复制根：能完整包住区间的最深容器。外壳无条件下沉，真正的容器只在严格内含时下沉 ——
 * 否则"选中了控件的开头到中间"会变成一个没有壳的半个控件（被切开的控件由 collect
 * 下沉成纯文字，见那里的注释）。
 *
 * 表格与它的行、格不下沉：它们是块级容器，行与格离开表格就不是可粘贴的内容，而"半张表"
 * 也没有形状可表达。碰到表格一律由外层加宽到整张表 —— 复制一张表就是复制整张表。
 */
const Node* pickRoot(const Node& start, const FlowIndex& idx, FlowPos lo, FlowPos hi) {
    const Node* cur = &start;
    for (;;) {
        const Node* next = nullptr;
        for (const Ref& c : cur->children) {
            int f = 0;
            int e = 0;
            if (!idx.intervalOf(c->id, &f, &e)) continue;
            if (!(f <= lo && hi <= e)) continue;
            if (!isContainer(c->kind)) continue;
            if (isShell(c->kind)) {
                next = c.get();
                break;
            }
            if (detail::isBlockContainer(c->kind)) continue;
            if (f < lo && hi < e) {
                next = c.get();
                break;
            }
        }
        if (next == nullptr) return cur;
        cur = next;
    }
}

/**
 * 纯文本投影：段落标记折成换行，表格里格之间是制表符、行之间是换行。
 */
void appendText(const NodeSpec& s, std::string* out) {
    if (isText(s.kind)) {
        *out += codepointsToUtf8(s.text);
        return;
    }
    if (isParagraphMark(s.kind)) {
        *out += '\n';
        return;
    }
    if (s.kind == NodeKind::Table) {
        bool first_row = true;
        for (const NodeSpec& row : s.children) {
            if (row.kind != NodeKind::TableRow) {
                appendText(row, out);
                continue;
            }
            if (!first_row) *out += '\n';
            first_row = false;
            bool first_cell = true;
            for (const NodeSpec& cell : row.children) {
                if (cell.kind != NodeKind::TableCell) {
                    appendText(cell, out);
                    continue;
                }
                if (!first_cell) *out += '\t';
                first_cell = false;
                for (const NodeSpec& kid : cell.children) appendText(kid, out);
            }
        }
        return;
    }
    for (const NodeSpec& kid : s.children) appendText(kid, out);
}

/**
 * 片段里用到哪些字体下标。字体表只列点过名的那些，与文档落盘同一条理由。
 */
void collectFonts(const NodeSpec& s, std::unordered_set<int>* out) {
    if (s.marks.font_index.has_value()) out->insert(*s.marks.font_index);
    if (s.para.font_index.has_value()) out->insert(*s.para.font_index);
    if (s.para.fallback_font_indices.has_value()) {
        for (int index : *s.para.fallback_font_indices) out->insert(index);
    }
    for (const NodeSpec& kid : s.children) collectFonts(kid, out);
}

/**
 * HTML 文本转义（只处理会破坏结构的五个字符）。
 * @param utf8 原文
 * @return 可放进 HTML 文本节点的串
 */
std::string escapeHtml(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

/**
 * 字符格式 → 内联 CSS。没有要表达的格式时给空串。
 *
 * 只写"粘贴到别的应用里还看得见的"那几项：粗细、斜体、下划线、字号、字色、字体。
 *   字体按 PostScript 名写，别的应用认不出就退回它自己的默认字体 —— 比不带强。
 * @param m 字符格式
 * @param opt 字体身份回调
 * @return CSS 声明串（分号分隔）
 */
std::string runStyleOf(const CharFormat& m, const SerializeOptions& opt) {
    std::string css;
    auto add = [&css](const std::string& decl) {
        if (!css.empty()) css += ';';
        css += decl;
    };
    if (m.bold.has_value()) add(std::string("font-weight:") + (*m.bold ? "700" : "400"));
    if (m.italic.has_value()) add(std::string("font-style:") + (*m.italic ? "italic" : "normal"));
    if (m.underline.has_value()) {
        add(std::string("text-decoration:") + (*m.underline ? "underline" : "none"));
    }
    if (m.font_index.has_value() && opt.font_ref) {
        const FontRef ref = opt.font_ref(*m.font_index);
        if (!ref.postscript.empty()) add("font-family:'" + ref.postscript + "'");
    }
    if (m.size.has_value() && m.size->kind == LengthKind::Abs) {
        const double pt = emuToPt(m.size->abs_emu);
        add("font-size:" + recjson::floatText(pt) + "pt");
    }
    if (m.color.has_value()) {
        char buf[16] = {0};
        std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", m.color->r, m.color->g, m.color->b);
        add(std::string("color:") + buf);
    }
    return css;
}

/**
 * 把一个子树写成 HTML。
 *
 * 结构照最常见的写法：段落 p、表格 table/tr/td，控件与域只留内容（它们的边界在 HTML 里
 *   没有对应物）。段落按"内容到了才开、段落标记到了才关"处理，于是空段是一个空的 p。
 * @param s 子树
 * @param out 输出缓冲
 * @param open_para 当前有没有开着的段落（跨调用保持）
 * @param opt 字体身份回调
 */
void appendHtml(const NodeSpec& s, std::string* out, bool* open_para, const SerializeOptions& opt) {
    if (isText(s.kind)) {
        const std::string text = escapeHtml(codepointsToUtf8(s.text));
        if (text.empty()) return;
        if (!*open_para) {
            *out += "<p>";
            *open_para = true;
        }
        const std::string css = runStyleOf(s.marks, opt);
        if (css.empty()) {
            *out += text;
        } else {
            *out += "<span style=\"" + css + "\">" + text + "</span>";
        }
        return;
    }
    if (isParagraphMark(s.kind)) {
        if (!*open_para) *out += "<p>";
        *out += "</p>";
        *open_para = false;
        return;
    }
    if (s.kind == NodeKind::Table) {
        *out += "<table>";
        for (const NodeSpec& row : s.children) appendHtml(row, out, open_para, opt);
        *out += "</table>";
        return;
    }
    if (s.kind == NodeKind::TableRow) {
        *out += "<tr>";
        for (const NodeSpec& cell : s.children) appendHtml(cell, out, open_para, opt);
        *out += "</tr>";
        return;
    }
    if (s.kind == NodeKind::TableCell) {
        // 格里自成一串段落：进来先收掉外层可能开着的段落，出去时关掉自己开的那一个。
        if (*open_para) {
            *out += "</p>";
            *open_para = false;
        }
        *out += "<td>";
        bool open_cell = false;
        for (const NodeSpec& kid : s.children) appendHtml(kid, out, &open_cell, opt);
        if (open_cell) *out += "</p>";
        *out += "</td>";
        return;
    }
    for (const NodeSpec& kid : s.children) appendHtml(kid, out, open_para, opt);
}

ordered_json nodeJsonOf(const NodeSpec& s, recjson::StringPool* strings) {
    ordered_json j = ordered_json::object();
    j["k"] = nodeKindName(s.kind);
    if (s.hidden) j["h"] = true;
    if (isText(s.kind)) {
        j["t"] = codepointsToUtf8(s.text);
        const ordered_json mk = recjson::marksJson(s.marks);
        if (!mk.empty()) j["mk"] = mk;
    }
    if (isParagraphMark(s.kind)) {
        const ordered_json p = recjson::paraJson(s.para);
        if (!p.empty()) j["p"] = p;
    }
    if (!s.styleRef.empty()) j["st"] = strings->add(s.styleRef);
    if (s.kind == NodeKind::ContentControl) j["ct"] = recjson::controlJson(s.control, strings);
    if (s.kind == NodeKind::Table) j["tb"] = recjson::tableJson(s.table);
    if (s.kind == NodeKind::TableRow) j["rw"] = recjson::rowJson(s.row);
    if (s.kind == NodeKind::TableCell) j["ce"] = recjson::cellJson(s.cell);
    if (s.kind == NodeKind::Image) j["im"] = recjson::imageJson(s.image);
    if (s.kind == NodeKind::Formula) j["fm"] = recjson::formulaJson(s.formula);
    if (s.kind == NodeKind::Field) j["fd"] = recjson::fieldJson(s.field);
    if (!s.children.empty()) {
        ordered_json kids = ordered_json::array();
        for (const NodeSpec& kid : s.children) kids.push_back(nodeJsonOf(kid, strings));
        j["c"] = std::move(kids);
    }
    return j;
}

/**
 * 读端环境：字符串表 + 字体下标换算，与文档落盘同一个契约。
 */
class ClipContext : public recjson::Context {
public:

    ClipContext(std::vector<std::string> strings, std::unordered_map<int, int> fonts)
        : strings_(std::move(strings)), fonts_(std::move(fonts)) {}

    bool stringAt(int index, std::string* out) const override {
        if (index < 0 || index >= static_cast<int>(strings_.size())) {
            error_ = "字符串下标越界：" + std::to_string(index);
            return false;
        }
        *out = strings_[static_cast<size_t>(index)];
        return true;
    }

    bool fail(const std::string& what) const override {
        if (error_.empty()) error_ = what;
        return false;
    }

    int mapFontIndex(int index) const override {
        const auto it = fonts_.find(index);
        return it == fonts_.end() ? index : it->second;
    }

    const std::string& error() const { return error_; }

private:

    std::vector<std::string> strings_;
    std::unordered_map<int, int> fonts_;
    mutable std::string error_;
};

bool parseNode(const ClipContext& ctx, const ordered_json& j, NodeSpec* out, std::string* error) {
    if (!j.is_object()) {
        *error = "片段里有不是对象的节点";
        return false;
    }
    if (!j.contains("k") || !j["k"].is_string()) {
        *error = "片段里的节点没有种类";
        return false;
    }
    if (!nodeKindFromName(j["k"].get<std::string>(), &out->kind)) {
        *error = "片段里有不认识的节点种类：" + j["k"].get<std::string>();
        return false;
    }
    if (!recjson::readBool(j, "h", &out->hidden)) {
        *error = "节点的 h 不是布尔";
        return false;
    }
    if (j.contains("t")) {
        if (!j["t"].is_string()) {
            *error = "节点的 t 不是字符串";
            return false;
        }
        out->text = utf8ToCodepoints(j["t"].get<std::string>());
    }
    if (!recjson::readMarks(ctx, j, "mk", &out->marks)) {
        *error = "字符格式读不下去";
        return false;
    }
    if (!recjson::readPara(ctx, j, "p", &out->para)) {
        *error = "段落格式读不下去";
        return false;
    }
    if (j.contains("st")) {
        if (!j["st"].is_number_integer()) {
            *error = "节点的 st 不是整数";
            return false;
        }
        if (!ctx.stringAt(j["st"].get<int>(), &out->styleRef)) {
            *error = ctx.error();
            return false;
        }
    }
    if (!recjson::readControl(ctx, j, "ct", &out->control)) {
        *error = "控件记录读不下去";
        return false;
    }
    if (!recjson::readTable(j, "tb", &out->table) || !recjson::readRow(j, "rw", &out->row) ||
        !recjson::readCell(j, "ce", &out->cell) || !recjson::readImage(j, "im", &out->image) ||
        !recjson::readFormula(j, "fm", &out->formula) ||
        !recjson::readField(j, "fd", &out->field)) {
        *error = "节点记录读不下去（" + std::string(nodeKindName(out->kind)) + "）";
        return false;
    }
    if (j.contains("c")) {
        if (!j["c"].is_array()) {
            *error = "节点的 c 不是数组";
            return false;
        }
        for (const ordered_json& kid : j["c"]) {
            NodeSpec child;
            if (!parseNode(ctx, kid, &child, error)) return false;
            out->children.push_back(std::move(child));
        }
    }
    return true;
}

}  // namespace

std::string clipboardPayload(const std::string& fragment, const std::string& text,
                             const std::string& error, const std::string& html) {
    ordered_json j = ordered_json::object();
    if (!error.empty() || fragment.empty()) {
        j["e"] = error.empty() ? std::string("没有选中内容") : error;
        return j.dump();
    }
    j["f"] = fragment;
    j["t"] = text;
    // HTML 是给"别的应用"的那一份：只带纯文本的话，粘到 Word / 浏览器里就是一段
    //   没有格式的字。它不进自定义 MIME，段内往返仍然走 f。
    if (!html.empty()) j["h"] = html;
    return j.dump();
}

ClipCapture captureRange(const Doc& doc, FlowVisibility vis, FlowPos lo, FlowPos hi) {
    ClipCapture out;
    if (!doc.root) {
        out.error = "没有装载文档";
        return out;
    }
    const FlowIndex idx(doc, vis);
    if (lo < 0) lo = 0;
    if (hi > idx.size()) hi = idx.size();
    if (lo >= hi) {
        out.error = "没有选中内容";
        return out;
    }
    const Node* root = pickRoot(*doc.root, idx, lo, hi);
    if (root == nullptr) {
        out.error = "取片段：找不到复制根";
        return out;
    }
    FlowPos wlo = lo;
    FlowPos whi = hi;
    widenToWholeBlocks(*root, idx, &wlo, &whi);
    std::string error;
    if (!collect(*root, idx, wlo, whi, &out.nodes, &error)) {
        out.nodes.clear();
        out.error = std::move(error);
        return out;
    }
    if (out.nodes.empty()) {
        out.error = "没有选中内容";
        return out;
    }
    out.lo = wlo;
    out.hi = whi;
    for (const NodeSpec& s : out.nodes) appendText(s, &out.text);
    out.ok = true;
    return out;
}

ClipWriteResult writeFragment(const std::vector<NodeSpec>& nodes, const SerializeOptions& opt) {
    ClipWriteResult result;
    if (nodes.empty()) {
        result.error = "片段是空的";
        return result;
    }
    recjson::StringPool strings;
    ordered_json arr = ordered_json::array();
    for (const NodeSpec& s : nodes) {
        if (isShell(s.kind)) {
            result.error = std::string("片段里含外壳容器：") + nodeKindName(s.kind);
            return result;
        }
        arr.push_back(nodeJsonOf(s, &strings));
    }
    {
        bool open_para = false;
        for (const NodeSpec& s : nodes) appendHtml(s, &result.html, &open_para, opt);
        if (open_para) result.html += "</p>";
    }
    ordered_json j = ordered_json::object();
    j[kV] = kFragmentVersion;
    if (!strings.list().empty()) j[kStrings] = strings.list();
    if (opt.font_ref) {
        std::unordered_set<int> used;
        for (const NodeSpec& s : nodes) collectFonts(s, &used);
        if (!used.empty()) {
            std::vector<int> sorted(used.begin(), used.end());
            std::sort(sorted.begin(), sorted.end());
            ordered_json fonts = ordered_json::object();
            for (int index : sorted) {
                const FontRef ref = opt.font_ref(index);
                ordered_json entry = ordered_json::object();
                if (!ref.postscript.empty()) entry["ps"] = ref.postscript;
                if (!ref.family.empty()) entry["family"] = ref.family;
                if (!ref.file.empty()) entry["file"] = ref.file;
                if (!entry.empty()) fonts[std::to_string(index)] = std::move(entry);
            }
            if (!fonts.empty()) j[kFonts] = std::move(fonts);
        }
    }
    j[kNodes] = std::move(arr);
    if (!recjson::validateStrings(j, &result.error)) return result;
    result.text = j.dump();
    result.ok = true;
    return result;
}

ClipReadResult readFragment(const std::string& text, const DeserializeOptions& opt) {
    ClipReadResult result;
    ordered_json j = ordered_json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        result.error = "片段不是 JSON 对象";
        return result;
    }
    if (!j.contains(kV) || !j[kV].is_number_integer()) {
        result.error = "片段没有版本号";
        return result;
    }
    if (j[kV].get<int>() > kFragmentVersion) {
        result.error = "片段的版本比这一侧新：" + std::to_string(j[kV].get<int>());
        return result;
    }
    std::vector<std::string> strings;
    if (j.contains(kStrings)) {
        if (!j[kStrings].is_array()) {
            result.error = "字符串表不是数组";
            return result;
        }
        for (const ordered_json& item : j[kStrings]) {
            if (!item.is_string()) {
                result.error = "字符串表里有非字符串";
                return result;
            }
            strings.push_back(item.get<std::string>());
        }
    }
    std::unordered_map<int, int> fonts;
    if (j.contains(kFonts) && opt.resolve_font) {
        if (!j[kFonts].is_object()) {
            result.error = "字体表不是对象";
            return result;
        }
        for (auto it = j[kFonts].begin(); it != j[kFonts].end(); ++it) {
            const ordered_json& item = it.value();
            FontRef ref;
            if (item.is_string()) {
                ref.file = item.get<std::string>();
            } else if (item.is_object()) {
                if (!recjson::readString(item, "ps", &ref.postscript) ||
                    !recjson::readString(item, "family", &ref.family) ||
                    !recjson::readString(item, "file", &ref.file)) {
                    result.error = "字体表里的项读不下去";
                    return result;
                }
            } else {
                result.error = "字体表里有既不是对象也不是字符串的项";
                return result;
            }
            int index = 0;
            {
                // wasm 关掉了异常，不能用 stoi：键是纯数字串时自己算，含别的字符就是坏数据。
                const std::string& key = it.key();
                bool digits = !key.empty();
                for (char c : key) {
                    if (c < '0' || c > '9') {
                        digits = false;
                        break;
                    }
                    index = index * 10 + (c - '0');
                }
                if (!digits) {
                    result.error = "字体表的键不是下标：" + key;
                    return result;
                }
            }
            const int local = opt.resolve_font(ref);
            if (local >= 0) fonts[index] = local;
        }
    }
    if (!j.contains(kNodes) || !j[kNodes].is_array()) {
        result.error = "片段没有节点数组";
        return result;
    }
    ClipContext ctx(std::move(strings), std::move(fonts));
    for (const ordered_json& item : j[kNodes]) {
        NodeSpec s;
        if (!parseNode(ctx, item, &s, &result.error)) return result;
        if (isShell(s.kind)) {
            result.error = std::string("片段里含外壳容器：") + nodeKindName(s.kind);
            return result;
        }
        result.nodes.push_back(std::move(s));
    }
    if (result.nodes.empty()) {
        result.error = "片段里没有节点";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace heditor::rev
