/**
 * 判据夹具：块流 + 字段布局 → 修订。
 *
 * 这是"测试怎么造一份带格式的文档"的那一层，只被 tests/ 用（wasm 产物里没有它）。
 *   以前它在 demo/ 库里，与演示内容（病历、蒙文样例、场景、表单样例）混在一起，
 *   于是"给 wasm 用的入口"把样例正文一起拖进了产物。
 *
 * 全是 inline / 匿名命名空间：这份头被两个测试 TU 包含，不该产生重复符号。
 */
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/doc/control.h"
#include "core/doc/document.h"
#include "core/doc/para_format.h"
#include "core/layout/layout.h"
#include "core/utils/types.h"
#include "core/utils/units.h"
#include "core/utils/utf8.h"
#include "core/utils/writing_mode.h"

namespace heditor {
namespace fixtures {

// 前向声明：下面的样例文书互相调用，顺序不该成为约束（原文件靠头文件声明绕开了）。
inline BlockSpec cjkBlock(std::string text, int cjk_font_index, float size_pt, float line_factor);
inline BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index = -1);
inline std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                             int latin_font_index = -1);

/**
 * 一个字段怎么摆在树里：起点 (para, so) 与终点 (epara, eo) 之间的那一段文档流、业务种类
 * 与编码、是否只读。
 *
 * epara 省略（小于 0）即"同段"；大于 para 时这个字段跨段 —— 中间那些段落连同它们的结束
 * 标记都落在它里面，那是模型层允许的形状（控件可以跨段）。
 */
struct DemoFieldSpec {
    int para = 0;
    int so = 0;
    int epara = -1;  // 终点所在段；< 0 = 与起点同段
    int eo = 0;
    ControlKind kind = ControlKind::TextField;
    std::string key;
    bool readonly = false;
};

/**
 * 字段的终点段号：epara 没给就是起点那一段。
 * @param f 字段
 * @returns 终点段号
 */
inline int endParaOf(const DemoFieldSpec& f) { return f.epara < 0 ? f.para : f.epara; }

inline BlockSpec mixedParagraphBlock(int cjk_font_index, int latin_font_index) {
    BlockSpec b = cjkBlock(
        "体格检查：T 36.8℃，P 92 次/分，R 20 次/分，BP 138/86 mmHg。"
        "辅助检查：2026-10-01 血常规示 WBC 11.2×10⁹/L，N 78.5%，Hb 132 g/L，PLT 216×10⁹/L；"
        "心电图示 P-R 间期 0.12 s，QRS 波群时限 0.08 s，QTc 0.41 s；"
        "心脏彩超示 LVEF 62%，LA 38 mm，LVEDD 48 mm。"
        "诊断：1. Paroxysmal Supraventricular Tachycardia；2. Hypertension grade 2 (high risk)；"
        "3. Type 2 Diabetes Mellitus。"
        "处置：Amiodarone 150 mg iv 缓慢推注，继以 1 mg/min 静脉维持 6 h，后 0.5 mg/min 维持 18 h；"
        "Metoprolol 25 mg po bid；复查 ECG、电解质及甲状腺功能。"
        "医师签名：______________   日期：2026-10-09",
        cjk_font_index, 12.5f, 1.25f);
    // 拉丁字体挂回退：NotoSansSC-VF 有 ¹²³（U+00B9/B2/B3），
    //   但没有 ⁰⁹（U+2070/U+2079）——而"10⁹/L"是检验报告的标准写法。
    if (latin_font_index >= 0) b.fallback_font_indices.push_back(latin_font_index);
    return b;
}

