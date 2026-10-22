// 错误降级探针：验"失败会变成页面上看得见的东西"。
//
// 要证伪的事：引擎的失败只有错误事件与内部数组知道，而事件不一定有人监听。于是失败在
//   页面上完全看不见 —— 页面要么空白、要么停在一个看起来能输入、实际打不进字的编辑器上。
//   这里按三档取证：
//
//   正常态：页面上不该有任何错误界面（反向一半 —— 没有这一半，"永远显示横幅"也能算过）。
//   降级：一次页面级失败出现会自动消失的提示条，且编辑器随后仍然可用（不是所有失败都该致命）。
//   致命：出现常驻横幅、元素真的可见、容器被标记不可用，且此后不再有一份"看起来能用"的编辑器。
//
// 致命那一档用 ?glue= 指向一个不存在的产物来造：这是真实会发生的配置错误，不需要往
//   产品代码里塞测试开关。两条传输都受影响（同线程是胶水脚本加载失败，Worker 是
//   importScripts 失败）。
//
// 两条传输通吃：URL 带 transport=worker 即走 Worker。
//   node scripts/error_probe.mjs --url http://127.0.0.1:8781/web/demo.html
import { CDP, Checks } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};

const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/dist-app/index.html');
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

// 把 URL 的某个参数换成别的值（用来造"产物不存在"）。
function withParam(url, key, value) {
  const u = new URL(url);
  u.searchParams.set(key, value);
  return u.href;
}

