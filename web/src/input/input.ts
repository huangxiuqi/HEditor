// 输入层：键盘、快捷键、IME、指针按下的判定与提交。
// 环境对象（两套传输的差异全部从这里进来）：
//   isReady() / isBusy() / ta / hot()           就绪、忙、承接层、{ vertical, blockReversed }
//   write(name, args) / hit(xPt, yPt)           发命令、命中；返回布尔/对象或 Promise
//   pointOf(ev) / zoom()                        客户端坐标到文档设备像素、折磅除数
//   capturePointer() / beginDrag(ev) / focus()  指针记账、起手、把焦点交给承接层
//   commit(changed)                             键位与组合结束后的收尾
//   place(act) / selectCellPara(para) / cellSelected()   落光标、落整格、三击收尾
//   status(text) / onChain(p)                   状态栏、登记在途链（可选）
import {
  clickCountOf,
  hitToAction,
  keyToOp,
  then1,
  type Args,
  type CaretAction,
  type ClickState,
  type Hit,
  type HitAction,
  type HotState,
  type Maybe,
} from './keymap';
import type { ImageBox } from '../types';

export interface InputEnv {
  /**
   * 引擎与宿主都就绪了没有。
   */
  isReady(): boolean;

  /**
   * 有在途命令时输入要先让路（异步宿主用）。
   */
  isBusy?(): boolean;

  /**
   * 承接键盘与合成的 textarea。
   */
  ta: HTMLTextAreaElement;

  /**
   * 热读：竖排 / 列序（决定方向键映射）。
   */
  hot(): HotState;

  /**
   * 发一条命令。
   * @param name 命令名
   * @param args 参数
   */
  write(name: string, args?: Args): Maybe<any>;

  /**
   * 命中。收设备像素（宿主内部折算成文档坐标，与拖选同一口径）。
   * @param xPx 文档横坐标（设备像素）
   * @param yPx 文档纵坐标（设备像素）
   */
  hit(xPx: number, yPx: number): Maybe<Hit | null>;

  /**
   * 设备像素 -> 文档坐标。写类命令（如 cellSelectAt）要自己传坐标时用它。
   * @param pt 设备像素坐标
   */
  docPt(pt: { x: number; y: number }): { x: number; y: number };

  /**
   * 客户端坐标折成文档设备像素（落在页外返回 null）。
   * @param ev 带客户端坐标的事件
   */
  pointOf(ev: { clientX: number; clientY: number }): { x: number; y: number } | null;

  /**
   * 换画布光标样式（空串 = 正文的默认样式）。
   * @param cursor CSS cursor 值
   */
  setCursor?(cursor: string): void;

  /**
   * 把当前按下绑到页容器上（拖出边界也继续收）。
   */
  capturePointer(): void;

  /**
   * 起手一次拖选。
   * @param ev 按下的客户端坐标
   */
  beginDrag(ev: { clientX: number; clientY: number }): void;

  /**
   * 拖选控制器（宿主造的）；没有就不拖。
   */
  drag?: { arm(on: boolean): void };

  /**
   * 把焦点交给承接层。
   */
  focus(): void;

  /**
   * 一次按键 / 合成的收尾。
   * @param changed 内容有没有真的变
   */
  commit(changed: boolean): void;

  /**
   * 落光标（含侧别与行别）。
   * @param act 命中折出来的落点
   */
  place(act: CaretAction): Maybe<any>;

  /**
   * 选中 para 所在的整格。
   * @param para 段落号
   */
  selectCellPara(para: number): Maybe<any>;

  /**
   * 整格选区的收尾（重画之后不再起拖）。
   */
  cellSelected(): void;

  /**
   * 状态栏。
   * @param text 要说的话
   * @param isErr true = 这是一次失败
   */
  status(text: string, isErr?: boolean): void;

  /**
   * 输入法组合串的显示：组合中画在光标处，结束收起。
   * @param text 组合中的串；空串 = 收起
   */
  preedit?(text: string): void;

  /**
   * 把在途链登记给宿主（测试可以等它）。
   * @param p 那条链
   */
  onChain?(p: Maybe<any>): void;

  /**
   * 缩放编辑器（Ctrl + 滚轮 / 触控板捏合 / Ctrl + 加减号）。
   * @param delta 缩放倍数的增量（正 = 放大）
   */
  zoomBy?(delta: number): void;

