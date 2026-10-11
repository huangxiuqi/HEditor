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
 * 名字不叫 Control 是因为引擎侧已有一个同名的旧表示（按 TextPos 记区间，待迁移），
 * 两者含义不同，不能共用一个名字。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/doc/length.h"
#include "core/utils/types.h"

namespace heditor {

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
 */
const char* valueShapeName(ValueShape shape);

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

const char* overflowName(Overflow o);

/**
 * 控件下划线的三档。与字符的 underline 标记不是一回事：它是挂在边界盒子上的装饰，
 * 不进 marks、不进内容，所以删掉里面的文字线还在（内容标记会跟着文字消失）。
 */
enum class Underline : uint8_t {
    None = 0,       // 不画
    Whole = 1,      // 整段都画
    BlankOnly = 2,  // 仅空白处画（表单填写的经典做法）
};

const char* underlineName(Underline u);

/**
 * 内容在控件里的对齐。
 */
enum class Align : uint8_t { Start = 0, Center = 1, End = 2 };

const char* alignName(Align a);

/**
 * 边框线型。没有"关闭"档：这一边画不画由 BorderSide::on 表态，两者不重复表达同一件事。
 */
enum class BorderStyle : uint8_t { Solid = 0, Dashed = 1, Dotted = 2 };

const char* borderStyleName(BorderStyle s);

/**
 * 一条边。四边各自独立而不是"边框开关加统一宽度"：病历表单常见只画下边（填空线）
 * 或只画左右边（栏位分隔），统一边框表达不了这两种。
 */
struct BorderSide {
    bool on = false;
    BorderStyle style = BorderStyle::Solid;

    /**
     * 线宽。Auto 表示由 styleRef 指的样式决定；显式写就是绝对长度。长度不做求值，
     * 换成磅由编译做一次换算（与段落格式同一个口径）。
     */
    Length width = Length::abs(ptToEmu(0.5));
    Color color = Color::rgb(0, 0, 0);

    bool operator==(const BorderSide& o) const = default;
};

struct Border {
    BorderSide left;
    BorderSide right;
    BorderSide top;
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
    std::string kind;
    Length width = Length::abs(ptToEmu(0.75));
    Length size;
    Color color = Color::rgb(0, 0, 0);
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
    Length width;  // 固定宽度；Auto 表示按内容
    Overflow overflow = Overflow::Expand;
    Underline underline = Underline::None;
    Border border;
    GlyphStyle glyph;
    Align align = Align::Start;
    std::string placeholder;
    std::string styleRef;
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
    bool flag = false;               // Bool
    std::string text;                // Text 的内容、Enum 的选项值、Resource 的资源 id
    std::vector<std::string> items;  // List

    bool operator==(const ControlValue& o) const = default;
};

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
};

}  // namespace heditor
