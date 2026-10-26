// 属性面板的描述层：把控件与表格的现状摊成字段表，再把字段表折回一次改动。
//
// 为什么放在库里：字段表随控件类型与表格三层记录变，宿主跟不上；而"哪一项落到哪个写入口、
//   哪些项只提交真的变了的"是引擎语义 —— 每个宿主各写一遍，写歪一次就是静默改错文档。
//   这里只产出数据，不碰 DOM：画成原生控件、组件库还是别的框架，由宿主定。
//
// 值的形态固定三种：文本类是字符串，数值是数字，开关是布尔。多行文本（选项表）也走字符串，
//   一行一项写成"值|标签"，折回去的解析在这一层做 —— 它属于值形态，不属于画法。
import type {
  ControlForm,
  ControlSummary,
  ControlUpdateOptions,
  FormField,
  FormOptions,
  FormValues,
  PageMargins,
  PageSetup,
  PageSetupForm,
  PaperSize,
  RGBA,
  TableCellOptions,
  TableForm,
  TableOptions,
  TableRowOptions,
  TableState,
} from './types';
import { PAPER_PT } from './types';

/**
 * 1 厘米等于多少磅（72 / 2.54）。页边距与纸张尺寸在界面上按厘米给。
 */
const kCmPt = 72 / 2.54;

/**
 * 默认标签。宿主想换语言就用 FormOptions.labels 逐键覆盖。
 */
const kLabels: Record<string, string> = {
  value: '内容',
  label: '标签',
  group: '互斥组',
  options: '选项',
  free: '自由输入',
  format: '格式',
  precision: '可填到',
  glyph: '记号颜色',
  readonly: '只读',
  required: '必填',
  key: '业务编码',
  tableWidth: '宽度',
  tableAlign: '水平位置',
  rowHeader: '标题行',
  rowSplit: '允许断行',
  rowBreak: '行前分页',
  rowHeight: '行高',
  rowRule: '行高规则',
  cellVAlign: '垂直对齐',
  paper: '纸张',
  orient: '方向',
  width: '宽',
  height: '高',
  marginTop: '上',
  marginBottom: '下',
  marginLeft: '左',
  marginRight: '右',
};

/**
 * 有没有选项表的两种组类型。
 */
const kGroupTypes = new Set(['radioGroup', 'checkBoxGroup']);

/**
 * 四种选择类（记号与颜色对它们都生效）。
 */
const kChoiceTypes = new Set(['radioButton', 'checkBox', 'radioGroup', 'checkBoxGroup']);

/**
 * 取一个字段的标签：宿主的替换表优先，其次默认表，最后退回键名。
 * @param key 字段键
 * @param opts 造字段表的选项
 * @returns 显示在左列的字
 */
function labelOf(key: string, opts?: FormOptions): string {
  return opts?.labels?.[key] ?? kLabels[key] ?? key;
}

/**
 * 把函数式选项表读成多行文本：一行一项，写成"值|标签"。
 * @param options 选项表
 * @returns 多行文本
 */
function optionsToText(options: Array<{ value: string; label: string }>): string {
  return options.map((op) => `${op.value}|${op.label}`).join('\n');
}

/**
 * 把多行文本读成选项表。每行"值|标签"，只写一段时值与标签相同。
 * @param text 多行文本
 * @returns 选项表
 */
function textToOptions(text: string): Array<{ value: string; label: string }> {
  const out: Array<{ value: string; label: string }> = [];
  for (const raw of text.split('\n')) {
    const line = raw.trim();
    if (!line) continue;
    const cut = line.indexOf('|');
    const value = cut < 0 ? line : line.slice(0, cut).trim();
    const label = cut < 0 ? line : line.slice(cut + 1).trim();
    if (!value) continue;
    out.push({ value, label: label || value });
  }
  return out;
}

/**
 * #rrggbb 读成四通道。
 * @param hex 颜色串
 * @returns RGBA（不透明度 255）
 */
