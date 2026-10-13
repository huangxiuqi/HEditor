/**
 * 演示文书的实现。
 *
 * 内容改动的判据是"探针与单测认得出这份文档"：它们按页结构断言，
 * 所以改这里时要同步看 tests/ 与浏览器探针里依赖页序的断言。
 */
#include "demo/demo_documents.h"

#include <cstddef>

#include "core/text/writing_mode.h"
#include "core/utils/types.h"
#include "core/utils/units.h"

namespace heditor {

/** 样例文书：单测与探针共用的固定内容。 */

BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index) {
    BlockSpec b;
    b.font_index = cjk_font_index;
    b.size_pt = 12.5f;
    b.wm = WritingMode::HorizontalTB();
    b.line_factor = 1.8f;
    b.language = "zh";
    b.color = Color::rgb(24, 24, 24);
    // 拉丁字体挂回退：NotoSansSC-VF 有 ¹²³（U+00B9/B2/B3），
    //   但没有 ⁰⁹（U+2070/U+2079）——而"10⁹/L"是检验报告的标准写法。
    if (latin_font_index >= 0) b.fallback_font_indices.push_back(latin_font_index);
    b.setText(
        "体格检查：T 36.8℃，P 92 次/分，R 20 次/分，BP 138/86 mmHg。"
        "辅助检查：2026-10-01 血常规示 WBC 11.2×10⁹/L，N 78.5%，Hb 132 g/L，PLT 216×10⁹/L；"
        "心电图示 P-R 间期 0.12 s，QRS 波群时限 0.08 s，QTc 0.41 s；"
        "心脏彩超示 LVEF 62%，LA 38 mm，LVEDD 48 mm。"
        "诊断：1. Paroxysmal Supraventricular Tachycardia；2. Hypertension grade 2 (high risk)；"
        "3. Type 2 Diabetes Mellitus。"
        "处置：Amiodarone 150 mg iv 缓慢推注，继以 1 mg/min 静脉维持 6 h，后 0.5 mg/min 维持 18 h；"
        "Metoprolol 25 mg po bid；复查 ECG、电解质及甲状腺功能。"
        "医师签名：______________   日期：2026-10-09");
    return b;
}

