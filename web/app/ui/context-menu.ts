// 右键菜单：一份数据驱动的多级菜单（动作、分组标题、分隔线、子菜单、勾选与禁用）。
//
// 为什么不直接用 <select> 或一堆按钮：右键菜单的形状是"一层不够就再开一层"，而层级、
//   键盘走向、子菜单的弹出方向与视口夹取都要有自己的规则。做成组件之后，加一项菜单就是
//   往数组里写一条，不必再碰 DOM 与事件 —— 表格、控件、图片三份菜单因此只差数据。
//
// 键盘口径照原生菜单：上下移动、左右进出子菜单、回车执行、Esc 关掉整棵。鼠标悬停进子菜单
//   有一小段延迟（手一滑扫过带子菜单的那一项不该把它弹开）。
import { iconHtml } from './icons';
import { createLayer, type Layer } from './layer';

/**
 * 菜单里的一项。
 */
export type MenuEntry =
  | {
      /**
       * 分隔线。
       */
      kind: 'separator';
    }
  | {
      /**
       * 分组标题（不可点，只用于把菜单分段）。
       */
      kind: 'heading';

      /**
       * 标题文字。
       */
      label: string;
    }
  | {
      /**
       * 一条可执行的动作。
       */
      kind: 'action';

      /**
       * 菜单上显示的文字。
       */
      label: string;

      /**
       * 图标名（web/app/icons 下的文件名，不含扩展名）。
       */
      icon?: string;

      /**
       * 右侧的快捷键提示（只显示，不绑定按键）。
       */
      hint?: string;

      /**
       * 置灰：看得见、点不动。用它表示"这个动作现在不可用"，而不是把项藏掉。
       */
      disabled?: boolean;

      /**
       * 打勾（当前值、当前开关状态）。
       */
      checked?: boolean;

      /**
       * 选中之后做什么。
       */
      run(): void;
    }
  | {
      /**
       * 子菜单：鼠标移上去（或按右键）再开一层。
       */
      kind: 'submenu';

      /**
       * 菜单上显示的文字。
       */
      label: string;

      /**
       * 图标名。
       */
      icon?: string;

      /**
       * 置灰（连子菜单一起关着）。
       */
      disabled?: boolean;

      /**
       * 下一层的项。
       */
      items: MenuEntry[];
    };

/**
 * 一块右键菜单。
 */
export interface ContextMenu {
  /**
   * 菜单根元素（已挂在 body 上，关闭时 hidden）。
   */
  el: HTMLElement;

  /**
   * 在视口坐标处打开。
   * @param items 菜单项
   * @param at 客户端坐标（CSS 像素），通常是右键落点
   */
  open(items: MenuEntry[], at: { x: number; y: number }): void;

  /**
   * 关掉（幂等）。
   */
  close(): void;

  /**
   * 现在是否打开。
   * @returns 真 = 打开着
   */
  isOpen(): boolean;
}

/**
 * 鼠标扫过带子菜单的那一项之后，等多久才弹出下一层。
 */
const kSubmenuDelayMs = 120;

/**
 * 子菜单与触发它的那一行在纵向上的重叠量（磅）。
 *
 * 与样式里的 top 用同一个值：正数表示子菜单比那一行高出几个像素，看着像"从那一行长出去"。
 */
const kSubmenuOverlap = 5;

/**
 * 浮层与视口边缘之间至少留多少（CSS 像素）。
 */
const kViewportGap = 6;

/**
 * 造一块右键菜单。
 * @param onClose 关掉之后的回调（宿主用它把键盘还回编辑器）
 * @returns 菜单
 */
