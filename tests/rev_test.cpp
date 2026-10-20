/**
 * 不可变修订层的判据。这一层要证明的只有三件事：编辑不碰旧值、没被改到的子树仍然共享、
 * 哈希能当"内容相同"的判据用。段落文本的正确性用一份朴素模型（vector<u32string>）对照，
 * 两者走同一串随机操作，每一步都比对。
 *
 * 判据必须含反向一半：新实现拒绝的操作，朴素模型也要认为它非法；哈希"应当相同"的地方
 * 相等、"应当不同"的地方不等 —— 只测相等会让一个恒返回 0 的哈希也通过。
 */
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "core/doc/compile.h"
#include "core/doc/document.h"
#include "core/doc/flow.h"
#include "core/doc/steps.h"
#include "core/doc/transaction.h"
#include "core/rev/flow.h"
#include "core/rev/project.h"
#include "core/rev/rev.h"
#include "core/rev/serialize.h"
#include "core/rev/session.h"
#include "core/rev/steps.h"

#include "demo/demo_documents.h"

#include "block_compare.h"

using namespace heditor;
using heditor::rev::Doc;
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
        const auto r = rev::insertText(*doc, p, cp, add);
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
        const auto r = rev::eraseText(*doc, p, cp, n);
        if (!r.ok) {
            ++*skipped;
            return false;
        }
        naive->erase(p, cp, n);
        *doc = r.doc;
        return true;
    }
    if (op == 2) {
        const auto r = rev::splitPara(*doc, p, cp);
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
    const auto r = rev::mergePara(*doc, p);
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
    return out;
}

/**
 * 两棵修订树是不是同一份：身份、种类、隐藏标记、段落数、哈希、子节点逐个比。
 *
 * 载荷不在这里比：它进哈希，哈希相同就够了。身份要单独比，因为哈希刻意不含身份，而
 * "读回来之后新旧节点号撞上"恰恰只能靠身份抓。
 */
bool sameTree(const rev::Ref& a, const rev::Ref& b) {
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
rev::Doc makeTableDoc(const Length& col0, const Length& row0_height) {
    int id = 3;  // 1 是 Document，2 是 Section
    auto run = [&id](const std::u32string& t) {
        return rev::makeNode(id++, NodeKind::TextRun, rev::RunPayload{cpsOf(t), CharFormat{}, {}},
                             {});
    };
    auto mark = [&id]() {
        return rev::makeNode(id++, NodeKind::ParagraphMark, rev::MarkPayload{}, {});
    };
    auto cell = [&](const std::u32string& t, const CellSpec& spec) {
        return rev::makeNode(id++, NodeKind::TableCell, spec, {run(t), mark()});
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

    const rev::Ref row0 =
        rev::makeNode(id++, NodeKind::TableRow, r0,
                      {cell(U"A0", merged), cell(U"B0", CellSpec{}), cell(U"C0", CellSpec{})});
    const rev::Ref row1 = rev::makeNode(id++, NodeKind::TableRow, RowSpec{},
                                        {cell(U"A1", CellSpec{}), cell(U"B1", CellSpec{})});
    const rev::Ref table = rev::makeNode(id++, NodeKind::Table, ts, {row0, row1});
    const rev::Ref section = rev::makeNode(2, NodeKind::Section, SectionSpec{},
                                           {run(U"表前"), mark(), table, run(U"表后"), mark()});
    return rev::adopt(rev::makeNode(1, NodeKind::Document, {}, {section}), id);
}

/**
 * 与 makeTableDoc 逐字段相同的模型层文档。两条路（编译与投影）的产物品比对它。
 */
NodeTree makeTableTree(const Length& col0, const Length& row0_height) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"表前"));
    tree.appendChild(sec, NodeKind::ParagraphMark);

    const NodeId table = tree.appendChild(sec, NodeKind::Table);
    Node* tn = tree.find(table);
    tn->table.columns.push_back(TableColumn{"patient.id", "住院号", "text", "input",
                                            Length::abs(ptToEmu(12.0)), Align::Center});
    tn->table.grid_cols = {col0, Length::abs(ptToEmu(40.0)), Length::percent(1, 2)};
    tn->table.width = Length::percent(1, 1);
    tn->table.align = Align::Center;

    auto add_row = [&](const RowSpec& rs) {
        const NodeId row = tree.appendChild(table, NodeKind::TableRow);
        tree.find(row)->row = rs;
        return row;
    };
    auto add_cell = [&](NodeId row, const std::u32string& t, const CellSpec& cs) {
        const NodeId c = tree.appendChild(row, NodeKind::TableCell);
        tree.find(c)->cell = cs;
        tree.setText(tree.appendChild(c, NodeKind::TextRun), cpsOf(t));
        tree.appendChild(c, NodeKind::ParagraphMark);
    };

    RowSpec r0;
    r0.is_header = true;
    r0.can_split_by_page = false;
    r0.page_break_before = true;
    r0.height = row0_height;
    r0.height_rule = HeightRule::AtLeast;
    CellSpec merged;
    merged.row_span = 2;
    merged.v_align = CellVAlign::Center;

    const NodeId row0 = add_row(r0);
    add_cell(row0, U"A0", merged);
    add_cell(row0, U"B0", CellSpec{});
    add_cell(row0, U"C0", CellSpec{});
    const NodeId row1 = add_row(RowSpec{});
    add_cell(row1, U"A1", CellSpec{});
    add_cell(row1, U"B1", CellSpec{});

    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"表后"));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

/**
 * 两套模型的节点号不同，格路径里的表格句柄没法直接比 —— 比对前把它归零。
 */
BlockSpec withoutTableHandle(BlockSpec b) {
    for (CellSlot& slot : b.cell_path) slot.table = 0;
    return b;
}

/**
 * 原子夹具：六段，把盒子几何的四条岔路与一枚公式、一个域都走一遍。
 *
 * 段落 0：文字 + 图片（只有像素）+ 文字
 * 段落 1：图片（只给绝对宽）
 * 段落 2：图片（只给绝对高，有像素 → 按原图比例推宽）
 * 段落 3：图片（宽是百分比 → 相对长度，按原始尺寸折算并记诊断）
 * 段落 4：公式（两边都 Auto → 按表达式估框）
 * 段落 5：文字 + 域（文档属性，带缓存值）
 *
 * cache 单独一个参数：它是求值结果，不进内容哈希，判据要能构造出"只有它不同"的两份。
 */
NodeTree makeAtomTree(const std::string& cache) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    auto text = [&](const std::u32string& t) {
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(t));
    };
    auto end = [&]() { tree.appendChild(sec, NodeKind::ParagraphMark); };
    auto image = [&](const ImageSpec& spec) {
        const NodeId id = tree.appendChild(sec, NodeKind::Image);
        tree.find(id)->image = spec;
        return id;
    };
    auto formula = [&](const FormulaSpec& spec) {
        const NodeId id = tree.appendChild(sec, NodeKind::Formula);
        tree.find(id)->formula = spec;
        return id;
    };

    text(U"图");
    ImageSpec px_only;
    px_only.resource_id = "res/a.png";
    px_only.px_w = 200;
    px_only.px_h = 100;
    image(px_only);
    text(U"后");
    end();

    ImageSpec width_only;
    width_only.resource_id = "res/b.png";
    width_only.width = Length::abs(ptToEmu(30.0));
    image(width_only);
    end();

    ImageSpec height_only;
    height_only.resource_id = "res/c.png";
    height_only.px_w = 100;
    height_only.px_h = 50;
    height_only.height = Length::abs(ptToEmu(20.0));
    image(height_only);
    end();

    ImageSpec pct;
    pct.resource_id = "res/d.png";
    pct.px_w = 100;
    pct.px_h = 100;
    pct.width = Length::percent(1, 2);
    image(pct);
    end();

    FormulaSpec f;
    f.expr = "x^2";
    formula(f);
    end();

    text(U"值");
    FieldSpec field;
    field.kind = FieldKind::DocProperty;
    field.name = "patient.name";
    field.cache = cache;
    const NodeId fid = tree.appendChild(sec, NodeKind::Field);
    tree.find(fid)->field = field;
    end();
    return tree;
}

/**
 * 两条路的诊断是不是同一份（种类、节点、文本，顺序也算）。
 *
 * 只比"都有诊断"是不够的：诊断是这层唯一能说明"某段内容没进产物"的通道，少一条就是
 * 静默丢内容。
 */
bool sameDiags(const std::vector<CompileDiagnostic>& a, const std::vector<CompileDiagnostic>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].node != b[i].node || a[i].detail != b[i].detail) {
            return false;
        }
    }
    return true;
}

/**
 * 嵌套表格夹具：外表格里两格，第一格里先是文字 "A"，再是一张两格的内表格。
 *
 * 段落号：0 是"前"，1 是 "A"，2 与 3 是内表格两格，4 是 "B"，5 是"后"。
 * 结构与 makeAtomTree 同一条路子：模型树建一次，fromTree 负责搬成修订。
 */
NodeTree makeNestedTableTree(const Length& inner_col0) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    auto text = [&](NodeId parent, const std::u32string& t) {
        tree.setText(tree.appendChild(parent, NodeKind::TextRun), cpsOf(t));
    };
    auto end = [&](NodeId parent) { tree.appendChild(parent, NodeKind::ParagraphMark); };
    auto cell = [&](NodeId row, const std::u32string& t) {
        const NodeId c = tree.appendChild(row, NodeKind::TableCell);
        text(c, t);
        end(c);
        return c;
    };

    text(sec, U"前");
    end(sec);

    const NodeId outer = tree.appendChild(sec, NodeKind::Table);
    Node* otn = tree.find(outer);
    otn->table.grid_cols = {Length::abs(ptToEmu(50.0)), Length::abs(ptToEmu(50.0))};
    otn->table.width = Length::percent(1, 1);

    const NodeId row0 = tree.appendChild(outer, NodeKind::TableRow);
    const NodeId cell0 = tree.appendChild(row0, NodeKind::TableCell);
    text(cell0, U"A");
    end(cell0);

    const NodeId inner = tree.appendChild(cell0, NodeKind::Table);
    Node* itn = tree.find(inner);
    itn->table.grid_cols = {inner_col0, Length::abs(ptToEmu(20.0))};
    itn->table.width = Length::percent(1, 1);
    const NodeId irow = tree.appendChild(inner, NodeKind::TableRow);
    cell(irow, U"i1");
    cell(irow, U"i2");

    cell(row0, U"B");

    text(sec, U"后");
    end(sec);
    return tree;
}

/**
 * 跨段控件夹具：控件从第一段中间开始、到第二段中间结束 —— 段落标记在控件内部。
 *
 * 这是"控件是一个容器，段落是它的孩子"那条语义的最小形状，也是投影与编译最容易分叉的
 * 一处：一对边界要落在两个不同的块上，两块的文本各自都要把控件里的文字算进自己的段。
 */
