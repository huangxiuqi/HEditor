/**
 * 记录级 JSON 编解码：字符属性、段落覆盖、外观、控件这些"挂在节点上的记录"与 JSON 互转。
 *
 * 模型层与修订层两套落盘格式共用这一份。两处各写一遍的后果不是重复，而是分叉：加一个字段
 * 只改一处，另一处读回来是默认值，而且看起来一切正常。
 *
 * 字符串按"池里写下标"编码：写端把重复出现的开放字符串（样式名、控件 key 与业务类型、
 * 校验规则名、语言标签）编进一张表，正文里只留下标；读端经 Context 查表。表由调用方持有，
 * 这一层只管怎么用 —— 两套格式各有各的表。
 *
 * 这里把 nlohmann 的类型放进了头文件（模型层原本只在 .cpp 里用它）：共用一份编解码比
 * "第三方类型不进头文件"更值当。换库时要改的是这一处与两个 .cpp 的装配代码。
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "core/doc/char_format.h"
#include "core/doc/control_spec.h"
#include "core/doc/field_spec.h"
#include "core/doc/formula_spec.h"
#include "core/doc/image_spec.h"
#include "core/doc/para_format.h"
#include "core/doc/section_spec.h"
#include "core/doc/table_spec.h"

namespace heditor::recjson {

using nlohmann::ordered_json;

/**
 * 写端的字符串池：同一个字符串只写一次，正文里留下标。按首次出现顺序编号，于是同一份内容
 * 写出来的字节序列是确定的。
 */
class StringPool {
public:
    int add(const std::string& s);

    /**
     * 池里的字符串，下标就是正文里留的标。
     * @return 字符串数组
     */
    const std::vector<std::string>& list() const { return list_; }

private:
    std::vector<std::string> list_;

    /**
     * 字符串到下标的反查。
     */
    std::unordered_map<std::string, int> index_;
};

/**
 * 读端的最小环境：字符串下标换回内容，以及记下第一条错误。
 *
 * 记录层不认识整份文档，错误只能是"哪个字段的哪一处读不下去"，由调用方把它接到自己的
 * 错误串上并决定前缀。fail 返回 false 是为了让调用处写成 return ctx.fail(...)。
 */
class Context {
public:
    virtual ~Context() = default;

    /**
     * 按下标取字符串。
     * @param index 下标
     * @param out 写回内容
     * @return false = 下标越界
     */
    virtual bool stringAt(int index, std::string* out) const = 0;

    /**
     * 记一条错误。
     * @param what 错误原文
     * @return 恒为 false（便于写成 return ctx.fail(...)）
     */
    virtual bool fail(const std::string& what) const = 0;

    /**
     * 文件里的字体下标换成本进程里的下标。
     *
     * 默认恒等：模型层那份格式不带字体表，下标就是下标。修订层带字体表时会改写这一处，
     * 让"文件说的第 2 号字体"落到"本机库里的第 3 号"。只在读的时候换 —— 写端不反向
     * 映射，它按当前下标查名字。
     * @param index 文件里的字体下标
     * @return 本进程里的字体下标（默认恒等）
     */
    virtual int mapFontIndex(int index) const { return index; }
};

/**
 * 浮点的十进制文本。最短往返表示（to_chars）：写出去再读回来是同一个 float，
 * 且没有多余的尾数（1.85 写成 "1.85"，不是 "1.85000002"）。
 * @param v 值
 * @return 最短往返表示的十进制文本
 */
std::string floatText(float v);

/**
 * 浮点文本 → float。
 * @param text 十进制文本
 * @param out 写回值
 * @return true = 文本是合法的十进制浮点
 */
bool parseFloatText(const std::string& text, float* out);

/**
 * 严格 UTF-8 校验：拒绝过长编码、代理区与超出 U+10FFFF 的码点。
 *
 * JSON 库遇到非法字节时，有异常的环境抛异常、没有异常的环境终止进程（wasm 关掉了异常）。
 * 两条路都不该走到，所以写出之前自己查一遍。
 * @param s 文本
 * @return true = 合法 UTF-8
 */
bool isValidUtf8(const std::string& s);

