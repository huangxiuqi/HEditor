// 引擎侧：命令表、状态、窗口计算。两侧宿主共用这一份。
//
// 为什么单独成文件：worker 宿主与单线程宿主的差别只有三处（帧去哪、结果何时到、
//   输入层怎么挂），引擎访问面本身没有差别。命令名、状态归属、窗口滞回这三样只要
//   各写一遍，两个宿主就会从"同构"漂成"两套"—— 实测两边的命令名交集只有 35 个，
//   而 app.js 直调了 96 个方法。收成一份之后，新增引擎能力只动这张表。
//
// 边界：本文件不碰 document、不碰 DOM、不碰 OffscreenCanvas。凡是要碰画布的地方
//   都走 cfg.host 的钩子（ctxOfPage / blitPages / invalidateSlots ...），由宿主实现。
//   这样同一份代码能跑在 Worker 里，也能跑在主线程里。
//
// 通道（与 web/engine_worker.js、未来的 transport_worker.js 的约定）：
//   下行：boot / pageCanvas / viewCanvas / view / do（改状态）/ ask（要返回值）
//   上行：ready / state / ack / reply / error
(function () {
'use strict';

/**
 * 造一份引擎 API。
 *
 * @param {object} cfg 传输与呈现钩子
 * @param {function} cfg.post 上行发送；worker 侧是 postMessage，主线程侧是直接回调
 * @param {object} cfg.host 画布钩子，见文件头的三条分叉
 * @returns {object} boot / do / ask / view / pageCanvas / viewCanvas / state / relayout
 */
function createEngineApi(cfg) {
const post = cfg.post;
const host = cfg.host;

const err = (e, where) =>
  post({ t: 'error', where: where || '', message: String((e && e.stack) || e) });

let mod = null;
let dpr = 1;
let viewW = 1;
let viewH = 1;
let scrollY = 0;
// 宿主报来的提前量（设备像素，连页模式下可能是负的）。它只用来选窗口 —— 真实落点一律按
//   scrollY 算，两者混用会让连页的内容跑到滚动位置前面去（停稳再弹回来）。
let scrollLead = 0;
let lastAnchor = '';
let scrollReset = false;   // 引擎自己归零过滚动位置，要通知宿主追平
let bootMs = 0;
let fontMs = 0;
let fontBytes = 0;
let fontList = [];
let fontMode = 'full';
// 与单线程宿主同一口径：缩放 1 倍 = 96 dpi = 4 / 3。
const kZoomBase = 4 / 3;
let zoomLevel = kZoomBase;   // markCheck 要用：括号左缘的像素位置随缩放走
// 页度量由宿主传，与单线程宿主同一组值（常量与理由见 app.js 的同名声明）。
// A4 = 595.276 x 841.89 磅；页边距 2 厘米 = 2 x 72 / 2.54 磅。
const kPageWidthPt = 595.276;
const kPageHeightPt = 841.89;
const kPageMarginPt = 56.7;

/* 字体 */

async function fetchFont(path) {
  const res = await fetch(path);
  if (!res.ok) throw new Error(path + ' -> HTTP ' + res.status);
  return new Uint8Array(await res.arrayBuffer());
}

/**
 * 两种装载方式。full 把整份字节交给引擎；lazy 先写进 MEMFS 再让引擎按路径按需读。
 *   MEMFS 在 Worker 里同样可用（它就是 JS 侧的内存文件系统），所以两条路都能在这边跑。
 */
async function installFonts(mode) {
  const t0 = performance.now();
  const idx = {};
  let bytes = 0;
  for (const f of fontList) {
    const buf = await fetchFont(f.path);
    bytes += buf.length;
    let ex = -1;
    if (mode === 'lazy') {
      const name = '/' + f.path.split('/').pop();
      try { mod.FS.unlink(name); } catch (e) { /* 不存在就算了 */ }
      // canOwn=true：MEMFS 直接持有这份字节，省掉一次十几 MB 的拷贝。
      mod.FS.createDataFile('/', name, buf, true, false, true);
      ex = mod.loadFontFromFile(name);
    } else {
      const ptr = mod.allocBuffer(buf.length);
      mod.HEAPU8.set(buf, ptr);
      ex = mod.loadFont(f.label, ptr, buf.length);
      mod.freeBuffer(ptr);
    }
    if (ex < 0) throw new Error('字体解析失败: ' + f.path);
    idx[f.role] = ex;
  }
  mod.setFontRoles(idx.cjk, idx.mongol, idx.latin);
  fontMs = performance.now() - t0;
  fontBytes = bytes;
  fontMode = mode;
  return bytes;
}

/* 几何 */

function geom(st) {
  const ph = mod.pagePxHeight();
  const step = ph + mod.pageGapPx();
  const first = mod.windowFirstPage();
  return {
    pageW: mod.pagePxWidth(),
    pageH: ph,
    gap: mod.pageGapPx(),
    first,
    count: mod.windowPageCount(),
    // 页画布在文档里的落点（设备像素）：纵向 = 页号 × 步距，横向由宿主居中。
    pageStep: step,
    frameW: mod.frameWidth(),
    frameH: mod.frameHeight(),
    dpr, viewW, viewH, scrollY,
    // 内容底边（设备像素）。连页下宿主拿它当滚动高度 —— 没有页可数，只有内容。
    contentPx: mod.contentPx ? mod.contentPx() : 0,
    // 槽数的上限（与文档位置无关，只随视口与页度量变）。宿主按它建槽、再把自己实际
    //   建了几个报回来（pageCanvas.slots）—— 一个数、一个来源，两侧不会分叉。
    maxWindowPages: maxWindowPages(),
    // 引擎侧的帧槽数（宿主切帧按它算源行）。它必须等于上面那个上限 —— 同一个数。
    frameSlots: mod.frameSlots ? mod.frameSlots() : 0,
    // 这一侧实际在用的槽数，供诊断读。它必须等于宿主报回来的那个值。
    slots: host.slotCount(),
    pagesReady: host.pagesMissing() === 0,
    // 与单线程宿主的 viewport 同形的两格：锚点原文与"该装哪几页"。
    //   探针靠它们判"窗口跟着可见页走"，缺了就是一条 .first 的 TypeError。
    anchor: lastAnchor,
    desired: st ? desiredWindow(st) : null,
    visible: st ? visibleRange(st) : null,
  };
}

/**
 * 按当前滚动位置决定窗口。返回 true 表示窗口变了（调用方要重排重画）。
 *
 * 滞回（与单线程宿主的 applyWindow 同一口径）：现有窗口只要还盖得住"看得见的那几页"，
 *   就一个字都不动。没有这一层的话，滚动每跨一条页边界就整帧重光栅（一页约 13 ms）；
 *   有了它，同一页内的滚动连一次重贴都不需要 —— 浏览器把画布搬走就行。
 */
// 看得见的那几页（设备像素下的滚动位置与视口高度）。
//   这里用的是"报了提前量的位置"（scrollY + scrollLead）：提前量只该影响"先装哪几页"，
//   让窗口朝滚动方向多留一点，不要等滚到了才开始装。它不能影响"画在哪儿" ——
//   连页模式下内容落点是按真实滚动位置算的（见 blitPageContinuous）。
function visibleRange(st) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const at = scrollY + scrollLead;
  const first = clamp(Math.floor(at / step), 0, total - 1);
  const last = clamp(Math.floor((at + viewH - 1) / step), first, total - 1);
  return { first, last };
}

/**
 * 窗口最多装几页：视口装得下的页数，再加 4 页余量。与单线程宿主的 maxWindowPages
 *   同一口径（那条注释给了实测数字：换窗成本正比于窗口长度而非位移，余量 8 时一跳要
 *   94~286 ms，余量 4 时 45 ms 上下；余量大了反而把落后从 3 页推到 8~28 页）。
 *   两道封顶同理：页数 8、帧像素 24 Mpx。
 */
const kWindowMarginPages = 4;
const kWindowMaxPages = 8;
const kWindowMaxPixels = 24 * 1024 * 1024;

function maxWindowPages() {
  const step = mod.pagePxHeight() + mod.pageGapPx();
  const vis = Math.ceil(viewH / step);
  const byPixels = Math.floor(kWindowMaxPixels /
                               Math.max(1, mod.pagePxWidth() * mod.pagePxHeight()));
  return Math.max(vis, Math.min(kWindowMaxPages, byPixels, vis + kWindowMarginPages));
}

/**
 * 该装哪几页（这是要求，不是"要设成的窗口"）。与单线程宿主的 desiredWindow 同一口径
 *   （那边注释解释了为什么光标页只在"不挤掉可见页"时才并进来）。
 */
function desiredWindow(st) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const vis = visibleRange(st);
  let lo = vis.first;
  let hi = vis.last;
  const cp = Number(st.caretPage);
  if (maxWindowPages() - (hi - lo + 1) > 0 && cp >= 0 && cp < total) {
    if (cp === lo - 1) lo = cp;
    else if (cp === hi + 1) hi = cp;
  }
  let first = Math.max(0, lo);
  const need = hi - lo + 1;
  const count = Math.max(1, Math.min(need, total));
  if (first + count > total) first = Math.max(0, total - count);
  return { first, count };
}

/**
 * 换窗的提前量（页）与滚动方向。与单线程宿主的同名常量同一口径 ——
 *   那边解释了为什么余量要偏向滚动方向、以及"贴到窗口边缘才换窗"为什么会闪白
 *   （换窗那一拍 20~45 ms，合成器在这期间继续往前滚，每秒 80 页就是 4~5 页）。
 */
const kWindowLookaheadPages = 2;
let scrollDir = 1;

/**
 * 要挪窗口时挪到哪：把多出来的页放在滚动方向上。与单线程宿主的 targetWindow 同一口径
 *   —— 只开"必须的那几页"会让每滚一页都整窗重光栅（那边注释里有实测数字：留余量之后
 *   一页滚动只要 1 ms，不留则一页 10~30 ms）。
 */
function targetWindow(req, total, cap) {
  const count = Math.max(req.count, Math.min(cap || 3, total));
  const spare = count - req.count;
  const behind = scrollDir < 0 ? Math.max(0, spare - 1) : 1;
  let first = req.first - Math.min(behind, spare);
  if (first + count > total) first = total - count;
  if (first < 0) first = 0;
  return { first, count: Math.max(1, Math.min(count, total)) };
}

function retarget(st) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const req = desiredWindow(st);
  const look = kWindowLookaheadPages;
  // 要求先夹到文档范围内：第 0 页往上、最后一页往下都没有"那一页"可留，
  //   不夹的话 needFirst 会是 -1 —— 那是个永远满足不了的判据（窗口不可能从 -1 页起），
  //   于是滞回恒判"盖不住"，每一拍都白换一次窗。
  const needFirst = Math.max(0, req.first - (scrollDir < 0 ? look : 0));
  const needLast = Math.min(total - 1,
                            req.first + req.count - 1 + (scrollDir > 0 ? look : 0));
  const cur = lastAnchor.split(':').map(Number);
  // 滞回：现有窗口只要盖得住"要求 + 滚动方向上 1 页提前量"就不动。判据与单线程宿主的
  //   applyWindow 逐条相同 —— 特别是不许再加"现有窗口不能超过总页数"那一条：装载时会
  //   先把窗口钉成 (0, 上限) 再排版，而单页文档的总页数是 1 ⇒ 多那一条会把"钉好的窗口"
  //   判成换窗，于是全量重排之外再 present 一次（那次只重画一个光标），首次全量那笔账
  //   被冲掉。判据取"要求"（加提前量）而不是"目标窗口"：目标带余量还随可见页挪，
  //   拿它当判据等于要求窗口永远精确居中，一滚就动、滞回形同虚设。
  if (cur.length === 2 && Number.isFinite(cur[0]) && Number.isFinite(cur[1]) &&
      cur[0] <= needFirst && needLast <= cur[0] + cur[1] - 1) {
    return false;   // 现有窗口还盖得住
  }
  const w = targetWindow(req, total, maxWindowPages());
  const key = w.first + ':' + w.count;
  if (key === lastAnchor) return false;
  mod.setViewportAnchor(w.first, w.count);
  lastAnchor = key;
  return true;
}