function hexToRgba(hex: string): RGBA {
  const v = hex.replace('#', '');
  const at = (i: number): number => Number.parseInt(v.slice(i, i + 2), 16) || 0;
  return [at(0), at(2), at(4), 255];
}

/**
 * 四通道读成 #rrggbb。全透明（跟随文字）时给一个中性色当起点。
 * @param rgba 四通道
 * @returns 颜色串
 */
function rgbToHex(rgba: RGBA): string {
  if (!rgba[3]) return '#000000';
  const two = (n: number): string => Math.max(0, Math.min(255, n | 0)).toString(16).padStart(2, '0');
  return `#${two(rgba[0])}${two(rgba[1])}${two(rgba[2])}`;
}

/**
 * 日期单位该配哪种原生输入控件、秒的步长是多少。
 *
 * 只有时间单位时用 time、只有日期时用 date、两者都有用 datetime-local；步长按有没有
 *   秒来定（60 = 界面上只到分）。这两项都由 dateUnits 推，宿主不该自己判一遍。
 * @param units 单位名
 * @returns 输入控件类型与步长
 */
function dateInputOf(units: string[]): {
  inputType: 'date' | 'time' | 'datetime-local';
  step: number;
} {
  const withTime = units.includes('hour') || units.includes('minute') || units.includes('second');
  const withDate = units.includes('year') || units.includes('month') || units.includes('day');
  return {
    inputType: !withDate ? 'time' : withTime ? 'datetime-local' : 'date',
    step: units.includes('second') ? 1 : 60,
  };
}

/**
 * 把一个完整 ISO 串裁到该输入控件认的长度。
 *
 * 长度必须与控件类型配套：type=date 塞 19 位串时浏览器把 value 当空串，界面上就是
 *   "点了日期，框里什么都没有"。所以这里按类型裁，不按串里有没有时间来裁。
 * @param iso 完整 ISO 串（YYYY-MM-DDTHH:MM:SS）
 * @param inputType 输入控件类型
 * @param withSecond 精度到不到秒
 * @returns 该控件认的那一段
 */
function isoPartOf(
  iso: string,
  inputType: 'date' | 'time' | 'datetime-local',
  withSecond: boolean,
): string {
  if (!iso) return '';
  if (inputType === 'date') return iso.slice(0, 10);
  if (inputType === 'time') return iso.slice(11, withSecond ? 19 : 16);
  return iso.slice(0, withSecond ? 19 : 16);
}

/**
 * 把输入控件给的片段补回完整 ISO 串。
 *
 * 补的是"控件类型决定不了的那几段"：日期型保留原有的时间（界面上没给改它的机会），
 *   时间型借原值的日期（只有时间的控件没有自己的日子），到分的那两型补 ":00"。
 * @param iso 现在的值（借它取缺失的那几段）
 * @param part 控件给的片段
 * @param inputType 输入控件类型
 * @returns 完整 ISO 串；片段为空时返回空串
 */
function isoJoinOf(
  iso: string,
  part: string,
  inputType: 'date' | 'time' | 'datetime-local',
): string {
  if (!part) return '';
  if (inputType === 'date') {
    const time = iso.length >= 19 ? iso.slice(11, 19) : '00:00:00';
    return part.slice(0, 10) + 'T' + time;
  }
  if (inputType === 'time') {
    const day = iso.length >= 10 ? iso.slice(0, 10) : '1970-01-01';
    return day + 'T' + (part.length === 5 ? part + ':00' : part.slice(0, 8));
  }
  // 只给到日期（10 位）时补零点：宿主把 date 那一型的片段塞给 datetime-local 是常事，
  //   切片到 10 位会得到一个不带时间的串，引擎按非法 ISO 整份拒绝。
  if (part.length <= 10) return part.slice(0, 10) + 'T00:00:00';
  return part.length === 16 ? part + ':00' : part.slice(0, 19);
}