  /**
   * 缩放回到 100%（Ctrl + 0）。
   */
  zoomReset?(): void;

  /**
   * 文档里全部图片（拖拽开始时要取起始尺寸）。
   */
  images?(): Maybe<ImageBox[]>;

  /**
   * 命中的控件是不是选择类；是就给出切换请求（含切换之前的值），否则 null。
   * @param hit 命中结果
   */
  controlToggle?(hit: Hit): {
    index: number;
    option: number;
    type: string;
    key: string;
    oldValue: { flag: boolean; text: string; items: string[] };
  } | null;

  /**
   * 切换之前问一次宿主（可能是异步的）。没接就一律允许。
   * @param change 切换请求
   */
  beforeControlChange?(change: any): Maybe<boolean>;

  /**
   * 让引擎切换某个控件的某一项。
   * @param index 控件下标
   * @param option 选项下标（单控件 -1）
   */
  toggleControl?(index: number, option: number): Maybe<any>;

  /**
   * 点到一个不是选择类的控件（下拉、日期）时通知宿主 —— 弹出层归宿主。
   * @param index 控件下标
   */
  onControlTap?(index: number): void;

  /**
   * 选中一张图片（-1 = 取消选中）。
   * @param index 图片下标
   */
  selectImage?(index: number): Maybe<any>;

  /**
   * 改图片尺寸（磅）。merge 为真表示与上一帧合成一步撤销。
   * @param index 图片下标
   * @param w 新宽度（磅）
   * @param h 新高度（磅）
   * @param merge 与上一帧合成一步撤销
   */
  resizeImage?(index: number, w: number, h: number, merge: boolean): Maybe<any>;

  /**
   * 拖拽改尺寸的预览（不动文档、不重排）：只把选中框换成"松手会变成多大"。
   * @param index 图片下标
   * @param w 预览宽度（磅）
   * @param h 预览高度（磅）
   * @param handle 正在拖的把手（0 左上顺时针）
   */
  previewImageSize?(index: number, w: number, h: number, handle: number): Maybe<any>;

  /**
   * 收起尺寸预览（拖拽取消时用；正常松手由 resizeImage 自己收）。
   */
  clearImagePreview?(): Maybe<any>;

  /**
   * 表格边界拖拽：指针落在内部格线上就由宿主接管（返回真），此后不落光标。
   *
   * 整条拖拽都归宿主：它要画指示线、把指针增量换算成磅，而那些都是"画面上的
   *   一条线"，引擎不该知道。哪一条线由命中给出（hit 的 gridTable / gridCol / gridRow）。
   * @param pt 文档坐标（设备像素）
   * @param h 这一下的命中结果（带格线三格）
   */
  tableResize?(pt: { x: number; y: number }, h: Hit): Maybe<boolean>;
}

export interface InputLayer {
  /**
   * 键盘按下。
   * @param ev 键盘事件
   */
  onKeyDown(ev: KeyboardEvent): Maybe<any>;

  /**
   * 输入法开始组合：这一段时间不碰引擎。
   */
  onCompositionStart(): void;

  /**
   * 组合串更新（只更新状态栏预览）。
   * @param ev 组合事件
   */
  onCompositionUpdate(ev: CompositionEvent): void;

  /**
   * 组合结束：一次提交进文档。
   * @param ev 组合事件
   */
  onCompositionEnd(ev: CompositionEvent): Maybe<any>;

  /**
   * 鼠标悬停：落在图片把手上时把光标换成对应方向。
   * @param ev 鼠标事件
   */
  onPointerMove(ev: MouseEvent): void;

  /**
   * 指针离开画布：把光标换回正文的样式。
   */
  onPointerLeave(): void;

  /**
   * 指针按下：命中、落光标、起拖选。
   * @param ev 鼠标事件
   */
  onPointerDown(ev: MouseEvent): Maybe<any>;

  /**
   * 滚轮：带 Ctrl（捏合也是）时转成编辑器缩放。
   *
   * 返回 true 表示这一下被接管，调用方要 preventDefault —— 不拦的话浏览器会去缩放
   *   整个页面。判断是同步的（preventDefault 只在事件派发期间有效），缩放本身异步下发。
   * @param ev 滚轮事件
   * @returns true = 这一下归编辑器
   */
  onWheel(ev: WheelEvent): boolean;

