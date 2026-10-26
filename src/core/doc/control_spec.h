/**
 * 内容控件的记录（见 docs/文档模型设计.md 第七节"控件表"）。
 *
 * 与 ParaFormat / CharFormat 一样挂在节点上、只对 ContentControl 有意义：编译、取值与索引
 * 都按流顺序取它。控件"标在哪一段"不在这里 —— 那由它在树里的位置决定，也就是那对成对的
 * 边界（第三节）。控件是哪种结构形态（容器型 / 原子型）也不在这里：本期控件都是容器型，
 * 到真有原子型控件时再加。
 *
 * 已落的是共同层里的这几项：key、controlType、valueShape 与 value、readonly、required、
 * structureLock、appearance、validateRef（id 就是 Node::id，hidden 是 Node 上的字段）。
 * 还差业务类型特有层的 options / group 与 format，它们要等 1.7 的表格与选项控件。
 * 名字不叫 Control 是因为引擎侧另有一个同名的扁平视图（ControlSet 里的 Control，按
 * TextPos 记区间，由编译产物导出、供宿主与几何使用）：一个是节点上的数据，一个是它的
 * 一种投影，含义不同，不能共用一个名字。
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "core/doc/length.h"
#include "core/utils/types.h"

namespace heditor {
struct ControlSpec;
struct ControlValue;

/**
 * 值形态：约束这个控件装的是什么值（见 docs/文档模型设计.md 第七节）。
 *
 * 它由业务类型决定，但不写进业务类型字符串里：宿主注册自己的控件类型时也要声明值形态，
 * 而值形态是封闭的一小组（引擎要按它决定值放哪、怎么校验、怎么按类型拼接文本）。
 * 业务类型是开放的，值形态不是 —— 这两层的封闭性不同，所以分开。
 */
enum class ValueShape : uint8_t {
    Text = 0,      // 值是它内部的内容（文本类控件没有 value 字段）
    Bool = 1,      // 是否选中
    Enum = 2,      // 选中的那个选项值
    List = 3,      // 选中的选项值集合
    Resource = 4,  // 资源 id（图片、附件这类）
};

/**
 * 种类名，用于诊断与报告。
 * @param shape 值形态
 * @return 名字
 */
const char* valueShapeName(ValueShape shape);

/**
 * 值形态名 → 值形态。不认识的名字返回 false。序列化按名字落盘（理由见 nodeKindFromName）。
 * @param name 名字
 * @param out 写回值形态
 * @return true = 名字认识
 */
bool valueShapeFromName(const std::string& name, ValueShape* out);

/**
 * 可以走属性入口的四个属性（第七节"属性入口只收运行时状态与策略这四个"）。
 *
 * 设计期属性（controlType / format / options / appearance）不在其中 —— 改它们等于重新定义控件，
 * 属于设计操作；标签也不在，它是内容，走正常编辑步骤。
 */
enum class ControlProp : uint8_t {
    Hidden = 0,         // 落在 Node 上（流位置换算要看它）
    Readonly = 1,       // 内容不可编辑
    Required = 2,       // 必填
    StructureLock = 3,  // 结构操作锁
};

/**
 * 属性名，用于诊断与报告。
 * @param prop 属性
 * @return 名字
 */
const char* controlPropName(ControlProp prop);

/**
 * 值超出固定宽度时的三档处理。默认 expand：不静默丢数据。
 *
 * 三档的排版后果不同（撑开会让断行与后续内容跟着动、裁切会让值看不见、溢出会盖住后面的内容），
 * 所以是属性而不是一个开关。控件是流里的跨度、没有自己的滚动视口，所以没有"横向滚动"这一档。
 */
enum class Overflow : uint8_t {
    Expand = 0,  // 宽度是下限，框跟着值变宽
    Clip = 1,    // 框宽不变，超出部分不绘制
    Spill = 2,   // 框宽不变，超出部分照常绘制
};

/**
 * 超出档的名字，用于诊断与落盘。
 * @param o 档位
 * @return 名字
 */
