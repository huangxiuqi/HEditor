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
 * 连页模式：与分页同一套贴图，区别只有页间隙 = 0（页首尾相接，看起来是一整条）。
 *   页画布本身就摆在 (页号 x 步距) 上，滚动由浏览器搬像素，引擎一次都不用被叫醒 ——
 *   这正是删掉"视口画布 + 每拍按滚动量重贴"那条支线换来的东西。
 */
let continuous = false;

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
 * 把第 p 页"画到画布上应该是什么"贴到它自己的画布上。
 *
 * rects 为空 = 整页贴：这张画布是新的（刚建、或刚改过尺寸），内容未知。
 * rects 非空 = 只贴这几块（页内像素坐标，来自引擎的 pageDirtyRects）：
 *   光标移动一次只有几条几十像素的矩形，而整页是 1 Mpx —— 差三个数量级。
 *
 * 源是 wasm 线性内存上这一页的导出像素（内容 + 瞬态，见 pageRgba），坐标就是页内坐标 ——
 *   所以这里没有"帧槽偏移"要折：页号对上，矩形对上的就是那一块。
 */
function blitPage(p, rects) {
  const c = ctxOfPage(p);
  if (!c) return 0;
  const e = api.engine();
  const pw = e.pagePxWidth();
  const ph = e.pagePxHeight();
  if (pw <= 0 || ph <= 0) return 0;
  const src = e.pageRgba(p);
  if (src.length !== pw * ph * 4) return 0;
  if (c.canvas.width !== pw || c.canvas.height !== ph) {
    // 改尺寸会清空画布，所以尺寸变了必定整块重画。
    c.canvas.width = pw;
    c.canvas.height = ph;
    rects = null;
  }
  if (!rects || !rects.length) {
    c.putImageData(
        new ImageData(new Uint8ClampedArray(src.buffer, src.byteOffset, src.length), pw, ph), 0,
        0);
    // 整页贴过了，这一页的矩形就没有意义了 —— 留着下一拍只会白贴一次。
    e.pageDirtyRectsJson(p);
    return pw * ph;
  }
  let px = 0;
  for (const r of rects) {
    if (!(r.w > 0) || !(r.h > 0)) continue;
    // 逐块拷成一张小图，再用两参数形式贴到页内坐标。
    //   六参数形式的语义是"目标 = (dx + dirtyX, dy + dirtyY)"，跨页时必须给负的 dy；
    //   负 dy 在 OffscreenCanvas 上会把那一块裁错（贴完仍差同样多像素，而普通 canvas
    //   上同一个表达式是对的）。这里没有非用六参数不可的理由 —— 一块几十像素的拷贝
    //   比"猜实现"便宜。
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
 * only 为空 = 窗口里每页都贴（窗口换了、或有新页的画布刚到，必须整窗来一遍）；
 *   only 给页号清单 = 只贴这几页 —— 编辑时用引擎报的脏页清单。一页一贴正是
 *   "一页一张画布"换来的东西：改一个字只碰它所在的那一页。
 *
 * 计时与页数随状态回推（见 engine_api.js sendState 的 blitMs）：它是"往返减去渲染"
 *   那一段里的一刀。
 */
let blitLastMs = 0;
let blitLastPages = 0;
let blitLastPx = 0;
function blitPages() {
  const tBlit = performance.now();
  const e = api.engine();
  if (!e) return 0;
  const first = e.windowFirstPage();
  const count = e.windowPageCount();
  let n = 0;
  let px = 0;
  // 要贴哪几页由"还欠着脏矩形"决定，不是"这一拍写过哪几页"。矩形是累积的：
  //   某一拍的 present 可能根本没被消费（窗口平移、槽重建、页面不可见），那一拍的
  //   矩形会留到这一拍。按本拍清单贴就会永久漏掉那几块 —— 屏幕上留几十像素的旧光标。
  const pending = JSON.parse(e.pageDirtyPagesJson());
  for (let p = first; p < first + count; ++p) {
    const s = slotOf(p);
    // 两个理由之一成立就要贴：这一页还欠着像素，或者它落的那个槽装的还是别的页
    //   （窗口平移进来的新页、或内容整体作废）。
    //   两者都不成立时它已经是对的 —— 窗口平移的绝大多数槽都属于这一档。
    const stale = slotPage[s] !== p;
    const dirty = pending.includes(p);
    if (!stale && !dirty) continue;
    // 槽里是别的页 ⇒ 整页贴（画布上那一份不属于这一页）。
    // 只是这一页变了 ⇒ 按引擎给的矩形贴：光标一次移动只有几十像素，
    //   而整页是 1 Mpx。报脏却给不出矩形时退回整页 —— 宁可多传，
    //   不能让画布上留着上一拍的像素（那种错只在切页之后显形）。
    let rects = null;
    if (!stale && dirty) {
      rects = JSON.parse(e.pageDirtyRectsJson(p));
      if (!rects.length) rects = null;
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

// 这一窗里还有几页没有画布（宿主还没把该页落的那个槽的 canvas 交过来）。
//   按页数、不按槽数：槽数可以大于窗口页数（那是"历史最大值"），空槽不算缺画布。
function pagesMissing() {
  const e = api.engine();
  if (!e) return 0;
  const first = e.windowFirstPage();
  const count = e.windowPageCount();
  let n = 0;
  for (let p = first; p < first + count; ++p) {
    if (!ctxOfPage(p)) ++n;
  }
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
  canvasCount: () => slotCtx.reduce((n, c) => n + (c ? 1 : 0), 0),
  invalidateSlots,
  blitPages,
  blitStats: () => ({ ms: blitLastMs, pages: blitLastPages, px: blitLastPx }),

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
    blitPages();
  },

  /**
   * 视图模式与它的页间隙。连页 = 间隙 0（页首尾相接）+ 引擎按连页排版；
   *   回分页 = 间隙 14。两套模式共用同一套"一页一张画布"，区别只有步距。
   *   间隙跟着模式走是有意的 —— 它是"页的落点"这件事的一半，分两路传会让中间那一拍
   *   出现"连页的间隙 + 分页的落点"这种不存在的组合。
   */
  setViewMode(m) {
    const e = api.engine();
    if (typeof m.gap === 'number' && e.setPageGap) e.setPageGap(m.gap);
    continuous = !!m.continuous;
    // 引擎侧的连页标志：它改的是排版的形状（页底断行、显式分页符、竖排吃页尾），
    //   所以必须与页间隙一起设下去 —— 只设间隙的话，排版还是分页的，只是页首尾
    //   相接（内容照样在页底断开），那正是"看着像连页、其实还有页"的来源。
    if (e.setContinuous) e.setContinuous(continuous);
    // 切模式会把排版的形状与页的落点整套换掉（间隙 0 与 14 是两套），所以这里要
    //   重排 + 重光栅页位图，再作废槽表重贴 —— 三件缺一不可：
    //   间隙一改步距就变，而页位图里还是上一套布局的像素。只作废槽表重贴的话，
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
    if (m.t === 'viewMode') return void api.viewMode(m);
    if (m.t === 'view') {
      // 自报这一下花了多少毫秒。宿主拿它和"发出→收到回话"的往返比：差值是排队等待，
      //   近 0 说明队列是空的（慢是因为这一下真的贵），差得远说明 Worker 在补课。
      const r = api.view(m);
      // 没换窗口 = 没有新内容要贴：页画布的位置是固定的 (页号 x 步距)，
      //   滚动是浏览器搬像素（连页与分页在这一层已经没有区别）。
      if (!r.moved) return void post({ t: 'ack', ts: m.ts, workMs: r.workMs });
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
