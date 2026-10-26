// 控件属性面板：按控件类型给不同的字段。
//
// 字段表按类型现算，面板本身不认识"下拉"这类概念 —— 它只认"标签 + 控件"。于是加一种类型
//   只需在 fieldsFor 里多一组字段，不必再写一块表单。
//
// 值走两条路：文本类的值就是内容（setControlValue），其余是定义（updateControl）。只读与必填
//   是运行时开关（setControlProp），与定义分开 —— 这也是引擎那边三个入口的分工。
import type { Component } from './types';
import type { ControlSummary, Editor, RGBA } from '../../src/index';
import { createPropsPanel, type PropField, type PropValues } from './props-panel';

/**
 * 面板参数。
 */
export interface ControlPropsOptions {
  /**
   * 打开面板那一刻的控件（form() 的那一项）。
   */
  control: ControlSummary;

  /**
   * 编辑器。
   */
  editor: Editor;

  /**
   * 关掉面板。
   */
  dismiss(): void;

  /**
   * 往状态栏写一句话。
   * @param text 要说的话
   */
  say(text: string): void;
}

/**
 * 选择类的四种类型。
 */
const kChoiceTypes = new Set(['radioButton', 'checkBox', 'radioGroup', 'checkBoxGroup']);

/**
 * 把函数式选项表读成字段值：一行一项，写成"值|标签"。
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
 * #rrggbb → 四通道。
 * @param hex 颜色串
 * @returns RGBA（不透明度 255）
 */
function hexToRgba(hex: string): RGBA {
  const v = hex.replace('#', '');
  const at = (i: number): number => Number.parseInt(v.slice(i, i + 2), 16) || 0;
  return [at(0), at(2), at(4), 255];
}

/**
 * 四通道 → #rrggbb。全透明（跟随文字）时给一个中性色当起点。
 * @param rgba 四通道
 * @returns 颜色串
 */