/**
 * 日期单位折成三档：只到日 / 到分 / 到秒。
 * @param units 单位名
 * @returns 档位
 */
function datePrecision(units: string[]): string {
  if (units.includes('second')) return 'second';
  if (units.includes('minute')) return 'minute';
  return 'day';
}

/**
 * 三档折回日期单位。
 * @param precision 档位
 * @returns 单位名
 */
function precisionUnits(
  precision: string,
): Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'> {
  if (precision === 'second') return ['year', 'month', 'day', 'hour', 'minute', 'second'];
  if (precision === 'minute') return ['year', 'month', 'day', 'hour', 'minute'];
  return ['year', 'month', 'day'];
}

/**
 * 一个控件的属性表单。
 *
 * 字段随业务类型变：文本与数值给内容，单控件给标签与互斥组，组控件与下拉给选项表，
 *   日期给格式与精度，四种选择类再给记号颜色；行为那三项所有类型都有。
 * @param c 控件现状
 * @param opts 标签替换表
 * @returns 字段表与面板标题
 */
export function controlFormOf(c: ControlSummary, opts?: FormOptions): ControlForm {
  const type = c.type || 'text';
  const L = (key: string): string => labelOf(key, opts);
  const fields: FormField[] = [];

  if (type === 'text' || type === 'number') {
    fields.push({
      kind: 'text',
      key: 'value',
      label: L('value'),
      value: c.text,
      placeholder: type === 'number' ? '只能填数字' : '',
    });
  } else if (type === 'radioButton' || type === 'checkBox') {
    fields.push({ kind: 'text', key: 'label', label: L('label'), value: c.label });
    if (type === 'radioButton') {
      fields.push({
        kind: 'text',
        key: 'group',
        label: L('group'),
        value: c.group,
        placeholder: '同组最多选一个；空 = 独立',
      });
    }
  } else if (kGroupTypes.has(type) || type === 'dropdownList') {
    fields.push({
      kind: 'lines',
      key: 'options',
      label: L('options'),
      value: optionsToText(c.options),
      rows: 4,
      hint:
        type === 'dropdownList'
          ? '一行一项，写成"值|标签"。'
          : '一行一项，写成"值|标签"。删掉某项，指向它的取值会跟着清空。',
    });
    if (type === 'dropdownList') {
      fields.push({ kind: 'toggle', key: 'free', label: L('free'), value: c.freeInput });
    }
  } else if (type === 'date') {
    const { inputType, step } = dateInputOf(c.dateUnits);
    fields.push({
      kind: 'datetime',
      key: 'value',
      label: L('value'),
      value: isoPartOf(c.value.text, inputType, c.dateUnits.includes('second')),
      inputType,
      step,
    });
    fields.push({
      kind: 'text',
      key: 'format',
      label: L('format'),
      value: c.format,
      placeholder: 'yyyy年MM月dd日 HH时mm分ss秒',
    });
    fields.push({
      kind: 'select',
      key: 'precision',
      label: L('precision'),
      value: datePrecision(c.dateUnits),
      options: [
        { value: 'day', text: '日期' },
        { value: 'minute', text: '到分' },
        { value: 'second', text: '到秒' },
      ],
    });
  }

  if (kChoiceTypes.has(type)) {
    // alpha 为 0 表示跟随文字颜色：色板只能给中性色，说明要写在字段下方，否则看着像被设成了黑。
    fields.push({
      kind: 'color',
      key: 'glyph',
      label: L('glyph'),
      value: rgbToHex(c.glyphColor),
      hint: c.glyphColor[3] ? '' : '全透明表示跟随文字颜色',
    });
  }

  fields.push({ kind: 'heading', key: '', label: '行为' });
  fields.push({ kind: 'toggle', key: 'readonly', label: L('readonly'), value: c.readonly });
  fields.push({ kind: 'toggle', key: 'required', label: L('required'), value: c.required });
  fields.push({
    kind: 'text',
    key: 'key',
    label: L('key'),
    value: c.key,
    placeholder: '同编码的几处显示是同一个字段',
  });

  return { index: c.index, type, title: `控件属性（${type}）`, fields };
}