std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                      int latin_font_index) {
    std::vector<BlockSpec> blocks;

    // 第 1 页：中文横排
    BlockSpec zh;
    zh.font_index = cjk_font_index;
    zh.size_pt = 12.0f;
    zh.wm = WritingMode::HorizontalTB();
    zh.line_factor = 1.75f;
    zh.language = "zh";
    zh.color = Color::rgb(24, 24, 24);
    zh.setText(
        "患者主诉：反复心悸3天，加重1天。"
        "现病史：患者3天前无明显诱因出现心悸，伴胸闷、气短，活动后加重，休息后稍缓解，"
        "无黑矇、晕厥，无胸痛、咯血。既往史：高血压病史10年，规律服用氨氯地平5mg每日一次，"
        "血压控制在130/80mmHg左右；2型糖尿病病史6年，口服二甲双胍0.5g每日三次。"
        "体格检查：体温36.8℃，脉搏92次/分，呼吸20次/分，血压138/86mmHg。神志清楚，查体合作，"
        "双肺呼吸音清，未闻及干湿性啰音。心界不大，心率92次/分，律齐，"
        "各瓣膜听诊区未闻及病理性杂音。腹软，无压痛及反跳痛。双下肢无水肿。"
        "初步诊断：1. 心律失常 阵发性室上性心动过速；2. 高血压病2级（高危）；3. 2型糖尿病。"
        "医师签名：______________");
    blocks.push_back(std::move(zh));

    // 第 2 页：蒙文竖排，列序从左往右（vertical-lr）
    std::string mn;
    for (int i = 0; i < 14; ++i) {
        if (i) mn += " ";
        // "\u182E\u1823\u1829\u182D\u1823\u182F" = "ᠮᠣᠩᠭᠣᠯ"（蒙古）
        // "\u182A\u1822\u1834\u1822\u182D"         = "ᠪᠢᠴᠢᠭ"（文字/script）
        mn += "\u182E\u1823\u1829\u182D\u1823\u182F \u182A\u1822\u1834\u1822\u182D";
    }
    BlockSpec mongo_lr;
    mongo_lr.font_index = mongol_font_index;
    mongo_lr.size_pt = 16.0f;
    mongo_lr.wm = WritingMode::VerticalLR();
    mongo_lr.line_factor = 1.8f;
    mongo_lr.language = "mn";
    mongo_lr.page_break_before = true;
    mongo_lr.color = Color::rgb(20, 60, 130);
    mongo_lr.setText(mn);
    blocks.push_back(std::move(mongo_lr));

    // 第 3 页：中文竖排（vertical-rl，列序从右往左，字形正立）
    //
    // 这才是 VerticalRL 的真实用例：中日竖排走 HarfBuzz 的 TTB + vert/vrt2 + vmtx，
    // 字形本身保持正立，列序从右往左 —— 与上一页蒙文"侧倒 + 列序从左往右"正好相反。
    // （把蒙文放进这条通道是错的：见 writing_mode.h 里 isSideways() 的说明。）
    BlockSpec zh_v;
    zh_v.font_index = cjk_font_index;
    zh_v.size_pt = 15.0f;
    zh_v.wm = WritingMode::VerticalRL();
    zh_v.line_factor = 1.9f;
    zh_v.language = "zh";
    zh_v.page_break_before = true;
    zh_v.color = Color::rgb(150, 60, 30);
    zh_v.setText(
        "患者主诉：反复心悸3天，加重1天。现病史：患者3天前无明显诱因出现心悸，"
        "伴胸闷、气短，活动后加重，休息后稍缓解。既往史：高血压病史10年。"
        "初步诊断：心律失常、阵发性室上性心动过速。");
    blocks.push_back(std::move(zh_v));

    // 第 4 页：中英文混排
    BlockSpec mixed = mixedParagraphBlock(cjk_font_index, latin_font_index);
    mixed.page_break_before = true;
    blocks.push_back(std::move(mixed));

    return blocks;
}