inline BlockSpec cjkBlock(std::string text, int cjk_font_index, float size_pt, float line_factor) {
    BlockSpec b;
    b.font_index = cjk_font_index;
    b.size_pt = size_pt;
    b.wm = WritingMode::HorizontalTB();
    b.line_spacing.factor = line_factor;
    b.language = "zh";
    b.color = Color::rgb(24, 24, 24);
    b.setText(text);
    return b;
}
inline std::vector<BlockSpec> sampleDocument(int cjk_font_index, int mongol_font_index,
                                             int latin_font_index) {
    std::vector<BlockSpec> blocks;

    // 第 1 页：中文横排
    BlockSpec zh = cjkBlock(
        "患者主诉：反复心悸3天，加重1天。"
        "现病史：患者3天前无明显诱因出现心悸，伴胸闷、气短，活动后加重，休息后稍缓解，"
        "无黑矇、晕厥，无胸痛、咯血。既往史：高血压病史10年，规律服用氨氯地平5mg每日一次，"
        "血压控制在130/80mmHg左右；2型糖尿病病史6年，口服二甲双胍0.5g每日三次。"
        "体格检查：体温36.8℃，脉搏92次/分，呼吸20次/分，血压138/86mmHg。神志清楚，查体合作，"
        "双肺呼吸音清，未闻及干湿性啰音。心界不大，心率92次/分，律齐，"
        "各瓣膜听诊区未闻及病理性杂音。腹软，无压痛及反跳痛。双下肢无水肿。"
        "初步诊断：1. 心律失常 阵发性室上性心动过速；2. 高血压病2级（高危）；3. 2型糖尿病。"
        "医师签名：______________",
        cjk_font_index, 12.0f, 1.2f);
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
    mongo_lr.line_spacing.factor = 1.25f;
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
    zh_v.line_spacing.factor = 1.3f;
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
 * 块级字段 → 段落覆盖。
 * @param b 块
 * @returns 段落格式覆盖
 */
ParaFormatPatch blockPara(const BlockSpec& b) {
    ParaFormatPatch p;
    p.font_index = b.font_index;
    p.size = Length::abs(ptToEmu(b.size_pt));
    p.wm = b.wm;
    p.line_rule = b.line_spacing.rule;
    p.line_factor = b.line_spacing.factor;
    p.line_exact = Length::abs(ptToEmu(b.line_spacing.exact_pt));
    p.space_after = Length::abs(ptToEmu(b.space_after_pt));
    p.page_break_before = b.page_break_before;
    p.language = internLanguage(b.language != nullptr ? b.language : "");
    p.color = b.color;
    if (!b.fallback_font_indices.empty()) p.fallback_font_indices = b.fallback_font_indices;
    return p;
}

/**
 * doc 原生的演示文档构造器：与树版本同一套字段切分规则，但直接产出修订。
 */
struct DemoDocBuilder {
    int id = 3;

    /**
     * 一段窗口的文本 run 与行内原子。
     * @param b 块
     * @param from 起点码点
     * @param to 终点码点
     * @returns 节点序列
     */
    std::vector<doc::Ref> textRuns(const BlockSpec& b, int from, int to) {
        const std::vector<uint32_t>& cps = b.cp_text;
        const int len = static_cast<int>(cps.size());
        std::vector<const InlineObject*> atoms;
        for (const InlineObject& o : b.objects) {
            if (o.kind != InlineObjectKind::Atom) continue;
            const int cp = static_cast<int>(o.cp);
            if (cp < from) continue;
            if (cp >= to && !(cp == len && to == len)) continue;
            atoms.push_back(&o);
        }
        std::vector<doc::Ref> out;
        size_t next_atom = 0;
        auto flushAtomsUpTo = [&](int at) {
            while (next_atom < atoms.size() && static_cast<int>(atoms[next_atom]->cp) <= at) {
                const InlineObject& o = *atoms[next_atom++];
                if (o.atom == InlineAtom::Formula) {
                    FormulaSpec f;
                    f.expr = "\\frac{a}{b}";
                    out.push_back(doc::makeNode(id++, NodeKind::Formula, f, {}));
                } else {
                    ImageSpec img;
                    img.resource_id = "img1";
                    img.px_w = std::max(1, static_cast<int>(o.w_pt * 96.0f / 72.0f + 0.5f));
                    img.px_h = std::max(1, static_cast<int>(o.h_pt * 96.0f / 72.0f + 0.5f));
                    out.push_back(doc::makeNode(id++, NodeKind::Image, img, {}));
                }
            }
        };
        int pos = from;
        flushAtomsUpTo(pos);
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
            if (end <= pos) end = to;
            if (next_atom < atoms.size()) {
                end = std::min(end, static_cast<int>(atoms[next_atom]->cp));
            }
            if (end <= pos) {
                flushAtomsUpTo(pos);
                continue;
            }
            CharFormat marks;
            if (r != nullptr) {
                marks.font_index = r->font_index;
                marks.size = Length::abs(ptToEmu(r->size_pt));
                marks.color = r->color;
            }
            doc::RunPayload run;
            run.text.assign(cps.begin() + pos, cps.begin() + end);
            run.marks = marks;
            out.push_back(doc::makeNode(id++, NodeKind::TextRun, std::move(run), {}));
            pos = end;
            flushAtomsUpTo(pos);
        }
        return out;
    }

    /**
     * 平文本窗口，含中间的段落标记。
     * @param blocks 块流
     * @param pf 起点段
     * @param off_f 起点偏移
     * @param pt 终点段
     * @param off_t 终点偏移
     * @returns 节点序列
     */
    std::vector<doc::Ref> emitPlain(const std::vector<BlockSpec>& blocks, int pf, int off_f, int pt,
                                    int off_t) {
        std::vector<doc::Ref> out;
        for (int i = pf; i <= pt; ++i) {
            const BlockSpec& b = blocks[static_cast<size_t>(i)];
            const int len = static_cast<int>(b.cp_text.size());
            std::vector<doc::Ref> runs =
                textRuns(b, (i == pf) ? off_f : 0, (i == pt) ? off_t : len);
            out.insert(out.end(), runs.begin(), runs.end());
            if (i == pt) break;
            out.push_back(doc::makeNode(id++, NodeKind::ParagraphMark,
                                        doc::MarkPayload{blockPara(b), "", {}}, {}));
        }
        return out;
    }

    /**
     * 按字段切分窗口。
     * @param blocks 块流
     * @param pf 起点段
     * @param off_f 起点偏移
     * @param pt 终点段
     * @param off_t 终点偏移
     * @param rest 候选字段
     * @returns 节点序列
     */
    std::vector<doc::Ref> emitFields(const std::vector<BlockSpec>& blocks, int pf, int off_f,
                                     int pt, int off_t, std::vector<const DemoFieldSpec*> rest) {
        std::vector<doc::Ref> out;
        int cur_p = pf;
        int pos = off_f;
        while (true) {
            size_t at = rest.size();
            for (size_t i = 0; i < rest.size(); ++i) {
                const DemoFieldSpec* f = rest[i];
                if (f->para < cur_p || endParaOf(*f) > pt) continue;
                if (f->para == cur_p && f->so < pos) continue;
                if (endParaOf(*f) == pt && f->eo > off_t) continue;
                if (at == rest.size() || startsBefore(f, rest[at])) at = i;
            }
            if (at == rest.size()) break;
            const DemoFieldSpec* pick = rest[at];

            std::vector<doc::Ref> plain = emitPlain(blocks, cur_p, pos, pick->para, pick->so);
            out.insert(out.end(), plain.begin(), plain.end());

            std::vector<const DemoFieldSpec*> inner;
            std::vector<const DemoFieldSpec*> remaining;
            remaining.reserve(rest.size());
            for (size_t i = 0; i < rest.size(); ++i) {
                const DemoFieldSpec* g = rest[i];
                if (i == at) continue;
                const bool same_span = (g->para == pick->para && g->so == pick->so &&
                                        endParaOf(*g) == endParaOf(*pick) && g->eo == pick->eo);
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
            ControlSpec control;
            control.key = pick->key;
            control.controlType = businessTypeOf(pick->kind);
            control.readonly = pick->readonly;
            const std::vector<doc::Ref> kids =
                emitFields(blocks, pick->para, pick->so, endParaOf(*pick), pick->eo, inner);
            out.push_back(doc::makeNode(id++, NodeKind::ContentControl, control, kids));
            rest.swap(remaining);
            cur_p = endParaOf(*pick);
            pos = pick->eo;
        }
        std::vector<doc::Ref> tail = emitPlain(blocks, cur_p, pos, pt, off_t);
        out.insert(out.end(), tail.begin(), tail.end());
        return out;
    }
};

}  // namespace