  /**
   * 按设备像素坐标落光标（自动化入口与按下共用这一条）。
   * @param pt 文档坐标（设备像素）
   * @param preHit 已经拿到的命中结果（省一次往返）
   * @param opts tap 为假时不触发控件的弹出层（右键走这条路：菜单归宿主，不该再弹一个值面板）
   */
  placeAt(pt: { x: number; y: number }, preHit?: Hit | null, opts?: { tap?: boolean }): Maybe<Hit | null>;
  readonly clickState: ClickState;
  readonly composing: boolean;
}

/**
 * 一档缩放。与示例页工具栏的加减号同一个步长，两处按下去的手感才一致。
 */
const kZoomStep = 0.125;

/**
 * 拖拽移动的起手门槛（CSS 像素）。与拖选用同一个数：手抖一像素不该把一段字搬走。
 */
const kMoveSlop = 3;

/**
 * 缩放快捷键：Ctrl + 加号 / 减号 / 0。浏览器对这三组的默认行为是缩放整个页面。
 *
 * 认 key 而不认 code：Ctrl + 加号在美式键盘上要按 Shift，event.key 那时才是 '+'，
 *   不按 Shift 时是 '=' —— 两个都收；小键盘给的是 '+' / '-' / '0'，同样落在这几条上。
 *
 * @param ev 键盘事件
 * @returns 缩放增量；NaN = 回到 100%；null = 不是缩放键
 */
function zoomKeyOf(ev: KeyboardEvent): number | null {
  if (!(ev.ctrlKey || ev.metaKey) || ev.altKey) return null;
  if (ev.key === '=' || ev.key === '+') return kZoomStep;
  if (ev.key === '-' || ev.key === '_') return -kZoomStep;
  if (ev.key === '0') return NaN;
  return null;
}

/**
 * 图片把手序号 -> 光标。0 左上起顺时针（见 ImageHit::handle）：角上是对角缩放，
 * 四条边各管一个轴。认方向而不认具体把手，与尺寸推导用的是同一份序号约定。
 * @param handle 把手序号（0..7）
 * @returns CSS cursor 值；不认识的序号给空串
 */
function cursorOfHandle(handle: number): string {
  switch (handle) {
    case 0:
    case 4:
      return 'nwse-resize';
    case 2:
    case 6:
      return 'nesw-resize';
    case 1:
    case 5:
      return 'ns-resize';
    case 3:
    case 7:
      return 'ew-resize';
    default:
      return '';
  }
}

/**
 * 造输入层。
 * @param env 宿主注入的环境（读写、命中、坐标换算、收尾）
 * @returns 键盘 / 合成 / 指针三个入口
 */
