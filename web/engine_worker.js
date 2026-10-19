// Worker 侧壳：槽、画布、消息。引擎语义全在 engine_api.js 里。
//
// 这个文件只做三件事，别让它长回来：
//   一、按槽摆画布并接收宿主转移过来的 OffscreenCanvas（帧不跨线程）；
//   二、把槽里的像素贴到画布上（blit），并报出贴了几页、花了多少毫秒；
//   三、把消息转发给 engine_api：boot / do / ask / view / pageCanvas / viewCanvas。
//
// 引擎状态、命令表、窗口滞回、上行状态对象都在 engine_api.js —— 那一份同时给单线程
//   宿主用（见 docs/一个引擎API与两种传输.md）。这里出现一个 mod.xxx 就说明边界破了。
//
// 画布是一页一张，不是一窗一张：
//   宿主在滚动容器里按页摆 canvas，每张 transferControlToOffscreen 交给这一侧，
//   这一侧的 map 按页号存上下文。滚动因此是浏览器搬像素（合成器），引擎一次都不跑；
//   编辑时也只重贴引擎报过的脏页，而不是整窗重贴 —— 改一个字只碰它所在的那一页。
//   帧本身不跨线程：它就是 wasm 线性内存上的一块，切片用子数组视图构造，不做拷贝。
(function () {
'use strict';

/**
 * 槽号 -> 该槽的离屏上下文。
 *
 * 槽 = 页在窗口里的位置（窗口首页是槽 0）。画布与转移都是一次性的，交出去就收不回来，
 *   所以"按槽固定"是唯一能复用的形态：换窗只改"哪个槽画哪一页"，上下文一直留着。
 */
const slotCtx = [];

/**
 * 槽数。与引擎的 kWindowMaxPages 同值，两侧各声明一份。
 *
 * 为什么取固定值而不是"按窗口需要多少就多少"：槽号 = 页号模槽数，槽数一变，
 *   所有页的槽号都会跟着变，于是"哪一页在哪个槽里"这件事整体作废、必须整窗重贴；
 *   更麻烦的是两侧会有一个消息往返的不同步窗口，那一帧里页会落到错的槽上。
 *   取固定值就没有这个问题：窗口页数上限本来就是 8（见 kWindowMaxPages），
 *   而槽数取 8 之后，窗口里任意连续 8 页以内的页号模 8 都互不相同。
 */
const kSlotCount = 8;

/**
 * 槽数。槽号 = 页号 mod 槽数，所以两侧必须是同一个数。
 *
 * 真源在宿主：画布在它手里，"实际有几个槽"只有它知道（它建了几个就交了几个）。
 *   它随每条 pageCanvas 消息把槽数报过来，这里照用 —— 不自己按几何现算。
 * 旧写法是两侧各算一次（这里取 max(8, 视口页数)，宿主取 max(8, 已建槽数)），
 *   在"视口从很高变矮"时分叉：宿主建的槽不回收，这里会回落到下限，
 *   于是槽号整体错位、画布上装的是别的页（out/_slotsync.mjs 复现过 11 对 8）。
 * 初值取 8 与宿主的下限一致：第一个 pageCanvas 到达之前不会有页要贴。
 */
let slotN = kSlotCount;

/**
 * 连页模式：一个视口画布，页按文档坐标贴（页间隙同时设成 0，页首尾相接）。
 *   与分页模式的差别只有"贴到哪儿" —— 源行仍按帧槽号取，帧的摆法与画布几张无关。
 */
let continuous = false;
let viewCtx = null;

// 页号 -> 槽号。取模处理负页号，所以先加一轮再取。
function slotOf(p) {
  return ((p % slotN) + slotN) % slotN;
}

/**
 * 每一槽当前画着哪一页（-1 = 空，或内容已整体失效）。
 *
 * 它是"要不要重贴"的判据：窗口平移之后，只有新进窗口的那几页所落的槽装着别的页，
 *   其余页仍在自己那个槽里、像素是对的，所以只要贴那几页 —— 这就是按模取槽换来的
 *   东西：窗口平移不再等于"所有槽都换了页"。
 */
const slotPage = [];

// 内容整体作废（换缩放、换页度量、换文档）：所有槽都要重贴。
function invalidateSlots() {
  slotPage.length = slotN;
  slotPage.fill(-1);
}

/**
 * 把槽数换成宿主报来的值。变了就整体作废 —— 槽数一变，所有页的槽号都跟着变。
 *   只在 pageCanvas 到达时调（那是宿主建槽的唯一时刻）。
 */
function setSlotCount(n) {
  const v = n | 0;
  if (v <= 0 || v === slotN) return false;
  slotN = v;
  invalidateSlots();
  return true;
}

/**
 * 页号 -> 上下文。现算，不缓存。
 *
 * 为什么不做成缓存的映射：读它的地方不只有贴图那一条路（自审那条走的是 ask，不经过
 *   贴图）。缓存的派生值只要有一条路没更新到，读出来就是上一窗的对应 —— 症状是
 *   "画布与帧对不上"，而看代码完全看不出哪里错了。现算的成本可以忽略。
 */
function ctxOfPage(p) {
  const e = api.engine();
  if (!e) return null;
  const first = e.windowFirstPage();
  if (p < first || p >= first + e.windowPageCount()) return null;
  return slotCtx[slotOf(p)] || null;
}

/* 上屏 */

/**
 * 把窗口帧里第 p 页那一段贴到它自己的画布上。
 *
 * 源是 wasm 线性内存上的整帧，用子数组视图取那几行连成一张小图 —— 不做拷贝。
 *   刻意不用 putImageData 的脏矩形重载：那个重载的目标位置口径（是否叠加 dirtyX/dirtyY）
 *   在实现之间容易踩错，两参数形式没有歧义。
 */
function blitPage(p) {
  const c = ctxOfPage(p);
  if (!c) return false;
  const e = api.engine();
  const fw = e.frameWidth();
  const ph = e.pagePxHeight();
  const step = ph + e.pageGapPx();
  const first = e.windowFirstPage();
  const local = p - first;
  if (local < 0 || local >= e.windowPageCount()) return false;
  if (fw <= 0 || ph <= 0) return false;
  const fv = e.frameRgba();
  if (c.canvas.width !== fw || c.canvas.height !== ph) {
    // 改尺寸会清空画布，所以尺寸变了必定整块重画。
    c.canvas.width = fw;
    c.canvas.height = ph;
  }
  // 源行按帧槽号算（页号 mod 引擎的槽数），不是"窗口内第几页"。
  //   槽表变了以后这就是唯一正确的算法；本地那个 local 只用来判"这一页在不在窗口里"。
  const fs = (e.frameSlots ? e.frameSlots() : 0) | 0;
  const srcY = (fs > 0 ? ((p % fs) + fs) % fs : local) * step;
  const img = new ImageData(
    new Uint8ClampedArray(fv.buffer, fv.byteOffset + srcY * fw * 4, fw * ph * 4), fw, ph);
  c.putImageData(img, 0, 0);
  return true;
}

/**
 * 连页模式：一个视口画布，页按文档坐标贴到它上面（同时页间隙设成 0，页首尾相接）。
 *
 * 与分页模式的差别只有"贴到哪儿"：源行仍按帧槽号取（帧怎么摆与画布几张无关），
 *   目标 y = 页号 x 步距 - 滚动量。超出画布的部分由 putImageData 自己裁掉。
 * 代价是"每拍都要贴"：滚动 1 像素，每页的目标 y 都变了，槽过期那套判据在这里不适用。
 *   实测一屏五页 1.1 ms（out/_blitprobe.mjs），与分页模式的一页一贴相当。
 */
function blitPageContinuous(p) {
  if (!viewCtx) return false;
  const e = api.engine();
  const vh = api.viewSize().h;
  const fw = e.frameWidth();
  const ph = e.pagePxHeight();
  const step = ph + e.pageGapPx();
  if (fw <= 0 || ph <= 0) return false;
  const dy = p * step - api.scrollY();
  if (dy + ph <= 0 || dy >= vh) return false;  // 整页在视口之外
  if (viewCtx.canvas.width !== fw || viewCtx.canvas.height !== vh) {
    viewCtx.canvas.width = fw;
    viewCtx.canvas.height = vh;
  }
  const fv = e.frameRgba();
  const fs = (e.frameSlots ? e.frameSlots() : 0) | 0;
  const local = p - e.windowFirstPage();
  const srcY = (fs > 0 ? ((p % fs) + fs) % fs : local) * step;
  const img = new ImageData(
    new Uint8ClampedArray(fv.buffer, fv.byteOffset + srcY * fw * 4, fw * ph * 4), fw, ph);
  viewCtx.putImageData(img, 0, dy);
  return true;
}

/**
 * 重贴。
 *
 * only 为空 = 窗口里每页都贴（窗口换了、或有新页的画布刚到，必须整窗来一遍）；
 *   only 给页号清单 = 只贴这几页 —— 编辑时用引擎报的脏页清单。一页一贴正是
 *   "一页一张画布"换来的东西：改一个字只碰它所在的那一页。
 *
 * 计时与页数随状态回推（见 engine_api.js sendState 的 blitMs）：它是"往返减去渲染"
 *   那一段里的一刀。
 */
let blitLastMs = 0;
let blitLastPages = 0;
function blitPages(only) {
  const tBlit = performance.now();
  const e = api.engine();
  if (!e) return 0;
  const first = e.windowFirstPage();
  const count = e.windowPageCount();
  let n = 0;
  if (continuous) {
    // 连页模式：目标 y 随滚动变，所以每拍都重贴（不看槽过期与脏页）。
    //   先清一遍：视口内可能露出"没有页"的地方（文档首尾之外），而那些像素
    //   还留着上一拍的内容。clearRect 写的是透明，页容器的底色因此透得出来。
    if (viewCtx) viewCtx.clearRect(0, 0, viewCtx.canvas.width, api.viewSize().h);
    for (let p = first; p < first + count; ++p) if (blitPageContinuous(p)) ++n;
    blitLastMs = performance.now() - tBlit;
    blitLastPages = n;
    return n;
  }
  for (let p = first; p < first + count; ++p) {
    const s = slotOf(p);
    // 两个理由之一成立就要贴：这一页的内容刚变（在 only 里），或者它落的那个槽
    //   装的还是别的页（窗口平移进来的新页、或内容整体作废）。
    //   两者都不成立时它已经是对的 —— 窗口平移的绝大多数槽都属于这一档。
    const stale = slotPage[s] !== p;
    const dirty = only ? only.includes(p) : false;
    if (!stale && !dirty) continue;
    if (blitPage(p)) {
      slotPage[s] = p;
      ++n;
    }
  }
  blitLastMs = performance.now() - tBlit;
  blitLastPages = n;
  return n;
}

// 这一窗里还有几页没有画布（宿主还没把该槽的 canvas 交过来）。
function pagesMissing() {
  const e = api.engine();
  if (!e) return 0;
  const count = e.windowPageCount();
  let n = 0;
  for (let s = 0; s < count; ++s) if (!slotCtx[s]) ++n;
  return n;
}

/* 传输 */

let api = null;
const post = (msg, transfer) => self.postMessage(msg, transfer || []);

const host = {
  /**
   * 加载胶水层并返回工厂。Worker 侧只有 importScripts 一条路；
   *   主线程侧的实现是 <script>，所以这件事交给宿主，engine_api 不碰。
   */
  loadGlue(url, exportName) {
    importScripts(url);
    return self[exportName];
  },
  slotCount: () => slotN,
  pagesMissing,
  ctxOfPage,
  isContinuous: () => continuous,
  viewCtx: () => viewCtx,
  canvasCount: () => slotCtx.reduce((n, c) => n + (c ? 1 : 0), 0),
  invalidateSlots,
  blitPages,
  blitStats: () => ({ ms: blitLastMs, pages: blitLastPages }),

  /**
   * 某个槽的画布到了：记下它，把那一槽贴出来。
   *   槽只建一次，之后换窗只会再收到新的槽，不会重复收到同一个槽。
   */
  addCanvas(m) {
    const c = m.canvas.getContext('2d', { alpha: false });
    if (!c) throw new Error('OffscreenCanvas 拿不到 2d 上下文');
    // 槽数由宿主说了算：它建了几个槽、就把几条画布交过来，槽号也由它定。
    if (setSlotCount(m.slots)) slotCtx.length = Math.max(slotCtx.length, slotN);
    slotCtx[m.slot | 0] = c;
    // 新来的槽内容未知，先作废，再让 blitPages 把该在它上面的那一页贴上去。
    //   刚启动时八个槽陆续到达，每来一个都会把已到齐的那几页补一遍，自然收敛。
    slotPage[m.slot | 0] = -1;
    blitPages(null);
  },

  /**
   * 视图模式与它的页间隙。连页：宿主建一张视口画布交过来（间隙 0 = 页首尾相接）；
   *   回分页：不带画布（间隙回到 14），这里把连页标志与画布一起丢掉。
   *   间隙跟着模式走是有意的 —— 它是"页的落点"这件事的一半，两套模式是两套落点，
   *   分开传会让中间那一拍出现"连页的间隙 + 分页的画布"这种不存在的组合。
   */
  setViewMode(m) {
    const e = api.engine();
    if (typeof m.gap === 'number' && e.setPageGap) e.setPageGap(m.gap);
    continuous = !!m.continuous;
    // 引擎侧的连页标志：它改的是排版的形状（页底断行、显式分页符、竖排吃页尾），
    //   所以必须与页间隙一起设下去 —— 只设间隙的话，排版还是分页的，只是页首尾
    //   相接（内容照样在页底断开），那正是"看着像连页、其实还有页"的来源。
    if (e.setContinuous) e.setContinuous(continuous);
    viewCtx = null;
    if (continuous && m.canvas) {
      viewCtx = m.canvas.getContext('2d');
      if (!viewCtx) throw new Error('OffscreenCanvas 拿不到 2d 上下文');
    }
    // 切模式会把帧的布局与页的落点整套换掉（间隙 0 与 14 是两套），所以这里要
    //   重排 + 重画帧，再作废槽表重贴 —— 三件缺一不可：
    //   间隙一改步距就变，而帧缓冲里还是上一套布局的像素。只作废槽表重贴的话，
    //   贴出去的是"新步距 x 旧像素"，从第二页起整体错位（第一页恰好对，它的偏移是 0）；
    //   更麻烦的是槽表随即被记成"这一槽已经装了这一页"，后面整窗重贴也会跳过它，
    //   错位就永久留在画面上。
    api.relayout();
  },
};

/* 消息入口 */

async function handleBoot(m) {
  // 带同一个缓存戳加载引擎 API：Worker 脚本的请求不受 Page 侧"关掉缓存"的影响，
  //   不带给 bust 就会跑旧副本（症状是改了 engine_api.js 页面毫无反应）。
  importScripts('engine_api.js' + (m.bust || ''));
  const factory = self.__heditorEngineApi;
  if (!factory) throw new Error('engine_api.js 没挂上 __heditorEngineApi');
  api = factory.createEngineApi({ post, host });
  await api.boot(m);
}

async function handle(m) {
  try {
    if (m.t === 'boot') return void await handleBoot(m);
    if (!api) throw new Error('引擎尚未 boot');
    if (m.t === 'pageCanvas') return void api.pageCanvas(m);
    if (m.t === 'viewCanvas') return void api.viewCanvas(m);
    if (m.t === 'view') {
      // 自报这一下花了多少毫秒。宿主拿它和"发出→收到回话"的往返比：差值是排队等待，
      //   近 0 说明队列是空的（慢是因为这一下真的贵），差得远说明 Worker 在补课。
      const r = api.view(m);
      if (!r.moved) {
        if (continuous && r.scrolled) blitPages(null);
        return void post({ t: 'ack', ts: m.ts, workMs: r.workMs });
      }
      return void api.state(m.ts, 'view', JSON.parse(api.engine().editStatsJson()), undefined,
                            r.workMs);
    }
    if (m.t === 'do') return void await api.do(m);
    if (m.t === 'ask') return void await api.ask(m);
    throw new Error('未知消息类型 ' + m.t);
  } catch (e) {
    if (m.t === 'ask') post({ t: 'reply', id: m.id, error: String((e && e.stack) || e) });
    // 视口消息抛了异常也要回一条：宿主那边靠回话解门闩，丢一条就把之后的滚动锁死
    //   （画面再也不动）。异常这条路只发错误通知、不回话，宿主就只能靠"超时放行"去猜，
    //   而超时放行会在真忙的时候放进第二条，队列于是又排起来，正是要消掉的东西。
    if (m.t === 'view') post({ t: 'ack', ts: m.ts, workMs: -1 });
    post({ t: 'error', where: m.t + (m.name ? ':' + m.name : ''),
           message: String((e && e.stack) || e) });
  }
}

self.onmessage = (ev) => { handle(ev.data || {}); };
})();
