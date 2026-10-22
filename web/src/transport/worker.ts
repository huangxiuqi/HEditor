// Worker 传输：引擎在另一条线程上，主线程只持镜像与画布。
// 与同线程传输的区别只有两处，都收在 EngineLink 里：结果何时到（命令返回 Promise，
//   worker 推回 state 才 resolve）与片元在哪贴（画布 transfer 给 worker，那一侧自己贴；
//   pixels 恒为 null）。
// 镜像（mirror）是"同步查询"的代价：caret / selection / geometry / text 都是同步读，
//   而跨线程读只能异步 —— 所以 worker 每一拍把这几样随状态一起推过来（见 api 的
//   sendState）。推的都是小对象，省掉的是每拍一次 200 µs 的往返。
import type { DomHost, EngineLink, HostView } from '../host/dom';
import { SlotCanvases, type CanvasStore } from '../host/pages';
import type { Args } from '../input/keymap';
import type { AssetPaths } from './inline';

const CALL_TIMEOUT_MS = 15000;

export interface WorkerOptions {
  container: HTMLElement;
  assets: AssetPaths;
  dpr: number;
  /**
   * 密度（设备像素 / 磅）。
   */
  zoom: number;
  empty?: boolean;
  /**
   * 宿主由这一侧造（它需要 link 与画布存储，而两者都由这一侧造）。
   * 画布存储必须延迟到宿主建好标记之后再造 —— 它要往 #pages 里塞元素。
   */
  makeHost(link: EngineLink, canvasStore: (pagesEl: HTMLElement) => CanvasStore): DomHost;
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
  dirtyPages: number[];
  caretReport: string;
  caretLive: number;
  caretVertical: boolean;
  caretBlockReversed: boolean;
  caretStyle: any;
  selectionStyle: any;
  heapMB: number;
  scrollReset: boolean;
}

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
    dirtyPages: [],
    caretReport: '',
    caretLive: -1,
    caretVertical: false,
    caretBlockReversed: false,
    caretStyle: null,
    selectionStyle: null,
    heapMB: 0,
    scrollReset: false,
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
    const el = container.querySelector<HTMLElement>('#stage') ?? container;
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
        case 'dirtyPages':
          return mirror.dirtyPages;
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
        case 'flowTextPos': {
          // 两端位置随 selection 一起推过来；别的流位置没有镜像，只能给 null。
          const sel = mirror.selection;
          const flow = Number(args?.flow ?? -1);
          if (!sel) return null;
          if (flow === sel.anchor) return sel.anchorPos ?? null;
          if (flow === sel.focus) return sel.focusPos ?? null;
          return null;
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
    settle() {
      // Worker 侧已经在 doOp 的收尾里 present + 贴页 + 回推，这一侧没有收尾可做。
      return true;
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
    pixels() {
      return null;
    },
    serialize() {
      return ask('serialize').then((v) => String(v ?? ''));
    },
    async flush() {
      // 有在途就等它们全部收到回话。ack / reply 由 Worker 处理完那条消息之后才发，
      //   而两条 postMessage 是 FIFO —— 回话到了，它前面那条命令连同推回来的 state
      //   都已经派发过了。
      if (pending.size || pendingReply.size) {
        await new Promise<void>((r) => idleWaiters.push(r));
      }
      // 再让出一拍。state 与 ack 是两条独立消息：同一条命令的回话到达时，若 Worker
      //   侧的发送顺序有变，最后一次 state 可能还在队列里。这一拍让它先跑完 ——
      //   少了它就出现"await flush() 之后读到上一拍的几何"（缩放后页宽还是旧值）。
      await new Promise<void>((r) => setTimeout(r, 0));
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
  });

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
    if (m.dirtyPages) mirror.dirtyPages = m.dirtyPages;
    if (typeof m.caretReport === 'string') mirror.caretReport = m.caretReport;
    if (typeof m.caretLive === 'number') mirror.caretLive = m.caretLive;
    if (typeof m.caretVertical === 'boolean') mirror.caretVertical = m.caretVertical;
    if (typeof m.caretBlockReversed === 'boolean') mirror.caretBlockReversed = m.caretBlockReversed;
    if (m.caretStyle) mirror.caretStyle = m.caretStyle;
    if (m.selectionStyle) mirror.selectionStyle = m.selectionStyle;
    if (typeof m.heapMB === 'number') mirror.heapMB = m.heapMB;
    if (m.scrollReset) mirror.scrollReset = true;

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
      // 槽要先建出来再 boot：boot 里首帧就要贴页，画布到得越早越好。
      host.syncPages();
      post({
        t: 'boot',
        ts: 0,
        glue: assets.glue,
        bust: assets.bust ?? '',
        exportName: 'CreateHEditor',
        fonts: assets.fonts,
        fontMode: 'full',
        dpr: dpr(),
        viewW: vs.w,
        viewH: vs.h,
        zoom: opts.zoom,
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
