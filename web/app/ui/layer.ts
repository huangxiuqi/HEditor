// 浮层：一块挂在 body 上的固定定位面板，负责"开在哪、什么时候关"。
//
// 为什么从 popover 里抽出来：要开的面板不止"按钮下面那一块"（工具条面板、右键菜单、
//   属性面板都算），而"点外面关、Esc 关、滚了关、窗口变了关"这套时机与面板里装什么
//   完全无关。做成一份之后，新的浮层只需要给元素与内容，时机不必实现第二遍。
//
// 定位用 fixed + 视口坐标：编辑器在页面流里，absolute 会被滚动容器裁剪；fixed 之下
//   开一次算一次，滚动与改窗口大小时直接关掉（重算一次位置不如关掉诚实）。
// 夹取到视口：右键菜单点在窗口右下角时，不夹的话它整个在窗口外 —— 画面上就是"没反应"。
//
// 两种形态，时机不同：
//   锚定（openAt / openAtPoint）—— 跟着某个东西走。锚点滚出视野就没意义了，所以滚动即关。
//   对话框（openCentered）—— 居中的一块，跟滚动无关。滚正文时它还该在，resize 时重新居中
//     而不是关掉；拖过之后位置归用户，只夹回视口。

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
   * 居中打开（对话框形态）。
   *
   * 与锚定的两块差别：滚动不关、窗口变化时重新居中而不是关掉。
   * @param opts scrim 为真时铺一层遮罩，点遮罩关闭
   */
  openCentered(opts?: { scrim?: boolean }): void;

  /**
   * 让一个子元素当拖动把手（通常是标题栏）。
   *
   * 只在居中形态下有意义 —— 锚定的浮层位置由锚点决定，拖动它没有语义。
   *   每次打开换内容时把手元素也跟着换，所以这一项要在开之前调。
   * @param handle 把手元素
   */
  dragBy(handle: HTMLElement): void;

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
 * 此刻开着的那些浮层。
 *
 * 用来判"点外面"：落在另一块浮层里的按下手势，是那一块的内部操作，不是"点到外面"。
 *   没有这一条时，面板里的下拉一打开就会把面板自己关掉 —— 目标在对话框之外，判成外点。
 */
