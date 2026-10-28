#include "core/rev/length.h"

namespace heditor {

namespace {

/**
 * 约分并归一符号：分母恒正，分子为 0 时分母归一为 1，于是相等比较与文本形式唯一。
 * 中间量用 int64，因为把 int32 的最小值取反在 32 位里放不下。
 * 分母为 0、或约分之后装不进 int32 时返回 false。
 */
bool normalize(int64_t num, int64_t den, int32_t* out_num, int32_t* out_den) {
    if (den == 0) return false;
    if (den < 0) {
        den = -den;
        num = -num;
    }
    int64_t a = num < 0 ? -num : num;
    int64_t b = den;
    while (b != 0) {
        const int64_t t = a % b;
        a = b;
        b = t;
    }
    if (a > 1) {
        num /= a;
        den /= a;
    }
    if (num == 0) den = 1;
    if (num < INT32_MIN || num > INT32_MAX || den > INT32_MAX) return false;
    *out_num = static_cast<int32_t>(num);
    *out_den = static_cast<int32_t>(den);
    return true;
}

/**
 * 造一个比例型长度。约分不通过（分母为 0 或结果装不下 int32）时返回默认构造的自动值，
 * 也就是"没有数值可表达"。
 */
Length ratio(LengthKind kind, int64_t num, int64_t den) {
    Length len;
    int32_t n = 0;
    int32_t d = 1;
    if (!normalize(num, den, &n, &d)) return len;
    len.kind = kind;
    len.num = n;
    len.den = d;
    return len;
}

/**
 * 倍数的千分位取整：1.6 倍 → 1600/1000 → 约分 8/5。
 */
Length scaledRatio(LengthKind kind, double multiple) {
    const double scaled = multiple * 1000.0;
    const int64_t milli = static_cast<int64_t>(scaled + (scaled < 0.0 ? -0.5 : 0.5));
    return ratio(kind, milli, 1000);
}

/**
 * 解析十进制整数，允许前导负号。空串、越界、含其它字符都算失败。
 * 自己解析而不调 strtoll：要能要求"整段都是数字"，而 strtoll 会跳过前导空白，
 * 越界还只能靠 errno 才发现。
 */
bool parseInt64(const char* s, size_t len, int64_t* out) {
    if (len == 0) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        i = 1;
        if (len == 1) return false;
    }
    const uint64_t limit =
        neg ? static_cast<uint64_t>(INT64_MAX) + 1u : static_cast<uint64_t>(INT64_MAX);
    uint64_t mag = 0;
    for (; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(s[i] - '0');
        // 先判会不会越界，再乘 —— 否则乘法本身就能溢出。
        if (mag > (limit - digit) / 10u) return false;
        mag = mag * 10u + digit;
    }
    if (!neg) {
        *out = static_cast<int64_t>(mag);
        return true;
    }
    // 负方向的界比正方向多一个数（INT64_MIN），所以要单独处理这一个。
    *out = (mag == static_cast<uint64_t>(INT64_MAX) + 1u) ? INT64_MIN : -static_cast<int64_t>(mag);
    return true;
}

bool startsWith(const std::string& text, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return text.size() >= n && text.compare(0, n, prefix) == 0;
}

/**
 * 分子（可选 分母）形式的载荷。省略分母等价于分母 1；分母必须为正 ——
 * 符号一律归到分子上，文本形式因此只有一种写法。
 */
bool parseRatio(const std::string& body, int64_t* num, int64_t* den) {
    const size_t slash = body.find('/');
    if (slash == std::string::npos) {
        *den = 1;
        return parseInt64(body.data(), body.size(), num);
    }
    if (!parseInt64(body.data(), slash, num)) return false;
    if (!parseInt64(body.data() + slash + 1, body.size() - slash - 1, den)) return false;
    return *den > 0;
}

/**
 * 解析比例型载荷并组装。约分不通过（含分母非正）时返回 false。
 */
bool parseRatioLength(const std::string& payload, LengthKind kind, Length* out) {
    int64_t num = 0;
    int64_t den = 1;
    if (!parseRatio(payload, &num, &den)) return false;
    const Length len = ratio(kind, num, den);
    if (len.kind != kind) return false;
    *out = len;
    return true;
}

std::string fractionText(const Length& len) {
    std::string s = std::to_string(len.num);
    if (len.den != 1) {
        s += '/';
        s += std::to_string(len.den);
    }
    return s;
}

}  // namespace

Length Length::abs(Emu e) {
    Length len;
    len.kind = LengthKind::Abs;
    len.abs_emu = e;
    return len;
}

Length Length::percent(int32_t num, int32_t den) { return ratio(LengthKind::Percent, num, den); }

Length Length::em(double multiple) { return scaledRatio(LengthKind::Em, multiple); }

Length Length::chars(double count) { return scaledRatio(LengthKind::Chars, count); }

Length Length::automatic() { return Length{}; }

std::string printLength(const Length& len) {
    switch (len.kind) {
        case LengthKind::Abs:
            return "abs:" + std::to_string(len.abs_emu);
        case LengthKind::Percent:
            return "pct:" + fractionText(len);
        case LengthKind::Em:
            return "em:" + fractionText(len);
        case LengthKind::Chars:
            return "chars:" + fractionText(len);
        case LengthKind::Auto:
            break;
    }
    return "auto";
}

bool parseLength(const std::string& text, Length* out) {
    if (out == nullptr) return false;
    if (text == "auto") {
        *out = Length::automatic();
        return true;
    }
    if (startsWith(text, "abs:")) {
        int64_t emu = 0;
        if (!parseInt64(text.data() + 4, text.size() - 4, &emu)) return false;
        *out = Length::abs(emu);
        return true;
    }
    if (startsWith(text, "pct:")) {
        return parseRatioLength(text.substr(4), LengthKind::Percent, out);
    }
    if (startsWith(text, "em:")) {
        return parseRatioLength(text.substr(3), LengthKind::Em, out);
    }
    if (startsWith(text, "chars:")) {
        return parseRatioLength(text.substr(6), LengthKind::Chars, out);
    }
    return false;
}

}  // namespace heditor