export function createInput(env: InputEnv): InputLayer {
  const click: ClickState = { seq: 0, time: 0, x: 0, y: 0 };
  let composing = false;
  // 正在拖图片把手：这期间悬停命中不参与，光标由把手方向定。
  let resizing = false;
  // 悬停命中只允许一条在途：鼠标每秒能发上百次移动，逐次下发会把通道堵住。
  let hover_busy = false;
  let hover_next: { x: number; y: number } | null = null;
  // 悬停结果的代际：指针离开画布之后，在途的那一条回来也不改光标。
  let hover_seq = 0;
  const isBusy = (): boolean => (typeof env.isBusy === 'function' ? env.isBusy() : false);
  /**
   * 把一串异步动作登记给宿主（测试可以等它），并保证拒绝不会变成未处理的 Promise。
   *
   * 调用方是 DOM 事件处理器：它不留句柄，返回值也没人 await。中途一条命令失败
   *   （最典型的是销毁时在途的那几条被拒），浏览器只在控制台留一条 uncaught ——
   *   用户看不见，而探针会把整份判据判红。这里统一降级成状态栏与 error 事件。
   */
  const keep = (p: Maybe<any>): Maybe<any> => {
    const guarded =
      p && typeof (p as Promise<any>).then === 'function'
        ? (p as Promise<any>).catch((e) => {
            const msg = String((e && (e as Error).message) || e);
            env.status('输入处理失败：' + msg, true);
          })
        : p;
    if (typeof env.onChain === 'function') env.onChain(guarded);
    return guarded;
  };
  const beginDrag = (ev: { clientX: number; clientY: number }): void => {
    if (typeof env.beginDrag === 'function') env.beginDrag(ev);
  };
  const armDrag = (on: boolean): void => {
    if (env.drag) env.drag.arm(on);
  };

  /**
   * 悬停命中的结果落成光标。
   * @param hit 命中结果（可能为空）
   */
  function applyHoverCursor(hit: Hit | null | undefined): void {
    if (typeof env.setCursor !== 'function') return;
    const handle = hit && typeof hit.imgHandle === 'number' ? hit.imgHandle : -1;
    env.setCursor(handle >= 0 ? cursorOfHandle(handle) : '');
  }

  /**
   * 发一次悬停命中（只保留最新坐标，最多一条在途）。
   */
  function pumpHover(): void {
    if (hover_busy || hover_next === null) return;
    const p = hover_next;
    hover_next = null;
    hover_busy = true;
    const seq = ++hover_seq;
    const done = (hit: Hit | null | undefined): void => {
      hover_busy = false;
      if (seq === hover_seq) applyHoverCursor(hit);
      pumpHover();
    };
    const r = env.hit(p.x, p.y);
    if (r && typeof (r as Promise<any>).then === 'function') {
      (r as Promise<any>).then(done, () => {
        hover_busy = false;
      });
    } else {
      done(r as Hit | null);
    }
  }

  /**
   * 鼠标悬停：图片把手上换成缩放光标，离开把手换回正文光标。
   * @param ev 鼠标事件
   */
  function onPointerMove(ev: MouseEvent): void {
    // 按着键移动（拖选、拖图片）不改光标：那一路有自己的状态，混进来会互相覆盖。
    if (ev.buttons !== 0) return;
    if (!env.isReady() || isBusy() || resizing) return;
    const p = env.pointOf(ev);
    if (!p) {
      hover_next = null;
      ++hover_seq;
      if (typeof env.setCursor === 'function') env.setCursor('');
      return;
    }
    hover_next = p;
    pumpHover();
  }

  /**
   * 指针离开画布：光标回到正文的默认样式。
   */
  function onPointerLeave(): void {
    hover_next = null;
    ++hover_seq;
    if (typeof env.setCursor === 'function') env.setCursor('');
  }

  /**
   * 图片把手的拖拽：按下时取一次起始尺寸与指针位置，之后每帧按增量算新尺寸。
   *
   * 等比 / 不等比只看把手落在哪：四个角按原图比例推另一边，四条边只动一个轴；角上按住
   * Shift 解除比例约束。拖动过程不改文档：每帧只把瞬态层那个框换成"松手会变成多大"
   * （previewImageSize），松手才发一次 resizeImage —— 每帧真改尺寸要重排 + 重光栅，
   * 而且拖到一半看到的已经是最终画面，反而看不出框在跟手。
   *
   * 监听在按下这一拍就挂上，起始尺寸才是异步取的：命中在 Worker 宿主上是一次往返，
   * 等它回来才挂的话，按下到回来之间的移动全丢（快速一拖表现为"把手拖不动"）。
   */
  function beginImageResize(pt: { x: number; y: number }, index: number, handle: number): void {
    if (typeof env.resizeImage !== 'function' || typeof env.images !== 'function') return;
    const images = env.images;
    const resize = env.resizeImage;
    const preview = env.previewImageSize;
    const dropPreview = env.clearImagePreview;
    const start = env.docPt(pt);
    const left = handle === 0 || handle === 6 || handle === 7;
    const right = handle === 2 || handle === 3 || handle === 4;
    const top = handle === 0 || handle === 1 || handle === 2;
    const bottom = handle === 4 || handle === 5 || handle === 6;
    const corner = (left || right) && (top || bottom);
    // 起始尺寸（磅）：取回来之前是 null —— 这时候到达的移动点先存在 pending。
    let size0: { w: number; h: number } | null = null;
    let pending: { x: number; y: number } | null = null;
    // 松手要落地的尺寸：null = 这一趟没动过（既不发预览，也不改文档）。
    let last: { w: number; h: number } | null = null;
    // 退路（没有预览入口）用：第一帧不带 merge，一次拖拽留一步撤销。
    let first_frame = true;
    const applyAt = (cur: { x: number; y: number }, shift: boolean): void => {
      if (!size0) return;
      const dx = cur.x - start.x;
      const dy = cur.y - start.y;
      let w = size0.w;
      let h = size0.h;
      if (right) w = size0.w + dx;
      else if (left) w = size0.w - dx;
      if (bottom) h = size0.h + dy;
      else if (top) h = size0.h - dy;
      if (corner && !shift) h = w / (size0.w / size0.h);
      if (!(w > 0) || !(h > 0)) return;
      // 下限 8 磅：再小就没有可点的图身与把手，拖到零之后再也拖不回来。
      const nw = Math.max(8, Math.round(w * 100) / 100);
      const nh = Math.max(8, Math.round(h * 100) / 100);
      last = { w: nw, h: nh };
      // 预览不可用时退回"每帧真改尺寸"：慢，但至少框跟着手走。
      if (typeof preview === 'function') keep(preview(index, nw, nh, handle));
      else {
        keep(resize(index, nw, nh, !first_frame));
        first_frame = false;
      }
    };
    const detach = (): void => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      resizing = false;
    };
    const onMove = (e: MouseEvent): void => {
      const p = env.pointOf(e);
      if (!p) return;
      const cur = env.docPt(p);
      if (!size0) {
        pending = cur;
        return;
      }
      applyAt(cur, e.shiftKey);
    };
    const onUp = (): void => {
      detach();
      if (last === null) {
        // 按下又松开、一磅都没动：预览可能已经闪过一帧，收掉它。
        if (typeof dropPreview === 'function') keep(dropPreview());
        return;
      }
      const done = last;
      // 尺寸真的变过才收尾一次：change 事件按"一次拖拽一次"发，不是按帧发。
      keep(
        then1(resize(index, done.w, done.h, false), () => {
          env.commit(true);
        }),
      );
    };
    resizing = true;
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    keep(
      then1(images(), (list) => {
        const box = (list || []).find((b) => b.index === index);
        if (!box || !(box.w > 0) || !(box.h > 0)) {
          // 这一趟起不来（图被删了、尺寸还没量出来）：收掉监听，别让它留着。
          if (resizing) detach();
          return null;
        }
        size0 = { w: box.w, h: box.h };
        if (pending) {
          const p = pending;
          pending = null;
          applyAt(p, false);
        }
        return null;
      }),
    );
  }

  // 命中之后的两条去向：定位、以及"这一下要不要起拖选"。
  // 落在正文之外时不起手 —— 从页外空白拖起不该选中任何东西。
  // 定位这一步必须等：异步宿主的落光标是一次往返，不等它，clickAt 这类入口会在
  // 光标还没摆好时就 resolve，调用方读到的光标是上一拍的。
  const placeAt = (
    pt: { x: number; y: number },
    preHit?: Hit | null,
    opts?: { tap?: boolean },
  ): Maybe<Hit | null> =>
    then1(preHit !== undefined ? preHit : env.hit(pt.x, pt.y), (h) => {
      // 图片优先：点在图身或把手上都不落光标 —— 图片是原子，光标只能停在它两侧。
      //   把手还要起一次拖拽（后续帧由 window 上的监听接手）。
      if (h && typeof h.img === 'number' && h.img >= 0) {
        armDrag(false);
        const selected = typeof env.selectImage === 'function' ? env.selectImage(h.img) : null;
        return then1(selected, () => {
          const handle = typeof h.imgHandle === 'number' ? h.imgHandle : -1;
          if (handle >= 0) beginImageResize(pt, h.img as number, handle);
          return h;
        });
      }
      // 选择类控件：点一下就切。前置回调可能是异步的 —— 等它回来再动文档，
      //   于是等待期间画面不变，也就没有"先勾上再弹回去"的那一帧。
      if (h && typeof h.field === 'number' && h.field >= 0 && typeof env.controlToggle === 'function') {
        const info = env.controlToggle(h);
        if (info) {
          // 落点照摆（选区先塌到这里）：光标不能停在控件里，选区可以 ——
          //   否则从控件内部起手拖选就成了死路，点一下也清不掉已有选区。
          //   拖动由下面 armDrag(true) 接着扩；切换与落点的先后不影响观感。
          const act = hitToAction(h);
          if (act && act.kind === 'caret') {
            const placed = env.place(act);
            armDrag(true);
            return then1(placed, () => {
              const allowed =
                typeof env.beforeControlChange === 'function' ? env.beforeControlChange(info) : true;
              return then1(allowed, (ok) => {
                if (ok) keep(env.toggleControl ? env.toggleControl(info.index, info.option) : null);
                return h;
              });
            });
          }
          armDrag(false);
          const allowed =
            typeof env.beforeControlChange === 'function' ? env.beforeControlChange(info) : true;
          return then1(allowed, (ok) => {
            if (!ok) return h;
            keep(env.toggleControl ? env.toggleControl(info.index, info.option) : null);
            return h;
          });
        }
        // 右键：菜单归宿主，值与下拉面板不该同时弹出来（两块浮层会叠在一起）。
        if (opts?.tap !== false) env.onControlTap?.(h.field);
      }
      const act = hitToAction(h);
      if (!act) {
        armDrag(false);
        return null;
      }
      if (act.kind === 'cell') {
        // 整格选区之后不再拖：这一下表达的是"我要这一格"，继续拖会把焦点端
        // 搬到别的格上去，与文字选区那条路混成一件说不清的事。
        const done = env.selectCellPara(act.para);
        armDrag(false);
        return then1(done, () => h);
      }
      const done = env.place(act);
      armDrag(true);
      return then1(done, () => h);
    });

  /**
   * 命中落在行内对象上（控件内部，或图片与图片把手）。
   * @param h 命中结果
   * @returns 是否落在行内对象上
   */
  const hitsInlineObject = (h: Hit | null | undefined): boolean =>
    !!h &&
    ((typeof h.field === 'number' && h.field >= 0) ||
      (typeof h.img === 'number' && h.img >= 0));

  /**
   * 命中压在某条内部格线上（拖列宽 / 行高）。
   *
   * 三格一起判：引擎只在"确实压着格线"时才把这三个键写出来，缺一个就是没压着。
   * @param h 命中结果
   * @returns 是否压在格线上
   */
  const onGridLine = (h: Hit | null | undefined): boolean =>
    !!h &&
    typeof h.gridTable === 'number' &&
    ((typeof h.gridCol === 'number' && h.gridCol > 0) ||
      (typeof h.gridRow === 'number' && h.gridRow >= 0));

  function onKeyDown(ev: KeyboardEvent): Maybe<any> {
    if (!env.isReady() || isBusy()) return undefined;
    // 组合期间一律不动文档：输入法在组合时会持续发 keydown，每次都重排会让画面
    // 又卡又闪，这是 WASM 编辑器最容易出问题的一处。
    if (composing || ev.isComposing) return undefined;
    // 缩放键排在命令表之前：它们不是文档命令（改的是密度，不是内容），而浏览器的
    //   默认行为是缩放整个页面 —— 不拦的话工具栏与画布会一起放大，画布是位图，放大只是插值。
    const zoom = zoomKeyOf(ev);
    if (zoom !== null) {
      ev.preventDefault();
      if (Number.isNaN(zoom)) env.zoomReset?.();
      else env.zoomBy?.(zoom);
      return true;
    }
    const op = keyToOp(ev, env.hot());
    if (!op) return undefined; // 不认识的键（含 IME 的 Process 键）放行
    ev.preventDefault();
    return keep(then1(env.write(op[0], op[1]), (changed) => env.commit(!!changed)));
  }

  function onCompositionStart(): void {
    composing = true;
    env.ta.value = '';
    env.preedit?.('');
  }

  function onCompositionUpdate(ev: CompositionEvent): void {
    // 组合串画在光标处：承接层是透明的 textarea，浏览器画在它里面的字用户看不见，
    //   而中文输入法在选字阶段屏幕上就什么都没有。正文仍等组合结束一次落地 ——
    //   组合期间改布局会让候选框跟着跳。
    env.preedit?.(ev.data || '');
  }

  function onCompositionEnd(ev: CompositionEvent): Maybe<any> {
    composing = false;
    env.preedit?.('');
    const text = ev.data || env.ta.value || '';
    env.ta.value = '';
    const ready = env.isReady() && !isBusy();
    const w = text && ready ? env.write('type', { s: text }) : false;
    return keep(
      then1(w, (changed) => {
        if (changed) env.commit(true);
        env.status('就绪');
      }),
    );
  }

  function onPointerDown(ev: MouseEvent): Maybe<any> {
    if (!env.isReady() || isBusy()) return undefined;
    if (ev.button !== 0) return undefined;
    const pt = env.pointOf(ev);
    if (!pt) return undefined;
    env.capturePointer();
    const count = clickCountOf(ev, click);
    // 监听必须当场挂上：命中在异步宿主上是往返，等它回来才起手的话，
    //   按下与第一次移动之间的移动会被整段丢掉，而那一段往往正是"按下去就往右拖"。
    beginDrag(ev);
    // 命中只算一次，这一下按的是什么全看它：格线（拖列宽行高）、已选中的内容（拖拽移动）、
    //   还是普通落点。三件事分成三条命令问的话，宿主就要三次往返才轮得到落光标，而按下
    //   到第一次移动之间往往只隔几毫秒 —— 拖选会拖到一半就断、图片把手一磅都不动。
    //
    //   搬运动作的监听同样在这一拍就挂上（trackMove）：抬手可能赶在命中回来之前到
    //   —— 一次快速单击就是"按下、抬起"背靠背，等命中回来才挂，那次抬手永远收不到，
    //   于是既没落光标也没搬家（判据：点控件内部清不掉已有选区）。
    const move = trackMove(pt);
    const chain = then1(env.hit(pt.x, pt.y), (h) => {
      if (onGridLine(h) && typeof env.tableResize === 'function') {
        // 格线：整条拖拽归宿主（它要画指示线、把指针增量换算成磅）。
        armDrag(false);
        move.cancel();
        // 抬手已经先到了：这一下就是"在格线上点了一下"，按单击落点收尾。
        //   交给宿主它只会挂上一串等不到的监听。
        if (move.afterUp()) return placeAt(pt, h);
        return then1(env.tableResize(pt, h as Hit), () => null);
      }
      // 拖拽移动：按住已经落定的选区或图片再拖，表达的是"搬到别处"。
      //   只认单击之后的拖动：第二下、第三下落在一个已有的选区里是常态（双击选词、
      //   三击选段的第一下就已经把光标摆在那儿了），把它们判成搬家，双击就再也选
      //   不出词、三击也选不出段 —— 连击的意图比"搬"明确得多。
      if (count === 1 && h && typeof h.drag === 'number' && h.drag > 0) {
        armDrag(false);
        move.arm(h);
        return null;
      }
      move.cancel();
      return placeChain(count, pt, h);
    });
    // 末尾那次 focus 是这条路的必要条件：canvas 与 div 都不可聚焦，浏览器在 mousedown
    //   的默认行为里会把 activeElement 打回 body，此后 keydown 再也到不了宿主。
    env.focus();
    ev.preventDefault();
    return keep(chain);
  }

  /**
   * 命中之后的三条去向：双击选词、三击选段（表内选整格）、单击落光标。
   * @param count 这是第几击
   * @param pt 文档坐标（设备像素）
   * @param h 这一下的命中结果（与上面那两条判断共用同一次命中）
   * @returns 这一下的收尾
   */
  function placeChain(count: number, pt: { x: number; y: number }, h: Hit | null): Maybe<any> {
    let chain: Maybe<any>;
    if (count >= 3) {
      // 三击：表格里选整格、正文里选整段。但先看命中 —— 控件与图片是"格子里的东西"，
      //   连点两下选项之后第三下仍落在控件上时，那一下表达的是"再点一次这个选项"，
      //   不是"我要这一格"。连击计数在这里断掉，后面按普通落点走（拖动也就照常起手）。
      click.seq = 0;
      if (hitsInlineObject(h)) {
        armDrag(true);
        chain = placeAt(pt, h);
      } else {
        // 走命令而不是查询：选整格会重画帧，查询只回一个值、不重贴脏页，
        //   画面于是停在旧帧上，看起来是"点了要等一秒才选中"。
        chain = then1(env.write('selectPara', env.docPt(pt)), (ok) => {
          if (ok) {
            // 选中之后照样起拖：三击再往下拖表达的是"从这里扩展"，而不是"钉住这一段"。
            armDrag(true);
            env.cellSelected();
            return null;
          }
          armDrag(true);
          return placeAt(pt, h);
        });
      }
    } else if (count === 2) {
      // 双击：按词选。同样先避开控件与图片 —— 双击选项在 Word 里也不是选词。
      if (hitsInlineObject(h)) {
        armDrag(true);
        chain = placeAt(pt, h);
      } else {
        chain = then1(env.write('selectWord', env.docPt(pt)), (ok) => {
          if (ok) {
            // 选中之后照样起拖：双击是"选一个词"，双击再往外拖是"从这儿开始选"。
            //   挂上拖选还兼着一个作用：连点两下之后再拖不能只当成第二击 —— 否则
            //   "按下不拖"与"按下就拖"混成一件事，拖选再也起不来。
            armDrag(true);
            return h;
          }
          armDrag(true);
          return placeAt(pt, h);
        });
      }
    } else {
      chain = placeAt(pt, h);
    }
    return keep(chain);
  }

  /**
   * 搬运动作的按键跟踪：按下这一拍挂上监听，命中回来之后再由 arm 决定它算不算搬。
   *
   * 为什么不把"挂监听"推迟到命中回来之后：一次快速单击是"按下、抬起"背靠背，
   *   命中在异步宿主上要一次往返 —— 等回来再挂，那次抬手永远收不到，于是这一下既
   *   没落光标也没搬家（症状：点控件内部清不掉已有选区）。
   *
   * 拖动过程中什么都不提交：每帧提交要重排重画，撤销栈也会留下几十步。
   * 没动就还给单击：按在选区里点一下表达的是"把光标放这儿"，不是"搬到原地"。
   *   不区分这两件事的话，选区里任何一次点击都会变成一次搬家（位置没变、撤销栈里
   *   却多一步），而且再也点不掉已有选区。
   * @param start 按下的位置（文档设备像素）
   * @returns arm（这一下是搬，带命中）与 cancel（不是搬）
   */
  function trackMove(start: { x: number; y: number }): {
    arm(h: Hit): void;
    cancel(): void;
    afterUp(): boolean;
  } {
    let hit: Hit | null = null;
    let moved = false;
    let last = start;
    let released = false;
    let settled = false;
    const setCursor = (c: string): void => {
      if (typeof env.setCursor === 'function') env.setCursor(c);
    };
    const detach = (): void => {
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      setCursor('');
    };
    /**
     * 收尾：没动过就按单击落点，动过就落地一次搬运。
     */
    const settle = (): void => {
      if (settled) return;
      settled = true;
      detach();
      const h = hit;
      if (!moved || h === null) {
        keep(placeAt(start, h));
        return;
      }
      const dest = env.docPt(last);
      const done =
        h.drag === 2 && typeof h.dragImage === 'number'
          ? env.write('moveImage', { i: h.dragImage, x: dest.x, y: dest.y })
          : env.write('moveSelection', { x: dest.x, y: dest.y });
      keep(
        then1(done, (changed) => {
          if (changed) env.commit(true);
          return changed;
        }),
      );
    };
    const onMove = (e: MouseEvent): void => {
      if (typeof e.buttons === 'number' && (e.buttons & 1) === 0) {
        // 抬手丢了（在窗口外松手）也要能收尾：移动事件自带按键状态。
        released = true;
        if (hit !== null) settle();
        else detach();
        return;
      }
      const p = env.pointOf(e);
      if (!p) return;
      if (!moved) {
        // 门槛与拖选同一个数：手抖一像素不该把一段字搬到一像素之外。
        const dx = p.x - start.x;
        const dy = p.y - start.y;
        if (dx * dx + dy * dy < kMoveSlop * kMoveSlop) return;
        moved = true;
      }
      last = p;
    };
    const onUp = (): void => {
      released = true;
      if (hit !== null) settle();
      else detach();
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    return {
      arm(h: Hit): void {
        if (settled) return;
        hit = h;
        setCursor('move');
        // 抬手已经先到了：这一下当场收尾（否则它永远等不到第二次抬手）。
        if (released) settle();
      },
      cancel(): void {
        if (!settled && !released) detach();
      },
      afterUp(): boolean {
        return released;
      },
    };
  }

  /**
   * 滚轮缩放：Ctrl（或触控板捏合，浏览器同样带 ctrlKey）时接管，其余放行给宿主的滚动。
   *
   * 触控板捏合会给一串密集的小 deltaY，这里按符号取一档 —— 合并交给宿主：
   *   每一下都整篇重排的话，捏合一次就会排队排几十遍。
   */
  function onWheel(ev: WheelEvent): boolean {
    if (!ev.ctrlKey && !ev.metaKey) return false;
    if (!env.isReady()) return false;
    const dir = ev.deltaY < 0 ? 1 : ev.deltaY > 0 ? -1 : 0;
    if (dir === 0) return true;
    env.zoomBy?.(dir * kZoomStep);
    return true;
  }

  return {
    onKeyDown,
    onCompositionStart,
    onCompositionUpdate,
    onCompositionEnd,
    onPointerMove,
    onPointerLeave,
    onPointerDown,
    onWheel,
    placeAt,
    clickState: click,
    get composing(): boolean {
      return composing;
    },
  };
}
