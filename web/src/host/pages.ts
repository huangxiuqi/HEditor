// 页画布表：文档里每页一张 canvas，按 (页号 x 步距) 绝对定位。
// 为什么一页一张而不是整篇一张：
//   引擎里已经没有"整窗帧"了（5D-2），渲染只写页位图；宿主这边照页切之后，改一页就只
//   重合成那一页。整篇一张时哪怕只改一个字，浏览器也要把整张 18 Mpx 的图层重新合成
//   一遍（实测多花整整一个 vsync，见 scripts/diag_perf.mjs）。
// 只保留窗口内的页：画布是宿主侧唯一持有像素的地方，留几张就等于留多少内存 ——
//   也是"整篇 3000 段"不再撑爆内存的原因。

/**
 * 一页画布：元素、上下文、以及"还没被贴过"的标志。
 */
export interface PageRec {
  el: HTMLCanvasElement;
  g: CanvasRenderingContext2D | null;
  /**
   * 还没贴过（新分配，或刚改过尺寸被清空）。
   *   它必须整页贴一次，与"这一页欠不欠脏矩形"无关 —— 窗口平移进来的页内容没变，
   *   引擎那边没有矩形，而画布是空白的。
   */
  fresh: boolean;
}

/**
 * 贴页所需的几何（设备像素，与引擎同一口径）。
 */
export interface PageGeometry {
  /**
   * 页宽 / 页高（设备像素）。
   */
  pw: number;
  ph: number;
  /**
   * 页间隙（设备像素）。
   */
  gap: number;
  /**
   * 屏幕像素比：CSS 尺寸 = 设备像素 / dpr。
   */
  dpr: number;
}

export class PageCanvases {
  private readonly pagesEl: HTMLElement;
  private readonly map = new Map<number, PageRec>();
  /**
   * 卸下来的画布留着复用：留住元素就是留住那份已实体化的 backing store。
   */
  private readonly pool: HTMLCanvasElement[] = [];
  /**
   * 一页画布建好时通知对端（worker 传输在这里 transfer）。
   */
  onCreated: ((page: number, el: HTMLCanvasElement) => void) | null = null;

  constructor(pagesEl: HTMLElement) {
    this.pagesEl = pagesEl;
  }

  get size(): number {
    return this.map.size;
  }

  list(): Array<[number, PageRec]> {
    return [...this.map.entries()];
  }

  get(page: number): PageRec | undefined {
    return this.map.get(page);
  }

  private canvasFor(page: number): PageRec {
    let rec = this.map.get(page);
    if (!rec) {
      const el = this.pool.pop() ?? document.createElement('canvas');
      rec = { el, g: el.getContext('2d', { alpha: false }), fresh: true };
      this.map.set(page, rec);
      this.pagesEl.appendChild(el);
      if (this.onCreated) this.onCreated(page, el);
    }
    return rec;
  }

  /**
   * 页纸底：让"还没渲染出来的页"也是一张白纸，而不是什么都没有。
   *
   * 为什么必须有：甩滚时引擎追不上（一页在屏幕上只停留十几毫秒，而渲染一页要几十
   * 毫秒），画布只存在于窗口覆盖的页上，于是可见区域里既没有字也没有画布 —— 看到的
   * 就是"滚动时一片空白、停下来才刷出来"，而那片空白看起来像坏了。
   * 为什么用 CSS 而不是多建画布：这张图是纯渐变，浏览器自己画，主线程零成本；
   * 多建画布要么白占几十 MB 背衬，要么还得为它跑一次 putImageData。
   */
  private syncBackdrop(geom: PageGeometry): void {
    const h = Math.round(geom.ph / geom.dpr);
    const s = Math.round((geom.ph + geom.gap) / geom.dpr);
    const img =
      `linear-gradient(to bottom, #ffffff 0 ${h}px,` + ` rgba(255,255,255,0) ${h}px ${s}px)`;
    if (this.pagesEl.style.backgroundImage !== img) {
      this.pagesEl.style.backgroundImage = img;
      this.pagesEl.style.backgroundSize = `100% ${s}px`;
      this.pagesEl.style.backgroundRepeat = 'repeat-y';
    }
  }

