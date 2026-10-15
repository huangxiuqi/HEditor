// 多实例探针：同页三份实例（两个单线程 + 一个 Worker）必须互不干扰。
//
// 页面上跑的是 web/multi.html：每个实例有自己的容器，容器里各有自己的
//   #stage / #pages / #ime（id 重复是有意的 —— 两个宿主都只在容器内查找元素）。
//
// 判据分三层：
//   独立性：各有各的画布与引擎，往 A 敲字不改 B 的字符数。
//   隔离性：A 上的一次失败只出现在 A 的错误界面上，B 与 C 的记账仍然是空的。
//   可拆性：dispose(A) 之后 A 不再报告任何内容，而 B 与 C 照常能编辑。
//
// 用法：scripts\verify_web.ps1 不合适（它只开一个页面），用
//   bash out/_run.sh scripts/multi_probe.mjs http://127.0.0.1:8781/web/multi.html out/_multi.log
import { CDP, Checks } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};

const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/multi.html');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const chk = new Checks('多实例');

async function findTarget() {
  for (let i = 0; i < 60; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page;
    } catch { /* 守护还没起来 */ }
    await sleep(500);
  }
  throw new Error(`连不上 Chrome 调试端口 ${PORT}`);
}

async function main() {
  const target = await findTarget();
  const cdp = await CDP.connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Log.enable');
  await cdp.disableHttpCache();
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 1500, height: 1400, deviceScaleFactor: 1, mobile: false,
  });

  console.log('→ 打开', URL_);
  await cdp.send('Page.navigate', { url: URL_ });

  const RAW = cdp.eval.bind(cdp);
  const asBody = (b) => 'return (async () => {\n' + b + '\n})();';
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;

  /**
   * 求值包装：表达式统一包 async IIFE，跑完把每一个还没拆掉的实例排空。
   *
   * 这一页没有 window.__heditor（演示页才挂那个名字），排空要遍历 window.__editors ——
   *   异步宿主上门面全是异步的，不排空就会读到上一拍的内容。
   */
  cdp.eval = async (expr) => {
    try { new AsyncFn(expr); } catch (e) {
      throw new Error('表达式语法错误：' + e.message + '\n---- 表达式原文 ----\n' + expr);
    }
    const r = await RAW(asBody(expr));
    // 排空的那一段也必须包 async IIFE：里面有 await，裸着发出去会直接抛
    //   "await is only valid in async functions"（这个写法在探针里出过两次）。
    await RAW(asBody('const all = Object.values(window.__editors || {});' +
                     'for (const h of all) if (h.quiesce && !h.disposed) await h.quiesce();' +
                     'return true;'));
    return r;
  };

  const E = 'window.__editors';
  const A = `${E}['ed-a']`;
  const B = `${E}['ed-b']`;
  const C = `${E}['ed-c']`;

  // 前提：三份都起来了

  for (let i = 0; i < 150; i++) {
    const all = await cdp.eval(
      `const eds = Object.values(${E} || {});` +
      'return eds.length === 3 && eds.every((e) => e.ready === true);');
    if (all) break;
    await sleep(400);
  }

  console.log('\n[P1] 三份实例各自启动');
  const boot = await cdp.eval(
    `const eds = Object.values(${E});` +
    'const rs = await Promise.all(eds.map((e) => e.whenReady));' +
    'return { ok: rs.map((r) => r.ok), ready: eds.map((e) => e.ready),' +
    '         same: new Set(eds.map((e) => e.container)).size,' +
    '         kinds: window.__edCases.map((c) => c.kind) };');
  chk.check('三份都启动成功', boot.ok.every((v) => v), JSON.stringify(boot.ok));
  chk.check('三份都处于就绪态', boot.ready.every((v) => v === true), JSON.stringify(boot.ready));
  chk.check('三份挂的是三个不同的容器', boot.same === 3, `不同容器 ${boot.same} 个`);
  chk.check('两种传输都在场', boot.kinds.join(',') === 'in-thread,in-thread,worker',
            boot.kinds.join(','));

  // 独立性：画布与内容

  console.log('\n[P2] 各有一份画布与内容');
  const own = await cdp.eval(
    `const a = ${A}, b = ${B}, c = ${C};` +
    'const n = (h) => h.container.querySelectorAll("canvas").length;' +
    'return { a: n(a), b: n(b), c: n(c),' +
    '         aPages: a.editStats.totalPages, bPages: b.editStats.totalPages,' +
    '         cPages: c.editStats.totalPages,' +
    '         sameText: a.text === b.text };');
  chk.check('每个容器里都有自己的页画布',
            own.a >= 1 && own.b >= 1 && own.c >= 1, JSON.stringify([own.a, own.b, own.c]));
  chk.check('三份各自排了版', own.aPages >= 1 && own.bPages >= 1 && own.cPages >= 1,
            `${own.aPages} / ${own.bPages} / ${own.cPages} 页`);
  chk.check('初始内容相同（同一场景，作为后面的对照基线）', own.sameText === true);

  console.log('\n[P3] 改 A 不动 B 与 C');
  const typeA = await cdp.eval(
    `const a = ${A}, b = ${B}, c = ${C};` +
    'const n = (h) => h.editStats.docCodepoints;' +
    'a.setCaret(0, 0);' +
    'const before = { a: n(a), b: n(b), c: n(c) };' +
    'a.type("X");' +
    'const after = { a: n(a), b: n(b), c: n(c) };' +
    'return { before, after, aHead: a.text.slice(0, 4), bHead: b.text.slice(0, 4) };');
  chk.check('A 的码点数加了 1',
            typeA.after.a === typeA.before.a + 1,
            `${typeA.before.a} → ${typeA.after.a}`);
  chk.check('B 的码点数没动（这是"独立"的核心判据）',
            typeA.after.b === typeA.before.b, `${typeA.before.b} → ${typeA.after.b}`);
  chk.check('C 的码点数没动', typeA.after.c === typeA.before.c);
  chk.check('A 与 B 的正文已经不同', typeA.aHead !== typeA.bHead,
            `A="${typeA.aHead}" B="${typeA.bHead}"`);

  console.log('\n[P4] 光标各自独立');
  const caret = await cdp.eval(
    `const a = ${A}, b = ${B};` +
    'a.setCaret(0, 3);' +
    'const aOff = a.caret.offset;' +
    'b.setCaret(0, 0);' +
    'return { aOff, aAfterB: a.caret.offset, bOff: b.caret.offset };');
  chk.check('A 的光标停在 3', caret.aOff === 3, `offset=${caret.aOff}`);
  chk.check('动 B 的光标不会挪动 A 的光标', caret.aAfterB === 3, `A=${caret.aAfterB}`);
  chk.check('B 的光标停在 0', caret.bOff === 0, `offset=${caret.bOff}`);

  console.log('\n[P5] 缩放各自独立，且缩放要落到画布上');
  const zoom = await cdp.eval(
    `const a = ${A}, b = ${B}, c = ${C};` +
    // 画布显示尺寸以矩形为准：Worker 宿主那侧的 width/height 属性已经被转移作废，
    //   只剩 CSS 尺寸算数，而"缩放没落到画布上"正是从这两个数之间露出来的。
    'const box = (h) => {' +
    '  const el = h.container.querySelector("canvas");' +
    '  const r = el.getBoundingClientRect();' +
    '  return [Math.round(r.width), Math.round(r.height),' +
    '          el.style.width, el.style.height, el.width, el.height];' +
    '};' +
    'a.run(6, 2);' +
    'c.zoomTo(2);' +
    'await c.present();' +
    'await new Promise((r) => setTimeout(r, 400));' +
    'return { a: a.zoomLevel, b: b.zoomLevel, c: c.zoomLevel,' +
    '         abox: box(a), cbox: box(c) };');
  chk.check('A 缩放到 2.0', zoom.a === 2, `zoomLevel=${zoom.a}`);
  chk.check('B 仍是 1.5（缩放不是页面级状态）', zoom.b === 1.5, `zoomLevel=${zoom.b}`);
  chk.check('C（Worker 那一路）也缩放到 2.0', zoom.c === 2, `zoomLevel=${zoom.c}`);
  chk.check('C 的画布显示尺寸与 A 逐项相同',
            zoom.cbox[0] === zoom.abox[0] && zoom.cbox[1] === zoom.abox[1],
            `A=${JSON.stringify(zoom.abox)} C=${JSON.stringify(zoom.cbox)}`);

  console.log('\n[P5b] 缩放输入框走真实 change 事件，画布要跟着变');
  // 上面那一条走的是门面调用，绕过了页面的输入框本身。而"缩放不对"这类问题恰好出在
  //   入口上（门面调对了、用户点不动），所以这里补一遍真实事件：改值 + 派发 change。
  const zoomUi = await cdp.eval(
    `const c = ${C};` +
    'const el = c.container.querySelector("#zoom");' +
    'const w = () => Math.round(c.container.querySelector("canvas").getBoundingClientRect().width);' +
    'const at2 = w();' +
    'el.value = "2.5";' +
    'el.dispatchEvent(new Event("change"));' +
    'return { before: at2, inputValue: el.value };');
  // 异步宿主：等它在页面上真的落地（每轮求值都会顺带排空，这里再等一拍稳态）。
  for (let i = 0; i < 40; i++) {
    const now = await cdp.eval(`return ${C}.zoomLevel;`);
    if (now === 2.5) break;
    await sleep(150);
  }
  await sleep(300);
  const zoomUi2 = await cdp.eval(
    `const c = ${C};` +
    'const el = c.container.querySelector("canvas");' +
    'const r = el.getBoundingClientRect();' +
    'return { zoom: c.zoomLevel, rect: [Math.round(r.width), Math.round(r.height)],' +
    '         geom: [c.geom.pageW, c.geom.pageH], dpr: c.geom.dpr,' +
    '         inputValue: c.container.querySelector("#zoom").value };');
  chk.check('改输入框之后 C 的 zoomLevel 真的变了（入口通到了宿主）',
            zoomUi2.zoom === 2.5, `zoomLevel=${zoomUi2.zoom}`);
  chk.check('画布显示尺寸等于几何的页宽页高（缩放落到画布上）',
            zoomUi2.rect[0] === Math.round(zoomUi2.geom[0] / zoomUi2.dpr) &&
            zoomUi2.rect[1] === Math.round(zoomUi2.geom[1] / zoomUi2.dpr),
            `矩形 ${JSON.stringify(zoomUi2.rect)}，几何 ${JSON.stringify(zoomUi2.geom)}`);
  chk.check('放大之后画布尺寸确实变大（反向一半：不是只改了内部状态）',
            zoomUi2.rect[0] > zoomUi.before,
            `${zoomUi.before} → ${zoomUi2.rect[0]}（zoom ${zoomUi2.zoom}）`);

  // 隔离性：错误面

  console.log('\n[P6] A 的失败不出现在 B 与 C 上');
  const fail = await cdp.eval(
    `const a = ${A}, b = ${B}, c = ${C};` +
    'const orig = window.fetch;' +
    'window.fetch = (u, o) => String(u).includes("medical-records")' +
    '  ? Promise.resolve(new Response("", { status: 500 })) : orig(u, o);' +
    'const sel = a.container.querySelector("#scenario");' +
    'sel.value = "8";' +
    'sel.dispatchEvent(new Event("change"));' +
    'await new Promise((r) => setTimeout(r, 700));' +
    'window.fetch = orig;' +
    'const box = (h) => !!h.container.querySelector("[data-host-error]");' +
    'return { a: a.hostErrors.entries.length, b: b.hostErrors.entries.length,' +
    '         c: c.hostErrors.entries.length,' +
    '         aBox: box(a), bBox: box(b), cBox: box(c),' +
    '         aFatal: a.hostErrors.fatalActive, bFatal: b.hostErrors.fatalActive };');
  chk.check('A 记下了这次失败', fail.a >= 1, `entries=${fail.a}`);
  chk.check('提示条只出现在 A 的容器里', fail.aBox === true && fail.bBox === false &&
            fail.cBox === false, JSON.stringify([fail.aBox, fail.bBox, fail.cBox]));
  chk.check('B 的记账仍是空的', fail.b === 0, `entries=${fail.b}`);
  chk.check('C 的记账仍是空的', fail.c === 0, `entries=${fail.c}`);
  chk.check('一次装载失败没有升级成致命', fail.aFatal === false && fail.bFatal === false);

  // 可拆性

  console.log('\n[P7] 拆掉 A 之后 B 与 C 照常工作');
  const gone = await cdp.eval(
    `const a = ${A};` +
    'const ret = a.dispose();' +
    'return { ret, disposed: a.disposed, ready: a.ready, text: a.text,' +
    '         canvases: a.container.querySelectorAll("canvas").length,' +
    '         box: !!a.container.querySelector("[data-host-error]") };');
  chk.check('dispose 返回真（第一次拆）', gone.ret === true);
  chk.check('拆掉之后不再报告内容', gone.text === '' && gone.ready === false,
            `text="${gone.text}" ready=${gone.ready}`);
  chk.check('自己的画布已从文档里摘掉', gone.canvases === 0, `剩 ${gone.canvases} 张`);
  chk.check('自己的错误界面也抹掉了', gone.box === false);

  const survive = await cdp.eval(
    `const b = ${B}, c = ${C};` +
    'const n = (h) => h.editStats.docCodepoints;' +
    'const b0 = n(b), c0 = n(c);' +
    'b.setCaret(0, 0);' +
    'b.type("Y");' +
    'await c.present();' +
    'return { b: [b0, n(b)], c: [c0, n(c)], bPages: b.editStats.totalPages,' +
    '         cPages: c.editStats.totalPages, bZoom: b.zoomLevel, cZoom: c.zoomLevel };');
  chk.check('B 仍能编辑（拆邻居不影响它）', survive.b[1] === survive.b[0] + 1,
            `${survive.b[0]} → ${survive.b[1]}`);
  chk.check('B 还记得自己的缩放', survive.bZoom === 1.5, `zoomLevel=${survive.bZoom}`);
  chk.check('C（Worker 那一路）仍然可用', survive.cPages >= 1, `${survive.cPages} 页`);

  console.log('\n[P8] 全程没有页面级异常');
  const errs = await cdp.eval(
    'const eds = Object.values(window.__editors);' +
    'return eds.map((e) => e.errors.slice());');
  const flat = errs.flat();
  chk.check('各实例的 errors 都是空的', flat.length === 0, JSON.stringify(flat).slice(0, 200));
  chk.check('没有 console 错误', cdp.consoleErrors.length === 0,
            JSON.stringify(cdp.consoleErrors).slice(0, 200));

  console.log('');
  console.log('──────────────────────────────');
  console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  process.exit(chk.failures ? 1 : 0);
}

main().catch((e) => {
  console.log('FATAL ' + String((e && e.stack) || e));
  process.exit(1);
});