/**
 * 表格的属性表单：表 / 行 / 格三层记录放进同一张。
 *
 * 三层写在一张里是有意的：它们对用户是同一件事（这张表长什么样），分成三张就要先进对的门。
 *   宽度与行高的"自动"在面板上写 0 —— 记录里的 -1 不该露给人看。
 * @param st 表格现状
 * @param opts 标签替换表
 * @returns 字段表与面板标题；不在表格里时 inTable 为假、字段为空
 */
export function tableFormOf(st: TableState, opts?: FormOptions): TableForm {
  if (!st.inTable) return { inTable: false, title: '表格属性', fields: [] };

  const L = (key: string): string => labelOf(key, opts);
  const widthNow = st.widthPt > 0 ? st.widthPt : 0;
  const heightNow = st.heightPt > 0 ? st.heightPt : 0;
  const merged = st.rowSpan > 1 || st.colSpan > 1;

  const fields: FormField[] = [
    { kind: 'heading', key: '', label: '表格' },
    {
      kind: 'number',
      key: 'tableWidth',
      label: L('tableWidth'),
      value: Math.round(widthNow * 100) / 100,
      unit: '磅（0 = 自动）',
      min: 0,
      step: 6,
    },
    {
      kind: 'select',
      key: 'tableAlign',
      label: L('tableAlign'),
      value: st.align,
      options: [
        { value: 'start', text: '左' },
        { value: 'center', text: '居中' },
        { value: 'end', text: '右' },
      ],
    },
    { kind: 'heading', key: '', label: `第 ${st.row + 1} 行` },
    { kind: 'toggle', key: 'rowHeader', label: L('rowHeader'), value: st.rowHeader },
    { kind: 'toggle', key: 'rowSplit', label: L('rowSplit'), value: st.rowSplit },
    { kind: 'toggle', key: 'rowBreak', label: L('rowBreak'), value: st.rowBreak },
    {
      kind: 'number',
      key: 'rowHeight',
      label: L('rowHeight'),
      value: Math.round(heightNow * 100) / 100,
      unit: '磅（0 = 按内容）',
      min: 0,
      step: 6,
    },
    {
      kind: 'select',
      key: 'rowRule',
      label: L('rowRule'),
      value: st.heightRule,
      options: [
        { value: 'auto', text: '按内容' },
        { value: 'atLeast', text: '不小于' },
        { value: 'exact', text: '固定' },
      ],
    },
    { kind: 'heading', key: '', label: merged ? '单元格（已合并）' : '单元格' },
    {
      kind: 'select',
      key: 'cellVAlign',
      label: L('cellVAlign'),
      value: st.vAlign,
      options: [
        { value: 'top', text: '顶端' },
        { value: 'middle', text: '居中' },
        { value: 'bottom', text: '底端' },
      ],
    },
  ];

  return { inTable: true, title: `表格属性（${st.rows} × ${st.cols}）`, fields };
}

/**
 * 把磅折成厘米，界面上的数保留两位。
 * @param pt 磅
 * @returns 厘米
 */
function cmOf(pt: number): number {
  return Math.round((pt / kCmPt) * 100) / 100;
}

/**
 * 纸张候选的文字：规格名加两个尺寸，尺寸从 PAPER_PT 现算，不另抄一份。
 * @param paper 固定规格
 * @returns 选项文字
 */
function paperText(paper: Exclude<PaperSize, 'Custom'>): string {
  const s = PAPER_PT[paper];
  const w = Math.round((s.w / kCmPt) * 10) / 10;
  const h = Math.round((s.h / kCmPt) * 10) / 10;
  return `${paper}（${w} × ${h} 厘米）`;
}