namespace {

/**
 * 业务种类 → 业务类型名。与引擎侧 controlKindFromBusinessType 那条映射互为逆，
 * 写反了的表现是"演示里那个数值栏被当成文本框"（落点判定一样，只有对外报的种类不同）。
 */
const char* businessTypeOf(ControlKind kind) {
    if (kind == ControlKind::NumberField) return "number";
    if (kind == ControlKind::DateField) return "date";
    return "text";
}

/**
 * 把块级字段搬到段落结束标记的段落格式上。
 *
 * 逐字段搬而不是"只搬与默认不同的那些"：块流里的值本来就是作者给的完整值，
 * 这里要的是"编译回来一字不差"，少搬一个字段就会静默换成默认值。
 */
void applyBlockFormat(const BlockSpec& b, Node* mark) {
    mark->para.font_index = b.font_index;
    mark->para.size = Length::abs(ptToEmu(b.size_pt));
    mark->para.wm = b.wm;
    mark->para.line_factor = b.line_factor;
    mark->para.space_after = Length::abs(ptToEmu(b.space_after_pt));
    mark->para.page_break_before = b.page_break_before;
    mark->para.language = internLanguage(b.language != nullptr ? b.language : "");
    mark->para.color = b.color;
    if (!b.fallback_font_indices.empty()) {
        mark->para.fallback_font_indices = b.fallback_font_indices;
    }
}

/**
 * 追加一段文本，按字符格式 run 切开：一段一个文本节点，格式落在它的标记上。
 *
 * 为什么要切：run 是作者意图，而编译正是从"标记 + 样式表"再解析出 run 的 —— 不切的话，
 * 块流里设了 runs 的文本搬进树就只剩一个节点，编译回来退化成"整段一个 run"，
 * 于是"按 run 取字号 / 颜色"那一整类判据在树这条路上全部失效。
 * 树里没有 run 级的语言与回退链，这两项只跟块级走（见 applyBlockFormat）。
 * runs 为空表示"整段一个 run"，属性取块级字段，与不切之前逐字段相同。
 */
void appendTextRuns(NodeTree& tree, NodeId parent, const BlockSpec& b, int from, int to) {
    const std::vector<uint32_t>& cps = b.cp_text;
    int pos = from;
    while (pos < to) {
        const FormatRun* r = nullptr;
        for (const FormatRun& cand : b.runs) {
            const int lo = static_cast<int>(cand.cp_begin);
            const int hi = static_cast<int>(cand.cp_end);
            if (lo <= pos && pos < hi) {
                r = &cand;
                break;
            }
        }
        int end = (r != nullptr) ? std::min(to, static_cast<int>(r->cp_end)) : to;
        if (end <= pos) end = to;  // runs 没铺满这一段时退回段末，不丢字
        const NodeId t = tree.appendChild(parent, NodeKind::TextRun);
        tree.setText(t, std::vector<uint32_t>(cps.begin() + pos, cps.begin() + end));
        if (r != nullptr) {
            Node* n = tree.find(t);
            n->marks.font_index = r->font_index;
            n->marks.size = Length::abs(ptToEmu(r->size_pt));
            n->marks.color = r->color;
        }
        pos = end;
    }
}

/** 零宽字段：两个端点落在同一处。 */
bool isEmptyField(const DemoFieldSpec* f) { return f->eo <= f->so; }

/** 终点谁更靠后：先比段号，再比段内偏移。 */
bool endsAfter(const DemoFieldSpec* a, const DemoFieldSpec* b) {
    const int ea = endParaOf(*a);
    const int eb = endParaOf(*b);
    if (ea != eb) return ea > eb;
    return a->eo > b->eo;
}

/**
 * 起点谁更靠前：先比段号；同段内起点最小者优先；起点相同时空字段在前（它紧贴"到此为止"
 * 的闭括号，要排在后一个字段的开括号之前）；再取终点更靠后者 —— 外层先开。
 */
bool startsBefore(const DemoFieldSpec* a, const DemoFieldSpec* b) {
    if (a->para != b->para) return a->para < b->para;
    if (a->so != b->so) return a->so < b->so;
    const bool a_empty = isEmptyField(a);
    const bool b_empty = isEmptyField(b);
    if (a_empty != b_empty) return a_empty;
    return endsAfter(a, b);
}

/**
 * 把 [(pf, off_f) .. (pt, off_t)] 这一段"平文本"建到 parent 下：段落内容 + 段落结束标记。
 *
 * 除最后一段之外，每段都要连它的结束标记一起吐出来 —— 标记是"这一段到此为止"的那一位，
 * 在流里就排在内容之后。最后一段的标记不由这里吐（调用方在它外面接着吐，或由 demoTree
 * 在最后补上）：那一位属于"最后一段的结束"，不该被任何字段的窗口截断。
 */
void emitPlain(NodeTree& tree, NodeId parent, const std::vector<BlockSpec>& blocks, int pf,
               int off_f, int pt, int off_t) {
    for (int i = pf; i <= pt; ++i) {
        const BlockSpec& b = blocks[static_cast<size_t>(i)];
        const int len = static_cast<int>(b.cp_text.size());
        appendTextRuns(tree, parent, b, (i == pf) ? off_f : 0, (i == pt) ? off_t : len);
        if (i == pt) break;
        const NodeId mark = tree.appendChild(parent, NodeKind::ParagraphMark);
        applyBlockFormat(b, tree.find(mark));
    }
}

/**
 * 把 [(pf, off_f) .. (pt, off_t)] 这一段文档流按 rest 里的字段切成"正文 + 内容控件"，
 * 嵌套的递归进去。窗口可以跨段：字段的起点与终点各自带段号。
 *
 * 三条次序规则（取谁当前、谁进内层）：
 *   一，外层先开（起点在前者先取）、同一处边界上闭括号先于开括号（见 startsBefore）；
 *   二，零宽字段贴在别的字段边界上时（端点重合）与它并列当前这一层 —— 它那一对排在
 *     "到此为止"那枚闭括号之后、后一个字段的开括号之前；
 *   三，零宽字段严格落在某字段内部时跟着递归进去，与区间树判父子同一口径。
 *
 * 两个完全相同的区间不互相包含（互为兄弟），各自成对排列。rest 按值收，每轮把这一枚
 * 连同递归吃掉的那些一起消掉：只消这一枚的话，内层字段会留在外层的候选里被再建一遍。
 */
void emitFields(NodeTree& tree, NodeId parent, const std::vector<BlockSpec>& blocks, int pf,
                int off_f, int pt, int off_t, std::vector<const DemoFieldSpec*> rest) {
    int cur_p = pf;
    int pos = off_f;
    while (true) {
        size_t at = rest.size();
        for (size_t i = 0; i < rest.size(); ++i) {
            const DemoFieldSpec* f = rest[i];
            // 候选 = 整个落在当前窗口里（比大小一律连带段号一起比，跨段字段才不会越界）。
            if (f->para < cur_p || endParaOf(*f) > pt) continue;
            if (f->para == cur_p && f->so < pos) continue;
            if (endParaOf(*f) == pt && f->eo > off_t) continue;
            if (at == rest.size() || startsBefore(f, rest[at])) at = i;
        }
        if (at == rest.size()) break;
        const DemoFieldSpec* pick = rest[at];

        emitPlain(tree, parent, blocks, cur_p, pos, pick->para, pick->so);
        const NodeId cc = tree.appendChild(parent, NodeKind::ContentControl);
        Node* cn = tree.find(cc);
        cn->control.key = pick->key;
        cn->control.controlType = businessTypeOf(pick->kind);
        cn->control.readonly = pick->readonly;

        std::vector<const DemoFieldSpec*> inner;
        std::vector<const DemoFieldSpec*> remaining;
        remaining.reserve(rest.size());
        for (size_t i = 0; i < rest.size(); ++i) {
            const DemoFieldSpec* g = rest[i];
            if (i == at) continue;
            const bool same_span = (g->para == pick->para && g->so == pick->so &&
                                    endParaOf(*g) == endParaOf(*pick) && g->eo == pick->eo);
            // 零宽字段按位置分两种（见本函数顶上第三条）：贴在边界上的与对方并列，
            //   严格落在内部的跟着递归进去 —— 后者的端点要真严格，贴边那一侧留给前一条。
            const bool inside = isEmptyField(g)
                                    ? (startsBefore(pick, g) && endsAfter(pick, g))
                                    : (!startsBefore(g, pick) && !endsAfter(g, pick));
            const bool covered = !isEmptyField(pick) && !same_span && inside;
            if (covered) {
                inner.push_back(g);
            } else {
                remaining.push_back(g);
            }
        }
        emitFields(tree, cc, blocks, pick->para, pick->so, endParaOf(*pick), pick->eo, inner);
        rest.swap(remaining);
        cur_p = endParaOf(*pick);
        pos = pick->eo;
    }
    emitPlain(tree, parent, blocks, cur_p, pos, pt, off_t);
}

}  // namespace

NodeTree demoTree(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    if (blocks.empty()) return tree;
    std::vector<const DemoFieldSpec*> all;
    all.reserve(fields.size());
    for (const DemoFieldSpec& f : fields) {
        // 段号越界的字段整个丢掉：宁可不摆，也不要拿它去索引别的段。
        if (f.para < 0 || f.para >= static_cast<int>(blocks.size())) continue;
        if (endParaOf(f) < f.para || endParaOf(f) >= static_cast<int>(blocks.size())) continue;
        all.push_back(&f);
    }
    const int last = static_cast<int>(blocks.size()) - 1;
    const int last_len = static_cast<int>(blocks[static_cast<size_t>(last)].cp_text.size());
    emitFields(tree, sec, blocks, 0, 0, last, last_len, all);
    // 末尾那一段的结束标记：它在流里排在所有内容之后，所以最后补。
    const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
    applyBlockFormat(blocks[static_cast<size_t>(last)], tree.find(mark));
    return tree;
}

}  // namespace heditor
