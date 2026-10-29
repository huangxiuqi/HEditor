#include "core/text/fallback.h"

#include <hb.h>

#include "core/text/font.h"
#include "core/utils/utf8.h"

namespace heditor {

bool needsOwnGlyph(uint32_t cp) {
    // 控制符：本身不可见，不需要字形
    if (cp < 0x20) return false;                 // C0
    if (cp >= 0x7F && cp <= 0x9F) return false;  // DEL + C1

    // 格式符 / 零宽字符：必须跟随前一个基字
    if (cp == 0x200B) return false;  // ZWSP
    if (cp == 0x200C) return false;  // ZWNJ
    if (cp == 0x200D) return false;  // ZWJ（emoji 连字、印度系连写的关键）
    if (cp == 0x2060) return false;  // WORD JOINER
    if (cp == 0xFEFF) return false;  // BOM / ZWNBSP
    if (cp == 0x180E) return false;  // 蒙文元音分隔符（Cf）

    // 蒙文自由变体选择符 FVS1–FVS3
    // 它们只用来在"词首/词中/词末"多个形态里挑一个，本身没有字形。
    // 一旦被分到别的字体，蒙文整词形态立刻错乱。
    if (cp >= 0x180B && cp <= 0x180D) return false;

    // 变体选择符
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;    // VS1–VS16
    if (cp >= 0xE0100 && cp <= 0xE01EF) return false;  // VS17–VS256

    // 组合标记（Mn）
    // 必须与基字同字体，否则 HarfBuzz 无法把标记定位到基字上。
    if (cp >= 0x0300 && cp <= 0x036F) return false;  // 组合附加符号
    if (cp >= 0x0483 && cp <= 0x0489) return false;  // 西里尔
    if (cp >= 0x0591 && cp <= 0x05C7) return false;  // 希伯来
    if (cp >= 0x0610 && cp <= 0x061A) return false;  // 阿拉伯
    if (cp >= 0x064B && cp <= 0x065F) return false;  // 阿拉伯
    if (cp == 0x0670) return false;
    if (cp >= 0x06D6 && cp <= 0x06DC) return false;
    if (cp >= 0x06DF && cp <= 0x06E8) return false;
    if (cp >= 0x06EA && cp <= 0x06ED) return false;
    if (cp >= 0x0900 && cp <= 0x0903) return false;  // 天城文
    if (cp >= 0x093A && cp <= 0x094F) return false;
    if (cp >= 0x20D0 && cp <= 0x20F0) return false;  // 符号用组合符
    if (cp >= 0xFE20 && cp <= 0xFE2F) return false;  // 半角组合符

    return true;
}

std::vector<FontRun> itemizeByFont(FontLibrary& lib, const uint32_t* cps, size_t count,
                                   const std::vector<int>& chain, float size_pt) {
    std::vector<FontRun> runs;
    if (!cps || count == 0) return runs;

    // chain 为空 → 整段归主字体（调用方自行处理 face_index=-1）
    if (chain.empty()) {
        runs.push_back(FontRun{0, count, -1});
        return runs;
    }

    // 预取 chain 中每个字体的实例。instance() 内部有 (face,size) 缓存，
    // 每个块调一次不会造成重复 hb_font_create。
    std::vector<hb_font_t*> chain_fonts;
    chain_fonts.reserve(chain.size());
    for (int fi : chain) {
        FontInstance* inst = lib.instance(fi, size_pt);
        chain_fonts.push_back(inst ? inst->hb() : nullptr);
    }

    // 逐码点定字体
    std::vector<int> owner(count, chain[0]);
    int last_owner = chain[0];
    for (size_t i = 0; i < count; ++i) {
        if (!needsOwnGlyph(cps[i])) {
            // 跟随前一个码点（第一个码点就无字形时，归主字体）
            owner[i] = last_owner;
            continue;
        }

        int chosen = -1;
        for (size_t k = 0; k < chain.size(); ++k) {
            hb_font_t* f = chain_fonts[k];
            if (!f) continue;
            hb_codepoint_t gid = 0;
            if (hb_font_get_nominal_glyph(f, cps[i], &gid) && gid != 0) {
                chosen = chain[k];
                break;
            }
        }
        // chain 全都不支持 → 落回主字体（会出豆腐块，但把"缺字"暴露出来，
        // 而不是静默换成别的字体。missing 计数由 layout 层统计。）
        owner[i] = (chosen >= 0) ? chosen : chain[0];
        last_owner = owner[i];
    }

    // 合并相邻同 owner 的码点为 run
    size_t i = 0;
    while (i < count) {
        size_t j = i + 1;
        while (j < count && owner[j] == owner[i]) ++j;
        runs.push_back(FontRun{i, j, owner[i]});
        i = j;
    }
    return runs;
}

std::vector<FontRun> itemizeByFont(FontLibrary& lib, std::string_view utf8,
                                   const std::vector<int>& chain, float size_pt) {
    const std::vector<uint32_t> cps = utf8ToCodepoints(std::string(utf8));
    return itemizeByFont(lib, cps.data(), cps.size(), chain, size_pt);
}

}  // namespace heditor
