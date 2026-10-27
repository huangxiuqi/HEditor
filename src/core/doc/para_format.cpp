#include "core/doc/para_format.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace heditor {

const char* internLanguage(const std::string& tag) {
    if (tag.empty()) return nullptr;
    // 函数内的静态表：进程内唯一、只在首次使用时构造。条目永不释放（这正是"静态生存期"），
    // 所以 map 的键与 set 里的字符串都必须自己持有 —— 不能指向调用方的临时串。
    static std::mutex mu;
    static std::set<std::string> pool;
    static std::map<std::string, const char*> index;
    const std::lock_guard<std::mutex> lock(mu);
    const auto it = index.find(tag);
    if (it != index.end()) return it->second;
    const auto inserted = pool.insert(tag);
    const char* kept = inserted.first->c_str();
    index.emplace(tag, kept);
    return kept;
}

void applyParaFormatPatch(const ParaFormatPatch& patch, ParaFormat* base) {
    if (base == nullptr) return;
    if (patch.font_index.has_value()) base->font_index = *patch.font_index;
    if (patch.size.has_value()) base->size = *patch.size;
    if (patch.wm.has_value()) base->wm = *patch.wm;
    if (patch.line_factor.has_value()) base->line_factor = *patch.line_factor;
    if (patch.space_before.has_value()) base->space_before = *patch.space_before;
    if (patch.space_after.has_value()) base->space_after = *patch.space_after;
    if (patch.page_break_before.has_value()) base->page_break_before = *patch.page_break_before;
    if (patch.language.has_value()) base->language = *patch.language;
    if (patch.color.has_value()) base->color = *patch.color;
    if (patch.fallback_font_indices.has_value())
        base->fallback_font_indices = *patch.fallback_font_indices;
}

ParaFormatPatch mergeParaFormatPatch(const ParaFormatPatch& under, const ParaFormatPatch& over) {
    ParaFormatPatch out = under;
    if (over.font_index.has_value()) out.font_index = over.font_index;
    if (over.size.has_value()) out.size = over.size;
    if (over.wm.has_value()) out.wm = over.wm;
    if (over.line_factor.has_value()) out.line_factor = over.line_factor;
    if (over.space_before.has_value()) out.space_before = over.space_before;
    if (over.space_after.has_value()) out.space_after = over.space_after;
    if (over.page_break_before.has_value()) out.page_break_before = over.page_break_before;
    if (over.language.has_value()) out.language = over.language;
    if (over.color.has_value()) out.color = over.color;
    if (over.fallback_font_indices.has_value())
        out.fallback_font_indices = over.fallback_font_indices;
    return out;
}

}  // namespace heditor
