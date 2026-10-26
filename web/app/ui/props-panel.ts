// 属性面板：一份字段描述 → 一块表单 + 确定 / 取消。
//
// 为什么单独做一件：表格（表 / 行 / 格三层）与控件（六种类型）的属性面板形状完全一样 ——
//   一串"标签 + 控件"，底下两个按钮。各写一份的话，字段类型（开关、下拉、数值、颜色、
//   多行选项表）的渲染要抄六遍，抄歪一次就是"这一张面板的数字框不小了"。
//
// 取值与下发分开：面板只负责"把字段变成控件、把控件读成一份值"，怎么落到文档由调用方给
//   的 apply 决定。表格那三组属性因此可以一次提交、也可以分别提交，面板不必知道。

/**
 * 一个字段。
 */
export type PropField =
  | {
      /**
       * 分组标题（不是字段，只用来把面板分段）。
       */
      kind: 'heading';

      /**
       * 标题文字。
       */
      text: string;
    }
  | {
      /**
       * 下拉框。
       */
      kind: 'select';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 左列的标签。
       */
      label: string;

      /**
       * 当前值。
       */
      value: string;

      /**
       * 候选。
       */
      options: Array<{ value: string; text: string }>;

      /**
       * 置灰。
       */
      disabled?: boolean;
    }
  | {
      /**
       * 开关（复选框）。
       */
      kind: 'toggle';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 右侧的文字（开关本身占左列）。
       */
      label: string;

      /**
       * 当前值。
       */
      value: boolean;

      /**
       * 置灰。
       */
      disabled?: boolean;
    }
  | {
      /**
       * 数值框。
       */
      kind: 'number';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 左列的标签。
       */
      label: string;

      /**
       * 当前值。
       */
      value: number;

      /**
       * 单位（显示在数字右边）。
       */
      unit?: string;

      /**
       * 下限。
       */
      min?: number;

      /**
       * 上限。
       */
      max?: number;

      /**
       * 步长。
       */
      step?: number;

      /**
       * 置灰。
       */
      disabled?: boolean;
    }
  | {
      /**
       * 单行文本。
       */
      kind: 'text';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 左列的标签。
       */
      label: string;

      /**
       * 当前值。
       */
      value: string;

      /**
       * 空着时显示的提示。
       */
      placeholder?: string;

      /**
       * 置灰。
       */
      disabled?: boolean;
    }
  | {
      /**
       * 多行文本（选项表这类"一行一项"的输入）。
       */
      kind: 'lines';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 左列的标签。
       */
      label: string;

      /**
       * 当前值（按换行分项）。
       */
      value: string;

      /**
       * 下方的说明（讲清每行的格式）。
       */
      hint?: string;

      /**
       * 显示几行。
       */
      rows?: number;
    }
  | {
      /**
       * 颜色。
       */
      kind: 'color';

      /**
       * 取值用的键。
       */
      key: string;

      /**
       * 左列的标签。
       */
      label: string;

      /**
       * 当前值（#rrggbb）。
       */
      value: string;
    };

/**
 * 面板读出来的值。
 */
export type PropValues = Record<string, string | number | boolean>;

/**
 * 面板参数。
 */
export interface PropsPanelOptions {
  /**
   * 面板标题。
   */
  title: string;

  /**
   * 字段表。
   */
  fields: PropField[];

  /**
   * 确定按钮的文字。默认"确定"。
   */
  applyLabel?: string;

  /**
   * 按确定之后做什么（读到的值已经按字段键收好）。
   * @param values 面板里的值
   */
  apply(values: PropValues): void | Promise<void>;

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
   * 重读一遍值（面板每次打开都是新的一份，这里只为与其它组件同形）。
   * @param values 新的值；不给就保持现状
   */
  update(values?: PropValues): void;

  /**
   * 现在的值。
   * @returns 按字段键收好的一份
   */
  values(): PropValues;
}

/**
 * 造一块属性面板。
 * @param o 参数
 * @returns 面板
 */
export function createPropsPanel(o: PropsPanelOptions): PropsPanel {
  const el = document.createElement('div');
  el.className = 'heditor-props';

  const head = document.createElement('div');
  head.className = 'heditor-props__head';
  head.textContent = o.title;
  el.appendChild(head);

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
  const writers = new Map<string, (v: PropValues) => void>();

  for (const f of o.fields) {
    if (f.kind === 'heading') {
      const h = document.createElement('div');
      h.className = 'heditor-props__group';
      h.textContent = f.text;
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
      box.checked = f.value;
      box.disabled = !!f.disabled;
      box.dataset.act = `prop-${f.key}`;
      row.appendChild(box);
      readers.set(f.key, () => box.checked);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        box.checked = typeof next === 'boolean' ? next : box.checked;
      });
    } else if (f.kind === 'select') {
      const sel = document.createElement('select');
      sel.className = 'heditor-props__select';
      sel.disabled = !!f.disabled;
      sel.dataset.act = `prop-${f.key}`;
      for (const opt of f.options) {
        const item = document.createElement('option');
        item.value = opt.value;
        item.textContent = opt.text;
        sel.appendChild(item);
      }
      sel.value = f.value;
      row.appendChild(sel);
      readers.set(f.key, () => sel.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string' || typeof next === 'number') sel.value = String(next);
      });
    } else if (f.kind === 'number') {
      const input = document.createElement('input');
      input.type = 'number';
      input.className = 'heditor-props__input';
      input.dataset.act = `prop-${f.key}`;
      if (f.min !== undefined) input.min = String(f.min);
      if (f.max !== undefined) input.max = String(f.max);
      input.step = String(f.step === undefined ? 1 : f.step);
      input.value = String(f.value);
      input.disabled = !!f.disabled;
      row.appendChild(input);
      if (f.unit) {
        const unit = document.createElement('span');
        unit.className = 'heditor-props__unit';
        unit.textContent = f.unit;
        row.appendChild(unit);
      }
      readers.set(f.key, () => (input.value === '' ? '' : Number(input.value)));
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'number') input.value = String(next);
      });
    } else if (f.kind === 'color') {
      const input = document.createElement('input');
      input.type = 'color';
      input.className = 'heditor-props__color';
      input.dataset.act = `prop-${f.key}`;
      input.value = f.value;
      row.appendChild(input);
      readers.set(f.key, () => input.value);
      writers.set(f.key, (v) => {
        const next = v[f.key];
        if (typeof next === 'string') input.value = next;
      });
    } else if (f.kind === 'lines') {
      const input = document.createElement('textarea');
      input.className = 'heditor-props__lines';
      input.dataset.act = `prop-${f.key}`;
      input.rows = f.rows === undefined ? 4 : f.rows;
      input.value = f.value;
      input.spellcheck = false;
      // 多行字段独占一行：选项表要的是宽度，挤在标签右边只剩一条缝。
      row.classList.add('heditor-props__row--wide');
      row.appendChild(input);
      if (f.hint) {
        const hint = document.createElement('div');
        hint.className = 'heditor-props__hint';
        hint.textContent = f.hint;
        row.appendChild(hint);
      }
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
      input.value = f.value;
      input.disabled = !!f.disabled;
      if (f.placeholder) input.placeholder = f.placeholder;
      row.appendChild(input);
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
  function values(): PropValues {
    const out: PropValues = {};
    for (const [key, read] of readers) out[key] = read();
    return out;
  }

  return {
    el,
    update(next?: PropValues): void {
      if (!next) return;
      for (const [key, write] of writers) write(next);
    },
    values,
  };
}