/* 呈现收尾 */

/**
 * 引擎自己不会画的那几个入口：改完要主动呈现一次。
 *
 * 为什么必须逐条列出来（这不是风格问题）：present() 会把 edit_stats_ 覆盖成
 *   "这一下重画了多少"。引擎的 *AndRender 系入口（typeText / backspace / moveCaret /
 *   undoEdit ...）在库里就把重排重绘收尾好了，那笔账正是"编辑这一下的代价"——再
 *   present 一次就把它冲掉，只剩一个光标。探针拿这笔账当分母（"整页重画 ≥ 半帧"、
 *   "光标移动 < 全量的 1/100"），产品面板也用；所以多做一次 present 是静默的失真。
 *   对照单线程宿主 app.js：settle() 只在窗口变了才 present，move() 则完全不 present，
 *   而 setCaret / setEditMode / undo ... 这些是自己 return this.present() 的。
 */
const PRESENT_ALWAYS = new Set([
  'present', 'blinkPhase', 'viewport',
  'setCaret', 'setCaretAt', 'setCaretAtSide', 'setFocusAtSide',
  'setCaretAtSideLine', 'setFocusAtSideLine',
  'setEditMode', 'setShowBrackets', 'setShowControlDots', 'setViewportAnchor',
  // 样式那两条：与 setEditMode 同档 —— 引擎只换参数（见 setCaretAndSelectionStyle），
  //   没有这层收尾的话帧上留的还是旧颜色。
  'caretStyle', 'selectionStyle',
  'undo', 'redo', 'indent', 'outdent', 'focusControl', 'tabCaret',
  'removeControl', 'setControlProp',
]);

/**
 * 收尾：必要时呈现一次，然后把脏页重贴上去，返回这一拍的账。
 *
 * 呈现的条件是"引擎没画（见 PRESENT_ALWAYS）或窗口换了" —— 与 app.js 的 settle 同一
 *   口径（窗口没变就不 present）。setFontCacheConfig / setUndoLimit 这类根本不碰帧的
 *   运算，两次条件都不成立，于是连 present 都不做。
 */
// 自己钉锚点的运算：不要再让 retarget 按可见范围改回来（见 WRITE.viewport 的说明）。
const KEEP_ANCHOR = new Set(['viewport']);

/**
 * 把"帧槽数"报给引擎。它等于窗口页数上限，与画布槽数是同一个数（同一个来源）。
 *
 * 引擎侧的帧按"页号 mod 槽数"布局，槽数一变所有页的位置都变 ⇒ 引擎会把帧整体作废、
 *   下一趟整窗重画。所以这里返回"是否变过"，调用方据此强制一次 present ——
 *   否则帧还是旧布局，而贴图已经按新槽号取源行，结果是每一页都贴错位置。
 *
 * 它与画布槽数可以不等，这是有意的：画布槽建了就不回收（已转移的画布不能再转移
 *   第二次，缩回去再涨就得新建加首画），所以那个数是"历史最大"；而帧槽只是内存，
 *   跟着窗口页数上限走就行。blitPage 用两个槽号分别取"源行"与"目标画布"，各自自洽。
 */
