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
    const std::vector<std::string>& list() const { return list_; }

private:
    std::vector<std::string> list_;
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
    virtual bool stringAt(int index, std::string* out) const = 0;
    virtual bool fail(const std::string& what) const = 0;
};

/**
 * 浮点的十进制文本。9 位有效数字是 float 的往返充分条件，所以写出去再读回来是同一个值。
 */
std::string floatText(float v);
bool parseFloatText(const std::string& text, float* out);

/**
 * 严格 UTF-8 校验：拒绝过长编码、代理区与超出 U+10FFFF 的码点。
 *
 * JSON 库遇到非法字节时，有异常的环境抛异常、没有异常的环境终止进程（wasm 关掉了异常）。
 * 两条路都不该走到，所以写出之前自己查一遍。
 */
bool isValidUtf8(const std::string& s);

/**
 * 把要写出去的 JSON 树里的每个字符串查一遍（值与原对象的键都查）。
 */
bool validateStrings(const ordered_json& j, std::string* error);

ordered_json colorJson(const Color& c);
ordered_json writingModeJson(const WritingMode& wm);
ordered_json paraJson(const ParaFormatPatch& p);
ordered_json marksJson(const CharFormat& m);
ordered_json borderSideJson(const BorderSide& b);
ordered_json glyphJson(const GlyphStyle& g);
ordered_json appearanceJson(const Appearance& a, StringPool* strings);

/**
 * 表格的三类记录。列定义的字段是普通字符串（不进池）：列定义只在开启了"按列落值"的表上
 * 出现，重复度低，进池只是多一层间接。
 */
ordered_json tableJson(const TableSpec& t);
ordered_json rowJson(const RowSpec& r);

/**
 * 单元格记录写成 [row_span, col_span] 加可选的垂直对齐名：两元素是默认对齐，第三个元素
 * 只在非默认时出现 —— 与"默认值不写"同一条，旧文本读出来就是顶端对齐。
 */
ordered_json cellJson(const CellSpec& c);

/**
 * 图片、公式、域三类记录。三者都是原子的记录（域本期不进排版输入），字段少、各自成组，
 * 所以与表格那组并列而不并进控件那组。
 */
ordered_json imageJson(const ImageSpec& im);
ordered_json formulaJson(const FormulaSpec& f);
ordered_json fieldJson(const FieldSpec& f);
ordered_json controlValueJson(const ControlValue& v);
ordered_json controlJson(const ControlSpec& c, StringPool* strings);

/**
 * 读端的第一条约定：键不存在即默认值（这是"默认值不写"的另一面），键存在但类型不对才是错。
 * 下面这组返回 false 只表示"类型不对"，位置由调用方补。
 */
bool readInt(const ordered_json& j, const char* key, int* out);
bool readSize(const ordered_json& j, const char* key, std::size_t* out);
bool readBool(const ordered_json& j, const char* key, bool* out);
bool readString(const ordered_json& j, const char* key, std::string* out);
bool readIntArray(const ordered_json& j, const char* key, std::vector<int>* out);
bool readU32Array(const ordered_json& j, const char* key, std::vector<uint32_t>* out);
bool readStringArrayValue(const ordered_json& j, const char* key, std::vector<std::string>* out);
bool readLength(const ordered_json& j, const char* key, Length* out);
bool readColor(const ordered_json& j, const char* key, Color* out);
bool readWritingMode(const ordered_json& j, const char* key, WritingMode* out);
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
 */
ordered_json sectionJson(const SectionSpec& sec);
bool readSection(const ordered_json& j, const char* key, SectionSpec* out);
ordered_json headerFooterJson(const HeaderFooter& hf);
bool readHeaderFooter(const ordered_json& arr, int variant, HeaderFooter* out);

bool readPara(const ordered_json& j, const char* key, ParaFormatPatch* out);
bool readMarks(const ordered_json& j, const char* key, CharFormat* out);
bool readBorderSide(const ordered_json& j, const char* key, BorderSide* out);
bool readAppearance(const Context& ctx, const ordered_json& j, const char* key, Appearance* out);
bool readTable(const ordered_json& j, const char* key, TableSpec* out);
bool readRow(const ordered_json& j, const char* key, RowSpec* out);
bool readCell(const ordered_json& j, const char* key, CellSpec* out);
bool readImage(const ordered_json& j, const char* key, ImageSpec* out);
bool readFormula(const ordered_json& j, const char* key, FormulaSpec* out);
bool readField(const ordered_json& j, const char* key, FieldSpec* out);
bool readControl(const Context& ctx, const ordered_json& j, const char* key, ControlSpec* out);

}  // namespace heditor::recjson
