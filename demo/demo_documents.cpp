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

/** 追加一段普通文本（空区间不建节点 —— 空文本节点会被规范化并掉，不如一开始就不建）。 */
void appendTextRun(NodeTree& tree, NodeId parent, const std::vector<uint32_t>& cps, int from,
                   int to) {
    if (to <= from) return;
    const NodeId t = tree.appendChild(parent, NodeKind::TextRun);
    tree.setText(t, std::vector<uint32_t>(cps.begin() + from, cps.begin() + to));
}

/** 零宽字段：区间为空，它的开闭两枚落在同一处。 */
bool isEmptyField(const DemoFieldSpec* f) { return f->eo <= f->so; }

/**
 * 同一层里谁排在前面：起点最小者优先；起点相同时空字段在前（它紧贴"到此为止"的闭括号，
 * 要排在后一个字段的开括号之前）；再取终点最大者 —— 外层先开。
 */
bool fieldBefore(const DemoFieldSpec* a, const DemoFieldSpec* b) {
    if (a->so != b->so) return a->so < b->so;
    const bool a_empty = isEmptyField(a);
    const bool b_empty = isEmptyField(b);
    if (a_empty != b_empty) return a_empty;
    return a->eo > b->eo;
}

/**
 * 把 [from, to) 这段文本按 rest 里的字段切成"正文 + 内容控件"，嵌套的递归进去。
 *
 * 两条与老路径对齐的次序规则，都写在"取谁当前、谁进内层"上：
 *   其一，外层先开（起点小的先取）、同一处边界上闭括号先于开括号（见 fieldBefore）；
 *   其二，**空字段不参与嵌套** —— 它与"到此为止"的闭括号落在同一列，老路径把它那一对排在
 *     那枚闭括号之后、后一个字段的开括号之前，而一个包的住的字段的闭括号排在它自己内容的
 *     最后 —— 所以空字段只有与包住它的字段并列在当前这一层，才画得出同一个次序。
 *
 * 两个完全相同的区间不互相包含（它们是兄弟，见 rangeContainsForNesting），各自成对排列。
 * rest 按值收，每轮把这一枚连同递归吃掉的那些一起消掉：只消这一枚的话，内层字段会留在外层
 * 的候选里被再建一遍。
 */
void emitFields(NodeTree& tree, NodeId parent, const std::vector<uint32_t>& cps, int from, int to,
                std::vector<const DemoFieldSpec*> rest) {
    int pos = from;
    while (true) {
        size_t at = rest.size();
        for (size_t i = 0; i < rest.size(); ++i) {
            const DemoFieldSpec* f = rest[i];
            if (f->so < pos || f->eo > to) continue;
            if (at == rest.size() || fieldBefore(f, rest[at])) at = i;
        }
        if (at == rest.size()) break;
        const DemoFieldSpec* pick = rest[at];

        appendTextRun(tree, parent, cps, pos, pick->so);
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
            const bool same_span = (g->so == pick->so && g->eo == pick->eo);
            const bool covered = !isEmptyField(g) && !isEmptyField(pick) && !same_span &&
                                 g->so >= pick->so && g->eo <= pick->eo;
            if (covered) {
                inner.push_back(g);
            } else {
                remaining.push_back(g);
            }
        }
        emitFields(tree, cc, cps, pick->so, pick->eo, inner);
        rest.swap(remaining);
        if (pick->eo > pos) pos = pick->eo;
    }
    appendTextRun(tree, parent, cps, pos, to);
}

}  // namespace

NodeTree demoTree(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields) {
    NodeTree tree;
    const NodeId sec = tree.firstSectionId();
    for (size_t i = 0; i < blocks.size(); ++i) {
        const BlockSpec& b = blocks[i];
        std::vector<const DemoFieldSpec*> mine;
        for (const DemoFieldSpec& f : fields) {
            if (f.para == static_cast<int>(i)) mine.push_back(&f);
        }
        const int n = static_cast<int>(b.cp_text.size());
        if (mine.empty()) {
            appendTextRun(tree, sec, b.cp_text, 0, n);
        } else {
            emitFields(tree, sec, b.cp_text, 0, n, mine);
        }
        // 段落标记挂在控件之外（与控件并列、排在它后面）：标记落进控件内部等于让那个控件跨段，
        //   闭边界会落到下一段去。
        const NodeId mark = tree.appendChild(sec, NodeKind::ParagraphMark);
        applyBlockFormat(b, tree.find(mark));
    }
    return tree;
}

}  // namespace heditor
