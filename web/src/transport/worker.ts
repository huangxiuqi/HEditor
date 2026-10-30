// Worker 传输：引擎在另一条线程上，主线程只持镜像与画布。
// 与同线程传输的区别只有两处，都收在 EngineLink 里：结果何时到（命令返回 Promise，
//   worker 推回 state 才 resolve）与片元在哪贴（画布 transfer 给 worker，那一侧自己贴；
//   pixels 恒为 null）。
// 镜像（mirror）是"同步查询"的代价：caret / selection / geometry / text 都是同步读，
//   而跨线程读只能异步 —— 所以 worker 每一拍把这几样随状态一起推过来（见 api 的
//   sendState）。推的都是小对象，省掉的是每拍一次 200 µs 的往返。
import type { DomHost, EngineLink, HostView } from '../host/dom';
import { SlotCanvases, type CanvasStore } from '../host/pages';
import { stageOf } from '../host/parts';
import type { Args } from '../input/keymap';
import type { AssetPaths } from './assets';

const CALL_TIMEOUT_MS = 15000;

export interface WorkerOptions {
  container: HTMLElement;
  assets: AssetPaths;
  dpr: number;

  /**
   * 密度（设备像素 / 磅）。
   */
  density: number;
  empty?: boolean;

  /**
   * 宿主由这一侧造（它需要 link 与画布存储，而两者都由这一侧造）。
   * 画布存储必须延迟到宿主建好标记之后再造 —— 它要往页容器里塞元素。
   * 第三个参数是远端的贴图账（账在 Worker 那边，宿主自己记只会记到零）。
   * @param link 传输层交出来的入口
   * @param canvasStore 画布存储的造法
   * @param blitStats 读远端贴图账
   */
  makeHost(
    link: EngineLink,
    canvasStore: (pagesEl: HTMLElement) => CanvasStore,
    blitStats: () => { ms: number; pages: number; px: number },
  ): DomHost;

  /**
   * 引擎整条不可用了（Worker 崩溃、装载失败）。创建期的失败以 createEditor 拒绝的形式
   * 到达调用方；这里管"起来之后才死"——那时没有 Promise 可拒，只剩这个回调。
   * @param message 失败原因（已格式化）
   */
  onFatal?(message: string): void;
}

/**
 * worker 推回来的那一份状态（只列主线程要用的）。
 */
interface Mirror {
  ready: boolean;
  stats: any;
  geom: any;
  layout: any;
  selection: any;
  controls: any;
  textAll: string;
  blockedEdits: number;
  pagesWrittenThisFrame: number[];
  caretReport: string;
  caretLive: number;
  caretVertical: boolean;
  caretBlockReversed: boolean;
  caretStyle: any;
  selectionStyle: any;

  /**
   * 已注册的字体清单。
   *
   * 必须是同步读：工具栏在启动时按它建字体下拉框，等一次往返会让"字体"那一格先空着
   *   再弹出来；注册新字体之后它也要立刻可读（宿主拿返回值直接用）。
   */
  fonts: any[];
  heapMB: number;
  scrollReset: boolean;

  /**
   * 这一拍 Worker 贴了几页、写了多少像素、花了多少毫秒。
   *
   * 账在贴图那一侧 —— 主线程没有 putImageData 可记。不推过来的话 blitStats 恒为
   *   {0,0,0}，而"按脏矩形贴"的判据正是靠它：一个恒报 0 的计数器让"上屏面积只有
   *   几十像素"与"贴了整页"看起来一样。
   */
  blit: { ms: number; pages: number; px: number };
}

/**
 * 造 Worker 传输：引擎在另一条线程上，主线程只持镜像与画布。
 * @param opts 容器、素材、缩放与造宿主的方法
 * @returns link（异步）、host、start 与 destroy
 */