const char* overflowName(Overflow o);

/**
 * 名字转超出档。
 * @param name 名字
 * @param out 写回档位
 * @return true = 名字认识
 */
bool overflowFromName(const std::string& name, Overflow* out);

/**
 * 控件下划线的三档。与字符的 underline 标记不是一回事：它是挂在边界盒子上的装饰，
 * 不进 marks、不进内容，所以删掉里面的文字线还在（内容标记会跟着文字消失）。
 */
enum class Underline : uint8_t {
    None = 0,       // 不画
    Whole = 1,      // 整段都画
    BlankOnly = 2,  // 仅空白处画（表单填写的经典做法）
};

/**
 * 下划线档的名字。
 * @param u 档位
 * @return 名字
 */
const char* underlineName(Underline u);

/**
 * 名字转下划线档。
 * @param name 名字
 * @param out 写回档位
 * @return true = 名字认识
 */
bool underlineFromName(const std::string& name, Underline* out);

/**
 * 内容在控件里的对齐。
 */
enum class Align : uint8_t { Start = 0, Center = 1, End = 2 };

/**
 * 对齐方式的名字。
 * @param a 对齐
 * @return 名字
 */
const char* alignName(Align a);

/**
 * 名字转对齐方式。
 * @param name 名字
 * @param out 写回对齐
 * @return true = 名字认识
 */
bool alignFromName(const std::string& name, Align* out);

/**
 * 边框线型。没有"关闭"档：这一边画不画由 BorderSide::on 表态，两者不重复表达同一件事。
 */
enum class BorderStyle : uint8_t { Solid = 0, Dashed = 1, Dotted = 2 };

/**
 * 线型的名字。
 * @param s 线型
 * @return 名字
 */
const char* borderStyleName(BorderStyle s);

/**
 * 名字转线型。
 * @param name 名字
 * @param out 写回线型
 * @return true = 名字认识
 */
bool borderStyleFromName(const std::string& name, BorderStyle* out);

/**
 * 一条边。四边各自独立而不是"边框开关加统一宽度"：病历表单常见只画下边（填空线）
 * 或只画左右边（栏位分隔），统一边框表达不了这两种。
 */
struct BorderSide {
    /**
     * 这一边画不画。
     */
    bool on = false;

    /**
     * 线型（实线 / 虚线 / 点线）。
     */
    BorderStyle style = BorderStyle::Solid;

    /**
     * 线宽。Auto 表示由 styleRef 指的样式决定；显式写就是绝对长度。长度不做求值，
     * 换成磅由编译做一次换算（与段落格式同一个口径）。
     */
    Length width = Length::abs(ptToEmu(0.5));

    /**
     * 线的颜色。
     */
    Color color = Color::rgb(0, 0, 0);

    bool operator==(const BorderSide& o) const = default;
};

struct Border {
    /**
     * 左边。
     */
    BorderSide left;

    /**
     * 右边。
     */
    BorderSide right;

    /**
     * 上边。
     */
    BorderSide top;

    /**
     * 下边。
     */
    BorderSide bottom;

    bool operator==(const Border& o) const = default;
};

/**
 * 图元外观：图元名加线宽、尺寸与两种颜色。kind 是字符串（内置图元名或宿主注册的配方名），
 * 空表示不画图元 —— 所以"加一种图元"是加数据，不改枚举、不改格式。
 *
 * 模型只存怎么画所需的数据，不存绘制指令；真正画它的是引擎（会上纸的内容必须由引擎绘制）。
 */
struct GlyphStyle {
    /**
     * 图元名（内置名或宿主注册的配方名）；空表示不画。
     */
    std::string kind;

    /**
     * 线宽。
     */
    Length width = Length::abs(ptToEmu(0.75));

    /**
     * 图元尺寸。
     */
    Length size;

    /**
     * 描边色，也就是选择类记号（圆圈 / 方框 / 对勾）的颜色。
     *
     * 默认全透明 = 跟随那一段文字的颜色。之所以用"透明"当哨兵而不是一个 bool：这一层
     * 是纯数据（落盘、哈希都按逐字段比），多一个开关字段就多一处要同步的地方。
     */
    Color color;