let frameSlotsSent = 0;
function syncFrameSlots() {
  if (!mod || typeof mod.setFrameSlots !== 'function') return false;
  const n = maxWindowPages();
  if (n <= 0 || n === frameSlotsSent) return false;
  frameSlotsSent = n;
  mod.setFrameSlots(n);
  return true;
}

function render(opName) {
  let st = JSON.parse(mod.editStatsJson());
  const moved = KEEP_ANCHOR.has(opName) ? false : retarget(st);
  // 槽数变了必须重画：帧的布局整体换了（引擎那边会把帧作废），不 present 就会拿
  //   新槽号去取旧布局的源行 —— 每一页都贴错位置。
  const slotsChanged = syncFrameSlots();
  if (moved || slotsChanged || PRESENT_ALWAYS.has(opName)) {
    mod.present();
    st = JSON.parse(mod.editStatsJson());
  }
  // 窗口换了要整窗重贴；没换就只贴脏页 —— 这正是"一页一张画布"省下来的那部分。
  // 窗口换了：blitPages 自己会看槽过期（新进窗口的页）与还欠着的脏矩形，
  //   这里不再替它挑页 —— 那两件事的判据在引擎与槽表里，宿主重算一遍只会不同步。
  //   内容整体作废（换缩放/换页度量/换文档）不在这里判 —— paintFull 已经把槽表全置空，
  //   于是这一趟无论传什么都会整窗重贴。
  host.blitPages();
  return st;
}

/**
 * 回话的打包成本（构造那份状态对象 + 结构化克隆），随每一份状态带回宿主。
 *
 * 它是"往返里没算进 work 的那一段"里最容易被忽略的一块：work 只从消息进来到
 *   present/blit 结束为止，而一份状态里有整段文本、控件、选区、光标报告、排版账，
 *   构造它们与克隆它们都在这之后。收与发都在 Worker 这边，所以量在这里最准。
 */
let packSum = 0, packMax = 0, packN = 0;
const packAvg = () => (packN ? packSum / packN : 0);

function sendState(ts, label, stats, changed, workMs) {
  const tPack = performance.now();
  const blit = host.blitStats();
  post({
    t: 'state', ts: ts || 0, label: label || '',
    stats, geom: geom(stats),
    scrollReset,
    // 这一下在 Worker 里花了多少毫秒。只有视口那条路会填；0 = 与视口无关的一份状态
    //   （编辑、装载、画布到达）。宿主拿它和往返比（见 host.viewDiag）。
    workMs: workMs || 0,
    // 打包那一段（构造 + 克隆）的近期均值与峰值。宿主只读不判 —— 它只为把
    //   "往返减 work"那段缺口拆成两半，好知道时间花在打包上还是花在路上。
    packAvgMs: packAvg(), packMaxMs: packMax,
    // 这一拍重贴了几页画布、传了多少像素、花了多少毫秒。挪窗时贴的是全窗，而真正需要
    //   新像素的只有新露出来那一页 —— 其余页的画布在文档流里位置没动、像素本来就是对的。
    //   px 是"实际 putImageData 的面积"：按脏矩形贴之后它应该比 pages × 页面积小两三个
    //   数量级（光标移动一次几十像素），这是"上屏没有整页搬"的唯一直接证据。
    blitMs: blit.ms, blitPages: blit.pages, blitPx: blit.px,
    // 这一下到底有没有真的改动。宿主门面的契约是"没改 => 返回 null"（对照单线程宿主
    //   app.js 里 if (!mod.X()) return null; return this.present();），
    //   引擎那一步的返回值就是它，必须一路带到主线程。
    changed: !!changed,
    heapMB: mod.HEAPU8.length / 1048576,
    fontMode,
    // 顺带把主线程要"同步读"的那几样一起推过去 —— 它们都很小，但每拍去 ask 一次
    //   就是一次往返（200 µs）。推一次让宿主侧的门面能保持同步读的形态。
    text0: mod.documentText(0),
    controls: JSON.parse(mod.controlsJson()),
    blockedEdits: mod.blockedEdits(),
    dirtyPages: JSON.parse(mod.dirtyPagesJson()),
    caretReport: mod.caretReport(),
    // 选区取证（两端全局流位置 + 页面坐标矩形）。与 caretReport 同档：小字符串，
    //   随 state 推过去，主线程的 host.selection 于是仍是同步读。
    selection: JSON.parse(mod.selectionReport()),
    // 引擎实时光标偏移（直读，不经过 editStats 那本账）。宿主拿它和 editStats 里的
    //   光标位置比，就能判出"有没有哪个改了状态的入口忘了给自己收尾"。
    caretLive: mod.caretOffsetLive(),
    // 排版那本账（statsJson）与编辑那本账（editStatsJson）在单线程宿主上是两个入口，
    //   上面那个 stats 走的是编辑那本（宿主的内部逻辑要它：光标矩形、脏页、重画像素）。
    //   这里必须把排版那本也推过去 —— 否则主线程的 stats 会与 editStats 指向同一个对象，
    //   页数 / 字形 run 数 / 分阶段形状耗时这些字段读出来全是 undefined。
    //   undefined 不会让任何断言变红，只会让报告里少几列：那是查错方向都定不下来的那种错。
    layout: JSON.parse(mod.statsJson()),
    // 样式现状。与 selection 同档：小字符串，随 state 推过去，主线程读它是同步的 ——
    //   面板初始化控件要用它，而默认值只在 EngineConfig 里有一份。
    caretStyle: JSON.parse(mod.caretStyleJson()),
    selectionStyle: JSON.parse(mod.selectionStyleJson()),
  });
  const pack = performance.now() - tPack;
  packSum += pack;
  ++packN;
  if (pack > packMax) packMax = pack;
  scrollReset = false;   // 一次性信号：随这一份状态送出去就清掉
}

function push(ts, opName, changed) { sendState(ts, opName, render(opName), changed); }

/* 装载 */

/**
 * 全量重排重绘的公共收尾。
 *
 * present 只画不排 —— 装载之后排版一次都没跑过时，present 出来是一片空白（首帧 ink 为 0、
 * totalPages 恒为 1；编辑走 *AndRender 版本会自己收尾，所以只有首帧露）。
 *
 * 锚点必须先钉住、再排 —— 与单线程宿主的 presetWindowForLoad 同一口径。锚点也不能事后留空：
 * 留空会让随后的 retarget 把"窗口从无到有"判成换窗，于是在全量之外再 present 一次 ——
 * 后一次只重画一个光标，首次全量那笔账（探针的分母）就被冲掉了。
 */
function paintFull() {
  // 换文档、换缩放、换页度量、重设窗口锚点：页像素尺寸或页面内容整体变了，
  //   每个槽里装的都可能是上一份的像素，所以槽表整体作废、这一趟整窗重贴。
  //
  // 这一条必须显式：paintFull 自己先把窗口锚点设好了，随后 render() 里的 retarget
  //   会判"窗口没动"。按页号建销的那一版里，这一趟总有一批新画布陆续到达、各自把
  //   那一页贴一次，于是"整窗都换过页"被顺带补齐了；改成按槽复用之后画布不再新建，
  //   那条隐式触发就没了 —— 表现为切字体之后画布还留着上一窗的像素，而帧是对的
  //   （自审逐像素比对一比就露）。所以这件事必须由上面说出来，不能指望下游有谁代劳。
  host.invalidateSlots();
  scrollY = 0;
  // 告诉宿主"引擎把滚动位置归零了"。必须是显式信号，不能让宿主那边
  //   用"两边不一致就追平"来猜：追平要写 DOM 的 scrollTop，而写它会触发 scroll
  //   事件、再发一条视口消息回来 —— 与正在滚动的用户（或外部脚本）来回打架，
  //   窗口跟着反复变、画布反复建与丢。实测症状：跨页滚动后某一页始终没有画布。
  scrollReset = true;
  // 槽数要在重排重画之前报过去：帧的布局由它决定，报晚了这一趟就按旧槽号画。
  syncFrameSlots();
  const n = maxWindowPages();
  mod.setViewportAnchor(0, n);
  lastAnchor = '0:' + n;
  mod.relayoutAndPaint();
}

