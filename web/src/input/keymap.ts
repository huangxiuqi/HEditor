// 输入层的判定部分：键位映射、命中归宿、连击计数、缩放口径、屏幕像素比跟随。
// 这些函数决定的是"这一下算哪条命令"，与"谁持有画布、结果何时到"无关 ——
//   后者从环境对象进来（见 ./input.ts）。两条纪律：这里不出现引擎句柄，也不出现
//   具体画布元素 —— 一旦出现，两个宿主又各有一套；写入一律按"同步值或 Promise"
//   处理（then1），不把同步宿主逼成微任务，因为单线程宿主靠"调用即生效"当对照物，
//   那条性质不能为了统一形状而丢掉。

/**
 * 同步值或 Promise。两套传输共用一套判定，差别只在返回的是哪一种。
 */
export type Maybe<T> = T | Promise<T>;

/**
 * 命令参数：键名由命令表那一列与调用方约定，这里不逐个建模。
 */
export type Args = Record<string, any>;

/**
 * 热读：方向键怎么映射取决于当前段是横排还是竖排、竖排列序朝哪边。
 */
export interface HotState {
  vertical?: boolean;
  blockReversed?: boolean;
}

/**
 * 引擎的命中结果（hitTest 的载荷）。字段有值就代表那一种归宿。
 */
export interface Hit {
  para: number;
  offset: number;

  /**
   * 相邻字段共享边界时要靠它分辨点的是哪一侧。
   */
  side?: number;

  /**
   * 折行处同一个偏移压着两行，要带行别。
   */
  line?: number;

  /**
   * 点在格子边距带上：这一下表达的是"选整格"。
   */
  cellPara?: number;

  /**
   * 落点归属的控件下标（-1 或没有这个键 = 不在控件里）。
   */
  field?: number;

  /**
   * 组控件里点在第几个选项上（没有这个键 = 不在组控件上）。
   */
  opt?: number;

  /**
   * 点在一张图片上时给出它的下标（-1 或没有这个键 = 不在图片上）。
   */
  img?: number;

  /**
   * 点在图柄上时给出把手序号（0 左上顺时针），否则 -1。
   */
  imgHandle?: number;

  /**
   * 这一点按下去能不能起一次拖拽移动：1 拖当前文字选区、2 拖已选中的那张图。
   * 没有这个键 = 不能拖（按在选区外面、或按在未选中的图上）。
   *
   * 与落点同一次命中：分成两条命令问，宿主就要多两次往返才轮得到落光标，
   * 而按下到第一次移动之间往往只隔几毫秒 —— 拖选与把手都来不及起手。
   */
  drag?: number;

  /**
   * drag 为 2 时被拖的那张图的下标。
   */
  dragImage?: number;

  /**
   * 压在表格内部的格线上时给出表的身份与两条边界的序号（拖列宽 / 行高用）。
   *
   * gridCol 为 0 表示没压着列边界；gridRow 为 -1 表示没压着行边界。
   * 三格齐全才出现，缺了就是"这一下不归格线"。
   */
  gridTable?: number;
  gridCol?: number;
  gridRow?: number;
}

/**
 * 落光标（可带侧别 / 行别）。
 */
export interface CaretAction {
  kind: 'caret';
  para: number;
  offset: number;
  side?: number;
  line?: number;
}

/**
 * 选整格：点的是格子边距带。
 */
export interface CellAction {
  kind: 'cell';
  para: number;
}

/**
 * 命中之后的两条去向。
 */
export type HitAction = CellAction | CaretAction;

/**
 * 连击计数状态，由调用方持有。
 */
export interface ClickState {
  seq: number;
  time: number;
  x: number;
  y: number;
}