    /**
     * 填充色。
     */
    Color fill = Color::rgb(0, 0, 0);

    bool operator==(const GlyphStyle& o) const = default;
};

/**
 * 控件的外观。全部是作者意图，随控件落盘。
 *
 * placeholder 是独立字段而不是内容：值存在内容或 value 上，提示文字存在这里，所以取值不会
 * 把它当值取出来。它是作者写的（"请填写住院号"），默认值必须存得住，所以要落盘；
 * 而"只读与打印模式不画它"是渲染策略，与落盘无关。
 *
 * styleRef 与 invalidStyleRef 是样式引用（名字），校验不通过时渲染层按 invalidStyleRef 画提示。
 * 模型只存"用哪套样式"，不存怎么画。
 */
struct Appearance {
    /**
     * 固定宽度；Auto 表示按内容。
     */
    Length width;

    /**
     * 值超出固定宽度时怎么处理。
     */
    Overflow overflow = Overflow::Expand;

    /**
     * 控件下划线画在哪一档。
     */
    Underline underline = Underline::None;

    /**
     * 四边边框。
     */
    Border border;

    /**
     * 边上的图元（如必填星号）；kind 为空表示不画。
     */
    GlyphStyle glyph;

    /**
     * 内容在控件里的对齐。
     */
    Align align = Align::Start;

    /**
     * 提示文字（值为空时画它，不当值取出来）。
     */
    std::string placeholder;

    /**
     * 外观样式引用。
     */
    std::string styleRef;

    /**
     * 校验不通过时用的样式引用。
     */
    std::string invalidStyleRef;

    bool operator==(const Appearance& o) const = default;
};

/**
 * 控件的值。值形态决定哪个字段有意义：Text 读内容（不用本结构）、Bool 读 flag、
 * Enum 与 Resource 读 text、List 读 items。
 *
 * 三个字段并存而不是 union/variant：记录要落盘（1.10），一个自描述的字段组比带标签的联合
 * 更好写也更好读；多出来的那几个字段是空串与空数组，代价可以忽略。
 */
struct ControlValue {
    /**
     * Bool 的值。
     */
    bool flag = false;

    /**
     * Enum 的选项值、Resource 的资源 id（Text 形态读内容，不用它）。
     */
    std::string text;

    /**
     * List 选中的选项值集合。
     */
    std::vector<std::string> items;

    bool operator==(const ControlValue& o) const = default;
};

/**
 * 一个选项：值 + 显示标签。
 *
 * 值与标签分开是刚性要求：值是对外报的业务码（换宿主、换语言都不变），标签是纸面上那段
 * 文字（可以改文案、可以翻译）。两者合成一个字符串的话，改一次文案就改了数据契约。
 */
struct ControlOption {
    /**
     * 选项值（业务码）。落盘、比较、回填都用它。
     */
    std::string value;

    /**
     * 显示标签。进控件的内容，也就是纸面上那段文字。
     */
    std::string label;

    bool operator==(const ControlOption& o) const = default;
};

/**
 * 选择类的记号字符（进内容的那一个码点）。
 *
 * 方框两种状态用 U+2610 / U+2611：语义正确（复制、导出、别的渲染器都认），但本工程
 * 那几个字体里没有这一对字形 —— 宽度借"□"（U+25A1）的，图形由引擎自己画（方框 + 对勾）。
 * 圆点用字体自带的 ○ / ◉，同样由引擎按记号色重画一次（不然单靠正文颜色改不了它）。
 */
inline constexpr uint32_t kCheckboxMarkOff = 0x2610;  // ☐ 未选中
inline constexpr uint32_t kCheckboxMarkOn = 0x2611;   // ☑ 选中
inline constexpr uint32_t kRadioMarkOff = 0x25CB;     // ○ 未选中
inline constexpr uint32_t kRadioMarkOn = 0x25C9;      // ◉ 选中
inline constexpr uint32_t kCheckMark = 0x2713;        // ✓ 画在方框里的对勾

