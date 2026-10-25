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
   */
  placeAt(pt: { x: number; y: number }): Maybe<Hit | null>;
  readonly clickState: ClickState;
  readonly composing: boolean;
}

/**
 * 一档缩放。与示例页工具栏的加减号同一个步长，两处按下去的手感才一致。
 */
const kZoomStep = 0.125;

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
 * 造输入层。
 * @param env 宿主注入的环境（读写、命中、坐标换算、收尾）
 * @returns 键盘 / 合成 / 指针三个入口
 */
export function createInput(env: InputEnv): InputLayer {
  const click: ClickState = { seq: 0, time: 0, x: 0, y: 0 };
  let composing = false;
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
   * 图片把手的拖拽：按下时取一次起始尺寸与指针位置，之后每帧按增量算新尺寸。
   *
   * 等比 / 不等比只看把手落在哪：四个角按原图比例推另一边，四条边只动一个轴。
   * 角上按住 Shift 解除比例约束 —— "角上默认等比、边上默认拉伸"是字处理软件的通行手感。
   */
  function beginImageResize(pt: { x: number; y: number }, index: number, handle: number): void {
    if (typeof env.resizeImage !== 'function' || typeof env.images !== 'function') return;
    const images = env.images;
    const resize = env.resizeImage;
    keep(
      then1(images(), (list) => {
        const box = (list || []).find((b) => b.index === index);
        if (!box || !(box.w > 0) || !(box.h > 0)) return null;
        const start = env.docPt(pt);
        const w0 = box.w;
        const h0 = box.h;
        const ratio = w0 / h0;
        const left = handle === 0 || handle === 6 || handle === 7;
        const right = handle === 2 || handle === 3 || handle === 4;
        const top = handle === 0 || handle === 1 || handle === 2;
        const bottom = handle === 4 || handle === 5 || handle === 6;
        const corner = (left || right) && (top || bottom);
        let first = true;
        const onMove = (e: MouseEvent): void => {
          const p = env.pointOf(e);
          if (!p) return;
          const cur = env.docPt(p);
          const dx = cur.x - start.x;
          const dy = cur.y - start.y;
          let w = w0;
          let h = h0;
          if (right) w = w0 + dx;
          else if (left) w = w0 - dx;
          if (bottom) h = h0 + dy;
          else if (top) h = h0 - dy;
          if (corner && !e.shiftKey) h = w / ratio;
          if (!(w > 0) || !(h > 0)) return;
          // 下限 8 磅：再小就没有可点的图身与把手，拖到零之后再也拖不回来。
          const nw = Math.max(8, Math.round(w * 100) / 100);
          const nh = Math.max(8, Math.round(h * 100) / 100);
          // 第一帧不带 merge：一次拖拽要在撤销栈里留下自己的一步，不能并进上一次。
          keep(resize(index, nw, nh, !first));
          first = false;
        };
        const onUp = (): void => {
          window.removeEventListener('mousemove', onMove);
          window.removeEventListener('mouseup', onUp);
          // 尺寸真的变过才收尾一次：change 事件按"一次拖拽一次"发，不是按帧发。
          if (!first) env.commit(true);
        };
        window.addEventListener('mousemove', onMove);
        window.addEventListener('mouseup', onUp);
        return null;
      }),
    );
  }

  // 命中之后的两条去向：定位、以及"这一下要不要起拖选"。
  // 落在正文之外时不起手 —— 从页外空白拖起不该选中任何东西。
  // 定位这一步必须等：异步宿主的落光标是一次往返，不等它，clickAt 这类入口会在
  // 光标还没摆好时就 resolve，调用方读到的光标是上一拍的。
  const placeAt = (pt: { x: number; y: number }): Maybe<Hit | null> =>
    then1(env.hit(pt.x, pt.y), (h) => {
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
    const triple = clickCountOf(ev, click) === 3;
    let chain: Maybe<any>;
    if (triple) {
      click.seq = 0;
      // 三击：选整格。走命令而不是查询 —— 选整格会重画帧，查询只回一个值、不重贴
      // 脏页，画面于是停在旧帧上，看起来是"点了要等一秒才选中"。点不在表格里时
      // 退回正常落光标，所以 beginDrag 放在回调里而不是前面。
      chain = then1(env.write('cellSelectAt', env.docPt(pt)), (ok) => {
        if (ok) {
          env.cellSelected();
          return null;
        }
        beginDrag(ev);
        return placeAt(pt);
      });
    } else {
      // 监听必须当场挂上：命中在异步宿主上是往返，等它回来才起手的话，
      // 按下与第一次移动之间的移动会被整段丢掉，而那一段往往正是"按下去就往右拖"。
      beginDrag(ev);
      chain = placeAt(pt);
    }
    // 末尾那次 focus 是这条路的必要条件：canvas 与 div 都不可聚焦，浏览器在 mousedown
    // 的默认行为里会把 activeElement 打回 body，此后 keydown 再也到不了宿主。
    env.focus();
    ev.preventDefault();
    return keep(chain);
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
    onPointerDown,
    onWheel,
    placeAt,
    clickState: click,
    get composing(): boolean {
      return composing;
    },
  };
}