function loadScenario(id, zoom) {
  if (zoom) { mod.setZoom(zoom); zoomLevel = zoom; }
  mod.setScenario(id);
  paintFull();
}

/**
 * 换块流之后的统一收尾（与 loadScenario 同理，只是入口在别处）。
 *
 * 锚点必须先钉住、再排 —— 与单线程宿主的 presetWindowForLoad 同一口径。
 *   这里若图省事写 lastAnchor = ''，retarget 会把"窗口从无到有"判成换窗，
 *   于是在 relayoutAndPaint 的全量之外再 present 一次 —— 而那次只重画一个光标，
 *   首次全量那笔账（探针的分母）就被冲掉了。
 */
function loadBlocks(fn) {
  fn();
  paintFull();
}

/**
 * 胶水与 wasm 这两个资源的传输字节数（口径与单线程宿主的 wasmBytes() 逐条相同）。
 *
 * 必须在 Worker 这一侧量：胶水（importScripts）与 wasm（胶水自己去取）都是这一侧发出的，
 *   主线程的 performance 资源表里没有它们 —— 在那边量恒为 0，而 0 会安安静静地进报告，
 *   没人会怀疑一个数字。
 */
function glueBytesInWorker() {
  let n = 0;
  for (const e of performance.getEntriesByType('resource')) {
    if (e.name.includes('heditor')) n += e.transferSize || e.decodedBodySize || 0;
  }
  return n;
}

async function boot(m) {
  const t0 = performance.now();
  // 胶水怎么加载由宿主决定：Worker 侧是 importScripts，主线程侧是 <script>。
  //   这条差异属于"传输"，不属于引擎 —— 放在这里会让同一份代码没法跑在主线程。
  const factory = await host.loadGlue(m.glue, m.exportName || 'CreateHEditor');
  if (typeof factory !== 'function') {
    throw new Error('胶水层没导出 ' + (m.exportName || 'CreateHEditor'));
  }
  const base = m.glue.replace(/[^/]*$/, '');
  // bust 一路带到 wasm：只给胶水加戳、不给 wasm 加，就会出现"新胶水配旧 wasm"。
  mod = await factory({ locateFile: (p) => base + p + (m.bust || '') });
  dpr = m.dpr || 1;
  viewW = m.viewW || 1;
  viewH = m.viewH || 1;
  fontList = m.fonts || [];
  await installFonts(m.fontMode || 'full');
  // 先把 ready 报出去，后面的装载失败也看得见 —— 顺序反了的话，装载里一抛异常
  //   主线程就永远等不到 ready，表现成"整个页面静默挂住"，是最难查的那种形态。
  bootMs = performance.now() - t0;
  post({
    t: 'ready', bootMs, fontMs, fontBytes, fontMode,
    glueBytes: glueBytesInWorker(),
    heapMB: mod.HEAPU8.length / 1048576,
    zeroCopyFrame: mod.frameRgba().buffer === mod.HEAPU8.buffer,
  });
  try {
    // 页度量要先设再装载：它会让整篇重排，排在装载之前只会排这一次。
    mod.setPageMetrics(kPageWidthPt, kPageHeightPt, kPageMarginPt);
    loadScenario(m.scenario === undefined ? 6 : m.scenario, m.zoom);
    // 首帧的几何先报出去，宿主才能按窗口把每页画布建出来并交给这一侧。
    //   这时候还没有画布，所以只推几何；画布到一张贴一张（见 pageCanvas 分支）。
    sendState(0, 'boot', JSON.parse(mod.editStatsJson()));
  } catch (e) {
    err(e, 'boot:layout');
  }
}

/* 运算表 */

