// 下拉：触发器 + 一块可搜索的候选列表。
//
// 为什么不用原生 <select>：候选一多（字体清单、页边距档位）就没法找 —— 原生 dropdown 只能
//   一路滚，而且各浏览器的外观差得很远、宽度跟着最长一项走。这里自己画，得到三件事：
//   搜索框、键盘走向、以及与本页其余浮层同一套外观。
//
// 浮层只有一份（模块级单例）：同一时刻只可能开一个下拉，每个实例各持一块会让功能区里
//   十几个下拉各留一个空 div。
import { createLayer, type Layer } from './layer';

/**
 * 一项候选。
 */
export interface SelectOption {
  /**
   * 交回去的值。
   */
  value: string;

  /**
   * 显示的字。
   */
  text: string;
}

/**
 * 一段下拉会话：谁开的、选中之后做什么、关掉之后做什么。
 */
interface Session {
  /**
   * 触发器元素（浮层锚在它下面）。
   */
  trigger: HTMLElement;

  /**
   * 候选表。
   */
  options: SelectOption[];

  /**
   * 当前值。
   */
  value: string;

  /**
   * 给不给搜索框；不给按候选数决定。
   */
  searchable?: boolean;

  /**
   * 选中之后回调。
   * @param value 新值
   */
  pick(value: string): void;

  /**
   * 关掉之后回调（没选中也算）。
   */
  dismiss?(): void;
}

/**
 * 候选多到这个数以上才给搜索框：三项的下拉加一个输入框只是多一层操作。
 */
const kSearchFrom = 8;

/**
 * 唯一的那块浮层。
 */
let pop: Layer | null = null;

/**
 * 此刻开着的那一段会话；没开为 null。
 */
let current: Session | null = null;

/**
 * 造一个下拉。
 * @param spec 初始候选与当前值
 * @returns 触发器与其操作入口
 */