  /**
   * 把画布集合调整成"窗口内每页一张"，并摆好位置与尺寸。
   * 位置用绝对页号算（不是窗口内下标）—— 这样滚动换窗口时画布不会跟着漂。
   */
  sync(first: number, count: number, total: number, geom: PageGeometry): void {
    const { pw, ph, gap, dpr } = geom;
    const step = ph + gap;
    // 只在真的变了才写 style：写一次就触发一次样式重算与布局，
    //   而这一段每拍都跑（敲一个字就跑一次），没变还写就是白付这笔钱。
    const w = pw / dpr + 'px';
    const h = (Math.max(1, total) * step - gap) / dpr + 'px';
    if (this.pagesEl.style.width !== w) this.pagesEl.style.width = w;
    if (this.pagesEl.style.height !== h) this.pagesEl.style.height = h;
    this.syncBackdrop(geom);

    for (const [p, rec] of this.map) {
      if (p < first || p >= first + count) {
        rec.el.remove();
        this.map.delete(p);
        if (this.pool.length < 8) this.pool.push(rec.el);
      }
    }
    for (let p = first; p < first + count; ++p) {
      const rec = this.canvasFor(p);
      // 页号的唯一读写口：诊断（自审要按页号找那一张画布）与指针归属都按它认页。
      const pStr = String(p);
      if (rec.el.dataset.page !== pStr) rec.el.dataset.page = pStr;
      if (rec.el.width !== pw || rec.el.height !== ph) {
        // 改尺寸会清空画布 —— 与新建同一种状态。
        rec.el.width = pw;
        rec.el.height = ph;
        rec.fresh = true;
      }
      const cssW = pw / dpr + 'px';
      const cssH = ph / dpr + 'px';
      if (rec.el.style.width !== cssW) rec.el.style.width = cssW;
      if (rec.el.style.height !== cssH) rec.el.style.height = cssH;
      const top = (p * step) / dpr + 'px';
      if (rec.el.style.top !== top) rec.el.style.top = top;
    }
  }

  /**
   * 整页贴一张（新页 / 报脏但给不出矩形时的退路）。
   */
  blitAll(page: number, src: Uint8ClampedArray, pw: number, ph: number): number {
    const rec = this.map.get(page);
    if (!rec || !rec.g) return 0;
    if (src.length !== pw * ph * 4) return 0;
    rec.g.putImageData(
      new ImageData(new Uint8ClampedArray(src.buffer, src.byteOffset, src.length), pw, ph),
      0,
      0,
    );
    rec.fresh = false;
    return pw * ph;
  }

  /**
   * 按脏矩形贴。光标移动一次只有几十像素，而整页是一百多万。
   *
   * 逐块拷成小图再按页内坐标贴：块只有几十像素，拷贝比"给整页建视图"便宜。
   */
  blitRects(
    page: number,
    src: Uint8ClampedArray,
    pw: number,
    ph: number,
    rects: Array<{ x: number; y: number; w: number; h: number }>,
  ): number {
    const rec = this.map.get(page);
    if (!rec || !rec.g) return 0;
    let px = 0;
    for (const r of rects) {
      if (r.w <= 0 || r.h <= 0) continue;
      const small = new ImageData(r.w, r.h);
      for (let y = 0; y < r.h; ++y) {
        const off = src.byteOffset + ((r.y + y) * pw + r.x) * 4;
        small.data.set(new Uint8ClampedArray(src.buffer, off, r.w * 4), y * r.w * 4);
      }
      rec.g.putImageData(small, r.x, r.y);
      px += r.w * r.h;
    }
    rec.fresh = false;
    return px;
  }

  /**
   * 内容整体作废：所有页下次都要整页贴。
   */
  invalidate(): void {
    for (const rec of this.map.values()) rec.fresh = true;
  }

  destroy(): void {
    for (const rec of this.map.values()) rec.el.remove();
    this.map.clear();
    this.pool.length = 0;
  }
}

/**
 * 画布存储：同线程按页建销，Worker 按槽复用。宿主只认这五个动作。
 */
export interface CanvasStore {
  readonly size: number;
  get(page: number): PageRec | undefined;
  list(): Array<[number, PageRec]>;
  sync(first: number, count: number, total: number, geom: PageGeometry): void;
  invalidate(): void;
  destroy(): void;
}