// 改状态的运算。跑完一律收尾（present + 重贴 + 回推 state）。
const WRITE = {
  // 什么都不改，只收尾。给宿主的 present() 用 —— 它就是"重画并回推"。
  present: () => {},
  scenario: (a) => loadScenario(a.id, a.zoom),
  reset: (a) => loadScenario(a.id === undefined ? 6 : a.id, a.zoom === undefined ? kZoomBase : a.zoom),
  zoom: (a) => {
    mod.setZoom(a.zoom);
    zoomLevel = a.zoom;
    // 屏幕像素比会随窗口换显示器而变，宿主每次改缩放都捎一份新的过来。回推的状态里带着
    //   它，宿主拿它折算页画布的 CSS 尺寸 —— 不回传就会按旧值折算，显示尺寸与背衬差一档。
    if (a.dpr) dpr = a.dpr;
    paintFull();
  },
  stress: (a) => loadBlocks(() => mod.loadStressDocument(a.paras | 0, a.chars | 0)),
  stressDiverse: (a) => loadBlocks(() => mod.loadStressDocumentDiverse(a.paras | 0, a.chars | 0)),
  textDoc: (a) => loadBlocks(() => mod.loadTextDocument(a.text, (a.repeat | 0) || 1)),
  formSample: (a) => loadBlocks(() => mod.loadFormSample(a.variant | 0)),
  fonts: async (a) => { await installFonts(a.mode || 'full'); paintFull(); },

  type: (a) => mod.typeText(a.s),
  backspace: () => mod.backspace(),
  deleteForward: () => mod.deleteForward(),
  splitParagraph: () => mod.splitParagraph(),
  mergePrev: () => mod.mergeWithPreviousParagraph(),
  mergeNext: () => mod.mergeWithNextParagraph(),
  move: (a) => mod.moveCaret(a.dcp | 0, !!a.extend),
  /**
   * 上下键（dir < 0 上 / > 0 下）与文档首尾。extend = Shift 扩展（锚点不动）。
   *   与 move 同一档：引擎的 *AndRender 版自己收尾，这里不额外 present。
   */
  vertical: (a) => mod.moveCaretVertical(a.dir | 0, !!a.extend),
  docStart: (a) => mod.caretToDocStart(!!a.extend),
  docEnd: (a) => mod.caretToDocEnd(!!a.extend),
  /**
   * 选区三个入口。两端是全局流位置（与事务里那份快照同一把尺子），不是段内偏移。
   */
  selectAll: () => mod.selectAll(),
  // 整格选区：段号来自 hit 的 cellPara 字段（引擎的几何结论），宿主原样搬回来。
  selectCellAtPara: (a) => mod.selectCellAtPara(a.para | 0),
  // 三击：按点选整格。点不在表格里返回 false，宿主退回落光标。
  //   必须留在这里（命令表）：它会重画帧，而查询那条路不重贴脏页 —— 画面要等下一次
  //   闪烁相位才更新，那就是"点了要等一秒才选中"。
  cellSelectAt: (a) => mod.cellSelectAt(a.x, a.y),
  setSelection: (a) => mod.setSelection(a.anchor | 0, a.focus | 0),
  clearSelection: () => mod.clearSelection(),
  // 落光标的三个入口都要无条件把相位拉回可见，与单线程宿主的 setCaretAndPresent
  //   逐字对齐（那边就是 mod.setCaret(...); caretOn = true; mod.showCaret(true);
  //   mod.present();）。漏掉的症状：相位停在隐藏那半拍时落一次光标，光标不出现 ——
  //   而这正是"点画布换字段后光标看不见"那个用户 bug 的一半。
  setCaret: (a) => { mod.setCaret(a.para | 0, a.offset | 0); mod.showCaret(true); },
  setCaretAt: (a) => {
    mod.setCaretAt(a.para | 0, a.offset | 0, a.field | 0);
    mod.showCaret(true);
  },
  setCaretAtSide: (a) => {
    mod.setCaretAtSide(a.para | 0, a.offset | 0, a.side | 0);
    mod.showCaret(true);
  },
  // 带行别的版本：折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
  //   只有行别分得开（点击落点走它，见 EditorEngine::caretLineAffinity）。
  setCaretAtSideLine: (a) => {
    mod.setCaretAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
    mod.showCaret(true);
  },
  setFocusAtSideLine: (a) => {
    mod.setFocusAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
    mod.showCaret(true);
  },
  // 拖选的移动端：只搬焦点端，anchor 不动。与 setCaretAtSide 同档 ——
  //   引擎不自己重绘，所以它也在 PRESENT_ALWAYS 里（否则拖动时选区不更新）。
  setFocusAtSide: (a) => {
    mod.setFocusAtSide(a.para | 0, a.offset | 0, a.side | 0);
    mod.showCaret(true);
  },
  indent: () => mod.indent(),
  outdent: () => mod.outdent(),
  tabCaret: (a) => mod.tabCaret(!!a.backward),
  lineStart: (a) => mod.caretToLineStart(!!a.extend),
  lineEnd: (a) => mod.caretToLineEnd(!!a.extend),
  undo: () => mod.undoEdit(),
  redo: () => mod.redoEdit(),
  setUndoLimit: (a) => mod.setUndoLimit(a.n | 0),

  setEditMode: (a) => mod.setEditMode(a.mode | 0),
  setShowBrackets: (a) => mod.setShowBrackets(!!a.on),
  setShowControlDots: (a) => mod.setShowControlDots(!!a.on),
  /**
   * 光标与选区的样式。缺的字段按现状补 —— 与单线程宿主门面同一套补全规则，
   *   两处不能各写一套（面板只改颜色时，宽度必须原样保留）。
   */
  caretStyle: (a) => {
    const s = a.style || {};
    const cur = JSON.parse(mod.caretStyleJson());
    const c = s.color || cur.color;
    mod.setCaretStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0,
                      s.widthPt === undefined ? cur.widthPt : Number(s.widthPt));
  },
  selectionStyle: (a) => {
    const s = a.style || {};
    const cur = JSON.parse(mod.selectionStyleJson());
    const c = s.color || cur.color;
    mod.setSelectionStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === undefined ? 255 : c[3]) | 0);
  },
  /**
   * 换字段名单。必须重排，不能只 present —— 括号标记（含只读的灰括号）是排进
   *   布局结果里的，只 present 的话标记还是上一轮的：症状是"控件清单里 ro=true，
   *   可槽清单里 ro 全是 false"（一条自相矛盾的读数，很难往"没重排"上想）。
   *   对照单线程宿主的 setFormControls：它就是 return relayoutAndBlit();。
   */
  setFormControls: (a) => {
    mod.setFormControls((a.list || []).map((it) => ({
      start: it.start, end: it.end, kind: it.kind || 0, id: it.id || '', ro: !!it.ro,
    })));
    paintFull();
  },
  /**
   * 闪烁那一拍：摆相位并呈现（对照单线程宿主 tick 里的 showCaret + present）。
   *   它必须是"会收尾"的那一类 —— 闪烁要在屏幕上看得见，就得重画。
   *   而门面的 showCaret / stopBlink 走的是 SILENT 那张表（只改状态不呈现），
   *   两者不能混：混了就会把探针的"全量基线"（分母）覆盖掉。
   */
  blinkPhase: (a) => mod.showCaret(!!a.on),
  focusControl: (a) => mod.focusControl(a.forward === undefined ? true : !!a.forward),
  removeControl: (a) => mod.removeControlAt(a.i | 0, !!a.keepContent),
  setControlProp: (a) => mod.setControlPropAt(a.i | 0, a.prop | 0, !!a.value),
  setFontCacheConfig: (a) => mod.setFontCacheConfig(a.blockSize | 0, a.blocks | 0),

  /**
   * 视口锚点：与单线程宿主的门面 setViewportAnchor 逐字对齐 —— 直接钉锚点（不碰滚动
   *   位置），并且必须呈现。
   * 也不能让它走 retarget：retarget 会按"滚动位置算出来的可见页"把锚点又挪回可见范围。
   *   探针靠这个入口把窗口挪到前/中/后去量按需读的覆盖面 —— 被挪回去就会量到别的页，
   *   而且是静默的（数字看着正常，只是不是那一页）。
   */
  viewport: (a) => {
    mod.setViewportAnchor(a.first | 0, a.count | 0);
    lastAnchor = (a.first | 0) + ':' + (a.count | 0);
  },
};

/**
 * 只改引擎状态、不做收尾的运算（不回推 state、不重画）。
 *
 * 为什么需要这一档：单线程宿主的 stopBlink() 与门面 showCaret() 都只调 mod.showCaret，
 *   不 present —— 那边注释写得很清楚："present 会把 edit_stats_ 覆盖成'只重画了个
 *   光标'，而探针紧接着要读的往往是'上一次全量呈现'的数字（分母）。停表就只停表。"
 *   搬进 Worker 之后若把这两个入口也走统一收尾，基线就被无声地换掉了：一堆增量断言会以
 *   错误的分母照绿（这是最难发现的一类失真）。
 * ⇒ 门面 showCaret / stopBlink 走这张表；闪烁那一拍走 WRITE.blinkPhase（那个要呈现）。
 */
const SILENT = {
  phase: (a) => mod.showCaret(!!a.on),
};