/**
 * 把要写出去的 JSON 树里的每个字符串查一遍（值与原对象的键都查）。
 * @param j 要查的 JSON 树
 * @param error 写回第一条错误
 * @return true = 全部合法
 */
bool validateStrings(const ordered_json& j, std::string* error);

/**
 * 把颜色写成 [r, g, b, a]。
 * @param c 颜色
 * @return 这个值的 JSON 形式
 */
ordered_json colorJson(const Color& c);

/**
 * 把书写模式写成 [行内轴, 块推进, 取向]。
 * @param wm 书写模式
 * @return 这个值的 JSON 形式
 */
ordered_json writingModeJson(const WritingMode& wm);

/**
 * 把段落格式覆盖写成对象（只写设置了的字段）。
 * @param p 覆盖
 * @return 这个值的 JSON 形式
 */
ordered_json paraJson(const ParaFormatPatch& p);

/**
 * 把字符格式写成对象（只写设置了的字段）。
 * @param m 字符格式
 * @return 这个值的 JSON 形式
 */
ordered_json marksJson(const CharFormat& m);

/**
 * 把一条边框写成 [开关, 线型, 线宽, 颜色]。
 * @param b 那一条边框
 * @return 这个值的 JSON 形式
 */
ordered_json borderSideJson(const BorderSide& b);

/**
 * 把图元外观写成 [名字, 线宽, 尺寸, 描边色, 填充色]。
 * @param g 图元外观
 * @return 这个值的 JSON 形式
 */
ordered_json glyphJson(const GlyphStyle& g);

/**
 * 把控件外观写成 JSON。
 * @param a 外观
 * @param strings 字符串池
 * @return 这个值的 JSON 形式
 */
ordered_json appearanceJson(const Appearance& a, StringPool* strings);

/**
 * 表格的三类记录。列定义的字段是普通字符串（不进池）：列定义只在开启了"按列落值"的表上
 * 出现，重复度低，进池只是多一层间接。
 *
 * 把表格记录写成 JSON。
 * @param t 表格记录
 * @return 这个值的 JSON 形式
 */
ordered_json tableJson(const TableSpec& t);

/**
 * 把行记录写成 JSON（三个开关与行高）。
 * @param r 行记录
 * @return 这个值的 JSON 形式
 */
ordered_json rowJson(const RowSpec& r);

/**
 * 单元格记录写成 [row_span, col_span] 加可选的垂直对齐名：两元素是默认对齐，第三个元素
 * 只在非默认时出现 —— 与"默认值不写"同一条，旧文本读出来就是顶端对齐。
 *
 * 把单元格跨度写成 [row_span, col_span] 加可选的垂直对齐名。
 * @param c 单元格记录
 * @return 这个值的 JSON 形式
 */
ordered_json cellJson(const CellSpec& c);

/**
 * 图片、公式、域三类记录。三者都是原子的记录（域本期不进排版输入），字段少、各自成组，
 * 所以与表格那组并列而不并进控件那组。
 *
 * 把图片记录写成 JSON。
 * @param im 图片记录
 * @return 这个值的 JSON 形式
 */
ordered_json imageJson(const ImageSpec& im);

/**
 * 把公式记录写成 JSON。
 * @param f 公式记录
 * @return 这个值的 JSON 形式
 */
ordered_json formulaJson(const FormulaSpec& f);

/**
 * 把域记录写成 JSON（含缓存值）。
 * @param f 域记录
 * @return 这个值的 JSON 形式
 */
ordered_json fieldJson(const FieldSpec& f);

/**
 * 把控件值写成 JSON（只写这个值形态用得到的字段）。
 * @param v 控件值
 * @return 这个值的 JSON 形式
 */
ordered_json controlValueJson(const ControlValue& v);

/**
 * 把控件记录写成 JSON。
 * @param c 控件记录
 * @param strings 字符串池
 * @return 这个值的 JSON 形式
 */
ordered_json controlJson(const ControlSpec& c, StringPool* strings);