NodeTree makeSpanningControlTree() {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"甲"));
    const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
    tree.find(cc)->control.key = "k";
    tree.find(cc)->control.controlType = "text";
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cpsOf(U"乙"));
    tree.appendChild(cc, NodeKind::ParagraphMark);
    tree.setText(tree.appendChild(cc, NodeKind::TextRun), cpsOf(U"丙"));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"丁"));
    tree.appendChild(sec, NodeKind::ParagraphMark);
    return tree;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== 不可变修订层：路径复制、结构共享、内容哈希 ===\n");

    std::printf("\n[1] 建文档与读出\n");
    {
        const Doc d = rev::makeTextDoc({U"甲乙", U"", U"丙"});
        expectTrue(rev::paraCount(d) == 3, "三段（含一个空段）");
        expectTrue(sameText(rev::paraTexts(d), {U"甲乙", U"", U"丙"}), "逐段文本与输入一致");
        const Doc empty = rev::makeTextDoc({});
        expectTrue(rev::paraCount(empty) == 0, "空文档：零段");
        expectTrue(rev::paraTexts(empty).empty(), "空文档：读不出段落");
    }

    std::printf("\n[2] 随机编辑序列与朴素模型逐步比对\n");
    {
        std::mt19937 rng(20261019u);
        Doc doc = rev::makeTextDoc({U"abc", U"", U"de"});
        Naive naive{{U"abc", U"", U"de"}};
        int mismatches = 0;
        int skipped = 0;
        const int kSteps = 400;
        for (int step = 0; step < kSteps; ++step) {
            applyRandomOp(rng, &doc, &naive, &skipped);
            if (!sameText(rev::paraTexts(doc), naive.paras)) {
                ++mismatches;
                std::printf("      第 %d 步不一致：新 %s / 旧 %s\n", step,
                            dump(rev::paraTexts(doc)).c_str(), dump(naive.paras).c_str());
                break;
            }
        }
        expectTrue(mismatches == 0, "连续 400 步编辑，每一步的逐段文本都与朴素模型一致");
        expectTrue(skipped < kSteps, "随机序列真的执行了编辑（不是全被拒绝）");
        std::printf("      最终 %d 段：%s\n", rev::paraCount(doc), dump(naive.paras).c_str());
    }

    std::printf("\n[3] 旧值不被改动\n");
    {
        Doc doc = rev::makeTextDoc({U"hello", U"world"});
        std::vector<Doc> history;
        std::vector<std::vector<std::u32string>> expected;
        for (int i = 0; i < 10; ++i) {
            history.push_back(doc);
            expected.push_back(rev::paraTexts(doc));
            const auto r = rev::insertText(doc, 0, 0, U"x");
            if (!r.ok) break;
            doc = r.doc;
        }
        int bad = 0;
        for (size_t i = 0; i < history.size(); ++i) {
            if (!sameText(rev::paraTexts(history[i]), expected[i])) ++bad;
        }
        expectTrue(bad == 0, "十次编辑之后，之前每一份修订读出来的文本都没变");
        expectTrue(history.size() >= 5, "确实积累了多份修订");
    }

    std::printf("\n[4] 结构共享\n");
    {
        Doc doc = rev::makeTextDoc({U"aaa", U"bbb", U"ccc"});
        const rev::Ref before_root = doc.root;
        const rev::Ref before_sec = doc.root->children.front();
        const auto r = rev::insertText(doc, 0, 1, U"z");
        expectTrue(r.ok, "改第 0 段成功");
        const rev::Ref after_sec = r.doc.root->children.front();
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
        const Doc a = rev::makeTextDoc({U"abc"});
        Doc b = rev::makeTextDoc({U"ab"});
        const auto r = rev::insertText(b, 0, 2, U"c");
        expectTrue(r.ok, "第二条构造路径：在 ab 后面插 c");
        b = r.doc;
        expectTrue(sameText(rev::paraTexts(a), rev::paraTexts(b)), "两条路径的文本相同");
        expectTrue(a.lo == b.lo && a.hi == b.hi, "内容相同则哈希相同（与构造路径无关）");

        Doc c = rev::makeTextDoc({U"abc"});
        const auto r2 = rev::insertText(c, 0, 1, U"X");
        expectTrue(r2.ok, "改一个码点成功");
        c = r2.doc;
        expectTrue(c.lo != a.lo || c.hi != a.hi, "改一个码点，哈希就不同");

        const auto r3 = rev::eraseText(c, 0, 1, 1);
        expectTrue(r3.ok, "把那一个码点删回去成功");
        expectTrue(r3.doc.lo == a.lo && r3.doc.hi == a.hi, "改回原样，哈希回到原值");

        const Doc two = rev::makeTextDoc({U"ab", U""});
        const Doc one = rev::makeTextDoc({U"ab"});
        expectTrue(two.lo != one.lo || two.hi != one.hi,
                   "多一个空段，哈希不同（哈希表达结构，不只是拼起来的文本）");
    }

    std::printf("\n[6] 非法操作被拒绝，且不给一个看起来没变的文档\n");
    {
        const Doc d = rev::makeTextDoc({U"ab", U"cd"});
        expectTrue(!rev::insertText(d, 2, 0, U"x").ok, "段落越界被拒");
        expectTrue(!rev::insertText(d, 0, 3, U"x").ok, "码点越界被拒");
        expectTrue(!rev::eraseText(d, 0, 1, 5).ok, "跨段删除被拒（不替调用方猜语义）");
        expectTrue(!rev::mergePara(d, 1).ok, "最后一段没有下一段可合并");
        expectTrue(!rev::splitPara(d, 0, 9).ok, "分段位置越界被拒");
        const auto r = rev::eraseText(d, 0, 0, 0);
        expectTrue(r.ok && r.doc.lo == d.lo, "删 0 个码点是合法的空操作，哈希不变");
    }

    std::printf("\n[7] 控件：插入、读出、隐藏\n");
    {
        ControlSpec spec;
        spec.key = "patient_name";
        spec.controlType = "text";
        spec.required = true;

        Doc d = rev::makeTextDoc({U"姓名："});
        NodeId cc = kInvalidNodeId;
        const auto r = rev::insertControl(d, 0, 3, spec, U"张三", &cc);
        expectTrue(r.ok, "在段内插入控件成功");
        d = r.doc;
        expectTrue(rev::paraCount(d) == 1, "控件不切段：仍是一段");
        expectTrue(rev::paraTexts(d)[0] == U"姓名：张三", "控件里的文字算作这一段自己的文字");
        const rev::Node* n = rev::findNode(d, cc);
        expectTrue(n != nullptr && n->kind == NodeKind::ContentControl, "按节点号找得到那个控件");
        const auto& got = std::get<ControlSpec>(n->payload);
        expectTrue(got.key == "patient_name" && got.required, "控件记录原样存住（字段没有丢）");

        const auto proj = rev::project(d, StyleSheet{}, {});
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
        const auto rh = rev::setHidden(d, cc, true);
        expectTrue(rh.ok, "设置隐藏成功");
        const rev::Projection design = rev::project(rh.doc, StyleSheet{}, {});
        const rev::Projection display =
            rev::project(rh.doc, StyleSheet{}, rev::ViewParams{FlowVisibility::Display});
        expectTrue(design.blocks.size() == 1 && design.blocks[0].cp_text.size() == 5 &&
                       design.blocks[0].objects.size() == 2,
                   "编辑态：隐藏控件照常参与投影（作者要能选中它改回来）");
        expectTrue(display.blocks.size() == 1 && display.blocks[0].cp_text.size() == 3 &&
                       display.blocks[0].objects.empty() && display.controls.empty(),
                   "呈现态：隐藏的控件整棵摘除，文字与边界都不在了");

        const auto rsame = rev::setHidden(rh.doc, cc, true);
        expectTrue(rsame.ok && rsame.doc.lo == rh.doc.lo && rsame.doc.hi == rh.doc.hi,
                   "重复设置同一个隐藏值是空操作，哈希不变");
    }

    std::printf("\n[8] 投影与编译逐字段一致（差分判据）\n");
    {
        ControlSpec spec;
        spec.key = "patient_name";
        spec.controlType = "text";
        spec.appearance.placeholder = "请填写";
        spec.appearance.underline = Underline::BlankOnly;
        spec.readonly = true;

        // 同一份文档两条构造路径：修订层（插控件）与节点树（appendChild）。
        Doc rdoc = rev::makeTextDoc({U"姓名：", U"", U"尾"});
        NodeId rcc = kInvalidNodeId;
        const auto rc = rev::insertControl(rdoc, 0, 3, spec, U"张三", &rcc);
        expectTrue(rc.ok, "修订层：插入控件");
        rdoc = rc.doc;

        NodeTree tree;
        const NodeId sec = tree.firstSectionId();
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"姓名："));
        const NodeId cc = tree.appendChild(sec, NodeKind::ContentControl);
        tree.find(cc)->control = spec;
        tree.setText(tree.appendChild(cc, NodeKind::TextRun), cpsOf(U"张三"));
        tree.appendChild(sec, NodeKind::ParagraphMark);
        tree.appendChild(sec, NodeKind::ParagraphMark);
        tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"尾"));
        tree.appendChild(sec, NodeKind::ParagraphMark);

        const StyleSheet sheet;
        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        const rev::Projection pr = rev::project(rdoc, sheet, {});

        expectTrue(cr.ok() && pr.ok(), "两条路都没有诊断");
        expectTrue(pr.blocks.size() == cr.blocks.size() && pr.blocks.size() == 3,
                   "块数一致（三段三块）");
        bool same_spec = pr.blocks.size() == cr.blocks.size();
        for (size_t i = 0; same_spec && i < pr.blocks.size(); ++i) {
            if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                same_spec = false;
                std::printf("      第 %zu 块不同：%s\n", i,
                            blockDiff(pr.blocks[i], cr.blocks[i]).c_str());
            }
        }
        expectTrue(same_spec, "逐块 BlockSpec 逐字段相同（文本、runs、行内对象、块级格式）");

        // 两边的节点号各自从 1 起分配，号码不可比；可比的是"它指向的那枚节点在各自模型里
        // 确实是一个段落标记"—— 来源身份指错地方时这条会红。
        bool source_ok = pr.blocks.size() == cr.sources.size();
        for (size_t i = 0; source_ok && i < pr.blocks.size(); ++i) {
            const rev::Node* rm = rev::findNode(rdoc, pr.keys[i].source.mark);
            const Node* cm = tree.find(cr.sources[i].mark);
            if (rm == nullptr || cm == nullptr || rm->kind != NodeKind::ParagraphMark ||
                cm->kind != NodeKind::ParagraphMark) {
                source_ok = false;
            }
        }
        expectTrue(source_ok, "每块的来源身份在两边都指向一枚段落标记");

        expectTrue(cr.blocks.size() == 3 && cr.blocks[0].objects.size() == 2 &&
                       cr.blocks[0].objects[1].cp == 5,
                   "编译侧的边界锚点与上一条一致（判据不是自己跟自己比）");

        // 呈现态一条：隐藏之后两条路都要同样地摘掉它。
        tree.find(cc)->hidden = true;
        // 两边的节点号各自分配，不能混用：修订层那个控件号是 rcc，树里那个是 cc。
        const auto rh = rev::setHidden(rdoc, rcc, true);
        const CompileResult cd = compileDocument(tree, FlowVisibility::Display, sheet);
        const rev::Projection pd =
            rev::project(rh.doc, sheet, rev::ViewParams{FlowVisibility::Display});
        bool same_hidden = cd.blocks.size() == pd.blocks.size();
        for (size_t i = 0; same_hidden && i < cd.blocks.size(); ++i) {
            if (!sameBlock(pd.blocks[i], cd.blocks[i])) {
                same_hidden = false;
                std::printf("      呈现态第 %zu 块不同：%s\n", i,
                            blockDiff(pd.blocks[i], cd.blocks[i]).c_str());
            }
        }
        expectTrue(same_hidden, "呈现态：隐藏控件之后两条路仍逐字段相同");
    }

    std::printf("\n[9] 块哈希：改一段只动那一段，损伤区间精确\n");
    {
        Doc d = rev::makeTextDoc({U"aa", U"bb", U"cc", U"dd", U"ee"});
        const auto h0 = rev::blockHashes(d, FlowVisibility::Design);
        expectTrue(h0.size() == 5, "五段五块");

        const auto r = rev::insertText(d, 2, 1, U"X");
        const auto h1 = rev::blockHashes(r.doc, FlowVisibility::Design);
        expectTrue(h0[0] == h1[0] && h0[1] == h1[1] && h0[3] == h1[3] && h0[4] == h1[4],
                   "没改到的块哈希一个都没变（缓存因此可以照用）");
        expectTrue(!(h0[2] == h1[2]), "改到的那一块哈希变了");
        const rev::BlockDamage d1 = rev::diffBlocks(h0, h1);
        expectTrue(d1.lo == 2 && d1.hi == 3, "损伤正好是改到的那一块");

        const auto sp = rev::splitPara(d, 0, 0);
        const rev::BlockDamage d2 =
            rev::diffBlocks(h0, rev::blockHashes(sp.doc, FlowVisibility::Design));
        expectTrue(d2.lo == 0 && d2.hi == 1, "段首插一段：损伤只有新出现的那一块（后缀整体复用）");

        const auto mg = rev::mergePara(d, 1);
        const rev::BlockDamage d3 =
            rev::diffBlocks(h0, rev::blockHashes(mg.doc, FlowVisibility::Design));
        expectTrue(d3.lo == 1 && d3.hi == 2, "合并第 1、2 段：损伤只有合并后的那一块");

        const auto noop = rev::insertText(d, 0, 0, U"");
        expectTrue(rev::diffBlocks(h0, rev::blockHashes(noop.doc, FlowVisibility::Design)).empty(),
                   "空编辑：损伤为空（不会凭空触发一轮重排）");

        // 整篇删空：区间长度是 0，但"变了"这件事必须说出口 —— 页表要清掉。
        const Doc empty = rev::makeTextDoc({});
        const rev::BlockDamage d4 =
            rev::diffBlocks(h0, rev::blockHashes(empty, FlowVisibility::Design));
        expectTrue(!d4.empty() && d4.count() == 0, "整篇删空：损伤区间为空但标记为变了");

        // 反方向：改回原样，哈希回到原值。
        const auto back = rev::eraseText(r.doc, 2, 1, 1);
        expectTrue(rev::diffBlocks(h0, rev::blockHashes(back.doc, FlowVisibility::Design)).empty(),
                   "改动撤回之后，损伤回到空（哈希是内容判据，不是版本号）");
    }

    std::printf("\n[10] 控件的每一个字段都进内容哈希\n");
    {
        auto hashOf = [](const ControlSpec& s) {
            const auto r = rev::insertControl(rev::makeTextDoc({U"x"}), 0, 0, s, U"v");
            return rev::blockHashes(r.doc, FlowVisibility::Design).at(0);
        };
        const auto variants = controlVariants();
        const rev::BlockHash h0 = hashOf(variants.front().second);

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
        const Doc same = rev::makeTextDoc(base);
        const rev::Damage none = rev::damage(same, same, FlowVisibility::Design);
        expectTrue(!none.range.changed, "同一份修订：没有损伤");
        expectTrue(none.visited == 1, "同一份修订：只比较了根一个节点（整篇没变是 O(1)）");

        // 段内改字（块数不变）：与块哈希比对逐字段一致。
        const Doc d0 = rev::makeTextDoc(base);
        const Doc d1 = rev::insertText(d0, 2, 1, U"X").doc;
        const rev::Damage inplace = rev::damage(d0, d1, FlowVisibility::Design);
        const rev::BlockDamage ref = rev::diffBlocks(rev::blockHashes(d0, FlowVisibility::Design),
                                                     rev::blockHashes(d1, FlowVisibility::Design));
        expectTrue(inplace.range.changed && ref.changed, "段内改字：两边都判为有损伤");
        expectTrue(inplace.range.lo == ref.lo && inplace.range.hi == ref.hi,
                   "段内改字：区间与块哈希比对完全一致（不许漏，也不许白报）");
        expectTrue(inplace.range.lo == 2 && inplace.range.hi == 3, "段内改字：正好是第 2 块");

        // 空编辑：没有变化，一步剪掉。
        const Doc noop = rev::insertText(d0, 0, 0, U"").doc;
        const rev::Damage dn = rev::damage(d0, noop, FlowVisibility::Design);
        expectTrue(!dn.range.changed && dn.visited == 1, "空编辑：没有损伤，且只看了根节点");

        // 结构改动：分段多一块、合并少一块，两者都只动一块。
        const Doc sp = rev::splitPara(d0, 0, 0).doc;
        const rev::Damage ds = rev::damage(d0, sp, FlowVisibility::Design);
        // cp=0 分段时模型会丢掉原 run、再建一个内容相同的新 run（新节点号），于是"删一个、
        //   插一个"被分别记成两块。多报的那一块内容与旧块完全相同，重排时命中块缓存 ——
        //   这里不为它加"按哈希跨位配对"的启发式：那类规则的价值只有一次缓存查询，
        //   而它的失效方式（把真变化的两个节点当成同一份内容）是静默的。区间上限钉在 2。
        expectTrue(ds.range.changed && ds.range.lo == 0 && ds.range.hi >= 1 && ds.range.hi <= 2,
                   "段首分段：从第 0 块起，最多两块（实际 [" + std::to_string(ds.range.lo) + "," +
                       std::to_string(ds.range.hi) + ")）");

        const Doc mg = rev::mergePara(d0, 1).doc;
        const rev::Damage dm = rev::damage(d0, mg, FlowVisibility::Design);
        const rev::BlockDamage rm = rev::diffBlocks(rev::blockHashes(d0, FlowVisibility::Design),
                                                    rev::blockHashes(mg, FlowVisibility::Design));
        expectTrue(dm.range.lo == 1 && dm.range.hi == 2, "合并两段：只有合并后的那一块（实际 [" +
                                                             std::to_string(dm.range.lo) + "," +
                                                             std::to_string(dm.range.hi) + ")）");
        expectTrue(rm.lo == dm.range.lo && rm.hi == dm.range.hi,
                   "合并两段：块哈希比对也对得上（后缀对齐吸收了挪位，实际 [" +
                       std::to_string(rm.lo) + "," + std::to_string(rm.hi) + ")）");

        // 尾部删空：块数变了但没有任何块要重排，与块哈希比对同一口径。
        const Doc empty = rev::makeTextDoc({});
        const rev::BlockDamage re =
            rev::diffBlocks(rev::blockHashes(d0, FlowVisibility::Design),
                            rev::blockHashes(empty, FlowVisibility::Design));
        const rev::Damage de = rev::damage(d0, empty, FlowVisibility::Design);
        expectTrue(de.range.changed && de.range.lo == re.lo && de.range.hi == re.hi,
                   "整篇删空：区间与块哈希比对一致（空区间，但标记为变了）");

        // 随机序列：块数不变的那些步必须与块哈希比对完全一致。
        {
            std::mt19937 rng(20261019u);
            Doc cur = rev::makeTextDoc({U"aa", U"bb"});  // 两段：够切也够并
            int compared = 0;
            int mismatched = 0;
            for (int step = 0; step < 300; ++step) {
                const int op = static_cast<int>(rng() % 4);
                const int paras = rev::paraCount(cur);
                const auto texts = rev::paraTexts(cur);
                const int p =
                    paras > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(paras)) : 0;
                const int len =
                    paras > 0 ? static_cast<int>(texts[static_cast<size_t>(p)].size()) : 0;
                rev::EditResult r;
                if (op == 0) {
                    r = rev::insertText(
                        cur, p,
                        len > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(len + 1)) : 0,
                        U"z");
                } else if (op == 1) {
                    r = rev::eraseText(cur, p, 0, 1 + static_cast<int>(rng() % 2));
                } else if (op == 2) {
                    r = rev::splitPara(
                        cur, p,
                        len > 0 ? static_cast<int>(rng() % static_cast<uint32_t>(len + 1)) : 0);
                } else {
                    r = rev::mergePara(cur, p);
                }
                if (!r.ok) continue;
                const std::vector<rev::BlockHash> hb =
                    rev::blockHashes(cur, FlowVisibility::Design);
                const std::vector<rev::BlockHash> ha =
                    rev::blockHashes(r.doc, FlowVisibility::Design);
                const rev::Damage got = rev::damage(cur, r.doc, FlowVisibility::Design);
                const bool count_same = hb.size() == ha.size();
                if (count_same) {
                    ++compared;
                    const rev::BlockDamage want = rev::diffBlocks(hb, ha);
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
            const Doc a1 = rev::makeTextDoc(short_doc);
            const Doc b1 = rev::insertText(a1, 399, 1, U"X").doc;
            const Doc a2 = rev::makeTextDoc(long_doc);
            const Doc b2 = rev::insertText(a2, 399, 1, U"X").doc;
            const rev::Damage m1 = rev::damage(a1, b1, FlowVisibility::Design);
            const rev::Damage m2 = rev::damage(a2, b2, FlowVisibility::Design);
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
                sink += rev::damage(a1, b1, FlowVisibility::Design).visited;
            const auto t1 = clock::now();
            for (int k = 0; k < 200; ++k) {
                sink += rev::diffBlocks(rev::blockHashes(a1, FlowVisibility::Design),
                                        rev::blockHashes(b1, FlowVisibility::Design))
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

        Doc d = rev::makeTextDoc({U"前"});
        NodeId cc = kInvalidNodeId;
        const auto rc = rev::insertControl(d, 0, 1, spec, U"ab", &cc);
        expectTrue(rc.ok, "前字之后插入一个控件（内容是 ab）");
        d = rc.doc;
        expectTrue(rev::paraTexts(d)[0] == U"前ab", "控件里的字算作这一段自己的文字");

        const rev::Node* run_before = d.root->children.front()->children.front().get();

        // 位置 2 落在控件内部的 run 里（前=0，a=1，b=2）。
        const auto ri = rev::insertText(d, 0, 2, U"X");
        expectTrue(ri.ok, "在控件内部打字（不再被拒绝）");
        const Doc inside = ri.doc;
        expectTrue(rev::paraTexts(inside)[0] == U"前aXb", "字落在控件内部、位置正确（前aXb）");
        expectTrue(rev::paraCount(inside) == 1, "控件内部打字不切段");
        expectTrue(rev::findNode(inside, cc) != nullptr, "控件还在，没有被打字挤掉");
        expectTrue(inside.root->children.front()->children.front().get() == run_before,
                   "控件之前那一枚 run 是同一个指针（容器之外的节点一个都没复制）");
        expectTrue(rev::paraTexts(d)[0] == U"前ab", "旧修订没有被改动");

        // 差分：同一份内容经节点树构造，投影结果必须逐字段相同。
        {
            NodeTree tree;
            const NodeId sec = tree.firstSectionId();
            tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"前"));
            const NodeId tcc = tree.appendChild(sec, NodeKind::ContentControl);
            tree.find(tcc)->control = spec;
            tree.setText(tree.appendChild(tcc, NodeKind::TextRun), cpsOf(U"aXb"));
            tree.appendChild(sec, NodeKind::ParagraphMark);

            const StyleSheet sheet;
            const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
            const rev::Projection pr = rev::project(inside, sheet, {});
            bool same = pr.ok() && cr.ok() && pr.blocks.size() == cr.blocks.size();
            for (size_t i = 0; same && i < pr.blocks.size(); ++i) {
                if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                    same = false;
                    std::printf("      第 %zu 块不同：%s\n", i,
                                blockDiff(pr.blocks[i], cr.blocks[i]).c_str());
                }
            }
            expectTrue(same, "控件内部打字：投影与编译逐字段相同");
        }

        // 在控件内部回车：段落标记落进控件，控件因此跨段。
        const auto rsp = rev::splitPara(inside, 0, 2);
        expectTrue(rsp.ok, "在控件内部回车（不再被拒绝）");
        const Doc crossed = rsp.doc;
        expectTrue(rev::paraCount(crossed) == 2, "控件内部回车真的多出一段");
        expectTrue(rev::paraTexts(crossed)[0] == U"前a" && rev::paraTexts(crossed)[1] == U"Xb",
                   "两段的文字都对（控件因此横跨两段）");

        {
            const StyleSheet sheet;
            const rev::Projection pr = rev::project(crossed, sheet, {});
            expectTrue(pr.ok() && pr.blocks.size() == 2, "投影两段两块");
            expectTrue(pr.controls.size() == 1, "仍是同一个控件");
            expectTrue(pr.controls[0].block == 0 && pr.controls[0].end_block == 1,
                       "控件跨度横跨两块（起在第 0 块、止在第 1 块）");

            NodeTree tree;
            const NodeId sec = tree.firstSectionId();
            tree.setText(tree.appendChild(sec, NodeKind::TextRun), cpsOf(U"前"));
            const NodeId tcc = tree.appendChild(sec, NodeKind::ContentControl);
            tree.find(tcc)->control = spec;
            tree.setText(tree.appendChild(tcc, NodeKind::TextRun), cpsOf(U"a"));
            tree.appendChild(tcc, NodeKind::ParagraphMark);
            tree.setText(tree.appendChild(tcc, NodeKind::TextRun), cpsOf(U"Xb"));
            tree.appendChild(sec, NodeKind::ParagraphMark);

            const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
            bool same = cr.ok() && pr.blocks.size() == cr.blocks.size();
            for (size_t i = 0; same && i < pr.blocks.size(); ++i) {
                if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                    same = false;
                    std::printf("      跨段第 %zu 块不同：%s\n", i,
                                blockDiff(pr.blocks[i], cr.blocks[i]).c_str());
                }
            }
            expectTrue(same, "跨段控件：投影与编译逐字段相同（含两块各自的行内对象）");
        }

        // 损伤：分段之后两块都要重排。
        const rev::Damage dm = rev::damage(inside, crossed, FlowVisibility::Design);
        expectTrue(dm.range.changed && dm.range.lo == 0 && dm.range.hi == 2,
                   "控件内分段：损伤覆盖两块（实际 [" + std::to_string(dm.range.lo) + "," +
                       std::to_string(dm.range.hi) + ")）");

        // 反向：越界的码点仍然被拒，且不返回一份"看起来没变"的文档。
        const auto bad = rev::insertText(crossed, 0, 99, U"Z");
        expectTrue(!bad.ok && !bad.error.empty(), "控件内部码点越界仍然被拒，且带原因");
        const auto bad2 = rev::insertText(crossed, 5, 0, U"Z");
        expectTrue(!bad2.ok, "段落越界仍然被拒");
    }

    std::printf("\n[13] 往返：写完再读回来，身份、哈希与记录字段一个不少\n");
    {
        const Doc plain = rev::makeTextDoc({U"第一段", U"", U"abc"});
        const auto typed = rev::insertText(plain, 2, 1, U"中");
        expectTrue(typed.ok, "准备：段内插字");

        ControlSpec spec = controlBase();
        spec.valueShape = ValueShape::Enum;
        spec.value.text = "opt";
        spec.appearance.border.bottom.on = true;
        spec.appearance.placeholder = "请填写";
        NodeId control = kInvalidNodeId;
        const auto placed = rev::insertControl(typed.doc, 0, 1, spec, U"标签", &control);
        expectTrue(placed.ok && control != kInvalidNodeId, "准备：插入控件并拿到节点号");
        const Doc doc = rev::setHidden(placed.doc, control, true).doc;

        const auto ser = rev::serialize(doc);
        expectTrue(ser.ok && !ser.text.empty(),
                   ser.ok ? std::string("序列化成功且非空") : ("序列化失败：" + ser.error));
        Doc back;
        const auto de = rev::deserialize(ser.text, &back);
        expectTrue(de.ok, de.ok ? std::string("反序列化成功") : ("反序列化失败：" + de.error));
        expectTrue(back.lo == doc.lo && back.hi == doc.hi,
                   "全文哈希相同（参与哈希的字段一个都没漏）");
        expectTrue(back.next_id == doc.next_id, "分配器水位相同");
        expectTrue(rev::paraCount(back) == rev::paraCount(doc), "段落数相同");
        expectTrue(rev::paraTexts(back) == rev::paraTexts(doc), "逐段文字相同");
        expectTrue(sameTree(doc.root, back.root), "节点身份、种类、隐藏标记、段落数、哈希逐个相同");
        expectTrue(rev::damage(doc, back, FlowVisibility::Design).range.empty(),
                   "两份修订之间没有损伤（没有块要重排）");

        const auto again = rev::serialize(back);
        expectTrue(again.ok && again.text == ser.text, "再写一遍字节完全相同（写端是确定的）");

        // 字符属性与段落覆盖：这两个载荷只在非默认时才写，所以另起一份全非默认的修订。
        rev::RunPayload run;
        run.text = {U'A'};
        run.marks.bold = true;
        run.marks.italic = false;
        run.marks.underline = true;
        run.marks.font_index = 3;
        run.marks.size = Length::abs(ptToEmu(12.5));
        run.marks.color = Color::rgb(1, 2, 3);
        run.style = "强调";
        rev::MarkPayload mark;
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
        const rev::Ref rich_section =
            rev::makeNode(2, NodeKind::Section, SectionSpec{},
                          {rev::makeNode(10, NodeKind::TextRun, run, {}),
                           rev::makeNode(11, NodeKind::ParagraphMark, mark, {})});
        const Doc rich = rev::adopt(rev::makeNode(1, NodeKind::Document, {}, {rich_section}), 12);

        const auto rich_ser = rev::serialize(rich);
        Doc rich_back;
        const auto rich_de = rev::deserialize(rich_ser.text, &rich_back);
        expectTrue(rich_ser.ok && rich_de.ok && rich_back.lo == rich.lo && rich_back.hi == rich.hi,
                   "字符属性与段落覆盖往返之后哈希相同");
        const rev::Node* got_run = rev::findNode(rich_back, 10);
        const rev::Node* got_mark = rev::findNode(rich_back, 11);
        expectTrue(got_run != nullptr && got_mark != nullptr, "两个富格式节点都读回来了");
        if (got_run != nullptr) {
            const auto& rp = std::get<rev::RunPayload>(got_run->payload);
            const bool same = rp.marks.bold.has_value() && *rp.marks.bold &&
                              rp.marks.italic.has_value() && !*rp.marks.italic &&
                              rp.marks.underline.has_value() && *rp.marks.underline &&
                              rp.marks.font_index.has_value() && *rp.marks.font_index == 3 &&
                              rp.marks.color.has_value() &&
                              *rp.marks.color == Color::rgb(1, 2, 3) && rp.style == "强调";
            expectTrue(same, "字符属性逐项读回来（三态、字号、颜色、样式引用）");
        }
        if (got_mark != nullptr) {
            const auto& mp = std::get<rev::MarkPayload>(got_mark->payload);
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
                rev::insertControl(rev::makeTextDoc({U"x"}), 0, 0, variant, U"v", &created);
            const auto text = rev::serialize(r.doc);
            Doc rt;
            const bool ok = text.ok && rev::deserialize(text.text, &rt).ok;
            const rev::Node* n = ok ? rev::findNode(rt, created) : nullptr;
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
            const auto r = rev::deserialize(text, &target);
            const bool kept = target.root == doc.root;
            expectTrue(!r.ok && !r.error.empty() && kept, what);
        };
        rejects(R"({"v":2,"next":9,"root":{}})", "版本号不对：拒绝");
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
        const rev::Ref dup = rev::makeNode(
            1, NodeKind::Document, {},
            {rev::makeNode(2, NodeKind::Section, SectionSpec{},
                           {rev::makeNode(3, NodeKind::TextRun,
                                          rev::RunPayload{cpsOf(U"a"), CharFormat{}, {}}, {}),
                            rev::makeNode(3, NodeKind::TextRun,
                                          rev::RunPayload{cpsOf(U"b"), CharFormat{}, {}}, {})})});
        expectTrue(!rev::serialize(rev::adopt(dup, 9)).ok, "节点号重复的修订：写不出来");
    }

    std::printf("\n[14] 表格：投影与编译逐字段相同，容器几何进块哈希\n");
    {
        const Length col0 = Length::abs(ptToEmu(30.0));
        const Length row_h = Length::abs(ptToEmu(20.0));
        const rev::Doc doc = makeTableDoc(col0, row_h);
        NodeTree tree = makeTableTree(col0, row_h);
        const StyleSheet sheet;

        expectTrue(rev::paraCount(doc) == 7, "七段：表前、五个格、表后");

        const rev::Projection pr = rev::project(doc, sheet);
        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        expectTrue(pr.ok() && cr.ok(), "两条路都没有诊断");
        expectTrue(pr.blocks.size() == cr.blocks.size(),
                   "块数相同（投影 " + std::to_string(pr.blocks.size()) + " / 编译 " +
                       std::to_string(cr.blocks.size()) + "）");

        bool same = pr.blocks.size() == cr.blocks.size();
        size_t bad = 0;
        for (size_t i = 0; same && i < pr.blocks.size(); ++i) {
            if (!sameBlock(withoutTableHandle(pr.blocks[i]), withoutTableHandle(cr.blocks[i]))) {
                same = false;
                bad = i;
            }
        }
        expectTrue(same, "每一块与编译产物逐字段相同（第一处不同：第 " + std::to_string(bad) +
                             " 块 " +
                             (same ? std::string("无")
                                   : blockDiff(withoutTableHandle(pr.blocks[bad]),
                                               withoutTableHandle(cr.blocks[bad]))));

        expectTrue(pr.tables.size() == cr.tables.size() && pr.tables.size() == 1,
                   "表格结构数量相同，只有一张");
        expectTrue(!pr.tables.empty() && sameTableShape(pr.tables[0], cr.tables[0]),
                   "表格结构逐字段相同（行、列、列宽、行高、每格的块区间）");
        expectTrue(!pr.blocks.empty() && pr.blocks[1].cell_path.size() == 1 &&
                       pr.blocks[1].cell_path[0].row == 0 && pr.blocks[1].cell_path[0].cell == 0,
                   "第一格里的块带着它的格路径");
        expectTrue(pr.blocks[6].cell_path.empty(), "表后的正文块没有格路径");
        expectTrue(pr.controls.empty(), "表格里没有控件");

        // 改一格文字：损伤只落在那一块。
        const auto typed = rev::insertText(doc, 2, 0, U"X");
        expectTrue(typed.ok, "格内插字：投影层能按段落号落进格");
        const rev::Damage one = rev::damage(doc, typed.doc, FlowVisibility::Design);
        expectTrue(one.range.changed && one.range.lo == 2 && one.range.hi == 3,
                   "改一格文字：损伤正好是那一块（实际 [" + std::to_string(one.range.lo) + "," +
                       std::to_string(one.range.hi) + ")）");
        expectTrue(typed.ok && rev::paraTexts(typed.doc)[2] == U"XB0", "字落进了第二格");

        // 改列宽：内容一个都没动，但整张表都要重排。
        const rev::Doc wide = makeTableDoc(Length::abs(ptToEmu(45.0)), row_h);
        const std::vector<rev::BlockHash> before = rev::blockHashes(doc, FlowVisibility::Design);
        const std::vector<rev::BlockHash> after = rev::blockHashes(wide, FlowVisibility::Design);
        expectTrue(before.size() == after.size(), "改列宽不改块数");
        bool outside_changed = before[0] != after[0] || before[6] != after[6];
        bool inside_changed = true;
        for (size_t i = 1; i < 6; ++i) {
            inside_changed = inside_changed && before[i] != after[i];
        }
        expectTrue(!outside_changed && inside_changed,
                   "改列宽：表内五块的哈希全变，表外的两块一个都没变");
        const rev::Damage dmg = rev::damage(doc, wide, FlowVisibility::Design);
        const rev::BlockDamage ref = rev::diffBlocks(before, after);
        expectTrue(dmg.range.lo == 1 && dmg.range.hi == 6,
                   "改列宽：损伤是整张表（实际 [" + std::to_string(dmg.range.lo) + "," +
                       std::to_string(dmg.range.hi) + ")）");
        expectTrue(dmg.range.lo == ref.lo && dmg.range.hi == ref.hi,
                   "改列宽：哈希下降与块哈希比对完全一致");

        // 改行高：同样只动容器记录，整张表进损伤。
        // 行高只影响那一行（三个格），不影响另一行 —— 行是这一层容器，它的范围就是自己的块。
        const rev::Doc tall = makeTableDoc(col0, Length::abs(ptToEmu(36.0)));
        const rev::Damage dh = rev::damage(doc, tall, FlowVisibility::Design);
        expectTrue(dh.range.lo == 1 && dh.range.hi == 4, "改行高：损伤是第一行的三块（实际 [" +
                                                             std::to_string(dh.range.lo) + "," +
                                                             std::to_string(dh.range.hi) + ")）");

        // 格内分段：一格里多出一块，结构表的 block_count 跟着变。
        const auto split = rev::splitPara(typed.doc, 2, 1);
        expectTrue(split.ok, "格内分段");
        const rev::Projection pr2 = rev::project(split.doc, sheet);
        expectTrue(pr2.ok() && pr2.blocks.size() == 8, "格内分段之后多一块");
        expectTrue(!pr2.tables.empty() && pr2.tables[0].cells.size() == 5, "结构表的格数没变");
        expectTrue(!pr2.tables.empty() && pr2.tables[0].cells[1].block_count == 2,
                   "第二格的块区间变成两块");

        // 落盘：表格记录逐字段（列定义、列宽、行开关、行高、跨度、垂直对齐）。
        const auto ser = rev::serialize(doc);
        Doc back;
        const auto de = ser.ok ? rev::deserialize(ser.text, &back) : rev::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == doc.lo && back.hi == doc.hi,
                   "表格往返之后全文哈希相同");
        const auto probe = [](const Doc& d) {
            const std::vector<rev::Ref>* body = rev::bodyOf(d);
            return (body != nullptr && body->size() > 2) ? (*body)[2] : rev::Ref{};
        };
        const rev::Ref a = probe(doc);
        const rev::Ref b = probe(back);
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
        NodeTree tree = makeAtomTree("张三");
        Doc doc;
        std::string why;
        const bool exported = rev::fromTree(tree, &doc, &why);
        expectTrue(exported, exported ? std::string("从模型导出修订") : ("导出失败：" + why));

        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        const rev::Projection pr = rev::project(doc, sheet);
        expectTrue(cr.blocks.size() == 6 && pr.blocks.size() == 6,
                   "六段（投影 " + std::to_string(pr.blocks.size()) + " / 编译 " +
                       std::to_string(cr.blocks.size()) + "）");

        bool same = cr.blocks.size() == pr.blocks.size();
        size_t bad = 0;
        for (size_t i = 0; same && i < cr.blocks.size(); ++i) {
            if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                same = false;
                bad = i;
            }
        }
        expectTrue(same, "每一块与编译产物逐字段相同（含原子的宽高与内容指纹）" +
                             (same ? std::string()
                                   : "：第 " + std::to_string(bad) + " 块 " +
                                         blockDiff(pr.blocks[bad], cr.blocks[bad])));
        expectTrue(sameDiags(pr.diagnostics, cr.diagnostics),
                   "诊断逐条相同（相对长度那条、域未进排版输入那条）");

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
        NodeTree other = makeAtomTree("张三");
        const NodeId sec = other.firstSectionId();
        const Node* first_img = other.find(sec)->children[1];
        expectTrue(first_img->kind == NodeKind::Image, "准备：第二节点的确是那张图");
        other.find(first_img->id)->image.resource_id = "res/z.png";
        Doc doc2;
        expectTrue(rev::fromTree(other, &doc2, &why), "改资源标识之后仍能导出");
        const auto h1 = rev::blockHashes(doc, FlowVisibility::Design);
        const auto h2 = rev::blockHashes(doc2, FlowVisibility::Design);
        expectTrue(h1.size() == h2.size() && h1[0] != h2[0] && h1[1] == h2[1],
                   "换一张同样大的图：只有那一块的哈希变");

        // 域的缓存值是派生量：只有它不同时，内容哈希必须相同。
        NodeTree other_cache = makeAtomTree("李四");
        Doc doc3;
        expectTrue(rev::fromTree(other_cache, &doc3, &why), "改缓存值之后仍能导出");
        expectTrue(doc3.lo == doc.lo && doc3.hi == doc.hi,
                   "只改域的缓存值：全文哈希不变（求值结果不是内容）");
        expectTrue(rev::blockHashes(doc3, FlowVisibility::Design) ==
                       rev::blockHashes(doc, FlowVisibility::Design),
                   "只改缓存值：块哈希一个都没变（没有块要重排）");

        // 落盘：三类记录逐字段。域的 cache 不在哈希里，所以它只能靠逐字段比对。
        const auto ser = rev::serialize(doc);
        Doc back;
        const auto de = ser.ok ? rev::deserialize(ser.text, &back) : rev::DeserializeResult{};
        expectTrue(ser.ok && de.ok, "原子与域能落盘并读回来");
        expectTrue(de.ok && back.lo == doc.lo && back.hi == doc.hi, "往返之后全文哈希相同");
        const auto body = [](const Doc& d) -> const std::vector<rev::Ref>* {
            return rev::bodyOf(d);
        };
        const std::vector<rev::Ref>* a = body(doc);
        const std::vector<rev::Ref>* b = body(back);
        int compared = 0;
        int mismatched = 0;
        if (a != nullptr && b != nullptr && a->size() == b->size()) {
            for (size_t i = 0; i < a->size(); ++i) {
                const rev::Node& x = *(*a)[i];
                const rev::Node& y = *(*b)[i];
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
        NodeTree multi;
        const NodeId s0 = multi.firstSectionId();
        const NodeId s1 = multi.appendChild(multi.rootId(), NodeKind::Section);
        multi.setText(multi.appendChild(s0, NodeKind::TextRun), cpsOf(U"a"));
        multi.appendChild(s0, NodeKind::ParagraphMark);
        multi.setText(multi.appendChild(s1, NodeKind::TextRun), cpsOf(U"b"));
        multi.appendChild(s1, NodeKind::ParagraphMark);
        Doc two;
        std::string why2;
        expectTrue(rev::fromTree(multi, &two, &why2), "多节文档：导出成功");
        expectTrue(rev::paraCount(two) == 2 &&
                       (rev::paraTexts(two) == std::vector<std::u32string>{U"a", U"b"}),
                   "两节的段落都在（不是只搬第一节）");
    }

    std::printf("\n[16] 嵌套表格：格路径是序列，容器几何各管各的一段\n");
    {
        const StyleSheet sheet;
        const Length inner_col = Length::abs(ptToEmu(18.0));
        NodeTree tree = makeNestedTableTree(inner_col);
        Doc doc;
        std::string why;
        expectTrue(rev::fromTree(tree, &doc, &why), "嵌套表：从模型导出修订");

        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        const rev::Projection pr = rev::project(doc, sheet);
        expectTrue(cr.ok() && pr.ok(), "两条路都没有诊断");
        expectTrue(cr.blocks.size() == 6 && pr.blocks.size() == 6,
                   "六块：前 / A / i1 / i2 / B / 后（投影 " + std::to_string(pr.blocks.size()) +
                       "，编译 " + std::to_string(cr.blocks.size()) + "）");

        bool same = cr.blocks.size() == pr.blocks.size();
        std::string where;
        for (size_t i = 0; same && i < cr.blocks.size(); ++i) {
            if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                same = false;
                where = "第 " + std::to_string(i) + " 块 " + blockDiff(pr.blocks[i], cr.blocks[i]);
            }
        }
        expectTrue(same, "每一块与编译产物逐字段相同" + (same ? std::string() : "：" + where));
        expectTrue(pr.tables.size() == 2 && cr.tables.size() == 2, "两张结构表（外 + 内）");
        expectTrue(pr.tables.size() == 2 && cr.tables.size() == 2 &&
                       sameTableShape(pr.tables[0], cr.tables[0]) &&
                       sameTableShape(pr.tables[1], cr.tables[1]),
                   "两张结构表各自逐字段相同（列宽、行高、每格的块区间）");

        // 格路径是"外层到内层"的序列：内表格里的块带两层，外表格另一格的块只带一层。
        expectTrue(pr.blocks[2].cell_path.size() == 2 && pr.blocks[3].cell_path.size() == 2,
                   "内表格的块带两层格路径");
        expectTrue(pr.blocks[2].cell_path[0].table != pr.blocks[2].cell_path[1].table,
                   "两层格路径指向两张不同的表");
        expectTrue(pr.blocks[4].cell_path.size() == 1, "外表格另一格的块只带一层");
        expectTrue(pr.blocks[1].cell_path.size() == 1 && pr.blocks[5].cell_path.empty(),
                   "格里的文字一层，表后的正文没有格路径");

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
        NodeTree wider = makeNestedTableTree(Length::abs(ptToEmu(30.0)));
        Doc doc2;
        expectTrue(rev::fromTree(wider, &doc2, &why), "改内层列宽之后仍能导出");
        const auto before = rev::blockHashes(doc, FlowVisibility::Design);
        const auto after = rev::blockHashes(doc2, FlowVisibility::Design);
        expectTrue(before.size() == after.size() && before[0] == after[0] &&
                       before[1] == after[1] && before[4] == after[4] && before[5] == after[5],
                   "改内层列宽：表外的两块与外表格另一格一个都没变");
        expectTrue(before[2] != after[2] && before[3] != after[3], "改内层列宽：内表格两块都变了");
        const rev::Damage dmg = rev::damage(doc, doc2, FlowVisibility::Design);
        const rev::BlockDamage ref = rev::diffBlocks(before, after);
        expectTrue(dmg.range.changed && dmg.range.lo == 2 && dmg.range.hi == 4,
                   "改内层列宽：损伤只到内表格两块（实际 [" + std::to_string(dmg.range.lo) + "," +
                       std::to_string(dmg.range.hi) + ")）");
        expectTrue(dmg.range.lo == ref.lo && dmg.range.hi == ref.hi,
                   "改内层列宽：哈希下降与块哈希比对一致");

        // 改外层列宽：外表格覆盖的块整段算损伤 —— 内表格也在外面那一层里。
        NodeTree outer_wider = makeNestedTableTree(inner_col);
        Node* ot = outer_wider.find(outer_wider.firstSectionId())->children[2];
        expectTrue(ot->kind == NodeKind::Table, "准备：节下第三个节点是外表格");
        ot->table.grid_cols[0] = Length::abs(ptToEmu(70.0));
        Doc doc3;
        expectTrue(rev::fromTree(outer_wider, &doc3, &why), "改外层列宽之后仍能导出");
        const rev::Damage dmg2 = rev::damage(doc, doc3, FlowVisibility::Design);
        expectTrue(dmg2.range.lo == 1 && dmg2.range.hi == 5,
                   "改外层列宽：损伤是整张外表格（[1,5)，实际 [" + std::to_string(dmg2.range.lo) +
                       "," + std::to_string(dmg2.range.hi) + ")）");

        // 落盘：嵌套结构原样回来。
        const auto ser = rev::serialize(doc);
        Doc back;
        const auto de = ser.ok ? rev::deserialize(ser.text, &back) : rev::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == doc.lo && back.hi == doc.hi,
                   "嵌套表格往返之后全文哈希相同");
        const rev::Projection pr2 = de.ok ? rev::project(back, sheet) : rev::Projection{};
        expectTrue(pr2.blocks.size() == pr.blocks.size() && !pr2.tables.empty() &&
                       sameTableShape(pr2.tables[0], pr.tables[0]),
                   "读回来再投影：块数与外表格结构不变");
    }

    std::printf("\n[17] 跨段控件：一对边界落在两块上\n");
    {
        const StyleSheet sheet;
        NodeTree tree = makeSpanningControlTree();
        Doc doc;
        std::string why;
        expectTrue(rev::fromTree(tree, &doc, &why), "跨段控件：从模型导出修订");
        expectTrue((rev::paraTexts(doc) == std::vector<std::u32string>{U"甲乙", U"丙", U"丁"}),
                   "三段：控件里的段落标记切开了它自己的内容");

        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        const rev::Projection pr = rev::project(doc, sheet);
        expectTrue(cr.ok() && pr.ok(), "两条路都没有诊断");
        bool same = cr.blocks.size() == pr.blocks.size() && pr.blocks.size() == 3;
        std::string where;
        for (size_t i = 0; same && i < cr.blocks.size(); ++i) {
            if (!sameBlock(pr.blocks[i], cr.blocks[i])) {
                same = false;
                where = "第 " + std::to_string(i) + " 块 " + blockDiff(pr.blocks[i], cr.blocks[i]);
            }
        }
        expectTrue(same, "每一块与编译产物逐字段相同" + (same ? std::string() : "：" + where));

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
        const rev::Node* cc_node =
            rev::bodyOf(doc) != nullptr ? (*rev::bodyOf(doc))[1].get() : nullptr;
        expectTrue(cc_node != nullptr && cc_node->kind == NodeKind::ContentControl &&
                       pr.controls[0].node.get() == cc_node,
                   "跨度指向的是那个控件节点本身（不是它的某个孩子）");

        // 呈现态：隐藏它，它内部的文字与两个边界一起消失；控件的段落标记也在内部，所以
        // 前两段并成一段。树那一侧要同样地隐藏 —— 节点号在两边不同，按形状找它（节下第二个孩子）。
        Node* model_cc = tree.find(tree.firstSectionId())->children[1];
        expectTrue(model_cc->kind == NodeKind::ContentControl, "准备：树里那个节点是控件");
        model_cc->hidden = true;
        const auto hid = rev::setHidden(doc, cc_node->id, true);
        const rev::Projection disp =
            rev::project(hid.doc, sheet, rev::ViewParams{FlowVisibility::Display});
        const CompileResult cdisp = compileDocument(tree, FlowVisibility::Display, sheet);
        bool same_disp = disp.blocks.size() == cdisp.blocks.size();
        std::string disp_where;
        for (size_t i = 0; same_disp && i < cdisp.blocks.size(); ++i) {
            if (!sameBlock(disp.blocks[i], cdisp.blocks[i])) {
                same_disp = false;
                disp_where =
                    "第 " + std::to_string(i) + " 块 " + blockDiff(disp.blocks[i], cdisp.blocks[i]);
            }
        }
        expectTrue(same_disp && disp.blocks.size() == 2 && disp.controls.empty(),
                   "呈现态：隐藏之后两条路仍逐字段相同（两段），控件表为空" +
                       (same_disp ? std::string() : "：" + disp_where));
    }

    std::printf("\n[18] 会话层：撤销栈是一串旧修订，不是一串逆操作\n");
    {
        const Doc initial = rev::makeTextDoc({U"第一段", U"第二段"});
        rev::Session s(initial, 6);
        Doc direct = initial;

        // 同一串编辑同时走会话与原语：会话多出来的只有"记住旧值"，其余必须一模一样。
        auto directOf = [](const Doc& d, const rev::Edit& e) {
            switch (e.kind) {
                case rev::Edit::Kind::InsertText:
                    return rev::insertText(d, e.para, e.cp, e.text);
                case rev::Edit::Kind::EraseText:
                    return rev::eraseText(d, e.para, e.cp, e.count);
                case rev::Edit::Kind::SplitPara:
                    return rev::splitPara(d, e.para, e.cp);
                case rev::Edit::Kind::MergePara:
                    return rev::mergePara(d, e.para);
                default:
                    return rev::EditResult{};
            }
        };

        std::mt19937 rng(20261020u);
        int steps = 0;
        int rejected = 0;
        int mismatched = 0;
        std::vector<Doc> timeline{initial};
        for (int i = 0; i < 120; ++i) {
            const std::vector<std::u32string> texts = rev::paraTexts(direct);
            const int paras = static_cast<int>(texts.size());
            rev::Edit e;
            e.para = static_cast<int>(rng() % static_cast<uint32_t>(paras));
            const int len = static_cast<int>(texts[e.para].size());
            e.cp = static_cast<int>(rng() % static_cast<uint32_t>(len + 1));
            const int op = static_cast<int>(rng() % 4);
            if (op == 0) {
                e.kind = rev::Edit::Kind::InsertText;
                e.text = std::u32string(1, static_cast<char32_t>('a' + rng() % 26));
            } else if (op == 1) {
                if (len == 0) continue;
                e.kind = rev::Edit::Kind::EraseText;
                e.count = 1 + static_cast<int>(
                                  rng() % static_cast<uint32_t>(len - e.cp > 0 ? len - e.cp : 1));
            } else if (op == 2) {
                e.kind = rev::Edit::Kind::SplitPara;
            } else {
                e.kind = rev::Edit::Kind::MergePara;
            }

            const rev::EditResult d = directOf(direct, e);
            const rev::Applied a = s.apply(e);
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
        rev::Edit ins;
        ins.kind = rev::Edit::Kind::InsertText;
        ins.para = 0;
        ins.cp = 0;
        ins.text = U"Z";
        expectTrue(s.apply(ins).ok, "撤销之后再编辑");
        expectTrue(s.redoDepth() == 0 && !s.canRedo(), "编辑之后重做栈清空");
        expectTrue(s.generation() > gen_before, "状态代号推进");

        // 被拒绝/没变化的一步：状态与历史都不动。
        //   这两条另起一个没吃满上限的会话：封顶之后深度怎么变都是同一个数，断言在那里
        //   恒真（注入"空编辑也入栈"的错，它照样绿）。
        rev::Session fresh(initial, 10);
        rev::Edit seed;
        seed.kind = rev::Edit::Kind::InsertText;
        seed.para = 0;
        seed.cp = 0;
        seed.text = U"seed";
        expectTrue(fresh.apply(seed).ok, "准备：新会话里先走一步");
        const Doc before_bad = fresh.doc();
        const int depth_before = fresh.undoDepth();

        rev::Edit bad;
        bad.kind = rev::Edit::Kind::MergePara;
        bad.para = rev::paraCount(fresh.doc()) - 1;  // 最后一段没有下一段
        const rev::Applied refused = fresh.apply(bad);
        expectTrue(!refused.ok && !refused.error.empty(), "末段合并：被拒绝并说明原因");
        expectTrue(fresh.doc().lo == before_bad.lo && fresh.doc().hi == before_bad.hi &&
                       fresh.undoDepth() == depth_before,
                   "被拒绝的一步不占撤销栈，当前修订不动");

        rev::Edit noop;
        noop.kind = rev::Edit::Kind::EraseText;
        noop.para = 0;
        noop.cp = 0;
        noop.count = 0;
        const rev::Applied nothing = fresh.apply(noop);
        expectTrue(nothing.ok && !nothing.changed, "删 0 个码点：成功但没有变化");
        expectTrue(fresh.undoDepth() == depth_before,
                   "没有变化的一步不占撤销栈（深度 " + std::to_string(fresh.undoDepth()) +
                       "，应为 " + std::to_string(depth_before) + "）");

        // 上限：超出时丢最旧的一份，撤销仍然一步步往回走。
        rev::Session small(initial, 3);
        std::vector<Doc> seen{initial};
        for (int i = 0; i < 6; ++i) {
            rev::Edit e;
            e.kind = rev::Edit::Kind::InsertText;
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
        rev::Session none(initial, 0);
        rev::Edit one;
        one.kind = rev::Edit::Kind::InsertText;
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
        auto buildTree = [](SectionBreak kind, const Length& left_margin) {
            NodeTree t;
            const NodeId s0 = t.firstSectionId();
            t.setText(t.appendChild(s0, NodeKind::TextRun), cpsOf(U"第一节"));
            t.appendChild(s0, NodeKind::ParagraphMark);
            const NodeId s1 = t.appendChild(t.rootId(), NodeKind::Section);
            t.find(s1)->section.break_kind = kind;
            if (left_margin != Length{}) t.find(s1)->section.margins.left = left_margin;
            t.setText(t.appendChild(s1, NodeKind::TextRun), cpsOf(U"第二节"));
            t.appendChild(s1, NodeKind::ParagraphMark);
            return t;
        };

        NodeTree tree = buildTree(SectionBreak::NextPage, Length{});
        Doc doc;
        std::string why;
        expectTrue(rev::fromTree(tree, &doc, &why), "两节文档导出成修订");
        expectTrue(rev::paraCount(doc) == 2, "两段（每节一段）");

        const rev::Projection pr = rev::project(doc, sheet);
        const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
        expectTrue(pr.ok() && cr.ok(), "两条路都没有诊断");
        expectTrue(pr.blocks.size() == 2 && cr.blocks.size() == 2,
                   "两块（投影 " + std::to_string(pr.blocks.size()) + " / 编译 " +
                       std::to_string(cr.blocks.size()) + "）");
        bool same = pr.blocks.size() == cr.blocks.size();
        for (size_t i = 0; same && i < cr.blocks.size(); ++i) {
            same = sameBlock(pr.blocks[i], cr.blocks[i]);
        }
        expectTrue(same, "每一块与编译产物逐字段相同");
        expectTrue(pr.blocks.size() == 2 && !pr.blocks[0].page_break_before &&
                       pr.blocks[1].page_break_before,
                   "分节分页落在第二节的第一段上（不是每一段）");

        // 连续分节：不该有段前分页。这一条证明上面那一位确实来自节的记录。
        NodeTree cont_tree = buildTree(SectionBreak::Continuous, Length{});
        Doc cont_doc;
        expectTrue(rev::fromTree(cont_tree, &cont_doc, &why), "连续分节版本导出");
        const rev::Projection cont_pr = rev::project(cont_doc, sheet);
        const CompileResult cont_cr = compileDocument(cont_tree, FlowVisibility::Design, sheet);
        bool cont_same = cont_pr.blocks.size() == cont_cr.blocks.size();
        for (size_t i = 0; cont_same && i < cont_cr.blocks.size(); ++i) {
            cont_same = sameBlock(cont_pr.blocks[i], cont_cr.blocks[i]);
        }
        expectTrue(cont_same && cont_pr.blocks.size() == 2 && !cont_pr.blocks[1].page_break_before,
                   "连续分节：没有段前分页，两条路仍相同");

        // 跨节编辑：段号在节之间连续，第 1 段落在第二节里。
        const auto typed = rev::insertText(doc, 1, 0, U"X");
        expectTrue(typed.ok && (rev::paraTexts(typed.doc) ==
                                std::vector<std::u32string>{U"第一节", U"X第二节"}),
                   "第 1 段落在第二节里（跨节编辑按段号走）");
        expectTrue(rev::damage(doc, typed.doc, FlowVisibility::Design).range.lo == 1,
                   "损伤定位落在第二块");

        // 页面设置：进内容哈希（改页边距就是改这份文档），但投影不消费它 —— 块哈希不变。
        NodeTree moved = buildTree(SectionBreak::NextPage, Length::abs(ptToEmu(3.0)));
        Doc moved_doc;
        expectTrue(rev::fromTree(moved, &moved_doc, &why), "改页边距之后导出");
        expectTrue(moved_doc.lo != doc.lo || moved_doc.hi != doc.hi,
                   "改页边距改了全文哈希（页面设置是内容）");
        expectTrue(rev::blockHashes(moved_doc, FlowVisibility::Design) ==
                       rev::blockHashes(doc, FlowVisibility::Design),
                   "块哈希一位不动（页边距不改变块的内容，排版由页度量参数决定）");

        // 落盘：节记录逐字段读回来。
        const auto ser = rev::serialize(moved_doc);
        Doc back;
        const auto de = ser.ok ? rev::deserialize(ser.text, &back) : rev::DeserializeResult{};
        expectTrue(ser.ok && de.ok && back.lo == moved_doc.lo && back.hi == moved_doc.hi,
                   "两节文档往返之后全文哈希相同");
        const auto sec_of = [](const Doc& d, size_t i) -> const rev::Node* {
            return (d.root && d.root->children.size() > i) ? d.root->children[i].get() : nullptr;
        };
        const rev::Node* a = sec_of(moved_doc, 1);
        const rev::Node* b = sec_of(back, 1);
        expectTrue(a != nullptr && b != nullptr && a->kind == NodeKind::Section &&
                       std::get<SectionSpec>(b->payload).sameAs(std::get<SectionSpec>(a->payload)),
                   "第二节的页面设置逐字段读回来（分页行为、页边距）");
    }

    std::printf("\n[20] 步骤应用：同一批步骤，模型层与修订层得到同一份文档\n");
    {
        const StyleSheet sheet;
        NodeTree model = makeSpanningControlTree();
        UndoStack stack(model);
        Doc doc;
        std::string why;
        expectTrue(rev::fromTree(model, &doc, &why), "基础文档导出成修订");

        int path_mismatch = 0;
        int batch_index = 0;
        auto applyBoth = [&](const std::vector<Step>& steps) {
            if (steps.empty()) return;
            ++batch_index;
            Transaction tx;
            tx.steps = steps;
            const StepResult a = stack.commit(tx);
            const rev::EditResult b = rev::applySteps(doc, steps, FlowVisibility::Design);
            if (a.ok != b.ok) {
                ++path_mismatch;
                std::printf("      第 %d 批：模型 %s（%s）/ 修订 %s（%s）\n", batch_index,
                            a.ok ? "成功" : "失败", a.error.c_str(), b.ok ? "成功" : "失败",
                            b.error.c_str());
            }
            if (!a.ok || !b.ok) return;
            doc = b.doc;
            Doc mirror;
            if (!rev::fromTree(model, &mirror, &why)) {
                ++path_mismatch;
                return;
            }
            if (!sameText(rev::paraTexts(mirror), rev::paraTexts(doc))) {
                ++path_mismatch;
                std::printf("      第 %d 批：文本 模型 %s / 修订 %s\n", batch_index,
                            dump(rev::paraTexts(mirror)).c_str(),
                            dump(rev::paraTexts(doc)).c_str());
            }
        };

        // 段首、段中、文末各插一个字再删掉：三种落点覆盖 run 内部与容器边界。
        for (int round = 0; round < 3; ++round) {
            const FlowIndex idx(model, FlowVisibility::Design);
            const int at = round == 0 ? 0 : (round == 1 ? 1 : idx.size());
            InsertText ins;
            ins.at = at;
            ins.cps = {static_cast<uint32_t>(U'a') + static_cast<uint32_t>(round)};
            applyBoth({Step{ins}});
            applyBoth(makeDeleteTextSteps(model, at, at + 1));
        }

        // 分段再并段：标记插入与移除。坐标取文档中部，避开控件边界。
        int split_at = -1;
        {
            const FlowIndex idx(model, FlowVisibility::Design);
            split_at = idx.size() / 2;
            InsertParaMark ins;
            ins.at = split_at;
            applyBoth({Step{ins}});
        }
        {
            const ParaMarkBuild rm = makeRemoveParaMark(model, split_at);
            if (rm.ok) applyBoth({rm.step});
        }

        // 改第一枚段落标记的格式：按节点号寻址，不经流位置。
        {
            NodeId mark = kInvalidNodeId;
            for (int i = 0;
                 i < model.find(model.rootId())->children.size() && mark == kInvalidNodeId; ++i) {
                const Node* sec = model.find(model.rootId())->children[static_cast<size_t>(i)];
                for (const Node* c : sec->children) {
                    if (isParagraphMark(c->kind)) {
                        mark = c->id;
                        break;
                    }
                }
            }
            if (mark != kInvalidNodeId) {
                SetParaMarkFormat s;
                s.id = mark;
                s.para.line_factor = 1.5f;
                s.styleRef = "quote";
                applyBoth({Step{s}});
            }
        }

        expectTrue(path_mismatch == 0, "每一批步骤两条路都成功，且段落文本逐步相同");

        Doc mirror;
        expectTrue(rev::fromTree(model, &mirror, &why), "最终文档再导出一次");
        const rev::Projection a = rev::project(mirror, sheet);
        const rev::Projection b = rev::project(doc, sheet);
        bool same_blocks = a.blocks.size() == b.blocks.size();
        for (size_t i = 0; same_blocks && i < a.blocks.size(); ++i) {
            same_blocks = sameBlock(a.blocks[i], b.blocks[i]);
        }
        expectTrue(same_blocks, "最终产物逐块逐字段相同（含跨段控件的两段）");
        expectTrue(a.tables.size() == b.tables.size(), "表格结构数量相同");
        expectTrue(rev::paraCount(doc) == rev::paraCount(mirror), "段落数相同");

        // 反向一半：一条越界步骤必须被拒，且不返回"看起来没变"的文档。
        const rev::EditResult bad =
            rev::applySteps(doc, {Step{RemoveParaMark{9999, {}, {}}}}, FlowVisibility::Design);
        expectTrue(!bad.ok && !bad.doc.root, "越界步骤被拒，且不带回半途的文档");
    }

    std::printf("\n[21] 演示文书：投影的段起点与编译一致（引擎的块流全靠这一项）\n");
    {
        const StyleSheet sheet;
        auto check = [&](const NodeTree& tree, const char* label) {
            Doc doc;
            std::string why;
            const bool exported = rev::fromTree(tree, &doc, &why);
            expectTrue(exported, std::string(label) + "：导出成修订");
            if (!exported) return;
            const rev::Projection pr = rev::project(doc, sheet);
            const CompileResult cr = compileDocument(tree, FlowVisibility::Design, sheet);
            bool same_blocks = pr.blocks.size() == cr.blocks.size();
            for (size_t i = 0; same_blocks && i < cr.blocks.size(); ++i) {
                same_blocks = testcmp::sameBlock(pr.blocks[i], cr.blocks[i]);
            }
            expectTrue(same_blocks, std::string(label) + "：块逐字段相同");

            const rev::FlowIndex ri(doc, FlowVisibility::Design);
            const FlowIndex mi(tree, FlowVisibility::Design);
            int bad = 0;
            for (size_t i = 0; i < cr.sources.size() && i < pr.blocks.size(); ++i) {
                int r_first = -1;
                int m_first = -1;
                if (!ri.intervalOf(pr.keys[i].source.first, &r_first, nullptr)) ++bad;
                if (!mi.intervalOf(cr.sources[i].first, &m_first, nullptr)) ++bad;
                if (r_first != m_first) ++bad;
                int r_mark = -1;
                int m_mark = -1;
                if (!ri.intervalOf(pr.keys[i].source.mark, &r_mark, nullptr)) ++bad;
                if (!mi.intervalOf(cr.sources[i].mark, &m_mark, nullptr)) ++bad;
                if (r_mark != m_mark) ++bad;
                if (bad != 0) {
                    std::printf("      第 %zu 段：投影 first=%d mark=%d / 编译 first=%d mark=%d\n",
                                i, r_first, r_mark, m_first, m_mark);
                    break;
                }
            }
            expectTrue(bad == 0, std::string(label) + "：每段的 first / mark 流位置一致");
        };
        check(demoTree(sampleDocument(0, 0), {}), "样本文书");
        check(demoTableTree(0), "表格文书");
    }

    std::printf("\n[22] 增量投影：没改到的块整块搬，产物与全量逐字段相同\n");
    {
        const StyleSheet sheet;
        const Doc base = rev::makeTextDoc({U"甲甲", U"乙乙", U"丙丙", U"丁丁"});
        // 块流由调用方持有：project 就地更新它，所以每次比较都拿"块流 + 身份账"两样。
        std::vector<BlockSpec> b0;
        const rev::Projection p0 = rev::project(base, sheet, {}, &b0);
        const rev::BlockHistory h0{p0.style_version, p0.keys};
        expectTrue(b0.size() == 4 && p0.reused == 0, "基线四块，且没有上一轮可复用");

        // 逐块比：内容、哈希、来源身份三样都要相同。只比内容看不出"块搬对了但身份错位"
        //   —— 那一种的症状是光标落偏一个字，而内容断言全绿。
        auto sameTo = [](const std::vector<BlockSpec>& a, const std::vector<rev::BlockKey>& ka,
                         const std::vector<BlockSpec>& b, const std::vector<rev::BlockKey>& kb) {
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
        const auto r = rev::insertText(base, 1, 1, U"X");
        const rev::Damage dmg1 = rev::damage(base, r.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c1 = b0;
        const rev::Projection inc1 = rev::project(r.doc, sheet, {}, &c1, &h0, &dmg1.range);
        std::vector<BlockSpec> b1;
        const rev::Projection p1 = rev::project(r.doc, sheet, {}, &b1);
        const rev::BlockHistory h1{p1.style_version, p1.keys};
        expectTrue(inc1.reused == 3, "改一段之后另外三块整块搬来");
        expectTrue(sameTo(c1, inc1.keys, b1, p1.keys),
                   "增量产物与全量逐字段相同（含来源身份）" + firstDiff(c1, b1));

        // 段首插一段：块数变了，而其余各段的身份没变，所以只有新段要算。
        const auto sp = rev::splitPara(r.doc, 0, 0);
        const rev::Damage dmg2 = rev::damage(r.doc, sp.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c2 = b1;
        const rev::Projection inc2 = rev::project(sp.doc, sheet, {}, &c2, &h1, &dmg2.range);
        std::vector<BlockSpec> b2;
        const rev::Projection p2 = rev::project(sp.doc, sheet, {}, &b2);
        const rev::BlockHistory h2{p2.style_version, p2.keys};
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
            const auto sm = rev::splitPara(base, 0, cut);
            if (!sm.ok) continue;
            const rev::Damage dm = rev::damage(base, sm.doc, FlowVisibility::Design);
            std::vector<BlockSpec> cm = b0;
            const rev::Projection incm = rev::project(sm.doc, sheet, {}, &cm, &h0, &dm.range);
            std::vector<BlockSpec> bm;
            const rev::Projection pm = rev::project(sm.doc, sheet, {}, &bm);
            const std::string tag = "段中分段(第 0 段第 " + std::to_string(cut) + " 位)";
            expectTrue(cm.size() == bm.size(), tag + "：块数相同（增量 " +
                                                   std::to_string(cm.size()) + " / 全量 " +
                                                   std::to_string(bm.size()) + "）");
            expectTrue(sameTo(cm, incm.keys, bm, pm.keys),
                       tag + "：增量与全量逐字段相同 " + firstDiff(cm, bm));
        }

        // 反向一半：删掉一段。被删的那一块不能"复活"，其余照旧复用。
        const auto mg = rev::mergePara(sp.doc, 0);
        const rev::Damage dmg3 = rev::damage(sp.doc, mg.doc, FlowVisibility::Design);
        std::vector<BlockSpec> c3 = b2;
        const rev::Projection inc3 = rev::project(mg.doc, sheet, {}, &c3, &h2, &dmg3.range);
        std::vector<BlockSpec> b3;
        const rev::Projection p3 = rev::project(mg.doc, sheet, {}, &b3);
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
        const rev::Projection inc4 = rev::project(r.doc, styled, {}, &c4, &h0, &dmg1.range);
        std::vector<BlockSpec> b4;
        const rev::Projection p4 = rev::project(r.doc, styled, {}, &b4);
        expectTrue(inc4.reused == 0, "样式表版本变了：一块都不复用（否则搬来的是旧格式）");
        expectTrue(sameTo(c4, inc4.keys, b4, p4.keys),
                   "样式表版本变了之后产物仍与全量相同" + firstDiff(c4, b4));
        expectTrue(!testcmp::sameBlock(b4[0], b1[0]),
                   "这条判据的前提成立：换默认格式确实改变了产物");

        // 增量必须真的省。同一份文档、同一次测量里比"全量"与"增量"，比值不受机器快慢影响。
        {
            std::vector<std::u32string> many(2000, U"一段用于测量的文字，内容长度与真实文书接近。");
            const Doc big = rev::makeTextDoc(many);
            const auto big_edit = rev::insertText(big, 1000, 3, U"X");
            std::vector<BlockSpec> big_blocks;
            const rev::Projection big_full = rev::project(big, sheet, {}, &big_blocks);
            const rev::BlockHistory big_hist{big_full.style_version, big_full.keys};
            const rev::Damage dmg_big = rev::damage(big, big_edit.doc, FlowVisibility::Design);
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<BlockSpec> a_blocks;
            const rev::Projection a = rev::project(big_edit.doc, sheet, {}, &a_blocks);
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<BlockSpec> cb = big_blocks;
            const rev::Projection b =
                rev::project(big_edit.doc, sheet, {}, &cb, &big_hist, &dmg_big.range);
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
                return rev::blockHashes(d, FlowVisibility::Design);
            };
            auto checkShape = [&](const Doc& start, const char* label) {
                Doc cur = start;
                int bad_count = 0;
                int bad_cover = 0;
                for (int k = 0; k < 12; ++k) {
                    const Doc before = cur;
                    switch (k % 4) {
                        case 0: {
                            const auto r2 = rev::insertText(cur, 0, 0, U"Z");
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        case 1: {
                            const auto r2 = rev::splitPara(cur, 0, 0);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        case 2: {
                            const auto r2 = rev::eraseText(cur, 0, 0, 1);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                        default: {
                            const auto r2 = rev::mergePara(cur, 0);
                            if (r2.ok) cur = r2.doc;
                            break;
                        }
                    }
                    const rev::Damage d = rev::damage(before, cur, FlowVisibility::Design);
                    std::vector<BlockSpec> tmp;
                    rev::project(cur, sheet, {}, &tmp);
                    if (d.range.after_blocks != static_cast<int>(tmp.size())) ++bad_count;
                    const rev::BlockDamage h = rev::diffBlocks(ledgerOf(before), ledgerOf(cur));
                    const bool covered =
                        h.empty() ? !d.range.changed
                                  : (d.range.changed && d.range.lo <= h.lo && d.range.hi >= h.hi);
                    if (!covered) ++bad_cover;
                }
                expectTrue(bad_count == 0, std::string(label) + "：damage 的块数与投影逐例相等");
                expectTrue(bad_cover == 0, std::string(label) + "：damage 的区间不漏报");
            };
            checkShape(rev::makeTextDoc({U"甲甲", U"乙乙", U"丙丙", U"丁丁", U"戊戊"}), "五段文本");
            checkShape(rev::makeTextDoc({U"单段"}), "单段");
            checkShape(rev::makeTextDoc({}), "空文档");
            // 末尾没有结束标记：投影会给它补一块，而 paras 不计 —— 坐标最容易差 1 的形状。
            {
                NodeTree open;
                const NodeId osec = open.firstSectionId();
                open.setText(open.appendChild(osec, NodeKind::TextRun),
                             utf8ToCodepoints("没有结束标记"));
                Doc open_doc;
                std::string open_why;
                expectTrue(rev::fromTree(open, &open_doc, &open_why), "末尾开放的树导出成功");
                std::vector<BlockSpec> ob;
                rev::project(open_doc, sheet, {}, &ob);
                expectTrue(ob.size() == 1, "末尾开放：投影补出最后那一块（paras 不计它）");
                checkShape(open_doc, "末尾开放");
            }
            {
                Doc tbl_doc;
                std::string tbl_why;
                expectTrue(rev::fromTree(demoTableTree(0), &tbl_doc, &tbl_why), "表格文书导出成功");
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
        Doc d = rev::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        rev::EditResult r = rev::insertControl(d, 0, 2, ControlSpec{}, U"XX", &cc);
        expectTrue(r.ok, "可见性：控件插入成功");
        d = r.doc;
        r = rev::setHidden(d, cc, true);
        expectTrue(r.ok, "可见性：控件隐藏成功");
        d = r.doc;

        const rev::FlowIndex dis(d, FlowVisibility::Display);
        const rev::FlowIndex des(d, FlowVisibility::Design);
        expectTrue(dis.size() == 7 && des.size() == 11,
                   "两种可见性的流位数不同（呈现态 7、设计态 11）");

        Step st = InsertText{};
        std::get<InsertText>(st).at = 4;  // 呈现态：两个 B 之间
        std::get<InsertText>(st).cps = {static_cast<uint32_t>(U'Z')};
        const rev::EditResult shown = rev::applySteps(d, {st}, FlowVisibility::Display);
        expectTrue(shown.ok && rev::paraTexts(shown.doc)[0] == U"AAXXBZB",
                   "★ 呈现态位置 4 落在 BB 里（不是落进隐藏控件）");
        // 反向：同一个整数按设计态读，落点就变成隐藏控件内部 —— 字会"打进去但看不见"。
        //   这一条钉的是"口径必须随坐标一起传"，不是"某个实现细节"。
        const rev::EditResult lost = rev::applySteps(d, {st}, FlowVisibility::Design);
        std::string lost_text = lost.ok ? std::string() : ("拒:" + lost.error);
        if (lost.ok) {
            const std::u32string t = rev::paraTexts(lost.doc)[0];
            lost_text.assign(t.begin(), t.end());
        }
        expectTrue(lost.ok && rev::paraTexts(lost.doc)[0] == U"AAZXXBB",
                   "反向：同一个整数按设计态读落进隐藏控件的内容起点（实际 " + lost_text + "）");
    }
    {
        // 损伤的块数：隐藏控件里含段落标记时，呈现态只有一块，设计态有两块。
        Doc d = rev::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        d = rev::insertControl(d, 0, 2, ControlSpec{}, U"XXYY", &cc).doc;
        d = rev::splitPara(d, 0, 4).doc;  // 段落标记落进控件内部
        d = rev::setHidden(d, cc, true).doc;

        const StyleSheet sheet;
        std::vector<BlockSpec> vis;
        std::vector<BlockSpec> des;
        rev::project(d, sheet, rev::ViewParams{FlowVisibility::Display}, &vis);
        rev::project(d, sheet, rev::ViewParams{FlowVisibility::Design}, &des);
        expectTrue(vis.size() == 1 && des.size() == 2,
                   "隐藏控件里的段落标记：呈现态 1 块、设计态 2 块");

        const Doc d1 = rev::insertText(d, 0, 0, U"Z").doc;
        const rev::Damage dm = rev::damage(d, d1, FlowVisibility::Display);
        expectTrue(dm.range.after_blocks == static_cast<int>(vis.size()),
                   "★ 呈现态的 damage.after_blocks 等于呈现态的块数");
        const rev::Damage dm_des = rev::damage(d, d1, FlowVisibility::Design);
        expectTrue(dm_des.range.after_blocks == static_cast<int>(des.size()),
                   "反向：设计态的 damage 仍按设计态数块");

        // 端到端：按这个损伤区间增量投影，块数必须与全量相同。
        //   after_blocks 报大了会把 shift 算成 +1，旧区间右端变 0 ⇒ 一块不删再插一块。
        std::vector<BlockSpec> live;
        rev::BlockHistory hist;
        {
            const rev::Projection p0 =
                rev::project(d, sheet, rev::ViewParams{FlowVisibility::Display}, &live);
            hist.keys = p0.keys;
            hist.style_version = p0.style_version;
        }
        const rev::Projection p1 = rev::project(d1, sheet, rev::ViewParams{FlowVisibility::Display},
                                                &live, &hist, &dm.range);
        expectTrue(live.size() == vis.size(), "★ 增量投影之后的块数与全量相同（不然块被搬重）");
        expectTrue(p1.reused + (dm.range.hi - dm.range.lo) >= 1, "增量真的发生了");
    }

    {
        // 可见性切换：隐藏/显示一个含段落标记的控件，损伤的块数要跟着换口径。
        //   这一对最容易错在"按设计态数了呈现态的块"—— 隐藏时多报一块，按区间搬块就会
        //   把后面那块往前挪错位。
        Doc d = rev::makeTextDoc({U"AABB"});
        NodeId cc = kInvalidNodeId;
        d = rev::insertControl(d, 0, 2, ControlSpec{}, U"XXYY", &cc).doc;
        d = rev::splitPara(d, 0, 4).doc;
        const Doc shown = d;
        const Doc hidden = rev::setHidden(shown, cc, true).doc;

        const StyleSheet sheet;
        std::vector<BlockSpec> b_shown;
        std::vector<BlockSpec> b_hidden;
        rev::project(shown, sheet, rev::ViewParams{FlowVisibility::Display}, &b_shown);
        rev::project(hidden, sheet, rev::ViewParams{FlowVisibility::Display}, &b_hidden);
        expectTrue(b_shown.size() == 2 && b_hidden.size() == 1, "隐藏前后：呈现态 2 块 → 1 块");

        const rev::Damage to_hide = rev::damage(shown, hidden, FlowVisibility::Display);
        expectTrue(to_hide.range.after_blocks == static_cast<int>(b_hidden.size()),
                   "隐藏：after_blocks 跟着呈现态少一块（实测 " +
                       std::to_string(to_hide.range.after_blocks) + "）");
        const rev::Damage to_show = rev::damage(hidden, shown, FlowVisibility::Display);
        expectTrue(to_show.range.after_blocks == static_cast<int>(b_shown.size()),
                   "显示：after_blocks 跟着呈现态多一块（实测 " +
                       std::to_string(to_show.range.after_blocks) + "）");
    }

    std::printf("\n结果：%d 项断言，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
