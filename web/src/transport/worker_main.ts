// Worker 侧壳：槽、画布、消息。引擎语义全在 engine/api.ts 里。
//
// 这个文件只做三件事，别让它长回来：
//   一、按槽摆画布并接收宿主转移过来的 OffscreenCanvas（像素不跨线程）；
//   二、把槽里的像素贴到画布上（blit），并报出贴了几页、花了多少毫秒；
//   三、把消息转发给命令表：boot / do / ask / view / pageCanvas / viewMode。
//
// 引擎状态、命令表、窗口滞回、上行状态对象都在 api.ts —— 那一份同时给同线程宿主用。
// 这里出现一个 mod.xxx 就说明边界破了。
//
// 槽号 = 页号模槽数。槽是画布的"座位"：transferControlToOffscreen 是一次性的，
//   交出去的画布收不回来，而"一页一张、随窗口建销"会让每一次换窗都新建一批元素、
//   转移一批、在新背衬上首画一次。槽数有硬上限（窗口页数上限，至多 8），把元素与
//   背衬钉在槽上就都能复用 —— 换窗只是"哪个槽画哪一页"变了。
import { createEngineApi, type EngineApi, type Args } from '../engine/api';

declare const self: {
  onmessage: ((ev: MessageEvent) => void) | null;
  postMessage(msg: unknown, transfer?: Transferable[]): void;
  [k: string]: unknown;
};
declare function importScripts(...urls: string[]): void;

/** 页在窗口里的座位。槽数由宿主说了算（它建了几个槽就交过来几条画布）。 */
const slotCtx: Array<OffscreenCanvasRenderingContext2D | null> = [];
/** 每一槽当前画着哪一页（-1 = 空，或内容已整体失效）。 */
const slotPage: number[] = [];
let slotN = 8;
let continuous = false;
let api: EngineApi | null = null;

function slotOf(p: number): number {
  return ((p % slotN) + slotN) % slotN;
}

/** 内容整体作废（换缩放、换页度量、换文档）：所有槽都要重贴。 */
function invalidateSlots(): void {
  slotPage.length = slotN;
  slotPage.fill(-1);
}

/** 槽数换成宿主报来的值。变了就整体作废 —— 槽数一变，所有页的槽号都跟着变。 */
function setSlotCount(n: number): boolean {
  const v = n | 0;
  if (v <= 0 || v === slotN) return false;
  slotN = v;
  invalidateSlots();
  return true;
}

/**
 * 页号 -> 上下文。现算，不缓存。
 *
 * 读它的地方不只有贴图那一条路（自审那条走的是 ask）。缓存的派生值只要有一条路
 *   没更新到，读出来就是上一窗的对应 —— 症状是"画布与帧对不上"，而看代码完全看不出。
 */
function ctxOfPage(p: number): OffscreenCanvasRenderingContext2D | null {
  const e = api?.engine();
  if (!e) return null;
  const first = e.windowFirstPage();
  if (p < first || p >= first + e.windowPageCount()) return null;
  return slotCtx[slotOf(p)] ?? null;
}

/* 上屏 */

let blitLastMs = 0;
let blitLastPages = 0;
let blitLastPx = 0;

/**
 * 把第 p 页"画到画布上应该是什么"贴到它自己的画布上。
 *
 * rects 为空 = 整页贴（这张画布是新的，或刚改过尺寸）；非空 = 只贴这几块。
 *   光标移动一次只有几十像素，而整页是 1 Mpx —— 差三个数量级。
 */
function blitPage(p: number, rects: Array<{ x: number; y: number; w: number; h: number }> | null): number {
  const c = ctxOfPage(p);
  const e = api?.engine();
  if (!c || !e) return 0;
  const pw = e.pagePxWidth();
  const ph = e.pagePxHeight();
  if (pw <= 0 || ph <= 0) return 0;
  const src = e.pageRgba(p) as Uint8ClampedArray;
  if (src.length !== pw * ph * 4) return 0;
  if (c.canvas.width !== pw || c.canvas.height !== ph) {
    // 改尺寸会清空画布，所以尺寸变了必定整块重画。
    c.canvas.width = pw;
    c.canvas.height = ph;
    rects = null;
  }
  if (!rects || !rects.length) {
    c.putImageData(
      new ImageData(new Uint8ClampedArray(src.buffer, src.byteOffset, src.length), pw, ph),
      0,
      0,
    );
    // 整页贴过了，这一页的矩形就没有意义了 —— 留着下一拍只会白贴一次。
    e.pageDirtyRectsJson(p);
    return pw * ph;
  }
  let px = 0;
  for (const r of rects) {
    if (!(r.w > 0) || !(r.h > 0)) continue;
    // 逐块拷成一张小图，再用两参数形式贴到页内坐标。
    //   六参数形式在 OffscreenCanvas 上遇到负 dy 会把那块裁错（贴完仍差同样多像素，
    //   而普通 canvas 上同一个表达式是对的）。拷一块几十像素比"猜实现"便宜。
    const small = new ImageData(r.w, r.h);
    for (let y = 0; y < r.h; ++y) {
      const off = src.byteOffset + ((r.y + y) * pw + r.x) * 4;
      small.data.set(new Uint8ClampedArray(src.buffer, off, r.w * 4), y * r.w * 4);
    }
    c.putImageData(small, r.x, r.y);
    px += r.w * r.h;
  }
  return px;
}

