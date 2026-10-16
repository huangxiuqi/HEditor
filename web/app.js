// 浏览器宿主（WASM 验证台）—— 档 1：可输入。
//
// 这个文件仍然刻意保持"薄"，而且现在要守住一条更难的纪律：
//
//   宿主不持有文档，也不做任何字符索引。
//     它不保存文本副本、不算 insert 位置、不把 offset 在 JS 字符串上加减。
//     原因是偏移的单位在引擎里是码点，而 JS 字符串是 UTF-16 码元：
//     一个 "⁹" 在引擎里是 1 个偏移、在 JS 里是 1 个码元但 3 个 UTF-8 字节，
//     在 "𝄞" 这种星平面字符上更会差成 2 个码元对 1 个码点。
//     只要 JS 侧碰过 offset 算术，这类 bug 就只在中文/上标/emoji 上出现，
//     而英文测试全过 —— 所以 JS 侧不碰偏移算术。
//
//   键盘 -> 一次跨界调用（typeText/backspace/moveCaret），
//     引擎内部完成 编辑 → 重排 → 光栅，回来的是"有没有变"+ 一组统计 + 一块位图。
//     循环留在引擎内，跨边界只有输入事件与位图 —— 这就是 Figma 那条纪律。
//
//   IME 组合期间一次都不调引擎。合成事件（compositionupdate）每秒能来几十次，
//     每次重排就是又卡又闪 —— 这正是 WASM 路线最容易被攻击的地方。
//     预编辑串先在状态栏本地预览，commit 时才脏化文档、只重排一次。
'use strict';

