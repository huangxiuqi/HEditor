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
#include "core/rev/project.h"
#include "core/rev/rev.h"
#include "core/rev/serialize.h"

using namespace heditor;
using heditor::rev::Doc;

namespace {

int g_checks = 0;
int g_failures = 0;

std::vector<uint32_t> cpsOf(const std::u32string& s) {
    std::vector<uint32_t> out;
    for (char32_t c : s) out.push_back(static_cast<uint32_t>(c));
    return out;
}

/**
 * 行内对象里 owner 是节点号（身份），两边各自从 1 起分配，号码不可比；其余字段表达内容。
 * 跨模型比对因此跳过 owner —— 但"它是不是指向同一个控件"不能就这么算了，由控件表那一组
 * 判据（块号、码点跨度、只读）单独管。
 */
bool sameObjects(const std::vector<InlineObject>& a, const std::vector<InlineObject>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].cp != b[i].cp || a[i].kind != b[i].kind || a[i].read_only != b[i].read_only) {
            return false;
        }
        if (a[i].w_pt != b[i].w_pt || a[i].h_pt != b[i].h_pt) return false;
        if (a[i].atom != b[i].atom || a[i].content_hash != b[i].content_hash) return false;
    }
    return true;
}

/**
 * 两块是不是同一份排版输入。除行内对象按上面那条口径外，其余字段交给 BlockSpec 的相等 ——
 * 它是逐字段写的，加字段漏比会编不过。
 */
bool sameBlock(const BlockSpec& a, const BlockSpec& b) {
    BlockSpec x = a;
    BlockSpec y = b;
    x.objects.clear();
    y.objects.clear();
    return x == y && sameObjects(a.objects, b.objects);
}

/**
 * 第一处不同的字段名，给差分判据的失败信息用。只说"不同"的断言没法查。
 */