/**
 * 页面设置的属性表单。
 *
 * 边距与宽高按厘米给：人按厘米想页边距。宽高两项在纸张不是自定义时带 disabled ——
 *   固定规格的尺寸是规格本身的一部分，不该看起来可以改。
 * @param page 当前页面设置
 * @param opts 标签替换表
 * @returns 字段表与面板标题
 */
export function pageSetupFormOf(page: PageSetup, opts?: FormOptions): PageSetupForm {
  const L = (key: string): string => labelOf(key, opts);
  const custom = page.paper === 'Custom';
  // 自定义宽高按纵向口径存；别的规格取规格表里的那一份，切过去就是它的尺寸。
  const fixed = page.paper as Exclude<PaperSize, 'Custom'>;
  const portrait = custom ? (page.custom ?? PAPER_PT.A4) : PAPER_PT[fixed];

  const fields: FormField[] = [
    { kind: 'heading', key: '', label: '纸张' },
    {
      kind: 'select',
      key: 'paper',
      label: L('paper'),
      value: page.paper,
      options: [
        ...(Object.keys(PAPER_PT) as Array<Exclude<PaperSize, 'Custom'>>).map((name) => ({
          value: name,
          text: paperText(name),
        })),
        { value: 'Custom', text: '自定义' },
      ],
    },
    {
      kind: 'select',
      key: 'orient',
      label: L('orient'),
      value: page.landscape ? 'landscape' : 'portrait',
      options: [
        { value: 'portrait', text: '纵向' },
        { value: 'landscape', text: '横向' },
      ],
    },
    {
      kind: 'number',
      key: 'width',
      label: L('width'),
      value: cmOf(portrait.w),
      unit: '厘米',
      min: 0,
      step: 0.1,
      disabled: !custom,
    },
    {
      kind: 'number',
      key: 'height',
      label: L('height'),
      value: cmOf(portrait.h),
      unit: '厘米',
      min: 0,
      step: 0.1,
      disabled: !custom,
    },
    { kind: 'heading', key: '', label: '页边距' },
    {
      kind: 'number',
      key: 'marginTop',
      label: L('marginTop'),
      value: cmOf(page.margins.top),
      unit: '厘米',
      min: 0,
      step: 0.1,
    },
    {
      kind: 'number',
      key: 'marginBottom',
      label: L('marginBottom'),
      value: cmOf(page.margins.bottom),
      unit: '厘米',
      min: 0,
      step: 0.1,
    },
    {
      kind: 'number',
      key: 'marginLeft',
      label: L('marginLeft'),
      value: cmOf(page.margins.left),
      unit: '厘米',
      min: 0,
      step: 0.1,
    },
    {
      kind: 'number',
      key: 'marginRight',
      label: L('marginRight'),
      value: cmOf(page.margins.right),
      unit: '厘米',
      min: 0,
      step: 0.1,
    },
  ];

  return { title: '页面设置', fields };
}

/**
 * 一次控件改动：定义与运行态开关一包，内容单独一项。
 */
export interface ControlWrite {
  /**
   * 要给 updateControl 的项；空对象表示定义与开关都没动。
   */
  update: ControlUpdateOptions;

  /**
   * 新的内容或值；null 表示不改。
   */
  value: string | null;
}

/**
 * 把面板值折成一次控件改动，只留真的变了的项。
 *
 * 键的白名单取字段表：不认识的键一律不读，宿主把一份值对象喂错类型也不会被当成改定义。
 * @param c 控件现状（与造字段表时同一份）
 * @param values 面板读出来的值
 * @returns 要提交的项
 */