/**
 * 这个内容偏移是不是某个选项的记号位置（图形占的那一个码点）。
 * @param spec 控件记录
 * @param cp_in_content 内容内的码点偏移
 * @param checked 写回这一项是否选中
 * @return true = 是记号位置
 */
bool controlMarkerAt(const ControlSpec& spec, int cp_in_content, bool* checked);

/**
 * 内容只收哪一类字符。它约束的是编辑入口（打进来的字），不是值的校验 ——
 * 值怎么校验由业务侧决定，见 controlValueValid。
 */
enum class ContentFilter : uint8_t {
    Any = 0,     // 什么都收
    Number = 1,  // 只收数字、一个小数点与开头的负号
};

/**
 * 数值内容的字符过滤器。它不是"整串合不合法"的判据（中间态 "-"、"1." 都得能打进去），
 * 只回答"再打这一个字行不行"。
 * @param current 当前内容（UTF-8）
 * @param add 想追加的一个码点
 * @return true = 收下这个字符
 */
bool numberAccepts(const std::string& current, uint32_t add);

/**
 * 整串是不是一个合法的数值（提交时的判据）。空串算合法：那是"没填"，由 required 管。
 * @param text 内容（UTF-8）
 * @return true = 空串或一个十进制数
 */
bool numberTextValid(const std::string& text);

/**
 * 单选的日期单位位掩码。没列出的单位不进选择器；显示格式另由 format 决定，两者不互相推导。
 */
inline constexpr uint16_t kDateUnitYear = 1u << 0;
inline constexpr uint16_t kDateUnitMonth = 1u << 1;
inline constexpr uint16_t kDateUnitDay = 1u << 2;
inline constexpr uint16_t kDateUnitHour = 1u << 3;
inline constexpr uint16_t kDateUnitMinute = 1u << 4;
inline constexpr uint16_t kDateUnitSecond = 1u << 5;

/**
 * 六个单位全选。默认值 —— 时间选择大多数场合要精确到秒。
 */
inline constexpr uint16_t kDateUnitsAll = kDateUnitYear | kDateUnitMonth | kDateUnitDay |
                                          kDateUnitHour | kDateUnitMinute | kDateUnitSecond;

/**
 * 日期值的默认格式。与 docs 里那条一致：2026年10月26日 01时57分41秒。
 */
inline constexpr const char* kDefaultDateFormat = "yyyy年MM月dd日 HH时mm分ss秒";

/**
 * 把 ISO 串按格式串折成显示文字。
 *
 * 值是 ISO 串（YYYY-MM-DDTHH:MM:SS），显示才是格式串 —— 存与显分开，换格式不改数据。
 * 认的记号是 yyyy / MM / dd / HH / mm / ss，其余字符原样输出。解析不了就原样返回输入，
 * 不猜、不补零：把一份坏的日期显示成"看起来正常"的日期，比显示原文更危险。
 * @param iso ISO 串（YYYY-MM-DDTHH:MM:SS）
 * @param pattern 格式串（空 = 默认格式）
 * @return 显示文字
 */
std::string formatControlDate(const std::string& iso, const std::string& pattern);

/**
 * 串是不是一个合法的 ISO 日期时间（YYYY-MM-DDTHH:MM:SS，范围含闰年）。空串算合法。
 * @param iso ISO 串
 * @return true = 空串或合法日期时间
 */
bool isoDateTimeValid(const std::string& iso);

/**
 * 选择类控件的特有层：标签、选项、互斥组与是否多选。
 *
 * 单控件（一个控件等于一个选项）用 label + group；组控件（一个控件含多个选项）用 options。
 * 两者的区别是布局，不是选项数量 —— 单控件能分散在文档各处、再按 group 关联，组控件整体断行。
 */
struct ChoiceDetail {
    /**
     * 单控件的标签（组控件为空）。
     */
    std::string label;

