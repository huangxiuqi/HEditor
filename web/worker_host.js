// 主线程侧：只做四件事，不碰像素，也不做排版。
//
//   一、按页摆 canvas 并把每张的所有权交给 Worker（帧从此不跨线程搬，一页一张）；
//   二、把键盘 / 输入法 / 滚动事件发过去，不等回话 —— 交互路径因此零 await；
//   三、接收 Worker 推回来的镜像（统计、光标矩形、页面几何、控件清单），用它摆输入法
//       候选窗、光标闪烁与滚动条，并让门面里那批"读"保持同步形态；
//   四、把命令与查询暴露成与单线程宿主同名的方法；命令返回 Promise 但不等它也行。
//
// 为什么是一页一张：画布摆在滚动容器里各自的页位置上，所以滚动由浏览器合成器搬像素，
//   引擎一次都不跑；编辑时 Worker 只重贴引擎报过的脏页，不是整窗重贴。
//   （一张和视口一样大、粘在顶上的画布也能跑，但每次滚动都要过一次边框 —— 实测 2~10 ms。）
//
// 为什么必须异步：主线程无法同步等 Worker。Atomics.wait 在主线程直接抛，而且它要
//   SharedArrayBuffer ⇒ 要跨源隔离。所以引擎搬进 Worker 就等于门面异步 —— 这是代价，
//   不是选择。上面第二条把交互路径上的 await 全消掉了：消息在调用那一刻就发出去了，
//   返回的 Promise 只是"结果到了"的通知。
//
// 整个文件包一层立即执行函数：同页多实例时这一份会与另一个宿主（app.js）一起加载，
//   两边各有自己的顶层常量（FONTS / DPR 之类），不隔离就是"标识符重复声明"的语法错误。
(function () {

// 两宿主共用的输入层（键位映射、命中归宿、连击、组合提交、DPI 跟随）。
//   没有它就不启动：缺了它也能跑，只是键盘与指针各走一份，两边迟早分叉。
if (!window.HEditorCore) throw new Error('web/host_core.js 未加载');
const core = window.HEditorCore;

// 屏幕像素比：1 个 CSS 像素背后有几个设备像素。渲染密度与显示换算都以它为准。
//   做成现读而不是加载时缓存：缓存那份一旦与屏幕脱节（窗口被拖到另一块显示器上），
//   凡是拿它折算的尺寸都会错，而且错得整整齐齐、看不出是 DPR 的问题。
//   与单线程宿主同构 —— 那边本来就是每处现读 window.devicePixelRatio。
const dprNow = () => window.devicePixelRatio || 1;
const FONTS = [
  { role: 'cjk', label: 'Noto Sans SC', path: 'assets/fonts/NotoSansSC-VF.ttf' },
  { role: 'mongol', label: 'Noto Sans Mongolian', path: 'assets/fonts/NotoSansMongolian-Regular.ttf' },
  { role: 'latin', label: 'Noto Sans (Latin)', path: 'assets/fonts/NotoSans-Latin-VF.ttf' },
];

/**
 * 缓存戳：验证台的 Chrome profile 是跨次运行复用的，而 Worker 脚本的请求不受
 * Page 侧"关掉缓存"的影响 —— 出现过改了 engine_worker.js 但页面仍跑旧副本。
 *   查询串同时带给胶水层与 wasm，避免"新胶水 + 旧 wasm"这种最难查的组合。
 */
const BUST = '?v=' + Date.now();
const CALL_TIMEOUT_MS = 15000;

/**
 * 造一个"引擎在 Worker 里"的编辑器实例：舞台、页容器、输入层都在 container 里找。
 *
 * 每份实例有自己的 Worker、自己的挂起表、自己的镜像与页画布 —— 同页多实例时互不来往。
 *   查找限定在容器内，因为 #stage / #ime 在同页多实例时重名，全局查找只会命中第一个
 *   实例那一份；容器里没有的元素再回落到整页查找，演示页的工具栏因此仍然接得上。
 *   opts.chrome 为 false 时只接编辑器本体（舞台 / 页容器 / 输入层）。
 *
 * 返回值是门面（与单线程宿主同形），另带 whenReady（resolve 成 { ok, error }，刻意
 *   不做成失败即 reject：没人接的 rejection 会变成一条未处理的异步失败）与 dispose()
 *   （终止 Worker、清定时器、摘画布）。函数体保持文件原有的零缩进，逐行重排缩进会把
 *   diff 变成整文件重写。
 */
function createWorkerEditor(container, opts) {
const o = opts || {};
const root = container || document.body;
const chrome = o.chrome !== false;
const $ = (id) => root.querySelector('#' + id) || document.getElementById(id);

const stageEl = $('stage');
const spacerEl = $('spacer');
const ta = $('ime');
const statEl = chrome ? $('stats') : null;
const barEl = chrome ? $('bar') : null;
const statusEl = chrome ? $('status') : null;

// 状态栏（与单线程宿主同形）：组合期间在这里本地预览预编辑串。
function setStatus(text) {
  if (statusEl) statusEl.textContent = text;
}

/**
 * 错误降级面（见 web/host_error.js）。
 *
 * 两档的分法在这一侧尤其重要，因为报错来源有两类：Worker 脚本自身出错（整条不可用），
 *   以及某一条命令在 Worker 里抛异常（引擎还活着，换一条命令照常能用）。此前两者都只
 *   写进 errors 数组，页面上反映出来的区别只是"点什么都不动了"。
 */
const errorRoot = root;
const errorSurface = typeof window.createErrorSurface === 'function'
  ? window.createErrorSurface(errorRoot, {}) : null;

// 容器里必须有三件：滚动舞台、撑高度的占位块、承接键盘与输入法的 textarea。
//   缺了就是容器配错，当场说清楚，而不是等到摆输入层时抛一个看不懂的错。
if (!stageEl || !spacerEl || !ta) {
  const missing = [!stageEl && '#stage', !spacerEl && '#spacer', !ta && '#ime']
    .filter(Boolean).join(' / ');
  const err = new Error(`容器里缺少 ${missing}，无法建立编辑器`);
  if (errorSurface) {
    errorSurface.fatal('编辑器初始化失败', {
      detail: err.message,
      hint: 'container 必须是包含 #stage / #spacer / #ime 的那个元素。',
    });
  }
  throw err;
}
const report = {
  fatal(message, info) { if (errorSurface) errorSurface.fatal(message, info); },
  warn(message, info) { if (errorSurface) errorSurface.warn(message, info); },
  get entries() { return errorSurface ? errorSurface.entries : []; },
  get fatalActive() { return errorSurface ? errorSurface.fatalActive : false; },
};

/**
 * 引擎是否已经不可用。
 *
 * 一旦为真：命令不再发出去（发了也没人执行，只会变成一串超时），查询当场以明确的
 *   理由拒绝。判据是"页面上已经有一条致命横幅"，而不是"猜测还能不能恢复"——
 *   唯一的恢复路径是页面重载，横幅上给了按钮。
 */
let dead = false;

// 这一份实例是否已经被拆掉（拆掉之后门面一律给空值，不再报告任何内容）。
let disposed = false;

const errors = [];
// 挂起表用自增序号做键，不能用 performance.now()：
//   页面没做跨源隔离时 Chrome 把计时器粗化到 100 µs，do 与 view 又是同一张表 ——
//   同一拍里两条消息会撞键，后一条把前一条静默覆盖，那条 Promise 就永远不落地
//   （症状是"视口消息超时"，而 Worker 其实既没报错也还活着）。发送时刻另存在
//   条目里，只用来算延迟。
const pendingState = new Map();  // 序号 -> {resolve,reject,tt,name}
const pendingReply = new Map();  // 序号 -> {resolve,reject,name}
// 槽数不在这里定：它等于窗口页数上限，由 worker 随几何报来（geom.maxWindowPages），
//   宿主再把自己实际建了几个报回去（pageCanvas.slots）。两侧必须同一个数 ——
//   各算一次就会分叉（视口从很高变矮时，一边只增不减、一边回落到下限，槽号整体错位）。
const slotOf = (p, n) => ((p % n) + n) % n;
// 槽号 -> canvas 元素。只增不减：建到上限就不再建（已转移的画布不能再转移第二次）。
//   窗口页数上限变小时多出来的槽留着不用，槽号仍按实际建了几个算。
const slotCanvases = [];
// 页号 -> canvas 元素。每个状态按槽重建的派生映射 —— 下游一律按页号读，所以它必须存在，
//   但它不是真源：真源是 slotCanvases 加当前窗口首页。
const pageCanvases = new Map();
let opSeq = 0;
let askSeq = 0;
let worker = null;
let bootLead = 0;
let input = null;         // 输入层句柄（见 host_core.createInput）
let blinkTimer = null;
let blinkAuto = false;   // 闪烁只在首帧自动起一次，别把 stopBlink 又拉起来
let caretOn = true;
// 与两宿主同一口径（理由见 app.js 的同名声明）：缩放 1 倍 = 96 dpi，其密度是 4 / 3。
//   界面输入框收"倍数"，引擎与门面收"密度"，换算只在 syncZoomInput 与 zoomToFactor 两处做。
//
// 换算本身与"两个 DPR"的记账在 host_core.createZoom 里（两宿主同一份）：
//   一个是当下屏幕的，一个是现有密度按哪个屏幕算的 —— 换屏那一拍两者不同。
// 这里保留原有函数名，调用点一处不动；密度仍存在 zoom 这个局部变量里。
const zoomCtl = core.createZoom({ base: 4 / 3 });
const baseDensity = () => zoomCtl.base();
const engineZoomOf = (factor) => zoomCtl.ofFactor(factor);
const uiZoomOf = (density) => zoomCtl.factorOf(density);
const setZoomDensity = (d) => { zoom = zoomCtl.set(d); };
let zoom = baseDensity();
let scenario = 6;
let mode = 0;
let lastPageX = 0;   // 页在视口里横向居中的偏移（设备像素）
let drag = null;     // 拖选控制器（共享模块造的，按下之后由它接管）
let lastPointerId = null;   // 最近一次 pointerdown 的指针 id（按下时用来做指针捕获）
let capturedId = null;      // 已经捕获在容器上的指针 id（下次按下时先放掉，见 pointerdown）

/**
 * 把当前的缩放按"倍数"回写到页面上的 #zoom 输入框。
 *
 * 输入框那一格是倍数（1 = 100% = 96 dpi），引擎与门面收的是密度（设备像素 / 磅），
 *   所以写进去之前要折成倍数。要拿换算因子请读门面的 zoomLevel —— 输入框里
 *   那个数不是它。
 *
 * 这一笔回写本身是有来历的：单线程宿主的 run() 会写输入框，搬进 Worker 之后这条回写
 *   一度没了 —— 于是引擎按 1.0 排版、外部脚本按输入框里的旧值算坐标，点击整片落到
 *   别处（错得整整齐齐，最不像坐标问题）。所以每次改 zoom 都要同步它。
 *   4 / 3 那样的无限小数截到 4 位再写，免得住 number 输入框里塞一串 1.3333333333333333
 *   （差 0.00003，千像素尺度上不到 0.1 像素，落不进任何容差）。
 */
function syncZoomInput() {
  const el = $('zoom');
  if (el) el.value = String(Number(uiZoomOf(zoom).toFixed(4)));
}

const now = () => performance.now();
// 引擎已不可用时不再往外发：这些消息只会变成一串超时，把真正的失败藏起来。
const post = (msg, transfer) => {
  if (dead) return;
  worker.postMessage(msg, transfer || []);
};

const mirror = {
  info: null, stats: null, layout: null, geom: null,
  text0: '', controls: null, blockedEdits: 0, dirtyPages: [], caretReport: '',
  selection: null, caretStyle: null, selectionStyle: null,
  caretLive: -1, heapMB: 0, fontMode: 'full', stateSeq: 0, lastLabel: '', lastMs: 0,
};
const host = { ready: false, errors, mirror, pageCanvasCount: 0 };

function glueUrl() {
  // 与单线程宿主的 WASM_DIR 同一口径：debug 走调试档，留空走交付档，
  //   别的值按产物目录处理（用来复现"产物不存在"这条失败路径）。
  const p = new URLSearchParams(location.search).get('wasm') || '';
  const dir = p === 'debug' ? 'build/wasm-debug/web/'
            : p ? (p.endsWith('/') ? p : p + '/')
            : 'build/wasm/web/';
  return new URL(dir + 'heditor.js', new URL('../', location.href)).href;
}

// 视口尺寸（设备像素）。画布尺寸不归宿主定，但"看得见多大"要知道。
const viewSize = () => ({
  viewW: Math.max(1, Math.round(stageEl.clientWidth * dprNow())),
  viewH: Math.max(1, Math.round(stageEl.clientHeight * dprNow())),
});

/* 消息 */

/**
 * 发一条"要等回话"的消息（命令与视口都用它）。返回 Promise；不等它就是单向消息
 *   —— 消息在调用那一刻就发出去了，"等"只是拿到通知。
 */
function waitOp(name, msg) {
  if (dead) return Promise.reject(new Error(`引擎已不可用，命令 ${name} 未发送`));
  const id = ++opSeq;
  const tt = now();
  return new Promise((resolve, reject) => {
    const to = setTimeout(() => {
      if (pendingState.delete(id)) {
        // 超时也降级给用户看：调用方各自只关心自己那一条，页面上要有人知道
        //   "刚才那一下没成功"。
        report.warn(`命令 ${name} 超时（${CALL_TIMEOUT_MS} ms 没有回话）`);
        reject(new Error(`${name} 超时`));
      }
    }, CALL_TIMEOUT_MS);
    pendingState.set(id, {
      resolve: (v) => { clearTimeout(to); resolve(v); },
      reject,
      tt,
      name,
    });
    post(Object.assign({ ts: id }, msg));
  });
}

/**
 * 发一条命令。返回 Promise，resolve 成 editStats（与单线程宿主的门面同形）。
 *
 * nullIfUnchanged = true 的那些入口（引擎那一步返回假值）resolve 成 null ——
 *   这是门面契约里的"没改动"，不是失败：对照 app.js 的
 *   if (!mod.X()) return null; return this.present();。
 *   不区分的话，探针里"已在段首向左移动不重绘（返回 null）"这类断言会永远拿到真值。
 */
function doOp(name, args, nullIfUnchanged) {
  return waitOp(`命令 ${name}`, { t: 'do', name, args: args || {} }).then((m) => {
    const st = m && m.stats ? m.stats : null;
    return nullIfUnchanged ? (m && m.changed ? st : null) : st;
  });
}

/**
 * 单向命令：发出去就不管了（ts=0 ⇒ Worker 推回来的 state 找不到对应挂起条目）。
 *   闪烁那一拍每 530 ms 来一次，给它也建挂起条目只是白造一批 setTimeout。
 */
const postOp = (name, args) => post({ t: 'do', ts: 0, name, args: args || {} });

/**
 * 用户动作（门面里的命令）：先把光标按住可见并重置闪烁，再把命令发出去。
 *
 * 放在命令之前而不是之后：命令自身的呈现才会把光标画在新位置上，先按住可见才能
 *   保证"移动完的那一帧光标就在那儿"，中间不夹一个隐藏帧。
 */
function opUser(name, args, nullIfUnchanged) {
  blinkHold();
  return doOp(name, args, nullIfUnchanged);
}

function askOp(name, args) {
  if (dead) return Promise.reject(new Error(`引擎已不可用，查询 ${name} 未发送`));
  const id = ++askSeq;
  return new Promise((resolve, reject) => {
    const to = setTimeout(() => {
      if (pendingReply.delete(id)) {
        report.warn(`查询 ${name} 超时（${CALL_TIMEOUT_MS} ms 没有回话）`);
        reject(new Error(`查询 ${name} 超时`));
      }
    }, CALL_TIMEOUT_MS);
    pendingReply.set(id, { resolve: (v) => { clearTimeout(to); resolve(v); }, reject, name });
    post({ t: 'ask', id, name, args: args || {} });
  });
}

function viewOp(scrollDevicePx) {
  const vs = viewSize();
  // 与命令同一个口径：resolve 成 editStats（窗口没换时 Worker 只回 ack，那就是 null）。
  return waitOp('视口', { t: 'view', scrollY: scrollDevicePx, viewW: vs.viewW, viewH: vs.viewH })
    .then((m) => (m && m.stats ? m.stats : null));
}

/**
 * 滚动事件专用的单向版本：不要回话，而且最多一条在飞。
 *
 * 为什么不要回话：滚动这条路上没人等结果（画布在文档流里，浏览器已经搬完像素了），
 *   给它也发一个 Promise 只是白造一批挂起条目。
 * 为什么还要门闩：甩滚时 scroll 事件每秒上百个，而 Worker 是一条线程、逐条处理，
 *   每次换窗要 20~45 ms —— 不限量就是"命令排在队列里排队执行"，画面呈现出来的就是
 *   "已经滚到底了，中间那些窗口还在一个一个地渲染"，停下后还要等一秒才追平
 *   （取证时状态里的窗口序列一路补课）。门闩把队列压到"一条在飞 + 一个待发标记"，
 *   于是 Worker 永远只处理最新那一个位置，中间那些过期的位置根本不会进去。
 *   代价是丢掉中间位置 —— 那正是想要的：它们是"已经被滚过去"的视口，
 *   渲染它们没有意义。
 */
let viewInFlight = false;
let viewWanted = false;
let viewTs = 0;          // 负数序列：与 waitOp 的正数 ts 天然不撞
let viewSentAt = 0;

/**
 * 视口这一路的小账，host.viewDiag() 读。
 *
 * rtt 是"发出到收到回话"的往返墙钟，work 是 Worker 自报的渲染耗时，pack 是 Worker 自报的
 *   打包耗时（构造那份状态 + 结构化克隆）。回话要带的字段很多，构造与克隆都不算在 work 里，
 *   所以要三段一起看：rtt 减 work 减 pack 剩下的才是"跨线程 + 主线程受理"那一段。
 * handle 是主线程处理这一份状态花的时间（改几何、对画布、定位输入法、重画面板）。
 * coalesced 是"在飞期间被折掉的位置"条数 —— 它就是门闩消掉的排队量；released 是在飞
 *   期间照样发的次数（见 kViewLatchMs），正常应当恒为 0。
 * blit 与 blitPages 是 Worker 自报的"这一拍重贴了几页画布、花了多少毫秒"。挪窗时贴的是
 *   全窗，而真正需要新像素的只有新露出来那一页，两者之差就是白贴的量。
 */
const viewDiag = {
  sent: 0, coalesced: 0, released: 0, settled: 0, canvasMsgs: 0,
  // 画布消息：新建（pageCanvas，每建一个槽一条）与回收。按槽复用之后回收恒为 0 ——
  //   留着它是为了当反证：哪天它不为 0，就说明有地方又在按页建销了。
  canvasNew: 0, canvasDrop: 0,
  // 状态回推的账：按 label 分类计数，外加文档正文那一格的体量（它是最大的一笔，
  //   且每推一份就重来一次）。主线程只读不判。
  label: Object.create(null), textMax: 0, textSum: 0,
  packAvg: 0, packMax: 0, rtt: [], work: [], handle: [], blit: [], blitPages: [],
};
const kViewDiagMax = 512;
function pushSample(arr, v) {
  if (arr.length >= kViewDiagMax) arr.shift();
  arr.push(v);
}

/**
 * 在飞期间的放行上限。它只在回话真的丢了时才有用 —— Worker 那边任何一条视口消息都会
 *   回话（连抛异常都回一条），所以这里只是最后一道保险，取大一点：取小了会在"这一下
 *   真的要 300 ms"的时候放进第二条，队列于是又排起来，而排起来正是要消掉的东西。
 */
const kViewLatchMs = 1000;
// 发出时刻按 ts 记账：放行时会有两条同时在飞，收到哪条就销哪条。
const viewSentAtByTs = new Map();

/**
 * 视口提前量：消息里报的位置沿滚动方向再往前一点。
 *
 * 为什么必须提前：从发出到 Worker 换完窗，合成器并不等主线程 —— 这一趟往返里用户又
 *   走了"速度乘往返"那么远。报当前位置的话，窗口（5 页长）就永远建在用户已经走过的
 *   那些页上：实测 4 秒档（40 页/秒、往返 50 ms）窗口落后可见首页中位 4 页、p90 6 页，
 *   而窗口只有 5 页 —— 可见的那几页于是有一半时间落在窗口外，画布全是给看不见的页建
 *   的（可见页缺画布 46%；2 秒档更差，96%）。
 * 提前量就是那一趟的估计：近期速度乘近期往返，夹在 kViewLeadMaxPages 页以内 ——
 *   夹上界是免得把窗口推到用户前面太远（推过头等于把"后面"变成看不见）。
 * 位置只用来选窗口，不写回 scrollTop（宿主只在 scrollReset 那份一次性信号上写，见
 *   syncScrollFromGeom），所以报一个未来的位置不会与用户打架。
 */
const kViewLeadMaxPages = 4;
const kSpeedIdleMs = 200;
let lastScrollTopPx = -1;
let lastScrollAt = 0;
let speedPxPerMs = 0;
let rttEmaMs = 0;

function trackScrollSpeed(top, t) {
  if (lastScrollTopPx < 0 || t - lastScrollAt > kSpeedIdleMs || t <= lastScrollAt) {
    speedPxPerMs = 0;
  } else {
    const v = (top - lastScrollTopPx) / (t - lastScrollAt);
    speedPxPerMs = speedPxPerMs === 0 ? v : speedPxPerMs * 0.6 + v * 0.4;
  }
  lastScrollTopPx = top;
  lastScrollAt = t;
}

function viewLeadPx() {
  if (!speedPxPerMs || !rttEmaMs || !mirror.geom) return 0;
  const cap = kViewLeadMaxPages * mirror.geom.pageStep;
  return Math.max(-cap, Math.min(cap, speedPxPerMs * rttEmaMs));
}

/**
 * 甩滚停下来之后补一趟"零提前量"的对齐。
 *
 * 提前过的位置只用来选窗口，但它会留在引擎记着的 scrollY 上，而那个值还要用来定位
 *   输入法候选窗与页内控件。停稳之后不回这一趟，候选窗就停在提前过的位置上。
 * 安静 kScrollSettleMs 之内又有滚动事件的话，定时器重排。
 */
const kScrollSettleMs = 200;
let viewSettleTimer = 0;

function armViewSettle() {
  if (viewSettleTimer) clearTimeout(viewSettleTimer);
  viewSettleTimer = setTimeout(() => {
    viewSettleTimer = 0;
    speedPxPerMs = 0;
    viewFireAndForget();
  }, kScrollSettleMs);
}

function viewFireAndForget() {
  if (dead) { viewInFlight = false; return; }
  const t = now();
  const top = stageEl.scrollTop * dprNow();
  trackScrollSpeed(top, t);
  if (viewInFlight && t - viewSentAt < kViewLatchMs) {
    ++viewDiag.coalesced;
    viewWanted = true;
    return;
  }
  if (viewInFlight) ++viewDiag.released;
  viewInFlight = true;
  viewWanted = false;
  viewSentAt = t;
  const ts = --viewTs;
  viewSentAtByTs.set(ts, viewSentAt);
  ++viewDiag.sent;
  const lead = viewLeadPx();
  if (lead !== 0) armViewSettle();
  const vs = viewSize();
  // scrollY 是真实滚动位置，leadPx 是提前量，两者分开发：
  //   连页模式下 scrollY 直接决定内容画在哪儿（页贴到 页号 x 步距 - scrollY），
  //   而提前量只该影响"先装哪几页"。合成一个数的话，滚动时内容会整体跑到实际位置
  //   前面去（最快能差小半页），停稳再弹回来。
  post({ t: 'view', ts, scrollY: Math.round(top), leadPx: Math.round(lead),
         viewW: vs.viewW, viewH: vs.viewH });
}

/**
 * 一条视口消息处理完了（state 或 ack 都算）：飞完之后若期间又滚过，再补发一次。
 *   补发的是"发的那一刻"的位置，所以永远追最新。
 */
function viewSettled(m) {
  const at = viewSentAtByTs.get(m.ts);
  if (at === undefined) return;      // 不在账上：别的命令的 ts，或已销过账
  viewSentAtByTs.delete(m.ts);
  const rtt = now() - at;
  pushSample(viewDiag.rtt, rtt);
  // 往返的近期均值：下一趟的提前量要乘它（见 viewLeadPx）。
  rttEmaMs = rttEmaMs === 0 ? rtt : rttEmaMs * 0.6 + rtt * 0.4;
  pushSample(viewDiag.work, typeof m.workMs === 'number' ? m.workMs : -1);
  if (typeof m.packAvgMs === 'number') {
    viewDiag.packAvg = m.packAvgMs;
    viewDiag.packMax = m.packMaxMs || 0;
  }
  pushSample(viewDiag.blit, typeof m.blitMs === 'number' ? m.blitMs : -1);
  pushSample(viewDiag.blitPages, typeof m.blitPages === 'number' ? m.blitPages : -1);
  ++viewDiag.settled;
  if (m.ts !== viewTs) return;       // 放行过的那条旧消息：销账即可，门闩归新的那条
  viewInFlight = false;
  if (viewWanted) viewFireAndForget();
}

/* 每页一张画布（按槽复用） */

/**
 * 造一个槽元素。
 *
 * 为什么按槽而不是按页号：transferControlToOffscreen 是一次性的，交出去的画布收不回来，
 *   而"一页一张、随窗口建销"会让每一次换窗都新建一批元素、转移一批、在新背衬上首画一次。
 *   槽的个数有硬上限（窗口页数上限，至多 8），把元素与背衬钉在槽上就都能复用 ——
 *   换窗只是"哪个槽画哪一页"变了，元素一个不建、一个不删。
 *   反过来按页号长期缓存是不行的：一页背衬就是 3.4 MB，全文 160 页是 550 MB。
 */
function acquireCanvas() {
  const el = document.createElement('canvas');
  el.className = 'pg';
  el.style.position = 'absolute';
  el.style.display = 'block';
  stageEl.appendChild(el);
  return el;
}

/**
 * 把一个槽的画布交给 Worker。
 *   转移列表必须带上 —— 不带会抛 DataCloneError（OffscreenCanvas 不能按值克隆），
 *   那个错只在"新增槽"时才露头，页面表现是那几页一直空白。
 */
function sendPageCanvas(slot, el, gm, slots) {
  const off = el.transferControlToOffscreen();
  ++viewDiag.canvasMsgs;
  ++viewDiag.canvasNew;
  // slots 是本宿主实际的槽数，引擎用它当槽号模数 —— 不再自己按几何现算。
  //   两侧各算一次的做法在"视口从很高变矮"时会分叉：宿主建的槽不回收，
  //   引擎却会回落到下限，槽号整体错位、画布上是别的页。
  post({ t: 'pageCanvas', ts: now(), slot, slots, canvas: off, dpr: gm.dpr,
         viewW: gm.viewW, viewH: gm.viewH }, [off]);
}

/**
 * 连页模式的视口画布：整个视口一张画布，内容按滚动位置重贴（见 Worker 侧的 blitPageContinuous）。
 *
 * 它挂在页容器外面（stage 的父级），因为放在滚动内容里就会随内容一起滚 ——
 *   那样"贴"这件事在屏幕上根本看不出来。指针事件仍由页容器接（这张画布 pointer-events: none），
 *   而页号在连页模式下由坐标反算（见 onPointerDown）—— 这里没有"页画布"可以问。
 */
let viewCanvas = null;
let viewContinuous = false;

function syncViewCanvas(gm) {
  const dpr = gm.dpr || 1;
  const vs = viewSize();
  const host = stageEl.parentElement;
  const sr = stageEl.getBoundingClientRect();
  const hr = host.getBoundingClientRect();
  if (!viewCanvas) {
    viewCanvas = document.createElement('canvas');
    viewCanvas.style.position = 'absolute';
    viewCanvas.style.zIndex = '1';
    viewCanvas.style.pointerEvents = 'none';
    host.appendChild(viewCanvas);
    // 只交一次：之后换窗、改尺寸都在这张上做（OffscreenCanvas 交出去就收不回来）。
    //   转移列表必须带上，理由与 sendPageCanvas 那条一样 —— 不带会抛 DataCloneError
    //   （OffscreenCanvas 不能按值克隆），而这一处抛在"状态到达"的路上，会被宿主当成
    //   致命错误整页停用（横幅：宿主处理引擎状态时抛异常）。
    const off = viewCanvas.transferControlToOffscreen();
    post({ t: 'viewCanvas', ts: now(), continuous: true, gap: 0, dpr, canvas: off }, [off]);
  }
  const w = Math.round(gm.pageW / dpr);
  const h = Math.round(vs.viewH / dpr);
  const left = Math.round((vs.viewW - gm.pageW) / 2 / dpr);
  const top = Math.round(sr.top - hr.top);
  const px = (v) => v + 'px';
  if (viewCanvas.style.width !== px(w)) viewCanvas.style.width = px(w);
  if (viewCanvas.style.height !== px(h)) viewCanvas.style.height = px(h);
  if (viewCanvas.style.left !== px(left)) viewCanvas.style.left = px(left);
  if (viewCanvas.style.top !== px(top)) viewCanvas.style.top = px(top);
  lastPageX = Math.max(0, Math.round((vs.viewW - gm.pageW) / 2));
  // 连页模式没有页画布：这两张表按空报，探针据此走"读不到像素"的分支。
  pageCanvases.clear();
  host.pageCanvasCount = 0;
}

/**
 * 让槽与"当前窗口"对齐：第 p 页永远用第 p 模槽数 个槽。
 *
 * 为什么按模取而不是"窗口里第几个位置"：后者在窗口平移时会让每个槽都换一页，
 *   于是每次换窗都得整窗重贴（实测 99 次换窗、每次中位 6.9 ms，合往返的 22%）。
 *   按模取之后，窗口平移只让"新进窗口的那几页"落到旧内容的槽上，其余槽里的页
 *   一动没动、像素本来就是对的。
 * 页号到元素的映射（pageCanvases）仍是每个状态重建的派生量 —— 下游二十来处读点
 *   （命中测试、诊断、探针）都按页号读，这样它们一行都不用改。
 * 八个槽一次建齐（首帧那一趟），此后元素与背衬全留着，换窗只是改几行 style。
 */
function syncPageCanvases(gm) {
  if (viewContinuous) return syncViewCanvas(gm);
  const dpr = gm.dpr || 1;
  const first = gm.first;
  const count = gm.count;
  const step = gm.pageStep;

  // 槽数 = 窗口页数上限，由 worker 算好随几何报来（见那里的 maxWindowPages）。
  //   不用"max(8, 视口装得下几页)"：那个下限 8 既大于窗口页数（本机窗口 5 页，白占 3 个槽），
  //   又与引擎的口径无关 —— 引擎的帧槽将来用的是同一个上限，两边必须是同一个数。
  //   窗口页数本来就是由这个上限截出来的，所以它一定装得下当前窗口；
  //   仍取 max(上限, 当前页数) 作下限 —— 门面允许外部把窗口页数指到上限之外（探针会如此），
  //   那时槽少了会让两页共用一个槽、那几页永远缺画布。
  const nSlots = Math.max(1, (gm.maxWindowPages | 0) || 1, count);
  // 一次建齐到 nSlots：慢慢长会让"哪一页在哪个槽里"在中途改一次口径，而那正是要避免的。
  // 先把槽建齐、再逐个转移：转移要带"最终槽数"，边建边发会让中途那几条带的是
  //   1、2、3…… 引擎每收到一条就得把槽表整体作废一次。
  const firstNew = slotCanvases.length;
  while (slotCanvases.length < nSlots) slotCanvases.push(acquireCanvas());
  for (let k = firstNew; k < slotCanvases.length; ++k) {
    sendPageCanvas(k, slotCanvases[k], gm, slotCanvases.length);
  }

  const vs = viewSize();
  lastPageX = Math.max(0, Math.round((vs.viewW - gm.pageW) / 2));
  const left = Math.round(lastPageX / dpr) + 'px';
  const w = Math.round(gm.pageW / dpr) + 'px';
  const h = Math.round(gm.pageH / dpr) + 'px';

  pageCanvases.clear();
  const used = new Set();
  for (let i = 0; i < count; ++i) {
    const p = first + i;
    const s = slotOf(p, slotCanvases.length);
    const el = slotCanvases[s];
    if (!el) continue;
    used.add(s);
    pageCanvases.set(p, el);
    const pStr = String(p);
    // 指针事件靠 dataset.page 认页（见 onPointerDown），换窗必须跟着改。
    if (el.dataset.page !== pStr) el.dataset.page = pStr;
    if (el.style.display === 'none') el.style.display = 'block';
    // 显示尺寸每推一次状态都要对一遍，不能只在建槽时设一次。转移之后这个元素的
    //   width/height 属性就作废了：背衬尺寸由 Worker 按帧的页宽页高定，CSS 尺寸只有
    //   这里能改。漏掉的表现是缩放之后"按新的倍率画进旧的显示尺寸"——整页偏小兼发虚。
    if (el.style.width !== w) el.style.width = w;
    if (el.style.height !== h) el.style.height = h;
    if (el.style.left !== left) el.style.left = left;
    const top = Math.round((p * step) / dpr) + 'px';
    if (el.style.top !== top) el.style.top = top;
  }
  // 这一窗没用到的那几个槽藏起来。窗口里连续不超过 8 页 ⇒ 页号模 8 互不相同 ⇒
  //   一个槽不会被两页同时认领。
  for (let s = 0; s < slotCanvases.length; ++s) {
    if (!used.has(s) && slotCanvases[s].style.display !== 'none') {
      slotCanvases[s].style.display = 'none';
    }
  }
  host.pageCanvasCount = pageCanvases.size;
}

/* 接收 */

function onWorkerError(m) {
  const brief = String(m.message).split('\n')[0];
  errors.push(m.where + ': ' + m.message);
  if (barEl) {
    barEl.textContent = 'Worker 报错 ' + m.where + '：' + brief;
    barEl.className = 'err';
  }
  /**
   * 两档判据：报错的来源决定"引擎还能不能用"。
   *
   *   worker.onerror  ：Worker 脚本自身出错（语法错误、未捕获异常）—— 整条不可用。
   *   boot 开头（boot / boot:layout）：装载就失败了（胶水取不到、实例化抛异常、
   *     首帧排版失败）—— 引擎没有可显示的内容，也是不可用。
   *   其余（do / ask / view 的某一条）：那一条命令失败，引擎还活着，换一条照常能用。
   */
  const fatal = m.where === 'worker.onerror' || /^boot/.test(m.where);
  if (fatal) {
    dead = true;
    report.fatal(m.where === 'worker.onerror' ? '引擎 Worker 崩溃' : '引擎装载失败', {
      detail: brief,
      hint: '这一页的编辑器已经不可用；重新加载页面会重建引擎。',
      retry: () => location.reload(),
    });
  } else {
    report.warn(`命令 ${m.where} 失败：${brief}`);
  }
  // 出错的那一拍不会再有 state / ack 回来 —— 把还在等的挂起调用当场拒掉。
  //   不这样做的话调用方只能等到超时，而真正的错误信息就在手边（症状是
  //   "视口超时"这种把根因藏起来的报错，最费时间）。
  rejectPending(`Worker 报错 ${m.where}：${brief}`);
}

/**
 * 宿主自己处理消息时抛的异常也要浮出来。
 *
 * 消息处理器里抛异常是无声的：既不会进 window.onerror 的那套收集，也不会回给
 *   调用方 —— 表现成"调用一直等不到回话然后超时"，而真相（比如某个 DOM 操作抛了）
 *   完全看不到。实测就是被这一条误导了一轮：Worker 把 state 推过来了、帧也重画了，
 *   宿主在解析时抛掉，调用方只看到超时。
 *
 * 这一档按致命处理：宿主处理不了引擎推来的状态，意味着画面已经和引擎对不上，
 *   继续让它看起来可用只会让人以为"编辑没生效"，那是更难查的一种失败。
 */
function rejectPending(reason) {
  const e = new Error(reason);
  for (const [id, p] of [...pendingState]) { pendingState.delete(id); p.reject(e); }
  for (const [id, p] of [...pendingReply]) { pendingReply.delete(id); p.reject(e); }
}

function onPageError(where, e) {
  const msg = String((e && e.stack) || e);
  const brief = String(msg).split('\n')[0];
  errors.push(`${where}: ${msg}`);
  if (barEl) {
    barEl.textContent = `宿主 ${where} 抛异常：${brief}`;
    barEl.className = 'err';
  }
  dead = true;
  report.fatal('宿主处理引擎状态时抛异常', {
    detail: brief,
    hint: '画面已与引擎不一致，这一页的编辑器不再可用。',
    retry: () => location.reload(),
  });
  rejectPending(`宿主 ${where} 抛异常：${brief}`);
}

function syncSpacer(st, gm) {
  const total = Math.max(1, Number(st.totalPages) || 1);
  const pageCss = Math.round(gm.pageW / dprNow());
  const bx = Math.round(lastPageX / dprNow()) + 'px';
  // 滚动高度：分页是"页数 x 步距"（页是实在的，末尾那一格页间隙不算）；
  //   连页没有页可数，只有内容 —— 取引擎报的内容底边（contentPx）。
  //   用页数算的话，末尾会多出一整条空条带的空白（内容排到哪算哪，条带却是整格）。
  const h = viewContinuous
    ? Math.max(1, Math.round((gm.contentPx || 0) / dprNow()))
    : Math.max(1, (total * gm.pageStep - gm.gap) / dprNow());
  if (spacerEl.style.height !== h + 'px') spacerEl.style.height = h + 'px';

  // 页纸底：让"还没渲染出来的页"也是一张白纸，而不是什么都没有。
  //   为什么必须有、为什么用 CSS 而不是多建画布，与单线程宿主的 syncPageBackdrop
  //   同一条理由（甩滚时引擎追不上，可见区域里既没有字也没有画布，看起来像坏了）。
  //   这里挂在撑高层上：画布是绝对定位在它外面的，所以还要按 lastPageX 把底图
  //   横向摆到页的位置上，宽度取页宽 —— 否则白纸会横铺满整个舞台。
  //   连页下没有页间隙可分隔，就是一条从顶到底的白列（背景块宽度仍取页宽）。
  const stepCss = Math.round(gm.pageStep / dprNow());
  const hCss = Math.round((gm.pageStep - gm.gap) / dprNow());
  const img = viewContinuous
    ? 'linear-gradient(#ffffff, #ffffff)'
    : `linear-gradient(to bottom, #ffffff 0 ${hCss}px,` +
      ` rgba(255,255,255,0) ${hCss}px ${stepCss}px)`;
  const size = viewContinuous ? `${pageCss}px 100%` : `${pageCss}px ${stepCss}px`;
  const repeat = viewContinuous ? 'no-repeat' : 'repeat-y';
  if (spacerEl.style.backgroundImage !== img) {
    spacerEl.style.backgroundImage = img;
    spacerEl.style.backgroundSize = size;
    spacerEl.style.backgroundRepeat = repeat;
  }
  if (spacerEl.style.backgroundPosition !== bx + ' 0px') {
    spacerEl.style.backgroundPosition = bx + ' 0px';
  }
}

/**
 * 帧槽号：页在帧内的位置由引擎的槽数决定（页号 mod 槽数）。
 *   与单线程宿主的 frameSlotOf 同一口径 —— 光标坐标里的 y 含的就是这个槽偏移。
 */
function frameSlotOf(page) {
  const n = host.frameSlots();
  if (n <= 0) return 0;
  return ((page % n) + n) % n;
}

function positionIme(st, gm) {
  const inWindow = st.caretPage >= gm.first && st.caretPage < gm.first + gm.count;
  if (!st.caretW || !inWindow) {
    ta.style.width = '1px';
    ta.style.height = '1px';
    return;
  }
  const lineH = Math.max(12, Math.round(st.caretH / dprNow()));
  ta.style.left = Math.round((lastPageX + st.caretX) / dprNow()) + 'px';
  // caretY 是"帧内的 y"，含的是槽偏移（第 caretPage 页落在哪个槽），不是"窗口里排第几页"。
  //   所以文档坐标 = 页号 x 步距 + 页内 y，而页内 y = caretY - 槽偏移。写成"窗口首页 x 步距
  //   + caretY"在窗口首页不是槽数整数倍时会把框摆到别的页上去（与命中测试那次同类）。
  //   连页与分页在这里是同一个式子：分页下画布在文档流里、连页下视口画布不动而内容按
  //   滚动量贴，两者都等价于"文档坐标减滚动量"。
  const local = st.caretY - frameSlotOf(st.caretPage) * gm.pageStep;
  ta.style.top =
    Math.round((st.caretPage * gm.pageStep + local) / dprNow() - stageEl.scrollTop) + 'px';
  ta.style.width = Math.max(24, Math.round((gm.pageW - st.caretX) / dprNow())) + 'px';
  ta.style.height = lineH + 'px';
  ta.style.lineHeight = lineH + 'px';
  ta.style.fontSize = Math.max(10, Math.round(lineH * 0.62)) + 'px';
}

function paintPanel() {
  const st = mirror.stats;
  const gm = mirror.geom;
  if (!st || !gm || !statEl) return;
  const rows = [
    ['页数', st.totalPages],
    ['字形', st.glyphCount],
    ['缺字', st.missingGlyphCount],
    ['光标', `第 ${st.caretPara} 段 / 偏移 ${st.caretOffset}`],
    ['窗口', `${gm.first} + ${gm.count} 页（页画布 ${host.pageCanvasCount} 张）`],
    ['帧', `${gm.frameW}×${gm.frameH}，每张 ${gm.pageW}×${gm.pageH}`],
    ['排版', `${st.layoutMs.toFixed(2)} ms`],
    ['上屏', `${st.paintMs.toFixed(2)} ms`],
    ['Worker 堆', mirror.heapMB.toFixed(1) + ' MB'],
    ['字体模式', mirror.fontMode],
    ['上一拍', mirror.lastLabel + ' ' + mirror.lastMs.toFixed(2) + ' ms'],
  ];
  statEl.innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join('');
}

/**
 * 让 DOM 的滚动位置跟随引擎 —— 只在引擎自己把滚动归零时（装载文档、换缩放）。
 *
 * 单线程宿主是在同一线程里直接写 stageEl.scrollTop = 0（presetWindowForLoad），这条
 *   通道在 Worker 化之后必须补出来。不补的症状不是"看起来没滚"，而是画布与点击
 *   坐标全对不上：代理画布的 rect 按引擎的 scrollY 算，浏览器却在另一个位置命中，
 *   于是"点字段框里的任意一点"全部落到别处（错得整整齐齐，不好往这儿想）。
 *
 * 靠的是 Worker 送来的显式信号（state.scrollReset），不是"两边不一致就追平"。
 *   后者是错的：追平要写 scrollTop，而写它会触发 scroll 事件、再发一条视口消息回来，
 *   与正在滚动的用户来回打架 —— 窗口反复变、画布反复建与丢，实测后果是跨页滚动之后
 *   某一页始终拿不到画布（逐像素比对时"比了 0 px、缺页 1"）。
 */
function syncScrollFromGeom(gm, reset) {
  if (!reset || !gm || typeof gm.scrollY !== 'number') return;
  stageEl.scrollTop = Math.round(gm.scrollY / dprNow());
}

function onState(m) {
  mirror.stats = m.stats;
  mirror.layout = m.layout || mirror.layout;
  mirror.geom = m.geom;
  mirror.text0 = m.text0;
  mirror.controls = m.controls;
  mirror.blockedEdits = m.blockedEdits;
  mirror.dirtyPages = m.dirtyPages;
  mirror.caretReport = m.caretReport;
  mirror.selection = m.selection || mirror.selection;
  // 样式现状：与 selection 同一条路（随 state 推过来），所以宿主侧读它也是同步的。
  mirror.caretStyle = m.caretStyle || mirror.caretStyle;
  mirror.selectionStyle = m.selectionStyle || mirror.selectionStyle;
  mirror.caretLive = m.caretLive;
  mirror.heapMB = m.heapMB;
  mirror.fontMode = m.fontMode;
  mirror.stateSeq += 1;
  syncSpacer(m.stats, m.geom);
  syncScrollFromGeom(m.geom, m.scrollReset);
  syncPageCanvases(m.geom);
  positionIme(m.stats, m.geom);
  const p = pendingState.get(m.ts);
  if (p) {
    pendingState.delete(m.ts);
    mirror.lastMs = p.tt ? now() - p.tt : 0;
    mirror.lastLabel = m.label;
    p.resolve(m);
  }
  // 首帧一到就把闪烁起来 —— 单线程宿主是在装载收尾里调 resetBlink 的，这里没有那个
  //   收尾点，所以挂在"第一份状态"上。只自动起一次：否则 stopBlink 之后随便一次
  //   状态推送又把它拉起来（探针里那句 stopBlink 就白调了）。
  if (!blinkAuto) { blinkAuto = true; resetBlinkPhase(); }
  paintPanel();
}

function onMessage(ev) {
  try {
    return handleMessage(ev.data || {});
  } catch (e) {
    onPageError('onMessage', e);
  }
}

function handleMessage(m) {
  if (m.t === 'ready') {
    mirror.info = m;
    mirror.heapMB = m.heapMB;
    mirror.fontMode = m.fontMode;
    // ready 一到就算起来了，不等第一帧：装载里抛异常时"永远等不到 ready"会把主线程
    //   变成静默挂起，而这个页面是拿来验证的 —— 挂住等于什么都没验到。
    host.ready = true;
    if (barEl) {
      barEl.textContent =
        `引擎在 Worker 里 · 启动 ${m.bootMs.toFixed(0)} ms（字体 ${m.fontMs.toFixed(0)} ms）· ` +
        `帧零拷贝 ${m.zeroCopyFrame} · 一页一张画布`;
    }
    host.readyWallMs = now() - bootLead;
    return;
  }
  if (m.t === 'state') {
    viewSettled(m);
    // 主线程受理这一份状态花的时间。与"往返减 work"不是一回事，但要一起看：那边缺口大
    //   而这边耗时小，说明时间花在路上（打包与跨线程），不是花在主线程的处理上。
    const tHandle = now();
    onState(m);
    // 这份状态是谁推的、正文那一格多大。正文随每份状态整体重来一次，所以它既是
    //   每份的固定成本、也随文档长度线性涨（见 viewDiag 的注释）。
    const lbl = m.label || '(空)';
    viewDiag.label[lbl] = (viewDiag.label[lbl] || 0) + 1;
    if (typeof m.text0 === 'string') {
      viewDiag.textSum += m.text0.length;
      if (m.text0.length > viewDiag.textMax) viewDiag.textMax = m.text0.length;
    }
    pushSample(viewDiag.handle, now() - tHandle);
    return;
  }
  // 视口消息在"窗口没变"时只回一个 ack：那一下 Worker 什么都没做（浏览器已经把
  //   画布搬走了），所以没有新状态可推。宿主拿到 ack 就把 Promise 收掉。
  if (m.t === 'ack') {
    viewSettled(m);
    const p = pendingState.get(m.ts);
    if (p) {
      pendingState.delete(m.ts);
      mirror.lastMs = p.tt ? now() - p.tt : 0;
      mirror.lastLabel = 'ack:窗口没变';
      p.resolve(m);
    }
    return;
  }
  if (m.t === 'reply') {
    const p = pendingReply.get(m.id);
    if (!p) return;
    pendingReply.delete(m.id);
    if (m.error) p.reject(new Error(m.error));
    else p.resolve(m.value);
    return;
  }
  if (m.t === 'error') return onWorkerError(m);
}

/* 输入层见 web/host_core.js：键位映射、命中归宿、连击、组合提交都在那里。 */

/* 门面 */

/**
 * 命令表：名字与单线程宿主一致，参数在这里翻译成 Worker 的运算名。
 *   写成表而不是一个个手写方法，是为了让"哪些是命令"一眼可数。
 */
const CMD = {
  present: () => ['present', {}],
  type: (s) => ['type', { s }, true],
  backspace: () => ['backspace', {}, true],
  deleteForward: () => ['deleteForward', {}, true],
  splitParagraph: () => ['splitParagraph', {}, true],
  mergeWithPreviousParagraph: () => ['mergePrev', {}, true],
  mergeWithNextParagraph: () => ['mergeNext', {}, true],
  move: (dcp, extend) => ['move', { dcp: dcp || 1, extend: !!extend }, true],
  // 上下键与文档首尾：与 move 同一档（引擎的 *AndRender 版自己收尾）。
  vertical: (dir, extend) => ['vertical', { dir: dir || 1, extend: !!extend }, true],
  docStart: (extend) => ['docStart', { extend: !!extend }, true],
  docEnd: (extend) => ['docEnd', { extend: !!extend }, true],
  lineStart: (extend) => ['lineStart', { extend: !!extend }, true],
  lineEnd: (extend) => ['lineEnd', { extend: !!extend }, true],
  // 选区三个入口：两端是全局流位置（不是段内偏移）。
  selectAll: () => ['selectAll', {}, true],
  // 整格选区：区间来自 hit 的 cell 字段。与 setSelection 的差别只在粒度。
  selectCellAtPara: (para) => ['selectCellAtPara', { para }, true],
  setSelection: (anchor, focus) => ['setSelection', { anchor, focus }, true],
  clearSelection: () => ['clearSelection', {}, true],
  setCaret: (para, offset) => ['setCaret', { para, offset }],
  setCaretAt: (para, offset, field) => ['setCaretAt', { para, offset, field }],
  setCaretAtSide: (para, offset, side) => ['setCaretAtSide', { para, offset, side }],
  // 带行别的版本：折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
  //   只有行别分得开（见 EditorEngine::caretLineAffinity）。
  setCaretAtSideLine: (para, offset, side, line) =>
    ['setCaretAtSideLine', { para, offset, side, line }],
  // 拖选的移动端：只搬焦点端，anchor 留在按下点。与 setCaretAtSide 同一档
  //   （引擎不自己重绘，Worker 侧在 PRESENT_ALWAYS 里主动 present）。
  setFocusAtSide: (para, offset, side) => ['setFocusAtSide', { para, offset, side }],
  setFocusAtSideLine: (para, offset, side, line) =>
    ['setFocusAtSideLine', { para, offset, side, line }],
  setCaretOnly: (para, offset) => ['setCaret', { para, offset }],
  indent: () => ['indent', {}, true],
  outdent: () => ['outdent', {}, true],
  tabCaret: (backward) => ['tabCaret', { backward: !!backward }, true],
  undo: () => ['undo', {}, true],
  redo: () => ['redo', {}, true],
  setUndoLimit: (n) => ['setUndoLimit', { n }],
  setEditMode: (m) => { mode = Number(m) | 0; return ['setEditMode', { mode }]; },
  setBracketScope: (s) => ['setBracketScope', { scope: Number(s) | 0 }],
  // 光标与选区的样式。参数整块递过去，缺的字段由 Worker 侧按现状补 —— 与单线程宿主
  //   门面同一套补全规则。返回的是 editStats（那边 setEditMode 也是），不是样式本身。
  setCaretStyle: (style) => ['caretStyle', { style: style || {} }],
  setSelectionStyle: (style) => ['selectionStyle', { style: style || {} }],
  setFormControls: (list) => ['setFormControls', { list: list || [] }],
  loadFormSample: (variant) => ['formSample', { variant }],
  loadStressDocument: (paras, chars) => ['stress', { paras, chars }],
  loadStressDocumentDiverse: (paras, chars) => ['stressDiverse', { paras, chars }],
  loadTextDocument: (text, repeat) => ['textDoc', { text, repeat }],
  run: (id, z) => {
    scenario = Number(id);
    if (z) { setZoomDensity(z); syncZoomInput(); }
    return ['scenario', { id: scenario, zoom: z }];
  },
  reset: (id, z) => {
    scenario = id === undefined ? 6 : Number(id);
    setZoomDensity(z === undefined ? baseDensity() : z);
    syncZoomInput();
    return ['reset', { id: scenario, zoom }];
  },
  reloadFonts: (m) => ['fonts', { mode: m || 'full' }],
  setFontCacheConfig: (blockSize, blocks) => ['setFontCacheConfig', { blockSize, blocks }],
  setViewportAnchor: (first, count) => ['viewport', { first, count }],
  // 每条改缩放的消息都捎上当前 DPR：引擎回推的状态里带着它，宿主拿它折算 CSS 尺寸，
  //   窗口换过显示器之后不回传就会按旧值折算，画布的显示尺寸与背衬差一个比例。
  zoomTo: (z) => { setZoomDensity(z); syncZoomInput(); return ['zoom', { zoom, dpr: dprNow() }]; },
  // 按倍数设缩放（界面输入框的入口）。门面其余成员收的都是密度。
  zoomToFactor: (f) => { setZoomDensity(engineZoomOf(f)); syncZoomInput(); return ['zoom', { zoom, dpr: dprNow() }]; },
  // 只改引擎里的相位，不呈现 —— 与单线程宿主门面的 showCaret 同语义
  //   （那边就是 mod.showCaret(!!on) 一行；探针常在这之后自己调 present()）。
  showCaret: (on) => ['phase', { on: !!on }],
  focusControl: (forward) => ['focusControl', { forward: forward === undefined ? true : !!forward }, true],
  removeControl: (i, keepContent) => ['removeControl', { i, keepContent: !!keepContent }, true],
  setControlProp: (i, prop, value) => ['setControlProp', { i, prop, value: !!value }, true],
};

for (const name of Object.keys(CMD)) {
  host[name] = (...a) => {
    const r = CMD[name](...a);
    // 走 opUser（不是 doOp）：每条用户命令跑完都把闪烁相位归到可见。
    //   r[2] = 这条命令是否"没改动就返回 null"（见 doOp 的说明）。
    return opUser(r[0], r[1], r[2]);
  };
}

// bracketSlots 与 allText 同理：单线程宿主那边是取值器（同步取得到槽清单），探针写的是
//   不带括号的形式。异步宿主给不出同步值，所以做成"取值器 + 值为 Promise"，
//   靠探针 eval 的 awaitPromise 把不带括号的读法撑住。
Object.defineProperty(host, 'bracketSlots', {
  get: () => askOp('bracketSlots', {}),
});

/**
 * 落光标的这几个入口返回的是光标本身，不是 editStats。
 *   对照单线程宿主：setCaretAndPresent 末尾是 return this.caret;，而探针紧接着就读
 *   cr0.x / cr0.page（拿它算点击坐标）。返回形状也是门面契约的一部分 ——
 *   形状不对时症状是一条 "clientX 不是有限数"，完全看不出是返回值的问题。
 */
for (const name of ['setCaret', 'setCaretAt', 'setCaretAtSide', 'setCaretAtSideLine',
                    'setFocusAtSide', 'setFocusAtSideLine', 'setCaretOnly']) {
  const spec = CMD[name];
  host[name] = (...a) => {
    const r = spec(...a);
    return opUser(r[0], r[1]).then((st) => (st ? host.caret : null));
  };
}

// reloadFonts 返回的是 { mode, rows }（app.js: return { mode: fontMode, rows: r.rows }），
//   不是 editStats。rows 只是启动面板那几行，这一侧给空数组 —— 探针只取 mode。
{
  const spec = CMD.reloadFonts;
  host.reloadFonts = (m) => {
    const r = spec(m);
    return waitOp('重装字体', { t: 'do', name: r[0], args: r[1] })
      .then((msg) => ({ mode: msg && msg.fontMode, rows: [] }));
  };
}

// setUndoLimit 返回的是新的上限（app.js 写的是 return this.undoLimit();），不是 editStats。
//   这类"返回一个标量"的入口也要按契约来，否则探针那条 === 100 永远不成立。
{
  const spec = CMD.setUndoLimit;
  host.setUndoLimit = (n) => {
    const r = spec(n);
    return opUser(r[0], r[1]).then(() => host.undoLimit());
  };
}

// 要返回值的查询：一律返回 Promise。
const ASK = {
  paragraphText: (i) => ['text', { i: i | 0 }],
  hit: (x, y) => ['hit', { x, y }],
  controlAt: (x, y) => ['controlAt', { x, y }],
  describeControls: () => ['describeControls', {}],
  auditFrame: () => ['auditFrame', {}],
  auditBase: () => ['auditBase', {}],
  auditLayout: () => ['auditLayout', {}],
  auditCompile: () => ['auditCompile', {}],
  auditGlyphPages: () => ['auditGlyphPages', {}],
  bench: (runs) => ['bench', { runs }],
  undoLimit: () => ['undoLimit', {}],
  selfCheck: () => ['selfCheck', {}],
  markCheck: () => ['markCheck', {}],
  ink: () => ['ink', {}],
  inkRows: () => ['inkRows', {}],
  frameDigest: () => ['frameDigest', {}],
  lines: (from, to) => ['lines', { from: from | 0, to: to | 0 }],
  engineStats: () => ['stats', {}],
  engineEditStats: () => ['editStats', {}],
  // 预热一页的字形（不呈现、不改画面）。与单线程宿主同名入口，形状是 Promise ——
  //   引擎在 Worker 那侧，异步是本质差异（与 allText 同一条说明）。
  prewarmPage: (page) => ['prewarmPage', { page: page | 0 }],
};
for (const name of Object.keys(ASK)) {
  host[name] = (...a) => {
    const r = ASK[name](...a);
    return askOp(r[0], r[1]);
  };
}
host.ask = askOp;

/**
 * 三击选整格：坐标是文档磅，与 hit 同一个口径；返回是否真的选中了一格。
 *
 * 走命令而不是查询（所以不在上面的 ASK 表里）：它会重画帧，而查询那条路只回一个值、
 *   不重贴脏页 —— 画面会停在旧帧上，直到下一次闪烁相位才更新（见 onPointerDown）。
 *   宿主只关心"成没成"，这里把 changed 折成布尔，与单线程宿主同名入口同形。
 */
host.cellSelectAt = async (x, y) => !!(await opUser('cellSelectAt', { x, y }, true));

/**
 * 整篇文本。一次问一段太多趟，一次传整篇又要有那个口子 —— 所以逐段问再拼。
 *
 * 做成取值器，值是一个 Promise，而不是方法：单线程宿主那边 allText 就是取值器
 *   （同步取得到整篇），探针写的是 window.__heditor.allText 不带括号。异步宿主给不出
 *   同步的整篇文本，这是本质差异 —— 但探针的 eval 带 awaitPromise，所以"取值器返回
 *   Promise"能让那几处不带括号的读取原样成立。唯一要改的是把它再套进 Array.from 的那
 *   两处（那里必须显式 await）。
 */
Object.defineProperty(host, 'allText', {
  get: () => (async () => {
    const es = await askOp('editStats', {});
    const total = Math.max(1, Number(es.totalParagraphs) || 1);
    const parts = [];
    for (let i = 0; i < total; ++i) parts.push(await askOp('text', { i }));
    return parts.join('\n');
  })(),
});

/**
 * 命中并定位（内部共用）。返回命中结果本身，没命中给 null。
 *
 * 坐标口径：xDev / yDev 是文档坐标（设备像素）—— x 从页左缘量，y 从文档第 0 页顶量。
 *   命中测试收的就是这个坐标系，所以折磅（除以 zoom）与三种归宿（整格 / 带侧别与行别 /
 *   普通落点）都在 host_core 里判。这里只剩一层壳，给门面的 clickAt 用。
 */
function placeAt(xDev, yDev) {
  return input.placeAt({ x: xDev, y: yDev });
}

/**
 * 切换视图模式：分页（一页一张画布，默认）与连页（一个视口画布）。
 *
 * 两条路的差别只有"贴到哪儿"（见 syncViewCanvas 与 Worker 侧的 blitPageContinuous），
 *   引擎那一侧完全不动 —— 帧、槽、增量重画、字形二分、编译增量都是按文档算的，
 *   与画布几张无关。页间隙随之切换：连页要 0（页首尾相接），分页回到 14 磅。
 *
 * 切换必须重来一遍画布：OffscreenCanvas 交出去就收不回来，所以每次切换都把旧的丢掉、
 *   让下一次 syncPageCanvases 按新模式重新建。
 */
host.setViewMode = function setViewMode(mode) {
  const want = mode === 'continuous';
  if (want === viewContinuous) return;
  viewContinuous = want;
  for (const cv of slotCanvases) cv.remove();
  slotCanvases.length = 0;
  pageCanvases.clear();
  if (viewCanvas) {
    viewCanvas.remove();
    viewCanvas = null;
  }
  // 回分页要显式说一声：Worker 那边还留着连页标志与视口画布，不说它就会继续往一张
  //   已经从 DOM 里摘掉的画布上贴 —— 表现是切回去之后画面停在最后一拍。
  //   连页方向不用在这里说：画布那条消息（syncViewCanvas）带着标志与间隙一起过去。
  //   间隙与模式同源（连页 0 = 页首尾相接，分页 14），所以它跟着这条消息走。
  if (!want) post({ t: 'viewCanvas', ts: now(), continuous: false, gap: 14 });
  viewFireAndForget();
};
Object.defineProperty(host, 'viewMode',
                      { get: () => (viewContinuous ? 'continuous' : 'paged') });

host.clickAt = async function clickAt(xPx, yPx) {
  const hit = await placeAt(xPx, yPx);
  return hit ? host.caret : mirror.stats;
};

/**
 * 客户端坐标 -> 文档坐标（设备像素）。
 *
 * 与单线程宿主的 framePointAt 同一口径，只是基准换成滚动容器：页在滚动内容里是绝对
 *   定位（top = 页号 × 步距），所以"某点的文档偏移" = 容器内偏移 + 滚动量。这样换算
 *   不依赖"点在哪一张页画布上" —— 拖选要按夹到容器边缘的点重新命中，那时没有目标元素。
 *   也不依赖窗口开在哪里：命中测试收的就是文档坐标（帧怎么摆是渲染的私事）。
 */
function framePointAt(clientX, clientY) {
  if (!mirror.geom) return null;
  const r = stageEl.getBoundingClientRect();
  return {
    x: (clientX - r.left) * dprNow() - lastPageX,
    y: (clientY - r.top) * dprNow() + stageEl.scrollTop * dprNow(),
  };
}

/**
 * 拖选拖动中那一端：只搬焦点端，anchor 留在按下点。
 *
 * 每一步都按住光标可见（与按下那一刻同一件事）：拖拽期间光标必须一直亮着 ——
 *   不按住的话 530 ms 的闪烁计时器照常翻转，拖的过程中光标一亮一灭。
 *   单线程宿主是同一口径：那边每一步都显式 showCaret(true) 再呈现。
 */
function dragExtendTo(hit) {
  blinkHold();
  host.setFocusAtSideLine(hit.para, hit.offset, hit.side, hit.line | 0);
}

/* 帧画布代理（给探针的适配层） */

/**
 * 把"窗口帧"当成一张虚拟画布暴露出去：几何按窗口帧算，事件转发到真正的页画布上。
 *
 * 为什么要有它：帧在本宿主里是按页切成多张画布的，而探针（与单线程宿主那套断言）
 *   是按"整帧一张画布"写的 —— 它要 canvas.getBoundingClientRect() 把帧磅坐标换成
 *   客户端坐标，再派 mousedown。单线程宿主的 app.js 里有一个同名同形的代理，注释原话
 *   是"老探针因此一行都不用改"。这一侧照同一口径补上，两套宿主才对得上。
 *   getContext 故意返回 null（真的上下文已经交出去了，主线程读不到像素）。
 */
const frameCanvasProxy = {
  get width() { return mirror.geom ? mirror.geom.frameW : 1; },
  get height() { return mirror.geom ? mirror.geom.frameH : 1; },
  get style() { return stageEl.style; },
  getBoundingClientRect() {
    const r = stageEl.getBoundingClientRect();
    const gm = mirror.geom;
    if (!gm) {
      return { left: r.left, top: r.top, right: r.left + 1, bottom: r.top + 1,
               width: 1, height: 1, x: r.left, y: r.top };
    }
    // 窗口首页的视口位置：页在滚动内容里是绝对定位（top = 页号 × 步距），
    //   所以还要减掉滚动量；横向是居中偏移 lastPageX。
    const left = r.left + lastPageX / dprNow();
    const top = r.top + (gm.first * gm.pageStep) / dprNow() - stageEl.scrollTop;
    const width = gm.frameW / dprNow();
    const height = gm.frameH / dprNow();
    return { left, top, right: left + width, bottom: top + height,
             width, height, x: left, y: top };
  },
  dispatchEvent(ev) {
    // 派到坐标所在的那一页画布上，而不是派到滚动容器上：指针处理按 event.target
    //   认页（dataset.page），派到容器上 target 就不是画布，命中测试根本不会跑。
    let el = null;
    if (typeof ev.clientY === 'number') {
      for (const c of pageCanvases.values()) {
        const r = c.getBoundingClientRect();
        if (ev.clientY >= r.top && ev.clientY <= r.bottom) { el = c; break; }
      }
    }
    if (!el) el = pageCanvases.get(mirror.geom ? mirror.geom.first : 0) || stageEl;
    return el.dispatchEvent(ev);
  },
  addEventListener(...a) { return stageEl.addEventListener(...a); },
  getContext() { return null; },
};

/* 同步读（来自推送的镜像） */

Object.defineProperties(host, {
  info: { get: () => mirror.info },
  // 这两本账不能指向同一个对象：单线程宿主那边 stats 是 statsJson（页数/字形/形状耗时），
  //   editStats 是 editStatsJson（重排行数/重画像素/光标矩形），来源不同、单位不同。
  //   两个入口指向同一份时，web_probe 读 stats.pageCount 会得到 undefined ——
  //   而 undefined 不会让任何断言变红，只会让报告里少几列。
  stats: { get: () => mirror.layout },
  editStats: { get: () => mirror.stats },
  geom: { get: () => mirror.geom },
  caret: {
    get: () => {
      const s = mirror.stats;
      if (!s) return null;
      return { para: s.caretPara, offset: s.caretOffset, side: s.caretSide, page: s.caretPage,
               x: s.caretX, y: s.caretY, w: s.caretW, h: s.caretH,
               visible: s.caretVisible, docCodepoints: s.docCodepoints };
    },
  },
  text: { get: () => mirror.text0 },
  controls: { get: () => mirror.controls },
  blockedEdits: { get: () => mirror.blockedEdits },
  dirtyPages: { get: () => mirror.dirtyPages },
  caretReport: { get: () => mirror.caretReport },
  // 选区（两端全局流位置 + 页面坐标矩形）。与 caretReport 同一条路：随 state 推过来，
  //   所以宿主侧读它是同步的 —— 引擎在 Worker 那侧，而"读一次选区"不该是一次往返。
  selection: { get: () => mirror.selection },
  // 光标与选区的样式。与 selection 同一条路（随 state 推过来），所以读它是同步的；
  //   形状与单线程宿主的 caretStyle / selectionStyle 逐字对齐。
  caretStyle: { get: () => mirror.caretStyle },
  selectionStyle: { get: () => mirror.selectionStyle },
  // 引擎实时光标偏移。引擎在 Worker 那侧，所以这个"实时"只能是推送那一刻的实时
  //   —— 与 editStats 里的光标位置取自同一次 state，两者可比。排空之后（quiesce）
  //   读到的就是最新那一拍的账。
  caretLive: { get: () => mirror.caretLive },
  pagePx: {
    get: () => (mirror.geom ? [mirror.geom.pageW, mirror.geom.pageH, mirror.geom.gap] : null),
  },
  // 与单线程宿主同形：方法而不是取值器（探针写的是 frameW()）。
  //   形状不一致的代价是一条 TypeError，看不出是命名的问题。
  //   frameW: { get: ... } 也不行 —— 那样 frameW 不是函数。
  heapMB: { get: () => mirror.heapMB },
  fontMode: { get: () => mirror.fontMode },
  // 环境信息，形状与单线程宿主的 env / fonts 一致：探针拿它记报告（胶水体积、UA、地址）。
  //   wasmGlue 必须由 Worker 侧量好随 ready 报过来 —— 主线程的资源表里没有 Worker 发出的
  //   请求（胶水与 wasm 都是那一侧取的），在那边量恒为 0，而 0 会安安静静地进报告。
  fonts: {
    get: () => ({
      wasmGlue: mirror.info ? mirror.info.glueBytes : 0,
      ua: navigator.userAgent,
      url: location.href,
    }),
  },
  // 同一件事的两种口径：zoomLevel 是密度（设备像素 / 磅），zoomFactor 是倍数
  //   （1 = 100% = 96 dpi）。界面上那一格与 zoomFactor 一致；引擎与判据读 zoomLevel。
  zoomLevel: { get: () => zoom },
  zoomFactor: { get: () => uiZoomOf(zoom) },
  scenario: { get: () => scenario },
  editMode: { get: () => mode },
  edit: { get: () => mirror.stats },
  blitStats: {
    get: () => ({ stateMs: mirror.lastMs, label: mirror.lastLabel, seq: mirror.stateSeq }),
  },
  textarea: { get: () => ta },
  // 页画布：每页一张。元素本身已经交出去了（主线程拿不到上下文），
  //   这里给的是页号清单与元素清单，供诊断与结构断言用。
  pageCanvasList: { get: () => [...pageCanvases.keys()].sort((a, b) => a - b) },
  pageCanvasEls: { get: () => [...pageCanvases.values()] },
  canvas: { get: () => frameCanvasProxy },
  // 形状与单线程宿主的 viewport 对齐（anchor / desired / visible / canvases 一个都不能少：
  //   探针靠它判"窗口跟着可见页走"，缺一格就是一条 .first 的 TypeError）。
  //   pooled = 闲置槽数（窗口缩小时留下的槽；已转移的画布不能再转移一次，
  //   所以"池"只能是"按槽固定"，不是"回收后重新分配"）。
  viewport: {
    get: () => (mirror.geom ? {
      anchor: mirror.geom.anchor,
      engineFirst: mirror.geom.first, engineCount: mirror.geom.count,
      desired: mirror.geom.desired, visible: mirror.geom.visible,
      scrollY: mirror.geom.scrollY, dpr: mirror.geom.dpr,
      pages: [...pageCanvases.keys()].sort((a, b) => a - b),
      canvases: pageCanvases.size, pooled: slotCanvases.length - pageCanvases.size,
      // 宿主侧实际在用的槽数（= 已建槽数，只增不减）。必须与引擎报的 geom.slots 相等。
      slots: slotCanvases.length,
      pagePx: [mirror.geom.pageW, mirror.geom.pageH, mirror.geom.gap],
      viewSize: viewSize(),
    } : null),
  },
});

/* 宿主自己的动作 */

// 帧尺寸（设备像素）。与单线程宿主一致地做成方法（见上面那段说明）。
host.frameW = () => (mirror.geom ? mirror.geom.frameW : 0);
// 引擎侧的帧槽数：切帧（blitPage / selfCheck / 光标定位）按它算源行。
//   由 worker 从引擎读回、随 geom 报来 —— 宿主不自己算，两侧必须是同一个数。
host.frameSlots = () => (mirror.geom ? (mirror.geom.frameSlots | 0) : 0);
host.frameH = () => (mirror.geom ? mirror.geom.frameH : 0);

/**
 * 宿主动作里"自带往返"的那些链（命中测试 → 落光标就是一条：中间要先问一次 hitTest）。
 *   排空屏障必须等它们，否则屏障会在链的中途就返回 —— 探针随即读到的是旧镜像，
 *   而症状是"点了画布但光标没动"这种看起来像功能没接的红。
 */
const inFlight = new Set();

function trackChain(p) {
  const w = Promise.resolve(p).catch(() => { /* 失败已记进 errors */ });
  inFlight.add(w);
  w.then(() => inFlight.delete(w));
  return p;
}

// DOM 输入事件引起的链：登记进在途表，并把它挂成 host.rendered。
function trackInput(p) {
  host.rendered = trackChain(p);
  return p;
}

/**
 * 等"最近一次输入引起的渲染"完成的 Promise。
 *
 * 交互路径是单向的（消息发出去就不等），所以宿主手里本来没有它的句柄 —— 但那个
 *   Promise 一直都在（message 发出那一刻就存在），只是被丢掉了。留着它就得到
 *   "输入 → 渲染完成"这个可等待的点：命令的 state 是 Worker 跑完 render()
 *   （present + 重贴脏页）才推回来的。边界：它保证"离屏画布已经有这一帧"，
 *   不保证合成器已经刷到屏幕上；查像素够用，要等上屏得再补一次 rAF。
 *
 * 与 quiesce 的分工：这是"我这一下画完了"，quiesce 是"队列全空了"。一次点击会牵出
 *   命中测试那样自带往返的链，那种要等的是 quiesce。
 */
host.rendered = Promise.resolve(true);

/**
 * 排空屏障：等 Worker 把此刻已经在途的消息全部处理完。
 *
 * 靠的是 postMessage 保序 —— 发一条什么都不算的 ping（READ.ping），它的 reply 回来时，
 *   之前发出的消息必然都已处理完。这是"交互之后要读到那一拍结果"的同步点。
 * 刻意用 ping 而不是拿 present 当哨兵：一次 present 会重画并改变帧，那会把
 *   "某个入口忘了收尾" 这类断言直接盖掉（哨兵必须无副作用）。
 *   与单线程宿主的 quiesce 是同一个契约（那边全同步，直接 resolve）。
 */
host.quiesce = async () => {
  // 引擎已经不可用时没有队列可等：在途调用在崩溃那一刻就被当场拒掉了，也没有谁能回 ping。
  //   这里必须直接返回，否则屏障本身会抛"引擎已不可用"，把真正的失败原因盖在下面。
  if (dead) return true;
  // 反复"等在途链 → 消息屏障"：ping 的等待期间，先前那条链的 hitTest 答复可能刚回来并
  //   接着发出了 setCaret —— 于是又有了新在途。循环把它收干净（有上限，不会空转）。
  for (let i = 0; i < 6; ++i) {
    while (inFlight.size) await Promise.all([...inFlight]);
    await askOp('ping', {});
    if (!inFlight.size) break;
  }
  return true;
};

/**
 * 引擎当前那一帧的一份拷贝（探针用）。
 *   拷贝由 Worker 侧做，然后转移 buffer 过来（O(1)，不白拷 8 MB）。
 *   必须是快照：wasm 堆上那个视图任何一次 present 都会把它原地覆盖，
 *   两次读到的永远是同一块内存 —— 逐字节比对会永远绿且毫无意义。
 */
host.rawFrame = () => askOp('rawFrame', {});

/**
 * 自检：把 Worker 算出来的像素差补上"只有主线程知道"的那几格。
 *
 * 返回形状与单线程宿主的 selfCheck 一致 —— 它是给用户的取证入口（F12 里把对象发出来），
 *   也是探针的主判据。cssSize / rectPx 要看 DOM，只能在这一侧补；dpr 也用主线程的，
 *   免得两边对"设备像素比"的口径有分歧。
 */
const askSelfCheck = host.selfCheck;
host.selfCheck = async () => {
  const r = await askSelfCheck();
  if (!r) return r;
  const el = pageCanvases.get(mirror.geom ? mirror.geom.first : 0);
  const pr = frameCanvasProxy.getBoundingClientRect();
  return Object.assign({}, r, {
    dpr: dprNow(),
    pages: Object.assign({}, r.pages, {
      cssSize: el ? [el.style.width, el.style.height] : null,
      rectPx: [Math.round(pr.width), Math.round(pr.height)],
    }),
  });
};

/**
 * 视口队列的账（见 viewDiag 的说明）。rtt 与 work 按同一次回话成对入账，逐对相减就是
 *   该次的排队等待。两个数组各留最近 512 条。
 */
host.viewDiag = () => ({
  sent: viewDiag.sent, coalesced: viewDiag.coalesced, released: viewDiag.released,
  settled: viewDiag.settled, canvasMsgs: viewDiag.canvasMsgs,
  // 画布消息分两类、状态回推按 label 分类（含正文那一格的体量）：门面是手搭形状，
  //   新加的账在这里漏一次就是"读数恒为 0"那种最难查的假象。
  canvasNew: viewDiag.canvasNew, canvasDrop: viewDiag.canvasDrop,
  label: { ...viewDiag.label }, textMax: viewDiag.textMax, textSum: viewDiag.textSum,
  packAvg: viewDiag.packAvg, packMax: viewDiag.packMax,
  rtt: viewDiag.rtt.slice(), work: viewDiag.work.slice(), handle: viewDiag.handle.slice(),
  blit: viewDiag.blit.slice(), blitPages: viewDiag.blitPages.slice(),
});

host.setScroll = (cssPx) => {
  stageEl.scrollTop = cssPx;
  return viewOp(Math.round(stageEl.scrollTop * dprNow()));
};

host.syncScroll = () => viewOp(Math.round(stageEl.scrollTop * dprNow()));

host.loadMedicalSample = async function loadMedicalSample(repeat) {
  const base = new URL('../', location.href);
  const res = await fetch(new URL('assets/samples/medical-records.md', base));
  if (!res.ok) throw new Error(String(res.status));
  const text = core.cleanMarkdownLines(await res.text());
  await host.loadTextDocument(text, Math.max(1, Number(repeat) || 1));
  return { paragraphs: text.split('\n').length, chars: text.length };
};

/* 光标闪烁 */

/**
 * 把闪烁相位归到"可见"并重启定时器。
 *
 * 光标不是 DOM 元素 —— 它是引擎画进帧里的（单线程宿主那侧同样如此：那边也是
 *   setInterval 里 showCaret + present）。宿主能做的只有每 530 ms 把相位发过去。
 * ⇒ 只"翻转"不"归相位"是不够的：相位可能停在隐藏上，用户敲完字光标却是隐的。
 *
 * 两个守卫：组合期间不动（与单线程宿主同一守卫，重排会让输入法候选窗跳）；
 *   拖选进行中整个停掉（按下之后手指停住不动时没有"拖了一步"这个事件可挂，
 *   而拖动期间光标一亮一灭看上去就是画面在抖）。
 */
function restartBlinkTimer() {
  if (blinkTimer) clearInterval(blinkTimer);
  blinkTimer = setInterval(() => {
    if (!host.ready || (input && input.composing)) return;
    if (dragActive()) return;
    caretOn = !caretOn;
    postOp('blinkPhase', { on: caretOn });   // 单向：这一拍只有可见相位在变
  }, 530);
}

/*
 * 拖选是否正在进行（按下已受理、还没收尾）。
 */
function dragActive() {
  const st = drag ? drag.state() : null;
  return !!(st && st.active);
}

/**
 * 交互（按键 / 打字 / 点击 / 命令）之后把光标按住可见，并把下一次翻转推迟一整拍。
 *
 * 两个动作缺一不可：
 *   一是相位若停在隐藏上，补一次 showCaret(true)。引擎那边 caret_visible_ 只由本侧翻转
 *      （engine.cpp 的 showCaret，引擎自己只在 repaintAfterEdit 里置真），一旦停在 false，
 *      后面每次 present 都不再画光标 —— 表现成"按了方向键，光标好像没动"。
 *   二是无条件重启定时器。即便相位此刻可见，也可能下一秒就到 530 ms 的翻转点，
 *      那样"刚移动完光标又灭了"，仍然像没重置。重启把这一次翻转整拍推后。
 *
 * 二是只动 JS 定时器、不发消息，所以"相位本来就可见"时这一路是零额外开销。
 */
function blinkHold() {
  if (!caretOn) postOp('blinkPhase', { on: true });
  // 无条件把本地相位也写成可见。只在补发时才写的话，探针用门面 showCaret(false)
  //   把引擎摆成隐藏之后，本地 caretOn 仍是 true —— 两边脱节，后续的"该补一次呈现"
  //   就永远不会发生（症状是光标一直不出现，看起来像渲染坏了）。
  caretOn = true;
  restartBlinkTimer();
}

// 启动与强制归相位：无条件让光标可见（首帧自动起、探针的 startBlink 走这条）。
function resetBlinkPhase() {
  caretOn = true;
  postOp('blinkPhase', { on: true });
  restartBlinkTimer();
}

host.stopBlink = () => {
  if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
  caretOn = true;
  // 走静默档（只改状态、不呈现）：与单线程宿主的 stopBlink 逐字对齐 ——
  //   探针紧接着要读的是"上一次全量呈现"的账（它的分母），这一下不能把它覆盖掉。
  return waitOp('停止闪烁', { t: 'do', name: 'phase', args: { on: true } });
};

host.startBlink = () => { resetBlinkPhase(); return true; };

/* 启动 */

/**
 * 窗口尺寸变化：把新的视口推给引擎。
 *
 * 具名而不是就地写箭头函数，是为了 dispose() 能摘掉这个监听 —— 监听挂在 window 上，
 *   实例拆掉之后它仍然活着，会拿着已经废弃的 stageEl 继续发消息。
 */
const onResize = () => viewOp(Math.round(stageEl.scrollTop * dprNow()));

/**
 * 屏幕像素比变了（窗口被拖到另一块显示器上）：密度基准跟着变，整幅要按新 DPR 重排重画。
 *
 * 监听与重挂由 host_core.armDprWatch 管，这里只写"变了之后做什么"：把同一个倍数按新
 *   基准重设一次。引擎那边按新密度重算页像素与帧，宿主再按回推的几何重建画布尺寸。
 *   与"这份密度是按哪个屏幕算的"比，不与上一次读到的 DPR 比：密度才是换算的唯一真源。
 */
const stopDprWatch = core.armDprWatch(() => {
  if (dprNow() === zoomCtl.dpr) return;
  setZoomDensity(engineZoomOf(uiZoomOf(zoom)));
  syncZoomInput();
  post({ t: 'zoom', zoom, dpr: dprNow() });
});

async function boot() {
  // 产物是用 -msimd128 编的：引擎不支持 SIMD 时，失败发生在 Worker 侧实例化那一步，
  //   报出来的话术却是"先执行 cmake --workflow --preset wasm"，与真实原因对不上。
  //   所以在起 Worker 之前判一次，抛出的失败自带标题与下一步。
  //   探测模块没加载时不拦：它与拖选同属可选前置，缺了不该让启动整个炸掉。
  if (window.heditorWasmFeature) window.heditorWasmFeature.assertSimd();
  worker = new Worker('engine_worker.js' + BUST);
  worker.onmessage = onMessage;
  worker.onerror = (e) => onWorkerError({ where: 'worker.onerror', message: e.message || String(e) });
  bootLead = now();
  const base = new URL('../', location.href);
  const vs = viewSize();
  // boot 不带画布：窗口要等引擎排完才知道，所以先报几何，宿主再按窗口把每页画布交过去。
  post({
    t: 'boot', glue: glueUrl() + BUST, bust: BUST, exportName: 'CreateHEditor',
    fonts: FONTS.map((f) => ({ ...f, path: new URL(f.path, base).pathname })),
    dpr: dprNow(), viewW: vs.viewW, viewH: vs.viewH,
    scenario: 6, zoom: baseDensity(), fontMode: 'full',
  });
  // 回写一次：让"界面那一格 = 门面的倍数"这条不变量从启动起就成立，而不是靠 HTML 里
  //   那个 value 恰好等于默认档。换算方向写反了在这里就会露出来。
  syncZoomInput();

  // 输入层：键位映射、命中归宿、连击、组合提交都在 host_core 里，两宿主共用一份。
  //   这里只交环境 —— 坐标怎么算、命令怎么发、落点怎么收尾，是这一侧才有的知识。
  input = core.createInput({
    isReady: () => host.ready,
    ta,
    hot: () => ({
      vertical: !!(mirror.stats && mirror.stats.caretVertical),
      blockReversed: !!(mirror.stats && mirror.stats.caretBlockReversed),
    }),
    // 一律按"没改动给 null"发：输入层只关心这一下有没有改动，而异步宿主的那一步
    //   结果要到下一拍才知道，形状统一成 null / 统计，两边判断才一致。
    write: (name, args) => opUser(name, args, true),
    hit: (xPt, yPt) => askOp('hit', { x: xPt, y: yPt }),
    pointOf: (ev) => framePointAt(ev.clientX, ev.clientY),
    zoom: () => zoom,
    capturePointer: () => {
      if (lastPointerId === null) return;
      try { stageEl.setPointerCapture(lastPointerId); capturedId = lastPointerId; }
      catch (e) { /* 合成事件没有指针 */ }
      lastPointerId = null;
    },
    // 监听必须当场挂上：命中是一次往返，等它回来才起手的话，按下与第一次移动之间的
    //   移动会被整段丢掉，而那一段往往正是"按下去就往右拖"。
    beginDrag: (ev) => { if (drag) drag.begin(ev, false); },
    get drag() { return drag; },
    focus: () => ta.focus(),
    // 引擎在 Worker 那侧自己收尾（编辑完重排重画、脏页推回来），闪烁相位由 opUser 归位，
    //   所以这一侧没有单线程宿主那套 afterEdit。
    commit: () => {},
    place: (a) => (typeof a.side === 'number'
      ? opUser('setCaretAtSideLine',
               { para: a.para, offset: a.offset, side: a.side, line: a.line | 0 })
      : opUser('setCaret', { para: a.para, offset: a.offset })),
    selectCellPara: (para) => opUser('selectCellAtPara', { para }, true),
    cellSelected: () => {},
    status: (text) => setStatus(text),
    // 在途链：排空要等它，host.rendered 也取它。
    onChain: (p) => trackInput(p),
  });

  // 键盘与 IME：全部由 textarea 承接（canvas 收不到键盘，也收不到合成事件）。
  ta.addEventListener('keydown', input.onKeyDown);
  ta.addEventListener('compositionstart', input.onCompositionStart);
  ta.addEventListener('compositionupdate', input.onCompositionUpdate);
  ta.addEventListener('compositionend', input.onCompositionEnd);
  // 指针走事件委托（挂在滚动容器上，不挂在每张画布上）：画布是随窗口动态建与丢的，
  //   逐张挂监听会在换窗时漏挂 —— 委托给容器就不会有那个缺口。
  stageEl.addEventListener('mousedown', input.onPointerDown);
  // 记下最近一次 pointerdown 的指针 id：mousedown 的事件对象上没有 pointerId，
  //   而按下时要拿它去 setPointerCapture。用掉就清 —— 探针派的合成 MouseEvent
  //   没有配套的 pointerdown，靠这一条不会拿到过期的 id 去乱捕获。
  //
  // 顺便把上一次的捕获放掉：捕获只在 pointerup 时由浏览器自动释放，而
  //   "抬起事件丢了"正是要处理的那种情形 —— 捕获会一直挂着，把之后的
  //   pointerdown/mousedown 重定向到容器上，于是下一次点在页上根本起不了拖选
  //   （症状从"一直在拖"变成"拖不动了"）。下一拍按下就清，是这一条唯一的自愈点。
  stageEl.addEventListener('pointerdown', (ev) => {
    if (capturedId !== null) {
      try { stageEl.releasePointerCapture(capturedId); } catch (e) { /* 已经没了 */ }
      capturedId = null;
    }
    lastPointerId = ev.pointerId;
  });
  stageEl.addEventListener('scroll', viewFireAndForget, { passive: true });
  // 视口变了不用换画布：页画布的尺寸只取决于页，视口只影响"窗口要哪几页" ——
  //   窗口一变，推回来的几何会让 syncPageCanvases 自己把缺的页建出来。
  window.addEventListener('resize', onResize);
  // 拖选：按下之后的移动与抬起由共享模块挂在 window 上（拖出画布再拖回来都收得到）。
  //   命中要走一次往返，所以模块那边按"只留最新一个待处理点"合并 —— 不合并的话
  //   每帧一次往返会排起长队，选区看起来追着鼠标跑却总是慢几拍。
  if (typeof window.createDragSelect === 'function') {
    drag = window.createDragSelect({
      container: stageEl,
      pointAt: framePointAt,
      hitAt: (x, y) => askOp('hit', { x: x / zoom, y: y / zoom }),
      extendTo: dragExtendTo,
    });
  } else {
    // 不许静默降级：模块没加载成功时"拖不动"要有痕迹，否则会一路查命中测试。
    report.warn('拖选不可用', { where: 'boot', message: 'web/drag_select.js 未加载' });
  }
  ta.focus();
}

// 错误降级面的取证句柄（与单线程宿主的 hostErrors 同形）。
Object.defineProperty(host, 'hostErrors', {
  get: () => ({
    fatalActive: report.fatalActive,
    bannerText: errorSurface ? errorSurface.bannerText : '',
    bannerDetail: errorSurface ? errorSurface.bannerDetail : '',
    toastTexts: errorSurface ? errorSurface.toastTexts : [],
    entries: report.entries,
    dead,
  }),
});

Object.defineProperty(host, 'container', { get: () => root });
Object.defineProperty(host, 'dead', { get: () => dead });
Object.defineProperty(host, 'disposed', { get: () => disposed });
/**
 * 拖选的诊断面。与单线程宿主逐字对齐：drag 是读数不是方法，dragSettled() 等所有
 *   在途命中落地 —— 这一侧拖动是几次往返，不等就读到的是旧选区。
 */
Object.defineProperty(host, 'drag', { get: () => (drag ? drag.state() : null) });
host.dragSettled = () => (drag ? drag.settled() : Promise.resolve());

/**
 * 拆掉这一份实例。
 *
 * 终止 Worker（这是唯一能真正释放那一侧 wasm 堆的动作）、清掉本地定时器、摘掉 window
 *   上的 resize 监听、把页画布从文档里移除、抹掉错误界面，最后把 ready 打回 false。
 *   容器里那些元素上的监听不逐个摘：它们随着容器一起被丢弃，而残留的处理器会因为
 *   dead 为真而什么都不做。
 */
host.dispose = () => {
  if (disposed) return false;
  disposed = true;
  dead = true;
  host.ready = false;
  if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
  window.removeEventListener('resize', onResize);
  if (stopDprWatch) stopDprWatch();
  if (worker) { worker.terminate(); worker = null; }
  for (const el of slotCanvases) if (el.parentNode) el.parentNode.removeChild(el);
  slotCanvases.length = 0;
  pageCanvases.clear();
  host.pageCanvasCount = 0;
  if (errorSurface) errorSurface.clear();
  return true;
};

/**
 * 启动这一份实例。resolve 成 { ok, error }，不 reject —— 没人接的 rejection 会变成
 *   一条未处理的异步失败，把真正的失败原因淹掉（这一侧的致命失败必须显示在横幅上）。
 */
const readyPromise = boot().then(
  () => ({ ok: true }),
  (e) => {
    const msg = String((e && e.message) || e);
    errors.push('boot: ' + ((e && e.stack) || e));
    dead = true;
    if (barEl) {
      barEl.textContent = '启动失败：' + msg;
      barEl.className = 'err';
    }
    // 启动失败必须在页面上看得见：Worker 起不来时这一侧什么都画不出，
    //   只写进 errors 数组的话用户面对的是一块空白区域。
    // 探测层（wasm_feature.js）抛的失败自带标题与下一步，原样展示。
    //   不这么做的话，浏览器不支持 SIMD 会被说成"先执行 cmake"—— 而产物明明在。
    report.fatal((e && e.heditorTitle) || '编辑器启动失败', {
      detail: msg,
      hint: (e && e.heditorHint) ||
            '先执行：cmake --workflow --preset wasm；若用 ?wasm= 指定了产物目录，检查它是否存在。',
      retry: () => location.reload(),
    });
    return { ok: false, error: e };
  });

Object.defineProperty(host, 'whenReady', { get: () => readyPromise });

return host;
}

// 页面入口
window.createWorkerEditor = createWorkerEditor;

/**
 * 演示页（worker.html）自动起一个实例，并把门面同时挂到 window.__wheditor 与
 *   window.__heditor 上：两个宿主对外是同一个门面形状，探针才可能同一份通吃。
 *   多实例页面在加载本文件之前写 window.__heditorAutoBoot = false，由页面自己调。
 */
if (window.__heditorAutoBoot !== false && document.getElementById('stage')) {
  const inst = createWorkerEditor(document.body, { chrome: true });
  window.__wheditor = inst;
  window.__heditor = inst;
}

})();   // 文件级作用域结束
