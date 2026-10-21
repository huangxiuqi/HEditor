// 用 Chrome DevTools 协议给 WASM 验证台做可证伪检查。
//
// 不做"截个图看看"这种模糊验证，而是把页面里的数字取回来：
//   window.__heditor.stats   每次布局的页数/字形数/缺字数/分阶段耗时
//   window.__heditor.bench   引擎内循环 12 次的平均耗时
//   window.__heditor.errors  页面级异常（有就说明静默失败了）
//   每个场景截一张 PNG，用于人眼复核
//
// 两宿主通吃，做法与 edit_probe.mjs / stress_probe.mjs 一致：表达式统一包 async IIFE
//   并预检语法，跑完等宿主排空。单线程宿主（index.html）全同步、没有 quiesce，跳过即可；
//   Worker 宿主（worker.html）的门面一律异步，不排空就会读到上一拍的 stats。
//   产物按宿主分名（见下面的 SUF），两个宿主各跑一遍时不会互相覆盖。
//
// 无第三方依赖：Node 22 自带 fetch 和 WebSocket。
//
// 用法：
//   scripts\verify_web.ps1 -Probe scripts\web_probe.mjs                    # 单线程宿主
//   scripts\verify_web.ps1 -Probe scripts\web_probe.mjs -Page worker.html  # Worker 宿主
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
// CDP 客户端统一从 cdp.mjs 来。本文件自己养了一份拷贝，结果是给 cdp.mjs 加了
// disableHttpCache() 之后这里在运行期才炸（'cdp.disableHttpCache is not a function'）。
import { CDP } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};

const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/index.html');
const OUT = join(ROOT, opt('out-dir', 'out/web'));
const ZOOM = Number(opt('zoom', '1.5'));

// 产物按宿主分名。两个宿主各跑一遍是常态，同名会互相覆盖 —— 那时"看着是绿的"看的
//   可能是另一份。index.html 保持原名（历史产物与文档都按那个名字引用）。
const SUF = (() => {
  const file = (new URL(URL_)).pathname.split('/').pop() || '';
  const base = file.replace(/\.[^.]*$/, '');
  return base && base !== 'index' ? '-' + base : '';
})();

