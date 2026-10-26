// 属性面板：一份字段描述 → 一块表单 + 确定 / 取消。
//
// 字段从哪来：库的 describeControl / describeTable 算，这里只认七种种类。加一种控件类型
//   不必动这一份 —— 字段表变了，渲染器照旧。
//
// 取值与下发分开：面板只负责"把字段变成控件、把控件读成一份值"，怎么落到文档由调用方给的
//   apply 决定（通常是 applyControlForm / applyTableForm，只提交真的变了的那些项）。
import type { FormField, FormValues } from '../../src/index';
import { createSelect } from './select';

/**
 * 面板参数。
 */
export interface PropsPanelOptions {
  /**
   * 面板标题。
   */
  title: string;

  /**
   * 字段表（库给的描述）。
   */
  fields: FormField[];

  /**
   * 确定按钮的文字。默认"确定"。
   */
  applyLabel?: string;

  /**
   * 按确定之后做什么（读到的值已经按字段键收好）。
   * @param values 面板里的值
   */
  apply(values: FormValues): void | Promise<void>;

  /**
   * 某个字段的值变了。条件字段靠它开合（页面设置的宽高只在自定义纸张下可用）。
   * @param key 变了的字段键
   * @param values 此刻整张面板的值
   */
  onChange?(key: string, values: FormValues): void;

  /**
   * 收工：面板该关掉了。
   */
  dismiss(): void;
}

/**
 * 一块属性面板。
 */
export interface PropsPanel {
  /**
   * 面板根元素。
   */
  el: HTMLElement;

  /**
   * 标题栏（拖拽把手交给浮层用）。
   */
  bar: HTMLElement;

  /**
   * 重读一遍值（面板每次打开都是新的一份，这里只为与其它组件同形）。
   * @param values 新的值；不给就保持现状
   */
  update(values?: FormValues): void;

  /**
   * 现在的值。
   * @returns 按字段键收好的一份
   */
  values(): FormValues;

  /**
   * 单独改一个字段的可用性。
   * @param key 字段键
   * @param off 真 = 置灰
   */
  disable(key: string, off: boolean): void;
}

/**
 * 造一块属性面板。
 * @param o 参数
 * @returns 面板
 */
