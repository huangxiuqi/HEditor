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

export interface InputEnv {
  isReady(): boolean;
  isBusy?(): boolean;
  /**
   * 承接键盘与合成的 textarea。
   */
  ta: HTMLTextAreaElement;
  /**
   * 热读：竖排 / 列序（决定方向键映射）。
   */
  hot(): HotState;
  write(name: string, args?: Args): Maybe<any>;
  /**
   * 命中。收设备像素（宿主内部折算成文档坐标，与拖选同一口径）。
   */
  hit(xPx: number, yPx: number): Maybe<Hit | null>;
  /**
   * 设备像素 -> 文档坐标。写类命令（如 cellSelectAt）要自己传坐标时用它。
   */
  docPt(pt: { x: number; y: number }): { x: number; y: number };
  pointOf(ev: { clientX: number; clientY: number }): { x: number; y: number } | null;
  capturePointer(): void;
  beginDrag(ev: { clientX: number; clientY: number }): void;
  /**
   * 拖选控制器（宿主造的）；没有就不拖。
   */
  drag?: { arm(on: boolean): void };
  focus(): void;
  commit(changed: boolean): void;
  place(act: CaretAction): Maybe<any>;
  selectCellPara(para: number): Maybe<any>;
  cellSelected(): void;
  status(text: string, isErr?: boolean): void;
  onChain?(p: Maybe<any>): void;
}

export interface InputLayer {
  onKeyDown(ev: KeyboardEvent): Maybe<any>;
  onCompositionStart(): void;
  onCompositionUpdate(ev: CompositionEvent): void;
  onCompositionEnd(ev: CompositionEvent): Maybe<any>;
  onPointerDown(ev: MouseEvent): Maybe<any>;
  placeAt(pt: { x: number; y: number }): Maybe<Hit | null>;
  readonly clickState: ClickState;
  readonly composing: boolean;
}

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

  // 命中之后的两条去向：定位、以及"这一下要不要起拖选"。
  // 落在正文之外时不起手 —— 从页外空白拖起不该选中任何东西。
  // 定位这一步必须等：异步宿主的落光标是一次往返，不等它，clickAt 这类入口会在
  // 光标还没摆好时就 resolve，调用方读到的光标是上一拍的。
  const placeAt = (pt: { x: number; y: number }): Maybe<Hit | null> =>
    then1(env.hit(pt.x, pt.y), (h) => {
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
    const op = keyToOp(ev, env.hot());
    if (!op) return undefined; // 不认识的键（含 IME 的 Process 键）放行
    ev.preventDefault();
    return keep(then1(env.write(op[0], op[1]), (changed) => env.commit(!!changed)));
  }

  function onCompositionStart(): void {
    composing = true;
    env.ta.value = '';
    env.status('输入法组合中…（此期间不触碰引擎）');
  }

  function onCompositionUpdate(ev: CompositionEvent): void {
    // 预编辑串先在状态栏本地预览。渲染进正文需要"组合期间不改布局"的完整版本，
    // 现在没有那一档。
    env.status('组合中：' + (ev.data || ''));
  }

  function onCompositionEnd(ev: CompositionEvent): Maybe<any> {
    composing = false;
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

  return {
    onKeyDown,
    onCompositionStart,
    onCompositionUpdate,
    onCompositionEnd,
    onPointerDown,
    placeAt,
    clickState: click,
    get composing(): boolean {
      return composing;
    },
  };
}
