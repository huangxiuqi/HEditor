#include "core/doc/section_spec.h"

namespace heditor {

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
