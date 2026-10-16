// Worker 宿主的验收探针（可证伪判据 + 实测数字）。
//
// 与 scripts/edit_probe.mjs 的分工：那一份验的是**单线程宿主**，这一份验的是
//   **引擎跑在 Worker 里**那个宿主。两份的断言口径尽量对齐（同样的墨迹判据、
//   同样的括号左缘判据），因为它们要能互相印证。
//
// 这一份特别要钉住的几件事：
//   ① 帧真的没有跨线程搬 —— 主线程那个 canvas 已经是占位画布，getContext 必须拿不到；
//   ② 交互路径真的可以零 await —— 命令发出去就不管，状态由推送回来；
//   ③ 新增的"把帧切片贴到画布"那段代码是对的 —— selfCheck 逐像素比画布与帧；
//   ④ 三类通道各自的代价 —— 按键端到端、查询往返、长文档装载。
//
// 两条写法上的硬约束（都栽过）：
//   求值体是 (function(){ ... })()，**不是 async** —— 表达式里不能出现顶层 await；
//   单行的直接 return 那个 Promise（awaitPromise 会等），多行的包一层 async IIFE。
//
// 跑法：
//   node scripts/worker_probe.mjs --port 9222 --url http://127.0.0.1:8781/web/worker.html
import { CDP, Checks, sleep } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/worker.html');

// 缩放的两套口径：门面（zoomLevel / zoomTo / run）与引擎收的是"密度"（设备像素 / 磅，
//   1 = 72 dpi），界面 #zoom 输入框收的是"倍数"（1 = 100% = 96 dpi）。差一个 4 / 3。
//   本文件里的 zoom 参数一律是密度，只有往输入框里写值时才换成倍数 —— 混用会在千像素
//   尺度上整片偏，而且只在倍数正好等于密度时看不出来。
const ZOOM_BASE = 4 / 3;
const uiZoom = (density) => density / ZOOM_BASE;

const say = (s) => process.stdout.write(s + '\n');
const j = (v) => JSON.stringify(v);
const body = (b) => 'return (async () => {\n' + b + '\n})();';

/**
 * 真键盘与真鼠标：走 CDP 的原始输入事件，不经过门面。
 *
 * 为什么必须有这一路：门面方法（h.move / h.setCaret）是程序化调用，它绕过了 keydown 与
 *   mousedown 这两个入口本身。而实测中恰好就是这两个入口出的问题 ——
 *   点在不可聚焦的 canvas 上会让 activeElement 掉回 body，此后 keydown 根本到不了宿主；
 *   这类问题用程序化调用永远验不出来。
 */
async function pressKey(c, key, vk) {
  const base = { key, code: key, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk };
  await c.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...base });
  await c.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
}

async function clickAt(c, x, y) {
  const p = { x, y, button: 'left', buttons: 1, clickCount: 1 };
  await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
  await c.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p, buttons: 0 });
}

const caretOf = (c) => c.eval(
  'const h = window.__wheditor;' +
  'if (!h || !h.editStats) return null;' +
  'return { off: h.editStats.caretOffset, vis: h.editStats.caretVisible, x: h.editStats.caretX };');

const chk = new Checks('Worker 宿主');