export function controlWriteOf(c: ControlSummary, values: FormValues): ControlWrite {
  const keys = new Set(controlFormOf(c).fields.map((f) => f.key));
  const update: ControlUpdateOptions = {};
  let value: string | null = null;

  /**
   * 读一个文本项。
   * @param key 字段键
   * @returns 字符串，或 null（这项不存在/类型不对）
   */
  const text = (key: string): string | null => {
    const v = keys.has(key) ? values[key] : undefined;
    return typeof v === 'string' ? v : null;
  };

  /**
   * 读一个开关项。
   * @param key 字段键
   * @returns 布尔，或 null（这项不存在/类型不对）
   */
  const flag = (key: string): boolean | null => {
    const v = keys.has(key) ? values[key] : undefined;
    return typeof v === 'boolean' ? v : null;
  };

  const label = text('label');
  if (label !== null && label !== c.label) update.label = label;

  const group = text('group');
  if (group !== null && group !== c.group) update.group = group;

  const key = text('key');
  if (key !== null && key !== c.key) update.key = key;

  const format = text('format');
  if (format !== null && format !== c.format) update.format = format;

  const precision = text('precision');
  if (precision !== null) {
    const next = precisionUnits(precision);
    if (next.join(',') !== c.dateUnits.join(',')) update.dateUnits = next;
  }

  const options = text('options');
  if (options !== null) {
    const next = textToOptions(options);
    if (optionsToText(next) !== optionsToText(c.options)) update.options = next;
  }

  const free = flag('free');
  if (free !== null && free !== c.freeInput) update.freeInput = free;

  const glyph = text('glyph');
  if (glyph !== null && rgbToHex(c.glyphColor) !== glyph) update.glyphColor = hexToRgba(glyph);

  const readonly = flag('readonly');
  if (readonly !== null && readonly !== c.readonly) update.readonly = readonly;

  const required = flag('required');
  if (required !== null && required !== c.required) update.required = required;

  const content = text('value');
  if (content !== null) {
    if (c.type === 'date') {
      // 日期控件的值在记录上、内容只是它的投影，所以比的是"裁到精度的值"而不是内容 ——
      //   拿投影文字比会永远判成改动，一次"确定"平白多出一步撤销。
      const { inputType } = dateInputOf(c.dateUnits);
      const cur = isoPartOf(c.value.text, inputType, c.dateUnits.includes('second'));
      if (content !== cur) value = isoJoinOf(c.value.text, content, inputType);
    } else if (content !== c.text) {
      value = content;
    }
  }

  return { update, value };
}

/**
 * 一次表格改动：三层各一包，空对象表示那一层没动。
 */
export interface TableWrite {
  /**
   * 表格层（宽度与水平位置）。
   */
  table: TableOptions;

  /**
   * 行层（标题行、跨页断行、行前分页、行高与规则）。
   */
  row: TableRowOptions;

  /**
   * 格层（垂直对齐）。
   */
  cell: TableCellOptions;
}

/**
 * 把面板值折成一次表格改动，只留真的变了的层。
 *
 * 一层里改了几项也只提交一次：一次"确定"因此最多三小步撤销，与用户按了几下无关。
 * @param st 表格现状（与造字段表时同一份）
 * @param values 面板读出来的值
 * @returns 要提交的三层
 */
