/**
 * 不可变修订层的判据。这一层要证明的只有三件事：编辑不碰旧值、没被改到的子树仍然共享、
 * 哈希能当"内容相同"的判据用。段落文本的正确性用一份朴素模型（vector<u32string>）对照，
 * 两者走同一串随机操作，每一步都比对。
 *
 * 判据必须含反向一半：新实现拒绝的操作，朴素模型也要认为它非法；哈希"应当相同"的地方
 * 相等、"应当不同"的地方不等 —— 只测相等会让一个恒返回 0 的哈希也通过。
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "core/doc/compile_result.h"
#include "core/doc/document.h"
#include "core/doc/flow.h"
#include "core/doc/flow_types.h"
#include "core/doc/fragment.h"
#include "core/doc/project.h"
#include "core/doc/serialize.h"
#include "core/doc/session.h"
#include "core/doc/step_types.h"
#include "core/doc/steps.h"
#include "core/doc/textbuf.h"
#include "core/layout/cell_index.h"

#include "demo/demo_documents.h"

#include "block_compare.h"

using namespace heditor;
using heditor::doc::Doc;
using testcmp::blockDiff;
using testcmp::sameBlock;
using testcmp::sameObjects;
using testcmp::sameTableShape;

namespace {

int g_checks = 0;
int g_failures = 0;

std::vector<uint32_t> cpsOf(const std::u32string& s) {
    std::vector<uint32_t> out;
    for (char32_t c : s) out.push_back(static_cast<uint32_t>(c));
    return out;
}

void expectTrue(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) {
        std::printf("  [PASS] %s\n", what.c_str());
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s\n", what.c_str());
    }
}

bool sameText(const std::vector<std::u32string>& a, const std::vector<std::u32string>& b) {
    return a == b;
}

std::string dump(const std::vector<std::u32string>& paras) {
    std::string s;
    for (size_t i = 0; i < paras.size(); ++i) {
        if (i) s += "|";
        for (char32_t c : paras[i]) s += (c < 128) ? static_cast<char>(c) : '?';
    }
    return s;
}

/**
 * 朴素模型：段落就是码点串的数组。它规定的语义只有四条，新实现必须逐条对上。
 */
struct Naive {
    std::vector<std::u32string> paras;

    void insert(int p, int cp, const std::u32string& t) { paras[p].insert(cp, t); }
    void erase(int p, int cp, int n) { paras[p].erase(cp, n); }
    void split(int p, int cp) {
        const std::u32string tail = paras[p].substr(cp);
        paras[p].resize(cp);
        paras.insert(paras.begin() + p + 1, tail);
    }
    void merge(int p) {
        paras[p] += paras[p + 1];
        paras.erase(paras.begin() + p + 1);
    }
};

/**
 * 随机编辑序列。返回 false 表示这一步是"新实现必须拒绝"的非法操作，
 * 调用方据此统计"有效性比例"，避免一整轮都在测拒绝路径。
 */
bool applyRandomOp(std::mt19937& rng, Doc* doc, Naive* naive, int* skipped) {
    std::uniform_int_distribution<int> para_pick(0, static_cast<int>(naive->paras.size()) - 1);
    const int p = para_pick(rng);
    const int len = static_cast<int>(naive->paras[p].size());
    std::uniform_int_distribution<int> cp_pick(0, len);
    const int cp = cp_pick(rng);
    std::uniform_int_distribution<int> op_pick(0, 3);
    const int op = op_pick(rng);

    if (op == 0) {
        std::uniform_int_distribution<int> ch_pick(0x61, 0x7a);
        const std::u32string add(1, static_cast<char32_t>(ch_pick(rng)));
        const auto r = doc::insertText(*doc, p, cp, add);
        if (!r.ok) {
            ++*skipped;
            return false;
        }
        naive->insert(p, cp, add);
        *doc = r.doc;
        return true;
    }
    if (op == 1) {
        if (len == 0) {
            ++*skipped;
            return false;
        }
        std::uniform_int_distribution<int> n_pick(1, len - cp);
        const int n = n_pick(rng);
        const auto r = doc::eraseText(*doc, p, cp, n);
        if (!r.ok) {
            ++*skipped;
            return false;
        }
        naive->erase(p, cp, n);
        *doc = r.doc;
        return true;
    }
    if (op == 2) {
        const auto r = doc::splitPara(*doc, p, cp);
        if (!r.ok) {
            ++*skipped;
            return false;
        }
        naive->split(p, cp);
        *doc = r.doc;
        return true;
    }
    if (p + 1 >= static_cast<int>(naive->paras.size())) {
        ++*skipped;
        return false;
    }
    const auto r = doc::mergePara(*doc, p);
    if (!r.ok) {
        ++*skipped;
        return false;
    }
    naive->merge(p);
    *doc = r.doc;
    return true;
}

/**
 * 控件基准记录：只有业务编码与类型非空，其余全默认。
 */
ControlSpec controlBase() {
    ControlSpec s;
    s.key = "k";
    s.controlType = "text";
    return s;
}

/**
 * 控件逐字段变体表：第一条是基准本身，其余每条只与基准差一个字段。
 *
 * 两条判据共用它。字段都进内容哈希（改一个就该换一个哈希），字段都过一遍落盘（读回来必须
 * 相等）。两处各列一份字段清单，迟早只改一处，而漏掉的表现都是静默的。
 */