// 要返回值的查询。
const READ = {
  /**
   * 排空屏障：什么都不算，立刻回一个值。
   *
   * 用途是给宿主一个"顺序保证"—— postMessage 保序，所以这条 reply 回来时，它之前
   *   发出的消息都已经处理完了。这是探针的同步点（交互之后要读到那一拍的结果）。
   * 它刻意不碰 mod、也不走 push：哨兵不能有副作用（一次 present 就会改变帧，
   *   把"某个入口忘了收尾"这类断言直接盖掉）。
   */
  ping: () => 1,
  /**
   * 一页的脏矩形（页内像素坐标），读后清零。
   *
   * 5B 的按页接口：宿主按它决定 putImageData 哪几块。光标移动只应该给出两三个小矩形，
   *   而不是整页 —— 那一条的判据在 edit_probe / edit_test [58]。
   */
  pageDirtyRects: (a) => JSON.parse(mod.pageDirtyRectsJson(a.page | 0)),
  /**
   * 还欠着脏矩形的页号。宿主按它决定贴哪几页 —— 不能用 dirtyPages：
   *   那一份是"本拍写过哪几页"，而矩形是累积的，两者不同步就会永久漏贴几块。
   */
  pageDirtyPages: () => JSON.parse(mod.pageDirtyPagesJson()),
  text: (a) => mod.documentText(a.i | 0),
  /**
   * 整帧里"非白"的像素数。主线程读不到画布（所有权在 Worker 这边），所以"帧上到底
   *   有没有字"这个判据只能在这里数 —— 与单线程版探针的口径一致（任一通道 < 200 算一个）。
   */
  ink: () => {
    const v = mod.frameRgba();
    let n = 0;
    for (let i = 0; i + 3 < v.length; i += 4) {
      if (v[i] < 200 || v[i + 1] < 200 || v[i + 2] < 200) ++n;
    }
    return n;
  },
  /**
   * 逐行的墨迹与底色，按文档顺序给出窗口内每一条带的那一段。
   *
   * 判据用它回答两个问题，都是"连页到底连不连"的直接读数：
   *   一、条带里有没有"不是白底"的行 —— 有就是还有页间隙或页边框（分页的痕迹）。
   *   二、相邻条带的文字行距是不是与条带内部一样 —— 不一样就是内容在边界上断开了。
   * 为什么在 Worker 侧数：帧的所有权在这一侧（主线程拿不到画布）。这是测量，
   *   走的是与绘制不同的代码路径（只读帧、不做任何布局判断），不会自我循环验证。
   */
  inkRows: () => {
    const f = mod.frameRgba();
    const fw = mod.frameWidth();
    const ph = mod.pagePxHeight();
    const step = ph + mod.pageGapPx();
    const fs = (mod.frameSlots ? mod.frameSlots() : 0) | 0;
    const first = mod.windowFirstPage();
    const count = mod.windowPageCount();
    const out = [];
    for (let p = first; p < first + count; ++p) {
      const slot = fs > 0 ? ((p % fs) + fs) % fs : p - first;
      const base = slot * step;
      const ink = [];
      const sel = [];
      const selDeep = [];
      let gray = 0;
      for (let y = 0; y < step; ++y) {
        const off = (base + y) * fw * 4;
        if (off + fw * 4 > f.length) { ink.push(-1); sel.push(-1); selDeep.push(-1); continue; }
        // 左边那一列（x = 1）是不是白：不是就是页间隙或页边框。
        const l = off + 4;
        if (f[l] < 250 || f[l + 1] < 250 || f[l + 2] < 250) ++gray;
        // 暗像素 = 字；偏蓝的浅色 = 选区（默认选区色混白之后约 228/241/255，
        //   而背景灰 238/240/243 的蓝红差只有 5，页边框 206/212/218 是 12 —— 取 15 分得开）。
        let n = 0;
        let s = 0;
        let deep = 0;
        for (let x = 0; x < fw; x += 2) {
          const i = off + x * 4;
          if (f[i] < 128 && f[i + 1] < 128 && f[i + 2] < 128) ++n;
          else if (f[i + 2] - f[i] > 15 && f[i] > 150) {
            ++s;
            // "深"的选区像素：混一次应当是 228/241/255（红 228），叠加多次会往 178 走。
            //   这一格是"选区被反复混色"的直接读数（见 engine_paint 里 sel_paint 的说明）。
            if (f[i] < 205) ++deep;
          }
        }
        ink.push(n);
        sel.push(s);
        selDeep.push(deep);
      }
      out.push({ page: p, step, ink, sel, selDeep, gray });
    }
    return { first, count, step, ph, slabs: out };
  },
  /**
   * 某几段的行盒与字形（诊断"字画到哪儿去了"）。见 wasm 侧 linesJson 的说明。
   */
  lines: (a) => JSON.parse(mod.linesJson(a.from | 0, a.to | 0)),
  /**
   * 帧的指纹（FNV-1a，只算 RGB 三通道）。
   *
   * 判据用它回答"同一份逻辑状态重画一次，像素是不是一个字都没变" —— 增量绘制最容易犯的
   *   错是"重画时把叠加式的效果又叠了一遍"（选区混色就是这么变深的）。整帧搬回去比太贵
   *   （8 MB 一份），指纹在这一侧算完只回一个数。
   */
  frameDigest: () => {
    const v = mod.frameRgba();
    let h = 2166136261;
    for (let i = 0; i + 3 < v.length; i += 4) {
      h ^= v[i]; h = Math.imul(h, 16777619);
      h ^= v[i + 1]; h = Math.imul(h, 16777619);
      h ^= v[i + 2]; h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  },
  // 帧与页的几何读数：宿主排版、滚动条与自检都要用。都是标量，跨传输安全。
  frameWidth: () => mod.frameWidth(),
  frameHeight: () => mod.frameHeight(),
  frameByteSize: () => mod.frameByteSize(),
  frameSlots: () => (mod.frameSlots ? mod.frameSlots() : 0),
  pagePxWidth: () => mod.pagePxWidth(),
  pagePxHeight: () => mod.pagePxHeight(),
  pageGapPx: () => mod.pageGapPx(),
  windowFirstPage: () => mod.windowFirstPage(),
  windowPageCount: () => mod.windowPageCount(),

  // 光标与选区的即时读数。它们不经过 edit_stats 那本账，拿它当分母会读错。
  caretOffsetLive: () => mod.caretOffsetLive(),
  caretVerticalNow: () => mod.caretVerticalNow(),
  caretBlockReversedNow: () => mod.caretBlockReversedNow(),
  selectionReport: () => JSON.parse(mod.selectionReport()),
  // 样式现状：写入口是 caretStyle / selectionStyle（缺字段按现状补），读是这两条。
  caretStyleJson: () => JSON.parse(mod.caretStyleJson()),
  selectionStyleJson: () => JSON.parse(mod.selectionStyleJson()),

  // 构建与后端标识：诊断面板与探针报告要用。
  buildStamp: () => (typeof mod.buildStamp === 'function' ? mod.buildStamp() : ''),
  rasterBackend: () => (mod.rasterBackend ? mod.rasterBackend() : '?'),

  stats: () => JSON.parse(mod.statsJson()),
  editStats: () => JSON.parse(mod.editStatsJson()),
  caretReport: () => mod.caretReport(),
  hit: (a) => JSON.parse(mod.hitTest(a.x, a.y)),
  controlAt: (a) => mod.controlAtPoint(a.x, a.y),
  describeControls: () => mod.describeControls(),
  controls: () => JSON.parse(mod.controlsJson()),
  bracketSlots: () => JSON.parse(mod.bracketSlotsJson()).slots,
  blockedEdits: () => mod.blockedEdits(),
  prewarmPage: (a) => mod.prewarmPageGlyphs(a.page | 0),
  dirtyPages: () => JSON.parse(mod.dirtyPagesJson()),
  editMode: () => mod.editMode(),
  showBrackets: () => mod.showBrackets(),
  showControlDots: () => mod.showControlDots(),
  undoLimit: () => mod.undoLimit(),
  auditFrame: () => JSON.parse(mod.auditFrame()),
  auditBase: () => JSON.parse(mod.auditBase()),
  // 页自审：第 p 页的缓存 vs 从零光栅。帧与底图两条自审读的都是缓存，
  //   缓存本身写错时它们照样全绿 —— 这一条是那一层的唯一参照物。
  auditPage: (a) => JSON.parse(mod.auditPage(a.page | 0)),
  auditLayout: () => (mod.auditLayout ? JSON.parse(mod.auditLayout()) : null),
  auditCompile: () => (mod.auditCompile ? JSON.parse(mod.auditCompile()) : null),
  auditGlyphPages: () => (mod.auditGlyphPages ? JSON.parse(mod.auditGlyphPages()) : null),
  bench: (a) => JSON.parse(mod.benchRelayoutPaint(a.runs || 12)),
  heap: () => mod.HEAPU8.length / 1048576,

  /**
   * 自检：把每页画布上真正的像素与帧里对应的一段逐点比。
   *
   * 返回形状必须与单线程宿主的 selfCheck 一致 —— 它是给用户的取证入口
   *   （F12 里 __heditor.selfCheck() 把对象发出来），也是探针的主判据：
   *   screenVsEngineDiffPx > 0 说明"上屏"贴错了，恒为 0 说明问题在引擎的排版与光栅化。
   *   形状不一致的代价不是"少几个字段"：探针读 sc.framePx[0] 会直接抛，
   *   整段断言连跑都跑不到（表现为一个 TypeError，看不出是这个原因）。
   * 多出来的 compared / ratio / missing / sizeBad 是本侧扩展：缺页与尺寸不符单独报数 ——
   *   "有页还没拿到画布"和"画错了"是两件事，混在一起会让人查错方向。
   */
  selfCheck: () => {
    // 先把欠着的上屏贴完，再比。否则比的是"帧已经前进了一拍、画布还停在上一拍"，
    //   而那不是贴图逻辑的错 —— 是两次操作之间的时序（闪烁拍 present 之后、blit 之前）。
    //   诊断接口要回答的是"贴得对不对"，不是"凑不凑巧"。
    host.blitPages();
    const fw = mod.frameWidth();
    const fh = mod.frameHeight();
    const ph = mod.pagePxHeight();
    const step = ph + mod.pageGapPx();
    const first = mod.windowFirstPage();
    const count = mod.windowPageCount();
    const fv = mod.frameRgba();
    const continuous = host.isContinuous();
    const viewCtx = host.viewCtx();
    let diff = 0;
    let compared = 0;
    let missing = 0;
    let sizeBad = 0;
    let comparedPages = 0;
    let minx = 1e9;
    let miny = 1e9;
    let maxx = -1;
    let maxy = -1;
    // 连页模式没有页画布，屏上那一份是视口画布：逐字节比的是"视口画布 vs 帧"。
    //   帧仍是源（引擎的产物），视口画布是它按文档坐标贴出来的那一份 —— 目标 y 算错、
    //   或者压根没贴上（画布没转移成功），这里都会报出来。
    //   这一支不是可有可无的：没有它，连页下每一页都会被记成"缺画布"，diff 恒为 0 ——
    //   看着全绿，实际一个字节都没比过。
    if (continuous) {
      if (!viewCtx) {
        missing = count;
      } else if (viewCtx.canvas.width !== fw || viewCtx.canvas.height !== viewH) {
        sizeBad = 1;
      } else {
        const V = viewCtx.getImageData(0, 0, fw, viewH).data;
        const fs = (mod.frameSlots ? mod.frameSlots() : 0) | 0;
        for (let p = first; p < first + count; ++p) {
          // 页在视口里的落点：目标 y = 页号 x 步距 - 滚动量（与 blitPageContinuous 同式）。
          const dy = p * step - scrollY;
          const y0 = Math.max(0, -dy);
          const y1 = Math.min(ph, viewH - dy);
          if (y1 <= y0) continue;                 // 这一页整页在视口外，没得比
          ++comparedPages;
          const srcY = (fs > 0 ? ((p % fs) + fs) % fs : p - first) * step;
          for (let y = y0; y < y1; ++y) {
            const j0 = (srcY + y) * fw * 4;
            const k0 = (dy + y) * fw * 4;
            for (let x = 0; x < fw; ++x) {
              const j = j0 + x * 4;
              const k = k0 + x * 4;
              ++compared;
              if (V[k] !== fv[j] || V[k + 1] !== fv[j + 1] || V[k + 2] !== fv[j + 2]) {
                ++diff;
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                const yy = dy + y;
                if (yy < miny) miny = yy;
                if (yy > maxy) maxy = yy;
              }
            }
          }
        }
      }
    } else {
      for (let p = first; p < first + count; ++p) {
        const c = host.ctxOfPage(p);
        if (!c) { ++missing; continue; }
        if (c.canvas.width !== fw || c.canvas.height !== ph) { ++sizeBad; continue; }
        ++comparedPages;
        const D = c.getImageData(0, 0, fw, ph).data;
        // 帧里的偏移按帧槽号算（引擎的槽数），不是"窗口内第几页"。
        //   这两套槽号可以不等（画布槽只增、帧槽跟着窗口页数上限走），所以这里必须
        //   与 blitPage 用同一个来源 —— 用画布槽号会把整段比错位置（实测差 17%）。
        const fs = (mod.frameSlots ? mod.frameSlots() : 0) | 0;
        const local = (fs > 0 ? ((p % fs) + fs) % fs : p - first);
        const base = local * step * fw * 4;
        for (let i = 0; i + 3 < D.length; i += 4) {
          const j = base + i;
          if (j + 3 >= fv.length) break;
          if (D[i] !== fv[j] || D[i + 1] !== fv[j + 1] || D[i + 2] !== fv[j + 2]) {
            ++diff;
            const q = i >> 2, x = q % fw, y = (q - x) / fw + local * step;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
          }
        }
        compared += D.length / 4;
      }
    }
    const st = JSON.parse(mod.editStatsJson());
    return {
      text: mod.documentText(0),
      docCodepoints: st.docCodepoints,
      caret: { para: st.caretPara, offset: st.caretOffset, x: st.caretX, y: st.caretY,
               w: st.caretW, h: st.caretH, visible: st.caretVisible, page: st.caretPage },
      framePx: [fw, fh],
      window: { first, count, comparedPages },
      pageCanvases: host.canvasCount(),
      // cssSize / rectPx 要看主线程上的 DOM，由宿主那边补（见 worker_host.js 的 selfCheck 包装）。
      pages: { backing: [mod.pagePxWidth(), ph], cssSize: null, rectPx: null },
      dpr,
      zoom: zoomLevel,
      backend: mod.rasterBackend ? mod.rasterBackend() : '?',
      screenVsEngineDiffPx: diff,
      screenVsEngineDiffBox: diff ? [minx, miny, maxx, maxy] : null,
      repainted: { pages: st.repaintedPages, pixels: st.repaintedPixels,
                   w: st.repaintedW, h: st.repaintedH },
      diff, compared, missing, sizeBad, ratio: compared ? diff / compared : 1,
    };
  },

  /**
   * 每个可编辑字段的左括号在帧上到底画了没有（扫左缘那一列里的"括号蓝"）。
   *   判据与探针一致：偏蓝够远 + 够亮 + 红通道 > 45（把光标色排除掉）。
   *   括号常显之后每个可编辑字段都必须有；"只画聚焦字段"那一档已经删了。
   */
  markCheck: () => {
    const cc = JSON.parse(mod.controlsJson());
    const f = mod.frameRgba();
    const fw = mod.frameWidth();
    const near = (r, g, b) => r > 45 && b - r > 60 && b > 140;
    const rows = [];
    for (let ci = 0; ci < cc.items.length; ++ci) {
      const it = cc.items[ci];
      if (it.ro || !it.rects.length) continue;
      const cx = it.rects[0].x;
      const lo = Math.round((cx - 10) * zoomLevel);
      const hi = Math.round((cx - 1) * zoomLevel);
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const x = (i >> 2) % fw;
        if (x < lo || x > hi) continue;
        if (near(f[i], f[i + 1], f[i + 2])) ++n;
      }
      rows.push({ control: ci, focused: ci === cc.caret, leftEdgePx: n, ok: n > 0 });
    }
    return {
      mode: cc.mode, showBrackets: mod.showBrackets(), count: cc.count,
      caretControl: cc.caret, zoom: zoomLevel, rows,
      allOk: rows.every((r) => r.ok),
    };
  },
};