/**
 * 读端的第一条约定：键不存在即默认值（这是"默认值不写"的另一面），键存在但类型不对才是错。
 * 下面这组返回 false 只表示"类型不对"，位置由调用方补。
 *
 * 从 JSON 读一个整数。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readInt(const ordered_json& j, const char* key, int* out);

/**
 * 从 JSON 读一个字节数。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readSize(const ordered_json& j, const char* key, std::size_t* out);

/**
 * 从 JSON 读一个布尔。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readBool(const ordered_json& j, const char* key, bool* out);

/**
 * 从 JSON 读一个字符串。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readString(const ordered_json& j, const char* key, std::string* out);

/**
 * 从 JSON 读一个整数数组。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readIntArray(const ordered_json& j, const char* key, std::vector<int>* out);

/**
 * 从 JSON 读一个码点数组。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readU32Array(const ordered_json& j, const char* key, std::vector<uint32_t>* out);

/**
 * 从 JSON 读一个字符串数组。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readStringArrayValue(const ordered_json& j, const char* key, std::vector<std::string>* out);

/**
 * 从 JSON 读一个长度。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readLength(const ordered_json& j, const char* key, Length* out);

/**
 * 从 JSON 读一个颜色。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readColor(const ordered_json& j, const char* key, Color* out);

/**
 * 从 JSON 读一个书写模式。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readWritingMode(const ordered_json& j, const char* key, WritingMode* out);

/**
 * 从 JSON 读一个浮点。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readFloat(const ordered_json& j, const char* key, float* out);

template <typename T>
bool readEnum(const ordered_json& j, const char* key, bool (*from)(const std::string&, T*),
              T* out) {
    if (!j.contains(key)) return true;
    if (!j[key].is_string()) return false;
    return from(j[key].get<std::string>(), out);
}

/**
 * 节记录（纸张、页边距、分页行为、页码、页眉页脚引用）。
 *
 * 模型层与修订层都要存它：节是唯一会让页度量在文档中途变化的机制，只搬结构不搬记录
 * 会让"多节文档"变成"几节共用第一节的页面设置"。
 *
 * 把节记录写成 JSON。
 * @param sec 节记录
 * @return 这个值的 JSON 形式
 */
ordered_json sectionJson(const SectionSpec& sec);

/**
 * 从 JSON 读节记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readSection(const ordered_json& j, const char* key, SectionSpec* out);

/**
 * 把一组页眉页脚写成 JSON。
 * @param hf 那一组页眉页脚
 * @return 这个值的 JSON 形式
 */
ordered_json headerFooterJson(const HeaderFooter& hf);

/**
 * 从三组页眉页脚里读第 variant 组。
 * @param arr 三组页眉页脚的数组
 * @param variant 变体下标
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readHeaderFooter(const ordered_json& arr, int variant, HeaderFooter* out);

/**
 * 从 JSON 读段落格式覆盖。
 * @param ctx 读端环境
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readPara(const Context& ctx, const ordered_json& j, const char* key, ParaFormatPatch* out);

/**
 * 从 JSON 读字符格式。
 * @param ctx 读端环境
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readMarks(const Context& ctx, const ordered_json& j, const char* key, CharFormat* out);

/**
 * 从 JSON 读一条边框。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readBorderSide(const ordered_json& j, const char* key, BorderSide* out);

/**
 * 从 JSON 读控件外观。
 * @param ctx 读端环境
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readAppearance(const Context& ctx, const ordered_json& j, const char* key, Appearance* out);

/**
 * 从 JSON 读表格记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readTable(const ordered_json& j, const char* key, TableSpec* out);

/**
 * 从 JSON 读行记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readRow(const ordered_json& j, const char* key, RowSpec* out);

/**
 * 从 JSON 读单元格记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readCell(const ordered_json& j, const char* key, CellSpec* out);

/**
 * 从 JSON 读图片记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readImage(const ordered_json& j, const char* key, ImageSpec* out);

/**
 * 从 JSON 读公式记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readFormula(const ordered_json& j, const char* key, FormulaSpec* out);

/**
 * 从 JSON 读域记录。
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readField(const ordered_json& j, const char* key, FieldSpec* out);

/**
 * 从 JSON 读控件记录。
 * @param ctx 读端环境
 * @param j 所属的 JSON 对象
 * @param key 键名
 * @param out 写回结果
 * @return true = 键不存在或类型对；false = 类型不对
 */
bool readControl(const Context& ctx, const ordered_json& j, const char* key, ControlSpec* out);

}  // namespace heditor::recjson