/**
 * 重贴。
 *
 * 要贴哪几页由"还欠着脏矩形"决定，不是"这一拍写过哪几页"：矩形是累积的 ——
 *   某一拍的 present 可能根本没被消费（窗口平移、槽重建），那一拍的矩形会留到这一拍。
 */
function blitPages(): number {
  const tBlit = performance.now();
  const e = api?.engine();
  if (!e) return 0;
  const first = e.windowFirstPage();
  const count = e.windowPageCount();
  let n = 0;
  let px = 0;
  const pending = JSON.parse(e.pageDirtyPagesJson()) as number[];
  for (let p = first; p < first + count; ++p) {
    const s = slotOf(p);
    // 两个理由之一成立就要贴：这一页还欠着像素，或者它落的那个槽装的还是别的页
    //   （窗口平移进来的新页、或内容整体作废）。
    const stale = slotPage[s] !== p;
    const dirty = pending.includes(p);
    if (!stale && !dirty) continue;
    let rects: Array<{ x: number; y: number; w: number; h: number }> | null = null;
    if (!stale && dirty) {
      rects = JSON.parse(e.pageDirtyRectsJson(p));
      if (!rects || !rects.length) rects = null;
    }
    const got = blitPage(p, rects);
    if (got > 0) {
      slotPage[s] = p;
      ++n;
      px += got;
    }
  }
  blitLastMs = performance.now() - tBlit;
  blitLastPages = n;
  blitLastPx = px;
  return n;
}

/** 这一窗里还有几页没有画布（宿主还没把该页落的那个槽的 canvas 交过来）。 */
function pagesMissing(): number {
  const e = api?.engine();
  if (!e) return 0;
  const first = e.windowFirstPage();
  const count = e.windowPageCount();
  let n = 0;
  for (let p = first; p < first + count; ++p) if (!ctxOfPage(p)) ++n;
  return n;
}

/* 传输 */

const post = (msg: unknown, transfer?: Transferable[]): void => self.postMessage(msg, transfer ?? []);

const host = {
  async loadGlue(url: string, exportName: string): Promise<unknown> {
    importScripts(url);
    return self[exportName];
  },
  slotCount: () => slotN,
  pagesMissing,
  ctxOfPage,
  canvasCount: () => slotCtx.reduce((n, c) => n + (c ? 1 : 0), 0),
  invalidateSlots,
  blitPages,
  blitStats: () => ({ ms: blitLastMs, pages: blitLastPages, px: blitLastPx }),

  /** 某个槽的画布到了：记下它，把那一槽贴出来。槽只建一次，之后换窗不会再收到同一个槽。 */
  addCanvas(m: Args): void {
    const off = m.canvas as OffscreenCanvas;
    const c = off.getContext('2d', { alpha: false });
    if (!c) throw new Error('OffscreenCanvas 拿不到 2d 上下文');
    if (setSlotCount(m.slots)) slotCtx.length = Math.max(slotCtx.length, slotN);
    slotCtx[m.slot | 0] = c;
    slotPage[m.slot | 0] = -1;
    blitPages();
  },

  /**
   * 视图模式与它的页间隙。两套模式共用同一套"一页一张画布"，区别只有步距；
   *   引擎侧那两个配置由 api.setViewMode 成套设下去（只设间隙会让排版还是分页的）。
   */
  setViewMode(m: Args): void {
    continuous = !!m.continuous;
    api?.setViewMode(m);
    // 切模式会把排版形状与页的落点整套换掉，所以要重排 + 重光栅页位图，再作废槽表重贴。
    api?.relayout();
  },
};

/* 消息入口 */

async function handleBoot(m: Args): Promise<void> {
  api = createEngineApi({ post, host });
  await api.boot(m);
}

async function handle(m: Args): Promise<void> {
  try {
    if (m.t === 'boot') return void (await handleBoot(m));
    if (!api) throw new Error('引擎尚未 boot');
    if (m.t === 'pageCanvas') return void api.pageCanvas(m);
    if (m.t === 'viewMode') return void api.viewMode(m);
    if (m.t === 'view') {
      const r = api.view(m);
      if (!r.moved) return void post({ t: 'ack', ts: m.ts, workMs: r.workMs });
      return void api.state(m.ts, 'view', r.stats, undefined, r.workMs);
    }
    if (m.t === 'do') return void (await api.do(m));
    if (m.t === 'ask') return void (await api.ask(m));
    throw new Error('未知消息类型 ' + String(m.t));
  } catch (e) {
    const err = e as { stack?: string } | null;
    if (m.t === 'ask') post({ t: 'reply', id: m.id, error: String(err?.stack || e) });
    // 视口消息抛了异常也要回一条：宿主靠回话解门闩，丢一条就把之后的滚动锁死。
    if (m.t === 'view') post({ t: 'ack', ts: m.ts, workMs: -1 });
    post({
      t: 'error',
      where: String(m.t) + (m.name ? ':' + String(m.name) : ''),
      message: String(err?.stack || e),
    });
  }
}

self.onmessage = (ev: MessageEvent) => {
  void handle((ev.data ?? {}) as Args);
};