    /**
     * 组控件的选项（单控件为空）。
     */
    std::vector<ControlOption> options;

    /**
     * 互斥组名：同组的单选最多一个选中。空 = 不与任何人互斥。
     */
    std::string group;

    /**
     * 多选还是单选。单控件靠它区分复选框与单选框，组控件靠它区分复选组与单选组。
     */
    bool multi = false;

    /**
     * 光标不许停在内容里（三档模式都生效）。选择类的整体性靠它，默认打开。
     */
    bool caret_barrier = true;

    bool operator==(const ChoiceDetail& o) const = default;
};

/**
 * 下拉的特有层：选项表与是否允许自由输入。选项在弹出层里，不进文档流。
 */
struct DropdownDetail {
    /**
     * 选项表。
     */
    std::vector<ControlOption> options;

    /**
     * 允许输入选项表以外的值。关闭时 value 必须是选项表里的某一个值。
     */
    bool free_input = false;

    bool operator==(const DropdownDetail& o) const = default;
};

/**
 * 日期的特有层：显示格式与可选单位。
 */
struct DateDetail {
    /**
     * 显示格式串（空 = 默认格式，见 kDefaultDateFormat）。
     */
    std::string format;

    /**
     * 选择器里能选中哪些单位（位掩码，见 kDateUnitXXX）。
     */
    uint16_t units = kDateUnitsAll;

    bool operator==(const DateDetail& o) const = default;
};

/**
 * 文本类的特有层：内容收哪些字符。纯文本控件没有特有层（monostate），
 * 只有数值输入需要它。
 */
struct TextDetail {
    /**
     * 内容接受哪一类字符。
     */
    ContentFilter filter = ContentFilter::Any;

    bool operator==(const TextDetail& o) const = default;
};

/**
 * 宿主自定义类型的特有层：不认识的属性原样存着。
 *
 * 导入外部文档遇到不认识的类型时，属性不能丢 —— 丢一份就是"存一次盘少一份数据"。
 * 引擎不认识这些属性，只负责搬运与落盘。
 */
struct CustomDetail {
    /**
     * 属性名到属性值（值按文本存，宿主自己解释）。
     */
    std::map<std::string, std::string> props;

    bool operator==(const CustomDetail& o) const = default;
};

/**
 * 类型特有层：一个控件只带它真正用的那一组。
 *
 * 做成变体而不是把七八个字段摊在 ControlSpec 上：文本控件带 options、日期控件带 group
 * 这类"永远用不到"的字段会让"这个控件有哪些属性"变成一句空话，序列化也要为每个字段
 * 判一次默认值。变体把这件事交给类型系统：拿到 ChoiceDetail 就说明它一定有选项语义。
 *
 * monostate 是"没有特有层"，给纯文本控件用。
 */
using ControlDetail = std::variant<std::monostate, TextDetail, ChoiceDetail, DropdownDetail,
                                   DateDetail, CustomDetail>;

/**
 * 取选择类特有层；不是选择类返回 nullptr。
 * @param spec 控件记录
 * @return 选择类特有层
 */
const ChoiceDetail* choiceOf(const ControlSpec& spec);

/**
 * 取下拉特有层；不是下拉返回 nullptr。
 * @param spec 控件记录
 * @return 下拉特有层
 */
const DropdownDetail* dropdownOf(const ControlSpec& spec);

/**
 * 取日期特有层；不是日期返回 nullptr。
 * @param spec 控件记录
 * @return 日期特有层
 */
const DateDetail* dateOf(const ControlSpec& spec);

/**
 * 取文本特有层；纯文本控件返回 nullptr。
 * @param spec 控件记录
 * @return 文本特有层
 */
const TextDetail* textOf(const ControlSpec& spec);

/**
 * 取自定义特有层；不是自定义类型返回 nullptr。
 * @param spec 控件记录
 * @return 自定义特有层
 */
const CustomDetail* customOf(const ControlSpec& spec);