/**
 * 把同步值与 Promise 折成一条链。
 *
 * 不用 Promise.resolve 包一层：那会把同步宿主也推进微任务，于是"输入事件处理完
 *   就已生效"这条性质消失，探针的同步假设连带失效（一轮下来耗时数倍、增量断言变红）。
 *   同步值当场回调，Promise 才等 —— 两个宿主的执行时序各自保持原样。
   * @param v 同步值或 Promise
   * @param fn 拿到值之后做什么
   * @returns 同步值或 Promise（与输入同形）
 */
export function then1<V, R>(v: Maybe<V>, fn: (v: V) => Maybe<R>): Maybe<R> {
  return v && typeof (v as Promise<V>).then === 'function'
    ? (v as Promise<V>).then(fn)
    : fn(v as V);
}

/**
 * 连击计数。st 由调用方持有（{ seq, time, x, y }）。
 *
 * 单线程宿主的 mousedown 本来带 detail，仍然自己数：另一侧挂在 pointerdown 上，
 * detail 恒为 0。两处各判一套，迟早出现"这边三击能选、那边没反应"。
   * @param ev 带客户端坐标的事件
   * @param st 连击计数状态（调用方持有）
   * @returns 这是第几击（1 起）
 */
export function clickCountOf(ev: { clientX: number; clientY: number }, st: ClickState): number {
  const now = typeof performance !== 'undefined' ? performance.now() : Date.now();
  const near =
    st.seq > 0 &&
    now - st.time <= 520 &&
    Math.abs(ev.clientX - st.x) <= 4 &&
    Math.abs(ev.clientY - st.y) <= 4;
  st.seq = near ? st.seq + 1 : 1;
  st.time = now;
  st.x = ev.clientX;
  st.y = ev.clientY;
  return st.seq;
}

/**
 * markdown 还原成纯段落。
 *
 * 病历样例走的是"服务端下发一份文本"，两份实现一旦分叉，同一份样例在两个宿主上
 * 会排出不同段数 —— 而段落数是探针的基准量之一。
   * @param raw 原始文本
   * @returns 每段一行的纯文本
 */
