// 错误降级探针：验"失败会变成页面上看得见的东西"。
//
// 要证伪的事：宿主此前的失败只有 errors 数组知道（数组只有探针会读），页面上要么空白、
//   要么停在一个看起来能输入、实际打不进字的编辑器上。这里按三档取证：
//
//   正常态：页面上不该有任何错误界面（反向一半 —— 没有这一半，"永远显示横幅"也能算过）。
//   单次失败：出现自动消失的提示条，且编辑器随后仍然可用（不是所有失败都该致命）。
//   致命失败：出现常驻横幅、元素真的可见、容器被标记不可用，且此后的操作不再假装成功。
//
// 致命那一档用 ?wasm= 指向一个不存在的产物目录来造：这是真实会发生的配置错误，
//   不需要往产品代码里塞测试开关。两宿主都受影响（单线程那边是胶水脚本加载失败，
//   Worker 那边是 importScripts 失败）。
//
// 两宿主通吃，用法：
//   scripts\verify_web.ps1 -Probe scripts\error_probe.mjs                    # 单线程宿主
//   scripts\verify_web.ps1 -Probe scripts\error_probe.mjs -Page worker.html  # Worker 宿主
import { CDP, Checks } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};

const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/index.html');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const chk = new Checks('错误降级');
const check = (name, cond, detail) => chk.check(name, cond, detail);

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

// 把 URL 的 wasm 参数换成别的值（用来造"产物不存在"）。
function withWasm(url, value) {
  const u = new URL(url);
  u.searchParams.set('wasm', value);
  return u.href;
}