/**
 * 按业务类型造一份记录：共同层给默认值，特有层按家族填。
 *
 * 认识的类型：text / number / radioButton / checkBox / radioGroup / checkBoxGroup /
 * dropdownList / date。不认识的名字落进 CustomDetail（原样透传）并给文本值形态 ——
 * 不假装认识，也不拒绝导入。
 * @param controlType 业务类型名
 * @return 新记录（key 与标签由调用方填）
 */
ControlSpec makeControlSpec(const std::string& controlType);

/**
 * 按记录与值生成控件的显示内容（进文档流的那段文字）。
 *
 * 文本与数值的内容就是值本身，返回空串表示"不改内容"；选择类生成图形加标签；
 * 下拉取选中项的标签（自由输入时就是那段输入）；日期按格式折。生成与解析只有这一份：
 * 命中某个选项、切换后重建内容、插入时给初始内容，都走它。
 * @param spec 控件记录
 * @param value 控件的值（文本类忽略）
 * @return 显示文字
 */
std::string controlDisplayText(const ControlSpec& spec, const ControlValue& value);

/**
 * 组控件每个选项在内容里的码点区间 [begin, end)。
 *
 * 点击命中靠它把"点了内容里的第几个码点"折成"点了哪个选项"。区间由生成内容的同一套
 * 排版给出（图形 1 + 空格 1 + 标签，项间两个空格），所以内容与命中不会各算一套。
 * @param spec 控件记录
 * @param out 每个选项一对；不是组控件时清空
 * @return true = 这是个组控件
 */
bool controlOptionSpans(const ControlSpec& spec, std::vector<std::pair<int, int>>* out);

/**
 * 改定义时"要改哪几项"。
 *
 * 每个字段都可选，空表示"这一项不动" —— 与"改成空值"是两件事（选项表清空是给空数组，
 * 不动是不给）。做成补丁而不是"整份记录再传一遍"：调用方只知道要改哪一项，让它把其余
 * 项抄一遍，抄漏一个就是静默丢配置。
 *
 * 这里只收没有别的写入路径的那几项：readonly / required / hidden / structureLock 走
 * SetControlProp，值走 SetControlValue。同一个字段开第二条写入路径，两条迟早会分叉。
 */
struct ControlPatch {
    /**
     * 业务编码。
     */
    std::optional<std::string> key;

    /**
     * 单控件的标签（组控件不用它，内容由选项表生成）。
     */
    std::optional<std::string> label;

    /**
     * 选项表（选择类与下拉）。清空传空数组，不动就不给。
     */
    std::optional<std::vector<ControlOption>> options;

    /**
     * 互斥组名（单选）。
     */
    std::optional<std::string> group;

    /**
     * 下拉允许自由输入。
     */
    std::optional<bool> free_input;

    /**
     * 日期显示格式串。
     */
    std::optional<std::string> format;

    /**
     * 日期能选中哪些单位（位掩码）。
     */
    std::optional<uint16_t> units;

    /**
     * 选择类记号的颜色（圆圈 / 方框 / 对勾）。全透明表示跟随文字颜色。
     */
    std::optional<Color> glyph_color;

    /**
     * 期望的业务类型。给了就必须与当前一致，否则整份拒绝。
     *
     * 记录里不存它 —— 它的用处只有一个：把"想换类型"的调用显式挡住。改类型不是改属性：
     * 值形态、内容形状、命中归属会一起变，那是"删掉再插一个"，该走插入那条路。
     */
    std::optional<std::string> expect_type;
};

/**
 * 把补丁合并进一份记录。
 *
 * 严格而不静默：给了一项这个类型不收的字段就整份拒绝并说明原因（例如给文本控件送 options）。
 * 静默忽略的后果是调用方以为改成了、实际什么都没发生 —— 那种错查起来最费时间。
 * @param spec 记录（就地改）
 * @param patch 要改的项
 * @param error 写回拒绝原因
 * @return true = 合并成功
 */
bool applyControlPatch(ControlSpec* spec, const ControlPatch& patch, std::string* error);