std::string blockDiff(const BlockSpec& a, const BlockSpec& b) {
    if (a.cp_text != b.cp_text) return "cp_text";
    if (a.font_index != b.font_index) return "font_index";
    if (a.size_pt != b.size_pt) return "size_pt";
    if (!(a.wm == b.wm)) return "wm";
    if (a.line_factor != b.line_factor) return "line_factor";
    if (a.space_after_pt != b.space_after_pt) return "space_after_pt";
    if (a.page_break_before != b.page_break_before) return "page_break_before";
    if (!BlockSpec::sameLanguage(a.language, b.language)) return "language";
    if (!(a.color == b.color)) return "color";
    if (a.fallback_font_indices != b.fallback_font_indices) return "fallback_font_indices";
    if (a.cell_path != b.cell_path) return "cell_path";
    if (a.runs.size() != b.runs.size()) return "runs.size";
    for (size_t i = 0; i < a.runs.size(); ++i) {
        if (!(a.runs[i] == b.runs[i])) return "runs[" + std::to_string(i) + "]";
    }
    if (a.objects.size() != b.objects.size()) return "objects.size";
    for (size_t i = 0; i < a.objects.size(); ++i) {
        if (a.objects[i].cp != b.objects[i].cp) return "objects.cp";
        if (a.objects[i].kind != b.objects[i].kind) return "objects.kind";
        if (a.objects[i].read_only != b.objects[i].read_only) return "objects.read_only";
    }
    return "（只有 owner 不同）";
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
        const auto& objs = proj.blocks[0].spec.objects;
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
        expectTrue(design.blocks.size() == 1 && design.blocks[0].spec.cp_text.size() == 5 &&
                       design.blocks[0].spec.objects.size() == 2,
                   "编辑态：隐藏控件照常参与投影（作者要能选中它改回来）");
        expectTrue(display.blocks.size() == 1 && display.blocks[0].spec.cp_text.size() == 3 &&
                       display.blocks[0].spec.objects.empty() && display.controls.empty(),
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
            if (!sameBlock(pr.blocks[i].spec, cr.blocks[i])) {
                same_spec = false;
                std::printf("      第 %zu 块不同：%s\n", i,
                            blockDiff(pr.blocks[i].spec, cr.blocks[i]).c_str());
            }
        }
        expectTrue(same_spec, "逐块 BlockSpec 逐字段相同（文本、runs、行内对象、块级格式）");

        // 两边的节点号各自从 1 起分配，号码不可比；可比的是"它指向的那枚节点在各自模型里
        // 确实是一个段落标记"—— 来源身份指错地方时这条会红。
        bool source_ok = pr.blocks.size() == cr.sources.size();
        for (size_t i = 0; source_ok && i < pr.blocks.size(); ++i) {
            const rev::Node* rm = rev::findNode(rdoc, pr.blocks[i].source.mark);
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
            if (!sameBlock(pd.blocks[i].spec, cd.blocks[i])) {
                same_hidden = false;
                std::printf("      呈现态第 %zu 块不同：%s\n", i,
                            blockDiff(pd.blocks[i].spec, cd.blocks[i]).c_str());
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
        const rev::Damage none = rev::damage(same, same);
        expectTrue(!none.range.changed, "同一份修订：没有损伤");
        expectTrue(none.visited == 1, "同一份修订：只比较了根一个节点（整篇没变是 O(1)）");

        // 段内改字（块数不变）：与块哈希比对逐字段一致。
        const Doc d0 = rev::makeTextDoc(base);
        const Doc d1 = rev::insertText(d0, 2, 1, U"X").doc;
        const rev::Damage inplace = rev::damage(d0, d1);
        const rev::BlockDamage ref = rev::diffBlocks(rev::blockHashes(d0, FlowVisibility::Design),
                                                     rev::blockHashes(d1, FlowVisibility::Design));
        expectTrue(inplace.range.changed && ref.changed, "段内改字：两边都判为有损伤");
        expectTrue(inplace.range.lo == ref.lo && inplace.range.hi == ref.hi,
                   "段内改字：区间与块哈希比对完全一致（不许漏，也不许白报）");
        expectTrue(inplace.range.lo == 2 && inplace.range.hi == 3, "段内改字：正好是第 2 块");

        // 空编辑：没有变化，一步剪掉。
        const Doc noop = rev::insertText(d0, 0, 0, U"").doc;
        const rev::Damage dn = rev::damage(d0, noop);
        expectTrue(!dn.range.changed && dn.visited == 1, "空编辑：没有损伤，且只看了根节点");

        // 结构改动：分段多一块、合并少一块，两者都只动一块。
        const Doc sp = rev::splitPara(d0, 0, 0).doc;
        const rev::Damage ds = rev::damage(d0, sp);
        // cp=0 分段时模型会丢掉原 run、再建一个内容相同的新 run（新节点号），于是"删一个、
        //   插一个"被分别记成两块。多报的那一块内容与旧块完全相同，重排时命中块缓存 ——
        //   这里不为它加"按哈希跨位配对"的启发式：那类规则的价值只有一次缓存查询，
        //   而它的失效方式（把真变化的两个节点当成同一份内容）是静默的。区间上限钉在 2。
        expectTrue(ds.range.changed && ds.range.lo == 0 && ds.range.hi >= 1 && ds.range.hi <= 2,
                   "段首分段：从第 0 块起，最多两块（实际 [" + std::to_string(ds.range.lo) + "," +
                       std::to_string(ds.range.hi) + ")）");

        const Doc mg = rev::mergePara(d0, 1).doc;
        const rev::Damage dm = rev::damage(d0, mg);
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
        const rev::Damage de = rev::damage(d0, empty);
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
                const rev::Damage got = rev::damage(cur, r.doc);
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
            const rev::Damage m1 = rev::damage(a1, b1);
            const rev::Damage m2 = rev::damage(a2, b2);
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
            for (int k = 0; k < 200; ++k) sink += rev::damage(a1, b1).visited;
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
                if (!sameBlock(pr.blocks[i].spec, cr.blocks[i])) {
                    same = false;
                    std::printf("      第 %zu 块不同：%s\n", i,
                                blockDiff(pr.blocks[i].spec, cr.blocks[i]).c_str());
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
                if (!sameBlock(pr.blocks[i].spec, cr.blocks[i])) {
                    same = false;
                    std::printf("      跨段第 %zu 块不同：%s\n", i,
                                blockDiff(pr.blocks[i].spec, cr.blocks[i]).c_str());
                }
            }
            expectTrue(same, "跨段控件：投影与编译逐字段相同（含两块各自的行内对象）");
        }

        // 损伤：分段之后两块都要重排。
        const rev::Damage dm = rev::damage(inside, crossed);
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
        expectTrue(ser.ok && !ser.text.empty(), "序列化成功且非空");
        Doc back;
        const auto de = rev::deserialize(ser.text, &back);
        expectTrue(de.ok, de.ok ? std::string("反序列化成功") : ("反序列化失败：" + de.error));
        expectTrue(back.lo == doc.lo && back.hi == doc.hi,
                   "全文哈希相同（参与哈希的字段一个都没漏）");
        expectTrue(back.next_id == doc.next_id, "分配器水位相同");
        expectTrue(rev::paraCount(back) == rev::paraCount(doc), "段落数相同");
        expectTrue(rev::paraTexts(back) == rev::paraTexts(doc), "逐段文字相同");
        expectTrue(sameTree(doc.root, back.root), "节点身份、种类、隐藏标记、段落数、哈希逐个相同");
        expectTrue(rev::damage(doc, back).range.empty(), "两份修订之间没有损伤（没有块要重排）");

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
            rev::makeNode(2, NodeKind::Section, {},
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
            {rev::makeNode(2, NodeKind::Section, {},
                           {rev::makeNode(3, NodeKind::TextRun,
                                          rev::RunPayload{cpsOf(U"a"), CharFormat{}, {}}, {}),
                            rev::makeNode(3, NodeKind::TextRun,
                                          rev::RunPayload{cpsOf(U"b"), CharFormat{}, {}}, {})})});
        expectTrue(!rev::serialize(rev::adopt(dup, 9)).ok, "节点号重复的修订：写不出来");
    }

    std::printf("\n结果：%d 项断言，%d 项失败\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