std::vector<std::pair<const char*, ControlSpec>> controlVariants() {
    std::vector<std::pair<const char*, ControlSpec>> out;
    out.emplace_back("基准", controlBase());
    auto add = [&out](const char* name, const ControlSpec& s) { out.emplace_back(name, s); };
    {
        ControlSpec s = controlBase();
        s.key = "k2";
        add("key", s);
    }
    {
        ControlSpec s = controlBase();
        s.controlType = "date";
        add("controlType", s);
    }
    {
        ControlSpec s = controlBase();
        s.valueShape = ValueShape::Bool;
        add("valueShape", s);
    }
    {
        ControlSpec s = controlBase();
        s.value.flag = true;
        add("value.flag", s);
    }
    {
        ControlSpec s = controlBase();
        s.value.text = "v2";
        add("value.text", s);
    }
    {
        ControlSpec s = controlBase();
        s.value.items = {"a"};
        add("value.items", s);
    }
    {
        ControlSpec s = controlBase();
        s.readonly = true;
        add("readonly", s);
    }
    {
        ControlSpec s = controlBase();
        s.required = true;
        add("required", s);
    }
    {
        ControlSpec s = controlBase();
        s.structureLock = false;
        add("structureLock", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.width = Length::abs(ptToEmu(10.0));
        add("appearance.width", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.overflow = Overflow::Clip;
        add("appearance.overflow", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.underline = Underline::Whole;
        add("appearance.underline", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.left.on = true;
        add("border.left.on", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.right.on = true;
        add("border.right.on", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.top.on = true;
        add("border.top.on", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.bottom.on = true;
        add("border.bottom.on", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.left.on = true;
        s.appearance.border.left.style = BorderStyle::Dashed;
        add("border.left.style", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.left.on = true;
        s.appearance.border.left.width = Length::abs(ptToEmu(2.0));
        add("border.left.width", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.border.left.on = true;
        s.appearance.border.left.color = Color::rgb(255, 0, 0);
        add("border.left.color", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.glyph.kind = "cross";
        add("glyph.kind", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.glyph.width = Length::abs(ptToEmu(1.5));
        add("glyph.width", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.glyph.size = Length::abs(ptToEmu(9.0));
        add("glyph.size", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.glyph.color = Color::rgb(0, 0, 255);
        add("glyph.color", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.glyph.fill = Color::rgb(0, 255, 0);
        add("glyph.fill", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.align = Align::Center;
        add("appearance.align", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.placeholder = "提示";
        add("appearance.placeholder", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.styleRef = "s1";
        add("appearance.styleRef", s);
    }
    {
        ControlSpec s = controlBase();
        s.appearance.invalidStyleRef = "s2";
        add("appearance.invalidStyleRef", s);
    }
    {
        ControlSpec s = controlBase();
        s.validateRef = "v1";
        add("validateRef", s);
    }
    // 特有层：每一族的每一个字段都要换一个哈希。漏一个的表现是"改了这个属性，修订被当成
    //   没变直接丢掉"——画面上什么都不变，而且不报错。
    {
        ControlSpec s = controlBase();
        TextDetail d;
        d.filter = ContentFilter::Number;
        s.detail = d;
        add("detail.text.filter", s);
    }
    {
        ControlSpec s = controlBase();
        TextDetail d;
        d.filter = ContentFilter::Any;
        s.detail = d;
        add("detail.text.any", s);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.label = "甲";
        s.detail = d;
        add("detail.choice.label", s);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.options = {{"a", "甲"}};
        s.detail = d;
        add("detail.choice.options", s);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.options = {{"a", "甲"}};
        s.detail = d;
        ControlSpec s2 = s;
        std::get<ChoiceDetail>(s2.detail).options[0].label = "乙";
        add("detail.choice.option.label", s2);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.group = "g";
        s.detail = d;
        add("detail.choice.group", s);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.multi = true;
        s.detail = d;
        add("detail.choice.multi", s);
    }
    {
        ControlSpec s = controlBase();
        ChoiceDetail d;
        d.caret_barrier = false;
        s.detail = d;
        add("detail.choice.caret_barrier", s);
    }
    {
        ControlSpec s = controlBase();
        DropdownDetail d;
        d.options = {{"a", "甲"}};
        s.detail = d;
        add("detail.dropdown.options", s);
    }
    {
        ControlSpec s = controlBase();
        DropdownDetail d;
        d.free_input = true;
        s.detail = d;
        add("detail.dropdown.free_input", s);
    }
    {
        ControlSpec s = controlBase();
        DateDetail d;
        d.format = "yyyy";
        s.detail = d;
        add("detail.date.format", s);
    }
    {
        ControlSpec s = controlBase();
        DateDetail d;
        d.units = kDateUnitYear | kDateUnitMonth;
        s.detail = d;
        add("detail.date.units", s);
    }
    {
        ControlSpec s = controlBase();
        CustomDetail d;
        d.props["p"] = "v";
        s.detail = d;
        add("detail.custom.props", s);
    }
    return out;
}

/**
 * 两棵修订树是不是同一份：身份、种类、隐藏标记、段落数、哈希、子节点逐个比。
 *
 * 载荷不在这里比：它进哈希，哈希相同就够了。身份要单独比，因为哈希刻意不含身份，而
 * "读回来之后新旧节点号撞上"恰恰只能靠身份抓。
 */
bool sameTree(const doc::Ref& a, const doc::Ref& b) {
    if (!a || !b) return a == b;
    if (a->id != b->id || a->kind != b->kind || a->hidden != b->hidden) return false;
    if (a->paras != b->paras) return false;
    if (a->hash_lo != b->hash_lo || a->hash_hi != b->hash_hi) return false;
    if (a->children.size() != b->children.size()) return false;
    for (size_t i = 0; i < a->children.size(); ++i) {
        if (!sameTree(a->children[i], b->children[i])) return false;
    }
    return true;
}

/**
 * 表格夹具：两行三列的布局，第一格纵向合并两行，表前后各一段正文。
 *
 * 两个参数是容器几何的可变项：换一套值重建一份，节点号与每个格里的内容都逐个相同，只有
 * 表格/行/格的记录变了。这正是"容器几何变了、块的内容一个都没动"的形状。
 *
 * 段落号：0 是表前，1 到 5 依次是五个格（A0 / B0 / C0 / A1 / B1），6 是表后。
 */
doc::Doc makeTableDoc(const Length& col0, const Length& row0_height) {
    int id = 3;  // 1 是 Document，2 是 Section
    auto run = [&id](const std::u32string& t) {
        return doc::makeNode(id++, NodeKind::TextRun, doc::RunPayload{cpsOf(t), CharFormat{}, {}},
                             {});
    };
    auto mark = [&id]() {
        return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
    };
    auto cell = [&](const std::u32string& t, const CellSpec& spec) {
        return doc::makeNode(id++, NodeKind::TableCell, spec, {run(t), mark()});
    };

    TableSpec ts;
    ts.columns.push_back(TableColumn{"patient.id", "住院号", "text", "input",
                                     Length::abs(ptToEmu(12.0)), Align::Center});
    ts.grid_cols = {col0, Length::abs(ptToEmu(40.0)), Length::percent(1, 2)};
    ts.width = Length::percent(1, 1);
    ts.align = Align::Center;

    RowSpec r0;
    r0.is_header = true;
    r0.can_split_by_page = false;
    r0.page_break_before = true;
    r0.height = row0_height;
    r0.height_rule = HeightRule::AtLeast;

    CellSpec merged;
    merged.row_span = 2;
    merged.v_align = CellVAlign::Center;

    const doc::Ref row0 =
        doc::makeNode(id++, NodeKind::TableRow, r0,
                      {cell(U"A0", merged), cell(U"B0", CellSpec{}), cell(U"C0", CellSpec{})});
    const doc::Ref row1 = doc::makeNode(id++, NodeKind::TableRow, RowSpec{},
                                        {cell(U"A1", CellSpec{}), cell(U"B1", CellSpec{})});
    const doc::Ref table = doc::makeNode(id++, NodeKind::Table, ts, {row0, row1});
    const doc::Ref section = doc::makeNode(2, NodeKind::Section, SectionSpec{},
                                           {run(U"表前"), mark(), table, run(U"表后"), mark()});
    return doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section}), id);
}

/**
 * 原子与域文档的 doc 原生版本。
 * @param cache 域的缓存值
 * @param first_image 第一张图片的资源标识
 * @returns 修订
 */
doc::Doc makeAtomDoc(const std::string& cache, const std::string& first_image = "res/a.png") {
    int id = 3;
    auto run = [&](const std::u32string& text) {
        return doc::makeNode(id++, NodeKind::TextRun,
                             doc::RunPayload{cpsOf(text), CharFormat{}, {}}, {});
    };
    auto mark = [&]() {
        return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
    };
    auto image = [&](const ImageSpec& spec) {
        return doc::makeNode(id++, NodeKind::Image, spec, {});
    };

    ImageSpec px_only;
    px_only.resource_id = first_image;
    px_only.px_w = 200;
    px_only.px_h = 100;

    ImageSpec width_only;
    width_only.resource_id = "res/b.png";
    width_only.width = Length::abs(ptToEmu(30.0));

    ImageSpec height_only;
    height_only.resource_id = "res/c.png";
    height_only.px_w = 100;
    height_only.px_h = 50;
    height_only.height = Length::abs(ptToEmu(20.0));

    ImageSpec pct;
    pct.resource_id = "res/d.png";
    pct.px_w = 100;
    pct.px_h = 100;
    pct.width = Length::percent(1, 2);

    FormulaSpec formula;
    formula.expr = "x^2";

    FieldSpec field;
    field.kind = FieldKind::DocProperty;
    field.name = "patient.name";
    field.cache = cache;

    const doc::Ref section =
        doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                      {run(U"图"), image(px_only), run(U"后"), mark(), image(width_only), mark(),
                       image(height_only), mark(), image(pct), mark(),
                       doc::makeNode(id++, NodeKind::Formula, formula, {}), mark(), run(U"值"),
                       doc::makeNode(id++, NodeKind::Field, field, {}), mark()});
    const NodeId root_id = id++;
    const doc::Ref root = doc::makeNode(root_id, NodeKind::Document, {}, {section});
    return doc::adopt(root, id);
}

/**
 * 嵌套表格的 doc 原生版本，形状与 makeNestedTableTree 相同。
 * @param inner_col0 内表第一列宽
 * @param outer_col0 外表第一列宽
 * @returns 修订
 */
doc::Doc makeNestedTableDoc(const Length& inner_col0,
                            const Length& outer_col0 = Length::abs(ptToEmu(50.0))) {
    int id = 3;
    auto run = [&](const std::u32string& text) {
        return doc::makeNode(id++, NodeKind::TextRun,
                             doc::RunPayload{cpsOf(text), CharFormat{}, {}}, {});
    };
    auto mark = [&]() {
        return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
    };
    auto cell = [&](const std::u32string& text) {
        return doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run(text), mark()});
    };

    TableSpec inner_table;
    inner_table.grid_cols = {inner_col0, Length::abs(ptToEmu(20.0))};
    inner_table.width = Length::percent(1, 1);
    const doc::Ref inner_row =
        doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {cell(U"i1"), cell(U"i2")});
    const doc::Ref inner = doc::makeNode(id++, NodeKind::Table, inner_table, {inner_row});
    const doc::Ref outer_cell0 =
        doc::makeNode(id++, NodeKind::TableCell, CellSpec{}, {run(U"A"), mark(), inner});

    TableSpec outer_table;
    outer_table.grid_cols = {outer_col0, Length::abs(ptToEmu(50.0))};
    outer_table.width = Length::percent(1, 1);
    const doc::Ref outer_row =
        doc::makeNode(id++, NodeKind::TableRow, RowSpec{}, {outer_cell0, cell(U"B")});
    const doc::Ref outer = doc::makeNode(id++, NodeKind::Table, outer_table, {outer_row});
    const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                           {run(U"前"), mark(), outer, run(U"后"), mark()});
    const NodeId root_id = id++;
    const doc::Ref root = doc::makeNode(root_id, NodeKind::Document, {}, {section});
    return doc::adopt(root, id);
}

/**
 * 跨段控件的 rev 版本。
 * @returns 修订
 */
doc::Doc makeSpanningControlDoc() {
    int id = 3;
    auto run = [&](const std::u32string& text) {
        return doc::makeNode(id++, NodeKind::TextRun,
                             doc::RunPayload{cpsOf(text), CharFormat{}, {}}, {});
    };
    auto mark = [&]() {
        return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
    };
    ControlSpec control;
    control.key = "k";
    control.controlType = "text";
    const doc::Ref cc =
        doc::makeNode(id++, NodeKind::ContentControl, control, {run(U"乙"), mark(), run(U"丙")});
    const doc::Ref section = doc::makeNode(id++, NodeKind::Section, SectionSpec{},
                                           {run(U"甲"), cc, mark(), run(U"丁"), mark()});
    const NodeId root_id = id++;
    const doc::Ref root = doc::makeNode(root_id, NodeKind::Document, {}, {section});
    return doc::adopt(root, id);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== 不可变修订层：路径复制、结构共享、内容哈希 ===\n");

    std::printf("\n[1] 建文档与读出\n");
    {
        const Doc d = doc::makeTextDoc({U"甲乙", U"", U"丙"});
        expectTrue(doc::paraCount(d) == 3, "三段（含一个空段）");
        expectTrue(sameText(doc::paraTexts(d), {U"甲乙", U"", U"丙"}), "逐段文本与输入一致");
        const Doc empty = doc::makeTextDoc({});
        expectTrue(doc::paraCount(empty) == 0, "空文档：零段");
        expectTrue(doc::paraTexts(empty).empty(), "空文档：读不出段落");
    }

    std::printf("\n[2] 随机编辑序列与朴素模型逐步比对\n");
    {
        std::mt19937 rng(20261019u);
        Doc doc = doc::makeTextDoc({U"abc", U"", U"de"});
        Naive naive{{U"abc", U"", U"de"}};
        int mismatches = 0;
        int skipped = 0;
        const int kSteps = 400;
        for (int step = 0; step < kSteps; ++step) {
            applyRandomOp(rng, &doc, &naive, &skipped);
            if (!sameText(doc::paraTexts(doc), naive.paras)) {
                ++mismatches;
                std::printf("      第 %d 步不一致：新 %s / 旧 %s\n", step,
                            dump(doc::paraTexts(doc)).c_str(), dump(naive.paras).c_str());
                break;
            }
        }
        expectTrue(mismatches == 0, "连续 400 步编辑，每一步的逐段文本都与朴素模型一致");
        expectTrue(skipped < kSteps, "随机序列真的执行了编辑（不是全被拒绝）");
        std::printf("      最终 %d 段：%s\n", doc::paraCount(doc), dump(naive.paras).c_str());
    }

    std::printf("\n[3] 旧值不被改动\n");
    {
        Doc doc = doc::makeTextDoc({U"hello", U"world"});
        std::vector<Doc> history;
        std::vector<std::vector<std::u32string>> expected;
        for (int i = 0; i < 10; ++i) {
            history.push_back(doc);
            expected.push_back(doc::paraTexts(doc));
            const auto r = doc::insertText(doc, 0, 0, U"x");
            if (!r.ok) break;
            doc = r.doc;
        }
        int bad = 0;
        for (size_t i = 0; i < history.size(); ++i) {
            if (!sameText(doc::paraTexts(history[i]), expected[i])) ++bad;
        }
        expectTrue(bad == 0, "十次编辑之后，之前每一份修订读出来的文本都没变");
        expectTrue(history.size() >= 5, "确实积累了多份修订");
    }

    std::printf("\n[4] 结构共享\n");
    {
        Doc doc = doc::makeTextDoc({U"aaa", U"bbb", U"ccc"});
        const doc::Ref before_root = doc.root;
        const doc::Ref before_sec = doc.root->children.front();
        const auto r = doc::insertText(doc, 0, 1, U"z");
        expectTrue(r.ok, "改第 0 段成功");
        const doc::Ref after_sec = r.doc.root->children.front();
        expectTrue(before_root.get() != r.doc.root.get(), "根节点换了一个（路径复制）");
        expectTrue(before_sec.get() != after_sec.get(), "正文那一节换了一个（它在路径上）");
        expectTrue(before_sec->children.size() == after_sec->children.size(), "子节点个数不变");
        bool shared = true;
        for (size_t i = 2; i < before_sec->children.size(); ++i) {
            if (before_sec->children[i].get() != after_sec->children[i].get()) shared = false;
        }
        expectTrue(shared, "没被改到的段落节点仍是同一批对象（不是整篇重建）");
    }

    std::printf("\n[5] 内容哈希\n");
    {
        const Doc a = doc::makeTextDoc({U"abc"});
        Doc b = doc::makeTextDoc({U"ab"});
        const auto r = doc::insertText(b, 0, 2, U"c");
        expectTrue(r.ok, "第二条构造路径：在 ab 后面插 c");
        b = r.doc;
        expectTrue(sameText(doc::paraTexts(a), doc::paraTexts(b)), "两条路径的文本相同");
        expectTrue(a.lo == b.lo && a.hi == b.hi, "内容相同则哈希相同（与构造路径无关）");

        Doc c = doc::makeTextDoc({U"abc"});
        const auto r2 = doc::insertText(c, 0, 1, U"X");
        expectTrue(r2.ok, "改一个码点成功");
        c = r2.doc;
        expectTrue(c.lo != a.lo || c.hi != a.hi, "改一个码点，哈希就不同");

        const auto r3 = doc::eraseText(c, 0, 1, 1);
        expectTrue(r3.ok, "把那一个码点删回去成功");
        expectTrue(r3.doc.lo == a.lo && r3.doc.hi == a.hi, "改回原样，哈希回到原值");

        const Doc two = doc::makeTextDoc({U"ab", U""});
        const Doc one = doc::makeTextDoc({U"ab"});
        expectTrue(two.lo != one.lo || two.hi != one.hi,
                   "多一个空段，哈希不同（哈希表达结构，不只是拼起来的文本）");
    }

    std::printf("\n[6] 非法操作被拒绝，且不给一个看起来没变的文档\n");
    {
        const Doc d = doc::makeTextDoc({U"ab", U"cd"});
        expectTrue(!doc::insertText(d, 2, 0, U"x").ok, "段落越界被拒");
        expectTrue(!doc::insertText(d, 0, 3, U"x").ok, "码点越界被拒");
        expectTrue(!doc::eraseText(d, 0, 1, 5).ok, "跨段删除被拒（不替调用方猜语义）");
        expectTrue(!doc::mergePara(d, 1).ok, "最后一段没有下一段可合并");
        expectTrue(!doc::splitPara(d, 0, 9).ok, "分段位置越界被拒");
        const auto r = doc::eraseText(d, 0, 0, 0);
        expectTrue(r.ok && r.doc.lo == d.lo, "删 0 个码点是合法的空操作，哈希不变");
    }

    std::printf("\n[7] 控件：插入、读出、隐藏\n");
    {
        ControlSpec spec;
        spec.key = "patient_name";
        spec.controlType = "text";
        spec.required = true;

        Doc d = doc::makeTextDoc({U"姓名："});
        NodeId cc = kInvalidNodeId;
        const auto r = doc::insertControl(d, 0, 3, spec, U"张三", &cc);
        expectTrue(r.ok, "在段内插入控件成功");
        d = r.doc;
        expectTrue(doc::paraCount(d) == 1, "控件不切段：仍是一段");
        expectTrue(doc::paraTexts(d)[0] == U"姓名：张三", "控件里的文字算作这一段自己的文字");
        const doc::Node* n = doc::findNode(d, cc);
        expectTrue(n != nullptr && n->kind == NodeKind::ContentControl, "按节点号找得到那个控件");
        const auto& got = std::get<ControlSpec>(n->payload);
        expectTrue(got.key == "patient_name" && got.required, "控件记录原样存住（字段没有丢）");

        const auto proj = doc::project(d, StyleSheet{}, {});
        expectTrue(proj.ok() && proj.blocks.size() == 1, "投影产出一块、没有诊断");
        const auto& objs = proj.blocks[0].objects;
        expectTrue(objs.size() == 2 && objs[0].kind == InlineObjectKind::ControlStart &&
                       objs[0].cp == 3 && objs[1].kind == InlineObjectKind::ControlEnd &&
                       objs[1].cp == 5,
                   "两个边界锚在控件内容的起止（3 与 5），不占文本位置");
        expectTrue(proj.controls.size() == 1 && proj.controls[0].block == 0 &&
                       proj.controls[0].cp == 3 && proj.controls[0].end_cp == 5,
                   "控件表给出它在块坐标里的跨度");

        // 隐藏只在呈现态生效：编辑态照常进流，呈现态整棵摘除。
        const auto rh = doc::setHidden(d, cc, true);
        expectTrue(rh.ok, "设置隐藏成功");
        const doc::Projection design = doc::project(rh.doc, StyleSheet{}, {});
        const doc::Projection display =
            doc::project(rh.doc, StyleSheet{}, doc::ViewParams{FlowVisibility::Display});
        expectTrue(design.blocks.size() == 1 && design.blocks[0].cp_text.size() == 5 &&
                       design.blocks[0].objects.size() == 2,
                   "编辑态：隐藏控件照常参与投影（作者要能选中它改回来）");
        expectTrue(display.blocks.size() == 1 && display.blocks[0].cp_text.size() == 3 &&
                       display.blocks[0].objects.empty() && display.controls.empty(),
                   "呈现态：隐藏的控件整棵摘除，文字与边界都不在了");

        const auto rsame = doc::setHidden(rh.doc, cc, true);
        expectTrue(rsame.ok && rsame.doc.lo == rh.doc.lo && rsame.doc.hi == rh.doc.hi,
                   "重复设置同一个隐藏值是空操作，哈希不变");
    }

    std::printf("\n[8] 控件投影：边界、来源身份与呈现态\n");
    {
        ControlSpec spec;
        spec.key = "patient_name";
        spec.controlType = "text";
        spec.appearance.placeholder = "请填写";
        spec.appearance.underline = Underline::BlankOnly;
        spec.readonly = true;

        Doc rdoc = doc::makeTextDoc({U"姓名：", U"", U"尾"});
        NodeId rcc = kInvalidNodeId;
        const auto rc = doc::insertControl(rdoc, 0, 3, spec, U"张三", &rcc);
        expectTrue(rc.ok, "修订层：插入控件");
        rdoc = rc.doc;

        const StyleSheet sheet;
        const doc::Projection pr = doc::project(rdoc, sheet, {});
        expectTrue(pr.ok(), "投影没有诊断");
        expectTrue(pr.blocks.size() == 3, "三段三块");
        expectTrue(pr.blocks[0].objects.size() == 2 && pr.blocks[0].objects[1].cp == 5,
                   "第一块的边界锚点：开括号与内容起始");

        bool source_ok = pr.keys.size() == pr.blocks.size();
        for (size_t i = 0; source_ok && i < pr.blocks.size(); ++i) {
            const doc::Node* mark = doc::findNode(rdoc, pr.keys[i].source.mark);
            if (mark == nullptr || mark->kind != NodeKind::ParagraphMark) source_ok = false;
        }
        expectTrue(source_ok, "每块的来源身份都指向一枚段落标记");

        const auto rh = doc::setHidden(rdoc, rcc, true);
        const doc::Projection pd =
            doc::project(rh.doc, sheet, doc::ViewParams{FlowVisibility::Display});
        expectTrue(pd.blocks.size() == 3 && pd.controls.empty(),
                   "呈现态：隐藏控件之后两段，控件表为空");
    }

    std::printf("\n[9] 块哈希：改一段只动那一段，损伤区间精确\n");
    {
        Doc d = doc::makeTextDoc({U"aa", U"bb", U"cc", U"dd", U"ee"});
        const auto h0 = doc::blockHashes(d, FlowVisibility::Design);
        expectTrue(h0.size() == 5, "五段五块");

        const auto r = doc::insertText(d, 2, 1, U"X");
        const auto h1 = doc::blockHashes(r.doc, FlowVisibility::Design);
        expectTrue(h0[0] == h1[0] && h0[1] == h1[1] && h0[3] == h1[3] && h0[4] == h1[4],
                   "没改到的块哈希一个都没变（缓存因此可以照用）");
        expectTrue(!(h0[2] == h1[2]), "改到的那一块哈希变了");
        const doc::BlockDamage d1 = doc::diffBlocks(h0, h1);
        expectTrue(d1.lo == 2 && d1.hi == 3, "损伤正好是改到的那一块");

        const auto sp = doc::splitPara(d, 0, 0);
        const doc::BlockDamage d2 =
            doc::diffBlocks(h0, doc::blockHashes(sp.doc, FlowVisibility::Design));
        expectTrue(d2.lo == 0 && d2.hi == 1, "段首插一段：损伤只有新出现的那一块（后缀整体复用）");

        const auto mg = doc::mergePara(d, 1);
        const doc::BlockDamage d3 =
            doc::diffBlocks(h0, doc::blockHashes(mg.doc, FlowVisibility::Design));
        expectTrue(d3.lo == 1 && d3.hi == 2, "合并第 1、2 段：损伤只有合并后的那一块");

        const auto noop = doc::insertText(d, 0, 0, U"");
        expectTrue(doc::diffBlocks(h0, doc::blockHashes(noop.doc, FlowVisibility::Design)).empty(),
                   "空编辑：损伤为空（不会凭空触发一轮重排）");

        // 整篇删空：区间长度是 0，但"变了"这件事必须说出口 —— 页表要清掉。
        const Doc empty = doc::makeTextDoc({});
        const doc::BlockDamage d4 =
            doc::diffBlocks(h0, doc::blockHashes(empty, FlowVisibility::Design));
        expectTrue(!d4.empty() && d4.count() == 0, "整篇删空：损伤区间为空但标记为变了");

        // 反方向：改回原样，哈希回到原值。
        const auto back = doc::eraseText(r.doc, 2, 1, 1);
        expectTrue(doc::diffBlocks(h0, doc::blockHashes(back.doc, FlowVisibility::Design)).empty(),
                   "改动撤回之后，损伤回到空（哈希是内容判据，不是版本号）");
    }

    std::printf("\n[10] 控件的每一个字段都进内容哈希\n");
    {
        auto hashOf = [](const ControlSpec& s) {
            const auto r = doc::insertControl(doc::makeTextDoc({U"x"}), 0, 0, s, U"v");
            return doc::blockHashes(r.doc, FlowVisibility::Design).at(0);
        };
        const auto variants = controlVariants();
        const doc::BlockHash h0 = hashOf(variants.front().second);

        int missed = 0;
        for (size_t i = 1; i < variants.size(); ++i) {
            if (hashOf(variants[i].second) == h0) {
                ++missed;
                std::printf("      字段没进哈希：%s\n", variants[i].first);
            }
        }
        expectTrue(missed == 0, "控件的每个字段都进哈希（" + std::to_string(variants.size() - 1) +
                                    " 个字段逐个试过）");
    }

    std::printf("\n[11] 损伤定位：哈希下降必须与块哈希比对一致\n");
    {
        const std::vector<std::u32string> base = {U"aa", U"bb", U"cc", U"dd", U"ee"};

        // 同一份修订：根一个节点就判完。
        const Doc same = doc::makeTextDoc(base);
        const doc::Damage none = doc::damage(same, same, FlowVisibility::Design);
        expectTrue(!none.range.changed, "同一份修订：没有损伤");
        expectTrue(none.visited == 1, "同一份修订：只比较了根一个节点（整篇没变是 O(1)）");

        // 段内改字（块数不变）：与块哈希比对逐字段一致。
        const Doc d0 = doc::makeTextDoc(base);
        const Doc d1 = doc::insertText(d0, 2, 1, U"X").doc;
        const doc::Damage inplace = doc::damage(d0, d1, FlowVisibility::Design);
        const doc::BlockDamage ref = doc::diffBlocks(doc::blockHashes(d0, FlowVisibility::Design),
                                                     doc::blockHashes(d1, FlowVisibility::Design));
        expectTrue(inplace.range.changed && ref.changed, "段内改字：两边都判为有损伤");
        expectTrue(inplace.range.lo == ref.lo && inplace.range.hi == ref.hi,
                   "段内改字：区间与块哈希比对完全一致（不许漏，也不许白报）");
        expectTrue(inplace.range.lo == 2 && inplace.range.hi == 3, "段内改字：正好是第 2 块");

        // 空编辑：没有变化，一步剪掉。
        const Doc noop = doc::insertText(d0, 0, 0, U"").doc;
        const doc::Damage dn = doc::damage(d0, noop, FlowVisibility::Design);
        expectTrue(!dn.range.changed && dn.visited == 1, "空编辑：没有损伤，且只看了根节点");

        // 结构改动：分段多一块、合并少一块，两者都只动一块。
        const Doc sp = doc::splitPara(d0, 0, 0).doc;
        const doc::Damage ds = doc::damage(d0, sp, FlowVisibility::Design);
        // cp=0 分段时模型会丢掉原 run、再建一个内容相同的新 run（新节点号），于是"删一个、
        //   插一个"被分别记成两块。多报的那一块内容与旧块完全相同，重排时命中块缓存 ——
        //   这里不为它加"按哈希跨位配对"的启发式：那类规则的价值只有一次缓存查询，
        //   而它的失效方式（把真变化的两个节点当成同一份内容）是静默的。区间上限钉在 2。
        expectTrue(ds.range.changed && ds.range.lo == 0 && ds.range.hi >= 1 && ds.range.hi <= 2,
                   "段首分段：从第 0 块起，最多两块（实际 [" + std::to_string(ds.range.lo) + "," +
                       std::to_string(ds.range.hi) + ")）");

        const Doc mg = doc::mergePara(d0, 1).doc;
        const doc::Damage dm = doc::damage(d0, mg, FlowVisibility::Design);
        const doc::BlockDamage rm = doc::diffBlocks(doc::blockHashes(d0, FlowVisibility::Design),
                                                    doc::blockHashes(mg, FlowVisibility::Design));
        expectTrue(dm.range.lo == 1 && dm.range.hi == 2, "合并两段：只有合并后的那一块（实际 [" +
                                                             std::to_string(dm.range.lo) + "," +
                                                             std::to_string(dm.range.hi) + ")）");
        expectTrue(rm.lo == dm.range.lo && rm.hi == dm.range.hi,
                   "合并两段：块哈希比对也对得上（后缀对齐吸收了挪位，实际 [" +
                       std::to_string(rm.lo) + "," + std::to_string(rm.hi) + ")）");

        // 尾部删空：块数变了但没有任何块要重排，与块哈希比对同一口径。
        const Doc empty = doc::makeTextDoc({});
        const doc::BlockDamage re =
            doc::diffBlocks(doc::blockHashes(d0, FlowVisibility::Design),
                            doc::blockHashes(empty, FlowVisibility::Design));
        const doc::Damage de = doc::damage(d0, empty, FlowVisibility::Design);
        expectTrue(de.range.changed && de.range.lo == re.lo && de.range.hi == re.hi,
                   "整篇删空：区间与块哈希比对一致（空区间，但标记为变了）");

        // 随机序列：块数不变的那些步必须与块哈希比对完全一致。
        {
            std::mt19937 rng(20261019u);
            Doc cur = doc::makeTextDoc({U"aa", U"bb"});  // 两段：够切也够并
            int compared = 0;
            int mismatched = 0;
            for (int step = 0; step < 300; ++step) {
                const int op = static_cast<int>(rng() % 4);
                const int paras = doc::paraCount(cur);
                const auto texts = doc::paraTexts(cur);
                const int p =
                    paras > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(paras)) : 0;
                const int len =
                    paras > 0 ? static_cast<int>(texts[static_cast<size_t>(p)].size()) : 0;
                doc::EditResult r;
                if (op == 0) {
                    r = doc::insertText(
                        cur, p,
                        len > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(len + 1)) : 0,
                        U"z");
                } else if (op == 1) {
                    r = doc::eraseText(cur, p, 0, 1 + static_cast<int>(rng() % 2));
                } else if (op == 2) {
                    r = doc::splitPara(
                        cur, p,
                        len > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(len + 1)) : 0);
                } else {
                    r = doc::mergePara(cur, p);
                }
                if (!r.ok) continue;
                const std::vector<doc::BlockHash> hb =
                    doc::blockHashes(cur, FlowVisibility::Design);
                const std::vector<doc::BlockHash> ha =
                    doc::blockHashes(r.doc, FlowVisibility::Design);
                const doc::Damage got = doc::damage(cur, r.doc, FlowVisibility::Design);
                const bool count_same = hb.size() == ha.size();
                if (count_same) {
                    ++compared;
                    const doc::BlockDamage want = doc::diffBlocks(hb, ha);
                    if (got.range.changed != want.changed || got.range.lo != want.lo ||
                        got.range.hi != want.hi) {
                        ++mismatched;
                    }
                }
                // 每一类操作都必须至少报一次损伤（除非它本来就是空操作）。
                if (hb != ha && !got.range.changed) ++mismatched;
                cur = r.doc;
            }
            expectTrue(compared > 40, "随机序列里有足够的\"块数不变\"步参与比对（" +
                                          std::to_string(compared) + " 步）");
            expectTrue(mismatched == 0, "300 步随机编辑：没有一步与块哈希比对不一致");
        }

        // 局部性：visited 只跟路径上的节点数有关，与段落文字量无关。
        {
            const std::vector<std::u32string> short_doc(400, U"段");
            const std::vector<std::u32string> long_doc(400, std::u32string(200, U'字'));
            const Doc a1 = doc::makeTextDoc(short_doc);
            const Doc b1 = doc::insertText(a1, 399, 1, U"X").doc;
            const Doc a2 = doc::makeTextDoc(long_doc);
            const Doc b2 = doc::insertText(a2, 399, 1, U"X").doc;
            const doc::Damage m1 = doc::damage(a1, b1, FlowVisibility::Design);
            const doc::Damage m2 = doc::damage(a2, b2, FlowVisibility::Design);
            expectTrue(m1.range.lo == 399 && m1.range.hi == 400,
                       "400 段改最后一段：区间正好是第 399 块");
            expectTrue(m1.visited == m2.visited,
                       "visited 与段落文字量无关（同一棵树形状，走到同一个地方）");
            std::printf("      400 段：visited %d、scanned %d（根下孩子 800 个）\n", m1.visited,
                        m1.scanned);

            // 同口径对照：块哈希比对要先把整篇投影一遍。只打印，不做时间断言（机器有噪声）。
            using clock = std::chrono::steady_clock;
            int sink = 0;
            const auto t0 = clock::now();
            for (int k = 0; k < 200; ++k)
                sink += doc::damage(a1, b1, FlowVisibility::Design).visited;
            const auto t1 = clock::now();
            for (int k = 0; k < 200; ++k) {
                sink += doc::diffBlocks(doc::blockHashes(a1, FlowVisibility::Design),
                                        doc::blockHashes(b1, FlowVisibility::Design))
                            .hi;
            }
            const auto t2 = clock::now();
            const double ms_damage =
                std::chrono::duration<double, std::milli>(t1 - t0).count() / 200.0;
            const double ms_hash =
                std::chrono::duration<double, std::milli>(t2 - t1).count() / 200.0;
            std::printf(
                "      200 次对照：哈希下降 %.3f ms/次，块哈希比对 %.3f ms/次（快 %.1f 倍）\n",
                ms_damage, ms_hash, ms_damage > 0 ? ms_hash / ms_damage : 0.0);
            expectTrue(sink > 0, "两条路算出的结果都被用掉（对照没有被优化掉）");
        }
    }

    std::printf("\n[12] 控件内部的编辑：与顶层走同一条路\n");
    {
        ControlSpec spec;
        spec.key = "patient_name";
        spec.controlType = "text";

        Doc d = doc::makeTextDoc({U"前"});
        NodeId cc = kInvalidNodeId;
        const auto rc = doc::insertControl(d, 0, 1, spec, U"ab", &cc);
        expectTrue(rc.ok, "前字之后插入一个控件（内容是 ab）");
        d = rc.doc;
        expectTrue(doc::paraTexts(d)[0] == U"前ab", "控件里的字算作这一段自己的文字");

        const doc::Node* run_before = d.root->children.front()->children.front().get();

        // 位置 2 落在控件内部的 run 里（前=0，a=1，b=2）。
        const auto ri = doc::insertText(d, 0, 2, U"X");
        expectTrue(ri.ok, "在控件内部打字（不再被拒绝）");
        const Doc inside = ri.doc;
        expectTrue(doc::paraTexts(inside)[0] == U"前aXb", "字落在控件内部、位置正确（前aXb）");
        expectTrue(doc::paraCount(inside) == 1, "控件内部打字不切段");
        expectTrue(doc::findNode(inside, cc) != nullptr, "控件还在，没有被打字挤掉");
        expectTrue(inside.root->children.front()->children.front().get() == run_before,
                   "控件之前那一枚 run 是同一个指针（容器之外的节点一个都没复制）");
        expectTrue(doc::paraTexts(d)[0] == U"前ab", "旧修订没有被改动");

        // 在控件内部回车：段落标记落进控件，控件因此跨段。
        const auto rsp = doc::splitPara(inside, 0, 2);
        expectTrue(rsp.ok, "在控件内部回车（不再被拒绝）");
        const Doc crossed = rsp.doc;
        expectTrue(doc::paraCount(crossed) == 2, "控件内部回车真的多出一段");
        expectTrue(doc::paraTexts(crossed)[0] == U"前a" && doc::paraTexts(crossed)[1] == U"Xb",
                   "两段的文字都对（控件因此横跨两段）");

        {
            const StyleSheet sheet;
            const doc::Projection pr = doc::project(crossed, sheet, {});
            expectTrue(pr.ok() && pr.blocks.size() == 2, "投影两段两块");
            expectTrue(pr.controls.size() == 1, "仍是同一个控件");
            expectTrue(pr.controls[0].block == 0 && pr.controls[0].end_block == 1,
                       "控件跨度横跨两块（起在第 0 块、止在第 1 块）");
        }

        // 损伤：分段之后两块都要重排。
        const doc::Damage dm = doc::damage(inside, crossed, FlowVisibility::Design);
        expectTrue(dm.range.changed && dm.range.lo == 0 && dm.range.hi == 2,
                   "控件内分段：损伤覆盖两块（实际 [" + std::to_string(dm.range.lo) + "," +
                       std::to_string(dm.range.hi) + ")）");

        // 反向：越界的码点仍然被拒，且不返回一份"看起来没变"的文档。
        const auto bad = doc::insertText(crossed, 0, 99, U"Z");
        expectTrue(!bad.ok && !bad.error.empty(), "控件内部码点越界仍然被拒，且带原因");
        const auto bad2 = doc::insertText(crossed, 5, 0, U"Z");
        expectTrue(!bad2.ok, "段落越界仍然被拒");
    }

    std::printf("\n[13] 往返：写完再读回来，身份、哈希与记录字段一个不少\n");
    {
        const Doc plain = doc::makeTextDoc({U"第一段", U"", U"abc"});
        const auto typed = doc::insertText(plain, 2, 1, U"中");
        expectTrue(typed.ok, "准备：段内插字");

        ControlSpec spec = controlBase();
        spec.valueShape = ValueShape::Enum;
        spec.value.text = "opt";
        spec.appearance.border.bottom.on = true;
        spec.appearance.placeholder = "请填写";
        NodeId control = kInvalidNodeId;
        const auto placed = doc::insertControl(typed.doc, 0, 1, spec, U"标签", &control);
        expectTrue(placed.ok && control != kInvalidNodeId, "准备：插入控件并拿到节点号");
        const Doc doc = doc::setHidden(placed.doc, control, true).doc;

        const auto ser = doc::serialize(doc);
        expectTrue(ser.ok && !ser.text.empty(),
                   ser.ok ? std::string("序列化成功且非空") : ("序列化失败：" + ser.error));
        Doc back;
        const auto de = doc::deserialize(ser.text, &back);
        expectTrue(de.ok, de.ok ? std::string("反序列化成功") : ("反序列化失败：" + de.error));
        expectTrue(back.lo == doc.lo && back.hi == doc.hi,
                   "全文哈希相同（参与哈希的字段一个都没漏）");
        expectTrue(back.next_id == doc.next_id, "分配器水位相同");
        expectTrue(doc::paraCount(back) == doc::paraCount(doc), "段落数相同");
        expectTrue(doc::paraTexts(back) == doc::paraTexts(doc), "逐段文字相同");
        expectTrue(sameTree(doc.root, back.root), "节点身份、种类、隐藏标记、段落数、哈希逐个相同");
        expectTrue(doc::damage(doc, back, FlowVisibility::Design).range.empty(),
                   "两份修订之间没有损伤（没有块要重排）");

        const auto again = doc::serialize(back);
        expectTrue(again.ok && again.text == ser.text, "再写一遍字节完全相同（写端是确定的）");

        // 字符属性与段落覆盖：这两个载荷只在非默认时才写，所以另起一份全非默认的修订。
        doc::RunPayload run;
        run.text = {U'A'};
        run.marks.bold = true;
        run.marks.italic = false;
        run.marks.underline = true;
        run.marks.font_index = 3;
        run.marks.size = Length::abs(ptToEmu(12.5));
        run.marks.color = Color::rgb(1, 2, 3);
        run.style = "强调";
        doc::MarkPayload mark;
        mark.para.font_index = 7;
        mark.para.size = Length::abs(ptToEmu(10.5));
        mark.para.wm =
            WritingMode{InlineAxis::Vertical, BlockFlow::RightToLeft, TextOrientation::Upright};
        mark.para.line_factor = 1.25f;
        mark.para.space_after = Length::abs(ptToEmu(6.0));
        mark.para.page_break_before = false;
        mark.para.language = internLanguage("zh-CN");
        mark.para.color = Color::rgb(9, 8, 7);
        mark.para.fallback_font_indices = {2, 5};
        mark.style = "正文";
        const doc::Ref rich_section =
            doc::makeNode(2, NodeKind::Section, SectionSpec{},
                          {doc::makeNode(10, NodeKind::TextRun, run, {}),
                           doc::makeNode(11, NodeKind::ParagraphMark, mark, {})});
        const Doc rich = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {rich_section}), 12);

        const auto rich_ser = doc::serialize(rich);
        Doc rich_back;
        const auto rich_de = doc::deserialize(rich_ser.text, &rich_back);
        expectTrue(rich_ser.ok && rich_de.ok && rich_back.lo == rich.lo && rich_back.hi == rich.hi,
                   "字符属性与段落覆盖往返之后哈希相同");
        const doc::Node* got_run = doc::findNode(rich_back, 10);
        const doc::Node* got_mark = doc::findNode(rich_back, 11);
        expectTrue(got_run != nullptr && got_mark != nullptr, "两个富格式节点都读回来了");
        if (got_run != nullptr) {
            const auto& rp = std::get<doc::RunPayload>(got_run->payload);
            const bool same = rp.marks.bold.has_value() && *rp.marks.bold &&
                              rp.marks.italic.has_value() && !*rp.marks.italic &&
                              rp.marks.underline.has_value() && *rp.marks.underline &&
                              rp.marks.font_index.has_value() && *rp.marks.font_index == 3 &&
                              rp.marks.color.has_value() &&
                              *rp.marks.color == Color::rgb(1, 2, 3) && rp.style == "强调";
            expectTrue(same, "字符属性逐项读回来（三态、字号、颜色、样式引用）");
        }
        if (got_mark != nullptr) {
            const auto& mp = std::get<doc::MarkPayload>(got_mark->payload);
            const bool same = mp.para.font_index.has_value() && *mp.para.font_index == 7 &&
                              mp.para.wm.has_value() &&
                              mp.para.wm->inline_axis == InlineAxis::Vertical &&
                              mp.para.line_factor.has_value() && *mp.para.line_factor == 1.25f &&
                              mp.para.fallback_font_indices.has_value() &&
                              *mp.para.fallback_font_indices == std::vector<int>({2, 5}) &&
                              mp.para.language.has_value() && *mp.para.language != nullptr &&
                              std::string(*mp.para.language) == "zh-CN" && mp.style == "正文";
            expectTrue(same, "段落覆盖逐项读回来（书写模式、行距、回退字体、语言）");
        }

        // 控件逐字段：每个变体都过一遍落盘，读回来必须与写进去的相等。
        int lost = 0;
        const auto variants = controlVariants();
        for (const auto& [name, variant] : variants) {
            NodeId created = kInvalidNodeId;
            const auto r =
                doc::insertControl(doc::makeTextDoc({U"x"}), 0, 0, variant, U"v", &created);
            const auto text = doc::serialize(r.doc);
            Doc rt;
            const bool ok = text.ok && doc::deserialize(text.text, &rt).ok;
            const doc::Node* n = ok ? doc::findNode(rt, created) : nullptr;
            if (n == nullptr || n->kind != NodeKind::ContentControl ||
                !(std::get<ControlSpec>(n->payload) == variant)) {
                ++lost;
                std::printf("      字段没落盘：%s\n", name);
            }
        }
        expectTrue(lost == 0,
                   "控件的每个字段都过一遍落盘（" + std::to_string(variants.size()) + " 条）");

        // 读坏一份文件就该拒绝，且目标一个字节都不动。每种坏法一条。
        auto rejects = [&doc](const std::string& text, const char* what) {
            Doc target = doc;
            const auto r = doc::deserialize(text, &target);
            const bool kept = target.root == doc.root;
            expectTrue(!r.ok && !r.error.empty() && kept, what);
        };
        rejects(R"({"v":3,"next":9,"root":{}})", "版本号不对：拒绝");
        rejects("not json", "不是 JSON：拒绝");
        rejects(R"({"v":1,"next":9})", "缺少根节点：拒绝");
        rejects(R"({"v":1,"next":9,"root":{"id":1,"k":"Nope"}})", "不认识的种类名：拒绝");
        rejects(R"({"v":1,"next":9,"root":{"id":1,"k":"Document","ch":[]}})",
                "Document 下没有节：拒绝");
        rejects(
            R"({"v":1,"next":9,"root":{"id":1,"k":"Document","ch":[{"id":2,"k":"Section"},{"id":2,"k":"Section"}]}})",
            "节点号重复：拒绝");
        rejects(R"({"v":1,"next":2,"root":{"id":1,"k":"Document","ch":[{"id":2,"k":"Section"}]}})",
                "水位不大于最大节点号：拒绝");
        rejects(
            R"({"v":1,"next":9,"root":{"id":1,"k":"Document","ch":[{"id":2,"k":"Section","ch":[{"id":3,"k":"TextRun","p":{}}]}]}})",
            "文本节点带段落覆盖：拒绝");
        rejects(
            R"({"v":1,"next":9,"root":{"id":1,"k":"Document","ch":[{"id":2,"k":"Section","ch":[{"id":3,"k":"TextRun","t":[65],"st":3}]}]}})",
            "样式引用下标越界：拒绝");

        // 反向：写端也不交出一份读不回来的文件。号重复的树只能在 makeNode 这一层造出来。
        const doc::Ref dup = doc::makeNode(
            1, NodeKind::Document, {},
            {doc::makeNode(2, NodeKind::Section, SectionSpec{},
                           {doc::makeNode(3, NodeKind::TextRun,
                                          doc::RunPayload{cpsOf(U"a"), CharFormat{}, {}}, {}),
                            doc::makeNode(3, NodeKind::TextRun,
                                          doc::RunPayload{cpsOf(U"b"), CharFormat{}, {}}, {})})});
        expectTrue(!doc::serialize(doc::adopt(dup, 9)).ok, "节点号重复的修订：写不出来");

        // v2 的两处"让文件自己解释自己"。
        //   文本：v1 把内存表示（码点数组）直接写进文件，打开看到的是 [24739,32773,...]；
        //     v2 写字符串。两条都要钉：写端必须是字符串，读端必须仍然认得 v1 的数组
        //     （上面那些 "v":1 的用例走的正是那条路）。
        //   字体：v1 只有 fi 这种库下标，换台机器就是另一个字体；v2 带一张 "下标 → 文件名"。
        expectTrue(ser.text.find("\"t\":[") == std::string::npos, "文本不再写成码点数组");
        expectTrue(ser.text.find("\"t\":\"") != std::string::npos, "文本写成 UTF-8 字符串");
        // 正面的一半：v1 的文件（数组）读得回来，而且读成同一段文字。上面那些 "v":1 的用例
        //   都是拒绝路径，只能说明"它走到了节点解析"，说明不了"它能读对"。
        Doc v1_back;
        const auto v1_read = doc::deserialize(
            R"({"v":1,"next":5,"root":{"id":1,"k":"Document","ch":[{"id":2,"k":"Section","ch":[)"
            R"({"id":3,"k":"TextRun","t":[25105,20204]},{"id":4,"k":"ParagraphMark","p":{}}]}]}})",
            &v1_back);
        expectTrue(v1_read.ok, v1_read.ok ? std::string("v1 的文件读得进来")
                                          : ("v1 的文件读不进来：" + v1_read.error));
        expectTrue(v1_read.ok && doc::paraTexts(v1_back) == std::vector<std::u32string>{U"我们"},
                   "v1 的码点数组还原成同一段文字");
        expectTrue(rich_ser.text.find("\"lf\":\"1.25\"") != std::string::npos,
                   "浮点写最短表示（1.25 而不是 1.25000000）");

        doc::SerializeOptions with_fonts;
        // 三样都给：PostScript 名当身份、族名给人看、文件名排在最后。
        with_fonts.font_ref = [](int index) {
            const std::string n = std::to_string(index);
            return doc::FontRef{"FacePS" + n + "-Regular", "Face Family " + n,
                                "face-" + n + ".ttf"};
        };
        const auto named = doc::serialize(rich, with_fonts);
        expectTrue(named.ok && named.text.find("\"fonts\":{") != std::string::npos,
                   "字体表跟着文件走（下标 → 字体身份）");
        expectTrue(named.text.find("\"3\":{\"ps\":\"FacePS3-Regular\",\"family\":\"Face Family 3\","
                                   "\"file\":\"face-3.ttf\"}") != std::string::npos,
                   "字体表写的是对象：PostScript 名 + 族名 + 文件名");
        expectTrue(named.text.find("\"5\":") != std::string::npos, "回退链里的字体也进表（5 号）");
        expectTrue(named.text.find("\"0\":") == std::string::npos,
                   "没用到 0 号就不写它（字体表不打包整个库）");

        // 换一台机器：文件里的 3 号在这边是 40 号。映射必须一路走到载荷里，
        //   只在顶层换个数字、载荷里还是 3，等于没做。
        doc::DeserializeOptions remap;
        remap.resolve_font = [](const doc::FontRef& ref) -> int {
            if (ref.postscript == "FacePS3-Regular") return 40;
            if (ref.postscript == "FacePS7-Regular") return 41;
            if (ref.postscript == "FacePS5-Regular") return 42;
            return -1;
        };
        Doc named_back;
        expectTrue(doc::deserialize(named.text, &named_back, remap).ok, "带字体表的文件读得回来");
        const doc::Node* remapped = doc::findNode(named_back, 10);
        expectTrue(
            remapped != nullptr && std::get<doc::RunPayload>(remapped->payload).marks.font_index ==
                                       std::optional<int>(40),
            "字符属性的字体下标按 PostScript 名换成本机的（3 → 40）");
        const doc::Node* remapped_mark = doc::findNode(named_back, 11);
        if (remapped_mark != nullptr) {
            const auto& mp = std::get<doc::MarkPayload>(remapped_mark->payload);
            expectTrue(mp.para.font_index == std::optional<int>(41),
                       "段落覆盖的字体下标同样映射（7 → 41）");
            expectTrue(mp.para.fallback_font_indices == std::vector<int>({2, 42}),
                       "回退链逐项映射，不认识的那一项原样留着（2 不动，5 → 42）");
        }

        // 三级匹配：文件改名之后，靠族名仍要能认回同一张脸；族名也没了才落到文件名。
        //   这一条正是"文件名不是身份"的理由 —— 它只是最后一级。
        doc::DeserializeOptions by_family;
        by_family.resolve_font = [](const doc::FontRef& ref) -> int {
            return ref.family == "Face Family 3" ? 50 : -1;
        };
        Doc family_back;
        const doc::Node* family_run = nullptr;
        if (doc::deserialize(named.text, &family_back, by_family).ok) {
            family_run = doc::findNode(family_back, 10);
        }
        expectTrue(family_run != nullptr &&
                       std::get<doc::RunPayload>(family_run->payload).marks.font_index ==
                           std::optional<int>(50),
                   "PostScript 名不认识时按族名认（文件改名不影响）");

        doc::DeserializeOptions by_file;
        by_file.resolve_font = [](const doc::FontRef& ref) -> int {
            return ref.file == "face-3.ttf" ? 60 : -1;
        };
        Doc file_back;
        const doc::Node* file_run = nullptr;
        if (doc::deserialize(named.text, &file_back, by_file).ok) {
            file_run = doc::findNode(file_back, 10);
        }
        expectTrue(
            file_run != nullptr && std::get<doc::RunPayload>(file_run->payload).marks.font_index ==
                                       std::optional<int>(60),
            "前两级都不认识才落到文件名（最后一级）");

        // v2 的字体表是"下标 → 文件名字符串"，读端照样认（按类型分派，不看版本号）。
        const std::string v2_fonts =
            R"({"v":2,"next":5,"fonts":{"0":"NotoSansSC-VF.ttf"},"root":{"id":1,"k":"Document",)"
            R"("ch":[{"id":2,"k":"Section","ch":[{"id":3,"k":"TextRun","t":"x","m":{"fi":0}},)"
            R"({"id":4,"k":"ParagraphMark"}]}]}})";
        doc::DeserializeOptions v2_remap;
        v2_remap.resolve_font = [](const doc::FontRef& ref) -> int {
            return ref.file == "NotoSansSC-VF.ttf" ? 70 : -1;
        };
        Doc v2_fonts_back;
        const bool v2_fonts_ok = doc::deserialize(v2_fonts, &v2_fonts_back, v2_remap).ok;
        const doc::Node* v2_run = v2_fonts_ok ? doc::findNode(v2_fonts_back, 3) : nullptr;
        expectTrue(
            v2_run != nullptr && std::get<doc::RunPayload>(v2_run->payload).marks.font_index ==
                                     std::optional<int>(70),
            "v2 的字体表（值是文件名字符串）仍然读得进来");

        // 写端不交出读不回来的文本：非法码点（代理区）当场失败，不替换成 U+FFFD。
        const doc::Ref lone = doc::makeNode(
            1, NodeKind::Document, {},
            {doc::makeNode(2, NodeKind::Section, SectionSpec{},
                           {doc::makeNode(3, NodeKind::TextRun,
                                          doc::RunPayload{{0x41, 0xD800}, CharFormat{}, {}}, {}),
                            doc::makeNode(4, NodeKind::ParagraphMark, doc::MarkPayload{}, {})})});
        const auto lone_ser = doc::serialize(doc::adopt(lone, 9));
        expectTrue(!lone_ser.ok && lone_ser.error.find("非法码点") != std::string::npos,
                   "文本里有孤立代理时写不出去（不悄悄换成 U+FFFD）");
    }

    std::printf("\n[14] 表格：投影与编译逐字段相同，容器几何进块哈希\n");
    {
        const Length col0 = Length::abs(ptToEmu(30.0));
        const Length row_h = Length::abs(ptToEmu(20.0));
        const doc::Doc doc = makeTableDoc(col0, row_h);
        const StyleSheet sheet;

        expectTrue(doc::paraCount(doc) == 7, "七段：表前、五个格、表后");

        const doc::Projection pr = doc::project(doc, sheet);
        expectTrue(pr.ok(), "投影没有诊断");
        expectTrue(pr.blocks.size() == 7,
                   "块数等于七段（实际 " + std::to_string(pr.blocks.size()) + "）");
        expectTrue(pr.tables.size() == 1, "只有一张表");
        expectTrue(!pr.tables.empty() && pr.tables[0].rows == 2 && pr.tables[0].cols == 3 &&
                       pr.tables[0].cells.size() == 5,
                   "表格结构：两行三列、五个逻辑格");
        // 块的归属由格的块区间反推，判据看的就是这条推导。
        heditor::CellIndex cells;
        cells.build(pr.tables, pr.blocks.size());
        std::vector<CellSlot> path;
        cells.pathAt(1, &path);
        expectTrue(path.size() == 1 && path[0].row == 0 && path[0].cell == 0,
                   "第一格里的块带着它的格路径");
        expectTrue(cells.nodeAt(6) == nullptr, "表后的正文块没有格路径");
        expectTrue(pr.controls.empty(), "表格里没有控件");

        // 改一格文字：损伤只落在那一块。
        const auto typed = doc::insertText(doc, 2, 0, U"X");
        expectTrue(typed.ok, "格内插字：投影层能按段落号落进格");
        const doc::Damage one = doc::damage(doc, typed.doc, FlowVisibility::Design);
        expectTrue(one.range.changed && one.range.lo == 2 && one.range.hi == 3,
                   "改一格文字：损伤正好是那一块（实际 [" + std::to_string(one.range.lo) + "," +
                       std::to_string(one.range.hi) + ")）");
        expectTrue(typed.ok && doc::paraTexts(typed.doc)[2] == U"XB0", "字落进了第二格");

        // 改列宽：内容一个都没动，但整张表都要重排。
        const doc::Doc wide = makeTableDoc(Length::abs(ptToEmu(45.0)), row_h);
        const std::vector<doc::BlockHash> before = doc::blockHashes(doc, FlowVisibility::Design);
        const std::vector<doc::BlockHash> after = doc::blockHashes(wide, FlowVisibility::Design);
        expectTrue(before.size() == after.size(), "改列宽不改块数");
        bool outside_changed = before[0] != after[0] || before[6] != after[6];
        bool inside_changed = true;
        for (size_t i = 1; i < 6; ++i) {
            inside_changed = inside_changed && before[i] != after[i];
        }
        expectTrue(!outside_changed && inside_changed,
                   "改列宽：表内五块的哈希全变，表外的两块一个都没变");
        const doc::Damage dmg = doc::damage(doc, wide, FlowVisibility::Design);
        const doc::BlockDamage ref = doc::diffBlocks(before, after);
        expectTrue(dmg.range.lo == 1 && dmg.range.hi == 6,
                   "改列宽：损伤是整张表（实际 [" + std::to_string(dmg.range.lo) + "," +
                       std::to_string(dmg.range.hi) + ")）");
        expectTrue(dmg.range.lo == ref.lo && dmg.range.hi == ref.hi,
                   "改列宽：哈希下降与块哈希比对完全一致");

        // 改行高：同样只动容器记录，整张表进损伤。
        // 行高只影响那一行（三个格），不影响另一行 —— 行是这一层容器，它的范围就是自己的块。
        const doc::Doc tall = makeTableDoc(col0, Length::abs(ptToEmu(36.0)));
        const doc::Damage dh = doc::damage(doc, tall, FlowVisibility::Design);
        expectTrue(dh.range.lo == 1 && dh.range.hi == 4, "改行高：损伤是第一行的三块（实际 [" +
                                                             std::to_string(dh.range.lo) + "," +
                                                             std::to_string(dh.range.hi) + ")）");

        // 格内分段：一格里多出一块，结构表的 block_count 跟着变。
        const auto split = doc::splitPara(typed.doc, 2, 1);
        expectTrue(split.ok, "格内分段");
        const doc::Projection pr2 = doc::project(split.doc, sheet);
        expectTrue(pr2.ok() && pr2.blocks.size() == 8, "格内分段之后多一块");
        expectTrue(!pr2.tables.empty() && pr2.tables[0].cells.size() == 5, "结构表的格数没变");
        expectTrue(!pr2.tables.empty() && pr2.tables[0].cells[1].block_count == 2,
                   "第二格的块区间变成两块");

        // 落盘：表格记录逐字段（列定义、列宽、行开关、行高、跨度、垂直对齐）。
        const auto ser = doc::serialize(doc);
        Doc back;
        const auto de = ser.ok ? doc::deserialize(ser.text, &back) : doc::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == doc.lo && back.hi == doc.hi,
                   "表格往返之后全文哈希相同");
        const auto probe = [](const Doc& d) {
            const doc::Children* body = doc::bodyOf(d);
            return (body != nullptr && body->size() > 2) ? (*body)[2] : doc::Ref{};
        };
        const doc::Ref a = probe(doc);
        const doc::Ref b = probe(back);
        expectTrue(a && b && std::get<TableSpec>(a->payload) == std::get<TableSpec>(b->payload),
                   "表格记录逐字段相同");
        expectTrue(a && b && !a->children.empty() && !b->children.empty() &&
                       std::get<RowSpec>(a->children[0]->payload) ==
                           std::get<RowSpec>(b->children[0]->payload),
                   "行记录逐字段相同（三个开关、行高、行高规则）");
        expectTrue(a && b && !a->children.empty() && !a->children[0]->children.empty() &&
                       std::get<CellSpec>(a->children[0]->children[0]->payload) ==
                           std::get<CellSpec>(b->children[0]->children[0]->payload),
                   "单元格记录逐字段相同（两个跨度、垂直对齐）");
    }

    std::printf("\n[15] 原子与域：一条盒子算法、两条路同样的产物\n");
    {
        const StyleSheet sheet;
        const Doc doc = makeAtomDoc("张三");
        const doc::Projection pr = doc::project(doc, sheet);
        expectTrue(pr.blocks.size() == 6, "六段（实际 " + std::to_string(pr.blocks.size()) + "）");
        expectTrue(pr.diagnostics.size() == 2, "两条诊断：相对宽度与域未进排版输入");

        // 四枚图片的盒子：像素折算、只给宽、只给高按比例、相对宽度按原始尺寸。
        auto box_of = [&](size_t block, size_t obj) {
            const auto& os = pr.blocks[block].objects;
            return obj < os.size() ? os[obj] : InlineObject{};
        };
        expectTrue(std::abs(box_of(0, 0).w_pt - 150.0f) < 0.01f &&
                       std::abs(box_of(0, 0).h_pt - 75.0f) < 0.01f,
                   "只有像素：200x100 @96dpi → 150x75 pt");
        expectTrue(std::abs(box_of(1, 0).w_pt - 30.0f) < 0.01f, "只给宽：宽度就是作者给的 30 pt");
        expectTrue(std::abs(box_of(2, 0).h_pt - 20.0f) < 0.01f &&
                       std::abs(box_of(2, 0).w_pt - 40.0f) < 0.01f,
                   "只给高且有像素：按原图 2:1 推宽到 40 pt");
        expectTrue(box_of(3, 0).atom == InlineAtom::Image && box_of(3, 0).w_pt > 0.0f,
                   "相对宽度：按原始尺寸折算出一个正数宽，并记诊断");
        expectTrue(box_of(4, 0).atom == InlineAtom::Formula && box_of(4, 0).h_pt > 0.0f,
                   "公式：按表达式估出一个框");

        // 内容指纹：换资源标识（尺寸可以一模一样）必须换指纹，否则行指纹看不出换了图。
        const Doc doc2 = makeAtomDoc("张三", "res/z.png");
        const auto h1 = doc::blockHashes(doc, FlowVisibility::Design);
        const auto h2 = doc::blockHashes(doc2, FlowVisibility::Design);
        expectTrue(h1.size() == h2.size() && h1[0] != h2[0] && h1[1] == h2[1],
                   "换一张同样大的图：只有那一块的哈希变");

        // 域的缓存值是派生量：只有它不同时，内容哈希必须相同。
        const Doc doc3 = makeAtomDoc("李四");
        expectTrue(doc3.lo == doc.lo && doc3.hi == doc.hi,
                   "只改域的缓存值：全文哈希不变（求值结果不是内容）");
        expectTrue(doc::blockHashes(doc3, FlowVisibility::Design) ==
                       doc::blockHashes(doc, FlowVisibility::Design),
                   "只改缓存值：块哈希一个都没变（没有块要重排）");

        // 落盘：三类记录逐字段。域的 cache 不在哈希里，所以它只能靠逐字段比对。
        const auto ser = doc::serialize(doc);
        Doc back;
        const auto de = ser.ok ? doc::deserialize(ser.text, &back) : doc::DeserializeResult{};
        expectTrue(ser.ok && de.ok, "原子与域能落盘并读回来");
        expectTrue(de.ok && back.lo == doc.lo && back.hi == doc.hi, "往返之后全文哈希相同");
        const auto body = [](const Doc& d) -> const doc::Children* { return doc::bodyOf(d); };
        const doc::Children* a = body(doc);
        const doc::Children* b = body(back);
        int compared = 0;
        int mismatched = 0;
        if (a != nullptr && b != nullptr && a->size() == b->size()) {
            for (size_t i = 0; i < a->size(); ++i) {
                const doc::Node& x = *(*a)[i];
                const doc::Node& y = *(*b)[i];
                if (x.kind == NodeKind::Image) {
                    ++compared;
                    if (!(std::get<ImageSpec>(x.payload) == std::get<ImageSpec>(y.payload))) {
                        ++mismatched;
                    }
                } else if (x.kind == NodeKind::Formula) {
                    ++compared;
                    if (!(std::get<FormulaSpec>(x.payload) == std::get<FormulaSpec>(y.payload))) {
                        ++mismatched;
                    }
                } else if (x.kind == NodeKind::Field) {
                    ++compared;
                    if (!(std::get<FieldSpec>(x.payload) == std::get<FieldSpec>(y.payload))) {
                        ++mismatched;
                    }
                }
            }
        }
        expectTrue(compared == 6 && mismatched == 0,
                   "六条原子记录逐字段读回来（图片 4 / 公式 1 / 域 1，含缓存值）");

        // 多节：两节的内容都要在，不是"只搬第一节"。
        auto run = [](NodeId id, const std::u32string& text) {
            return doc::makeNode(id, NodeKind::TextRun,
                                 doc::RunPayload{cpsOf(text), CharFormat{}, {}}, {});
        };
        const doc::Ref s0 = doc::makeNode(
            2, NodeKind::Section, SectionSpec{},
            {run(3, U"a"), doc::makeNode(4, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const doc::Ref s1 = doc::makeNode(
            5, NodeKind::Section, SectionSpec{},
            {run(6, U"b"), doc::makeNode(7, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        Doc two = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {s0, s1}), 8);
        expectTrue(doc::paraCount(two) == 2 &&
                       (doc::paraTexts(two) == std::vector<std::u32string>{U"a", U"b"}),
                   "两节的段落都在（不是只搬第一节）");
    }

    std::printf("\n[16] 嵌套表格：格路径是序列，容器几何各管各的一段\n");
    {
        const StyleSheet sheet;
        const Length inner_col = Length::abs(ptToEmu(18.0));
        const Doc doc = makeNestedTableDoc(inner_col);

        const doc::Projection pr = doc::project(doc, sheet);
        expectTrue(pr.ok(), "嵌套表投影没有诊断");
        expectTrue(pr.blocks.size() == 6, "六块：前 / A / i1 / i2 / B / 后（实际 " +
                                              std::to_string(pr.blocks.size()) + "）");
        expectTrue(pr.tables.size() == 2, "两张结构表（外 + 内）");

        // 格路径是"外层到内层"的序列：内表格里的块带两层，外表格另一格的块只带一层。
        heditor::CellIndex cells;
        cells.build(pr.tables, pr.blocks.size());
        std::vector<CellSlot> p2;
        std::vector<CellSlot> p4;
        std::vector<CellSlot> p5;
        cells.pathAt(2, &p2);
        cells.pathAt(4, &p4);
        cells.pathAt(5, &p5);
        expectTrue(p2.size() == 2 && cells.depthAt(3) == 2, "内表格的块带两层格路径");
        expectTrue(p2.size() == 2 && p2[0].table != p2[1].table, "两层格路径指向两张不同的表");
        expectTrue(p4.size() == 1, "外表格另一格的块只带一层");
        expectTrue(cells.depthAt(1) == 1 && p5.empty(), "格里的文字一层，表后的正文没有格路径");

        // 块区间：外表格第一格含 "A" 与内表格两块，所以是三块；第二格一块。
        expectTrue(pr.tables.size() == 2 && pr.tables[0].cells.size() == 2 &&
                       pr.tables[0].cells[0].first_block == 1 &&
                       pr.tables[0].cells[0].block_count == 3,
                   "外表格第一格的块区间是 [1,4)（A 与内表格两块）");
        expectTrue(pr.tables.size() == 2 && pr.tables[0].cells.size() == 2 &&
                       pr.tables[0].cells[1].block_count == 1,
                   "外表格第二格一块");
        expectTrue(pr.tables.size() == 2 && pr.tables[1].cells.size() == 2 &&
                       pr.tables[1].cells[0].first_block == 2 &&
                       pr.tables[1].cells[1].first_block == 3,
                   "内表格两格各自一块，块号接着外层的顺序走");

        // 只改内层列宽：内表格两块要重排，外层那几块（前 / A / B / 后）一个都不动。
        const Doc doc2 = makeNestedTableDoc(Length::abs(ptToEmu(30.0)));
        const auto before = doc::blockHashes(doc, FlowVisibility::Design);
        const auto after = doc::blockHashes(doc2, FlowVisibility::Design);
        expectTrue(before.size() == after.size() && before[0] == after[0] &&
                       before[1] == after[1] && before[4] == after[4] && before[5] == after[5],
                   "改内层列宽：表外的两块与外表格另一格一个都没变");
        expectTrue(before[2] != after[2] && before[3] != after[3], "改内层列宽：内表格两块都变了");
        const doc::Damage dmg = doc::damage(doc, doc2, FlowVisibility::Design);
        const doc::BlockDamage ref = doc::diffBlocks(before, after);
        expectTrue(dmg.range.changed && dmg.range.lo == 2 && dmg.range.hi == 4,
                   "改内层列宽：损伤只到内表格两块（实际 [" + std::to_string(dmg.range.lo) + "," +
                       std::to_string(dmg.range.hi) + ")）");
        expectTrue(dmg.range.lo == ref.lo && dmg.range.hi == ref.hi,
                   "改内层列宽：哈希下降与块哈希比对一致");

        // 改外层列宽：外表格覆盖的块整段算损伤 —— 内表格也在外面那一层里。
        const Doc doc3 = makeNestedTableDoc(inner_col, Length::abs(ptToEmu(70.0)));
        const doc::Damage dmg2 = doc::damage(doc, doc3, FlowVisibility::Design);
        expectTrue(dmg2.range.lo == 1 && dmg2.range.hi == 5,
                   "改外层列宽：损伤是整张外表格（[1,5)，实际 [" + std::to_string(dmg2.range.lo) +
                       "," + std::to_string(dmg2.range.hi) + ")）");

        // 落盘：嵌套结构原样回来。
        const auto ser = doc::serialize(doc);
        Doc back;
        const auto de = ser.ok ? doc::deserialize(ser.text, &back) : doc::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == doc.lo && back.hi == doc.hi,
                   "嵌套表格往返之后全文哈希相同");
        const doc::Projection pr2 = de.ok ? doc::project(back, sheet) : doc::Projection{};
        expectTrue(pr2.blocks.size() == pr.blocks.size() && !pr2.tables.empty() &&
                       sameTableShape(pr2.tables[0], pr.tables[0]),
                   "读回来再投影：块数与外表格结构不变");
    }

    std::printf("\n[17] 跨段控件：一对边界落在两块上\n");
    {
        const StyleSheet sheet;
        const Doc doc = makeSpanningControlDoc();
        expectTrue((doc::paraTexts(doc) == std::vector<std::u32string>{U"甲乙", U"丙", U"丁"}),
                   "三段：控件里的段落标记切开了它自己的内容");

        const doc::Projection pr = doc::project(doc, sheet);
        expectTrue(pr.ok(), "跨段控件投影没有诊断");
        expectTrue(pr.blocks.size() == 3, "三块");
        expectTrue(pr.blocks.size() == 3 && pr.blocks[0].objects.size() == 1 &&
                       pr.blocks[0].objects[0].kind == InlineObjectKind::ControlStart &&
                       pr.blocks[0].objects[0].cp == 1,
                   "第一块只有开括号（锚在 1 之后）");
        expectTrue(pr.blocks.size() == 3 && pr.blocks[1].objects.size() == 1 &&
                       pr.blocks[1].objects[0].kind == InlineObjectKind::ControlEnd &&
                       pr.blocks[1].objects[0].cp == 1,
                   "第二块只有闭括号（锚在 1 之后）");
        expectTrue(pr.blocks.size() == 3 && pr.blocks[2].objects.empty(), "第三块里没有任何边界");

        expectTrue(pr.controls.size() == 1 && pr.controls[0].block == 0 && pr.controls[0].cp == 1 &&
                       pr.controls[0].end_block == 1 && pr.controls[0].end_cp == 1,
                   "控件表给出的跨度跨两块：块 0 的 cp 1 到块 1 的 cp 1");
        const doc::Node* cc_node =
            doc::bodyOf(doc) != nullptr ? (*doc::bodyOf(doc))[1].get() : nullptr;
        expectTrue(cc_node != nullptr && cc_node->kind == NodeKind::ContentControl &&
                       pr.controls[0].node.get() == cc_node,
                   "跨度指向的是那个控件节点本身（不是它的某个孩子）");

        // 呈现态：隐藏它，它内部的文字与两个边界一起消失；控件的段落标记也在内部，所以
        // 前两段并成一段。
        const auto hid = doc::setHidden(doc, cc_node->id, true);
        const doc::Projection disp =
            doc::project(hid.doc, sheet, doc::ViewParams{FlowVisibility::Display});
        expectTrue(disp.blocks.size() == 2 && disp.controls.empty(),
                   "呈现态：隐藏之后两段，控件表为空");
    }

    std::printf("\n[18] 会话层：撤销栈是一串旧修订，不是一串逆操作\n");
    {
        const Doc initial = doc::makeTextDoc({U"第一段", U"第二段"});
        doc::Session s(initial, 6);
        Doc direct = initial;

        // 同一串编辑同时走会话与原语：会话多出来的只有"记住旧值"，其余必须一模一样。
        auto directOf = [](const Doc& d, const doc::Edit& e) {
            switch (e.kind) {
                case doc::Edit::Kind::InsertText:
                    return doc::insertText(d, e.para, e.cp, e.text);
                case doc::Edit::Kind::EraseText:
                    return doc::eraseText(d, e.para, e.cp, e.count);
                case doc::Edit::Kind::SplitPara:
                    return doc::splitPara(d, e.para, e.cp);
                case doc::Edit::Kind::MergePara:
                    return doc::mergePara(d, e.para);
                default:
                    return doc::EditResult{};
            }
        };

        std::mt19937 rng(20261020u);
        int steps = 0;
        int rejected = 0;
        int mismatched = 0;
        std::vector<Doc> timeline{initial};
        for (int i = 0; i < 120; ++i) {
            const std::vector<std::u32string> texts = doc::paraTexts(direct);
            const int paras = static_cast<int>(texts.size());
            doc::Edit e;
            e.para = static_cast<int>(rng() % static_cast<uint32_t>(paras));
            const int len = static_cast<int>(texts[e.para].size());
            e.cp = static_cast<int>(rng() % static_cast<uint32_t>(len + 1));
            const int op = static_cast<int>(rng() % 4);
            if (op == 0) {
                e.kind = doc::Edit::Kind::InsertText;
                e.text = std::u32string(1, static_cast<char32_t>('a' + rng() % 26));
            } else if (op == 1) {
                if (len == 0) continue;
                e.kind = doc::Edit::Kind::EraseText;
                e.count = 1 + static_cast<int>(
                                  rng() % static_cast<uint32_t>(len - e.cp > 0 ? len - e.cp : 1));
            } else if (op == 2) {
                e.kind = doc::Edit::Kind::SplitPara;
            } else {
                e.kind = doc::Edit::Kind::MergePara;
            }

            const doc::EditResult d = directOf(direct, e);
            const doc::Applied a = s.apply(e);
            if (a.ok != d.ok) {
                ++mismatched;
                continue;
            }
            if (!a.ok) {
                ++rejected;
                if (s.doc().lo != direct.lo || s.doc().hi != direct.hi) ++mismatched;
                continue;
            }
            direct = d.doc;
            ++steps;
            if (!sameTree(s.doc().root, direct.root)) ++mismatched;
            timeline.push_back(s.doc());
        }
        expectTrue(mismatched == 0, "同一串编辑走会话与走原语，结果逐节点相同（" +
                                        std::to_string(steps) + " 步成功 / " +
                                        std::to_string(rejected) + " 步被拒）");
        expectTrue(steps > 30, "这串编辑真的推进了很多步（" + std::to_string(steps) + "）");

        // 撤销到底：回到起点，且再撤一次返回 false。
        int undone = 0;
        while (s.undo()) ++undone;
        expectTrue(undone == s.limit() || undone == steps,
                   "撤销次数到顶（" + std::to_string(undone) + " 次，上限 " +
                       std::to_string(s.limit()) + "）");
        expectTrue(!s.canUndo() && !s.undo(), "撤到底之后再撤：返回 false");
        bool same_as_start = sameTree(s.doc().root, timeline[timeline.size() - 1 - undone].root);
        expectTrue(same_as_start, "撤到底之后落在本会话保留的最早一份修订上");

        // 重做全回去：与直接走到终点的结果相同。
        while (s.redo()) {
        }
        expectTrue(!s.canRedo() && sameTree(s.doc().root, direct.root),
                   "全部重做之后与直接应用的结果逐节点相同");

        // 撤销之后再编辑，重做栈必须清空：留着它意味着一路跳回另一条分支。
        expectTrue(s.undo(), "准备：撤销一步");
        const uint64_t gen_before = s.generation();
        doc::Edit ins;
        ins.kind = doc::Edit::Kind::InsertText;
        ins.para = 0;
        ins.cp = 0;
        ins.text = U"Z";
        expectTrue(s.apply(ins).ok, "撤销之后再编辑");
        expectTrue(s.redoDepth() == 0 && !s.canRedo(), "编辑之后重做栈清空");
        expectTrue(s.generation() > gen_before, "状态代号推进");

        // 被拒绝/没变化的一步：状态与历史都不动。
        //   这两条另起一个没吃满上限的会话：封顶之后深度怎么变都是同一个数，断言在那里
        //   恒真（注入"空编辑也入栈"的错，它照样绿）。
        doc::Session fresh(initial, 10);
        doc::Edit seed;
        seed.kind = doc::Edit::Kind::InsertText;
        seed.para = 0;
        seed.cp = 0;
        seed.text = U"seed";
        expectTrue(fresh.apply(seed).ok, "准备：新会话里先走一步");
        const Doc before_bad = fresh.doc();
        const int depth_before = fresh.undoDepth();

        doc::Edit bad;
        bad.kind = doc::Edit::Kind::MergePara;
        bad.para = doc::paraCount(fresh.doc()) - 1;  // 最后一段没有下一段
        const doc::Applied refused = fresh.apply(bad);
        expectTrue(!refused.ok && !refused.error.empty(), "末段合并：被拒绝并说明原因");
        expectTrue(fresh.doc().lo == before_bad.lo && fresh.doc().hi == before_bad.hi &&
                       fresh.undoDepth() == depth_before,
                   "被拒绝的一步不占撤销栈，当前修订不动");

        doc::Edit noop;
        noop.kind = doc::Edit::Kind::EraseText;
        noop.para = 0;
        noop.cp = 0;
        noop.count = 0;
        const doc::Applied nothing = fresh.apply(noop);
        expectTrue(nothing.ok && !nothing.changed, "删 0 个码点：成功但没有变化");
        expectTrue(fresh.undoDepth() == depth_before,
                   "没有变化的一步不占撤销栈（深度 " + std::to_string(fresh.undoDepth()) +
                       "，应为 " + std::to_string(depth_before) + "）");

        // 上限：超出时丢最旧的一份，撤销仍然一步步往回走。
        doc::Session small(initial, 3);
        std::vector<Doc> seen{initial};
        for (int i = 0; i < 6; ++i) {
            doc::Edit e;
            e.kind = doc::Edit::Kind::InsertText;
            e.para = 0;
            e.cp = 0;
            e.text = std::u32string(1, static_cast<char32_t>('A' + i));
            expectTrue(small.apply(e).ok, "上限用例：第 " + std::to_string(i + 1) + " 步");
            seen.push_back(small.doc());
        }
        expectTrue(small.undoDepth() == 3,
                   "撤销栈封顶在上限（" + std::to_string(small.undoDepth()) + "）");
        int back = 0;
        while (small.undo()) ++back;
        expectTrue(back == 3 && sameTree(small.doc().root, seen[3].root),
                   "撤到底落在第三份修订上（更早的两份已丢弃，但可以撤销回去）");

        // 上限 0：没有历史，只能前进。
        doc::Session none(initial, 0);
        doc::Edit one;
        one.kind = doc::Edit::Kind::InsertText;
        one.para = 0;
        one.cp = 0;
        one.text = U"x";
        expectTrue(none.apply(one).ok && !none.canUndo(), "上限 0：编辑成功但没有历史");
        expectTrue(!none.undo(), "上限 0：撤销返回 false");
    }

    std::printf("\n[19] 多节：分节分页、跨节编辑、页面设置往返\n");
    {
        const StyleSheet sheet;
        // 两节：第一节一段，第二节从新页开始。
        auto buildDoc = [](SectionBreak kind, const Length& left_margin) {
            int id = 3;
            auto run = [&](const std::u32string& text) {
                return doc::makeNode(id++, NodeKind::TextRun,
                                     doc::RunPayload{cpsOf(text), CharFormat{}, {}}, {});
            };
            auto mark = [&]() {
                return doc::makeNode(id++, NodeKind::ParagraphMark, doc::MarkPayload{}, {});
            };
            SectionSpec second;
            second.break_kind = kind;
            if (left_margin != Length{}) second.margins.left = left_margin;
            const doc::Ref s0 =
                doc::makeNode(id++, NodeKind::Section, SectionSpec{}, {run(U"第一节"), mark()});
            const doc::Ref s1 =
                doc::makeNode(id++, NodeKind::Section, second, {run(U"第二节"), mark()});
            const NodeId root_id = id++;
            const doc::Ref root = doc::makeNode(root_id, NodeKind::Document, {}, {s0, s1});
            return doc::adopt(root, id);
        };

        const Doc doc = buildDoc(SectionBreak::NextPage, Length{});
        expectTrue(doc::paraCount(doc) == 2, "两段（每节一段）");

        const doc::Projection pr = doc::project(doc, sheet);
        expectTrue(pr.ok(), "投影没有诊断");
        expectTrue(pr.blocks.size() == 2, "两块");
        expectTrue(pr.blocks.size() == 2 && !pr.blocks[0].page_break_before &&
                       pr.blocks[1].page_break_before,
                   "分节分页落在第二节的第一段上（不是每一段）");

        // 连续分节：不该有段前分页。这一条证明上面那一位确实来自节的记录。
        const Doc cont_doc = buildDoc(SectionBreak::Continuous, Length{});
        const doc::Projection cont_pr = doc::project(cont_doc, sheet);
        expectTrue(cont_pr.blocks.size() == 2 && !cont_pr.blocks[1].page_break_before,
                   "连续分节：没有段前分页");

        // 跨节编辑：段号在节之间连续，第 1 段落在第二节里。
        const auto typed = doc::insertText(doc, 1, 0, U"X");
        expectTrue(typed.ok && (doc::paraTexts(typed.doc) ==
                                std::vector<std::u32string>{U"第一节", U"X第二节"}),
                   "第 1 段落在第二节里（跨节编辑按段号走）");
        expectTrue(doc::damage(doc, typed.doc, FlowVisibility::Design).range.lo == 1,
                   "损伤定位落在第二块");

        // 页面设置：进内容哈希（改页边距就是改这份文档），但投影不消费它 —— 块哈希不变。
        const Doc moved_doc = buildDoc(SectionBreak::NextPage, Length::abs(ptToEmu(3.0)));
        expectTrue(moved_doc.lo != doc.lo || moved_doc.hi != doc.hi,
                   "改页边距改了全文哈希（页面设置是内容）");
        expectTrue(doc::blockHashes(moved_doc, FlowVisibility::Design) ==
                       doc::blockHashes(doc, FlowVisibility::Design),
                   "块哈希一位不动（页边距不改变块的内容，排版由页度量参数决定）");

        // 落盘：节记录逐字段读回来。
        const auto ser = doc::serialize(moved_doc);
        Doc back;
        const auto de = ser.ok ? doc::deserialize(ser.text, &back) : doc::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == moved_doc.lo && back.hi == moved_doc.hi,
                   "两节文档往返之后全文哈希相同");
        const auto sec_of = [](const Doc& d, size_t i) -> const doc::Node* {
            return (d.root && d.root->children.size() > i) ? d.root->children[i].get() : nullptr;
        };
        const doc::Node* a = sec_of(moved_doc, 1);
        const doc::Node* b = sec_of(back, 1);
        expectTrue(a != nullptr && b != nullptr && a->kind == NodeKind::Section &&
                       std::get<SectionSpec>(b->payload).sameAs(std::get<SectionSpec>(a->payload)),
                   "第二节的页面设置逐字段读回来（分页行为、页边距）");
    }

    std::printf("\n[22] 增量投影：没改到的块整块搬，产物与全量逐字段相同\n");
    {
        const StyleSheet sheet;
        const Doc base = doc::makeTextDoc({U"甲甲", U"乙乙", U"丙丙", U"丁丁"});
        // 块流由调用方持有：project 就地更新它，所以每次比较都拿"块流 + 身份账"两样。
        std::vector<BlockSpec> b0;
        const doc::Projection p0 = doc::project(base, sheet, {}, &b0);
        const doc::BlockHistory h0{p0.style_version, p0.keys};
        expectTrue(b0.size() == 4 && p0.reused == 0, "基线四块，且没有上一轮可复用");

        // 逐块比：内容、哈希、来源身份三样都要相同。只比内容看不出"块搬对了但身份错位"
        //   —— 那一种的症状是光标落偏一个字，而内容断言全绿。
        auto sameTo = [](const std::vector<BlockSpec>& a, const std::vector<doc::BlockKey>& ka,
                         const std::vector<BlockSpec>& b, const std::vector<doc::BlockKey>& kb) {
            if (a.size() != b.size() || ka.size() != a.size() || kb.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (!testcmp::sameBlock(a[i], b[i])) return false;
                if (!(ka[i].hash == kb[i].hash)) return false;
                if (ka[i].source.first != kb[i].source.first) return false;
                if (ka[i].source.mark != kb[i].source.mark) return false;
            }
            return true;
        };
        auto firstDiff = [](const std::vector<BlockSpec>& a, const std::vector<BlockSpec>& b) {
            if (a.size() != b.size()) {
                return std::string("块数 ") + std::to_string(a.size()) + " vs " +
                       std::to_string(b.size());
            }
            for (size_t i = 0; i < a.size(); ++i) {
                if (testcmp::sameBlock(a[i], b[i])) continue;
                return "第 " + std::to_string(i) + " 块：" + testcmp::blockDiff(a[i], b[i]);
            }
            return std::string();
        };

        // 改第 2 段一个字符：那一块的哈希变了，其余三块应当整块搬来。
        const auto r = doc::insertText(base, 1, 1, U"X");
        const doc::Damage dmg1 = doc::damage(base, r.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c1 = b0;
        const doc::Projection inc1 = doc::project(r.doc, sheet, {}, &c1, &h0, &dmg1.range);
        std::vector<BlockSpec> b1;
        const doc::Projection p1 = doc::project(r.doc, sheet, {}, &b1);
        const doc::BlockHistory h1{p1.style_version, p1.keys};
        expectTrue(inc1.reused == 3, "改一段之后另外三块整块搬来");
        expectTrue(sameTo(c1, inc1.keys, b1, p1.keys),
                   "增量产物与全量逐字段相同（含来源身份）" + firstDiff(c1, b1));

        // 段首插一段：块数变了，而其余各段的身份没变，所以只有新段要算。
        const auto sp = doc::splitPara(r.doc, 0, 0);
        const doc::Damage dmg2 = doc::damage(r.doc, sp.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c2 = b1;
        const doc::Projection inc2 = doc::project(sp.doc, sheet, {}, &c2, &h1, &dmg2.range);
        std::vector<BlockSpec> b2;
        const doc::Projection p2 = doc::project(sp.doc, sheet, {}, &b2);
        const doc::BlockHistory h2{p2.style_version, p2.keys};
        expectTrue(inc2.reused == 3,
                   "段首插一段：原四块里三块整块搬，只有被拆的那一块换了节点号要重算（实际 " +
                       std::to_string(inc2.reused) + "）");
        expectTrue(c2.size() == 5 && b2.size() == 5,
                   "插入之后是五块：增量与全量同数，块数变化没有让后缀整体重算");
        expectTrue(sameTo(c2, inc2.keys, b2, p2.keys),
                   "块数变化之后增量仍与全量逐字段相同" + firstDiff(c2, b2));

        // 段中分段：一个 run 被拆成两半 + 一枚新标记。坐标最容易错的分支 ——
        //   段首分段只插标记、不拆 run，测不到它。
        for (int cut = 1; cut <= 2; ++cut) {
            const auto sm = doc::splitPara(base, 0, cut);
            if (!sm.ok) continue;
            const doc::Damage dm = doc::damage(base, sm.doc, FlowVisibility::Design);
            std::vector<BlockSpec> cm = b0;
            const doc::Projection incm = doc::project(sm.doc, sheet, {}, &cm, &h0, &dm.range);
            std::vector<BlockSpec> bm;
            const doc::Projection pm = doc::project(sm.doc, sheet, {}, &bm);
            const std::string tag = "段中分段(第 0 段第 " + std::to_string(cut) + " 位)";
            expectTrue(cm.size() == bm.size(), tag + "：块数相同（增量 " +
                                                   std::to_string(cm.size()) + " / 全量 " +
                                                   std::to_string(bm.size()) + "）");
            expectTrue(sameTo(cm, incm.keys, bm, pm.keys),
                       tag + "：增量与全量逐字段相同 " + firstDiff(cm, bm));
        }

        // 反向一半：删掉一段。被删的那一块不能"复活"，其余照旧复用。
        const auto mg = doc::mergePara(sp.doc, 0);
        const doc::Damage dmg3 = doc::damage(sp.doc, mg.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c3 = b2;
        const doc::Projection inc3 = doc::project(mg.doc, sheet, {}, &c3, &h2, &dmg3.range);
        std::vector<BlockSpec> b3;
        const doc::Projection p3 = doc::project(mg.doc, sheet, {}, &b3);
        expectTrue(sameTo(c3, inc3.keys, b3, p3.keys),
                   "合并两段之后增量与全量相同" + firstDiff(c3, b3));

        // 样式表版本变了：块哈希一模一样（哈希只算作者内容），格式却要重新解析。
        //   版本不判就是"改了样式但屏幕上没变"，而任何内容判据都是绿的。
        //   这里改文档默认格式：每一段的产物都会变，而一个码点都没动。
        StyleSheet styled = sheet;
        ParaFormatPatch dp;
        dp.space_after = Length::abs(ptToEmu(12.0));
        CharFormat dr;
        dr.bold = true;
        styled.setDocDefaults(dp, dr);
        // 上一轮取 base 的块流、损伤取 base→r.doc：三者自洽，于是"只有样式版本变了"
        //   成为唯一变量 —— 不复用必须是版本判断的结果，而不是别的原因让它退回全量。
        std::vector<BlockSpec> c4 = b0;
        const doc::Projection inc4 = doc::project(r.doc, styled, {}, &c4, &h0, &dmg1.range);
        std::vector<BlockSpec> b4;
        const doc::Projection p4 = doc::project(r.doc, styled, {}, &b4);
        expectTrue(inc4.reused == 0, "样式表版本变了：一块都不复用（否则搬来的是旧格式）");
        expectTrue(sameTo(c4, inc4.keys, b4, p4.keys),
                   "样式表版本变了之后产物仍与全量相同" + firstDiff(c4, b4));
        expectTrue(!testcmp::sameBlock(b4[0], b1[0]),
                   "这条判据的前提成立：换默认格式确实改变了产物");

        // 增量必须真的省。同一份文档、同一次测量里比"全量"与"增量"，比值不受机器快慢影响。
        {
            std::vector<std::u32string> many(2000, U"一段用于测量的文字，内容长度与真实文书接近。");
            const Doc big = doc::makeTextDoc(many);
            const auto big_edit = doc::insertText(big, 1000, 3, U"X");
            std::vector<BlockSpec> big_blocks;
            const doc::Projection big_full = doc::project(big, sheet, {}, &big_blocks);
            const doc::BlockHistory big_hist{big_full.style_version, big_full.keys};
            const doc::Damage dmg_big = doc::damage(big, big_edit.doc, FlowVisibility::Design);
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<BlockSpec> a_blocks;
            const doc::Projection a = doc::project(big_edit.doc, sheet, {}, &a_blocks);
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<BlockSpec> cb = big_blocks;
            const doc::Projection b =
                doc::project(big_edit.doc, sheet, {}, &cb, &big_hist, &dmg_big.range);
            const auto t2 = std::chrono::steady_clock::now();
            const double full_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            const double incr_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
            std::printf("      2000 段：全量 %.3f ms / 增量 %.3f ms（复用 %d 块）\n", full_ms,
                        incr_ms, b.reused);
            expectTrue(b.reused == 1999, "2000 段改一段：其余 1999 块整块搬");
            expectTrue(sameTo(cb, b.keys, a_blocks, a.keys), "2000 段：增量与全量逐字段相同");
            // 耗时只记录、不断言：机器有负载时同一份代码会从 0.5 翻到 0.9 ms（2026-10-20
            //   实测两次翻红），阈值判据在"省了多少"上没有判别力。省没省由结构量钉住：
            //   复用 1999 块、只有 1 块落在损伤区间里 —— 与机器快慢无关。
            expectTrue(dmg_big.range.count() == 1, "2000 段改一段：损伤区间只覆盖 1 块（实测 " +
                                                       std::to_string(dmg_big.range.count()) +
                                                       " 块）");
        }

        // 按区间搬块的前提：damage 的坐标必须与投影的块号同一套。它对不上时，按区间搬
        //   会把块搬到错位置 —— 症状是"画面停在旧内容上"，而内容断言全绿。
        //   两半：分母（after_blocks）与不漏报（区间包含逐段比哈希得到的损伤）。
        {
            auto ledgerOf = [&](const Doc& d) {
                return doc::blockHashes(d, FlowVisibility::Design);
            };
            auto checkShape = [&](const Doc& start, const char* label) {
                Doc cur = start;
                int bad_count = 0;
                int bad_cover = 0;
                for (int k = 0; k < 12; ++k) {
                    const Doc before = cur;
                    switch (k % 4) {
                        case 0: {
                            const auto r2 = doc::insertText(cur, 0, 0, U"Z");
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        case 1: {
                            const auto r2 = doc::splitPara(cur, 0, 0);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        case 2: {
                            const auto r2 = doc::eraseText(cur, 0, 0, 1);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        default: {
                            const auto r2 = doc::mergePara(cur, 0);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                    }
                    const doc::Damage d = doc::damage(before, cur, FlowVisibility::Design);
                    std::vector<BlockSpec> tmp;
                    doc::project(cur, sheet, {}, &tmp);
                    if (d.range.after_blocks != static_cast<int>(tmp.size())) ++bad_count;
                    const doc::BlockDamage h = doc::diffBlocks(ledgerOf(before), ledgerOf(cur));
                    const bool covered =
                        h.empty() ? !d.range.changed
                                  : (d.range.changed && d.range.lo <= h.lo && d.range.hi >= h.hi);
                    if (!covered) ++bad_cover;
                }
                expectTrue(bad_count == 0, std::string(label) + "：damage 的块数与投影逐例相等");
                expectTrue(bad_cover == 0, std::string(label) + "：damage 的区间不漏报");
            };
            checkShape(doc::makeTextDoc({U"甲甲", U"乙乙", U"丙丙", U"丁丁", U"戊戊"}), "五段文本");
            checkShape(doc::makeTextDoc({U"单段"}), "单段");
            checkShape(doc::makeTextDoc({}), "空文档");
            // 末尾没有结束标记：投影会给它补一块，而 paras 不计 —— 坐标最容易差 1 的形状。
            {
                const doc::Ref section = doc::makeNode(
                    2, NodeKind::Section, SectionSpec{},
                    {doc::makeNode(
                        3, NodeKind::TextRun,
                        doc::RunPayload{utf8ToCodepoints("没有结束标记"), CharFormat{}, {}}, {})});
                const Doc open_doc =
                    doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {section}), 4);
                std::vector<BlockSpec> ob;
                doc::project(open_doc, sheet, {}, &ob);
                expectTrue(ob.size() == 1, "末尾开放：投影补出最后那一块（paras 不计它）");
                checkShape(open_doc, "末尾开放");
            }
            {
                const Doc tbl_doc =
                    makeTableDoc(Length::abs(ptToEmu(30.0)), Length::abs(ptToEmu(20.0)));
                checkShape(tbl_doc, "表格文书");
            }
        }
    }

    std::printf("\n[23] 可见性：步骤坐标与损伤块数按同一个口径\n");
    {
        // 隐藏控件把呈现态的流位整体前移。下面这串文书两种口径分别是：
        //   呈现态 0 节首 | 1 A | 2 A | 3 B | 4 B | 5 段落标记
        //   设计态 0 节首 | 1 A | 2 A | 3 控件起 | 4 X | 5 X | 6 控件止 | 7 B | 8 B | 9 标记
        // 同一个整数 4 在两种口径下是两个落点：呈现态是"两个 B 之间"，设计态是隐藏控件里面。
        Doc d = doc::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        doc::EditResult r = doc::insertControl(d, 0, 2, ControlSpec{}, U"XX", &cc);
        expectTrue(r.ok, "可见性：控件插入成功");
        d = r.doc;
        r = doc::setHidden(d, cc, true);
        expectTrue(r.ok, "可见性：控件隐藏成功");
        d = r.doc;

        const doc::FlowIndex dis(d, FlowVisibility::Display);
        const doc::FlowIndex des(d, FlowVisibility::Design);
        expectTrue(dis.size() == 7 && des.size() == 11,
                   "两种可见性的流位数不同（呈现态 7、设计态 11）");

        Step st = InsertText{};
        std::get<InsertText>(st).at = 4;  // 呈现态：两个 B 之间
        std::get<InsertText>(st).cps = {static_cast<uint32_t>(U'Z')};
        const doc::EditResult shown = doc::applySteps(d, {st}, FlowVisibility::Display);
        expectTrue(shown.ok && doc::paraTexts(shown.doc)[0] == U"AAXXBZB",
                   "★ 呈现态位置 4 落在 BB 里（不是落进隐藏控件）");
        // 反向：同一个整数按设计态读，落点就变成隐藏控件内部 —— 字会"打进去但看不见"。
        //   这一条钉的是"口径必须随坐标一起传"，不是"某个实现细节"。
        const doc::EditResult lost = doc::applySteps(d, {st}, FlowVisibility::Design);
        std::string lost_text = lost.ok ? std::string() : ("拒:" + lost.error);
        if (lost.ok) {
            const std::u32string t = doc::paraTexts(lost.doc)[0];
            lost_text.assign(t.begin(), t.end());
        }
        expectTrue(lost.ok && doc::paraTexts(lost.doc)[0] == U"AAZXXBB",
                   "反向：同一个整数按设计态读落进隐藏控件的内容起点（实际 " + lost_text + "）");
    }
    {
        // 损伤的块数：隐藏控件里含段落标记时，呈现态只有一块，设计态有两块。
        Doc d = doc::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        d = doc::insertControl(d, 0, 2, ControlSpec{}, U"XXYY", &cc).doc;
        d = doc::splitPara(d, 0, 4).doc;  // 段落标记落进控件内部
        d = doc::setHidden(d, cc, true).doc;

        const StyleSheet sheet;
        std::vector<BlockSpec> vis;
        std::vector<BlockSpec> des;
        doc::project(d, sheet, doc::ViewParams{FlowVisibility::Display}, &vis);
        doc::project(d, sheet, doc::ViewParams{FlowVisibility::Design}, &des);
        expectTrue(vis.size() == 1 && des.size() == 2,
                   "隐藏控件里的段落标记：呈现态 1 块、设计态 2 块");

        const Doc d1 = doc::insertText(d, 0, 0, U"Z").doc;
        const doc::Damage dm = doc::damage(d, d1, FlowVisibility::Display);
        expectTrue(dm.range.after_blocks == static_cast<int>(vis.size()),
                   "★ 呈现态的 damage.after_blocks 等于呈现态的块数");
        const doc::Damage dm_des = doc::damage(d, d1, FlowVisibility::Design);
        expectTrue(dm_des.range.after_blocks == static_cast<int>(des.size()),
                   "反向：设计态的 damage 仍按设计态数块");

        // 端到端：按这个损伤区间增量投影，块数必须与全量相同。
        //   after_blocks 报大了会把 shift 算成 +1，旧区间右端变 0 ⇒ 一块不删再插一块。
        std::vector<BlockSpec> live;
        doc::BlockHistory hist;
        {
            const doc::Projection p0 =
                doc::project(d, sheet, doc::ViewParams{FlowVisibility::Display}, &live);
            hist.keys = p0.keys;
            hist.style_version = p0.style_version;
        }
        const doc::Projection p1 = doc::project(d1, sheet, doc::ViewParams{FlowVisibility::Display},
                                                &live, &hist, &dm.range);
        expectTrue(live.size() == vis.size(), "★ 增量投影之后的块数与全量相同（不然块被搬重）");
        expectTrue(p1.reused + (dm.range.hi - dm.range.lo) >= 1, "增量真的发生了");
    }

    {
        // 可见性切换：隐藏/显示一个含段落标记的控件，损伤的块数要跟着换口径。
        //   这一对最容易错在"按设计态数了呈现态的块"—— 隐藏时多报一块，按区间搬块就会
        //   把后面那块往前挪错位。
        Doc d = doc::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        d = doc::insertControl(d, 0, 2, ControlSpec{}, U"XXYY", &cc).doc;
        d = doc::splitPara(d, 0, 4).doc;
        const Doc shown = d;
        const Doc hidden = doc::setHidden(shown, cc, true).doc;

        const StyleSheet sheet;
        std::vector<BlockSpec> b_shown;
        std::vector<BlockSpec> b_hidden;
        doc::project(shown, sheet, doc::ViewParams{FlowVisibility::Display}, &b_shown);
        doc::project(hidden, sheet, doc::ViewParams{FlowVisibility::Display}, &b_hidden);
        expectTrue(b_shown.size() == 2 && b_hidden.size() == 1, "隐藏前后：呈现态 2 块 → 1 块");

        const doc::Damage to_hide = doc::damage(shown, hidden, FlowVisibility::Display);
        expectTrue(to_hide.range.after_blocks == static_cast<int>(b_hidden.size()),
                   "隐藏：after_blocks 跟着呈现态少一块（实测 " +
                       std::to_string(to_hide.range.after_blocks) + "）");
        const doc::Damage to_show = doc::damage(hidden, shown, FlowVisibility::Display);
        expectTrue(to_show.range.after_blocks == static_cast<int>(b_shown.size()),
                   "显示：after_blocks 跟着呈现态多一块（实测 " +
                       std::to_string(to_show.range.after_blocks) + "）");
    }

    std::printf("\n[24] 流索引的占用只随活节点数走，不随节点号水位走\n");
    {
        // 节点号是递增水位：新建节点才分配新号，旧号不回收。按节点号开数组的索引会让
        //   占用随会话长度增长，而活节点数并不增长 —— 长会话里它是一条只变差的曲线。
        //   分段再合并：每轮都新建一个段落标记节点，水位只涨不落，段数回到原样。
        std::vector<std::u32string> paras(20, U"段内容长度固定的占位文字。");
        Doc d = doc::makeTextDoc(paras);
        const NodeId id_before = d.next_id;
        const doc::FlowIndex base(d, FlowVisibility::Display);

        int rounds = 0;
        for (int i = 0; i < 3000; ++i) {
            const int p = i % 19;  // 最后一段没有下一段可合并
            const doc::EditResult s = doc::splitPara(d, p, 6);
            if (!s.ok) break;
            const doc::EditResult m = doc::mergePara(s.doc, p);
            if (!m.ok) break;
            d = m.doc;
            ++rounds;
        }
        const doc::FlowIndex grown(d, FlowVisibility::Display);

        // 前提：水位确实涨了、编辑确实发生了。没有这两条，下面的判据在"什么都没做"时恒真。
        expectTrue(rounds == 3000,
                   "前提：三千轮分段+合并都成功（实际 " + std::to_string(rounds) + " 轮）");
        expectTrue(d.next_id >= id_before + 3000, "前提：水位被推高了（" +
                                                      std::to_string(id_before) + " → " +
                                                      std::to_string(d.next_id) + "）");
        expectTrue(static_cast<int>(paras.size()) == static_cast<int>(doc::paraTexts(d).size()),
                   "前提：段数回到原样");

        // 主判据折成"每活节点字节数"：水位涨、节点数涨、两者一起涨，三种情形只有一个数
        //   能把它们分开。上界按"四个数组 + 哈希表容量不超过两倍"给。
        const double per_node =
            static_cast<double>(grown.allocatedBytes()) / std::max(1, grown.nodeCount());
        expectTrue(per_node <= 96.0, "★ 每个活节点的索引开销 ≤ 96 字节（实测 " +
                                         std::to_string(static_cast<long long>(per_node)) +
                                         "，水位 " + std::to_string(d.next_id) + "）");
        expectTrue(grown.allocatedBytes() < static_cast<std::size_t>(d.next_id) * 8,
                   "★ 索引占用与节点号水位脱钩（" +
                       std::to_string(static_cast<unsigned long long>(grown.allocatedBytes())) +
                       " 字节，水位 " + std::to_string(d.next_id) + "）");

        // 反向一半：占用确实随活节点数变，否则上面两条可以靠"恒返回一个小值"通过。
        std::vector<std::u32string> many(400, U"段");
        const Doc big = doc::makeTextDoc(many);
        const std::size_t big_bytes = doc::FlowIndex(big, FlowVisibility::Display).allocatedBytes();
        expectTrue(big_bytes > base.allocatedBytes() * 4,
                   "反向：四百段文档的索引明显更大（" +
                       std::to_string(static_cast<unsigned long long>(big_bytes)) + " 字节）");
    }

    std::printf("\n[25] 剪贴板片段：取、写、读、插回\n");
    {
        // 取整篇：控件不能被摊平成文字，否则"复制一段带控件的文字"粘回来控件就没了。
        const Doc a = makeSpanningControlDoc();
        const doc::FlowIndex idx(a, FlowVisibility::Design);
        const doc::ClipCapture cap = doc::captureRange(a, FlowVisibility::Design, 0, idx.size());
        expectTrue(cap.ok, "取整篇成功（" + cap.error + "）");
        bool has_control = false;
        for (const NodeSpec& s : cap.nodes) {
            if (s.kind == NodeKind::ContentControl) has_control = true;
        }
        expectTrue(has_control, "★ 片段里保留着控件节点（不是被摊平成文字）");
        expectTrue(cap.lo == 0 && cap.hi == idx.size(), "整篇的取用区间就是整条流");

        // 段落标记折成换行：控件内部的段落标记也要出现在纯文本投影里。
        //   每段一枚段落标记，所以每段之后都有一个换行（含最后一段）。
        const std::vector<std::u32string> src_paras = doc::paraTexts(a);
        std::vector<uint32_t> joined;
        for (size_t i = 0; i < src_paras.size(); ++i) {
            const std::vector<uint32_t> cps(src_paras[i].begin(), src_paras[i].end());
            joined.insert(joined.end(), cps.begin(), cps.end());
            joined.push_back(U'\n');
        }
        expectTrue(utf8ToCodepoints(cap.text) == joined,
                   "★ 纯文本投影等于逐段文本以换行结尾相连（实测 " +
                       std::to_string(utf8ToCodepoints(cap.text).size()) + " 个码点，期望 " +
                       std::to_string(joined.size()) + "）");

        const doc::ClipWriteResult w1 = doc::writeFragment(cap.nodes);
        const doc::ClipWriteResult w2 = doc::writeFragment(cap.nodes);
        expectTrue(w1.ok, "片段写得出来（" + w1.error + "）");
        expectTrue(w1.text == w2.text, "同一份内容写两遍，字节完全相同");
        const doc::ClipReadResult r = doc::readFragment(w1.text);
        expectTrue(r.ok, "读得回来（" + r.error + "）");
        const doc::ClipWriteResult w3 = doc::writeFragment(r.nodes);
        expectTrue(w3.ok && w3.text == w1.text, "★ 读回来再写一遍，字节完全相同（往返闭合）");

        // 插进另一份空文档：整份内容原样过去 —— 这是"跨实例复制"的最小形状。
        Doc empty = doc::makeTextDoc({});
        std::vector<Step> steps;
        FlowPos cur = 0;
        for (const NodeSpec& s : r.nodes) {
            InsertNode in;
            in.at_flow = cur;
            in.subtree = s;
            steps.push_back(Step{in});
            cur += static_cast<FlowPos>(s.span());
        }
        const doc::EditResult pasted = doc::applySteps(empty, steps, FlowVisibility::Design);
        expectTrue(pasted.ok, "★ 片段插进空文档成功（" + pasted.error + "）");
        expectTrue(doc::paraCount(pasted.doc) == doc::paraCount(a),
                   "粘出来的段数与来源相同（" + std::to_string(doc::paraCount(pasted.doc)) +
                       " vs " + std::to_string(doc::paraCount(a)) + "）");
        expectTrue(sameText(doc::paraTexts(pasted.doc), src_paras), "粘出来的逐段文本与来源相同");
        const doc::Projection proj = doc::project(pasted.doc, StyleSheet{}, {});
        expectTrue(proj.controls.size() == 1,
                   "粘出来的文档里控件还在（" + std::to_string(proj.controls.size()) + " 个）");

        // 取半段文字：文本按码点截断，标记随节点带走。
        //   流位置 0 是节的开边界，所以 [1, 3) 盖住的是头两个码点。
        const Doc plain = doc::makeTextDoc({U"甲乙丙丁"});
        const doc::ClipCapture part = doc::captureRange(plain, FlowVisibility::Design, 1, 3);
        expectTrue(part.ok, "取中间两个字（" + part.error + "）");
        expectTrue(
            part.nodes.size() == 1 && part.nodes[0].kind == NodeKind::TextRun,
            "半段文字取出来是一个文本节点（实际 " + std::to_string(part.nodes.size()) + " 个）");
        expectTrue(part.nodes[0].text.size() == 2,
                   "截断到两个码点（实测 " + std::to_string(part.nodes[0].text.size()) + "）");
        expectTrue(part.text == "甲乙", "截出来的正是那两个码点（实测 " + part.text + "）");

        // 空区间与越界：不产出一个"看起来成功"的空片段。
        const doc::ClipCapture none = doc::captureRange(a, FlowVisibility::Design, 2, 2);
        expectTrue(!none.ok && !none.error.empty(), "空区间被拒并给出原因（" + none.error + "）");

        // 表格整块：端点落在表内时向外扩到整张表的两端。
        const Doc tbl = makeTableDoc(Length::abs(ptToEmu(30.0)), Length::abs(ptToEmu(20.0)));
        const doc::FlowIndex ti(tbl, FlowVisibility::Design);
        int tf = 0;
        int te = 0;
        NodeId table_id = kInvalidNodeId;
        for (const doc::Ref& c : tbl.root->children.front()->children) {
            if (c->kind != NodeKind::Table) continue;
            table_id = c->id;
            ti.intervalOf(c->id, &tf, &te);
        }
        expectTrue(table_id != kInvalidNodeId && te > tf + 2, "前提：表格在流里有区间");
        // 从表格内部一格开始取：取用区间必须扩到整张表。
        const doc::ClipCapture half =
            doc::captureRange(tbl, FlowVisibility::Design, tf + 3, te - 1);
        expectTrue(half.ok, "从表格内部取一段（" + half.error + "）");
        expectTrue(half.lo <= tf && half.hi >= te,
                   "★ 取用区间扩到整张表（" + std::to_string(half.lo) + ".." +
                       std::to_string(half.hi) + "，表 " + std::to_string(tf) + ".." +
                       std::to_string(te) + "）");
        bool has_table = false;
        for (const NodeSpec& s : half.nodes) {
            if (s.kind == NodeKind::Table) has_table = true;
        }
        expectTrue(has_table, "扩出来的片段里有整张表");
        expectTrue(!half.text.empty(), "表格的纯文本投影非空（有字）");
    }

    std::printf("\n[26] 排版支持边界：按节页度量与页眉页脚显式拒绝\n");
    {
        std::string why;
        expectTrue(doc::checkSupportedLayout(doc::makeTextDoc({U"甲乙"}), &why),
                   "纯正文文档可排版");

        // 纸张的默认宽高与引擎默认页度量必须是同一张 A4。两处各写一份数字时差 5 个 EMU
        //   （0.0004 磅），那种差看不出来，只会在某一行刚好压线时分页多出一行。
        //   反向：把默认宽高改回零或改回 210mm 口径，这条即红。
        const Paper a4{};
        const PageMetrics def = PageMetrics::A4();
        expectTrue(a4.width == Length::abs(ptToEmu(def.width_pt)) &&
                       a4.height == Length::abs(ptToEmu(def.height_pt)),
                   "纸张默认宽高与 PageMetrics 默认同源（" + std::to_string(a4.width.abs_emu) +
                       " / " + std::to_string(a4.height.abs_emu) + "）");

        // 规格名不进文档：纸张段只写宽高与方向；旧记录里带 name 键也不读。
        //   反向：把写入端的 name 段恢复，「落盘里没有规格名」即红；把默认宽高改回零，
        //   「全默认的节不写纸张段」与上面那条同源判据一起红。
        {
            const doc::SerializeResult plain = doc::serialize(doc::makeTextDoc({U"甲"}));
            expectTrue(plain.ok, plain.ok ? "默认文档序列化成功" : plain.error);
            expectTrue(plain.ok && plain.text.find("\"paper\"") == std::string::npos,
                       "全默认的节不写纸张段");

            // A3 的宽高在 C++ 侧没有任何规格名，只能按长度给 —— 这条用例本身就是
            //   「规格名不进文档」的正面证据。
            SectionSpec a3;
            a3.paper.width = Length::abs(ptToEmu(841.89));
            a3.paper.height = Length::abs(ptToEmu(1190.551));
            const doc::Ref sec =
                doc::makeNode(2, NodeKind::Section, a3,
                              {doc::makeNode(3, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
            const doc::Doc src = doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {sec}), 4);
            const doc::SerializeResult saved = doc::serialize(src);
            expectTrue(saved.ok, saved.ok ? "A3 文档序列化成功" : saved.error);
            expectTrue(saved.ok && saved.text.find("\"name\"") == std::string::npos,
                       "落盘里没有纸张规格名");

            doc::Doc back;
            const doc::DeserializeResult loaded = doc::deserialize(saved.text, &back);
            expectTrue(loaded.ok, loaded.ok ? "A3 文档读得回来" : loaded.error);
            const SectionSpec* got = nullptr;
            if (back.root) {
                for (const doc::Ref& c : back.root->children) {
                    if (c && c->kind == NodeKind::Section) {
                        got = std::get_if<SectionSpec>(&c->payload);
                        break;
                    }
                }
            }
            expectTrue(got != nullptr && got->paper == a3.paper, "宽高按磅往返逐值相同");
        }

        SectionSpec spec;
        spec.headers[0].root = 9;  // 有页眉内容
        const doc::Ref header_sec =
            doc::makeNode(2, NodeKind::Section, spec,
                          {doc::makeNode(3, NodeKind::TextRun,
                                         doc::RunPayload{cpsOf(U"页眉"), CharFormat{}, {}}, {}),
                           doc::makeNode(4, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const Doc header_doc =
            doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {header_sec}), 10);
        why.clear();
        expectTrue(!doc::checkSupportedLayout(header_doc, &why) && !why.empty(),
                   "有页眉内容时拒绝（" + why + "）");

        SectionSpec margins;
        margins.margins.left = Length::abs(ptToEmu(72.0));
        const doc::Ref margin_sec =
            doc::makeNode(2, NodeKind::Section, margins,
                          {doc::makeNode(4, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const Doc margin_doc =
            doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {margin_sec}), 5);
        // 整篇一份页度量是支持的：装载时把这一份采用成引擎页度量（见 adoptPageFromDocument），
        //   所以单节写非默认页边距不再拒绝。
        why.clear();
        expectTrue(doc::checkSupportedLayout(margin_doc, &why),
                   "单节写了页边距仍然可排版（" + why + "）");

        // 多节各写各的才拒绝 —— 那是按节排版，不在支持范围里。
        SectionSpec other_margins;
        other_margins.margins.left = Length::abs(ptToEmu(36.0));
        const doc::Ref sec_a =
            doc::makeNode(6, NodeKind::Section, margins,
                          {doc::makeNode(7, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const doc::Ref sec_b =
            doc::makeNode(8, NodeKind::Section, other_margins,
                          {doc::makeNode(9, NodeKind::ParagraphMark, doc::MarkPayload{}, {})});
        const Doc two_papers =
            doc::adopt(doc::makeNode(1, NodeKind::Document, {}, {sec_a, sec_b}), 10);
        why.clear();
        expectTrue(!doc::checkSupportedLayout(two_papers, &why) && !why.empty(),
                   "多节页边距不同时拒绝（" + why + "）");

        why.clear();
        expectTrue(!doc::checkSupportedLayout(makeAtomDoc(""), &why) && !why.empty(),
                   "域元素尚未进入排版时拒绝（" + why + "）");
    }

    std::printf("\n[27] 样式表随文档往返\n");
    {
        StyleSheet sheet;
        ParaFormatPatch defaults_para;
        defaults_para.space_after = Length::abs(ptToEmu(6.0));
        CharFormat defaults_run;
        defaults_run.size = Length::abs(ptToEmu(12.0));
        sheet.setDocDefaults(defaults_para, defaults_run);

        StyleDef body;
        body.name = "正文";
        body.kind = StyleKind::Paragraph;
        body.para.space_before = Length::abs(ptToEmu(3.0));
        body.run.bold = true;
        sheet.add(body);

        StyleDef emphasis;
        emphasis.name = "强调";
        emphasis.kind = StyleKind::Character;
        emphasis.based_on = "正文";
        emphasis.run.italic = true;
        sheet.add(emphasis);

        const Doc doc = doc::makeTextDoc({U"甲乙"});
        const doc::SerializeResult saved = doc::serialize(doc, sheet);
        expectTrue(saved.ok, "样式表与文档一起写出（" + saved.error + "）");

        Doc loaded;
        StyleSheet loaded_sheet;
        const doc::DeserializeResult read = doc::deserialize(saved.text, &loaded, &loaded_sheet);
        expectTrue(read.ok && read.has_styles, "读回样式表（" + read.error + "）");
        expectTrue(loaded_sheet.docDefaultsRun().size.has_value() &&
                       *loaded_sheet.docDefaultsRun().size == Length::abs(ptToEmu(12.0)),
                   "文档默认字符格式读回");
        const StyleDef* got_body = loaded_sheet.find(StyleKind::Paragraph, "正文");
        expectTrue(got_body != nullptr && got_body->run.bold == std::optional<bool>(true),
                   "段落样式的字符格式读回");
        const StyleDef* got_emphasis = loaded_sheet.find(StyleKind::Character, "强调");
        expectTrue(got_emphasis != nullptr && got_emphasis->based_on == "正文" &&
                       got_emphasis->run.italic == std::optional<bool>(true),
                   "字符样式的继承与格式读回");
    }

    std::printf("\n[28] 分块孩子表：只有改动所在的那个块被换掉\n");
    {
        std::vector<std::u32string> paras;
        paras.reserve(80);
        for (int i = 0; i < 80; ++i)
            paras.push_back(U"甲" + std::u32string(1, char32_t(U'0' + i % 10)));
        const Doc before = doc::makeTextDoc(paras);
        const doc::Children& before_kids = *doc::bodyOf(before);
        const doc::EditResult edited = doc::insertText(before, 0, 0, U"乙");
        expectTrue(edited.ok, "改第一段（" + edited.error + "）");
        const doc::Children& after_kids = *doc::bodyOf(edited.doc);
        expectTrue(before_kids.size() == after_kids.size(), "孩子数不变");
        expectTrue(!before_kids.sharesChunkWith(after_kids, 0), "改动所在块被换掉");
        expectTrue(before_kids.sharesChunkWith(after_kids, 100), "未改动块仍然共享");
    }
    std::printf("\n[29] 投影文本切片：不复制整段\n");
    {
        const Doc doc = doc::makeTextDoc({U"甲乙丙"});
        std::vector<BlockSpec> blocks;
        const doc::Projection pr = doc::project(doc, StyleSheet{}, {}, &blocks);
        expectTrue(pr.ok() && blocks.size() == 1, "投影出一块");
        const doc::Node* run = nullptr;
        for (const doc::Ref& child : *doc::bodyOf(doc)) {
            if (child->kind == NodeKind::TextRun) {
                run = child.get();
                break;
            }
        }
        expectTrue(run != nullptr, "找到第一枚文本节点");
        if (run != nullptr) {
            const doc::Text& source = std::get<doc::RunPayload>(run->payload).text;
            expectTrue(blocks[0].cp_text.sharesBufferWith(source),
                       "投影块的文本共享修订里的缓冲区");
            expectTrue(blocks[0].cp_text.data() == source.data(), "码点指针直接指向共享缓冲区");
        }
    }
    std::printf("\n结果：%d 项断言，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