/**
 * 定义变了之后把值夹回合法范围：指向已被删掉的选项的值跟着清掉。
 *
 * 不清的话会留下"值不在选项表里"的脏数据 —— 显示时找不到标签、报出去业务侧不认，
 * 而画面上看起来只是少了几个字。
 * @param spec 记录（新定义）
 * @param value 值（就地夹）
 */
void pruneControlValue(const ControlSpec& spec, ControlValue* value);

/**
 * 控件外观的摘要（括号颜色、边框粗细、记号颜色与尺寸）。
 *
 * 放进块流对象的内容指纹里：边界不占字形，改这些属性时位置一个都不动，行指纹若不比
 * 它，那一页就被判成新鲜 —— 命令成功了、读回的值也对，画面却停在旧帧（TRAPS 195 同族）。
 * 两个编译入口（模型树与投影）共用这一份，免得两处各写一份、迟早只改一处。
 * @param a 控件外观
 * @return 摘要
 */
uint64_t controlAppearanceDigest(const Appearance& a);

/**
 * 值对这个控件合不合法。空值一律合法（那是"没填"，由 required 管）。
 * @param spec 控件记录
 * @param value 值
 * @param error 写回不合法原因
 * @return true = 合法
 */
bool controlValueValid(const ControlSpec& spec, const ControlValue& value, std::string* error);

struct ControlSpec {
    /**
     * 业务编码（如 chief_complaint）。可重复 —— 同一 key 绑多个控件表示同一个字段的多处显示，
     * 按 key 批量设值会一起改。它与节点 id 的分工是"业务标识"与"模型身份"。
     */
    std::string key;

    /**
     * 业务类型（text / richText / date / checkbox / ...），字符串、可注册。写进枚举就等于
     * 每加一种控件都要同时改容器判定、不变量检查器、编译与序列化，所以宁可是数据。
     */
    std::string controlType;

    /**
     * 值形态：值放内容里还是属性上、读哪个字段。宿主建控件时给出；按 controlType 自动推断
     * 要等控件类型注册表落地（它会填默认值形态），那时这个字段仍留着（注册表给的是默认值）。
     */
    ValueShape valueShape = ValueShape::Text;

    /**
     * 属性上的值。Text 形态不用它（内容即值），其余形态按 valueShape 读其中一个字段。
     */
    ControlValue value;

    /**
     * 内容不可编辑。它只是可编辑集合的一层：包住光标位置的每一层都允许，位置才可编辑
     * （第七节"可编辑集合取交集"）。默认 false，于是表单模式默认内容可编辑。
     */
    bool readonly = false;

    /**
     * 必填。提交校验用；隐藏的控件默认不参与必填校验（用户根本填不了）。
     */
    bool required = false;

    /**
     * 结构操作锁：true 表示控件自身不可被整体删除。默认锁住，于是表单模式的默认是
     * "内容可编辑、控件不可删"；作者要开放就显式设为 false。它随控件落盘（与 readonly 一致）。
     *
     * 第七节把它记成"可否做结构操作的开关，默认 false"，说的是同一件事的反面；这里按字段名
     * structureLock 的方向定成正向的锁，默认 true 与"默认不可删"等价。
     */
    bool structureLock = true;

    /**
     * 外观：宽度、超出档、下划线、边框、图元、对齐、提示文字与两条样式引用。全部落盘。
     */
    Appearance appearance;

    /**
     * 校验规则引用。存的是名字而不是代码 —— 与控件事件同一条判据：绑定是数据、代码是程序，
     * 凡"换一个宿主仍要生效"的只能是数据。宿主把名字映射到自己的校验钩子；名字为空表示不校验。
     */
    std::string validateRef;

    /**
     * 类型特有层：只带这个控件真正用的那一组属性（见 ControlDetail）。
     */
    ControlDetail detail;

    /**
     * 逐字段相等。序列化要拿它判断"这个控件是不是全默认"，从而决定写不写这个条目；
     * 少比一个字段的表现是"改了它却没落盘"。
     */
    bool operator==(const ControlSpec& o) const = default;
};

}  // namespace heditor
