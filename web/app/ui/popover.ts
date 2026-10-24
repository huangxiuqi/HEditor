// 弹层：挂在某个按钮下面的一块面板。
//
// 为什么单独做一件：工具条上会开面板的按钮不止一个（表格尺寸、以后的插入项），而"关掉
//   ——点外面、按 Esc、窗口滚了"这套时机与面板里装什么无关。做成一份之后，新面板只要
//   给一个元素，时机不用再实现第二遍。
//
// 定位用 fixed + 锚点矩形：功能区在页面流里，用 absolute 会被滚动容器裁剪；fixed 之下
//   面板跟着锚点算一次就够，滚动与改窗口大小时直接关掉（重算一次位置不如关掉诚实）。

/**
 * 一块弹层。
 */
export interface Popover {
  /**
   * 面板根元素（已挂在 body 上，未打开时 display:none）。
   */
  el: HTMLElement;

  /**
   * 在锚点下面打开。
   * @param anchor 锚点元素（按钮）
   */
  open(anchor: HTMLElement): void;

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
 * 造一块弹层。
 * @param onClose 关掉之后的回调（清理面板内容用）
 * @returns 弹层
 */
export function createPopover(onClose?: () => void): Popover {
  const el = document.createElement('div');
  el.className = 'heditor-popover';
  el.hidden = true;
  document.body.appendChild(el);

  let opened = false;

  /**
   * 点在面板与锚点之外。
   * @param ev 指针事件
   * @returns 真 = 这一次点击在意料之外
   */
  const outside = (ev: Event): boolean => {
    const t = ev.target instanceof Node ? ev.target : null;
    if (!t) return true;
    return !el.contains(t) && !(anchorEl && anchorEl.contains(t));
  };

  let anchorEl: HTMLElement | null = null;

  /**
   * 关掉并清空内容。
   */
  const close = (): void => {
    if (!opened) return;
    opened = false;
    el.hidden = true;
    el.textContent = '';
    anchorEl = null;
    document.removeEventListener('pointerdown', onPointer, true);
    document.removeEventListener('keydown', onKey, true);
    window.removeEventListener('resize', close);
    window.removeEventListener('scroll', close, true);
    if (onClose) onClose();
  };

  /**
   * 面板外的按下手势：先关，再让这一下照常落到目标上。
   * @param ev 指针事件
   */
  const onPointer = (ev: Event): void => {
    if (outside(ev)) close();
  };

  /**
   * Esc 关面板。
   * @param ev 键盘事件
   */
  const onKey = (ev: Event): void => {
    const k = ev as KeyboardEvent;
    if (k.key !== 'Escape') return;
    close();
    k.preventDefault();
    k.stopPropagation();
  };

  return {
    el,

    /**
     * 打开：内容由调用方先塞进 el。
     * @param anchor 锚点元素
     */
    open(anchor: HTMLElement): void {
      if (opened) {
        close();
        return;
      }
      opened = true;
      anchorEl = anchor;
      el.hidden = false;
      // 先显示再量：隐藏元素的 offsetWidth 是 0，按它算会把面板推到屏幕外。
      const r = anchor.getBoundingClientRect();
      el.style.left = `${Math.round(r.left)}px`;
      el.style.top = `${Math.round(r.bottom + 2)}px`;
      document.addEventListener('pointerdown', onPointer, true);
      document.addEventListener('keydown', onKey, true);
      window.addEventListener('resize', close);
      window.addEventListener('scroll', close, true);
    },

    close,
    isOpen: () => opened,
  };
}