export function createContextMenu(onClose?: () => void): ContextMenu {
  // 关掉时两层都要收：layer 管浮层与关闭时机，这里管自己挂的键盘监听。
  //   只挂不收的话，Esc 关掉的那些菜单会各留一个死监听，开几十次就攒几十个。
  const layer: Layer = createLayer('heditor-menu', () => {
    document.removeEventListener('keydown', onKey, true);
    if (onClose) onClose();
  });
  const root = layer.el;
  root.setAttribute('role', 'menu');
  root.tabIndex = -1;

  /**
   * 亮着的那一串项，从最外层到最深那一层。
   *
   * 高亮不是"单独一项"：开着子菜单的那一项在老菜单里一直是选中态 —— 手移进二级之后
   *   父项仍然亮着，眼睛才看得出"这几项是一条路下来的"。最深那一项才是键盘当下的落点。
   */
  let chain: HTMLElement[] = [];

  /**
   * 键盘当下的落点（= 高亮链的最深一项）。
   */
  let active: HTMLElement | null = null;

  /**
   * 子菜单弹出的定时器（鼠标悬停延迟）。
   */
  let hoverTimer = 0;

  /**
   * 打开着的子菜单，从外到内（最后一层是最深的）。
   */
  const openStack: HTMLElement[] = [];

  /**
   * 造一项的图标。
   * @param name 图标名
   * @returns 元素
   */
  const iconOf = (name: string): HTMLElement => {
    const span = document.createElement('span');
    span.className = 'heditor-menu__icon';
    // 图标与工具条走同一份资源（按原文读进来的 SVG），图形里的 currentColor 跟着文字走。
    span.innerHTML = iconHtml(name);
    return span;
  };

  /**
   * 造一层的列表。
   * @param items 这一层的项
   * @param depth 第几层（0 = 最外层）
   * @returns 列表元素
   */
  const buildList = (items: MenuEntry[], depth: number): HTMLElement => {
    const list = document.createElement('div');
    list.className = 'heditor-menu__list';
    list.dataset.depth = String(depth);
    if (depth > 0) {
      list.hidden = true;
      list.classList.add('heditor-menu__list--sub');
    }

    for (const entry of items) {
      if (entry.kind === 'separator') {
        const sep = document.createElement('div');
        sep.className = 'heditor-menu__sep';
        sep.setAttribute('role', 'separator');
        list.appendChild(sep);
        continue;
      }
      if (entry.kind === 'heading') {
        const head = document.createElement('div');
        head.className = 'heditor-menu__heading';
        head.textContent = entry.label;
        list.appendChild(head);
        continue;
      }

      const row = document.createElement('div');
      row.className = 'heditor-menu__item';
      row.setAttribute('role', 'menuitem');
      row.tabIndex = -1;
      if (entry.disabled) {
        row.classList.add('heditor-menu__item--off');
        row.setAttribute('aria-disabled', 'true');
      }
      if (entry.kind === 'action') {
        if (entry.checked) {
          row.classList.add('heditor-menu__item--on');
          row.setAttribute('aria-checked', 'true');
        }
      }
      if (entry.icon) row.appendChild(iconOf(entry.icon));

      // 勾选槽每项都有（勾与不勾占同样宽度），不然勾选态会让整列文字左右错开。
      const mark = document.createElement('span');
      mark.className = 'heditor-menu__mark';
      if (entry.kind === 'action' && entry.checked) mark.textContent = '✓';
      row.appendChild(mark);

      const label = document.createElement('span');
      label.className = 'heditor-menu__label';
      label.textContent = entry.label;
      row.appendChild(label);

      if (entry.kind === 'action' && entry.hint) {
        const hint = document.createElement('span');
        hint.className = 'heditor-menu__hint';
        hint.textContent = entry.hint;
        row.appendChild(hint);
      }
      if (entry.kind === 'submenu') {
        const arrow = document.createElement('span');
        arrow.className = 'heditor-menu__arrow';
        arrow.textContent = '›';
        row.appendChild(arrow);
        const sub = buildList(entry.items, depth + 1);
        row.appendChild(sub);
      }

      // 悬停：每一行都要接，置灰的那些也算。
      //   置灰项不走 setActive(row)（高亮代表"回车就执行"，点不动的项不该亮），但必须把
      //   上一项的高亮与更深的层收掉 —— 否则鼠标停在置灰项上，前面那一项还亮着，看着像
      //   两项都能点，回车的去处也和眼睛看到的不一致。祖先留着：它这一层的子菜单还开着。
      row.addEventListener('pointerenter', () => {
        window.clearTimeout(hoverTimer);
        if (entry.disabled) {
          highlight(chainOf(row).slice(0, -1));
          const depth = Number(listOf(row).dataset.depth ?? '0');
          hoverTimer = window.setTimeout(() => closeSubmenus(depth), kSubmenuDelayMs);
          return;
        }
        setActive(row);
        if (entry.kind === 'submenu') {
          hoverTimer = window.setTimeout(() => openSubmenu(row, false), kSubmenuDelayMs);
          return;
        }
        // 移到没有子菜单的项上：把比它更深的那几层收掉。深度按这一行所在的列表算，
        //   写成固定值会把这一层自己关掉（二级里的项一被指到，二级就消失）。
        const depth = Number(listOf(row).dataset.depth ?? '0');
        hoverTimer = window.setTimeout(() => closeSubmenus(depth), kSubmenuDelayMs);
      });

      if (!entry.disabled) {
        // 用 click 而不是 pointerdown：菜单项在浮层内部，浮层的"点外面关"不会抢先；
        //   而 click 是"按下又抬起在同一项上"，手滑按错还能拖开。
        row.addEventListener('click', (ev) => {
          ev.preventDefault();
          ev.stopPropagation();
          if (entry.kind === 'submenu') {
            // 悬停延迟还挂着时点开，定时器随后还会再开一次同一层 —— 同一层压两遍，
            //   往里面走一步就把自己收掉（二级"打开就关"的来源）。
            window.clearTimeout(hoverTimer);
            // 点开但不把高亮送到第一项：鼠标还在父项上，亮的是父项；手真的移进二级，
            //   指针经过哪一项才亮哪一项（键盘那条路才要"进去就落在第一项"）。
            openSubmenu(row, false);
            setActive(row);
            return;
          }
          // 先关再跑：动作往往要开另一块面板（属性、取色），菜单留在原地会与它抢 Esc。
          const run = entry.run;
          close();
          run();
        });
      }

      list.appendChild(row);
    }
    return list;
  };

  /**
   * 某一层列表的项（跳过分隔线与标题）。
   * @param list 列表
   * @returns 可聚焦的项
   */
  const itemsOf = (list: HTMLElement): HTMLElement[] => {
    return Array.from(list.children).filter(
      (el): el is HTMLElement =>
        el instanceof HTMLElement &&
        el.classList.contains('heditor-menu__item') &&
        !el.classList.contains('heditor-menu__item--off'),
    );
  };

  /**
   * 这一项属于哪一层。
   * @param row 菜单项
   * @returns 所在列表
   */
  const listOf = (row: HTMLElement): HTMLElement =>
    (row.parentElement instanceof HTMLElement ? row.parentElement : root);

  /**
   * 某一层列表挂在哪个菜单项下面（最外层返回 null）。
   * @param list 列表
   * @returns 拥有它的那一项
   */
  const ownerOf = (list: HTMLElement): HTMLElement | null =>
    list.parentElement instanceof HTMLElement &&
    list.parentElement.classList.contains('heditor-menu__item')
      ? list.parentElement
      : null;

  /**
   * 这一项自己加上它的祖先（一路到最外层）。
   * @param row 菜单项
   * @returns 从外到内的那一条
   */
  const chainOf = (row: HTMLElement): HTMLElement[] => {
    const out: HTMLElement[] = [];
    let cur: HTMLElement | null = row;
    while (cur) {
      out.unshift(cur);
      cur = ownerOf(listOf(cur));
    }
    return out;
  };

  /**
   * 换一条高亮链（同一层仍然只亮一项）。
   * @param next 从外到内要亮的项
   */
  const highlight = (next: HTMLElement[]): void => {
    for (const el of chain) el.classList.remove('heditor-menu__item--active');
    chain = next;
    for (const el of chain) el.classList.add('heditor-menu__item--active');
    active = chain.length > 0 ? chain[chain.length - 1] : null;
  };

  /**
   * 设高亮：这一项连同它的祖先（父项的子菜单既然开着，父项就一直亮着）。
   * @param row 要亮的项（null = 全灭）
   */
  const setActive = (row: HTMLElement | null): void => {
    highlight(row ? chainOf(row) : []);
  };

  /**
   * 关掉第 depth 层及更深的子菜单。
   * @param depth 保留到第几层（0 = 只留最外层）
   */
  const closeSubmenus = (depth: number): void => {
    while (openStack.length > depth) {
      const list = openStack.pop();
      if (!list) break;
      list.hidden = true;
    }
  };

  /**
   * 打开某一项的子菜单。
   * @param row 带子菜单的项
   * @param focusFirst true = 打开之后把焦点移到第一项（键盘进来才这样）
   */
  const openSubmenu = (row: HTMLElement, focusFirst: boolean): void => {
    const list = Array.from(row.children).find(
      (el): el is HTMLElement =>
        el instanceof HTMLElement && el.classList.contains('heditor-menu__list--sub'),
    );
    if (!list) return;
    const depth = Number(listOf(row).dataset.depth ?? '0');
    closeSubmenus(depth + 1);
    list.hidden = false;
    const r = row.getBoundingClientRect();
    // 横向：右边放不下就翻到左边（贴着窗口右缘的菜单，往右开等于开到屏幕外）。
    const w = list.offsetWidth;
    const flip = r.right + w > window.innerWidth - 4;
    list.classList.toggle('heditor-menu__list--left', flip);
    // 纵向：贴着触发它的那一行，但整块必须留在视口里 —— 照 CSS 的 top 原样开，
    //   一级很长时二级的下半截会跑出窗口底部，那几项点不到。放不下就整块上移，
    //   移到顶边还不够高时给内部滚动（只对没有下一层的列表开：overflow 会裁掉孙级）。
    const h = list.offsetHeight;
    const limit = window.innerHeight - kViewportGap;
    const want = r.top - kSubmenuOverlap;
    const top = Math.max(kViewportGap, Math.min(want, limit - h));
    list.style.top = Math.round(top - r.top) + 'px';
    if (h > limit - kViewportGap && list.querySelector('.heditor-menu__list--sub') === null) {
      list.style.maxHeight = limit - kViewportGap + 'px';
      list.style.overflowY = 'auto';
    } else {
      list.style.maxHeight = '';
      list.style.overflowY = '';
    }
    // 同一层已经在栈里就不再压一遍：压重了之后收一层等于把还看得见的那层藏起来。
    if (openStack[openStack.length - 1] !== list) openStack.push(list);
    if (focusFirst) {
      const first = itemsOf(list)[0] ?? null;
      setActive(first);
    }
  };

  /**
   * 当前该响应键盘的那一层。
   * @returns 列表
   */
  const activeList = (): HTMLElement => {
    if (openStack.length === 0) {
      const first = root.querySelector('.heditor-menu__list');
      return first instanceof HTMLElement ? first : root;
    }
    return openStack[openStack.length - 1];
  };

  /**
   * 在同一层里移到下一项 / 上一项。
   * @param dir 1 = 往下，-1 = 往上
   */
  const move = (dir: number): void => {
    const list = activeList();
    const entries = itemsOf(list);
    if (entries.length === 0) return;
    const from = active ? entries.indexOf(active) : -1;
    // 到头回卷：原生菜单也没有"卡住不动"这一档。
    const next = from < 0 ? (dir > 0 ? 0 : entries.length - 1) : (from + dir + entries.length) % entries.length;
    setActive(entries[next]);
  };

  /**
   * 执行当前高亮项。
   */
  const runActive = (): void => {
    if (!active) return;
    if (active.classList.contains('heditor-menu__item--off')) return;
    const sub = Array.from(active.children).find(
      (el): el is HTMLElement =>
        el instanceof HTMLElement && el.classList.contains('heditor-menu__list--sub'),
    );
    if (sub) {
      openSubmenu(active, true);
      return;
    }
    active.click();
  };

  /**
   * 键盘：只在菜单打开时接管（Esc 与关闭时机在 layer 里）。
   * @param ev 键盘事件
   */
  const onKey = (ev: KeyboardEvent): void => {
    if (!layer.isOpen()) return;
    switch (ev.key) {
      case 'ArrowDown':
        ev.preventDefault();
        move(1);
        break;
      case 'ArrowUp':
        ev.preventDefault();
        move(-1);
        break;
      case 'Home':
        ev.preventDefault();
        setActive(itemsOf(activeList())[0] ?? null);
        break;
      case 'End': {
        ev.preventDefault();
        const all = itemsOf(activeList());
        setActive(all.length > 0 ? all[all.length - 1] : null);
        break;
      }
      case 'ArrowRight':
        ev.preventDefault();
        runActive();
        break;
      case 'ArrowLeft':
        ev.preventDefault();
        // 回到上一层：把这一层的子菜单收起来，高亮留在父项上。
        if (openStack.length > 0) {
          closeSubmenus(openStack.length - 1);
          const parentList = activeList();
          const parentRow = Array.from(parentList.children).find(
            (el): el is HTMLElement =>
              el instanceof HTMLElement &&
              el.classList.contains('heditor-menu__item') &&
              el.querySelector('.heditor-menu__list--sub:not([hidden])') !== null,
          );
          if (parentRow) setActive(parentRow);
        }
        break;
      case 'Enter':
      case ' ':
        ev.preventDefault();
        runActive();
        break;
      case 'Tab':
        // 菜单是模态的一小块：Tab 不该把焦点带到页面别处。
        ev.preventDefault();
        close();
        break;
      default:
        break;
    }
  };

  /**
   * 关掉：清空内容、收起所有子菜单、把键盘监听摘掉。
   */
  const close = (): void => {
    window.clearTimeout(hoverTimer);
    openStack.length = 0;
    highlight([]);
    document.removeEventListener('keydown', onKey, true);
    layer.close();
  };

  return {
    el: root,

    /**
     * 打开。
     *
     * 内容每次重建：菜单项带着打开那一刻的值（勾选态、禁用态），复用上一份等于显示旧状态。
     * @param items 菜单项
     * @param at 客户端坐标
     */
    open(items: MenuEntry[], at: { x: number; y: number }): void {
      close();
      root.textContent = '';
      const top = buildList(items, 0);
      root.appendChild(top);
      layer.openAtPoint(at.x, at.y);
      document.addEventListener('keydown', onKey, true);
      root.focus({ preventScroll: true });
      setActive(itemsOf(top)[0] ?? null);
    },

    close,

    isOpen: () => layer.isOpen(),
  };
}