export function cleanMarkdownLines(raw: string): string {
  return raw
    .split('\n')
    .map((l) =>
      l
        .replace(/\*\*/g, '')
        .replace(/\s*\\$/, '')
        .replace(/^#+\s*/, '')
        .replace(/^\d+[.、]\s*/, '')
        .trim(),
    )
    .filter((l) => l.length > 0)
    .join('\n');
}

/**
 * 跟随屏幕像素比。返回摘除函数。
 *
 * 判据取 devicePixelRatio 本身，不取 resize 事件：拖窗口大小也会来 resize，
 * 那时 DPR 没变，重排一遍纯属白花。媒体查询按 Ndppx 精确匹配，每变一次要重挂，
 * 否则下一次不再触发。
   * @param onChange 屏幕像素比变了要做什么
   * @returns 拆掉监听的函数
 */
export function armDprWatch(onChange: (dpr: number) => void): () => void {
  let q: MediaQueryList | null = null;
  const arm = (): void => {
    if (q) q.removeEventListener('change', onRehit);
    const dpr = window.devicePixelRatio || 1;
    q = window.matchMedia('(resolution: ' + dpr + 'dppx)');
    q.addEventListener('change', onRehit);
  };
  function onRehit(): void {
    arm();
    onChange(window.devicePixelRatio || 1);
  }
  arm();
  return () => {
    if (q) q.removeEventListener('change', onRehit);
    q = null;
  };
}

/**
 * 键位到命令。null 表示这个键放行给浏览器。
 *
 * ctx 是宿主的热读（单线程读引擎、异步宿主读镜像）：方向键按物理方向映射 —— 横排左右
 *   走字、上下换行，竖排翻过来；换列往哪边由列序定（中日竖排自右向左、蒙文自左向右），
 *   只按"是竖排吗"映射会把蒙文的左右按反。
 *
 * @param ev 键盘事件
 * @param ctx 热读（竖排、列序）
 * @returns 命令与参数；不认识的键返回 null
 */
export function keyToOp(ev: KeyboardEvent, ctx?: HotState): [string, Args] | null {
  if ((ev.ctrlKey || ev.metaKey) && !ev.altKey) {
    const k = (ev.key || '').toLowerCase();
    // 重做收两个键位：Ctrl+Y 与 Ctrl+Shift+Z。比较前转小写 —— shift 按下时给的是大写 Z。
    // Cmd 一并认下：Mac 上 Command 才是惯用修饰键。
    if (k === 'z' || k === 'y') return [ev.shiftKey || k === 'y' ? 'redo' : 'undo', {}];
    // 全选与"到文档首尾"必须在这里拦下。放行给浏览器时，全选选中的是承接层里那一行
    // 假文本（空的），与文档毫无关系；Ctrl+Home/End 则会把整页滚到顶/底。
    if (k === 'a') return ['selectAll', {}];
    if (ev.key === 'Home') return ['docStart', { extend: ev.shiftKey }];
    if (ev.key === 'End') return ['docEnd', { extend: ev.shiftKey }];
  }
  if (ev.ctrlKey || ev.metaKey || ev.altKey) return null;
  const c = ctx ?? {};
  const dirLeft = c.blockReversed ? 1 : -1; // 物理向左 = 行序 +1 还是 -1
  switch (ev.key) {
    case 'Backspace':
      return ['backspace', {}];
    case 'Delete':
      return ['deleteForward', {}];
    // extend = Shift：锚点不动，选区从锚点那一端继续长。
    case 'ArrowLeft':
      return c.vertical
        ? ['vertical', { dir: dirLeft, extend: ev.shiftKey }]
        : ['move', { dcp: -1, extend: ev.shiftKey }];
    case 'ArrowRight':
      return c.vertical
        ? ['vertical', { dir: -dirLeft, extend: ev.shiftKey }]
        : ['move', { dcp: 1, extend: ev.shiftKey }];
    case 'ArrowUp':
      return c.vertical
        ? ['move', { dcp: -1, extend: ev.shiftKey }]
        : ['vertical', { dir: -1, extend: ev.shiftKey }];
    case 'ArrowDown':
      return c.vertical
        ? ['move', { dcp: 1, extend: ev.shiftKey }]
        : ['vertical', { dir: 1, extend: ev.shiftKey }];
    // 行首行尾走引擎的现成命令，不用 moveCaret(-1e9) 凑：那个表达的语义是段首段尾。
    case 'Home':
      return ['lineStart', { extend: ev.shiftKey }];
    case 'End':
      return ['lineEnd', { extend: ev.shiftKey }];
    // 回车 = 分段（跨段编辑），不是"确认/提交"。段落模型是文档流的单位，
    // 回车在光标处断段，后面那段整体继承样式。软换行要的是"行内换行"这种与段落
    // 不同的东西，现在没有，就不假装有。
    case 'Enter':
      return ['splitParagraph', {}];
    // Tab = 缩进（编辑器语义），不是"跳到下一个控件"。
    case 'Tab':
      return [ev.shiftKey ? 'outdent' : 'indent', {}];
    default:
      // 可打印字符（ASCII / 标点 / 空格）直接在 keydown 里提交，并挡掉浏览器往
      // 承接层里的插入。不能指望 input 事件：合成 KeyboardEvent 根本不产生 input，
      // 真键盘下又因为这里挡掉了插入 —— 两头都不成立，打字会静默地什么都不发生。
      if (ev.key && ev.key.length === 1) return ['type', { s: ev.key }];
      return null;
  }
}

/**
 * 命中结果到落点。
 *
 * 三种归宿，判据都在引擎给出的字段上：
 *   cellPara 有值 —— 点的是格子边距带，这一下是"选整格"，不是落光标；
 *   side 有值 —— 落光标并带上侧别；相邻字段共享边界时同一个偏移上压着好几枚括号，
 *               光有偏移分不出点的是哪一侧，两个控件之间整段都点不进去；
 *   line 与 side 同时给 —— 折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
 *               不带行别，点下一行行首会画到上一行行尾。
 * 搬的是段号而不是流区间：空格的内容长度为 0，流区间分不出是哪一格。
   * @param hit 命中结果
   * @returns 落点动作；落不进正文时返回 null
 */
export function hitToAction(hit: Hit | null | undefined): HitAction | null {
  if (!hit) return null;
  if (typeof hit.cellPara === 'number') return { kind: 'cell', para: hit.cellPara | 0 };
  const act: CaretAction = { kind: 'caret', para: hit.para, offset: hit.offset };
  if (typeof hit.side === 'number') {
    act.side = hit.side;
    if (typeof hit.line === 'number') act.line = hit.line | 0;
  }
  return act;
}

/**
 * 缩放口径的两个读数。
 */
export interface ZoomControl {
  /**
   * 当前密度（设备像素 / 磅）。
   */
  readonly density: number;

  /**
   * 现有密度是按哪个屏幕算的。换屏判断要跟它比，不要跟当下 DPR 比。
   */
  readonly dpr: number;

  /**
   * 当下屏幕的基准密度：倍数 1 对应的密度。
   */
  base(): number;

  /**
   * 倍数转密度。
   *
   * @param f 倍数（1 = 100%）
   * @returns 密度（设备像素 / 磅）
   */
  ofFactor(f: number): number;

  /**
   * 密度转倍数。
   *
   * @param d 密度（设备像素 / 磅）
   * @returns 倍数（1 = 100%）
   */
  factorOf(d: number): number;

  /**
   * 改密度。返回新密度，宿主只管把它存进自己的变量。
   *
   * @param d 密度（设备像素 / 磅）
   * @returns 记下来的密度
   */
  set(d: number): number;
}

/**
 * 缩放换算。口径只有一条：引擎收"密度"（设备像素 / 磅，1 = 72 dpi），
 * 界面收"倍数"（1 = 100% = 96 dpi），两者差 4 / 3。
 *
 * 两个 DPR 必须分开记，这是这一层唯一的难点：
 *   liveDpr   当下屏幕的像素比。算"基准密度"与"倍数转密度"用它 —— 换屏之后新算的
 *             密度就该按新屏幕来。
 *   storedDpr 现有这个密度是按哪个屏幕算出来的。反推倍数用它 —— 换屏那一拍两者不同，
 *             拿新 DPR 去除会读到一个平白缩掉一半的假倍数，照它反推回去等于空操作。
 * 所以 set(density) 要同时把 storedDpr 记成"算这一刻用的那个屏幕"。
   * @param opts 基准密度与 dpr 的取值方式
   * @returns 密度 / 倍数的换算器
 */
export function createZoom(opts?: { base?: number; dpr?: () => number }): ZoomControl {
  const base = opts?.base ?? 4 / 3;
  const liveDpr = opts?.dpr ?? (() => window.devicePixelRatio || 1);
  let storedDpr = liveDpr();
  let density = base * storedDpr;
  return {
    get density(): number {
      return density;
    },
    get dpr(): number {
      return storedDpr;
    },
    base(): number {
      return base * liveDpr();
    },
    /**
     * 倍数 -> 密度。
     *
     * @param f 倍数（1 = 100%）
     * @returns 密度（设备像素 / 磅）
     */
    ofFactor(f: number): number {
      return Number(f) * base * liveDpr();
    },
    /**
     * 密度 -> 倍数。
     *
     * @param d 密度（设备像素 / 磅）
     * @returns 倍数（1 = 100%）
     */
    factorOf(d: number): number {
      return Number(d) / (base * storedDpr);
    },
    /**
     * 记下新的密度（同时刷新记住的 dpr）。
     *
     * @param d 密度（设备像素 / 磅）
     * @returns 记下来的密度
     */
    set(d: number): number {
      storedDpr = liveDpr();
      density = Number(d);
      return density;
    },
  };
}