/**
 * 每轮换窗之后往前预热一页的字形，且要等 90 ms 没有新的视口消息才动手。
 *
 * 为什么要预热：换窗那一拍要重新光栅新露出来的那几页，而冷字形占那一拍的一大半 ——
 *   同一条路上同一页量到：冷的 14.6 µs·字形、整拍 44.5 ms；预热带过之后 7.0 µs、25.7 ms。
 * 为什么要限量成"一页 + 静下来 90 ms 才跑"：预热是 Worker 这条唯一线程上的活，
 *   成串地跑会把视口消息、编辑命令与宿主的查询全排在后面 —— 实测探针的求值直接超时。
 *   连续快滚时它几乎不跑（那时每一拍都该留给滚动），滚动一停就把下一页先烘上。
 */
const PREFETCH_PAGES = 1;
const PREFETCH_DELAY_MS = 90;
let pfTimer = 0;
let pfNext = -1;
let pfLeft = 0;
let pfDir = 1;

function schedulePrewarm(dir) {
  if (!mod) return;
  pfDir = dir < 0 ? -1 : 1;
  pfNext = pfDir > 0 ? mod.windowFirstPage() + mod.windowPageCount() : mod.windowFirstPage() - 1;
  pfLeft = PREFETCH_PAGES;
  if (pfTimer) clearTimeout(pfTimer);
  pfTimer = setTimeout(pumpPrewarm, PREFETCH_DELAY_MS);
}