const SCENARIOS = [
  [0, '完整样例（4 页）'],
  [1, '蒙文竖排 vertical-lr（侧倒）'],
  [2, '中文竖排 vertical-rl（正立）'],
  [3, '中文长文本（横排）'],
  [4, '中英混排 + 字体回退'],
  [5, '蒙文竖排 vertical-rl（对照）'],
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function findTarget() {
  for (let i = 0; i < 60; i++) {
    try {
      const res = await fetch(`http://127.0.0.1:${PORT}/json/list`);
      const list = await res.json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page;
    } catch { /* 守护还没起来 */ }
    await sleep(500);
  }
  throw new Error(`连不上 Chrome 调试端口 ${PORT}`);
}

async function waitFor(cdp, expr, label, timeoutMs = 180000) {
  const t0 = Date.now();
  let last = null;
  while (Date.now() - t0 < timeoutMs) {
    try {
      last = await cdp.eval(expr);
      if (last) return Date.now() - t0;
    } catch (e) {
      last = null;   // 导航中执行上下文会失效，重试即可
    }
    await sleep(400);
  }
  throw new Error(`等待超时：${label}（最后取值 ${JSON.stringify(last)}）`);
}

async function main() {
  mkdirSync(OUT, { recursive: true });
  console.log(`→ 产物目录 ${OUT}（后缀 ${SUF || '(无，即 index.html 那套)'}）`);

  const target = await findTarget();
  const cdp = await CDP.connect(target.webSocketDebuggerUrl);
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  await cdp.send('Log.enable');
  // 导航之前先关 HTTP 缓存（profile 跨次运行复用，理由见 cdp.mjs）
  await cdp.disableHttpCache();
  await cdp.send('Emulation.setDeviceMetricsOverride', {
    width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false,
  });

  console.log('→ 打开', URL_);
  await cdp.send('Page.navigate', { url: URL_ });

  const bootMs = await waitFor(cdp, 'return !!(window.__heditor && window.__heditor.ready)',
                               'window.__heditor.ready');
  console.log(`→ 页面就绪，耗时 ${(bootMs / 1000).toFixed(1)} s`);

  // 包装要在"页面就绪之后"才生效：waitFor 用的那句求值必须原样发出去，
  //   不能在 ready 之前就去 quiesce（那时 Worker 还没起来处理 ping）。
  const RAW = cdp.eval.bind(cdp);
  const asBody = (b) => 'return (async () => {\n' + b + '\n})();';
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
  cdp.eval = async (expr) => {
    try { new AsyncFn(expr); } catch (e) {
      throw new Error('表达式语法错误：' + e.message + '\n---- 表达式原文 ----\n' + expr);
    }
    const r = await RAW(asBody(expr));
    await RAW('const H = window.__heditor;' +
              'if (window.__wheditor && !H.quiesce)' +
              '  throw new Error("异步宿主必须提供 quiesce()");' +
              'return H.quiesce ? H.quiesce().then(() => true) : true;');
    return r;
  };

  const env = await cdp.eval('return window.__heditor.fonts');
  const report = {
    url: URL_,
    bootMs,
    glueBytes: env.wasmGlue,
    userAgent: env.ua,
    scenarios: [],
    bench: null,
    pageErrors: [],
    consoleErrors: [],
  };

  for (const [id, label] of SCENARIOS) {
    await cdp.eval(`await window.__heditor.run(${id}, ${ZOOM}); return true;`);
    // 先读 stats，再睡。顺序反了会读到别的一拍的账。
    //
    //   装载后立刻读是 paintedGlyphs=568 / paintMs=63ms，隔 650ms 再读变成 0 / 0ms：
    //   stats 里的 paint 一列描述的是最近一次绘制，而宿主每 530ms 会为光标闪烁做一次
    //   只有光标的呈现，这份账会把装载这份盖掉。若先睡 350ms 再读，就变成与闪烁周期
    //   赛跑：赢到装载那笔账（几百个字形），输就拿到 0，同一份代码两次能差两个数量级。
    //   睡挪到后面既能拿到装载那笔账，又保住截图前先让合成器歇一下的做法。
    const st = await cdp.eval('return window.__heditor.stats');
    report.scenarios.push({ id, label, ...st });
    await sleep(350);

    const shot = await cdp.send('Page.captureScreenshot', {
      format: 'png', captureBeyondViewport: true,
    });
    writeFileSync(join(OUT, `scene${SUF}-${id}.png`), Buffer.from(shot.data, 'base64'));
    console.log(`  [${id}] ${label}  pages=${st.pageCount} glyphs=${st.glyphCount} ` +
                `missing=${st.missingGlyphCount} runs=${st.fontRunCount} ` +
                `shape=${st.shapeMs}ms layout=${st.layoutMs}ms paint=${st.paintMs}ms`);
  }

  report.bench = await cdp.eval('return window.__heditor.bench(12)');
  console.log(`→ 引擎内循环 12 次：每帧 ${report.bench.totalMs.toFixed(1)} ms ` +
              `(shape ${report.bench.shapeMs.toFixed(2)} / layout ${report.bench.layoutMs.toFixed(2)} ` +
              `/ paint ${report.bench.paintMs.toFixed(2)})`);

  report.pageErrors = await cdp.eval('return window.__heditor.errors');
  report.consoleErrors = cdp.consoleErrors;

  writeFileSync(join(OUT, `probe${SUF}.json`), JSON.stringify(report, null, 2), 'utf8');

  const bad = report.pageErrors.length + report.consoleErrors.length;
  console.log('');
  console.log(bad === 0 ? ' 无页面异常、无 console 错误'
                        : ` 发现 ${bad} 条异常：`);
  for (const e of [...report.pageErrors, ...report.consoleErrors]) console.log('   ' + e);

  const missing = report.scenarios.reduce((a, s) => a + s.missingGlyphCount, 0);
  console.log(missing === 0 ? ' 所有场景缺字数为 0' : ` 缺字合计 ${missing}`);

  // 批量验证时由 scripts/verify_web.ps1 统一收浏览器（它按 profile 杀整棵进程树）。
  //   这里各自关一次的话，一条命令跑十个探针就只能跑第一个：第二个连不上 9222。
  if (!process.env.HEDITOR_KEEP_BROWSER) await cdp.send('Browser.close').catch(() => {});
  process.exit(bad ? 1 : 0);
}

main().catch((e) => {
  console.error('探针失败：', e.message);
  process.exit(2);
});