export function createWorker(opts: WorkerOptions): {
  link: EngineLink;
  host: DomHost;
  start(): Promise<void>;
  destroy(): void;
} {
  const { container, assets } = opts;
  const dpr = (): number => window.devicePixelRatio || 1;

  const mirror: Mirror = {
    ready: false,
    stats: null,
    geom: null,
    layout: null,
    selection: null,
    controls: null,
    textAll: '',
    blockedEdits: 0,
    pagesWrittenThisFrame: [],
    caretReport: '',
    caretLive: -1,
    caretVertical: false,
    caretBlockReversed: false,
    caretStyle: null,
    selectionStyle: null,
    fonts: [],
    heapMB: 0,
    scrollReset: false,
    blit: { ms: 0, pages: 0, px: 0 },
  };

  const workerUrl = assets.worker ?? new URL('heditor_worker.js', document.baseURI).href;
  const worker = new Worker(workerUrl);
  let dead = false;
  let disposed = false;
  let opSeq = 0;
  let askSeq = 0;
  const pending = new Map<
    number,
    { resolve: (m: any) => void; reject: (e: Error) => void; timer: number; name: string }
  >();
  const pendingReply = new Map<
    number,
    { resolve: (v: any) => void; reject: (e: Error) => void; timer: number; name: string }
  >();
  const errors: string[] = [];

  const post = (msg: Args, transfer?: Transferable[]): void => {
    worker.postMessage(msg, transfer ?? []);
  };

  const viewSize = (): { w: number; h: number } => {
    const el = stageOf(container) ?? container;
    return {
      w: Math.max(1, Math.round(el.clientWidth * dpr())),
      h: Math.max(1, Math.round(el.clientHeight * dpr())),
    };
  };

  /**
   * 在途清空时唤醒的等待者（flush 用）。
   *
   * 为什么不轮询：flush 的语义是"等队列排空"，轮询要么多等一拍、要么漏掉刚刚
   *   落地的那一批。这里由回话的处理点直接唤醒，条件一成立就是当前这一拍。
   */
  let idleWaiters: Array<() => void> = [];
  function maybeIdle(): void {
    if (pending.size || pendingReply.size) return;
    const w = idleWaiters;
    idleWaiters = [];
    for (const fn of w) fn();
  }

  /**
   * 把还在等的调用当场拒掉：出错的那一拍不会再有 state / ack 回来。
   */
  function rejectPending(reason: string): void {
    const e = new Error(reason);
    for (const [id, p] of [...pending]) {
      clearTimeout(p.timer);
      pending.delete(id);
      p.reject(e);
    }
    for (const [id, p] of [...pendingReply]) {
      clearTimeout(p.timer);
      pendingReply.delete(id);
      p.reject(e);
    }
    maybeIdle();
  }

  /**
   * 等队列排空：在途的写与查询都收到了回话，且那一条的状态已经落到镜像上。
   * @returns 排空时 resolve
   */
  function whenIdle(): Promise<void> {
    if (!pending.size && !pendingReply.size) return Promise.resolve();
    return new Promise<void>((r) => idleWaiters.push(r));
  }

  function waitOp(name: string, msg: Args): Promise<any> {
    if (dead) return Promise.reject(new Error(`引擎已不可用，命令 ${name} 未发送`));
    const id = ++opSeq;
    return new Promise((resolve, reject) => {
      const timer = window.setTimeout(() => {
        if (pending.delete(id)) reject(new Error(`${name} 超时（${CALL_TIMEOUT_MS} ms 没有回话）`));
      }, CALL_TIMEOUT_MS);
      pending.set(id, { resolve, reject, timer, name });
      post({ ...msg, ts: id });
    });
  }

  function ask(name: string, args?: Args): Promise<any> {
    if (dead) return Promise.reject(new Error(`引擎已不可用，查询 ${name} 未发送`));
    const id = ++askSeq;
    return new Promise((resolve, reject) => {
      const timer = window.setTimeout(() => {
        if (pendingReply.delete(id)) reject(new Error(`查询 ${name} 超时`));
      }, CALL_TIMEOUT_MS);
      pendingReply.set(id, { resolve, reject, timer, name });
      post({ t: 'ask', id, name, args: args ?? {} });
    });
  }

  const link: EngineLink = {
    read(name: string, args?: Args): any {
      switch (name) {
        case 'editStats':
          return mirror.stats;
        case 'stats':
          return mirror.layout;
        case 'textAll':
          return mirror.textAll;
        case 'text': {
          const i = Number(args?.i ?? 0) | 0;
          const parts = mirror.textAll.split('\n');
          return parts[i] ?? '';
        }
        case 'selectionReport':
          return mirror.selection;
        case 'controls':
          return mirror.controls;
        case 'blockedEdits':
          return mirror.blockedEdits;
        case 'pagesWrittenThisFrame':
          return mirror.pagesWrittenThisFrame;
        case 'caretReport':
          return mirror.caretReport;
        case 'caretOffsetLive':
          return mirror.caretLive;
        // 这两个是键位映射的热读，必须是同步布尔：hot() 拿它分支，而 ask 返回的
        //   Promise 恒为真 —— 少了这两条，Worker 下方向键全部按竖排文档处理
        //   （左右变成上下，位移读数为零），而 API 的 moveCaret 照样正常。
        case 'caretVerticalNow':
          return mirror.caretVertical;
        case 'caretBlockReversedNow':
          return mirror.caretBlockReversed;
        case 'caretStyleJson':
          return mirror.caretStyle;
        case 'selectionStyleJson':
          return mirror.selectionStyle;
        case 'fontList':
          return mirror.fonts;
        case 'heap':
          return mirror.heapMB * 1048576;
        case 'pagePxWidth':
          return mirror.geom?.pageW ?? 0;
        case 'pagePxHeight':
          return mirror.geom?.pageH ?? 0;
        case 'pageGapPx':
          return mirror.geom?.gap ?? 0;
        case 'windowFirstPage':
          return mirror.geom?.first ?? 0;
        case 'windowPageCount':
          return mirror.geom?.count ?? 0;
        case 'hit':
          // 命中是输入层的必经之路，不是诊断：少了它，Worker 模式下单击不落光标、
          //   拖选永远是空的。它必须是一次真往返（ask），不能落进下面的 default。
          return ask('hit', args);
        case 'textPosOf': {
          // 选区两端的位置随 selection 一起推过来（同步镜像）；别的流位置没有镜像，
          //   走一次真往返 —— 判据拿它把命中的流位置换成读数，不是每个位置都能预推。
          const sel = mirror.selection;
          const flow = Number(args?.flow ?? -1);
          if (sel) {
            if (flow === sel.anchor) return sel.anchorPos ?? null;
            if (flow === sel.focus) return sel.focusPos ?? null;
          }
          return ask('textPosOf', args);
        }
        default:
          // 其余读命令（审计、帧摘要、控件表……）没有本地镜像，走一次真往返。
          //   不能返回 null：诊断面在 worker 传输下会整体变成"读到 null 也算成功"，
          //   而 diag 的每个成员只做转发 —— 它没有别的地方能发现自己是空的。
          return ask(name, args);
      }
    },
    write(name: string, args?: Args) {
      return waitOp(name, { t: 'do', name, args: args ?? {} }).then((m) => !!m?.changed);
    },
    async settle() {
      // Worker 侧已经在 doOp 的收尾里 present + 贴页 + 回推，这一侧没有收尾可做 ——
      //   但要等那一拍的回推到达：write 是排队 + 回推 state，不等的话这里拿到的是
      //   上一拍的镜像（光标位置、页号、窗口全是旧的）。上层拿它决定"要不要滚视口"
      //   与"IME 摆哪"，多页文档上点第二页就会被拽回上一次编辑所在的那一页。
      // 返回值与同线程那一份同形（{ stats } 而不是 true）：调用方拿的是 st.stats，
      //   给一个布尔会让收尾拿到 true 当状态读 —— 光标位置、页号全是 undefined。
      await whenIdle();
      return { moved: true, scrolled: false, stats: mirror.stats };
    },
    view(scrollY: number) {
      const vs = viewSize();
      return waitOp('view', { t: 'view', scrollY, viewW: vs.w, viewH: vs.h }).then((m) => ({
        moved: !!(m && m.stats),
      }));
    },
    geom(): HostView {
      const g = mirror.geom;
      return {
        pw: g?.pageW ?? 0,
        ph: g?.pageH ?? 0,
        gap: g?.gap ?? 0,
        dpr: g?.dpr ?? dpr(),
        totalPages: Math.max(1, Number(mirror.stats?.totalPages ?? 1)),
        first: g?.first ?? 0,
        count: Math.max(1, g?.count ?? 1),
        // 连页的滚动高度按内容底边算（见 pages.sync）；分页下这两个字段都不作数。
        continuous: !!g?.continuous,
        contentPx: Number(g?.contentPx ?? 0),
      };
    },
    setViewMode(gap: number, continuous: boolean) {
      return waitOp('viewMode', { t: 'viewMode', gap, continuous }).then(() => true);
    },
    takeScrollReset() {
      const v = mirror.scrollReset;
      mirror.scrollReset = false;
      return v;
    },
    serialize() {
      return ask('serialize').then((v) => String(v ?? ''));
    },
    copy() {
      return ask('copy').then((v) => String(v ?? ''));
    },
    async flush() {
      // 排空 = 连"在途命令的续体又发出去的那一条"也等完。
      //
      //   只等当前在途那几条是不够的：命中 → reply 到了之后才决定"要不要落光标"，
      //   而那一条写是在 reply 的回调里（微任务）才发出去的。等在途清空的那一刻
      //   队列确实是空的，可下一微任务就会再塞一条进来 —— 于是 flush 返回时
      //   光标还没落位，调用方读到的是上一拍（这条曾被读成"点击根本不落光标"）。
      //   做法是"空一拍再看一次"：宏任务跑完时，续体已经把它要发的那条发出去了。
      for (let round = 0; round < 64; ++round) {
        if (pending.size || pendingReply.size) {
          await new Promise<void>((r) => idleWaiters.push(r));
        }
        await new Promise<void>((r) => setTimeout(r, 0));
        if (!pending.size && !pendingReply.size) return;
      }
    },
  };

  // 画布（按槽 transfer）

  let store: SlotCanvases | null = null;
  const host = opts.makeHost(link, (pagesEl) => {
    // 槽数上限：与引擎侧 kWindowMaxPages 同值。少建会让两页共用一个槽、那几页永远缺
    //   画布；多建没有意义（窗口不会那么长）。
    const s = new SlotCanvases(pagesEl, 8);
    s.onCreated = (slot, el, n) => {
      const off = el.transferControlToOffscreen();
      // 转移列表必须带上 —— 不带会抛 DataCloneError（OffscreenCanvas 不能按值克隆）。
      post({ t: 'pageCanvas', ts: 0, slot, slots: n, canvas: off, dpr: dpr() }, [off]);
    };
    store = s;
    return s;
  }, () => mirror.blit);

  // 消息

  function onState(m: Args): void {
    if (disposed) return;
    mirror.stats = m.stats ?? mirror.stats;
    mirror.geom = m.geom ?? mirror.geom;
    if (m.layout) mirror.layout = m.layout;
    if (m.selection) mirror.selection = m.selection;
    if (m.controls) mirror.controls = m.controls;
    if (typeof m.textAll === 'string') mirror.textAll = m.textAll;
    if (typeof m.blockedEdits === 'number') mirror.blockedEdits = m.blockedEdits;
    if (m.pagesWrittenThisFrame) mirror.pagesWrittenThisFrame = m.pagesWrittenThisFrame;
    if (typeof m.caretReport === 'string') mirror.caretReport = m.caretReport;
    if (typeof m.caretLive === 'number') mirror.caretLive = m.caretLive;
    if (typeof m.caretVertical === 'boolean') mirror.caretVertical = m.caretVertical;
    if (typeof m.caretBlockReversed === 'boolean') mirror.caretBlockReversed = m.caretBlockReversed;
    if (m.caretStyle) mirror.caretStyle = m.caretStyle;
    if (m.selectionStyle) mirror.selectionStyle = m.selectionStyle;
    // 空数组也是有效值（一份字体都没装误配时，宿主该看到"没有字体"而不是上一份清单）。
    if (Array.isArray(m.fonts)) mirror.fonts = m.fonts;
    if (typeof m.heapMB === 'number') mirror.heapMB = m.heapMB;
    // 引擎把滚动位置归零了：当场应用，不停在镜像里等下一次 finish。
    //   留在镜像里会被"下一次任意一拍"消费 —— 用户已经滚到第十页时来一份状态，视口
    //   被拽回顶部（多页文档上就是"点一下就乱跳"）。这里用掉即清，宿主侧不会再看到它。
    if (m.scrollReset) {
      host.resetScroll();
      mirror.scrollReset = false;
    }
    if (typeof m.blitPx === 'number') {
      mirror.blit = { ms: Number(m.blitMs ?? 0), pages: Number(m.blitPages ?? 0), px: m.blitPx };
    }

    if (m.ts) {
      const p = pending.get(m.ts as number);
      if (p) {
        clearTimeout(p.timer);
        pending.delete(m.ts as number);
        p.resolve(m);
      }
    }
    // 画布集合跟着窗口走：新进窗口的页要先建槽再转移，缺这一步那几页永远是空白。
    host.syncPages();
    host.finish(mirror.stats);
    // 普通写的回话点是这里，不是 ack：一次写只推一条 state，ts 挂在它上面。
    //   唤醒放在 finish 之后 —— flush 的语义是"连这一拍的状态都落地了"，早一点唤醒
    //   就会读到上一拍的几何（缩放后页宽还是旧值）。
    maybeIdle();
  }

  function onWorkerError(m: Args): void {
    const brief = String(m.message ?? '').split('\n')[0];
    const where = String(m.where ?? '');
    errors.push(where + ': ' + String(m.message ?? ''));
    // boot 失败与 Worker 自身出错 = 整条不可用；其余是那一条命令失败，引擎还活着。
    dead = where === 'worker.onerror' || /^boot/.test(where);
    rejectPending(`Worker 报错 ${where}：${brief}`);
    if (dead) opts.onFatal?.(`引擎已不可用（${where}）：${brief}`);
  }

  worker.onmessage = (ev: MessageEvent): void => {
    const m = (ev.data ?? {}) as Args;
    try {
      switch (m.t) {
        case 'state':
          onState(m);
          break;
        case 'ack': {
          const p = m.ts ? pending.get(m.ts as number) : null;
          if (p) {
            clearTimeout(p.timer);
            pending.delete(m.ts as number);
            p.resolve(null);
          }
          // 先 resolve 再唤醒 flush：等待者要在"这一条已经落地"之后才继续。
          maybeIdle();
          break;
        }
        case 'reply': {
          const p = pendingReply.get(m.id as number);
          if (p) {
            clearTimeout(p.timer);
            pendingReply.delete(m.id as number);
            if (m.error) p.reject(new Error(String(m.error)));
            else p.resolve(m.value);
          }
          maybeIdle();
          break;
        }
        case 'ready':
          mirror.ready = true;
          break;
        case 'error':
          onWorkerError(m);
          break;
        default:
          break;
      }
    } catch (e) {
      // 消息处理器里抛异常是无声的：调用方只会等到超时，而真相就在手边。
      onWorkerError({ where: 'host:' + String(m.t), message: String((e as Error)?.stack ?? e) });
    }
  };

  worker.onerror = (e: ErrorEvent): void => {
    onWorkerError({ where: 'worker.onerror', message: e.message || String(e) });
  };

  /**
   * 等 worker 报 ready（它先报 ready 再装载 —— 否则装载里一抛异常宿主就永远等不到）。
   */
  function waitReady(): Promise<void> {
    return new Promise((resolve, reject) => {
      const t0 = performance.now();
      const tick = (): void => {
        if (mirror.ready) return resolve();
        if (dead) return reject(new Error(errors.join(' | ') || 'Worker 启动失败'));
        if (performance.now() - t0 > CALL_TIMEOUT_MS) return reject(new Error('Worker 启动超时'));
        window.setTimeout(tick, 20);
      };
      tick();
    });
  }

  return {
    link,
    host,
    async start(): Promise<void> {
      const vs = viewSize();
      // 槽不在这里建：起来之前每页都没有内容，先摆一排空白页画布等于在失败页上画了
      //   一堆"看起来像是文档"的白块。第一次状态（只有 boot 成功排完版才会发）到达时
      //   由 onState 的 syncPages 建槽，那一拍之后画布才会被贴。
      post({
        t: 'boot',
        ts: 0,
        glue: assets.glue,
        // wasm 显式给了就按它取（开发服务器的根不是仓库根时靠这一条）。
        wasm: assets.wasm ?? '',
        bust: assets.bust ?? '',
        exportName: 'CreateHEditor',
        fonts: assets.fonts,
        fontMode: 'full',
        dpr: dpr(),
        viewW: vs.w,
        viewH: vs.h,
        density: opts.density,
        empty: !!opts.empty,
      });
      await waitReady();
      host.start();
      // 首帧：引擎装载时已经排完画完，这边只差"把像素搬上画布"。
      await waitOp('present', { t: 'do', name: 'present', args: {} });
      host.finish(mirror.stats);
    },
    destroy(): void {
      disposed = true;
      dead = true;
      rejectPending('编辑器已销毁');
      store?.destroy();
      worker.terminate();
    },
  };
}