/**
 * 按槽复用的画布集合（Worker 传输用）。
 *
 * 为什么不能按页建：transferControlToOffscreen 是一次性的，交出去的画布收不回来。
 *   按页建销意味着每次换窗都要新建一批元素、转移一批、在新背衬上首画一次；
 *   槽的个数有硬上限（窗口页数上限 ≤ 8），把元素与背衬钉在槽上就都能复用 ——
 *   换窗只是"哪个槽画哪一页"变了，元素一个不建、一个不删。
 *
 * 槽号 = 页号 mod 槽数。按模取而不是"窗口里第几个位置"：后者在窗口平移时会让每个槽
 *   都换一页，于是每次换窗都得整窗重贴。按模取之后，窗口平移只让新进窗口的那几页
 *   落到旧内容的槽上，其余槽里的页一动没动、像素本来就是对的。
 */
export class SlotCanvases implements CanvasStore {
  private readonly pagesEl: HTMLElement;
  private readonly slots: HTMLCanvasElement[] = [];
  private readonly byPage = new Map<number, PageRec>();
  private nSlots: number;
  /**
   * 一槽画布第一次建好时通知对端（这里 transfer）。
   */
  onCreated: ((slot: number, el: HTMLCanvasElement, slots: number) => void) | null = null;

  constructor(pagesEl: HTMLElement, nSlots: number) {
    this.pagesEl = pagesEl;
    this.nSlots = Math.max(1, nSlots | 0);
  }

  get size(): number {
    return this.byPage.size;
  }

  get(page: number): PageRec | undefined {
    return this.byPage.get(page);
  }

  list(): Array<[number, PageRec]> {
    return [...this.byPage.entries()];
  }

  private acquire(): HTMLCanvasElement {
    const el = document.createElement('canvas');
    el.className = 'pg';
    el.style.position = 'absolute';
    el.style.display = 'block';
    this.pagesEl.appendChild(el);
    return el;
  }

  invalidate(): void {
    // 槽里的像素属于哪一页没有变，变的是"那一页现在该是什么" —— 由 Worker 侧重贴，
    //   这一侧不作废任何东西。留着这个方法是为了与 PageCanvases 同形。
  }

  sync(first: number, count: number, total: number, geom: PageGeometry): void {
    const { pw, ph, gap, dpr } = geom;
    const step = ph + gap;
    // 槽数要装得下当前窗口：窗口页数本来由上限截出来，但外部可以把窗口指到上限之外。
    const n = Math.max(1, this.nSlots, count);
    const firstNew = this.slots.length;
    while (this.slots.length < n) this.slots.push(this.acquire());
    // 先把槽建齐、再逐个转移：转移要带"最终槽数"，边建边发会让中途那几条带的是
    //   1、2、3……，引擎每收到一条就得把槽表整体作废一次。
    for (let k = firstNew; k < this.slots.length; ++k) {
      this.onCreated?.(k, this.slots[k], this.slots.length);
    }

    const h = (Math.max(1, total) * step - gap) / dpr + 'px';
    if (this.pagesEl.style.height !== h) this.pagesEl.style.height = h;

    const w = Math.round(pw / dpr) + 'px';
    const ch = Math.round(ph / dpr) + 'px';
    this.byPage.clear();
    const used = new Set<number>();
    for (let i = 0; i < count; ++i) {
      const p = first + i;
      const s = ((p % this.slots.length) + this.slots.length) % this.slots.length;
      const el = this.slots[s];
      if (!el) continue;
      used.add(s);
      this.byPage.set(p, { el, g: null, fresh: false });
      const pStr = String(p);
      if (el.dataset.page !== pStr) el.dataset.page = pStr;
      if (el.style.display === 'none') el.style.display = 'block';
      // 显示尺寸每推一次状态都要对一遍：转移之后元素的 width/height 属性就作废了，
      //   背衬尺寸由 Worker 定，CSS 尺寸只有这里能改。漏掉的表现是缩放之后"按新倍率
      //   画进旧的显示尺寸"——整页偏小兼发虚。
      if (el.style.width !== w) el.style.width = w;
      if (el.style.height !== ch) el.style.height = ch;
      const top = Math.round((p * step) / dpr) + 'px';
      if (el.style.top !== top) el.style.top = top;
    }
    for (let s = 0; s < this.slots.length; ++s) {
      const el = this.slots[s];
      if (!used.has(s) && el.style.display !== 'none') el.style.display = 'none';
    }
  }

  destroy(): void {
    for (const el of this.slots) el.remove();
    this.slots.length = 0;
    this.byPage.clear();
  }
}