export function tableWriteOf(st: TableState, values: FormValues): TableWrite {
  const write: TableWrite = { table: {}, row: {}, cell: {} };
  if (!st.inTable) return write;

  const widthNow = st.widthPt > 0 ? st.widthPt : 0;
  const heightNow = st.heightPt > 0 ? st.heightPt : 0;

  /**
   * 读一个数值项，负数按 0 处理（界面上 0 就是"自动"）。
   * @param key 字段键
   * @returns 数值，或 NaN（这项不存在/不是数）
   */
  const num = (key: string): number => {
    const v = values[key];
    if (typeof v === 'number') return Math.max(0, v);
    if (typeof v === 'string' && v.trim() !== '') return Math.max(0, Number(v));
    return Number.NaN;
  };

  /**
   * 读一个文本项。
   * @param key 字段键
   * @returns 字符串，或 null
   */
  const text = (key: string): string | null => {
    const v = values[key];
    return typeof v === 'string' ? v : null;
  };

  const width = num('tableWidth');
  if (Number.isFinite(width) && width !== widthNow) write.table.widthPt = width;

  const align = text('tableAlign');
  if (align !== null && align !== st.align) {
    write.table.align = align as TableOptions['align'];
  }

  if (values['rowHeader'] !== undefined && !!values['rowHeader'] !== st.rowHeader) {
    write.row.header = !!values['rowHeader'];
  }
  if (values['rowSplit'] !== undefined && !!values['rowSplit'] !== st.rowSplit) {
    write.row.split = !!values['rowSplit'];
  }
  if (values['rowBreak'] !== undefined && !!values['rowBreak'] !== st.rowBreak) {
    write.row.breakBefore = !!values['rowBreak'];
  }

  const height = num('rowHeight');
  if (Number.isFinite(height) && height !== heightNow) write.row.heightPt = height;

  const rule = text('rowRule');
  if (rule !== null && rule !== st.heightRule) {
    write.row.heightRule = rule as TableRowOptions['heightRule'];
  }

  const vAlign = text('cellVAlign');
  if (vAlign !== null && vAlign !== st.vAlign) {
    write.cell.vAlign = vAlign as TableCellOptions['vAlign'];
  }

  return write;
}

/**
 * 把面板值折成一份页面设置，只留真的改了的项。
 *
 * 不认识的纸张名与读不出的数字一律按现状处理：一份面板值不该把版面带到未定义的状态。
 * @param page 当前页面设置
 * @param values 面板读出来的值
 * @returns 新的页面设置；与现状相同时为 null
 */
export function pageSetupWriteOf(page: PageSetup, values: FormValues): PageSetup | null {
  /**
   * 读一个数值项。
   * @param key 字段键
   * @param fallback 读不出时用它
   * @returns 数值
   */
  const num = (key: string, fallback: number): number => {
    const v = values[key];
    const n = typeof v === 'number' ? v : typeof v === 'string' ? Number(v) : Number.NaN;
    return Number.isFinite(n) && n >= 0 ? n * kCmPt : fallback;
  };

  const name = values['paper'];
  if (typeof name === 'string' && name !== 'Custom' && !(name in PAPER_PT)) return null;
  const paper = (typeof name === 'string' ? name : page.paper) as PaperSize;

  const orient = values['orient'];
  const landscape = typeof orient === 'string' ? orient === 'landscape' : page.landscape;

  // 自定义宽高在一份记录里是纵向口径；固定规格时把旧值留着，切回去不用重输。
  const custom =
    paper === 'Custom'
      ? { w: num('width', page.custom?.w ?? PAPER_PT.A4.w), h: num('height', page.custom?.h ?? PAPER_PT.A4.h) }
      : page.custom;

  const margins: PageMargins = {
    top: num('marginTop', page.margins.top),
    bottom: num('marginBottom', page.margins.bottom),
    left: num('marginLeft', page.margins.left),
    right: num('marginRight', page.margins.right),
  };

  const sameMargins =
    Math.abs(margins.top - page.margins.top) < 0.01 &&
    Math.abs(margins.bottom - page.margins.bottom) < 0.01 &&
    Math.abs(margins.left - page.margins.left) < 0.01 &&
    Math.abs(margins.right - page.margins.right) < 0.01;
  const sameCustom =
    (custom === undefined && page.custom === undefined) ||
    (!!custom &&
      !!page.custom &&
      Math.abs(custom.w - page.custom.w) < 0.01 &&
      Math.abs(custom.h - page.custom.h) < 0.01);
  if (paper === page.paper && landscape === page.landscape && sameMargins && sameCustom) return null;

  return { paper, landscape, ...(custom ? { custom } : {}), margins };
}