export function createSelect(spec: {
  options: SelectOption[];
  value: string;
  searchable?: boolean;
  title?: string;
  act?: string;
  pick(value: string): void;
}): {
  el: HTMLButtonElement;
  setOptions(list: SelectOption[]): void;
  setValue(v: string): void;
  value(): string;
  setDisabled(off: boolean): void;
} {
  let options = spec.options.slice();
  let value = spec.value;
  let searchable = spec.searchable;

  const el = document.createElement('button');
  el.type = 'button';
  el.className = 'heditor-select';
  if (spec.act) el.dataset.act = spec.act;
  if (spec.title) el.title = spec.title;
  el.setAttribute('aria-haspopup', 'listbox');
  el.setAttribute('aria-expanded', 'false');

  /**
   * 触发器上显示的那个字。
   */
  const text = document.createElement('span');
  text.className = 'heditor-select__text';
  el.appendChild(text);

  /**
   * 右侧的小箭头（用 border 画，省一个图标资源）。
   */
  const caret = document.createElement('span');
  caret.className = 'heditor-select__caret';
  el.appendChild(caret);

  /**
   * 把当前值写到触发器上。
   */
  const paint = (): void => {
    const hit = options.find((o) => o.value === value);
    // 值不在候选里（别处存来的 15 磅、文档里的字体）时原样显示 —— 显示空白等于"没有回显"。
    text.textContent = hit ? hit.text : value;
    // 值也落在元素上：宿主与判据都要"显示的字"之外的那一份（字体显示名字、存的是索引）。
    el.dataset.value = value;
    el.classList.toggle('is-empty', !hit && !value);
  };
  paint();

  /**
   * 开浮层。
   * @param session 这一段会话
   */
  const open = (session: Session): void => {
    if (pop === null) {
      // 定位与阴影复用 popover 那一份，尺寸与内边距由 heditor-pop 覆盖。
      pop = createLayer('heditor-popover heditor-pop', () => {
        el.setAttribute('aria-expanded', 'false');
        const done = current;
        current = null;
        if (done && done.dismiss) done.dismiss();
      });
    }
    // 同一个触发器再点一次就是"收起"。
    if (pop.isOpen() && current !== null && current.trigger === session.trigger) {
      pop.close();
      return;
    }
    if (pop.isOpen()) pop.close();
    current = session;

    const withSearch = session.searchable ?? session.options.length >= kSearchFrom;
    const list = document.createElement('div');
    list.className = 'heditor-select__list';
    list.setAttribute('role', 'listbox');

    /**
     * 现成的候选（过滤之后的那一批）。
     */
    let shown = session.options.slice();

    /**
     * 按过滤词重建列表。
     * @param needle 过滤词（空 = 全给）
     */
    const fill = (needle: string): void => {
      const q = needle.trim().toLowerCase();
      shown = q
        ? session.options.filter(
            (o) => o.text.toLowerCase().includes(q) || o.value.toLowerCase().includes(q),
          )
        : session.options.slice();
      list.textContent = '';
      if (shown.length === 0) {
        const none = document.createElement('div');
        none.className = 'heditor-select__none';
        none.textContent = '没有匹配项';
        list.appendChild(none);
        return;
      }
      for (const o of shown) {
        const item = document.createElement('button');
        item.type = 'button';
        item.className = 'heditor-select__item';
        item.dataset.value = o.value;
        item.setAttribute('role', 'option');
        item.setAttribute('aria-selected', String(o.value === session.value));
        item.textContent = o.text;
        if (o.value === session.value) item.classList.add('is-on');
        item.addEventListener('pointerdown', (ev) => {
          // 按下就选：等 click 会被外面那层"点面板外关闭"抢在前面。
          ev.preventDefault();
          session.pick(o.value);
          if (pop !== null) pop.close();
        });
        list.appendChild(item);
      }
    };

    let needleInput: HTMLInputElement | null = null;
    if (withSearch) {
      needleInput = document.createElement('input');
      needleInput.type = 'text';
      needleInput.className = 'heditor-select__search';
      needleInput.placeholder = '搜索…';
      needleInput.spellcheck = false;
      needleInput.addEventListener('input', () => fill(needleInput === null ? '' : needleInput.value));
      pop.el.textContent = '';
      pop.el.appendChild(needleInput);
    } else if (pop !== null) {
      pop.el.textContent = '';
    }
    fill('');
    pop.el.appendChild(list);
    pop.openAt(session.trigger);
    el.setAttribute('aria-expanded', 'true');

    if (needleInput !== null) {
      needleInput.focus({ preventScroll: true });
    } else {
      // 键盘要能直接走项，所以焦点落在"当前项"上 —— 落到容器上按方向键没有接收者。
      const on = list.querySelector('.heditor-select__item.is-on');
      const first = on ?? list.querySelector('.heditor-select__item');
      if (first instanceof HTMLElement) first.focus({ preventScroll: true });
    }
  };

  /**
   * 记下新值、刷新回显、再通知调用方 —— 顺序不能反：回调里可能要读 value()。
   * @param next 新值
   */
  const commit = (next: string): void => {
    value = next;
    paint();
    spec.pick(next);
  };

  /**
   * 开这一段会话。
   */
  const popup = (): void => {
    if (el.disabled) return;
    open({ trigger: el, options, value, searchable, pick: commit });
  };

  el.addEventListener('pointerdown', (ev) => {
    ev.preventDefault();
    popup();
  });

  el.addEventListener('keydown', (ev) => {
    if (ev.key !== 'ArrowDown' && ev.key !== 'Enter' && ev.key !== ' ') return;
    ev.preventDefault();
    popup();
  });

  return {
    el,

    /**
     * 换候选（函数型候选重算之后走这里）。
     * @param next 新候选表
     */
    setOptions(next: SelectOption[]): void {
      options = next.slice();
      paint();
    },

    /**
     * 换当前值（回显）。
     * @param next 新值
     */
    setValue(next: string): void {
      value = next;
      paint();
    },

    value: () => value,

    /**
     * 置灰。
     * @param off 真 = 不可用
     */
    setDisabled(off: boolean): void {
      el.disabled = off;
    },
  };
}

/**
 * 把一份 { value, text } 表转成下拉要的形状。
 *
 * 只是给调用方省一层 map —— 两个字段名不一样的地方（库给 text、选项表给 label）在
 *   这里对齐，别让每个调用点各转一遍。
 * @param list 原本的候选
 * @returns 下拉用的候选
 */
export function selectOptions(
  list: Array<{ value: string; text?: string; label?: string }>,
): SelectOption[] {
  return list.map((o) => ({ value: o.value, text: o.text ?? o.label ?? o.value }));
}
