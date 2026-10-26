// 浮层：一块挂在 body 上的固定定位面板，负责"开在哪、什么时候关"。
//
// 为什么从 popover 里抽出来：要开的面板不止"按钮下面那一块"（工具条面板、右键菜单、
//   属性面板都算），而"点外面关、Esc 关、滚了关、窗口变了关"这套时机与面板里装什么
//   完全无关。做成一份之后，新的浮层只需要给元素与内容，时机不必实现第二遍。
//
// 定位用 fixed + 视口坐标：编辑器在页面流里，absolute 会被滚动容器裁剪；fixed 之下
//   开一次算一次，滚动与改窗口大小时直接关掉（重算一次位置不如关掉诚实）。
// 夹取到视口：右键菜单点在窗口右下角时，不夹的话它整个在窗口外 —— 画面上就是"没反应"。

/**
 * 一块浮层。
 */
export interface Layer {
  /**
   * 面板根元素（已挂在 body 上，未打开时 hidden）。
   */
  el: HTMLElement;

  /**
   * 在锚点元素的正下方打开（放不下时翻到上方）。
   * @param anchor 锚点元素
   */
  openAt(anchor: HTMLElement): void;

  /**
   * 在视口坐标处打开（右键菜单、指针弹出的面板）。
   * @param x 客户端坐标 x（CSS 像素）
   * @param y 客户端坐标 y（CSS 像素）
   */
  openAtPoint(x: number, y: number): void;

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
 * 浮层与视口边缘之间至少留多少（CSS 像素）。
 */
const kEdgeGap = 8;

/**
 * 造一块浮层。
 * @param className 面板的 class（样式与判据都按它定位）
 * @param onClose 关掉之后的回调（清理内容用）
 * @returns 浮层
 */
export function createLayer(className: string, onClose?: () => void): Layer {
  const el = document.createElement('div');
  el.className = className;
  el.hidden = true;
  document.body.appendChild(el);

  let opened = false;
  let anchorEl: HTMLElement | null = null;

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

  /**
   * 把面板夹进视口。先显示再量：隐藏元素的尺寸是 0，按它算会把面板推到屏幕外。
   * @param left 期望的左边（客户端坐标）
   * @param top 期望的上边（客户端坐标）
   */
  const place = (left: number, top: number): void => {
    const w = el.offsetWidth;
    const h = el.offsetHeight;
    const maxLeft = Math.max(kEdgeGap, window.innerWidth - w - kEdgeGap);
    const maxTop = Math.max(kEdgeGap, window.innerHeight - h - kEdgeGap);
    el.style.left = `${Math.round(Math.min(Math.max(left, kEdgeGap), maxLeft))}px`;
    el.style.top = `${Math.round(Math.min(Math.max(top, kEdgeGap), maxTop))}px`;
  };

  /**
   * 打开之后的公共收尾：挂监听。
   * @param anchor 锚点（没有锚点的按点定位）
   */
  const arm = (anchor: HTMLElement | null): void => {
    opened = true;
    anchorEl = anchor;
    el.hidden = false;
    document.addEventListener('pointerdown', onPointer, true);
    document.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', close);
    window.addEventListener('scroll', close, true);
  };

  return {
    el,

    /**
     * 锚点下方（或上方）：下方放不下、上方放得下时翻上去 —— 工具条贴着窗口顶部时
     * 往下的空间永远够，贴着窗口底部时（比如状态栏上的入口）只有翻上去才看得见。
     * @param anchor 锚点元素
     */
    openAt(anchor: HTMLElement): void {
      if (opened) {
        close();
        return;
      }
      arm(anchor);
      const r = anchor.getBoundingClientRect();
      const below = r.bottom + 2;
      const fits = below + el.offsetHeight + kEdgeGap <= window.innerHeight;
      place(r.left, fits ? below : r.top - el.offsetHeight - 2);
    },

    /**
     * 指针位置：面板左上角跟着指针，越界时夹回视口。
     * @param x 客户端坐标 x
     * @param y 客户端坐标 y
     */
    openAtPoint(x: number, y: number): void {
      if (opened) close();
      arm(null);
      place(x, y);
    },

    close,
    isOpen: () => opened,
  };
}
