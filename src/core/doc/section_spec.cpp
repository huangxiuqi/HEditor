#include "core/doc/section_spec.h"

namespace heditor {

namespace {

/**
 * 内置纸张规格，单位毫米，纵向。B5 取 ISO 176x250（另有 JIS 182x257，本期只登记 ISO）。
 */
struct MmSize {
    double w;
    double h;
};

bool builtinSizeMm(PaperName name, MmSize* out) {
    switch (name) {
        case PaperName::A4:
            *out = {210.0, 297.0};
            return true;
        case PaperName::A5:
            *out = {148.0, 210.0};
            return true;
        case PaperName::B5:
            *out = {176.0, 250.0};
            return true;
        case PaperName::Letter:
            *out = {215.9, 279.4};
            return true;
        case PaperName::Legal:
            *out = {215.9, 355.6};
            return true;
        case PaperName::Custom:
            return false;
    }
    return false;
}

}  // namespace

const char* paperNameName(PaperName name) {
    switch (name) {
        case PaperName::A4:
            return "A4";
        case PaperName::A5:
            return "A5";
        case PaperName::B5:
            return "B5";
        case PaperName::Letter:
            return "Letter";
        case PaperName::Legal:
            return "Legal";
        case PaperName::Custom:
            return "Custom";
    }
    return "（未知）";
}

const char* orientationName(Orientation o) {
    switch (o) {
        case Orientation::Portrait:
            return "纵向";
        case Orientation::Landscape:
            return "横向";
    }
    return "（未知）";
}

const char* sectionBreakName(SectionBreak b) {
    switch (b) {
        case SectionBreak::Continuous:
            return "连续";
        case SectionBreak::NextPage:
            return "下一页";
        case SectionBreak::OddPage:
            return "奇数页";
        case SectionBreak::EvenPage:
            return "偶数页";
    }
    return "（未知）";
}

const char* verticalAlignName(VerticalAlign a) {
    switch (a) {
        case VerticalAlign::Top:
            return "上";
        case VerticalAlign::Center:
            return "居中";
        case VerticalAlign::Bottom:
            return "下";
        case VerticalAlign::Both:
            return "两端";
    }
    return "（未知）";
}

const char* pageNumberFormatName(PageNumberFormat f) {
    switch (f) {
        case PageNumberFormat::Arabic:
            return "阿拉伯";
        case PageNumberFormat::RomanUpper:
            return "大写罗马";
        case PageNumberFormat::RomanLower:
            return "小写罗马";
        case PageNumberFormat::Chinese:
            return "中文数字";
    }
    return "（未知）";
}

bool paperNameFromName(const std::string& text, PaperName* out) {
    if (out == nullptr) return false;
    const PaperName all[] = {PaperName::A4,     PaperName::A5,    PaperName::B5,
                             PaperName::Letter, PaperName::Legal, PaperName::Custom};
    for (PaperName p : all) {
        if (text == paperNameName(p)) {
            *out = p;
            return true;
        }
    }
    return false;
}

bool orientationFromName(const std::string& text, Orientation* out) {
    if (out == nullptr) return false;
    const Orientation all[] = {Orientation::Portrait, Orientation::Landscape};
    for (Orientation o : all) {
        if (text == orientationName(o)) {
            *out = o;
            return true;
        }
    }
    return false;
}

bool sectionBreakFromName(const std::string& text, SectionBreak* out) {
    if (out == nullptr) return false;
    const SectionBreak all[] = {SectionBreak::Continuous, SectionBreak::NextPage,
                                SectionBreak::OddPage, SectionBreak::EvenPage};
    for (SectionBreak b : all) {
        if (text == sectionBreakName(b)) {
            *out = b;
            return true;
        }
    }
    return false;
}

bool verticalAlignFromName(const std::string& text, VerticalAlign* out) {
    if (out == nullptr) return false;
    const VerticalAlign all[] = {VerticalAlign::Top, VerticalAlign::Center, VerticalAlign::Bottom,
                                 VerticalAlign::Both};
    for (VerticalAlign a : all) {
        if (text == verticalAlignName(a)) {
            *out = a;
            return true;
        }
    }
    return false;
}

bool pageNumberFormatFromName(const std::string& text, PageNumberFormat* out) {
    if (out == nullptr) return false;
    const PageNumberFormat all[] = {PageNumberFormat::Arabic, PageNumberFormat::RomanUpper,
                                    PageNumberFormat::RomanLower, PageNumberFormat::Chinese};
    for (PageNumberFormat f : all) {
        if (text == pageNumberFormatName(f)) {
            *out = f;
            return true;
        }
    }
    return false;
}

PaperSizeEmu paperSize(const Paper& paper) {
    PaperSizeEmu size;
    if (paper.name == PaperName::Custom) {
        // 自定义纸张的宽高必须是绝对长度：没有参照的长度推不出 EMU，返回零尺寸让调用方
        // 处理，而不是拿一个凭空的数值当纸张。
        if (paper.width.kind == LengthKind::Abs && paper.height.kind == LengthKind::Abs) {
            size.width = paper.width.abs_emu;
            size.height = paper.height.abs_emu;
        }
    } else {
        MmSize mm{};
        if (builtinSizeMm(paper.name, &mm)) {
            size.width = mmToEmu(mm.w);
            size.height = mmToEmu(mm.h);
        }
    }
    // 横向交换宽高：纸张的方向只影响摆放，不改规格本身，所以交换发生在派生这一步，
    // 而不写回 Paper。
    if (paper.orient == Orientation::Landscape) {
        const Emu w = size.width;
        size.width = size.height;
        size.height = w;
    }
    return size;
}

bool SectionSpec::sameAs(const SectionSpec& o) const {
    return paper == o.paper && margins == o.margins && gutter == o.gutter &&
           mirror_margins == o.mirror_margins && break_kind == o.break_kind && valign == o.valign &&
           writing_mode.inline_axis == o.writing_mode.inline_axis &&
           writing_mode.block_flow == o.writing_mode.block_flow &&
           writing_mode.orientation == o.writing_mode.orientation && numbering == o.numbering &&
           headers[0] == o.headers[0] && headers[1] == o.headers[1] && headers[2] == o.headers[2] &&
           footers[0] == o.footers[0] && footers[1] == o.footers[1] && footers[2] == o.footers[2] &&
           watermark == o.watermark && extra == o.extra;
}

}  // namespace heditor