export function createPropsPanel(o: PropsPanelOptions): PropsPanel {
  const el = document.createElement('div');
  el.className = 'heditor-props';

  // 标题栏兼拖动把手：整块面板靠它挪位置（定位那条线在 layer 的 dragBy 里）。
  const bar = document.createElement('div');
  bar.className = 'heditor-props__bar';
  const title = document.createElement('span');
  title.className = 'heditor-props__title';
  title.textContent = o.title;
  const shut = document.createElement('button');
  shut.type = 'button';
  shut.className = 'heditor-props__close';
  shut.dataset.act = 'prop-close';
  shut.title = '关闭';
  shut.setAttribute('aria-label', '关闭');
  shut.textContent = '\u00d7';
  shut.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    ev.stopPropagation();
    o.dismiss();
  });
  bar.append(title, shut);
  el.appendChild(bar);

  const body = document.createElement('div');
  body.className = 'heditor-props__body';
  el.appendChild(body);

  /**
   * 每个字段的读取函数（按字段键收好，取值时统一调）。
   */
  const readers = new Map<string, () => string | number | boolean>();

  /**
   * 每个字段的回填函数（update 用）。
   */
  const writers = new Map<string, (v: FormValues) => void>();

  /**
   * 每个字段的控件（disable 用）。
   */
  const controls = new Map<string, HTMLInputElement | HTMLTextAreaElement>();

  /**
   * 自定义控件的置灰入口：它们不是表单元素，没有 disabled 这个属性可设。
   */
  const customs = new Map<string, (off: boolean) => void>();

  /**
   * 记下这个字段的控件，并把"值变了"转给宿主。
   * @param key 字段键
   * @param control 控件元素
   */
  const arm = (key: string, control: HTMLInputElement | HTMLTextAreaElement): void => {
    controls.set(key, control);
    const fire = (): void => {
      if (o.onChange) o.onChange(key, values());
    };
    control.addEventListener('input', fire);
    control.addEventListener('change', fire);
  };

  /**
   * 在字段下方挂一句说明。说明要独占一行，所以这一行也跟着允许折行。
   * @param row 这一行
   * @param text 说明
   */
  const addHint = (row: HTMLElement, text: string): void => {
    if (!text) return;
    row.classList.add('heditor-props__row--wide');
    const hint = document.createElement('div');
    hint.className = 'heditor-props__hint';
    hint.textContent = text;
    row.appendChild(hint);
  };

  for (const f of o.fields) {
    if (f.kind === 'heading') {
      const h = document.createElement('div');
      h.className = 'heditor-props__group';
      h.textContent = f.label;
      body.appendChild(h);
      continue;
    }

    const row = document.createElement('label');
    row.className = 'heditor-props__row';
    const text = document.createElement('span');
    text.className = 'heditor-props__label';
    text.textContent = f.label;
    row.appendChild(text);

    if (f.kind === 'toggle') {
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.className = 'heditor-props__check';
      box.checked = f.value === true;
      box.disabled = !!f.disabled;
      box.dataset.act = `prop-${f.key}`;
      row.appendChild(box);
      arm(f.key, box);
      readers.set(f.key, () => box.checked);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        box.checked = typeof next === 'boolean' ? next : box.checked;
      });
    } else if (f.kind === 'select') {
      const sel = createSelect({
        options: (f.options ?? []).map((x) => ({ value: x.value, text: x.text })),
        value: String(f.value ?? ''),
        act: `prop-${f.key}`,
        title: f.label,
        pick: () => {
          // 选中就算一次改动：条件字段（纸张 → 宽高）靠它开合。
          if (o.onChange) o.onChange(f.key, values());
        },
      });
      sel.setDisabled(!!f.disabled);
      row.appendChild(sel.el);
      customs.set(f.key, (off) => sel.setDisabled(off));
      readers.set(f.key, () => sel.value());
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string' || typeof next === 'number') sel.setValue(String(next));
      });
    } else if (f.kind === 'number') {
      const input = document.createElement('input');
      input.type = 'number';
      input.className = 'heditor-props__input';
      input.dataset.act = `prop-${f.key}`;
      if (f.min !== undefined) input.min = String(f.min);
      if (f.max !== undefined) input.max = String(f.max);
      input.step = String(f.step === undefined ? 1 : f.step);
      input.value = String(f.value ?? '');
      input.disabled = !!f.disabled;
      row.appendChild(input);
      if (f.unit) {
        const unit = document.createElement('span');
        unit.className = 'heditor-props__unit';
        unit.textContent = f.unit;
        row.appendChild(unit);
      }
      arm(f.key, input);
      readers.set(f.key, () => (input.value === '' ? '' : Number(input.value)));
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'number') input.value = String(next);
      });
    } else if (f.kind === 'datetime') {
      // 日期与时间用原生控件：无障碍、locale、移动端的系统选择器都是白拿的。
      //   值已经按 inputType 裁好，直接塞就行（长度不对浏览器会当空串）。
      const input = document.createElement('input');
      input.type = f.inputType ?? 'date';
      input.className = 'heditor-props__input';
      input.dataset.act = `prop-${f.key}`;
      input.value = String(f.value ?? '');
      if (f.step !== undefined) input.step = String(f.step);
      input.disabled = !!f.disabled;
      row.appendChild(input);
      arm(f.key, input);
      readers.set(f.key, () => input.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string') input.value = next;
      });
    } else if (f.kind === 'color') {
      const input = document.createElement('input');
      input.type = 'color';
      input.className = 'heditor-props__color';
      input.dataset.act = `prop-${f.key}`;
      input.value = String(f.value ?? '#000000');
      row.appendChild(input);
      arm(f.key, input);
      readers.set(f.key, () => input.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string') input.value = next;
      });
      addHint(row, f.hint ?? '');
    } else if (f.kind === 'lines') {
      const input = document.createElement('textarea');
      input.className = 'heditor-props__lines';
      input.dataset.act = `prop-${f.key}`;
      input.rows = f.rows === undefined ? 4 : f.rows;
      input.value = String(f.value ?? '');
      input.spellcheck = false;
      // 多行字段独占一行：选项表要的是宽度，挤在标签右边只剩一条缝。
      row.classList.add('heditor-props__row--wide');
      row.appendChild(input);
      addHint(row, f.hint ?? '');
      arm(f.key, input);
      readers.set(f.key, () => input.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string') input.value = next;
      });
    } else {
      const input = document.createElement('input');
      input.type = 'text';
      input.className = 'heditor-props__input';
      input.dataset.act = `prop-${f.key}`;
      input.value = String(f.value ?? '');
      input.disabled = !!f.disabled;
      if (f.placeholder) input.placeholder = f.placeholder;
      row.appendChild(input);
      arm(f.key, input);
      readers.set(f.key, () => input.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string') input.value = next;
      });
    }

    body.appendChild(row);
  }

  const foot = document.createElement('div');
  foot.className = 'heditor-props__foot';

  const cancel = document.createElement('button');
  cancel.type = 'button';
  cancel.className = 'heditor-props__cancel';
  cancel.dataset.act = 'prop-cancel';
  cancel.textContent = '取消';
  cancel.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    o.dismiss();
  });

  const apply = document.createElement('button');
  apply.type = 'button';
  apply.className = 'heditor-props__apply';
  apply.dataset.act = 'prop-apply';
  apply.textContent = o.applyLabel ?? '确定';
  // 按下就应用：等 click 会被外面的 pointerdown（关闭浮层）抢在前面。
  apply.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    void o.apply(values());
    o.dismiss();
  });

  foot.append(cancel, apply);
  el.appendChild(foot);

  /**
   * 读一遍所有字段。
   * @returns 按字段键收好的一份
   */
  function values(): FormValues {
    const out: FormValues = {};
    for (const [key, read] of readers) out[key] = read();
    return out;
  }

  return {
    el,
    bar,
    update(next?: FormValues): void {
      if (!next) return;
      for (const [key, write] of writers) write(next);
    },
    values,
    disable(key: string, off: boolean): void {
      const c = controls.get(key);
      if (c) c.disabled = off;
      const custom = customs.get(key);
      if (custom) custom(off);
    },
  };
}