async function main() {
  const target = await findTarget();
  const cdp = await CDP.connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Log.enable');
  await cdp.disableHttpCache();

  const RAW = cdp.eval.bind(cdp);
  const asBody = (b) => 'return (async () => {\n' + b + '\n})();';
  cdp.eval = async (expr) => {
    const r = await RAW(asBody(expr));
    // 异步宿主：读之前先让队列排空（与 edit_probe 同一口径）。
    await RAW('const H = window.__demo;' +
              'return H && H.flush ? H.flush().then(() => true) : true;').catch(() => true);
    return r;
  };

  const ready = async () => {
    for (let i = 0; i < 120; i++) {
      const st = await cdp.eval('return !!(window.__demo && window.__demoReady);')
        .catch(() => false);
      if (st === true) return true;
      await sleep(400);
    }
    return false;
  };

  console.log('→ 打开', URL_);
  await cdp.send('Page.navigate', { url: URL_ });
  const ok = await ready();
  console.log(`→ 页面就绪 ${ok}`);
  const probe = (body) => cdp.eval(body);

  // 第一档：正常态

  console.log('\n[E1] 正常态：不该有任何错误界面');
  const normal = await probe(
    'const h = window.__demo;' +
    'return { ready: !!h, startError: window.__demoStartError || "",' +
    '         entries: h ? h.diag.errors().length : -1,' +
    '         banner: !!document.querySelector("[data-host-error=\\"fatal\\"]"),' +
    '         toast: !!document.querySelector("[data-host-error=\\"warn\\"]"),' +
    '         marked: document.querySelectorAll("[data-host-fatal=\\"1\\"]").length };');
  check('启动成功（前置条件）', ok && normal.ready === true, JSON.stringify(normal));
  check('没有致命横幅', !normal.banner && normal.marked === 0);
  check('没有提示条残留', !normal.toast);
  check('降级记账为空（反向一半）', normal.entries === 0, `entries=${normal.entries}`);
  const bootErrors = cdp.consoleErrors.filter((t) => !/favicon/i.test(t));
  check('启动过程没有 console 错误', bootErrors.length === 0, bootErrors.slice(0, 2).join(' | '));

  // 第二档：单次失败（页面级异常 → 降级提示，编辑器继续可用）

  console.log('\n[E2] 单次失败：提示一次，编辑器继续可用');
  const before = await probe('return window.__demo.text().length;');
  await probe(
    // 造一次真实的页面级失败：未捕获异常走 window.onerror。宿主把这类失败记成"降级"
    //   而不是致命，因为引擎还活着 —— 它只说明页面上别处有 bug。
    'setTimeout(() => { throw new Error("error_probe 故意抛的一次失败"); }, 0);' +
    'return true;');
  await sleep(120);
  const afterOp = await probe(
    'const h = window.__demo;' +
    'return { entries: h.diag.errors(),' +
    '         toast: !!document.querySelector("[data-host-error=\\"warn\\"]"),' +
    '         banner: !!document.querySelector("[data-host-error=\\"fatal\\"]"),' +
    '         role: (document.querySelector("[data-host-error=\\"warn\\"]") || {}).getAttribute' +
    '           ? document.querySelector("[data-host-error=\\"warn\\"]").getAttribute("role") : "" };');
  check('记下了一条降级', afterOp.entries.length >= 1,
        `entries=${afterOp.entries.length}：${(afterOp.entries[0] || '').slice(0, 60)}`);
  check('没有升级成致命', !afterOp.banner);
  check('页面上有提示条', afterOp.toast === true,
        'DOM 里应存在 [data-host-error="warn"]');
  check('提示条是无障碍可见的（role=status）', afterOp.role === 'status', afterOp.role);
  // 反向一半：失败之后编辑器必须还能用。只看"没有致命"不够 ——
  //   引擎可能已经失效而没有任何标志位反映出来。
  const alive = await probe(
    `const h = window.__demo;
     await h.type('活');
     return { pages: h.geometry().pages, len: h.text().length };`);
  check('失败之后编辑器仍然可用', alive.pages >= 1 && alive.len === before + 1,
        `页 ${alive.pages}，字数 ${before} → ${alive.len}`);

  // 第三档：致命失败

  console.log('\n[E3] 致命失败：常驻横幅 + 不再假装可用');
  const badUrl = withParam(URL_, 'glue', 'build/__missing__/web/heditor.js');
  console.log('→ 打开', badUrl);
  await cdp.send('Page.navigate', { url: badUrl });
  // 等"启动失败"落定：这里等的是 createEditor 拒绝之后的那份横幅。
  for (let i = 0; i < 40; ++i) {
    const seen = await probe(
      'return !!(window.__demoStartError || document.querySelector("[data-host-error=\\"fatal\\"]"));')
      .catch(() => false);
    if (seen) break;
    await sleep(250);
  }
  await sleep(300);
  const fatal = await probe(
    'const el = document.querySelector("[data-host-error=\\"fatal\\"]");' +
    'const r = el ? el.getBoundingClientRect() : null;' +
    'const host = document.getElementById("ed");' +
    'return { editor: !!window.__demo,' +
    '         startError: window.__demoStartError || "",' +
    '         hasBanner: !!el,' +
    '         title: el ? el.querySelector(".he-t").textContent : "",' +
    '         detail: el ? el.querySelector(".he-d").textContent : "",' +
    '         hint: el ? el.querySelector(".he-h").textContent : "",' +
    '         role: el ? el.getAttribute("role") : "",' +
    '         rect: r ? [Math.round(r.width), Math.round(r.height)] : null,' +
    '         style: el ? getComputedStyle(el).display : "",' +
    '         retry: !!document.querySelector("[data-host-error=\\"fatal\\"] button[data-he-retry]"),' +
    '         marked: host ? host.getAttribute("data-host-fatal") : null,' +
    '         canvases: document.querySelectorAll("#pages canvas").length };');
  console.log(`     横幅：${fatal.title} / ${fatal.detail}`);
  check('引擎确实没起来（前置条件）', fatal.editor === false && !!fatal.startError,
        `editor=${fatal.editor} startError=${String(fatal.startError).slice(0, 60)}`);
  check('出现致命横幅', fatal.hasBanner === true);
  check('横幅在 DOM 里且真的可见',
        !!fatal.rect && fatal.rect[0] > 0 && fatal.rect[1] > 0 && fatal.style !== 'none',
        `rect=${JSON.stringify(fatal.rect)} display=${fatal.style}`);
  check('横幅是无障碍可见的（role=alert）', fatal.role === 'alert', fatal.role);
  check('横幅带失败原因', fatal.detail.length > 0, fatal.detail.slice(0, 60));
  check('横幅带下一步（提示）', fatal.hint.length > 0, fatal.hint.slice(0, 40));
  check('容器被标成不可用', fatal.marked === '1', String(fatal.marked));
  check('给了恢复入口（重试按钮）', fatal.retry === true);
  check('不再假装有内容（一页画布都没有）', fatal.canvases === 0, `${fatal.canvases} 张`);

  console.log('');
  console.log('──────────────────────────────');
  console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  process.exit(chk.failures ? 1 : 0);
}

main().catch((e) => {
  console.log('FATAL ' + String((e && e.stack) || e));
  process.exit(1);
});
