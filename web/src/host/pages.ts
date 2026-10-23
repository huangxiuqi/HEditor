// 页画布：窗口内每页一张 canvas，按 (页号 x 步距) 绝对定位，由 Worker 按槽复用。
//
// 为什么不是整篇一张：引擎里没有"整窗帧"，渲染只写页位图；宿主照页切之后，改一页就只
//   重合成那一页。整篇一张时哪怕只改一个字，浏览器也要把整张 18 Mpx 的图层重新合成一遍。
// 为什么只保留窗口内那几页：画布是主线程唯一持有像素的地方，留几张就等于留多少内存。

/**
 * 页画布的元素约定：定位与层叠只在这里定一次。
 *
 * 元素约定只写一次：漏了它，画布就参与文档流 —— style.top 完全失效（页间距丢失、
 *   逐页漂移），换窗时 appendChild 会改变流的高度，浏览器据此做滚动锚定、把 scrollTop
 *   拽回去（TRAPS 189）。
 *
 * 幂等：对已经设过的元素再调一次不产生任何写入（池里捞回来的那些都走这条路）。
 * @param el 一张页画布
 */
function stylePageCanvas(el: HTMLCanvasElement): void {
  if (el.className !== 'pg') el.className = 'pg';
  if (el.style.position !== 'absolute') el.style.position = 'absolute';
  if (el.style.left !== '0px') el.style.left = '0';
  if (el.style.display !== 'block') el.style.display = 'block';
}

/**
 * 一页画布：元素、上下文、以及"还没被贴过"的标志。
 */
export interface PageRec {
  el: HTMLCanvasElement;

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

/**
 * 画布存储：按槽复用，宿主只认这几个动作。
 */
export interface CanvasStore {
  readonly size: number;

  /**
   * 按页号取记录。
   * @param page 页号
   */
  get(page: number): PageRec | undefined;

  /**
   * 当前持有的页（页号与记录）。
   */
  list(): Array<[number, PageRec]>;

  /**
   * 把画布集合对齐到窗口，并摆好位置与尺寸。
   * @param first 窗口首页
   * @param count 窗口页数
   * @param total 总页数（算滚动高度用）
   * @param geom 页度量
   */
  sync(first: number, count: number, total: number, geom: PageGeometry): void;

  /**
   * 内容整体作废：所有页下次整页贴。
   */
  invalidate(): void;

  /**
   * 丢掉全部画布并清空容器。
   */
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

  /**
   * 取这一页现在落在哪个槽。
   * @param page 页号
   * @returns 记录；不在窗口里时为 undefined
   */
  get(page: number): PageRec | undefined {
    return this.byPage.get(page);
  }

  /**
   * 当前窗口里每页落在哪个槽。
   * @returns 页号与记录的二元组
   */
  list(): Array<[number, PageRec]> {
    return [...this.byPage.entries()];
  }

  /**
   * 造一个槽元素并挂进容器。
   * @returns 新元素
   */
  private acquire(): HTMLCanvasElement {
    const el = document.createElement('canvas');
    stylePageCanvas(el);
    this.pagesEl.appendChild(el);
    return el;
  }

  /**
   * 内容整体作废。槽里的像素属于哪一页没有变，变的是那一页现在该是什么，所以这一侧
   *   不作废任何东西 —— 重贴由 Worker 侧做。
   */
  invalidate(): void {}

  /**
   * 让槽与当前窗口对齐（第 p 页永远用第 p 模槽数 个槽）。
   * @param first 窗口首页
   * @param count 窗口页数
   * @param total 总页数
   * @param geom 页度量
   */
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
      this.byPage.set(p, { el, fresh: false });
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
      if (used.has(s)) continue;
      if (el.style.display !== 'none') el.style.display = 'none';
      // 页号一起摘掉：槽里的像素还在（下一次复用时整页重贴或按脏矩形补），但它此刻
      //   不属于任何一页 —— 留着上一个页号会让"这张画布显示的是哪一页"这个读数说谎。
      if (el.dataset.page !== undefined) delete el.dataset.page;
    }
  }

  /**
   * 拆掉所有槽元素。
   */
  destroy(): void {
    for (const el of this.slots) el.remove();
    this.slots.length = 0;
    this.byPage.clear();
  }
}