function pumpPrewarm() {
  pfTimer = 0;
  if (!mod || pfLeft <= 0) return;
  const total = Math.max(1, JSON.parse(mod.editStatsJson()).totalPages);
  if (pfNext < 0 || pfNext >= total) return;
  mod.prewarmPageGlyphs(pfNext);
  pfNext += pfDir;
  --pfLeft;
}

/* 消息入口 */

async function doOp(m) {
  // 静默档先查：它只改引擎状态，什么都不回推（对照单线程宿主的 stopBlink/showCaret）。
  const silent = SILENT[m.name];
  if (silent) {
    await silent(m.args || {});
    return void post({ t: 'ack', ts: m.ts });
  }
  const fn = WRITE[m.name];
  if (!fn) throw new Error('未知命令 ' + m.name);
  const ret = await fn(m.args || {});
  return void push(m.ts, m.name, ret);
}

async function askOp(m) {
  // rawFrame 要单独走：帧是 8 MB 量级，按值结构化克隆会白拷一份；拷到新 buffer
  //   再转移所有权过去是 O(1)。探针要拿它做逐字节比对，所以必须是一份快照
  //   （wasm 堆上那个视图任何一次 present 都会被原地覆盖）。
  if (m.name === 'rawFrame') {
    const v = mod.frameRgba();
    const copy = new Uint8ClampedArray(v.length);
    copy.set(v);
    return void post({ t: 'reply', id: m.id, value: copy }, [copy.buffer]);
  }
  const fn = READ[m.name];
  if (!fn) throw new Error('未知查询 ' + m.name);
  return void post({ t: 'reply', id: m.id, value: await fn(m.args || {}) });
}

/**
 * 视口变化：更新位置、按需换窗、必要时呈现并重贴。
 *
 * 返回 { moved, scrolled, workMs }：moved 表示窗口换了（已经 present + 重贴过），
 *   scrolled 表示滚动位置变了（连页模式下调用方要重贴）。
 */
function view(m) {
  const tWork = performance.now();
  const prevScrollY = scrollY;
  scrollY = Math.max(0, m.scrollY || 0);
  scrollLead = Number(m.leadPx) || 0;
  viewW = m.viewW || viewW;
  viewH = m.viewH || viewH;
  // 方向只用来决定余量放前面还是后面（见 targetWindow），得在 retarget 之前定下来。
  if (scrollY !== prevScrollY) scrollDir = scrollY > prevScrollY ? 1 : -1;
  const scrolled = scrollY !== prevScrollY;
  // 窗口没变就什么都不做：画布摆在文档流里，浏览器已经把像素搬走了。
  //   这一条是"滚动不花引擎的钱"的落点 —— 只回一个 ack，不重贴、不推统计。
  //   连页是例外：屏上那一张视口画布不动，页在它上面的落点是随滚动量算出来的，
  //   所以滚动量一变就必须重贴 —— 哪怕窗口一动没动（否则画面停在上一拍的位置，
  //   而那正是"滚动后有的页看着还是旧内容"的来源）。
  if (!retarget(JSON.parse(mod.editStatsJson()))) {
    return { moved: false, scrolled, workMs: performance.now() - tWork };
  }
  // 滚动这一路也会遇到视口变化（窗口被拉高/拉矮），所以槽数同样要在画之前报过去。
  //   这里本来就整窗重贴，槽数变过时正是它需要的。
  syncFrameSlots();
  mod.present();
  host.blitPages();
  schedulePrewarm(scrollY - prevScrollY);
  return { moved: true, scrolled, workMs: performance.now() - tWork };
}

/**
 * 页面画布到达：交给宿主登记，然后把已到齐的那几页补一遍，再回推几何。
 */
function pageCanvas(m) {
  host.addCanvas(m);
  sendState(m.ts, 'pageCanvas', JSON.parse(mod.editStatsJson()));
}

/**
 * 视图模式切换（分页 / 连页）：宿主已换好画布，这里改引擎的排版形状并整窗重贴。
 */
function viewCanvas(m) {
  host.setViewMode(m);
  sendState(m.ts, 'viewCanvas', JSON.parse(mod.editStatsJson()));
}

/**
 * 换页度量与重排（给宿主的 resize / dpr 变化用）。
 */
function relayout() {
  syncFrameSlots();
  mod.relayoutAndPaint();
  host.invalidateSlots();
  host.blitPages();
}

/**
 * 直接执行一条读命令，返回原值。给 in-thread 宿主用：它自己管呈现与窗口，
 *   不需要这边的收尾（present + blit + 回推 state），只要引擎那一步的结果。
 *
 * @param {string} name 读命令名
 * @param {object} args 参数
 * @returns {*} 命令结果；未知命令抛错
 */
function read(name, args) {
  const fn = READ[name];
  if (!fn) throw new Error('未知读取 ' + name);
  return fn(args || {});
}

/**
 * 直接执行一条写命令，返回引擎那一步的结果（通常是"有没有改动"）。
 *   in-thread 宿主拿到它之后自己做 settle（present + 重贴），收尾策略留在宿主侧。
 *
 * @param {string} name 写命令名
 * @param {object} args 参数
 * @returns {*} 引擎返回值；未知命令抛错
 */
function write(name, args) {
  const fn = WRITE[name] || SILENT[name];
  if (!fn) throw new Error('未知命令 ' + name);
  return fn(args || {});
}

/**
 * 把已经装好的引擎交给 API。
 *
 * in-thread 宿主的装载路径与 Worker 不同（它要读 HEAD、比构建戳、量字体字节），
 *   所以由它自己装载，再把实例交进来；Worker 侧仍走 boot。
 *
 * @param {object} engine wasm 模块实例
 */
function attach(engine) {
  mod = engine;
}

return {
  boot, do: doOp, ask: askOp, view, pageCanvas, viewCanvas, relayout, sendState, geom,
  attach, read, write,
  // 引擎句柄：只给宿主取帧视图与装载用，不要拿它绕过命令表。
  engine: () => mod,
  // 当前滚动位置（设备像素）：贴连页时算目标 y 要用。
  scrollY: () => scrollY,
  // 视口尺寸（设备像素）。
  viewSize: () => ({ w: viewW, h: viewH }),
  state: (ts, label, stats, changed, workMs) => sendState(ts, label, stats, changed, workMs),
};
}

// importScripts 与 <script> 都走这里。两侧各取一次，互不影响。
self.__heditorEngineApi = { createEngineApi };
})();