/**
 * 把一份块流连同字段布局建成修订树。
 *
 * @param blocks 块流
 * @param fields 字段布局
 * @returns 修订
 */
doc::Doc demoDoc(const std::vector<BlockSpec>& blocks, const std::vector<DemoFieldSpec>& fields) {
    DemoDocBuilder builder;
    std::vector<const DemoFieldSpec*> all;
    if (!blocks.empty()) {
        for (const DemoFieldSpec& f : fields) {
            if (f.para < 0 || f.para >= static_cast<int>(blocks.size())) continue;
            if (endParaOf(f) < f.para || endParaOf(f) >= static_cast<int>(blocks.size())) continue;
            all.push_back(&f);
        }
    }
    std::vector<doc::Ref> kids;
    if (!blocks.empty()) {
        const int last = static_cast<int>(blocks.size()) - 1;
        const int last_len = static_cast<int>(blocks[static_cast<size_t>(last)].cp_text.size());
        kids = builder.emitFields(blocks, 0, 0, last, last_len, all);
        kids.push_back(doc::makeNode(
            builder.id++, NodeKind::ParagraphMark,
            doc::MarkPayload{blockPara(blocks[static_cast<size_t>(last)]), "", {}}, {}));
    }
    const doc::Ref section =
        doc::makeNode(builder.id++, NodeKind::Section, SectionSpec{}, std::move(kids));
    const NodeId root_id = builder.id++;
    return doc::adopt(doc::makeNode(root_id, NodeKind::Document, {}, {section}), builder.id);
}

}  // namespace fixtures
}  // namespace heditor