function rgbToHex(rgba: RGBA): string {
  if (!rgba[3]) return '#000000';
  const two = (n: number): string => Math.max(0, Math.min(255, n | 0)).toString(16).padStart(2, '0');
  return `#${two(rgba[0])}${two(rgba[1])}${two(rgba[2])}`;
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
function precisionUnits(precision: string): Array<'year' | 'month' | 'day' | 'hour' | 'minute' | 'second'> {
  if (precision === 'second') return ['year', 'month', 'day', 'hour', 'minute', 'second'];
  if (precision === 'minute') return ['year', 'month', 'day', 'hour', 'minute'];
  return ['year', 'month', 'day'];
}

/**
 * 按控件类型给字段表。
 * @param c 控件
 * @returns 字段
 */
function fieldsFor(c: ControlSummary): PropField[] {
  const fields: PropField[] = [];
  const type = c.type || 'text';

  if (type === 'text' || type === 'number') {
    fields.push({
      kind: 'text',
      key: 'value',
      label: '内容',
      value: c.text,
      placeholder: type === 'number' ? '只能填数字' : '',
    });
  } else if (type === 'radioButton' || type === 'checkBox') {
    fields.push({ kind: 'text', key: 'label', label: '标签', value: c.label });
    if (type === 'radioButton') {
      fields.push({
        kind: 'text',
        key: 'group',
        label: '互斥组',
        value: c.group,
        placeholder: '同组最多选一个；空 = 独立',
      });
    }
  } else if (type === 'radioGroup' || type === 'checkBoxGroup') {
    fields.push({
      kind: 'lines',
      key: 'options',
      label: '选项',
      value: optionsToText(c.options),
      hint: '一行一项，写成"值|标签"。改值会让原来的取值失效。',
      rows: 4,
    });
  } else if (type === 'dropdownList') {
    fields.push({
      kind: 'lines',
      key: 'options',
      label: '选项',
      value: optionsToText(c.options),
      hint: '一行一项，写成"值|标签"。',
      rows: 4,
    });
    fields.push({
      kind: 'toggle',
      key: 'free',
      label: '自由输入',
      value: c.freeInput,
    });
  } else if (type === 'date') {
    fields.push({
      kind: 'text',
      key: 'format',
      label: '格式',
      value: c.format,
      placeholder: 'yyyy年MM月dd日 HH时mm分ss秒',
    });
    fields.push({
      kind: 'select',
      key: 'precision',
      label: '可填到',
      value: datePrecision(c.dateUnits),
      options: [
        { value: 'day', text: '日期' },
        { value: 'minute', text: '到分' },
        { value: 'second', text: '到秒' },
      ],
    });
  }

  if (kChoiceTypes.has(type)) {
    // alpha 为 0 表示"跟随文字颜色"：这时色板给的是中性色，标签上要说清楚 ——
    //   不然用户看到黑色方块，以为记号被设成了黑色。
    fields.push({
      kind: 'color',
      key: 'glyph',
      label: c.glyphColor[3] ? '记号颜色' : '记号颜色（跟随文字）',
      value: rgbToHex(c.glyphColor),
    });
  }

  fields.push({ kind: 'heading', text: '行为' });
  fields.push({ kind: 'toggle', key: 'readonly', label: '只读', value: c.readonly });
  fields.push({ kind: 'toggle', key: 'required', label: '必填', value: c.required });
  fields.push({ kind: 'text', key: 'key', label: '业务编码', value: c.key, placeholder: '同编码的几处显示是同一个字段' });
  return fields;
}

/**
 * 造一块控件属性面板。
 * @param o 参数
 * @returns 组件
 */
export function createControlProps(o: ControlPropsOptions): Component {
  const c = o.control;
  const panel = createPropsPanel({
    title: `控件属性（${c.type || 'text'}）`,
    fields: fieldsFor(c),
    apply: (v: PropValues) => {
      const update: Record<string, unknown> = {};
      if (typeof v['label'] === 'string' && v['label'] !== c.label) update.label = v['label'];
      if (typeof v['group'] === 'string' && v['group'] !== c.group) update.group = v['group'];
      if (typeof v['key'] === 'string' && v['key'] !== c.key) update.key = v['key'];
      if (typeof v['format'] === 'string' && v['format'] !== c.format) update.format = v['format'];
      if (typeof v['precision'] === 'string') {
        const next = precisionUnits(v['precision']);
        if (next.join(',') !== c.dateUnits.join(',')) update.dateUnits = next;
      }
      if (typeof v['options'] === 'string') {
        const next = textToOptions(v['options']);
        const now = optionsToText(c.options);
        if (optionsToText(next) !== now) update.options = next;
      }
      if (typeof v['free'] === 'boolean' && v['free'] !== c.freeInput) update.freeInput = v['free'];
      if (typeof v['glyph'] === 'string') {
        const next = hexToRgba(v['glyph']);
        if (rgbToHex(c.glyphColor) !== v['glyph']) update.glyphColor = next;
      }

      const valueChanged = typeof v['value'] === 'string' && v['value'] !== c.text;
      const readonlyChanged = !!v['readonly'] !== c.readonly;
      const requiredChanged = !!v['required'] !== c.required;

      void (async () => {
        let changed = false;
        if (Object.keys(update).length > 0) {
          changed = (await o.editor.updateControl(c.index, update as never)) || changed;
        }
        if (valueChanged) {
          changed = (await o.editor.setControlValue(c.index, { text: String(v['value'] ?? '') })) || changed;
        }
        // 运行时开关的编号与引擎的 ControlProp 同序：1 只读、2 必填（0 隐藏、3 结构锁）。
        if (readonlyChanged) {
          changed = (await o.editor.setControlProp(c.index, 1, !!v['readonly'])) || changed;
        }
        if (requiredChanged) {
          changed = (await o.editor.setControlProp(c.index, 2, !!v['required'])) || changed;
        }
        o.say(changed ? '控件属性已更新' : '控件属性没有变化');
      })();
    },
    dismiss: o.dismiss,
  });

  return {
    el: panel.el,
    /**
     * 面板每次打开都是新的一份，这里只为与其它组件同形。
     */
    update(): void {
      panel.update();
    },
  };
}