// 整个文件包一层立即执行函数：同页多实例时这一份会与另一个宿主（worker_host.js）
//   一起加载，两边各有自己的顶层常量（FONTS / DPR 之类），不隔离就是"标识符重复声明"
//   的语法错误 —— 而这类错误只在同页加载两个宿主时才出现，单个演示页永远看不到。
(function () {

const BASE = new URL('../', location.href);
// 产物目录：默认交付档；带 ?wasm=debug 时指向调试档（断言 + 栈检查 + 源码映射）。
//   两档是各自独立构建的目录，切换只影响取胶水层的路径。
//   ?wasm= 给别的值时按"产物目录"处理（相对仓库根，末尾有无斜杠都行）：这样
//   "产物不存在"这条失败路径才能在页面上复现出来，错误降级那条通路才有可证伪的造法。
const wasmParam = new URLSearchParams(location.search).get('wasm') || '';
const WASM_DIR = wasmParam === 'debug'
                     ? 'build/wasm-debug/web/'
                     : wasmParam
                       ? (wasmParam.endsWith('/') ? wasmParam : wasmParam + '/')
                       : 'build/wasm/web/';
const WASM_GLUE = new URL(WASM_DIR + 'heditor.js', BASE);

const FONTS = [
  { role: 'cjk',    label: 'Noto Sans SC',        path: 'assets/fonts/NotoSansSC-VF.ttf' },
  { role: 'mongol', label: 'Noto Sans Mongolian', path: 'assets/fonts/NotoSansMongolian-Regular.ttf' },
  { role: 'latin',  label: 'Noto Sans (Latin)',   path: 'assets/fonts/NotoSans-Latin-VF.ttf' },
];

const SCENARIOS = [
  { id: 0, label: '完整样例（4 页）' },
  { id: 1, label: '蒙文竖排 vertical-lr（侧倒）' },
  { id: 2, label: '中文竖排 vertical-rl（正立）' },
  { id: 3, label: '中文长文本（横排）' },
  { id: 4, label: '中英混排 + 字体回退' },
  { id: 5, label: '蒙文竖排 vertical-rl（对照）' },
  { id: 6, label: ' 可编辑（敲字 / 光标 / 增量）' },
  { id: 7, label: '长文档 200 段（增量排版）' },
  { id: 8, label: '真病历样例（8 例，读文件）' },
];

const DEFAULT_SCENARIO = 6;   // 直接进可编辑场景，省得还要先切一下

/**
 * 造一个编辑器实例：舞台、页容器、输入层都在 container 里找。
 *
 * 查找限定在容器内，因为同页多实例时 #stage / #pages / #ime 会重名，全局查找只会命中
 *   第一个实例那一份 —— 两个实例会互相抢元素；容器里没有的元素再回落到整页查找，演示页
 *   放在容器外的工具条与统计栏因此仍然接得上。opts.chrome 为 false 时只接编辑器本体。
 *
 * 返回值是门面（与 Worker 宿主同形），另带 whenReady（resolve 成 { ok, error }，刻意
 *   不做成失败即 reject：没人接的 rejection 会变成一条未处理的异步失败）与 dispose()。
 *   函数体保持文件原有的零缩进，逐行重排缩进会把 diff 变成整文件重写。
 */
function createEditor(container, opts) {
const o = opts || {};
const root = container || document.body;
const chrome = o.chrome !== false;

const $ = (id) => root.querySelector('#' + id) || document.getElementById(id);
const stageEl = $('stage');
const pagesEl = $('pages');
const ta = $('ime');

let mod = null;
let busy = false;
let ready = false;
// 缩放在界面那一格上是"倍数"（1 = 100% = Web 标准的 96 dpi，无量纲），引擎收的却是
//   "密度"（设备像素 / 磅，1 = 72 dpi，有量纲）—— 两者差一个 4 / 3。
//   96 dpi 对应 96 / 72 = 4 / 3：页宽 round(595.276 x 4 / 3) = 794。
//   换算只做在界面接缝上（两个输入框、门面的 zoomFactor / zoomToFactor 一对）。
//   引擎、判据、诊断读数一律继续用密度，别把倍数漏进去。
const kZoomBase = 4 / 3;
const engineZoomOf = (factor) => Number(factor) * kZoomBase;
const uiZoomOf = (density) => density / kZoomBase;
let currentZoom = kZoomBase;

/**
 * 把当前缩放按"倍数"回写到输入框与它右边那一格。
 *
 * 两个方向上都要过换算：读输入框要乘 kZoomBase（倍数转密度），写输入框要除以它。
 *   截到 4 位是给输入框的 —— 4 / 3 这种无限小数直接塞进去会拖一长串；那一档差
 *   0.00003，千像素尺度上不到 0.1 像素，落不进任何容差。
 * 元素可能不存在（产品页只有舞台与输入层），缺了就跳过，不抛异常。
 */
function syncZoomLabel() {
  const zEl = $('zoom');
  const zVal = $('zoomVal');
  const factor = uiZoomOf(currentZoom);
  if (zEl) zEl.value = String(Number(factor.toFixed(4)));
  if (zVal) zVal.textContent = String(Number(factor.toFixed(3)));
}
// 页度量由宿主传：引擎自带的那份缺省值只服务 native 单测，产品路径一律在这里设。
// A4 = 595.276 x 841.89 磅（210 x 297 毫米）；页边距 2 厘米 = 2 x 72 / 2.54 磅。
const kPageWidthPt = 595.276;
const kPageHeightPt = 841.89;
const kPageMarginPt = 56.7;
let lastEdit = null;
let composing = false;
let caretOn = true;
let blinkTimer = null;
let drag = null;
let lastPointerId = null;   // 最近一次 pointerdown 的指针 id（按下时用来做指针捕获）
let capturedId = null;      // 已经捕获在页容器上的指针 id（下次按下时先放掉）
const pageErrors = [];
let disposed = false;

/**
 * 错误降级面（见 web/host_error.js）。
 *
 * 挂在容器上：那一块就是"这个实例所在的位置"，横幅出现时不会跑到别的实例头上。
 *   host_error.js 没加载成功时退化成"只记进 pageErrors"，不因为展示层缺席而抛异常。
 *   容器建议写成 position:relative，横幅才落在这块实例内部（多实例页面上尤其重要）。
 */
const errorSurface = typeof window.createErrorSurface === 'function'
  ? window.createErrorSurface(root, {}) : null;

// 容器里必须有三件：滚动舞台、页容器、输入层。缺了就是容器配错，当场说清楚 ——
//   否则会等到某次摆输入层时抛一个看不懂的错，或者整个区域一片空白。
if (!stageEl || !pagesEl || !ta) {
  const missing = [!stageEl && '#stage', !pagesEl && '#pages', !ta && '#ime']
    .filter(Boolean).join(' / ');
  const err = new Error(`容器里缺少 ${missing}，无法建立编辑器`);
  if (errorSurface) {
    errorSurface.fatal('编辑器初始化失败', {
      detail: err.message,
      hint: 'container 必须是包含 #stage / #pages / #ime 的那个元素。',
    });
  }
  throw err;
}

/**
 * 失败上报的两个档位。
 *
 * 分成两档的理由：启动失败与"某一次操作失败"对用户的含义完全不同 —— 前者要说明
 *   这个编辑器已经不能用，后者只需提示一次并让编辑继续。混成一档的话，要么把可恢复的
 *   失败说成致命，要么把致命失败只写进日志而不显示。
 */
const report = {
  fatal(message, info) { if (errorSurface) errorSurface.fatal(message, info); },
  warn(message, info) { if (errorSurface) errorSurface.warn(message, info); },
  get entries() { return errorSurface ? errorSurface.entries : []; },
  get fatalActive() { return errorSurface ? errorSurface.fatalActive : false; },
};

/**
 * 引擎是否已经不可用。
 *
 * 这一侧目前唯一会致命的是启动失败（引擎没起来，mod 为 null），此时 UI 接线还没跑过，
 *   所以不需要额外的输入拦截；把它记下来是为了与 Worker 宿主的 hostErrors 同形，
 *   让"哪个实例坏了"在多实例页面上读法一致。
 */
let dead = false;

/**
 * 宿主侧上屏耗时（最近一轮的四段分解）。
 *
 * 引擎的 paint_ms 只算到"帧缓冲组装完"。帧缓冲跨到 JS 之后还有三段钱：
 * 取视图、构造 ImageData、putImageData。面板此前完全没有这三个数，
 * 于是"页面卡"只能看到引擎那一半，另一半是盲区 —— 而这一段恰好是唯一
 * 随帧面积走的部分（画布是整篇大小的单张位图，脏矩形只决定源数据偏移，
 * 不决定浏览器要把多大的图层重新合成）。
 * 记"最近一轮"而不是累加：面板每次读的都是刚发生的一轮。
 */
let lastBlit = { frameMs: 0, imageMs: 0, putMs: 0, imeMs: 0, totalMs: 0,
                 full: true, w: 0, h: 0, boxW: 0, boxH: 0, framePx: 0,
                 pages: 0, windowFirst: 0 };

/**
 * 每页一张画布。
 *
 * 帧从"整篇一张位图"改成"窗口一张"之后，宿主这边也要跟着切：把窗口位图按页切成
 * 若干块，每块贴进它自己那张画布。这样改哪一页就只重合成那一页 —— 而"整篇一张"时
 * 哪怕只改 1 页，浏览器也要把整张 18 Mpx 的图层重新合成一遍（实测多花整整一个
 * vsync，见 scripts/diag_perf.mjs）。
 *
 * 只保留窗口内的页：画布是宿主侧唯一持有像素的地方，留几张就等于留多少内存，
 * 也是"整篇 3000 段"不再撑爆内存的原因。
 */
const pageCanvases = new Map();  // 绝对页号 -> { el, g }

/**
 * 帧画布的兼容代理（只给探针与 __heditor.canvas 用，宿主自己一律走页画布）。
 *
 * 探针的契约是"帧坐标"：取 rect、按帧宽高换算、往它上面派 mousedown。帧现在被切成
 * 每页一张画布，于是这里按"窗口帧在屏幕上占的那块区域"给出几何，事件转发给真正
 * 接鼠标的页容器 —— 探针因此不必区分一张还是多张，判据口径也没变。
 */
const frameCanvasProxy = {
  get width() { return mod ? mod.frameWidth() : 1; },
  get height() { return mod ? mod.frameHeight() : 1; },
  get style() { return pagesEl.style; },
  getBoundingClientRect() {
    const r = pagesEl.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const step = mod ? mod.pagePxHeight() + mod.pageGapPx() : 0;
    const first = mod ? mod.windowFirstPage() : 0;
    const top = r.top + (first * step) / dpr;
    const width = this.width / dpr;
    const height = this.height / dpr;
    return { left: r.left, top, right: r.left + width, bottom: top + height,
             width, height, x: r.left, y: top };
  },
  dispatchEvent(ev) { return pagesEl.dispatchEvent(ev); },
  addEventListener(...a) { return pagesEl.addEventListener(...a); },
  getContext() { return null; },
};

/**
 * 画布池：卸下来的页画布留着复用，不销毁。
 *
 * 为什么值得留着：换窗口时要卸掉几页、建起几页。实测（out/_diagCanvasAlloc.mjs，
 *   一页 893×1263）把尺寸设成页背衬只要 0.001 ms —— 背衬是"第一次真画"时才实体化的，
 *   那一下 2.65 ms/张；而已经实体化过的画布再贴一次只要 0.7 ms。池子留住元素，就是
 *   留住那份已实体化的背衬，重新上场时省掉的是这 2 ms 上下的"首次触碰"。
 *   （重建 4 张 14.60 ms 与复用 4 张 3.80 ms 的旧读数，差的正是这一笔，不在
 *   "重新分配"本身 —— 分配那一步在页内几乎量不到。）
 * 池子封顶 8 页（与 maxWindowPages 的上限同值），够换一个来回。
 */
const canvasPool = [];

function pageCanvasFor(page) {
  let rec = pageCanvases.get(page);
  if (!rec) {
    const el = canvasPool.pop() || document.createElement('canvas');
    rec = { el, g: el.getContext('2d', { alpha: false }) };
    pageCanvases.set(page, rec);
    pagesEl.appendChild(el);
  }
  return rec;
}

/**
 * 页纸底：让"还没渲染出来的页"也是一张白纸，而不是什么都没有。
 *
 * 为什么必须有：甩滚时引擎追不上（一页在屏幕上只停留十几毫秒，而渲染一页要几十毫秒），
 *   画布只存在于窗口覆盖的页上，于是可见区域里既没有字也没有画布 —— 看到的就是
 *   "滚动时一片空白、停下来才刷出来"，而那片空白看起来像坏了。铺一层页形底之后，
 *   全程都看得见一页页白纸在动，内容随后补上；PDF 阅读器用的也是这一手。
 * 为什么用 CSS 而不是多建画布：这张图是纯渐变，浏览器自己画，主线程零成本；多建画布
 *   要么白占几十 MB 背衬，要么还得为它跑一次 putImageData —— 那又把主线程占住了
 *   （正是要避开的东西）。它是占位，不是内容：真正的字仍然只在画布上。
 * 周期与页高都按 devicePixelRatio 折算，与画布用的是同一组尺寸，所以底下那层与画布
 *   逐个对齐（对不齐会看到两条错位的边）。
 */
function syncPageBackdrop(pw, ph, step, dpr) {
  const h = Math.round(ph / dpr);
  const s = Math.round(step / dpr);
  const img = `linear-gradient(to bottom, #ffffff 0 ${h}px,` +
              ` rgba(255,255,255,0) ${h}px ${s}px)`;
  if (pagesEl.style.backgroundImage !== img) {
    pagesEl.style.backgroundImage = img;
    pagesEl.style.backgroundSize = `100% ${s}px`;
    pagesEl.style.backgroundRepeat = 'repeat-y';
  }
}

/**
 * 把页画布集合调整成"窗口内每页一张"，并摆好位置与尺寸。
 * 位置用绝对页号算（不是窗口内下标）—— 这样滚动换窗口时画布不会跟着漂。
 */
function syncPageCanvases(first, count, total) {
  const pw = mod.pagePxWidth();
  const ph = mod.pagePxHeight();
  const step = ph + mod.pageGapPx();
  const dpr = window.devicePixelRatio || 1;

  // 只在真的变了才写 style：写一次就触发一次样式重算与布局，
  //   而这一段每拍都跑（敲一个字就跑一次），没变还写就是白付这笔钱。
  const w = (pw / dpr) + 'px';
  const h = ((Math.max(1, total) * step - mod.pageGapPx()) / dpr) + 'px';
  if (pagesEl.style.width !== w) pagesEl.style.width = w;
  if (pagesEl.style.height !== h) pagesEl.style.height = h;
  syncPageBackdrop(pw, ph, step, dpr);

  for (const [p, rec] of pageCanvases) {
    if (p < first || p >= first + count) {
      rec.el.remove();
      pageCanvases.delete(p);
      // 进池而不是丢掉：它的 backing store 还留着，下次换窗口拿来就能用。
      if (canvasPool.length < 8) canvasPool.push(rec.el);
    }
  }
  for (let p = first; p < first + count; ++p) {
    const rec = pageCanvasFor(p);
    if (rec.el.width !== pw || rec.el.height !== ph) {
      rec.el.width = pw;
      rec.el.height = ph;
    }
    const cssW = (pw / dpr) + 'px';
    const cssH = (ph / dpr) + 'px';
    if (rec.el.style.width !== cssW) rec.el.style.width = cssW;
    if (rec.el.style.height !== cssH) rec.el.style.height = cssH;
    const top = (p * step / dpr) + 'px';
    if (rec.el.style.top !== top) rec.el.style.top = top;
  }
}

/**
 * 视口窗口：帧只装"看得见的那几页"（再留一两页余量），并尽量把光标页并进来。
 *
 * 光标那一页要尽量进来：帧只装窗口内的页，光标不在窗口里就既画不出来、也点不中。
 * 窗口一变就整帧重建（底图按新范围重光栅，实测 3 页约 30 ms），所以锚点只在真的
 * 盖不住"必须装下的那几页"时才挪 —— 见 applyWindow 的滞回。
 *
 * 长度封顶 maxWindowPages() 页：光标页与可见页可能隔得很远（用户把屏幕滚到别处去了），
 * 不封顶就会算出跨越整篇的窗口，那正好把"帧只装几页"这件事作废。
 * 实测（scripts/diag_perf.mjs 扫 zoom）：窗口 2~3 页时帧间隔只由单拍操作耗时决定，
 * 画布面积本身不再多花时间；而作废之前那张 18.3 Mpx 的整篇画布要多花整整一个 vsync。
 */
let lastAnchor = '';

function visiblePageRange() {
  const dpr = window.devicePixelRatio || 1;
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const total = Math.max(1, lastEdit ? lastEdit.totalPages : 1);
  const sr = stageEl.getBoundingClientRect();
  const pr = pagesEl.getBoundingClientRect();
  const top = Math.max(0, (sr.top - pr.top) * dpr);
  const bot = Math.max(top, (sr.bottom - pr.top) * dpr);
  const first = Math.min(total - 1, Math.max(0, Math.floor(top / step)));
  const last = Math.min(total - 1, Math.max(first, Math.floor(Math.max(top, bot - 1) / step)));
  return { first, last };
}

/**
 * 窗口最多装几页：视口装得下的页数，再加 4 页余量 —— 余量就是"还能滚多远才需要换
 *   窗"，也是换窗那一拍能用来垫的行程；它不由"看得见几页"定，而由"一拍跨过几页"定
 *   —— 真机甩滚实测到每秒 80 页，一拍就是 1~2 页。
 * 为什么不是更大（试过 8）：换窗成本正比于窗口长度而不是位移，窗口落后超过一个窗口长
 *   时引擎走整窗重建（重画 count 页），一跳 94~286 ms，而余量 4 只要 45 ms 上下；余量
 *   大了省下的只是换窗次数，而它不是瓶颈（实测余量 8 把落后从 3 页推到 8~28 页）。
 * 页数按视口高度算而不是按"可见页 first..last"：后者随页边界在 n 与 n+1 之间跳，
 *   而窗口尺寸一变就整窗重建。
 * 两道封顶都是内存：一页约 4.5 MB 设备像素（893x1263 的 RGBA）故页数上限定 8；
 *   高 DPI 下同样页数翻几倍，再按帧像素总量封一道（24 Mpx 约 96 MB），取小的那个。
 * 封顶只压余量，不压"盖住视口"：盖不住就是整页空白，那是硬要求。
 */
const kWindowMarginPages = 4;
const kWindowMaxPages = 8;
const kWindowMaxPixels = 24 * 1024 * 1024;

function maxWindowPages() {
  const dpr = window.devicePixelRatio || 1;
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const vis = Math.ceil((stageEl.clientHeight * dpr) / step);
  const byPixels = Math.floor(kWindowMaxPixels /
                               Math.max(1, mod.pagePxWidth() * mod.pagePxHeight()));
  return Math.max(vis, Math.min(kWindowMaxPages, byPixels, vis + kWindowMarginPages));
}

/**
 * 换窗的提前量（页）与滚动方向。
 *
 * 为什么需要提前量：换窗那一拍要重光栅新露出来的那几页并挪一次底图，中位 20~45 ms，
 *   而合成器不等主线程 —— 这几十毫秒里它照样按用户速度往前滚（每秒 80 页就是 4~5 页）。
 *   所以这笔钱得在"窗口还盖得住前面那几页"时就付掉；等可见页贴到窗口边缘才动手，
 *   那几十毫秒里新露出来的地方必然是空白。
 * 为什么是 2 页：提前量要盖住的是"触发到呈现完成"的延迟。实测换窗一拍 20~45 ms，
 *   而每秒 80 页对应每毫秒 0.08 页 —— 45 ms 就是 3.6 页。取 2 页意味着触发时前方
 *   还剩 2 页（约 25 ms @80 页/s）可以垫，而余量 8 页保证这 2 页是"额外的"，
 *   不会把窗口长度吃掉（长度一改就走整窗重建）。
 * 方向只影响"多出来的页放前面还是后面"，不影响长度，所以来回滚不会反复整窗重建。
 */
const kWindowLookaheadPages = 2;
let scrollDir = 1;

/**
 * 让出点：滚动快到"追不上"时这一拍不换窗，把主线程让给合成器。判据是实测出来的而不是常数：换窗那一拍花
 *   T 毫秒，而合成器不等主线程 —— 这 T 毫秒里它照样按用户速度往前滚，滚过的页数就是 T 乘速度。触发时前方
 *   只剩"提前量"那几页可以垫（kWindowLookaheadPages），所以只要 T × 速度 > 提前量，窗口就必然在这期间被
 *   滚出可见范围，而且每一拍都追不回来 —— 这不是"慢一点"，是雪崩。
 * 实测（真滚轮，2 秒滚完 160 页 = 每秒 80 页）进入这一档后窗口落后 8~28 页、可见页 55% 的帧无画布、长任务
 *   占用 169%~318%，画面仍是空白：代价全付了，一点内容都没换来。所以这一档要主动放弃 —— 不换窗，让合成器
 *   把已有内容平滑滚过去（画布在文档流里，滚动本身不花引擎的钱），速度降下来（用户松手或放慢）再一次性对齐。
 *   对齐的成本只与窗口大小有关（整窗重建 count 页），与落后多少页无关，所以不会因为放弃而变贵。
 * 速度取平滑值而不是瞬时值：相邻两拍的位移差很大（合成器按惯性加速），用瞬时值会让判据在
 *   "放弃—对齐—放弃"之间来回切；平滑之后它在整段甩滚里稳定地保持放弃状态。单位取页/毫秒，
 *   判据两边都是页；空闲超过 200 ms 直接归零 —— 用户已经停下，速度不该被惯性拖着。
 */
const kSpeedIdleMs = 200;
let speedPagesPerMs = 0;
let lastSpeedStamp = 0;
// 上一拍"对齐窗口 + 重光栅 + 上屏"实际占住的墙钟时间（只用于取证显示）。
let lastScrollWorkMs = 0;
// 判据用的成本估计：对齐耗时的近期最小值，而不是最后那一拍的值。
//
// 为什么不能用最后一次：一旦放弃过一次，下一次对齐必然是"追了很久之后的整窗重建"
//   （窗口落后超过一个窗口长，走不了平移复用），那一拍要 60~150 ms。拿它当成本，
//   判据就会一直说"追不上"，于是永远放弃下去 —— 实测这个自锁把每秒 40 页那一档
//   从"整屏全空 0%"直接打成 87%（它本来跟得上）。
//   取最小值拿到的是稳态路径的成本（平移复用一页，实测 26 ms 上下），那才是
//   "跟得上时本来要花多少"的答案。乘 1.05 是允许它慢慢爬：机器真的更慢时，
//   成本估计会跟着上去，判据最终还是能得出"追不上"。
let followCostMs = 0;

function updateScrollSpeed(movedPages, t) {
  if (lastSpeedStamp > 0) {
    const dt = t - lastSpeedStamp;
    if (dt > 0 && dt < kSpeedIdleMs) {
      speedPagesPerMs = speedPagesPerMs * 0.6 + (movedPages / dt) * 0.4;
    } else if (dt >= kSpeedIdleMs) {
      speedPagesPerMs = 0;
    }
  }
  lastSpeedStamp = t;
}

function tooFastToFollow() {
  return followCostMs > 0 && followCostMs * speedPagesPerMs > kWindowLookaheadPages;
}

/**
 * 必须装下的那几页：看得见的那几页，外加（还放得下时）紧挨着的光标页。
 *
 * 这是要求，不是"要设成的窗口" —— 两者分开是这一块的关键，理由见 targetWindow。
 * 以看得见的那几页为准：它们必须全在窗口里，否则那几页没有画布（一片空白）。
 *
 *   光标页只在"不挤掉可见页"时才并进要求里：
 *     无条件并进来，会让用户把屏幕滚到别处之后窗口仍被钉在光标页上；
 *     有位置就并但不管可见页，会让可见页的最后一页落到窗口外（同样没有画布）。
 *     两种错法的症状一样（滚动之后一片空白），只有这条断言能分辨它们。
 *   光标不在窗口里时它不画 —— 它本来就在屏幕外，画了也看不见。
 */
function desiredWindow() {
  const total = Math.max(1, lastEdit ? lastEdit.totalPages : 1);
  const vis = visiblePageRange();
  let lo = vis.first;
  let hi = vis.last;
  // 可见页数封顶在 maxWindowPages()：真值本来就不可能到那儿（视口装得下的页数
  //   是 ceil(视口/步距)+1，而 maxWindowPages 是这个数再加 3），所以这一条只会拦下
  //   算歪了的范围 —— 而 range 是拿 getBoundingClientRect 算的，滚动位置刚变、
  //   布局还没落定的那一拍它能虚高好几页。虚高的后果不是画多了，而是滞回判"窗口
  //   盖不住"，白换一次窗：那次 setViewportAnchor 其实和引擎的窗口一样，于是只重画
  //   一个光标，把上一拍整篇重绘的账冲成 186 px（edit_probe 的参考臂就是这么红的）。
  const visCap = maxWindowPages();
  if (hi - lo + 1 > visCap) hi = lo + visCap - 1;
  const cp = lastEdit ? lastEdit.caretPage : -1;
  if (maxWindowPages() - (hi - lo + 1) > 0 && cp >= 0 && cp < total) {
    if (cp === lo - 1) lo = cp;
    else if (cp === hi + 1) hi = cp;
  }
  let first = Math.max(0, lo);
  const need = hi - lo + 1;
  const count = Math.max(1, Math.min(need, total));
  // 下界是"装下可见页"，maxWindowPages 只是它的上界而不是替代品：
  //   舞台比 3 页还高时（最大化窗口、缩小 zoom），宁可多画几页也不能留空白页。
  if (first + count > total) first = Math.max(0, total - count);
  return { first, count };
}

/**
 * 要挪窗口时挪到哪：把多出来的页放在滚动方向上，见 kWindowLookaheadPages。
 *
 * 为什么不直接把要求当成窗口（窗口刚好贴住可见页）：那样每滚一页，要求立刻跑到
 *   窗口外面，于是每滚一页都要整窗重光栅 —— 3 页窗口量到 30 ms、1 页窗口 10 ms，
 *   连续滚动就是十几二十帧的节奏。
 *   留出余量之后，一页滚动落在余量里：窗口不动，引擎一个字都不用重画
 *   （实测这个时候 present 只要 1 ms，画布搬运全交给合成器）。
 * 为什么余量偏向滚动方向而不是两边各一半：两边各一半时，往下滚只有一半余量在前面，
 *   而"前面还剩多少"正好就是换窗那一拍能容许多久（见 kWindowLookaheadPages）。
 *   放 1 页在后面是为了吸收"来回轻推"——完全不给后面留余量时，往回一点就得再换一次窗。
 */
function targetWindow(req, total) {
  const count = Math.max(req.count, Math.min(maxWindowPages(), total));
  const spare = count - req.count;
  const behind = scrollDir < 0 ? Math.max(0, spare - 1) : 1;
  let first = req.first - Math.min(behind, spare);
  if (first + count > total) first = total - count;
  if (first < 0) first = 0;
  return { first, count: Math.max(1, Math.min(count, total)) };
}

/**
 * 按当前滚动位置与光标把视口锚点对一遍。返回 true 表示锚点变了（调用方要重新呈现）。
 *
 * 滞回：现有窗口只要还盖得住"要求 + 滚动方向上 1 页提前量"，就原样留着不动。
 *   判据取要求 + 提前量，而不是"目标窗口" —— 目标带余量、还会随可见页挪，
 *   拿它当判据等于"窗口要永远精确居中"，一滚就动、滞回形同虚设。
 *   但也不能只取要求（旧写法）：那时换窗发生在"可见页已经贴到窗口边缘"的一刻，
 *   而换窗那一拍有 48~67 ms，合成器在这期间继续往前滚 —— 露出来的地方必然空白。
 *   多留这一页，触发时前方就还剩约一页（83 ms @12000 px/s）可以垫。
 *   实测（scripts/diag_selperf.mjs 的 O 段）这条把"可见页没有画布的帧"从 8~9% 压到 0。
 * 没有滞回的话，光标一路往下打字、可见页一格一格地换，每换一格就整帧重建一次，
 * 手感就是"每隔十几个字卡一下"。
 */
function applyWindow() {
  if (!mod) return false;
  const total = Math.max(1, lastEdit ? lastEdit.totalPages : 1);
  const req = desiredWindow();
  const look = kWindowLookaheadPages;
  // 要求先夹到文档范围内：第 0 页往上、最后一页往下都没有"那一页"可留，
  //   不夹的话 needFirst 会是 -1 —— 那是个永远满足不了的判据（窗口不可能从 -1 页起），
  //   于是滞回恒判"盖不住"，每一拍都白换一次窗（文档顶端往上滚过之后，每次
  //   applyWindow 都触发，把整篇重绘那一拍的账冲成"只重画光标"的 186 px）。
  const needFirst = Math.max(0, req.first - (scrollDir < 0 ? look : 0));
  const needLast = Math.min(total - 1,
                            req.first + req.count - 1 + (scrollDir > 0 ? look : 0));
  const cur = lastAnchor.split(':').map(Number);
  if (cur.length === 2 && Number.isFinite(cur[0]) && Number.isFinite(cur[1]) &&
      cur[0] <= needFirst && needLast <= cur[0] + cur[1] - 1) {
    return false;
  }
  const w = targetWindow(req, total);
  const key = w.first + ':' + w.count;
  if (key === lastAnchor) return false;
  lastAnchor = key;
  mod.setViewportAnchor(w.first, w.count);
  return true;
}

/**
 * 换文档之前先把窗口摆到"开头几页"。
 *
 * 换文档会让光标归零、滚动回顶，所以窗口必然落在开头。先设好再重排 —— 顺序反了的话
 * 第一次重排会按上一篇文档留下的锚点栅格化，随后 settle 再挪一次窗口重排，
 * 于是"装载花了多少"这件事被后一次覆盖掉（面板与探针读到的会是"重排 0 段"）。
 *
 * 这里故意开满 maxWindowPages() 页而不是精确算：settle 里的滞回只认"现有窗口盖不盖得住"，
 * 盖得住就不动 —— 开满一定盖得住，于是装载之后不会再挪窗口、不会再重排一次。
 * 精确算成 1 页反而会因为差一页而触发第二次重排（那正是要避免的）。
 */
function presetWindowForLoad() {
  if (!mod) return;
  stageEl.scrollTop = 0;
  const n = maxWindowPages();
  mod.setViewportAnchor(0, n);
  lastAnchor = '0:' + n;
}

/**
 * 把光标滚进视口。编辑之后光标可能落在窗口外（程序化定位、跳段），
 * 那种情况下窗口会自己把光标页并进来，但屏幕上看不见它 —— 所以要滚。
 */
function scrollCaretIntoView(st) {
  if (!st || st.caretPage < 0) return;
  const dpr = window.devicePixelRatio || 1;
  const step = mod.pagePxHeight() + mod.pageGapPx();
  // 光标不在窗口里时（用户把屏幕滚到别处了）caretY 是个"窗口帧外"的数，
  //   拿它算绝对位置会滚到一个毫无关系的地方 —— 直接按页算到那一页的页首。
  const inWindow = st.caretPage >= st.windowFirstPage &&
                   st.caretPage < st.windowFirstPage + st.windowPages;
  // caretY 是"窗口帧内的 y"，它已经含了 (caretPage - 窗口首页) * step 这一项
  //   （见 EditorEngine::caretRectPx 的 off_y）。所以文档坐标 = 窗口首页偏移 + caretY。
  //   写成 caretPage * step + caretY 等于把页偏移算了两遍 —— 点第二页时光标坐标会落到
  //   第三页去，屏幕跟着往下冲两页；随后窗口重算、又一次滚动，看起来就是"乱滚"。
  const top = inWindow ? (mod.windowFirstPage() * step + st.caretY) / dpr
                       : (st.caretPage * step) / dpr;
  const bot = top + Math.max(12, st.caretH / dpr);
  const pad = 18;
  const viewTop = stageEl.scrollTop;
  const viewBot = viewTop + stageEl.clientHeight - 2 * pad;
  if (top - pad < viewTop) stageEl.scrollTop = Math.max(0, top - pad);
  else if (bot + pad > viewBot) stageEl.scrollTop = bot + pad - stageEl.clientHeight;
}

/**
 * 一次操作之后统一收尾：把光标滚进视口 → 对窗口 → 上屏 → 刷面板。
 * 返回最终那一轮的编辑统计。
 *
 * 顺序要紧，两步都不能换：
 *   先滚再对窗口 —— 反过来的话窗口按"旧的滚动位置"算，光标在第 0 页而屏幕还停在
 *     第 15 页时会把窗口拉成整篇（封顶那一层只是兜住它，根子在这里）。
 *   先对窗口再上屏 —— 反过来的话第一次上屏会把帧贴到错的页上（帧覆盖的页与
 *     画布集合不是同一批），画面上就是"别处的内容出现在这一页"。
 */
function settle(st, opts) {
  if (!mod) return st;
  const t0 = performance.now();
  const o = opts || {};
  if (st) lastEdit = st;
  if (o.scroll !== false) scrollCaretIntoView(st);
  if (applyWindow()) {
    mod.present();
    st = JSON.parse(mod.editStatsJson());
    lastEdit = st;
  }
  const t1 = performance.now();
  blitFrame(st);
  const t2 = performance.now();
  if (o.render !== false) {
    renderEdit(st);
    renderForm(st);
    renderLayout(JSON.parse(mod.statsJson()));
  }
  // 宿主侧两段的账：窗口对齐 + 滚动（窗口）+ 上屏 + 刷面板。
  //   引擎那一段由 editStats 报，这两段只有宿主自己知道 —— 而"敲一个字 20 ms"
  //   里它们占了小一半，不记下来就只能一直猜是引擎慢。
  lastBlit.windowMs = t1 - t0;
  lastBlit.settleMs = performance.now() - t0;
  return st;
}

// 页面级异常照旧进 pageErrors（探针断言它是空的），同时降级成一条提示：
//   脚本错误不必然致命（引擎还在），但它绝不该只有控制台知道。
//   监听器写成具名函数是为了 dispose() 能摘掉它们；多实例页面上每个实例都会收到
//   同一条页面级异常 —— 它本来就是页面级的，谁也没法把它归给某一个实例。
function onWindowError(e) {
  if (disposed) return;
  pageErrors.push('error: ' + String(e.message));
  report.warn('页面脚本抛错：' + String(e.message));
}
function onUnhandledRejection(e) {
  if (disposed) return;
  pageErrors.push('unhandledrejection: ' + String(e.reason));
  report.warn('未处理的异步失败：' + String(e.reason));
}
window.addEventListener('error', onWindowError);
window.addEventListener('unhandledrejection', onUnhandledRejection);

const fmt = (n, d = 2) => Number(n).toFixed(d);
const kb = (n) => (n / 1024).toFixed(1) + ' KB';

function setStatus(text, isErr) {
  const el = $('status');
  if (!el) return;   // 产品容器里没有状态栏，不该因此抛异常
  el.textContent = text;
  el.classList.toggle('err', !!isErr);
}

function kv(id, rows) {
  const el = $(id);
  if (!el) return;   // 统计栏同样是演示页专有的
  el.innerHTML = rows
    .map(([k, v, cls]) => `<dt>${k}</dt><dd class="${cls || ''}">${v}</dd>`)
    .join('');
}

/**
 * 胶水层按页加载一次，多个实例共用同一份模块工厂。
 *
 *   每个实例都要调一次 CreateHEditor() 拿自己的引擎（每次都是独立实例），但胶水脚本
 *   本身只该注入一次 —— 重复注入同一个 URL 时浏览器会不会再发一次 load 事件并不确定，
 *   而在这条路上押"会"等于赌一个未定义行为。
 */
let gluePromises = new Map();
function loadScript(src) {
  let p = gluePromises.get(src);
  if (!p) {
    p = new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = src;
      s.onload = resolve;
      s.onerror = () =>
        reject(new Error(`无法加载 ${src}\n先执行：cmake --workflow --preset wasm`));
      document.head.appendChild(s);
    });
    // 失败之后允许下一次重试：把 memo 清掉，否则后续实例会直接复用同一个失败结果。
    p.catch(() => { gluePromises.delete(src); });
    gluePromises.set(src, p);
  }
  return p;
}

/**
 * 启动
 */
/**
 * 字体装载模式：'full' 把整份字节交给引擎（原有方式），'lazy' 先写进 MEMFS
 * 再让引擎按路径按需读（内存里只留一张骨架与块缓存）。探针靠 reloadFonts
 * 在同一页里对照两者。
 */
let fontMode = 'full';

/**
 * 装字体并设好角色。两种模式的差别只在"字节怎么交给引擎"这一步：
 *   lazy：字节写进 MEMFS（数据留在 JS 侧），引擎拿路径按块读；
 *   full：字节穿边界整份交给引擎。
 * 返回装载行给启动面板显示，调用方不必知道模式。
 */
/**
 * 把 markdown 里的示例文本还原成纯段落：去掉标题号、强调记号与行尾的换行符，丢掉空行。
 * 标点、数字与单位原样保留 —— 病历里的体温、剂量、比值都要能原样排版。
 */
function cleanMarkdownLines(raw) {
  return raw
    .split('\n')
    .map((l) =>
      l
        .replace(/\*\*/g, '')
        .replace(/\s*\\$/, '')
        .replace(/^#+\s*/, '')
        .replace(/^\d+[.、]\s*/, '')
        .trim())
    .filter((l) => l.length > 0)
    .join('\n');
}

/**
 * 装真病历样例：读 assets/samples/medical-records.md，清洗后交给引擎按段落建文档。
 *
 * 与其余场景不同，它的内容不在 wasm 产物里而是一份可替换的文件 —— 病历正文本来就走
 * 服务端下发这条路，把二十几 KB 文本编进产物只会多出一份需要同步的副本。
 */
async function applyMedicalSample(repeat) {
  const res = await fetch(new URL('assets/samples/medical-records.md', BASE));
  if (!res.ok) throw new Error(`${res.status}`);
  const text = cleanMarkdownLines(await res.text());
  mod.loadTextDocument(text, Math.max(1, Number(repeat) || 1));
  relayoutAndBlit();
  return { paragraphs: text.split('\n').length, chars: text.length };
}

async function loadFonts(mode) {
  const indices = {};
  const rows = [];
  let bytes = 0;
  const t0 = performance.now();

  for (const f of FONTS) {
    setStatus(`加载字体 ${f.label}…`);
    const res = await fetch(new URL(f.path, BASE));
    if (!res.ok) throw new Error(`${f.path} → HTTP ${res.status}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    bytes += buf.length;

    let idx = -1;
    if (mode === 'lazy') {
      // MEMFS 里用文件名：引擎拿到的是路径，它按块去读。
      //   先 unlink 再建 —— 同一个名字重复 createDataFile 会抛 EEXIST（切档重装时会遇到）。
      //   canOwn 传 true：MEMFS 直接持有这份字节，不再拷一份（十几 MB，这一拷很贵）。
      const name = '/' + f.path.split('/').pop();
      try {
        mod.FS.unlink(name);
      } catch (e) {
        // 文件不在就是常态（第一次装），什么都不用做
      }
      mod.FS.createDataFile('/', name, buf, true, false, true);
      idx = mod.loadFontFromFile(name);
    } else {
      const ptr = mod.allocBuffer(buf.length);  // ① 在 wasm 堆里要一块地
      mod.HEAPU8.set(buf, ptr);                 // ② 字节穿边界（alloc 之后才读 HEAPU8）
      idx = mod.loadFont(f.label, ptr, buf.length);  // ③ wasm 侧解析
      mod.freeBuffer(ptr);
    }
    if (idx < 0) throw new Error(`字体解析失败：${f.path}（${mode}）`);
    indices[f.role] = idx;
    rows.push([f.label, `${kb(buf.length)} → #${idx}（${mode}）`]);
  }

  mod.setFontRoles(indices.cjk, indices.mongol, indices.latin);
  return { indices, rows, bytes, ms: performance.now() - t0 };
}

async function boot() {
  // 产物是用 -msimd128 编的：引擎不支持 SIMD 时，失败发生在胶水层自己去实例化那一步，
  //   报出来的话术却是"先执行 cmake --workflow --preset wasm"，与真实原因对不上。
  //   所以在最前面判一次，抛出的失败自带标题与下一步。
  //   探测模块没加载时不拦：它与拖选同属可选前置，缺了不该让启动整个炸掉。
  if (window.heditorWasmFeature) window.heditorWasmFeature.assertSimd();
  setStatus('加载 WASM 胶水层…');
  // 先问一次"磁盘上那份 wasm 是什么时候构建的"。
  //   加它的理由：重新编译之后"改了没生效"的假象，绝大多数不是没编译，而是浏览器
  //   拿了旧副本（静态服务不发 Cache-Control 时，浏览器按启发式新鲜度直接复用缓存）。
  //   稍后会和 wasm 自己报出来的构建时间对照显示在工具条上，一眼可判。
  //   配 scripts/dev_server.py 的 no-store 才是根治：开发期每次刷新都拿真文件。
  let fileStampRaw = '';
  let fileStamp = '未知';
  try {
    const head = await fetch(WASM_GLUE.href, { method: 'HEAD', cache: 'no-store' });
    fileStampRaw = head.headers.get('last-modified') || '';
    const d = new Date(fileStampRaw);
    if (!Number.isNaN(d.getTime())) {
      const p = (n) => String(n).padStart(2, '0');
      fileStamp = `${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
                  `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
    }
  } catch (err) {
    console.warn('读不到 wasm 的构建时间（不影响启动）', err);
  }

  // 用文件自己的时间当版本号加载胶水层与 wasm。
  //   为什么值得这么绕：静态服务不发 Cache-Control 时，浏览器会按"启发式新鲜度"复用
  //   磁盘缓存 —— 重新编译了却还在跑旧副本，是排查里最费时间的一种假象。
  //   带上 ?v=<文件时间> 之后，URL 随文件本身变化：文件没变就命中缓存（快），
  //   文件变了就是全新 URL（不可能命中旧副本）。
  const cacheBust = fileStampRaw ? '?v=' + encodeURIComponent(fileStampRaw) : '';
  await loadScript(WASM_GLUE.href + cacheBust);
  if (typeof CreateHEditor !== 'function') {
    throw new Error('胶水层已加载，但没有导出 CreateHEditor');
  }

  setStatus('实例化 WASM…');
  const tWasm = performance.now();
  // 注意这个参数对象会被消费：它就是 emscripten 的 Module，工厂会把状态写回去。
  //   所以两次实例化必须各传一个新对象 —— 复用同一个对象会让第二次实例化直接抛
  //   "BindingError: Cannot register multiple overloads of a function..."（实测）。
  //   多实例宿主尤其要守这一条；这里是字面量，天然满足。
  mod = await CreateHEditor({
    // 胶水层是从自己所在的目录拼 wasm 路径的（不含 query），所以这里显式补上 ——
    //   否则会出现"胶水是新的、wasm 是缓存里的旧的"这种最难查的组合。
    locateFile: (path) => new URL(path, WASM_GLUE.href).href + cacheBust,
  });
  const wasmMs = performance.now() - tWasm;

  // 页面跑的到底是哪一版：两次对照的必须是同一种时刻。
  //   runStamp 来自 wasm 自己（编进去的配置时刻），而产物文件的 mtime 是构建时刻 ——
  //   拿两者相减，只要"构建比配置晚五分钟以上"就恒报旧副本，而页面跑的其实是最新那份。
  //   判据改用产物旁边的 heditor.buildstamp（由构建成功时刷新，与编进 wasm 的值同源）：
  //   一致 = 跑的就是磁盘上这一份；不一致才是真旧副本（浏览器拿了缓存里的旧 wasm）。
  let diskStamp = '';
  try {
    const r = await fetch(new URL('heditor.buildstamp', WASM_GLUE.href).href,
                          { cache: 'no-store' });
    if (r.ok) diskStamp = (await r.text()).trim();
  } catch (err) {
    console.warn('读不到构建戳旁证（不影响启动）', err);
  }
  const runStamp = typeof mod.buildStamp === 'function' ? mod.buildStamp() : '未知';
  const staleSeed = diskStamp !== '' && diskStamp !== runStamp;
  const buildEl2 = document.getElementById('build');
  if (buildEl2) {
    buildEl2.textContent = `wasm ${runStamp} ｜ 构建 ${fileStamp}`;
    buildEl2.className = staleSeed ? 'stale' : '';
    buildEl2.title = staleSeed
      ? `页面跑的不是磁盘上这一份（跑 ${runStamp}，产物是 ${diskStamp} 产的）` +
        ` —— 请 Ctrl+Shift+R 硬刷新`
      : '页面实际加载的那份 wasm 的配置时刻，以及产物文件的构建时刻';
  }

  // 字体：JS fetch 进来，字节穿过边界，wasm 包里一个字体都没有。
  //   装载方式由 loadFonts 决定（这里可能是按需那条路，见它的说明）。
  const fontLoad = await loadFonts(fontMode);
  const indices = fontLoad.indices;
  const fontRows = fontLoad.rows;
  const fontBytes = fontLoad.bytes;
  const fontMs = fontLoad.ms;

  kv('boot', [
    ['WASM 构建时间（wasm 自己报的）', runStamp],
    ['WASM 文件时间（磁盘上）', fileStamp],
    ['WASM 实例化', `${fmt(wasmMs, 0)} ms`],
    ['字体 fetch + 解析', `${fmt(fontMs, 0)} ms`],
    ['字体合计', kb(fontBytes)],
    ...fontRows,
    ['wasm 包体', `${kb(wasmBytes())} (js+wasm)`],
  ]);

  // UI 接线
  //
  //   整段按"元素可能不存在"写：演示页有工具条与统计栏，产品容器只有舞台与输入层。
  //   缺元素时跳过接线，而不是抛异常 —— 抛在这里的表现是"编辑器空白"，最难查的一种。
  const sel = chrome ? $('scenario') : null;
  if (sel) {
    sel.innerHTML = SCENARIOS.map((s) => `<option value="${s.id}">${s.label}</option>`).join('');
    sel.value = String(DEFAULT_SCENARIO);
    sel.disabled = false;
    sel.addEventListener('change', () => {
      const id = Number(sel.value);
      if (id === 8) {
        // 真病历样例的内容来自文件，不走 wasm 里的场景表。
        setStatus('读取病历样例…');
        applyMedicalSample(1)
          .then((r) => setStatus(`病历样例：${r.paragraphs} 段 / ${r.chars} 字`))
          .catch((err) => {
            // 一次装载失败不致命：引擎还活着，用户可以直接换一个场景。
            setStatus(`病历样例读取失败：${err.message}`, true);
            report.warn('病历样例读取失败：' + err.message);
          });
        ta.focus();
        return;
      }
      mod.setScenario(id);
      relayoutAndBlit();
      ta.focus();
    });
  }

  const zoom = chrome ? $('zoom') : null;
  if (zoom) {
    zoom.disabled = false;
    zoom.addEventListener('input', () => {
      currentZoom = engineZoomOf(zoom.value);
      syncZoomLabel();
      mod.setZoom(currentZoom);
      relayoutAndBlit();
    });
  }

  const paintBtn = chrome ? $('paint') : null;
  if (paintBtn) {
    paintBtn.disabled = false;
    paintBtn.addEventListener('click', () => relayoutAndBlit());
  }

  const focusBtn = chrome ? $('focus') : null;
  if (focusBtn) {
    focusBtn.disabled = false;
    focusBtn.addEventListener('click', () => ta.focus());
  }

  const benchBtn = chrome ? $('bench') : null;
  if (benchBtn) {
    benchBtn.disabled = false;
    benchBtn.addEventListener('click', runBench);
  }

  // 表单：模式切换 + 样例
  //
  // 策略判断全在 C++ 侧（编辑能力表），这里只是把开关递过去。
  //   如果"能不能改"由 JS 决定，那它就只是个 UI 提示，不是边界。
  const modeSel = chrome ? $('editMode') : null;
  if (modeSel) {
    modeSel.disabled = false;
    modeSel.addEventListener('change', () => {
      mod.setEditMode(Number(modeSel.value));
      relayoutAndBlit();
      ta.focus();
    });
  }

  // 表单样例有三份：相邻的一可编辑加一只读、跨行大字段内嵌只读、三个可编辑字段连排。
  //   三连那一份是给"共享边界"用的：边界两侧都可写，所以在边界上打字、把某个字段删空、
  //   在字段之间退格，全都能当场试出来。
  const variantSel = chrome ? $('formVariant') : null;
  if (variantSel) variantSel.disabled = false;
  const formHint = [
    '表单样例：只有两个字段能改，中间的病程只读 —— 试着往别处敲字',
    '表单样例：一个大字段跨行，里面嵌着一个只读小字段',
    '表单样例：三个可编辑字段紧挨着 —— 在边界上打字 / 把中间那个删空，看括号怎么跟',
    '表单样例：三层嵌套（都可编辑）—— 点最里层的开头打字，三层一起变长',
  ];
  const formBtn = chrome ? $('formSample') : null;
  if (formBtn) {
    formBtn.disabled = false;
    formBtn.addEventListener('click', () => {
      const v = variantSel ? Number(variantSel.value) : 0;
      mod.loadFormSample(v);
      if (modeSel) modeSel.value = String(mod.editMode());
      relayoutAndBlit();
      ta.focus();
      setStatus(formHint[v] || formHint[0]);
    });
  }

  // 光标与选区的样式。控件里的初值从引擎读 —— 默认值只在 C++ 的 EngineConfig 里有一份，
  //   在这里再写一遍就会分成两处，改一处忘一处时界面上显示的和实际画的不是一回事。
  //   选区另给一个 alpha 数字框：它是 90（半透明）而不是 255，取色器表达不了这一档。
  const caretColor = chrome ? $('caretColor') : null;
  const caretWidth = chrome ? $('caretWidth') : null;
  const selColor = chrome ? $('selColor') : null;
  const selAlpha = chrome ? $('selAlpha') : null;
  if (caretColor && caretWidth && selColor && selAlpha) {
    const hex = (c) => '#' + c.slice(0, 3).map((v) => v.toString(16).padStart(2, '0')).join('');
    const rgb = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
    const initCaret = JSON.parse(mod.caretStyleJson());
    const initSel = JSON.parse(mod.selectionStyleJson());
    caretColor.value = hex(initCaret.color);
    caretWidth.value = String(initCaret.widthPt);
    selColor.value = hex(initSel.color);
    selAlpha.value = String(initSel.color[3]);
    for (const el of [caretColor, caretWidth, selColor, selAlpha]) el.disabled = false;
    // input 而不是 change：取色器拖动时要跟着变。引擎只换参数、不作废底图，
    //   所以这条路上每一拍只有一次 present，没有整页重光栅。
    const pushCaret = () => {
      const c = rgb(caretColor.value);
      mod.setCaretStyle(c[0], c[1], c[2], 255, Number(caretWidth.value));
      presentStyleChange();
    };
    const pushSel = () => {
      const c = rgb(selColor.value);
      mod.setSelectionStyle(c[0], c[1], c[2], Number(selAlpha.value));
      presentStyleChange();
    };
    caretColor.addEventListener('input', pushCaret);
    caretWidth.addEventListener('input', pushCaret);
    selColor.addEventListener('input', pushSel);
    selAlpha.addEventListener('input', pushSel);
  }

  // 键盘与 IME：全部由 textarea 承接（canvas 收不到键盘，也收不到合成事件）
  ta.addEventListener('keydown', onKeyDown);
  ta.addEventListener('compositionstart', onCompositionStart);
  ta.addEventListener('compositionupdate', onCompositionUpdate);
  ta.addEventListener('compositionend', onCompositionEnd);
  // 鼠标挂在页容器上而不是某一张画布：事件落在哪一页都走同一条换算。
  pagesEl.addEventListener('mousedown', onCanvasMouseDown);
  // 记下最近一次 pointerdown 的指针 id：mousedown 上没有 pointerId，而按下时要拿它
  //   去 setPointerCapture。用掉就清，合成事件（探针 clickAt 派的 MouseEvent）
  //   不会拿到过期的 id 去乱捕获。
  //   顺便把上一次的捕获放掉：捕获只在 pointerup 时由浏览器自动释放，而"抬起事件
  //   丢了"正是要处理的情形 —— 它会一直挂着，把之后的按下重定向到容器上，
  //   下一次点在页上就起不了拖选。下一拍按下就清，是唯一的自愈点。
  pagesEl.addEventListener('pointerdown', (ev) => {
    if (capturedId !== null) {
      try { pagesEl.releasePointerCapture(capturedId); } catch (e) { /* 已经没了 */ }
      capturedId = null;
    }
    lastPointerId = ev.pointerId;
  });
  stageEl.addEventListener('scroll', onStageScroll, { passive: true });
  // 拖选：按下之后的移动与抬起由共享模块挂在 window 上（拖出画布再拖回来都收得到，
  //   挂在画布上会在出边界那一下就断）。按下时的定位仍走上面那个处理器，模块只管拖动。
  if (typeof window.createDragSelect === 'function') {
    drag = window.createDragSelect({
      container: stageEl,
      pointAt: framePointAt,
      hitAt: (x, y) => {
        const hit = mod.hitTest(x / currentZoom, y / currentZoom);
        return hit === 'null' ? null : JSON.parse(hit);
      },
      extendTo: dragExtendTo,
    });
  } else if (errorSurface) {
    // 不许静默降级：模块没加载成功时"拖不动"要有痕迹，否则会一路查命中测试。
    errorSurface.warn('拖选不可用', { detail: 'web/drag_select.js 未加载' });
  }

  currentZoom = zoom ? engineZoomOf(zoom.value) : currentZoom;
  // 回写一次：让"界面那一格 = 门面的倍数"这条不变量从装载起就成立，而不是靠 HTML 里
  //   那个 value 恰好等于默认档。换算方向写反了在这里就会露出来。
  syncZoomLabel();
  // 页度量要先设再装载：它会让整篇重排，排在装载之前只会排这一次。
  mod.setPageMetrics(kPageWidthPt, kPageHeightPt, kPageMarginPt);
  mod.setZoom(currentZoom);
  // 先把视口锚点摆成"开头几页"，再装载。顺序反了的话第一次重排会按整篇栅格化 ——
  //   200 段那一份就是 16 页、~450 ms，而其中 13 页马上就会被丢掉。
  //   页数这时还不知道，交给引擎夹回文档范围内即可。
  presetWindowForLoad();
  mod.setScenario(DEFAULT_SCENARIO);
  relayoutAndBlit();
  ta.focus();

  ready = true;
  // 旧副本要显眼地说出来：这种情况下下面所有"改了没生效"的观感都是假象，
  //   不提示的话会一路查到底层代码，白费时间。
  if (staleSeed) {
    setStatus(`⚠ 页面跑的不是磁盘上这一份 wasm（跑 ${runStamp}，产物是 ${diskStamp} 产的）` +
              ` —— 按 Ctrl+Shift+R 硬刷新`, true);
  } else {
    setStatus('就绪 · 点画布定位光标，然后敲字');
  }
  resetBlink();
}

/**
 * 上屏：把窗口位图按页切片，贴进各页画布。
 *
 * 四段计时写进 lastBlit：取帧 / 构造 ImageData / putImageData / 摆 IME。
 * 分段的理由与引擎的 paint_fill/glyph/present 同一条：只有分开记，
 * 才知道该优化哪一段，而不是笼统地说"上屏慢"。
 *
 * 切片的做法是 putImageData 的源矩形：位图是整张窗口帧，源 y 取"该页在窗口里的
 * 偏移"，于是不需要在 JS 里再拷一份。页与页之间的间隙不贴 —— 宿主没有它的画布，
 * 它只是引擎帧上的背景色。
 */
function blitFrame(st) {
  const t0 = performance.now();
  const w = mod.frameWidth();
  const h = mod.frameHeight();
  const view = mod.frameRgba();
  if (view.byteLength !== w * h * 4) {
    throw new Error(`帧长度不符：${view.byteLength} ≠ ${w}×${h}×4`);
  }
  const t1 = performance.now();

  const first = mod.windowFirstPage();
  const count = mod.windowPageCount();
  const ph = mod.pagePxHeight();
  const step = ph + mod.pageGapPx();
  syncPageCanvases(first, count, st ? st.totalPages : 1);

  // 帧像素与设备像素一一对应：CSS 尺寸按 devicePixelRatio 折算过（在 syncPageCanvases 里），
  // 浏览器因此不做任何重采样。引擎那边的 zoom 才是"分辨率"的唯一旋钮，DPI 不该再叠一层。
  const img = new ImageData(
    new Uint8ClampedArray(view.buffer, view.byteOffset, view.byteLength), w, h);
  const t2 = performance.now();

  // 只贴引擎报过的脏页。口径与引擎那份 bbox 完全一致（同一批矩形算出来的），
  //   所以不会出现"引擎画了、画布没更新"。光标与表单标记画在帧上而不进底图，
  //   它们所在的那一页也在清单里 —— 这正是"页指纹"给不出的信息。
  //
  //   源矩形与目标位置：putImageData(img, dx, dy, dirtyX, dirtyY, dw, dh) 把
  //     ImageData 上 (dirtyX, dirtyY) 那一块画到画布的 (dx + dirtyX, dy + dirtyY)。
  //     目标位置是"和"，不是 dx/dy —— 写成 putImageData(img, 0, 0, 0, srcY, w, ph)
  //     会把第 2 页起的内容画到画布外面（超出的部分被裁掉、画布一点没变），
  //     症状是"只有第 1 页是对的、其余页停在旧内容上"。所以 dy 要取 -srcY 抵消。
  const dirty = JSON.parse(mod.dirtyPagesJson());
  let patched = 0;
  for (const p of dirty) {
    const rec = pageCanvases.get(p);
    if (!rec) continue;
    const srcY = (p - first) * step;
    rec.g.putImageData(img, 0, -srcY, 0, srcY, w, ph);
    ++patched;
  }
  const t3 = performance.now();

  positionIme(st);
  const t4 = performance.now();

  lastBlit = {
    frameMs: t1 - t0, imageMs: t2 - t1, putMs: t3 - t2, imeMs: t4 - t3,
    totalMs: t4 - t0, full: patched >= count, w, h, boxW: w, boxH: ph,
    framePx: w * h, pages: patched, windowFirst: first,
  };
}

/**
 * 把承接键盘的 textarea 摆到光标处。
 *
 * 这不是 UI 细节，而是架构依赖：IME 的候选框位置由输入元素的屏幕位置决定。
 * 所以"光标几何"（caretRect）是"中文能输得舒服"的前置条件 ——
 * 也就是为什么档 1 不能跳过、必须先把行盒几何做出来。
 *
 * 坐标基准换成页容器：它是 #stage 的滚动内容，1 CSS 像素 = dpr 帧像素，
 * 而它的顶边就是文档第 0 页的顶边 —— 于是"第 k 页 + 页内 y"直接就是文档坐标。
 */
function positionIme(st) {
  // 光标不在窗口里时把输入框收成 1 px：这一帧光标本来就没画，
  //   拿窗口帧外的 caretY 去摆会把输入框（以及它带出来的输入法候选框）
  //   摆到屏幕某个莫名其妙的角落。
  const inWindow = st && st.caretPage >= st.windowFirstPage &&
                   st.caretPage < st.windowFirstPage + st.windowPages;
  if (!st || !st.caretW || !inWindow) {
    ta.style.width = '1px';
    ta.style.height = '1px';
    return;
  }
  const dpr = window.devicePixelRatio || 1;
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const sr = stageEl.getBoundingClientRect();
  const pr = pagesEl.getBoundingClientRect();

  const left = pr.left - sr.left + stageEl.scrollLeft + st.caretX / dpr;
  const top = pr.top - sr.top + stageEl.scrollTop +
              (mod.windowFirstPage() * step + st.caretY) / dpr;
  const lineH = Math.max(12, Math.round(st.caretH / dpr));

  ta.style.left = Math.round(left) + 'px';
  ta.style.top = Math.round(top) + 'px';
  ta.style.width = Math.max(24, Math.round(pagesEl.clientWidth - st.caretX / dpr)) + 'px';
  ta.style.height = lineH + 'px';
  ta.style.lineHeight = lineH + 'px';
  ta.style.fontSize = Math.max(10, Math.round(lineH * 0.62)) + 'px';
}

function afterEdit() {
  // 顺序有讲究：先在编辑引起的那次重绘之后把统计取下来，
  //   这才是"敲一个字写了多少像素"。若先做别的事再读，读到的就是别的账。
  let st = JSON.parse(mod.editStatsJson());

  // 不变量：编辑之后光标必须立即可见。
  //   闪烁相位可能正好停在"隐藏"那半拍上。
  //   现在这条不变量由引擎保证（repaintAfterEdit() 会先把 caret_visible_
  //     置真再重绘），所以下面这个分支正常情况下不会被走到。
  if (!st.caretVisible) {
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    st = JSON.parse(mod.editStatsJson());
  }

  // 窗口对齐、上屏、刷面板、把光标滚进视口 —— 收尾只有这一处，
  //   漏掉任何一步的症状都是"画面停在上一拍"。
  st = settle(st);
  resetBlink();
  return st;
}

function relayoutAndBlit() {
  if (!ready && !mod) return;
  presetWindowForLoad();
  mod.relayoutAndPaint();
  return settle(JSON.parse(mod.editStatsJson()), { scroll: false });
}

/**
 * 光标与选区的样式改完之后收尾：呈现一次并上屏。
 *
 * 为什么不走 relayoutAndBlit：样式参数只在绘制期用（见 setCaretAndSelectionStyle），
 *   重排一遍是白排；而引擎不自己重绘，少了那次 present 画面就停在旧颜色上。
 * 不滚动视口：改颜色与光标在哪无关，把视口拽到光标处是多余的位移。
 */
function presentStyleChange() {
  mod.present();
  return settle(JSON.parse(mod.editStatsJson()), { scroll: false });
}

/**
 * 移动光标并立即呈现。
 * setCaret 只改引擎里的位置，不动像素 —— 漏掉 present() 的症状是
 * "点了画布，光标过一会儿才跳过去"（下次闪烁 tick 才重绘）。
 *
 * 呈现前先把闪烁相位摆到可见，理由与引擎的 repaintAfterEdit() 相同：否则
 * editStats.caretVisible 为 false，afterEdit() 会判定"刚才那次重绘把光标画成隐的"，
 * 再 present 一次并把上屏矩形换成那一轮的（那一轮只写了光标与标记那几小块），
 * 于是引擎帧正确而画布停在旧排版上。
 * 上屏现已由 takeBlitBox() 的累加器保证，这里只是去掉那层冗余：一次点击不该产生两次 present。
 */
function setCaretAndPresent(para, offset) {
  mod.setCaret(para, offset);
  caretOn = true;
  mod.showCaret(true);
  mod.present();
  return afterEdit();
}

/**
 * 键盘
 */
function onKeyDown(ev) {
  if (!ready || busy) return;
  // 组合期间一律不动文档。中文输入法在组合时会持续发 keydown，
  //   若每次都去重排，画面就会又卡又闪 —— WASM 编辑器最容易在这里出问题。
  if (composing || ev.isComposing) return;

  // 撤销 / 重做：Ctrl+Z 撤、Ctrl+Y 重做，Ctrl+Shift+Z 也当重做。
  //
  //   这一段必须排在下面那句"带修饰键一律放行"之前 —— 否则 Ctrl+Z 会被直接交给
  //     浏览器。本页没有可撤销的 DOM 编辑（键盘的承接层是个只读 textarea），
  //     交给浏览器就等于这个键位没绑。
  //
  //   重做收两个键位：Ctrl+Y 与 Ctrl+Shift+Z。两者互不冲突，代价只是多一个判断。
  //     比较前要转小写 —— 真键盘在 shift 按下时给的是大写 'Z'。
  //   Cmd 一并认下：Mac 上 Command 才是惯用修饰键，写在这里免得另开一条分支。
  //
  //   走 wasm 的 undoEdit/redoEdit（引擎侧的 *AndRender 版）：引擎改完状态就把
  //     重排重绘收尾好，这里只管上屏 —— 与上面的 Backspace/Enter 同一个模式。
  //     栈空时返回 false，落到下面那条 resetBlink()，光标重新开始闪。
  if ((ev.ctrlKey || ev.metaKey) && !ev.altKey) {
    const k = (ev.key || '').toLowerCase();
    if (k === 'z' || k === 'y') {
      const redo = (k === 'y') || ev.shiftKey;
      ev.preventDefault();
      if (redo ? mod.redoEdit() : mod.undoEdit()) afterEdit();
      else resetBlink();
      return;
    }
    if (k === 'a') {
      // 全选走引擎。不能让浏览器对 #ime 做原生全选 —— 那只会选中承接层里那一行
      //   假文本（它是空的），与文档毫无关系，而且会把选区交给 DOM，
      //   于是"选中之后打字"落到浏览器手里、文档一个字都不变。
      ev.preventDefault();
      if (mod.selectAll()) afterEdit();
      else resetBlink();
      return;
    }
    if (ev.key === 'Home' || ev.key === 'End') {
      // Ctrl+Home / Ctrl+End = 文档首 / 文档末；行首行尾是裸 Home/End。
      ev.preventDefault();
      const ok = ev.key === 'Home' ? mod.caretToDocStart(ev.shiftKey)
                                   : mod.caretToDocEnd(ev.shiftKey);
      if (ok) afterEdit();
      else resetBlink();
      return;
    }
  }

  if (ev.ctrlKey || ev.metaKey || ev.altKey) return;

  let changed = false;
  let handled = true;

  switch (ev.key) {
    case 'Backspace':
      changed = mod.backspace();
      break;
    case 'Delete':
      changed = mod.deleteForward();
      break;
    case 'ArrowLeft':
      changed = mod.moveCaret(-1, ev.shiftKey);
      break;
    case 'ArrowRight':
      changed = mod.moveCaret(1, ev.shiftKey);
      break;
    case 'ArrowUp':
      // 上下键：沿块轴走到相邻行，列位沿行内轴保持（跨短行不丢列）。
      changed = mod.moveCaretVertical(-1, ev.shiftKey);
      break;
    case 'ArrowDown':
      changed = mod.moveCaretVertical(1, ev.shiftKey);
      break;
    case 'Home':
      // 走引擎的"到行首"，不要用 moveCaret(-1e9) 凑。
      //   之所以"能用"，只是因为 moveCaretBy 走不动时会提前 break；
      //   而且 1e9 表达的是段首段尾，Home/End 要的是行首行尾，语义并不一样。
      changed = mod.caretToLineStart(ev.shiftKey);
      break;
    case 'End':
      changed = mod.caretToLineEnd(ev.shiftKey);
      break;
    case 'Enter':
      // 回车 = 分段（跨段编辑），不是"确认/提交"。
      //   段落模型是文档流的单位，回车在光标处断段，后面那段整体继承样式。
      //   Shift+Enter 暂时与 Enter 同义（软换行留给以后：它需要"行内换行"这种
      //   与段落不同的东西，现在没有，就不要假装有）。
      changed = mod.splitParagraph();
      break;
    case 'Tab':
      // Tab = 缩进（编辑器语义），不是"跳到下一个控件"。
      //   两种模式现在都是这样：文档模式的"Tab = 制表符"与表单模式的"Tab = 跳下一个可编辑
      //     字段"都还没接（引擎侧的 tabCaret 已经就绪，目前只有探针在调）。
      //   Shift+Tab = 反缩进（删掉光标前最多 tab_spaces 个空格）。
      //   走 mod.indent/outdent（*AndRender 路径）：入口改状态就必须收尾，
      //     否则宿主会拿上一次的脏矩形上屏，画布变成新旧混合体。
      changed = ev.shiftKey ? mod.outdent() : mod.indent();
      break;
    default:
      // 可打印字符（中文走 IME，这里处理 ASCII/标点/空格）
      if (ev.key.length === 1) changed = mod.typeText(ev.key);
      else handled = false;
  }

  if (!handled) return;
  ev.preventDefault();          // 别让字符再落进 textarea（那里只是键盘的承接层）

  if (changed) afterEdit();
  else resetBlink();
}

function onCompositionStart() {
  composing = true;
  ta.value = '';
  setStatus('输入法组合中…（此期间不触碰引擎）');
}

function onCompositionUpdate(ev) {
  // 预编辑串先在状态栏本地预览。
  // 把它渲染到正文位置属于档 3 —— 那时才需要"组合期间不改布局"的完整版本。
  setStatus(`组合中：${ev.data || ''}`);
}

function onCompositionEnd(ev) {
  composing = false;
  const text = ev.data || ta.value || '';
  ta.value = '';
  if (text && ready && !busy) {
    if (mod.typeText(text)) afterEdit();
  }
  setStatus('就绪');
}

/**
 * 把一次的客户端坐标换算成"窗口帧坐标（帧像素）"。
 *
 * 事件可能落在任意一张页画布上，所以基准取页容器：页容器里 1 CSS 像素 = dpr 帧像素，
 * 而它的顶边就是文档第 0 页的顶边 —— 于是"页内位置"直接就是文档坐标，再减去窗口
 * 首页的偏移就是帧坐标。这样点第几页都一样，不需要知道点的是哪张画布。
 *
 * 收客户端坐标而不是事件对象：拖选的边缘自动滚动要把指针夹到容器边缘再换算，
 *   那时手上没有事件。事件入口 framePointFromEvent 只是它的一层壳。
 */
function framePointAt(clientX, clientY) {
  const r = pagesEl.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const step = mod.pagePxHeight() + mod.pageGapPx();
  return {
    x: (clientX - r.left) * dpr,
    y: (clientY - r.top) * dpr - mod.windowFirstPage() * step,
  };
}

function framePointFromEvent(ev) {
  return framePointAt(ev.clientX, ev.clientY);
}

/**
 * 拖选的移动端：搬焦点端，不搬 anchor。
 *
 * 走 settle 的"不滚视口"档：拖动过程中每一拍都把光标滚进视口，会把视口拽到焦点端去，
 *   与边缘自动滚动抢同一个 scrollTop，看起来是画面在抖。滚动只由拖选自己那一侧负责。
 */
function dragExtendTo(hit) {
  if (!mod || !mod.setFocusAtSide) return;
  mod.setFocusAtSide(hit.para, hit.offset, hit.side);
  caretOn = true;
  mod.showCaret(true);
  mod.present();
  settle(JSON.parse(mod.editStatsJson()), { scroll: false });
  resetBlink();
}

function onCanvasMouseDown(ev) {
  if (!ready || busy) return;
  // 指针捕获：在浏览器窗口之外松手时页面未必收得到 mouseup —— 那是"松开了还在拖"
  //   的一条来源。捕获之后抬起一定回到容器上（它常驻，不像页画布会被换窗丢掉）。
  //   捕获失败也无所谓：drag_select 那边还有 pointerup/pointercancel/dragend
  //   与"移动时按钮已抬起"两条自愈路。
  if (lastPointerId !== null) {
    try { pagesEl.setPointerCapture(lastPointerId); capturedId = lastPointerId; }
    catch (e) { /* 合成事件没有指针 */ }
    lastPointerId = null;
  }
  const pt = framePointFromEvent(ev);
  // hitTest 收的是帧磅坐标，而页容器上量到的是帧像素 —— 除以 zoom 换算。
  const hit = mod.hitTest(pt.x / currentZoom, pt.y / currentZoom);
  let onText = false;
  if (hit !== 'null') {
    onText = true;
    const p = JSON.parse(hit);
    // 连"这一下点的是哪一个位置"一起带过去。
    //   相邻两个字段共享边界时（[反复]│[心悸]，两枚括号并排在同一列上），落点是同一个
    //   偏移，光有偏移分不出用户点的是哪一侧；同一个偏移上还可能压着好几枚括号（相邻的
    //   空字段就是 ][][ ），于是"字与括号之间""两个控件之间"整段都点不进去。
    //   引擎在命中测试里已经算出了侧别（括号占位各占一格，数格子即可），这里只负责搬运。
    //   p.field 是它的投影，只用来显示"当前在哪个字段"，搬运请用 p.side。
    if (typeof p.side === 'number' && mod.setCaretAtSide) {
      mod.setCaretAtSide(p.para, p.offset, p.side);
      caretOn = true;
      mod.showCaret(true);
      mod.present();
      afterEdit();
    } else {
      setCaretAndPresent(p.para, p.offset);
    }
  }
  // 定位之后再交给拖选：这一下已经把 anchor 摆在按下点，之后的移动只搬焦点端。
  //   落在灰底上（没命中正文）时只定位不拖选 —— 从页外的空白起手不该选中任何东西。
  //   这条路上命中是同步的，所以 begin 直接带上结论；两宿主的分工保持同形
  //   （Worker 侧命中是往返，先 begin(false) 再 arm）。
  if (drag) drag.begin(ev, false);
  if (drag) drag.arm(onText);
  ta.focus();
  ev.preventDefault();
}

/**
 * 光标闪烁
 *
 * 这个函数只负责"把定时器重置到可见相位"。呈现与记账在 tick 里做。
 * （在这里也 present + blit 一次，结果是编辑后多跑一趟，
 * 还把面板上的数字覆盖成闪烁那一笔。）
 */
function resetBlink() {
  caretOn = true;
  if (blinkTimer) clearInterval(blinkTimer);
  blinkTimer = setInterval(() => {
    if (!ready || busy || composing) return;
    caretOn = !caretOn;
    mod.showCaret(caretOn);
    mod.present();
    // 统计必须在 present() 之后读。
    //   present 之前拿到的是上一帧的脏矩形，拿它去 putImageData 会贴错位置 ——
    //   症状是"光标走了，残影留在原地"。所以这里只读一次，且读完就用。
    //   闪烁这一拍不滚视口：它每 530ms 来一次，跟着滚会把用户正在看的地方拽走。
    const st = settle(JSON.parse(mod.editStatsJson()), { render: false, scroll: false });
    renderBlink(st);
  }, 530);
}

/**
 * 滚动：可见页变了就挪窗口，再重新呈现。
 *
 * 用 rAF 合流：滚动事件一次手势能来几十个，每个都重排会让滚动变成幻灯片。
 * 只有"窗口真的变了"才重排（见 applyWindow），窗口内滚动一格都不碰引擎。
 */
let scrollRaf = 0;
let lastScrollTop = 0;

// 每轮滚动之后往前预热一页：一页约 10~30 ms，且要等滚动静下来 90 ms 才动手。
//   这两个限量的理由都是"别把事件循环占死"：预热一旦成串地跑，它就把滚动自己的
//   那一拍、输入与探针的求值全排在后面 —— 那不是优化，是拿一个卡顿换另一个。
const PREFETCH_PAGES = 1;
const PREFETCH_DELAY_MS = 90;
let prefetchDir = 1;
let prefetchNext = -1;
let prefetchLeft = 0;
let prefetchTimer = 0;

/**
 * 空闲一拍里预热"再往前会露出来的那一页"的字形。
 *
 * 为什么需要它：滚动换窗那一拍要重新光栅新露出来的那几页，而冷字形占那一拍的一大半。
 *   同一条路上同一页量到：冷的 14.6 µs·字形、整拍 44.5 ms；预热带过之后 7.0 µs、25.7 ms。
 *   把这笔钱挪到"当前画面还够用"的空闲帧里先付掉，换窗那一拍就短一截。
 * 每轮换窗只排一页、且要等 90 ms 没有新滚动才真的跑：连续快滚时它几乎不跑（那是
 *   对的，那时候每一拍都该留给滚动），滚动一停就先把下一页烘上。
 */
function schedulePrefetch(dir) {
  if (!ready || !mod || typeof mod.prewarmPageGlyphs !== 'function') return;
  prefetchDir = dir < 0 ? -1 : 1;
  prefetchNext = prefetchDir > 0 ? mod.windowFirstPage() + mod.windowPageCount()
                                 : mod.windowFirstPage() - 1;
  prefetchLeft = PREFETCH_PAGES;
  if (prefetchTimer) clearTimeout(prefetchTimer);
  prefetchTimer = setTimeout(pumpPrefetch, PREFETCH_DELAY_MS);
}

function pumpPrefetch() {
  prefetchTimer = 0;
  if (!ready || !mod || prefetchLeft <= 0) return;
  const total = Math.max(1, lastEdit ? lastEdit.totalPages : 1);
  if (prefetchNext < 0 || prefetchNext >= total) return;
  mod.prewarmPageGlyphs(prefetchNext);
  prefetchNext += prefetchDir;
  --prefetchLeft;
}

/**
 * 连续放弃的对数上限：放弃状态不许无限持续。
 *
 * 判据是实测推出来的，而实测会高估 —— 平滑后的速度在惯性尾部降得比真实慢，一直放弃的
 *   后果是"画面停在早已滚过的那一屏"。所以每放弃这么多拍就强制对齐一次：对齐的成本只与
 *   窗口大小有关（整窗重建 5 页 ≈ 55 ms），偶发一次完全可以承受，而它保证画面每隔
 *   一小段时间就回到正确的位置上。
 */
const kMaxSkipRun = 6;
let skipRun = 0;

/**
 * 滚动停下后的收尾对齐。
 *
 * 为什么必须有：放弃期间画面故意停在旧位置，而停下来之后不会再有 scroll 事件
 *   —— 没有事件就没有下一拍，画面就永远停在旧位置上（看到的正是"停下后还要等"）。
 *   所以放弃时挂一个定时器：安静够久就对齐一次。用定时器而不是 scrollend 事件，
 *   是因为放弃期间本来就在等"安静下来"这件事，两者是同一个条件，多一个事件源只是
 *   多一条要维护的降级路径。
 */
const kScrollSettleMs = 140;
let settleTimer = 0;

function alignAfterScroll() {
  settleTimer = 0;
  if (!ready || busy || !mod) return;
  if (performance.now() - lastSpeedStamp < kScrollSettleMs) {
    // 还在动：再等一轮。这样连续滚动时它一直不会对齐，也就不会与放弃判据打架。
    settleTimer = setTimeout(alignAfterScroll, kScrollSettleMs);
    return;
  }
  // 停了：速度归零，下一拍起恢复正常跟随，并立刻把窗口对齐到当前可见页。
  speedPagesPerMs = 0;
  skipRun = 0;
  if (!applyWindow()) return;
  mod.present();
  settle(JSON.parse(mod.editStatsJson()), { scroll: false });
}

function onStageScroll() {
  if (!ready || busy || scrollRaf) return;
  scrollRaf = requestAnimationFrame(() => {
    scrollRaf = 0;
    if (!ready || busy || !mod) return;
    const t = performance.now();
    const top = stageEl.scrollTop;
    const dirPx = top - lastScrollTop;
    lastScrollTop = top;
    // 方向只用来决定"多出来的余量放前面还是后面"（见 targetWindow），
    //   所以只在真的动了的时候更新 —— 把 0 差值当下行会让来回轻推翻转方向。
    if (dirPx !== 0) scrollDir = dirPx > 0 ? 1 : -1;
    const dpr = window.devicePixelRatio || 1;
    const step = mod.pagePxHeight() + mod.pageGapPx();
    const movedPages = Math.abs(dirPx) * dpr / Math.max(1, step);
    updateScrollSpeed(movedPages, t);
    if (tooFastToFollow() && skipRun < kMaxSkipRun) {
      ++skipRun;
      // 放弃期间必须留一个收尾：停下之后不会再有 scroll 事件，见 alignAfterScroll。
      if (!settleTimer) settleTimer = setTimeout(alignAfterScroll, kScrollSettleMs);
      return;
    }
    skipRun = 0;
    // 这一拍被占住多久：从对齐窗口到上屏完成。引擎那一段（present）不在 settle 里，
    //   settleMs 只记了宿主侧的上屏与面板（实测 5 ms 对 45 ms），所以这里自己量整段。
    const tWork = performance.now();
    if (!applyWindow()) return;
    mod.present();
    const st = JSON.parse(mod.editStatsJson());
    settle(st, { scroll: false });
    lastScrollWorkMs = performance.now() - tWork;
    followCostMs = followCostMs === 0 ? lastScrollWorkMs
                                      : Math.min(followCostMs * 1.05, lastScrollWorkMs);
    schedulePrefetch(Math.sign(dirPx));
  });
}

/**
 * 面板
 */
function renderEdit(st) {
  const pct = st.framePixels ? (100 * st.repaintedPixels) / st.framePixels : 0;
  // 三态，不能压成两态：
  //     没重排（移动光标/闪烁） / 重排了且是全量 / 重排了且增量
  //   把它压成"是否全量"的布尔，就会把"根本没重排"误报成"已经增量了"。
  const laid = !!st.layoutRan;
  // 档 2 之后"全量"不再是一个缺陷：整篇标脏（换文档、改页度量）和"脏段就是第一段"
  //   （单段文档、在开头打字）都只能全量 —— 没有前缀可复用。所以这里只报事实，
  //   增量给 ok，全量不加色（加了就成了假警报）。
  const incr = laid && !st.layoutWasFull;
  kv('edit', [
    ['本轮是否重排', laid ? '是' : '否（只重绘）', laid ? undefined : 'ok'],
    ['重排段 / 复用段 / 总段',
      `${st.relaidParagraphs} / ${st.reusedParagraphs} / ${st.totalParagraphs}`, incr ? 'ok' : undefined],
    // 复用有两种：原样搬回（后缀一个字节都没动）与整体平移（位置变了、断行没重算）。
    // 分开报是因为它们对"脏页数"的含义完全不同 —— 平移之后后缀的每一页都可能要重画。
    ['后缀复用方式', !laid ? '—' : (st.suffixShifted ? '整体平移（断行结果复用）' : '原样搬回')],
    ['重排行 / 总行', `${st.relaidLines} / ${st.totalLines}`, incr ? 'ok' : undefined],
    ['布局粒度', !laid ? '— 本轮无布局' : (st.layoutWasFull ? '全量' : '增量'),
      incr ? 'ok' : undefined],
    ['脏页 / 总页', `${st.dirtyPageCount} / ${st.totalPages}`],
    ['窗口页 / 首页', `${st.windowPages} / ${st.windowFirstPage}`],
    // 行带：把"这一页要重画"缩到"这一页的哪几行"。一页约 1000 个字形，
    //   敲一个字只动一两行 —— 页粒度下改一行与改整页同价。
    ['重画行带（页内 y）', st.bandY1 < 0 ? '—（本轮没重画）' : `${st.bandY0} ~ ${st.bandY1} px`],
    ['重画页数', st.repaintedPages],
    ['重画区域', `${st.repaintedW} × ${st.repaintedH} px`, 'hi'],
    ['重画像素', `${fmt(st.repaintedPixels, 0)}`, 'hi'],
    ['占全帧', `${fmt(pct, 3)} %`, 'hi'],
    ['— layout', `${fmt(st.layoutMs)} ms`],
    ['— pageFp', `${fmt(st.pageFpMs)} ms`],
    ['— paint', `${fmt(st.paintMs)} ms`],
    ['— 合计', `${fmt(st.totalMs)} ms`],
    // 引擎那四行只算到帧缓冲组装完。跨界与上屏是另外的账，分开记的理由：
    //   该优化哪一边完全取决于钱在哪一段 —— putImageData 的代价随画布面积走，
    //   取帧与 ImageData 构造都是常数级，而它们加起来可能比引擎那一段还大。
    ['— 上屏取帧', `${fmt(lastBlit.frameMs)} ms`],
    ['— putImageData', `${fmt(lastBlit.putMs)} ms`, 'hi'],
    ['— 上屏合计', `${fmt(lastBlit.totalMs)} ms`, 'hi'],
    // 宿主收尾 = 窗口对齐 + 滚动 + 面板刷新。它不含上屏那几段（那几行在上面），
    //   而"敲一个字"的总账 = 引擎（上面那四行）+ 上屏 + 这一行。
    ['— 宿主收尾（窗口+面板）',
      `${fmt(Math.max(0, (lastBlit.settleMs || 0) - lastBlit.totalMs))} ms`, 'hi'],
    ['上屏页数 / 窗口页数', `${lastBlit.pages} / ${mod.windowPageCount()}`],
    ['窗口', `第 ${lastBlit.windowFirst} 页起 · 帧 ${lastBlit.w} × ${lastBlit.h} px`],
    ['帧面积（=画布面积）', `${(lastBlit.framePx / 1e6).toFixed(2)} Mpx`],
    ['光标', `段 ${st.caretPara} · 偏移 ${st.caretOffset} 码点`],
    ['光标矩形', `${st.caretX},${st.caretY} ${st.caretW}×${st.caretH} px`],
    ['全文码点数', st.docCodepoints],
  ]);
}

/**
 * 表单面板。
 *
 * 这一块必须放在界面上，理由是：策略生效时编辑是静默返回 false 的 ——
 * 表现就是"打字没反应"，而那与"程序卡住/出错"在观感上完全一样。
 * 「被拒编辑次数」是这两者之间唯一廉价的分辨器。
 */
function renderForm(st) {
  const MODE = ['文档', '表单', '只读'];
  const m = Number(st.editMode) || 0;
  kv('form', [
    ['编辑模式', MODE[m] || '?', m === 0 ? 'ok' : 'warn'],
    ['控件数', st.controlCount],
    ['光标所在控件', st.caretControl < 0 ? '—（不在任何控件内）' : `#${st.caretControl}`,
      st.caretControl < 0 && m === 1 ? 'warn' : ''],
    ['被拒编辑次数', fmt(st.blockedEdits, 0), st.blockedEdits > 0 ? 'warn' : 'ok'],
  ]);
}

/**
 * 光标闪烁单独一块：它和"敲字"是两条不同的路径，数字不该互相覆盖。
 * 闪烁走的是"底图一个字都没变、只把光标那一小块贴回去"，
 * 所以它的重画像素数应当是敲字的几千分之一 —— 这正是档 1 要证明的增量能力。
 */
function renderBlink(st) {
  const pct = st.framePixels ? (100 * st.repaintedPixels) / st.framePixels : 0;
  kv('blinkOut', [
    ['重画像素', fmt(st.repaintedPixels, 0), 'hi'],
    ['占全帧', `${fmt(pct, 4)} %`, 'hi'],
    ['重画区域', `${st.repaintedW} × ${st.repaintedH} px`],
    ['重画页数', st.repaintedPages],
    ['光标', st.caretVisible ? '可见' : '隐藏'],
    ['— paint', `${fmt(st.paintMs)} ms`],
  ]);
}

/**
 * 面板上"paint"那一行必须写明是谁在光栅化。
 * 两个后端在帧尺寸/重画页数/写入像素数这些指标上完全一致，
 * 写死一个名字就等着误导排查（见 wasm_main.cpp 里 rasterBackend 的注释）。
 */
function rasterBackendLabel() {
  try {
    return (mod && mod.rasterBackend) ? mod.rasterBackend() : '未知后端';
  } catch (e) {
    return '未知后端';
  }
}

function renderLayout(st) {
  kv('layout', [
    ['页数', st.pageCount],
    ['字形数', st.glyphCount, 'hi'],
    ['行 / 列数', st.placedLines],
    ['簇数', st.clusters],
    ['字体回退 run 数', st.fontRunCount],
    ['缺字（.notdef）', st.missingGlyphCount === 0 ? '0 ' : st.missingGlyphCount,
      st.missingGlyphCount === 0 ? 'ok' : 'warn'],
    ['— shape', `${fmt(st.shapeMs)} ms`],
    ['— paint（' + rasterBackendLabel() + '）', `${fmt(st.paintMs)} ms`],
    // 绘制三段：铺面积 / 字形 / 帧搬运。分开报的理由与引擎里那三个字段一致 ——
    //   它们各自对应一种完全不同的优化手段，合成一个数就只能得出"绘制慢"。
    ['  · 铺面积', `${fmt(st.paintFillMs)} ms`],
    ['  · 字形', `${fmt(st.paintGlyphMs)} ms`, 'hi'],
    // 每字形微秒：分不出"字多"与"每个字贵"就等于没量。
    //   几百个字形 1 ms（1~3 µs/个）是正常量级；上了 10 µs/个 才是光栅本身的问题。
    ['  · 每字形', st.paintedGlyphs ? `${fmt(st.paintGlyphMs * 1000 / st.paintedGlyphs, 2)} µs` +
                                     `（${st.paintedGlyphs} 个）` : '—'],
    ['  · 帧搬运', `${fmt(st.paintPresentMs)} ms`],
    ['字形缓存 命中 / 未命中', `${st.glyphCacheHits} / ${st.glyphCacheMisses}`],
    ['帧尺寸', `${mod.frameWidth()} × ${mod.frameHeight()} px`],
    ['页尺寸 / 页间隙', `${mod.pagePxWidth()} × ${mod.pagePxHeight()} / ${mod.pageGapPx()} px`],
    ['视口窗口', `第 ${mod.windowFirstPage()} 页起 · ${mod.windowPageCount()} 页` +
      `（${pageCanvases.size} 张页画布）`],
    ['帧字节数', kb(mod.frameByteSize())],
  ]);
}

/**
 * 引擎内循环基准
 */
function runBench() {
  if (!mod || busy) return;
  const btn = $('bench');
  busy = true;
  if (btn) btn.disabled = true;
  setStatus('引擎内循环 12 次…');
  try {
    const r = mod.benchRelayoutPaint(12);
    kv('benchOut', [
      ['次数', r.runs],
      ['shape 平均', `${fmt(r.shapeMs)} ms`],
      ['layout 平均', `${fmt(r.layoutMs)} ms`],
      ['paint 平均', `${fmt(r.paintMs)} ms`, 'hi'],
      ['每帧合计', `${fmt(r.totalMs)} ms`, 'hi'],
      ['理论帧率（仅排版+光栅）', `${fmt(1000 / r.totalMs, 0)} fps`],
    ]);
    setStatus(`基准完成 · 每帧 ${fmt(r.totalMs, 0)} ms`);
  } catch (e) {
    setStatus(e.message, true);
    console.error(e);
  } finally {
    busy = false;
    if (btn) btn.disabled = false;
  }
}

function wasmBytes() {
  let n = 0;
  for (const e of performance.getEntriesByType('resource')) {
    if (e.name.includes('heditor')) n += e.transferSize || e.decodedBodySize || 0;
  }
  return n;
}

// 对外诊断接口
//
// 这些接口是给无头探针（scripts/edit_probe.mjs）用的，所以刻意分成两组，
//   不要合并：
//     layout*  —— 引擎本次排版的统计（页数/字形数/缺字/分阶段耗时）
//     edit*    —— 引擎本次编辑的统计（重排行数/重画像素/光标矩形）
//   两组的数字来源完全不同；把它们混在一个对象里，探针就会拿"页数"去断言
//   一个根本不存在的字段，然后静默通过。
const facade = {
  get ready() { return ready; },
  get errors() { return pageErrors.slice(); },
  get edit() { return lastEdit; },
  get text() { return mod ? mod.documentText(0) : ''; },
  // 按段取文本 + 整篇拼起来。跨段编辑的探针要核对"字符账守恒"（分段不丢不重），
  //   而 __heditor.text 只给第 0 段。
  paragraphText(i) { return mod ? mod.documentText(i) : ''; },
  get allText() {
    if (!mod) return '';
    const n = JSON.parse(mod.editStatsJson()).totalParagraphs;
    const parts = [];
    for (let i = 0; i < n; ++i) parts.push(mod.documentText(i));
    return parts.join('\n');
  },
  get caret() {
    if (!mod) return null;
    const st = JSON.parse(mod.editStatsJson());
    return { para: st.caretPara, offset: st.caretOffset, index: st.caretIndex,
             side: st.caretSide, page: st.caretPage,
             x: st.caretX, y: st.caretY, w: st.caretW, h: st.caretH,
             visible: st.caretVisible, docCodepoints: st.docCodepoints };
  },
  get caretReport() { return mod ? mod.caretReport() : ''; },
  get env() {
    return { wasmGlue: wasmBytes(), ua: navigator.userAgent, url: location.href };
  },
  // 兼容旧探针的字段名
  get fonts() { return this.env; },
  // 排版统计（= statsJson）
  get stats() { return mod ? JSON.parse(mod.statsJson()) : null; },
  // 编辑统计（= editStatsJson）
  get editStats() { return mod ? JSON.parse(mod.editStatsJson()) : null; },
  // 宿主侧上屏耗时（最近一轮的四段分解），见 lastBlit。
  //   与 editStats 并列而不是塞进去：这两笔账的来源不同（一个是 wasm 里的计时，
  //   一个是 JS 的 performance.now），合成一个对象会让探针分不清单位归属。
  get blitStats() { return lastBlit; },

  // 压力文档：造 N 段、每段 M 字。性能判据只能在大文档上做 —— 演示场景只有一段，
  //   量出来的编译与索引耗时全是 0.0x ms，看不出 O(文档) 的那一层。
  loadStressDocument(paras, chars) {
    if (!ready) return null;
    mod.loadStressDocument(paras, chars);
    relayoutAndBlit();
    return this.editStats;
  },

  // 同上，但每段用一批不同的常用字 —— 给"字形集合要有规模"的判据用。
  //   整篇同一个字时字形缓存与块缓存都会命中到失真，按需读的账量不出来。
  loadStressDocumentDiverse(paras, chars) {
    if (!ready) return null;
    mod.loadStressDocumentDiverse(Number(paras), Number(chars));
    relayoutAndBlit();
    return this.editStats;
  },

  // 用真文本造文档：宿主把文件读进来、清洗掉 markdown 记号后传进来，重复 repeat 遍。
  //   合成文本（一批码点循环）的用字分布不像真病历，缓存账会偏，所以判据要能喂真文本。
  loadTextDocument(text, repeat) {
    if (!ready) return null;
    mod.loadTextDocument(text, Number(repeat) || 1);
    relayoutAndBlit();
    return this.editStats;
  },

  // 真病历样例：从文件读、清洗后按段落建文档。页面下拉与探针都走这一条，
  //   清洗逻辑只此一份。repeat 用来把语料重复放大到长文档规模。
  async loadMedicalSample(repeat) {
    if (!ready) return null;
    const r = await applyMedicalSample(repeat);
    return Object.assign({}, r, { stats: this.editStats });
  },

  // 切场景并按 zoom 重排重绘（供探针用；走的是和 UI 完全相同的路径）
  //
  // 这里的 zoom 是密度（设备像素 / 磅），与引擎和 zoomLevel 同口径 —— 门面不做单位
  //   换算，按倍数调就用 zoomToFactor。输入框那一格收的才是倍数，写回去要除以 kZoomBase。
  //
  // 返回值就是"本次全量呈现"的统计 —— 探针要拿它当增量比值的分母。
  //   分母只能是真正的全量，不能取"再 present 一次"的数字：后者本身已经是增量
  //   （帧尺寸没变，只贴了个旧光标），拿它当分母会得出"100%"这种荒谬结论。
  run(id, zoom) {
    if (zoom) {
      currentZoom = Number(zoom);
      syncZoomLabel();
      mod.setZoom(currentZoom);
    }
    mod.setScenario(Number(id));
    presetWindowForLoad();
    mod.relayoutAndPaint();
    return settle(JSON.parse(mod.editStatsJson()), { scroll: false });
  },
  reset(id, zoom) { return this.run(id === undefined ? 6 : id, zoom === undefined ? kZoomBase : zoom); },

  // 按倍数设缩放（界面输入框的入口）。门面其余成员收的都是密度。
  //
  // 走的收尾与 run 同一条路（重排 + 重绘），只是不换场景。
  zoomToFactor(factor) {
    currentZoom = engineZoomOf(factor);
    syncZoomLabel();
    mod.setZoom(currentZoom);
    relayoutAndBlit();
    return currentZoom;
  },

  // 直接调引擎（绕开键盘），走和真人编辑相同的上屏步骤。
  //
  //  这里必须 blitFrame，不能只调引擎。
  //    只调引擎的话，画布还停在旧内容上，而引擎内部已经画完了 ——
  //    紧接着任何一次"只有光标脏"的 present（比如闪烁 tick）只会贴光标那一小块，
  //    画布于是永远停在旧文字上，症状是"光标往后走、插入处的字不动、两个叠在一起"。
  //    探针 [2b] 的 __heditor.selfCheck() 当场量出过 9372 个像素不一致。
  //    探针钩子必须和真人路径一样把画面收尾，否则它自己就是 bug 的制造者。
  type(s) {
    if (!ready || !mod.typeText(s)) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  backspace() {
    if (!ready || !mod.backspace()) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  deleteForward() {
    if (!ready || !mod.deleteForward()) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  // 跨段编辑：段首退格 / 段末 Delete 已经自动走合并（见引擎 backspace/deleteForward），
  //   所以这里只需要给回车与"显式合并"各一个入口。
  splitParagraph() {
    if (!ready || !mod.splitParagraph()) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  mergeWithPreviousParagraph() {
    if (!ready || !mod.mergeWithPreviousParagraph()) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  mergeWithNextParagraph() {
    if (!ready || !mod.mergeWithNextParagraph()) return null;
    return settle(JSON.parse(mod.editStatsJson()));
  },
  // 返回 null 表示"光标没动"（引擎返回 false 就不重绘）——
  //   探针据此断言"已经在段首了，再左移不会重绘"。
  // extend = Shift 扩展：锚点不动，选区从锚点那一端继续长。
  move(dcp, extend) {
    if (!ready || !mod.moveCaret(dcp || 1, !!extend)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  // 上下键（dir < 0 上 / > 0 下）、文档首尾、全选、选区读写。
  //   全部走引擎的 *AndRender 版 —— 与真人按键同一条路径，探针拿到的统计才有意义。
  vertical(dir, extend) {
    if (!ready || !mod.moveCaretVertical(dir || 1, !!extend)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  docStart(extend) {
    if (!ready || !mod.caretToDocStart(!!extend)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  docEnd(extend) {
    if (!ready || !mod.caretToDocEnd(!!extend)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  selectAll() {
    if (!ready || !mod.selectAll()) return null;
    return JSON.parse(mod.editStatsJson());
  },
  setSelection(anchor, focus) {
    if (!ready || !mod.setSelection(anchor, focus)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  clearSelection() {
    if (!ready) return null;
    mod.clearSelection();
    return JSON.parse(mod.editStatsJson());
  },
  // 选区取证：两端（全局流位置）+ 页面坐标矩形。空选区给空 rects。
  //   是属性不是方法 —— 与 caretReport / caretLive 同一个形态，两个宿主的门面
  //   在这里必须逐字对齐（探针两边跑同一份脚本）。
  get selection() { return mod ? JSON.parse(mod.selectionReport()) : null; },
  // 拖选的诊断面：state() 说"现在在不在拖、移动过没有、自动滚动起没起"，
  //   settled() 等所有在途的命中落地（异步宿主下拖动是几次往返，不等就读会读到旧选区）。
  //   单线程宿主上命中是同步的，settled() 立刻兑现 —— 探针两边写同一句。
  get drag() { return drag ? drag.state() : null; },
  dragSettled() { return drag ? drag.settled() : Promise.resolve(); },
  // 走的是和"点画布"完全相同的路径（改位置 + 呈现），
  //   所以探针拿到的统计和真人操作得到的是一回事。
  setCaret(para, offset) { setCaretAndPresent(para, offset); return this.caret; },
  // 带归属的定位 —— 与真人点画布走的同一条路径（hitTest 给出归属，
  //   宿主原样回传）。探针要验证"点后一个字段的开头"就必须走它，走 setCaret 是另一条路。
  setCaretAt(para, offset, field) {
    if (!ready) return null;
    mod.setCaretAt(para, offset, field);
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    afterEdit();
    return this.caret;
  },
  // 带侧别的定位 —— 与点画布完全相同的那条路（onCanvasMouseDown 搬的就是侧别）。
  //   归属只是侧别的投影，同一个偏移上压着好几枚括号时投影会抹平细节，所以
  //   探针要复现"点在两个控件之间"这类手势，得走这一个而不是 setCaretAt。
  setCaretAtSide(para, offset, side) {
    if (!ready) return null;
    mod.setCaretAtSide(para, offset, side);
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    afterEdit();
    return this.caret;
  },
  // 点画布的真实入口（与页容器上那个 mousedown 处理同一套坐标换算），供探针复现用户手势。
  //   收的是窗口帧像素；帧坐标 → 文档坐标 → 屏幕坐标，反向走一遍 framePointFromEvent。
  clickAt(xPx, yPx) {
    if (!ready) return null;
    const r = pagesEl.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const step = mod.pagePxHeight() + mod.pageGapPx();
    pagesEl.dispatchEvent(new MouseEvent('mousedown', {
      clientX: r.left + xPx / dpr,
      clientY: r.top + (yPx + mod.windowFirstPage() * step) / dpr,
      bubbles: true, cancelable: true,
    }));
    return this.caret;
  },
  // 只改位置不呈现 —— 仅用于需要把"改位置"和"呈现"分账的对照实验
  setCaretOnly(para, offset) { mod.setCaret(para, offset); return this.caret; },
  hit(xPt, yPt) { return JSON.parse(mod.hitTest(xPt, yPt)); },
  bench(runs) { return JSON.parse(mod.benchRelayoutPaint(runs || 12)); },

  // 表单：控件是区间，边界是策略
  //
  // 这三个 setter 都走 present() 收尾 —— 和真人路径一样。
  //   只调引擎不收尾，画布就会停在旧内容上（同上，必须在同一个调用里收尾）。
  get editMode() { return mod ? mod.editMode() : 0; },
  get controls() { return mod ? JSON.parse(mod.controlsJson()) : null; },
  // 占位槽清单：每一枚括号落在哪个偏移、是开还是闭、属于哪个字段、在哪一行。
  //   括号的排布只存在于排版结果里（区间数据看不出来），探针要钉住
  //   "两个零宽字段的括号交替成 ][][]、而不是互相嵌套" 就得读这一份。
  get bracketSlots() { return mod ? JSON.parse(mod.bracketSlotsJson()).slots : null; },
  // 直读，不走 editStats：编辑被拒时不会 repaint，而 editStats 是在呈现时刷新的 ——
  //   走那条路读到的永远是上一拍的数字，探针按键后立刻读会差一拍。
  get blockedEdits() { return mod ? mod.blockedEdits() : 0; },
  setEditMode(m) {
    mod.setEditMode(Number(m));
    return this.present();
  },
  // 括号显示范围：0 = All（默认，所有字段常驻）、1 = Focused（旧行为，只显示光标所在字段）。
  // 引擎侧会自己重排（括号进文档流，占位变了），宿主只需上屏。
  setBracketScope(s) {
    mod.setBracketScope(Number(s));
    return this.present();
  },
  get bracketScope() { return mod ? mod.bracketScope() : 0; },
  // 光标与选区的样式（色值 [r,g,b,a]，宽度单位磅）。  //   引擎只换参数、不作废底图（见 setCaretAndSelectionStyle），所以这里补一次 present
  //   就够 —— 走 setConfig 那条路会把窗口里的每一页重新光栅，取色器连续改时每一下都付。
  //   两个取值器与两个 setter 的读写形状一致，探针两边写同一句。
  get caretStyle() { return mod ? JSON.parse(mod.caretStyleJson()) : null; },
  get selectionStyle() { return mod ? JSON.parse(mod.selectionStyleJson()) : null; },
  // 宿主侧上屏那四段（取帧 / ImageData / putImageData / 摆 IME）。引擎的 paintMs 不含它 ——
  //   滚动时"一卡一下"的总时长是两段之和，分开记才知道该优化哪一边。
  get lastBlit() { return lastBlit; },
  // 预热一页的字形：不呈现、不改画面，只把 mask 光栅进缓存。返回触及的字形数。
  prewarmPage(page) { return mod ? mod.prewarmPageGlyphs(page | 0) : 0; },
  // 部分字段缺省时按现状补全：面板往往只想改颜色，不该被迫把宽度也读一遍。
  setCaretStyle(style) {
    if (!ready) return null;
    const cur = JSON.parse(mod.caretStyleJson());
    const c = (style && style.color) || cur.color;
    const w = style && style.widthPt !== undefined ? Number(style.widthPt) : cur.widthPt;
    mod.setCaretStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0, w);
    return presentStyleChange();
  },
  setSelectionStyle(style) {
    if (!ready) return null;
    const cur = JSON.parse(mod.selectionStyleJson());
    const c = (style && style.color) || cur.color;
    mod.setSelectionStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0);
    return presentStyleChange();
  },
  // 用一份区间清单整批重建控件（探针用）。
  //   为什么不逐个改：区间树的父子关系是全局性质，改一个字段的端点会改变别的
  //   字段的 depth/parent，必须整棵重建。只读标记也只有建的时候给得进去，
  //   所以"把某个字段临时标成只读、看它那一对括号变不变灰"要靠这条。
  //   引擎那边一次建完（换名单 + 重建树在同一次调用里），中间态不存在，
  //   光标连侧别由引擎自己搬 —— 宿主不必再存一份。
  setFormControls(list) {
    mod.setFormControls(list.map((it) => ({
      start: it.start, end: it.end, kind: it.kind || 0,
      id: it.id || '', ro: !!it.ro,
    })));
    return relayoutAndBlit();
  },
  // 走 relayoutAndBlit 而不是 present()：loadFormSample 会换文档，
  //   必须重排。只 present 的话，控件区间会投影到上一次布局的行盒上
  //   （实测症状：4 个字的字段被切成两块，因为它在旧几何里跨了行）。
  loadFormSample(variant) {
    mod.loadFormSample(variant === undefined ? 0 : variant);
    return relayoutAndBlit();
  },
  // 帧自审：当前帧 vs "整帧重画一遍"，逐字节比。
  //   selfCheck 只能抓"上屏贴错"；这个能抓"引擎帧本身就是错的"（旧标记没擦掉）。
  //   跑完会顺带把这一帧修成真值。
  auditFrame() { return mod ? JSON.parse(mod.auditFrame()) : null; },

  // 底图自审：平移复用的底图 vs "从零全建"，逐字节比。
  //   auditFrame 比的是帧，而帧是从底图搬的 —— 底图本身错了它照样全绿。
  //   滚动换窗走的就是底图平移，所以这一条是那条路唯一的判据。
  auditBase() { return mod && mod.auditBase ? JSON.parse(mod.auditBase()) : null; },

  // 排版自审：增量续排的结果 vs 一次全量重排，逐字段比对（diff 必须为 0）。
  //   顺便带回两个耗时：curMs 是刚才那次增量，fullMs 是本次审计做的全量 ——
  //   分子分母同一次测量，探针拿它们算"增量是全量的百分之几"。
  auditLayout() { return mod && mod.auditLayout ? JSON.parse(mod.auditLayout()) : null; },

  // 编译自审：本轮实际编译耗时与一次整树全量编译同源对照。
  //   这是"编译这一层有没有做到增量"的唯一证据 —— 只有分子没有分母的耗时数说明不了事。
  auditCompile() { return mod && mod.auditCompile ? JSON.parse(mod.auditCompile()) : null; },

  // 本轮写过哪几页（绝对页号）—— 宿主按页上屏的依据，也是"帧切页之后"取代
  //   原先那个单矩形 blitBox 的判据：一次上屏现在贴的是若干整页，不是一个矩形。
  //   （引擎的 takeBlitBox 仍在，单测 edit_test 用它钉"一次操作两次 present 的账
  //     不会互相挤掉"；宿主这条路已经不吃它了。）
  get dirtyPages() { return mod && mod.dirtyPagesJson ? JSON.parse(mod.dirtyPagesJson()) : null; },

  // 给探针用的：直接摆光标可见相位（真实闪烁由定时器摆，探针要能自己定住）。
  //   存在的理由：用户的 bug 只在"光标停在隐藏那半拍"时触发 ——
  //   而所有老断言开头都调过 stopBlink()，正好把这一半概率全屏蔽掉了。
  showCaret(on) { if (!mod) return false; mod.showCaret(!!on); return true; },

  // Tab / Shift+Tab = 缩进 / 反缩进（走真实引擎路径，会自己收尾）
  indent() { if (!mod.indent()) return null; return this.present(); },
  outdent() { if (!mod.outdent()) return null; return this.present(); },

  // 返回 null = 光标没动（比如只有一个字段）
  // 注意：Tab 不再绑到它。Tab 是缩进。这个接口留着给探针/自动化用。
  focusControl(forward) {
    if (!mod.focusControl(forward === undefined ? true : forward)) return null;
    return this.present();
  },
  // 策略版 Tab：表单模式在可编辑控件之间前进/后退，到最后一个返回 null（不消费）。
  //   与 focusControl 的区别是"不绕回第一个"且判据走可编辑集合谓词（见 engine.h）。
  //   默认键位同样不绑它 —— 裸 Tab 留给缩进，绑什么由宿主决定。
  tabCaret(backward) {
    if (!mod.tabCaret(backward === undefined ? false : !!backward)) return null;
    return this.present();
  },
  controlAt(xPt, yPt) { return mod.controlAtPoint(xPt, yPt); },
  describeControls() { return mod ? mod.describeControls() : ''; },

  // 删控件 / 改控件属性：结构操作的引擎入口（宿主 UI 还没接，先给探针与自动化用）。
  //   keepContent = true 是"拆壳"（字段内容留下来变成普通正文）。
  //   表单模式默认被 structureLock 拒（返回 null，不消费）；探针要删就先切文档模式。
  //   引擎侧走的是 AndRender 版，这里只负责把它上屏 —— 与 indent/outdent 同一个写法。
  removeControl(i, keepContent) {
    if (!mod.removeControlAt(Number(i), !!keepContent)) return null;
    return this.present();
  },
  // 改运行时属性：prop 0 = hidden、1 = readonly、2 = required、3 = structureLock。
  //   幂等（值没变）时引擎返回 true 但不重排重绘，这里的账也随之为"没重画"。
  setControlProp(i, prop, value) {
    if (!mod.setControlPropAt(Number(i), Number(prop), !!value)) return null;
    return this.present();
  },

  // 撤销 / 重做（栈空时返回 null）。
  //   默认键位在 onKeyDown 里：Ctrl/Cmd+Z 撤、Ctrl+Y 或 Ctrl+Shift+Z 重做。
  //   这里留着的是同一件事的宿主门面版：探针与自动化用它（不必造键盘事件），
  //   也不受 ready/busy 限制 —— 键位那条路要受。
  /**
   * 重装字体并切换装载模式（'full' 整份交给引擎 / 'lazy' 写 MEMFS 后按需读）。
   *   探针靠它在一页里对照两种模式：引擎侧只新增脸、不做替换，所以两种模式的脸
   *   会同时留在库里 —— 按需那条路的统计只算有数据源的脸，不受影响。
   */
  async reloadFonts(mode) {
    const m = mode === 'lazy' ? 'lazy' : 'full';
    const r = await loadFonts(m);
    fontMode = m;
    relayoutAndBlit();
    return { mode: fontMode, rows: r.rows };
  },
  get fontMode() { return fontMode; },
  // 改字体块缓存的配置（字节 / 块数），并清空已有块与统计 —— 给"块大小该取多少"做实验。
  //   注意它同时会改"之后新装的脸"用的配置，所以换档之后要重装字体才干净。
  setFontCacheConfig(blockSize, maxBlocks) {
    mod.setFontCacheConfig(Number(blockSize), Number(maxBlocks));
    return true;
  },
  // 视口锚点：探针量"按需读覆盖多少页"时用。走真实滚动路径受布局与 rAF 节流影响，
  //   直接设锚点更稳，也正好把"换窗口就要读新字形"这件事单独量出来。
  setViewportAnchor(first, count) {
    mod.setViewportAnchor(Number(first), Number(count));
    return this.present();
  },
  undo() { if (!mod.undoEdit()) return null; return this.present(); },
  redo() { if (!mod.redoEdit()) return null; return this.present(); },

  // 撤销栈的深度上限（条数，0 = 不限制）。
  //   默认 100（EngineConfig::undo_limit）—— 与 Office 系和 Quill 的 maxStack 同值。
  //   改它不动呈现，所以这里不上屏；调小时当场裁掉多余的条目。
  //   粒度是一笔编辑一步、没有时间合并，同样 100 条覆盖的输入比 Quill 短。
  setUndoLimit(n) { mod.setUndoLimit(Number(n)); return this.undoLimit(); },
  undoLimit() { return mod.undoLimit(); },

  // 给探针用的：把引擎当前那一帧原样拷出来
  //
  //  必须拷一份，不能直接返回 mod.frameRgba()：那返回的是 wasm 堆上的视图，
  //    任何一次 present（哪怕只是光标闪烁的一 tick）都会把它原地覆盖。
  //    探针要拿"一个一个敲进去"和"一次性插入"这两帧做逐字节比较，
  //    不拷的话两次读到的永远是同一块内存 —— 这条断言会永远绿，且毫无意义。
  rawFrame() {
    const v = mod.frameRgba();
    return new Uint8ClampedArray(new Uint8ClampedArray(v.buffer, v.byteOffset, v.byteLength));
  },
  frameW() { return mod.frameWidth(); },
  frameH() { return mod.frameHeight(); },

  // 按当前光标可见相位重画一次并上屏。
  //
  //  探针需要它来"定格相位"：闪烁每 530ms 自己 present 一次，
  //    两次采样若各自落在不同半拍上，比对出来的差异里就混着闪烁，
  //    真正的渲染差异反而被淹掉。定格之后两帧才有可比性。
  present() {
    if (!ready) return null;
    mod.present();
    return settle(JSON.parse(mod.editStatsJson()));
  },

  // 一键自检：把屏幕上真实的像素和引擎那一帧逐字节比一遍，外加缩放/DPI 现场。
  //
  // 存在的理由："输入时光标走了、插入处的字没走、两个叠在一起"，
  //   在开发机上一比一复现不出来（连打 60 字 + 闪烁在跑 + 110ms 节奏，
  //   屏幕与引擎帧逐字节相同）。既然症状是"看到的和引擎画的不一致"，
  //   那就在出问题的那台机器上当场量一次，比来回猜快得多。
  //
  //   用法：F12 控制台 →  __heditor.selfCheck()  把返回的对象发出来。
  //   screenVsEngineDiffPx > 0 就说明"上屏"这一步贴错了（引擎帧本身是对的）；
  //   恒为 0 就说明问题在引擎的排版/光栅化，而不在上屏。
  //
  //   帧被切成每页一张画布之后，判据的口径跟着改：宿主手里只有页的像素（页与页之间
  //   的间隙只是引擎帧上的背景色，没有画布），所以逐字节比的是"窗口内每一页"，
  //   页间隙不比。覆盖范围与"整篇一张画布"时相同 —— 少拼一页就等于漏检一页。
  selfCheck() {
    if (!ready) return null;
    const w = mod.frameWidth();
    const h = mod.frameHeight();
    const ph = mod.pagePxHeight();
    const step = ph + mod.pageGapPx();
    const first = mod.windowFirstPage();
    const count = mod.windowPageCount();
    const F = mod.frameRgba();
    let diff = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
    let comparedPages = 0;
    for (const [p, rec] of pageCanvases) {
      const local = p - first;
      if (local < 0 || local >= count) continue;
      ++comparedPages;
      const g = rec.g;
      const S = g.getImageData(0, 0, w, ph).data;
      const base = local * step * w * 4;
      for (let i = 0; i + 3 < S.length; i += 4) {
        const j = base + i;
        if (j + 3 >= F.length) break;
        if (S[i] !== F[j] || S[i + 1] !== F[j + 1] || S[i + 2] !== F[j + 2]) {
          ++diff;
          const q = i >> 2, x = q % w, y = (q - x) / w + local * step;
          if (x < minx) minx = x;
          if (x > maxx) maxx = x;
          if (y < miny) miny = y;
          if (y > maxy) maxy = y;
        }
      }
    }
    const st = JSON.parse(mod.editStatsJson());
    const r = pagesEl.getBoundingClientRect();
    return {
      text: mod.documentText(0),
      docCodepoints: st.docCodepoints,
      caret: { para: st.caretPara, offset: st.caretOffset, x: st.caretX, y: st.caretY,
               w: st.caretW, h: st.caretH, visible: st.caretVisible, page: st.caretPage },
      framePx: [w, h],
      window: { first, count, comparedPages },
      pageCanvases: pageCanvases.size,
      pages: {
        backing: [mod.pagePxWidth(), ph],
        cssSize: [pagesEl.style.width, pagesEl.style.height],
        rectPx: [Math.round(r.width), Math.round(r.height)],
      },
      dpr: window.devicePixelRatio,
      zoom: currentZoom,
      backend: mod.rasterBackend ? mod.rasterBackend() : '?',
      screenVsEngineDiffPx: diff,
      screenVsEngineDiffBox: diff ? [minx, miny, maxx, maxy] : null,
      repainted: { pages: st.repaintedPages, pixels: st.repaintedPixels,
                   w: st.repaintedW, h: st.repaintedH },
    };
  },

  // 表单标记自检：当场核对"每个字段的括号都画出来了"
  //
  // 给用户的取证入口。是"有时括号渲染不更新" —— 这类间歇性
  //   问题在开发机上复现不出来，所以把判据做成一调用，让他在出问题的那台机器上量。
  //
  // 判据（默认档 All）：每个可编辑字段的左缘外侧都必须有括号墨迹，
  //   与光标在哪无关（括号常驻）。Focused 档（scope=1）下才退回旧判据：
  //   聚焦字段有括号、其余为空。
  //   用法：F12 控制台 →  __heditor.markCheck()   把返回的对象发出来。
  //   allOk=false 就说明确实有字段没画对，rows 里能看到是哪个、差多少。
  markCheck() {
    if (!ready || !mod) return null;
    const cc = JSON.parse(mod.controlsJson());
    const f = this.rawFrame();
    const fw = this.frameW();
    const focusedOnly = this.bracketScope === 1;
    // 与探针同一条判据：偏蓝够远 + 够亮 + 红通道 > 45（把光标色排除掉）
    const near = (r, g, b) => r > 45 && b - r > 60 && b > 140;
    const rows = [];
    for (let ci = 0; ci < cc.items.length; ++ci) {
      const it = cc.items[ci];
      if (it.ro || !it.rects.length) continue;
      const cx = it.rects[0].x;
      const lo = Math.round((cx - 10) * currentZoom);
      const hi = Math.round((cx - 1) * currentZoom);
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const x = (i >> 2) % fw;
        if (x < lo || x > hi) continue;
        if (near(f[i], f[i + 1], f[i + 2])) ++n;
      }
      const focused = ci === cc.caret;
      rows.push({
        control: ci,
        focused,
        leftEdgePx: n,
        ok: focusedOnly ? (focused ? n > 0 : n === 0) : n > 0,
      });
    }
    return {
      mode: cc.mode,
      bracketScope: this.bracketScope,
      count: cc.count,
      caretControl: cc.caret,
      zoom: currentZoom,
      caret: this.caret,
      rows,
      allOk: rows.every((r) => r.ok),
    };
  },

  // 给探针用的：把闪烁停掉
  //  必须有这个口子。闪烁每 530ms 会 present() 一次并覆盖 edit_stats_；
  //   探针"派事件 → 读统计"之间只要落进一次 tick，读到的就是闪烁那一笔
  //   （几百像素而不是整页），断言会随机失败 —— 那种测试比没有更糟。
  //
  //  这里不做 present()：present 会把 edit_stats_ 覆盖成"只重画了个光标"，
  //   而探针紧接着要读的往往是"上一次全量呈现"的数字（分母）。停表就只停表。
  stopBlink() {
    if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
    caretOn = true;
    mod.showCaret(true);
    return true;
  },
  startBlink() { resetBlink(); return true; },

  // 引擎实时光标偏移，直读不经过 editStats。
  //   editStats 是"上一次呈现"写下的账；拿它和这个比，就能判出
  //   "有没有哪个改了状态的入口忘了给自己收尾"（Tab 跳字段就栽在这上面）。
  get caretLive() { return mod ? mod.caretOffsetLive() : -1; },

  // 给探针用的 DOM 句柄（键盘路径要往真实元素上派事件，不能直接调引擎）
  get textarea() { return ta; },
  // 帧画布的代理（见 frameCanvasProxy）。帧已按页切成多张画布，这个代理按"窗口帧"
  //   算几何、事件转发给页容器，老探针因此一行都不用改。
  get canvas() { return frameCanvasProxy; },
  // 真正持有像素的那批页画布（绝对页号 → canvas），供新探针核对"窗口内每页都在"。
  get pageCanvases() {
    const out = [];
    for (const [p, rec] of pageCanvases) out.push({ page: p, el: rec.el });
    return out.sort((a, b) => a.page - b.page);
  },
  // 窗口里当前有画布的页号（升序）。与 Worker 宿主的同名取值器逐字对齐 ——
  //   探针要数"看得见的那几页里有几张没有画布"（没有画布 = 屏幕上一片空白），
  //   两个宿主给的形状不一致的话，那一句就得分成两处写。
  get pageCanvasList() { return [...pageCanvases.keys()].sort((a, b) => a - b); },
  // 页在帧像素下的几何 [宽, 高, 间隙]。
  get pagePx() {
    return mod ? [mod.pagePxWidth(), mod.pagePxHeight(), mod.pageGapPx()] : null;
  },
  // 视口窗口现场：宿主与引擎各自认为窗口是哪几页（两者必须一致）。
  get viewport() {
    if (!mod) return null;
    return { anchor: lastAnchor, engineFirst: mod.windowFirstPage(),
             engineCount: mod.windowPageCount(),
             desired: desiredWindow(), visible: visiblePageRange(),
             pagePx: [mod.pagePxWidth(), mod.pagePxHeight(), mod.pageGapPx()],
             canvases: pageCanvases.size, pooled: canvasPool.length };
  },
  // 同一件事的两种口径：zoomLevel 是密度（设备像素 / 磅），zoomFactor 是倍数
  //   （1 = 100% = 96 dpi）。界面上那一格与 zoomFactor 一致；引擎与判据读 zoomLevel。
  get zoomLevel() { return currentZoom; },
  get zoomFactor() { return uiZoomOf(currentZoom); },
  get scenario() {
    const el = $('scenario');
    return el ? Number(el.value) : DEFAULT_SCENARIO;
  },
  // 这一份实例的根节点（多实例页面上用来分清是哪一个）。
  get container() { return root; },
  // 引擎是否已经不可用（与 Worker 宿主的同名成员同形）。
  get dead() { return dead; },
  // 启动结果。刻意 resolve 成 { ok, error } 而不是失败即 reject：没人接的 rejection
  //   会变成一条未处理的异步失败，把真正的失败原因淹掉。
  get whenReady() { return readyPromise; },
  /**
   * 拆掉这一份实例。
   *
   * DOM、定时器、监听、错误界面、画布引用都摘掉；wasm 堆交给 GC（Emscripten 实例
   *   没有显式销毁接口）。拆完 ready 变回 false，门面剩下的取值器一律给空值 ——
   *   一个已经拆掉的实例继续报告内容，就是最难查的那种假象。
   */
  dispose() {
    if (disposed) return false;
    disposed = true;
    dead = true;
    ready = false;
    if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
    window.removeEventListener('error', onWindowError);
    window.removeEventListener('unhandledrejection', onUnhandledRejection);
    for (const [, rec] of pageCanvases) if (rec.el.parentNode) rec.el.parentNode.removeChild(rec.el);
    pageCanvases.clear();
    canvasPool.length = 0;
    if (errorSurface) errorSurface.clear();
    mod = null;
    return true;
  },
  get disposed() { return disposed; },
  // 错误降级面的取证句柄。界面本身带 data-host-error 属性，探针也可以直接从 DOM 读；
  //   这里给一份对象是为了让"哪个实例降级了"在多实例页面上分得清。
  get hostErrors() {
    return {
      fatalActive: report.fatalActive,
      bannerText: errorSurface ? errorSurface.bannerText : '',
      bannerDetail: errorSurface ? errorSurface.bannerDetail : '',
      toastTexts: errorSurface ? errorSurface.toastTexts : [],
      entries: report.entries,
      dead,
    };
  },
};

/**
 * 启动这一份实例。
 *
 * 放在门面定义之后：boot() 会碰门面的取值器（例如 ready），而 errorSurface 与
 *   report 在更早处就已经就绪，所以这里的先后只影响可读性。
 */
const readyPromise = (async () => {
  try {
    await boot();
    return { ok: true };
  } catch (e) {
    const msg = String((e && e.message) || e);
    dead = true;
    setStatus(msg, true);
    // #note 是演示页的说明栏，产品页不会有它 —— 取不到就只留横幅，不能因此再抛一次。
    const note = $('note');
    if (note) note.innerHTML = `<b>启动失败。</b><br>${msg.replace(/\n/g, '<br>')}`;
    // 探测层（wasm_feature.js）抛的失败自带标题与下一步，原样展示。
    //   不这么做的话，浏览器不支持 SIMD 会被说成"先执行 cmake"—— 而产物明明在。
    report.fatal((e && e.heditorTitle) || '编辑器启动失败', {
      detail: msg,
      hint: (e && e.heditorHint) ||
            '先执行：cmake --workflow --preset wasm；若用 ?wasm= 指定了产物目录，检查它是否存在。',
      retry: () => location.reload(),
    });
    console.error(e);
    return { ok: false, error: e };
  }
})();

return facade;
}

// 页面入口
window.createEditor = createEditor;

/**
 * 演示页（index.html）自动起一个实例并把门面挂到 window.__heditor 上。
 *
 *   容器给 body：页面上的 #stage / #pages / #ime 与工具条都在里面，行为与"整页一个
 *   编辑器"的旧形态逐条一致。多实例页面在加载本文件之前写
 *   window.__heditorAutoBoot = false，由页面自己调 createEditor。
 */
if (window.__heditorAutoBoot !== false && document.getElementById('stage')) {
  window.__heditor = createEditor(document.body, { chrome: true });
}

})();   // 文件级作用域结束