async function main() {
  const t = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(t.webSocketDebuggerUrl);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Page.navigate', { url: URL_ });
  // 等首帧 + 页画布齐：ready 是**故意提前报**的（装载里抛异常时"永远等不到 ready"
  //   会变成静默挂起）；而画布是引擎报出窗口之后宿主才建的，得等每一页都交过去。
  await c.waitFor(
    'return !!(window.__wheditor && window.__wheditor.stats &&' +
    ' window.__wheditor.geom && window.__wheditor.geom.pagesReady)',
    'first frame + page canvases');

  chk.section('[W1] 启动与结构性判据');
  const info = await c.eval('return window.__wheditor.info');
  const struct = await c.eval(
    'const h = window.__wheditor;' +
    'const els = h.pageCanvasEls;' +
    'let allTransferred = els.length > 0; let why = "";' +
    'for (const cv of els) {' +
    '  try { cv.getContext("2d"); allTransferred = false; } catch (e) { why = e.name; }' +
    '}' +
    'return { pages: h.pageCanvasList, els: els.length, allTransferred, why,' +
    '         window: [h.geom.first, h.geom.count], pagePx: h.pagePx,' +
    '         aliased: window.__heditor === window.__wheditor };');
  chk.check('引擎在 Worker 里起来了', !!info, `boot ${info.bootMs.toFixed(0)} ms`);
  chk.check('帧共享 wasm 线性内存（零拷贝）', info.zeroCopyFrame === true, j(info.zeroCopyFrame));
  // 已转移控制权的画布在主线程 getContext 会**抛**（不是返回 null）—— 这是"像素确实
  //   不归主线程管"最硬的一条结构性判据：不是我们不去读，而是读不到。
  chk.check('页画布一页一张，且每一张都已交出去（getContext 直接抛）',
            struct.allTransferred === true && struct.els === struct.pages.length,
            `${struct.els} 张 / 页 ${j(struct.pages)} / ${struct.why}`);
  chk.check('页画布与当前窗口一一对应',
            struct.pages.length === struct.window[1] &&
            struct.pages[0] === struct.window[0],
            `窗口 ${j(struct.window)} / 页画布 ${j(struct.pages)}`);
  chk.check('两套门面名字指向同一个宿主（__heditor 是别名）',
            struct.aliased === true, j(struct.aliased));
  chk.check('字体在 Worker 里自己装载（20 MB 量级）',
            info.fontBytes > 15000000,
            `${(info.fontBytes / 1048576).toFixed(1)} MB / ${info.fontMs.toFixed(0)} ms`);
  chk.check('Worker 里单实例堆在合理范围', info.heapMB > 10 && info.heapMB < 200,
            `${info.heapMB.toFixed(1)} MB`);

  // 两本账必须分开：单线程宿主那边 stats 是 statsJson（页数 / 字形 / run 数 / 形状耗时），
  //   editStats 是 editStatsJson（段数 / 总页数 / 光标矩形 / 重画像素），来源与单位都不同。
  //   两个入口指向同一个对象时，web_probe 读 stats.pageCount 会得到 undefined —— 而
  //   undefined 不会让任何断言变红，只会让报告里少几列。这条判据把它钉住。
  chk.section('[W1b] 两个统计入口是两本账（形状与单线程宿主一致）');
  {
    const two = await c.eval(
      'const h = window.__wheditor;' +
      'return { same: h.stats === h.editStats,' +
      '         layPages: h.stats.pageCount, layRuns: h.stats.fontRunCount,' +
      '         layShape: h.stats.shapeMs, layGlyphs: h.stats.glyphCount,' +
      '         edPages: h.editStats.totalPages, edParas: h.editStats.totalParagraphs,' +
      '         edMissing: h.editStats.missingGlyphCount };');
    chk.check('stats 与 editStats 不是同一个对象（两本账）', two.same === false, j(two.same));
    chk.check('排版那本（stats）带页数 / run 数 / 形状耗时',
              two.layPages >= 1 && two.layRuns >= 1 && typeof two.layShape === 'number',
              `页 ${two.layPages} / run ${two.layRuns} / shape ${two.layShape}`);
    chk.check('编辑那本（editStats）带段数 / 总页数 / 缺字数',
              two.edParas >= 1 && two.edPages >= 1 && typeof two.edMissing === 'number',
              `段 ${two.edParas} / 页 ${two.edPages} / 缺字 ${two.edMissing}`);
  }

  chk.section('[W2] 首帧：帧上真的有字，且每页画布与帧对应段逐像素一致');
  const s0 = await c.eval('return window.__wheditor.stats');
  const e0 = await c.eval('return window.__wheditor.editStats');
  const ink0 = await c.eval('return window.__wheditor.ink()');
  const self0 = await c.eval('return window.__wheditor.selfCheck()');
  chk.check('首帧有页有字', e0.totalPages >= 1 && s0.glyphCount > 0,
            `${e0.totalPages} 页 / ${s0.glyphCount} 字形`);
  chk.check('帧上有墨迹', ink0 > 500, `${ink0} px`);
  chk.check(`每页画布都在，且与帧对应段逐像素一致（差 ${self0.diff} / ${self0.compared}）`,
            self0.missing === 0 && self0.sizeBad === 0 && self0.ratio < 0.0005,
            `缺页 ${self0.missing} / 尺寸不符 ${self0.sizeBad} / 比例 ${self0.ratio.toFixed(6)}`);

  chk.section('[W3] 命令：编辑 / 光标 / 段落结构');
  // 每一步都先把光标摆到明确位置再动 —— 上一步操作会带着光标走，靠"上一步留下的
  //   位置"推下一步的语义，最容易把判据写成"操作没生效"。
  //
  // 合并的语义要按引擎的守卫来写：mergeWithNextParagraph 要求**光标在段尾**
  //   （engine.cpp 里 `snapToCluster(p.offset) != len` 就返回 false），mergeWithPrevious
  //   要求光标在段首。它们是 backspace/deleteForward 在段落边界处委托的原语，
  //   所以边界外的位置本来就不该动 —— 这一点先单独钉一条，省得后面把"没动"当成 bug。
  const s6 = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.run(6, 1.5);' +
    'const p0 = h.editStats.totalParagraphs;' +
    'const before = h.text;' +
    'await h.setCaret(0, 0);' +
    'await h.type("测");' +
    'const typed = { text: h.text, off: h.caret.offset };' +
    'await h.splitParagraph();' +
    'const split = h.editStats.totalParagraphs;' +
    'await h.setCaret(0, 0);' +
    'await h.mergeWithNextParagraph();' +
    'const atStart = h.editStats.totalParagraphs;' +
    'await h.setCaret(0, h.text.length);' +
    'await h.mergeWithNextParagraph();' +
    'const merged = h.editStats.totalParagraphs;' +
    'await h.setCaret(0, 1);' +
    'const moved0 = h.caret;' +
    'await h.move(-1);' +
    'const moved1 = h.caret;' +
    'await h.setCaret(0, 1);' +
    'await h.backspace();' +
    'const deleted = h.text;' +
    'await h.setCaret(0, 1);' +
    'await h.splitParagraph();' +
    'const split2 = h.editStats.totalParagraphs;' +
    'await h.setCaret(1, 0);' +
    'await h.mergeWithPreviousParagraph();' +
    'const prevMerged = h.editStats.totalParagraphs;' +
    'return { before, p0, typed, split, atStart, merged, moved0, moved1, deleted,' +
    '         split2, prevMerged };'));
  chk.check('打字改变了第 0 段文本', s6.typed.text.length === String(s6.before).length + 1,
            `${String(s6.before).length} -> ${s6.typed.text.length}`);
  chk.check('打字后光标落在插入点之后', s6.typed.off === 1, `offset=${s6.typed.off}`);
  chk.check('回车分段让段数 +1', s6.split === s6.p0 + 1, `${s6.p0} -> ${s6.split}`);
  chk.check('光标在段首时"向后合并"不动（引擎的守卫如此）',
            s6.atStart === s6.split, `${s6.split} -> ${s6.atStart}`);
  chk.check('光标在段尾时"向后合并"让段数 -1', s6.merged === s6.p0,
            `${s6.atStart} -> ${s6.merged}`);
  chk.check('光标在段首时"向前合并"让段数 -1', s6.prevMerged === s6.split2 - 1,
            `${s6.split2} -> ${s6.prevMerged}`);
  chk.check('退格删掉了刚打的那个字', s6.deleted === s6.before,
            `${j(s6.typed.text)} -> ${j(s6.deleted)}`);
  chk.check('方向键让光标偏移真的动了',
            s6.moved0.offset === 1 && s6.moved1.offset === 0,
            `${s6.moved0.offset} -> ${s6.moved1.offset}`);

  chk.section('[W4] 撤销与重做');
  const undo = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.run(6, 1.5);' +
    'const t0 = h.text;' +
    'await h.type("甲乙丙");' +
    'const t1 = h.text;' +
    'await h.undo();' +
    'const t2 = h.text;' +
    'await h.redo();' +
    'const t3 = h.text;' +
    'return { t0, t1, t2, t3, limit: await h.undoLimit() };'));
  chk.check('打字后撤销回到原文本', undo.t2 === undo.t0, `${j(undo.t1)} -> ${j(undo.t2)}`);
  chk.check('重做回到打字后的文本', undo.t3 === undo.t1, `${j(undo.t2)} -> ${j(undo.t3)}`);
  chk.check('撤销上限可读', typeof undo.limit === 'number', String(undo.limit));

  chk.section('[W5] 表单模式：控件清单、字段跳转、括号范围');
  const form = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.loadFormSample(0);' +
    'const doc = { controls: h.controls.count, mode: h.controls.mode };' +
    'await h.setEditMode(1);' +
    'const inForm = h.controls.mode;' +
    'const slots0 = ((await h.bracketSlots) || []).length;' +
    'await h.focusControl(true);' +
    'const caretAfterTab = h.controls.caret;' +
    'await h.setBracketScope(1);' +
    'const mark1 = await h.markCheck();' +
    'await h.setBracketScope(0);' +
    'const mark0 = await h.markCheck();' +
    'return { doc, inForm, slots0, caretAfterTab, mark0, mark1, blocked: h.blockedEdits };'));
  chk.check('表单样例带出了可编辑控件', form.doc.controls > 0, `${form.doc.controls} 个`);
  chk.check('切到表单模式后模式位跟着变', form.inForm === 1, `mode=${form.inForm}`);
  chk.check('括号占位槽非空（排布来自排版结果）', form.slots0 > 0, `${form.slots0} 槽`);
  chk.check('跳字段把光标移到某个控件上', typeof form.caretAfterTab === 'number',
            `caret=${form.caretAfterTab}`);
  chk.check('全部范围：每个可编辑字段的左括号都画了', form.mark0.allOk === true,
            `${form.mark0.rows.length} 行 / 不符 ${form.mark0.rows.filter((r) => !r.ok).length}`);
  chk.check('仅焦点范围：最多只有一个字段画括号',
            form.mark1.rows.filter((r) => r.leftEdgePx > 0).length <= 1,
            `画了 ${form.mark1.rows.filter((r) => r.leftEdgePx > 0).length} 个`);

  chk.section('[W6] 审计与不变式');
  const audit = await c.eval(body(
    'const h = window.__wheditor;' +
    'return { frame: await h.auditFrame(), layout: await h.auditLayout(),' +
    '         compile: await h.auditCompile() };'));
  chk.check('帧审计有内容', audit.frame && Object.keys(audit.frame).length > 0,
            j(audit.frame).slice(0, 70));
  chk.check('排版审计有内容', audit.layout && Object.keys(audit.layout).length > 0,
            j(audit.layout).slice(0, 70));
  chk.check('编译审计有内容', audit.compile && Object.keys(audit.compile).length > 0,
            j(audit.compile).slice(0, 70));

  chk.section('[W7] 长文档、滚动与命中测试');
  const long = await c.eval(body(
    'const h = window.__wheditor;' +
    'const a = performance.now();' +
    'await h.run(7, 1.5);' +
    'const loadMs = performance.now() - a;' +
    'const pages = h.editStats.totalPages;' +
    'const before = { first: h.geom.first, ink: await h.ink(), self: await h.selfCheck() };' +
    'const step = h.geom.pageH + h.geom.gap;' +
    'const b = performance.now();' +
    'await h.setScroll((3 * step + 30) / h.geom.dpr);' +
    'const scrollMs = performance.now() - b;' +
    'const after = { first: h.geom.first, ink: await h.ink(), self: await h.selfCheck() };' +
    'const hit = await h.hit(40, 20);' +
    'return { loadMs, pages, before, after, scrollMs, hit };'));
  chk.check('长文档真的分成了很多页', long.pages > 50,
            `${long.pages} 页 / 装载 ${long.loadMs.toFixed(0)} ms`);
  chk.check('滚到第 4 页后窗口跟着走', long.after.first > long.before.first,
            `${long.before.first} -> ${long.after.first}`);
  chk.check('滚动后帧上仍有字', long.after.ink > 1000, `${long.before.ink} -> ${long.after.ink}`);
  chk.check('滚动后画布仍与帧逐像素一致（窗口换了要整窗重贴）',
            long.after.self.ratio < 0.0005 && long.after.self.missing === 0,
            `比例 ${long.after.self.ratio.toFixed(6)}（滚前 ${long.before.self.ratio.toFixed(6)}）`);
  chk.check('命中测试给出归属或 null', long.hit === null || typeof long.hit === 'object',
            j(long.hit));

  chk.section('[W8] 字体按需装载在 Worker 里也走得通');
  const lazy = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.reloadFonts("lazy");' +
    'const mode = h.fontMode;' +
    'const ink = await h.ink();' +
    'const self = await h.selfCheck();' +
    'return { mode, ink, self, missing: h.editStats.missingGlyphCount };'));
  chk.check('切到按需装载后宿主记到了模式', lazy.mode === 'lazy', j(lazy.mode));
  chk.check('按需装载后帧上仍有字', lazy.ink > 500, `${lazy.ink} px`);
  chk.check('按需装载后每页画布仍与帧一致', lazy.self.ratio < 0.0005,
            `比例 ${lazy.self.ratio.toFixed(6)}`);

  chk.section('[W9] 三类通道的代价');
  const cost = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.run(6, 1.5);' +
    'const keys = [];' +
    'for (let i = 0; i < 20; ++i) { const a = performance.now();' +
    '  await h.type("测"); keys.push(performance.now() - a); }' +
    'keys.sort((x, y) => x - y);' +
    'const asks = [];' +
    'for (let i = 0; i < 100; ++i) { const a = performance.now();' +
    '  await h.ask("editStats", {}); asks.push(performance.now() - a); }' +
    'asks.sort((x, y) => x - y);' +
    'return { keys: { median: keys[10], max: keys[19] },' +
    '         asks: { medianUs: +(asks[50] * 1000).toFixed(1), p95Us: +(asks[95] * 1000).toFixed(1) },' +
    '         seq: h.blitStats.seq };'));
  chk.check(`按键端到端中位 ${cost.keys.median.toFixed(1)} ms（一帧 16.7 ms）`,
            cost.keys.median < 16.7, `最大 ${cost.keys.max.toFixed(1)} ms`);
  chk.check(`查询往返中位 ${cost.asks.medianUs} us`, cost.asks.medianUs < 2000,
            `p95 ${cost.asks.p95Us} us`);
  chk.check('推送在持续更新（stateSeq 在涨）', cost.seq > 20, `seq=${cost.seq}`);

  chk.section('[W10] 一窗多页 + 换窗：每页一张画布，换窗时页集合跟着换');
  // 这一条要的是"窗口里同时有好几页"（浏览器窗口开得比一页高），因为两件事只有在这种
  //   窗口下才验得到：① 每页都有自己的画布、各自与帧对应段逐像素相等；
  //   ② 换窗时旧页的画布要**丢掉并新建** —— 这就是刚才踩的坑（复用已转移的 canvas
  //   会抛 InvalidStateError，transferControlToOffscreen 是一次性的）。
  // 用缩小让一页变矮，窗口里自然就装下好几页 —— 比去斗浏览器窗口尺寸可靠得多。
  const multi = await c.eval(body(
    'const h = window.__wheditor;' +
    // 先换成多页文档：单页文档里窗口永远只有 1 页，这一节就验不到任何东西。
    'await h.run(7, 1.5);' +
    'await h.zoomTo(0.5);' +
    'const before = { count: h.geom.count, pages: h.pageCanvasList, first: h.geom.first,' +
    '                 self: await h.selfCheck() };' +
    'const step = h.geom.pageStep;' +
    // 滚够 3 页：窗口是按"视口能装几页 + 2 页余量"开的，滚 2 页可能整段落在余量里
    //   （那是对的，不是漏洞）—— 那样子集合本来就不该换，这条断言也就验不到东西。
    'await h.setScroll((h.geom.count * step + step) / h.geom.dpr);' +
    'const after = { count: h.geom.count, pages: h.pageCanvasList, first: h.geom.first,' +
    '                self: await h.selfCheck() };' +
    'await h.zoomTo(1.5);' +
    'return { before, after, count: h.pageCanvasCount, errs: h.errors.slice() };'));
  chk.check('缩小之后窗口里同时装了好几页', multi.before.count >= 2,
            `窗口 ${multi.before.first}+${multi.before.count} 页`);
  chk.check('每页都有自己的画布',
            multi.before.pages.length === multi.before.count,
            `页画布 ${j(multi.before.pages)} / 窗口 ${multi.before.count} 页`);
  chk.check('多页窗口下每页都与帧对应段逐像素相等',
            multi.before.self.ratio < 0.0005 && multi.before.self.missing === 0,
            `比例 ${multi.before.self.ratio.toFixed(6)} / 缺页 ${multi.before.self.missing}`);
  chk.check('换窗后页集合跟着换（旧页画布丢掉、新页新建）',
            multi.after.first > multi.before.first &&
            multi.after.pages[0] === multi.after.first &&
            multi.after.pages.length === multi.after.count,
            `换前 ${j(multi.before.pages)} → 换后 ${j(multi.after.pages)}`);
  chk.check('换窗后每页仍与帧逐像素相等',
            multi.after.self.ratio < 0.0005 && multi.after.self.missing === 0,
            `比例 ${multi.after.self.ratio.toFixed(6)} / 缺页 ${multi.after.self.missing}`);
  chk.check('换窗没有产生任何错误（画布都是新建的，没有复用已转移的）',
            multi.errs.length === 0, j(multi.errs));

  chk.section('[W11] 滚动的代价（一页一张画布换来的就是这一段）');
  // 一页一张画布 → 画布在文档流里 → **滚动是浏览器搬像素**，引擎一次都不跑。
  //   所以两段必须分开量：窗口没变的那一半应当接近"只有一次消息往返"，
  //   窗口变了的那一半才要整帧重光栅（一页约 13 ms）。混在一起只有一个平均数，
  //   看不出"贵在哪"，也就没法判断这个方案到底赢没赢。
  const scroll = await c.eval(body(
    'const h = window.__wheditor;' +
    'await h.run(7, 1.5);' +
    'const step = h.geom.pageH + h.geom.gap;' +
    'const dpr = h.geom.dpr;' +
    'await h.setScroll(10 / dpr);' +
    'await h.setScroll(20 / dpr);' +   // 预热：首拍含 JIT 与首次重贴，不能算进"最坏值"
    'const firstBefore = h.geom.first;' +
    'const inWin = [];' +
    'for (let i = 1; i <= 6; ++i) {' +
    '  const t = performance.now();' +
    '  await h.setScroll((10 + i * 20) / dpr);' +
    '  inWin.push(+(performance.now() - t).toFixed(2));' +
    '}' +
    'const firstAfter = h.geom.first;' +
    'const cross = [];' +
    'let changes = 0; let prev = h.geom.first;' +
    'for (let i = 0; i < 6; ++i) {' +
    '  const t = performance.now();' +
    '  await h.setScroll(((i + 3) * step) / dpr);' +
    '  cross.push(+(performance.now() - t).toFixed(2));' +
    '  if (h.geom.first !== prev) ++changes;' +
    '  prev = h.geom.first;' +
    '}' +
    // ★ 换窗之后要等"新页的画布到了"再做逐像素比对。这不是掩盖问题，而是这一套设计的
    //   固有节奏：窗口由引擎改，画布由主线程建并把所有权交过来 —— 中间隔着一趟往返，
    //   那段窗口期里新页本来就没有画布（geom.pagesReady 就是为此存在的信号）。
    //   不等的话量到的是"比了 0 px、缺页 1"，看着像画布贴错了。
    'for (let i = 0; i < 200 && !h.geom.pagesReady; ++i) {' +
    '  await new Promise((r) => setTimeout(r, 10));' +
    '}' +
    'return { step, firstBefore, firstAfter, inWin, cross, changes,' +
    '         self: await h.selfCheck(), pages: h.editStats.totalPages };'));
  chk.check('同一页内来回滚动不换窗口（滞回在起作用）',
            scroll.firstBefore === scroll.firstAfter,
            `窗口首 ${scroll.firstBefore} -> ${scroll.firstAfter}`);
  // 断言取**中位**：窗口没变时这一拍只是"一条消息 + 一次会话"，量级应在 1 ms 上下；
  //   单个样本偶发 4 ms 级抖动（GC）不该把结论说成"这一拍很贵"。最坏值照样打出来。
  const medIn = [...scroll.inWin].sort((a, b) => a - b)[scroll.inWin.length >> 1];
  chk.check(`同一页内滚动中位 ${medIn.toFixed(2)} ms（最坏 ${Math.max(...scroll.inWin).toFixed(2)} ms）`,
            medIn < 3, scroll.inWin.join(' / '));
  // 6 次单页滚动一共跨了 6 页，而窗口封顶 3 页 ⇒ 必然换过窗。
  //   这条是滞回的反向一半：上面两条都在说"不要乱换窗"，没有它，一个"永不更新"的
  //   实现也能让那两条全绿。阈值取 3 而不是 5：窗口现在留余量（以可见页为中心开满
  //   3 页），一页滚动大多落在余量里不动，实测 6 次跨页滚动换 3 次 ——
  //   这正是"换窗次数少一个量级"的正面证据，不是滞回失效。
  chk.check('跨页滚动确实会换窗口（滞回不是"永不更新"）',
            scroll.changes >= 3, `换了 ${scroll.changes} / 6 次`);
  chk.check('跨页滚动后画布仍与帧逐像素一致', scroll.self.ratio < 0.0005,
            `比例 ${scroll.self.ratio.toFixed(6)}` +
              `｜比了 ${scroll.self.compared} px、缺页 ${scroll.self.missing}、尺寸不符 ${scroll.self.sizeBad}` +
              `｜窗口 ${scroll.self.window.first}+${scroll.self.window.count}`);
  chk.check('滚动没有破坏文档', scroll.pages > 50, `${scroll.pages} 页`);

  chk.section('[W13] 真实输入事件：点击落光标、焦点不被抢走、移动即重置闪烁');
  // 这一节的入口是真实事件，不是门面调用 —— 门面调用会绕过 keydown / mousedown
  //   这两个入口本身，而用户报的问题恰好就出在入口上。
  await c.eval('return window.__wheditor.run(6, 1.5);');   // 换回可编辑单页文档
  await c.waitFor('return !!(window.__wheditor.geom && window.__wheditor.geom.pagesReady)',
                  'page canvases after reset');
  const box = await c.eval(
    'const el = document.querySelector("canvas.pg");' +
    'if (!el) return null;' +
    'const r = el.getBoundingClientRect();' +
    'return { x: Math.round(r.left + r.width / 2),' +
    '         y: Math.round(r.top + Math.min(200, r.height / 3)) };');
  await c.eval('window.__wheditor.setCaret(0, 0); return true;');
  await sleep(300);
  const beforeClick = await caretOf(c);
  await clickAt(c, box.x, box.y);
  await sleep(350);
  const afterClick = await caretOf(c);
  chk.check('真鼠标点击让光标落过去（命中测试确实跑了）',
            afterClick.off !== beforeClick.off && afterClick.x !== beforeClick.x,
            `偏移 ${beforeClick.off} → ${afterClick.off}，x ${beforeClick.x} → ${afterClick.x}`);
  const focus = await c.eval(
    'const a = document.activeElement;' +
    'return { id: a ? a.id : null, isTa: a === window.__wheditor.textarea };');
  // ★ 这一条是"点击之后键盘整个失灵"的解：canvas 不可聚焦，Chrome 在 mousedown 的
  //   默认行为里会把 activeElement 打回 body，此后 keydown 到不了宿主。
  //   必须在指针处理里显式 ta.focus()（单线程宿主也一样）。
  chk.check('点击之后焦点仍在输入层上（否则键盘会整个失灵）',
            focus.isTa === true, `activeElement = ${focus.id}`);
  await pressKey(c, 'ArrowLeft', 37);
  await sleep(300);
  const afterClickKey = await caretOf(c);
  chk.check('点击之后键盘仍然能动光标',
            afterClickKey.off !== afterClick.off,
            `偏移 ${afterClick.off} → ${afterClickKey.off}`);

  // 闪烁：等相位落到隐藏，再按一下方向键 —— 必须立刻回到可见，且下一次翻转被推后整拍。
  let sawHidden = true;
  try {
    await c.waitFor(
      'return !!(window.__wheditor && window.__wheditor.stats)' +
      ' && window.__wheditor.editStats.caretVisible === false', '相位落到隐藏', 6000);
  } catch { sawHidden = false; }
  chk.check('闪烁在跑（6 秒内能等到隐藏相位）', sawHidden, sawHidden ? '' : '一直没隐藏');
  if (sawHidden) {
    await pressKey(c, 'ArrowRight', 39);
    await sleep(140);
    const v = (await caretOf(c))?.vis;
    chk.check('隐藏相位下按方向键，光标立刻回到可见', v === true, `vis=${v}`);
  }
  const seq = [];
  for (let i = 0; i < 5; ++i) {
    const left = i % 2 === 1;
    await pressKey(c, left ? 'ArrowLeft' : 'ArrowRight', left ? 37 : 39);
    await sleep(120);
    seq.push((await caretOf(c))?.vis);
  }
  chk.check('连按 5 次，每次按完光标都是可见的（移动即重置闪烁）',
            seq.every((v) => v === true), j(seq));
  const idle = [];
  for (let i = 0; i < 8; ++i) { idle.push((await caretOf(c))?.vis); await sleep(230); }
  chk.check('空闲时闪烁仍在翻转（没被"移动即可见"弄停）',
            new Set(idle).size > 1, j(idle));

  chk.section('[W14] 缩放：显示尺寸与几何都要跟着倍率走');
  // 这一段是从一个页面上看得见的缺陷反推出来的：画布的 CSS 尺寸只在新建画布时设过一次，
  //   缩放之后背衬（Worker 按新倍率重画的页宽页高）变了而显示尺寸没变 ⇒ 整页按旧尺寸
  //   显示新倍率的内容，表现是"缩放之后整页偏小兼发虚"。判据取两条：画布的矩形宽高必须
  //   等于几何里的页宽页高（这才是"缩放落到画布上"）；同一光标位置的帧内坐标必须按倍率
  //   成比例（这才是"缩放真的重排了"）。
  // 传入的 z 是密度，写进输入框要换成倍数；断言与几何仍是密度口径。
  const zoomGeom = async (z) => {
    await c.eval(
      'const el = document.getElementById("zoom");' +
      'el.value = String(' + uiZoom(z) + ');' +
      'el.dispatchEvent(new Event("change"));' +
      'return true;');
    await c.waitFor(
      'return !!(window.__wheditor.geom && window.__wheditor.editStats)' +
      ' && Math.abs(window.__wheditor.zoomLevel - ' + z + ') < 1e-6' +
      ' && window.__wheditor.geom.pagesReady', '缩放 ' + z + ' 落地');
    await c.eval('return window.__wheditor.setCaret(0, 1);');
    // 光标也要等它真的落地：这是一个异步宿主，读早了拿到的是上一拍的位置，
    //   而两个倍率读的若是不同位置，最后那条比例判据会变成噪声。
    await c.waitFor(
      'const s = window.__wheditor.editStats;' +
      'return !!s && s.caretPara === 0 && s.caretOffset === 1;', '光标停到 (0,1)');
    await sleep(150);
    return c.eval(
      'const h = window.__wheditor;' +
      'const el = document.querySelector("canvas.pg");' +
      'const r = el.getBoundingClientRect();' +
      'return { zoom: h.zoomLevel, geom: [h.geom.pageW, h.geom.pageH], dpr: h.geom.dpr,' +
      '         rect: [Math.round(r.width), Math.round(r.height)],' +
      '         style: [el.style.width, el.style.height],' +
      '         caretX: h.editStats.caretX };');
  };
  const z15 = await zoomGeom(1.5);
  const z20 = await zoomGeom(2);
  chk.check('改 #zoom 输入框（真实 change 事件）之后 zoomLevel 跟着变（输入框写倍数，回读是密度）',
            z15.zoom === 1.5 && z20.zoom === 2, `${z15.zoom} → ${z20.zoom}`);
  chk.check('画布矩形宽高等于几何的页宽页高（1.5 倍）',
            z15.rect[0] === Math.round(z15.geom[0] / z15.dpr) &&
            z15.rect[1] === Math.round(z15.geom[1] / z15.dpr),
            `矩形 ${j(z15.rect)}，几何 ${j(z15.geom)} / dpr ${z15.dpr}`);
  chk.check('画布矩形宽高等于几何的页宽页高（2 倍）',
            z20.rect[0] === Math.round(z20.geom[0] / z20.dpr) &&
            z20.rect[1] === Math.round(z20.geom[1] / z20.dpr),
            `矩形 ${j(z20.rect)}，几何 ${j(z20.geom)} / dpr ${z20.dpr}`);
  chk.check('画布显示尺寸确实变大了（不是只有内部状态变）',
            z20.rect[0] > z15.rect[0] && z20.rect[1] > z15.rect[1],
            `${j(z15.rect)} → ${j(z20.rect)}`);
  // 反向一半：显示尺寸对了不等于缩放真的重排了。同一光标位置的帧内 x 必须按倍率成比例 ——
  //   少了这一条，"只改样式不改排版"同样能让上面几条通过。
  const ratio = z20.caretX / z15.caretX;
  chk.check('同一光标位置的帧内坐标按倍率成比例（缩放真的重排了）',
            Math.abs(ratio - 2 / 1.5) < 0.02,
            `caretX ${z15.caretX} → ${z20.caretX}，比值 ${ratio.toFixed(4)}（应为 ${(2 / 1.5).toFixed(4)}）`);
  await zoomGeom(1.5);

  chk.section('[W12] 错误面');
  const errs = await c.eval('return window.__wheditor.errors');
  chk.check('宿主与 Worker 都没有报错', errs.length === 0, j(errs));

  say('');
  say(`      同一页内滚动（不换窗口）：${scroll.inWin.join(' / ')} ms`);
  say(`      跨页滚动（换窗口重光栅）：${scroll.cross.join(' / ')} ms`);

  say('');
  say('──────────────────────────────');
  say(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  process.exit(chk.failures === 0 ? 0 : 1);
}

main().catch((e) => {
  say('FATAL ' + String((e && e.stack) || e));
  process.exit(1);
});