const openedLayers = new Set<HTMLElement>();

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
  // 居中形态：滚动不关、resize 重新居中。
  let centered = false;
  // 拖过之后位置归用户，resize 时只夹回视口，不再拉回中间。
  let dragged = false;

  /**
   * 遮罩（第一次要的时候才建）。挂在 body 上、比面板低一层。
   */
  let scrim: HTMLElement | null = null;

  /**
   * 铺开遮罩并接管点击。
   */
  const showScrim = (): void => {
    if (scrim === null) {
      scrim = document.createElement('div');
      scrim.className = 'heditor-scrim';
      scrim.hidden = true;
      document.body.appendChild(scrim);
      // 点遮罩等于"点面板外面"：先关，再让这一下不再落到正文上。
      scrim.addEventListener('pointerdown', (ev) => {
        ev.preventDefault();
        close();
      });
    }
    scrim.hidden = false;
  };

  /**
   * 收起遮罩。
   */
  const hideScrim = (): void => {
    if (scrim !== null) scrim.hidden = true;
  };

  /**
   * 点在面板与锚点之外。
   * @param ev 指针事件
   * @returns 真 = 这一次点击在意料之外
   */
  const outside = (ev: Event): boolean => {
    const t = ev.target instanceof Node ? ev.target : null;
    if (!t) return true;
    if (el.contains(t)) return false;
    if (anchorEl !== null && anchorEl.contains(t)) return false;
    for (const other of openedLayers) {
      if (other !== el && other.contains(t)) return false;
    }
    return true;
  };

  /**
   * 关掉并清空内容。
   */
  const close = (): void => {
    if (!opened) return;
    opened = false;
    centered = false;
    dragged = false;
    el.hidden = true;
    el.textContent = '';
    el.style.left = '';
    el.style.top = '';
    anchorEl = null;
    openedLayers.delete(el);
    hideScrim();
    document.removeEventListener('pointerdown', onPointer, true);
    document.removeEventListener('keydown', onKey, true);
    window.removeEventListener('resize', onResize);
    window.removeEventListener('scroll', onScroll, true);
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
   * 页面滚了。
   *
   * 锚定的浮层跟着锚点走，锚点滚出视野就没意义了，所以滚一下就关。但浮层自己内部的滚动
   *   不算：候选列表本来就是可滚的，滚它一下把下拉关掉等于选项多的下拉没法用。
   *   scroll 事件不冒泡，可在 window 上捕获阶段能收到任意元素的滚动 —— 所以这里要认目标。
   * @param ev 滚动事件（目标是滚动的那个元素）
   */
  const onScroll = (ev: Event): void => {
    const t = ev.target;
    if (t instanceof Node && el.contains(t)) return;
    close();
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
   * 摆到视口正中（略偏上：重心在上三分之一处比机械居中稳）。
   */
  const center = (): void => {
    const w = el.offsetWidth;
    const h = el.offsetHeight;
    place((window.innerWidth - w) / 2, (window.innerHeight - h) / 3);
  };

  /**
   * 窗口尺寸变了。
   *
   * 锚定的浮层跟着锚点走，锚点位置已经算不清了，关掉最诚实；居中的重新摆。
   */
  const onResize = (): void => {
    if (!centered) {
      close();
      return;
    }
    if (dragged) {
      place(el.offsetLeft, el.offsetTop);
      return;
    }
    center();
  };

  /**
   * 打开之后的公共收尾：挂监听。
   * @param anchor 锚点（没有锚点的按点定位）
   */
  const arm = (anchor: HTMLElement | null): void => {
    opened = true;
    anchorEl = anchor;
    el.hidden = false;
    openedLayers.add(el);
    document.addEventListener('pointerdown', onPointer, true);
    document.addEventListener('keydown', onKey, true);
    window.addEventListener('resize', onResize);
    window.addEventListener('scroll', onScroll, true);
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
      const below = r.bottom + kEdgeGap / 2;
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

    /**
     * 居中打开：滚动不关（滚正文时对话框还该在），窗口变化时重新居中。
     * @param opts scrim 为真时铺遮罩
     */
    openCentered(opts?: { scrim?: boolean }): void {
      if (opened) close();
      arm(null);
      centered = true;
      // 居中形态不跟滚动走：把"滚了就关"这条摘掉，其余时机照旧。
      window.removeEventListener('scroll', onScroll, true);
      if (opts && opts.scrim) showScrim();
      center();
    },

    /**
     * 让一个子元素当拖动把手。
     * @param handle 把手元素
     */
    dragBy(handle: HTMLElement): void {
      handle.classList.add('is-grab');
      handle.addEventListener('pointerdown', (ev) => {
        if (ev.button !== 0) return;
        // 把手上的按钮（关闭）是动作，不是拖动起点。
        const t = ev.target instanceof Element ? ev.target : null;
        if (t !== null && t.closest('button') !== null) return;
        ev.preventDefault();
        dragged = true;
        const fromLeft = el.offsetLeft;
        const fromTop = el.offsetTop;
        const fromX = ev.clientX;
        const fromY = ev.clientY;
        handle.setPointerCapture(ev.pointerId);
        handle.classList.add('is-grabbing');
        const move = (e: PointerEvent): void => {
          place(fromLeft + e.clientX - fromX, fromTop + e.clientY - fromY);
        };
        const up = (e: PointerEvent): void => {
          if (handle.hasPointerCapture(e.pointerId)) handle.releasePointerCapture(e.pointerId);
          handle.removeEventListener('pointermove', move);
          handle.removeEventListener('pointerup', up);
          handle.removeEventListener('pointercancel', up);
          handle.classList.remove('is-grabbing');
        };
        handle.addEventListener('pointermove', move);
        handle.addEventListener('pointerup', up);
        handle.addEventListener('pointercancel', up);
      });
    },

    close,
    isOpen: () => opened,
  };
}