async function main() {
  const target = await findTarget();
  const cdp = await CDP.connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.disableHttpCache();

  const RAW = cdp.eval.bind(cdp);
  const asBody = (b) => 'return (async () => {\n' + b + '\n})();';
  cdp.eval = async (expr) => {
    const r = await RAW(asBody(expr));
    await RAW('const H = window.__heditor;' +
              'return H && H.quiesce ? H.quiesce().then(() => true) : true;');
    return r;
  };

  const waitReady = async (wantReady) => {
    for (let i = 0; i < 120; i++) {
      const st = await cdp.eval(
        'const H = window.__heditor; return H ? !!H.ready : false;').catch(() => false);
      if (st === wantReady) return true;
      await sleep(400);
    }
    return false;
  };

  console.log('→ 打开', URL_);
  await cdp.send('Page.navigate', { url: URL_ });
  const ready = await waitReady(true);
  console.log(`→ 页面就绪 ${ready}`);

  const probe = async (body) => cdp.eval(body);

  // 第一档：正常态

  console.log('\n[E1] 正常态：不该有任何错误界面');
  const normal = await probe(
    'const h = window.__heditor;' +
    'return { ready: !!h.ready, fatal: h.hostErrors.fatalActive,' +
    '         entries: h.hostErrors.entries.length,' +
    '         banner: !!document.querySelector("[data-host-error=\\"fatal\\"]"),' +
    '         toast: !!document.querySelector("[data-host-error=\\"warn\\"]") };');
  check('启动成功（前置条件）', normal.ready, JSON.stringify(normal));
  check('没有致命横幅', !normal.fatal && !normal.banner);
  check('没有提示条残留', !normal.toast);
  check('降级记账为空（反向一半）', normal.entries === 0, `entries=${normal.entries}`);

  // 第二档：单次操作失败

  console.log('\n[E2] 单次失败：提示一次，编辑器继续可用');
  const isWorker = await probe('return !!window.__wheditor;');
  // 两宿主各自找一个真实可达的单次失败：
  //   Worker 宿主：查询表里没有的名字（引擎抛"未知查询"，只影响这一条）。
  //   单线程宿主：真病历样例读不到（把那条 fetch 换成 500，走工具条那条真实路径）。
  const spot = isWorker
    ? await probe(
        'let msg = "";' +
        'try { await window.__heditor.ask("__no_such_query__"); } catch (e) { msg = String(e.message); }' +
        'return { msg: msg.slice(0, 80), kind: "unknown-query" };')
    : await probe(
        'const orig = window.fetch;' +
        'window.fetch = (u, o) => String(u).includes("medical-records")' +
        '  ? Promise.resolve(new Response("", { status: 500 })) : orig(u, o);' +
        'const sel = document.getElementById("scenario");' +
        'sel.value = "8";' +
        'sel.dispatchEvent(new Event("change"));' +
        'await new Promise((r) => setTimeout(r, 600));' +
        'window.fetch = orig;' +
        'return { msg: "scenario 8 fetch 500", kind: "sample-500" };');
  console.log(`     造法：${spot.kind}`);
  const afterOp = await probe(
    'const h = window.__heditor;' +
    'return { fatal: h.hostErrors.fatalActive, dead: h.hostErrors.dead,' +
    '         warns: h.hostErrors.entries.filter((e) => e.level === "warn").length,' +
    '         last: (h.hostErrors.entries.slice(-1)[0] || {}).text || "",' +
    '         toast: !!document.querySelector("[data-host-error=\\"warn\\"]") };');
  check('记下了一条降级（warn）', afterOp.warns >= 1, `warns=${afterOp.warns}：${afterOp.last}`);
  check('没有升级成致命', !afterOp.fatal);
  check('引擎没有被标死', afterOp.dead === false);
  check('页面上有提示条', afterOp.toast === true,
        'DOM 里应存在 [data-host-error="warn"]');
  // 反向一半：失败之后编辑器必须还能用。只看"没有致命"不够 —— 引擎可能已经
  //   失效而没有任何标志位反映出来。
  const alive = await probe(
    'const h = window.__heditor;' +
    'await h.present();' +
    'return { pages: h.editStats ? h.editStats.totalPages : -1,' +
    '         chars: h.text.length };');
  check('失败之后编辑器仍然可用', alive.pages >= 1 && alive.chars > 0, JSON.stringify(alive));

  // 第三档：致命失败

  console.log('\n[E3] 致命失败：常驻横幅 + 不再假装可用');
  const badUrl = withWasm(URL_, 'build/__missing__/web/');
  console.log('→ 打开', badUrl);
  await cdp.send('Page.navigate', { url: badUrl });
  // 等"启动失败"落定：这里等的是 ready 一直不来的那个状态，所以给足时间再看。
  await sleep(2500);
  const fatal = await probe(
    'const h = window.__heditor;' +
    'const el = document.querySelector("[data-host-error=\\"fatal\\"]");' +
    'const r = el ? el.getBoundingClientRect() : null;' +
    'return { ready: h ? !!h.ready : false,' +
    '         fatalActive: h ? h.hostErrors.fatalActive : false,' +
    '         dead: h ? h.hostErrors.dead : false,' +
    '         title: el ? el.querySelector(".he-t").textContent : "",' +
    '         detail: el ? el.querySelector(".he-d").textContent : "",' +
    '         role: el ? el.getAttribute("role") : "",' +
    '         rect: r ? [Math.round(r.width), Math.round(r.height)] : null,' +
    '         style: el ? getComputedStyle(el).display : "",' +
    '         retry: !!document.querySelector("[data-host-error=\\"fatal\\"] button[data-he-retry]"),' +
    '         marks: document.querySelectorAll("[data-host-fatal=\\"1\\"]").length };');
  console.log(`     横幅：${fatal.title} / ${fatal.detail}`);
  check('引擎确实没起来（前置条件）', fatal.ready === false);
  check('出现致命横幅', fatal.fatalActive === true);
  check('横幅在 DOM 里且真的可见',
        fatal.rect && fatal.rect[0] > 0 && fatal.rect[1] > 0 && fatal.style !== 'none',
        `rect=${JSON.stringify(fatal.rect)} display=${fatal.style}`);
  check('横幅是无障碍可见的（role=alert）', fatal.role === 'alert');
  check('横幅带失败原因', fatal.detail.length > 0, fatal.detail.slice(0, 60));
  check('容器被标成不可用', fatal.marks >= 1, `标记 ${fatal.marks} 处`);
  check('给了恢复入口（重试按钮）', fatal.retry === true);
  check('宿主记录引擎已不可用', fatal.dead === true);

  const notPretending = await probe(
    'const h = window.__heditor;' +
    'const stats = h.stats;' +
    'let rejected = false;' +
    'try { await h.present(); } catch (e) { rejected = true; }' +
    'return { stats, rejected, isWorker: !!window.__wheditor };');
  check('不再假装有内容（stats 为空）', notPretending.stats === null);
  check('此后的操作不再假装成功',
        notPretending.isWorker ? notPretending.rejected === true : notPretending.stats === null,
        notPretending.isWorker ? `命令被当场拒掉：rejected=${notPretending.rejected}`
                               : '这一侧引擎没起来，任何取内容都只能拿到空');

  console.log('');
  console.log('──────────────────────────────');
  console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  process.exit(chk.failures ? 1 : 0);
}

main().catch((e) => {
  console.log('FATAL ' + String((e && e.stack) || e));
  process.exit(1);
});
