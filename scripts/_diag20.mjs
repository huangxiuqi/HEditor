// 一次性诊断：拖选出可视区那一节为什么没滚动（不入判据）。
import { CDP, sleep } from './cdp.mjs';

const PORT = 9222;
const iUrl = process.argv.indexOf('--url');
const URL_ = iUrl >= 0 ? process.argv[iUrl + 1] : 'http://127.0.0.1:8781/web/index.html';

const cdp = { conn: null };
const c = await (async () => {
  const t = await CDP.findPageTarget(PORT);
  const conn = await CDP.connect(t.webSocketDebuggerUrl);
  await conn.send('Page.enable');
  await conn.send('Runtime.enable');
  await conn.disableHttpCache();
  await conn.send('Emulation.setDeviceMetricsOverride',
                  { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await conn.send('Page.navigate', { url: URL_ });
  await conn.waitFor('return !!(window.__heditor && window.__heditor.ready)',
                     'window.__heditor.ready');
  cdp.conn = conn;
  return conn;
})();

// await 写在非 async 求值体里是语法错，所以带 await 的表达式一律包 async IIFE。
// 两个入口：run 跑语句、ev 取表达式的值。
//
// 外层那个 return 不能省：cdp 的 eval 把整段包成一个立即调用函数，
//   没有 return 的话内层 async IIFE 的 Promise 被丢掉，外层返回 undefined，
//   于是 awaitPromise 等了个空 —— 读数全变成 undefined，而副作用照跑（更难发现）。
const run = (code) => c.eval('return (async () => { ' + code + ' })();');
const ev = (expr) => c.eval('return (async () => { return ' + expr + '; })();');

// box 是语句体（不是表达式），所以走 run 而不是 ev。
const box = () => run(`
  const s = document.getElementById('stage');
  const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
  const sr = s.getBoundingClientRect();
  return { scrollTop: s.scrollTop, scrollHeight: s.scrollHeight, clientHeight: s.clientHeight,
           stageTop: sr.top, stageBottom: sr.bottom, stageLeft: sr.left, stageRight: sr.right,
           proxyTop: r.top, proxyLeft: r.left, proxyW: r.width, proxyH: r.height,
           cvW: cv.width, cvH: cv.height, dpr: window.devicePixelRatio,
           zoom: Number((document.getElementById('zoom') || {}).value) || 1.5 };`);

console.log('--- 装载长文档 ---');
await run('await window.__heditor.loadStressDocument(200, 20); return true;');
await sleep(400);
console.log('装载后', JSON.stringify(await box(), null, 1));

const top = () => run('return document.getElementById("stage").scrollTop;');

console.log('--- 先滚到 5800 再看谁把它改回去 ---');
await run('document.getElementById("stage").scrollTop = 5800; return true;');
console.log('设 5800 后立即读 =', await top());
await sleep(400);
console.log('等 400ms 后读 =', await top());

console.log('--- 重新装载长文档（探针在 (i) 之前也经历过别的文档）---');
await run('await window.__heditor.loadStressDocument(200, 20); return true;');
console.log('重装后立即读 =', await top());
await sleep(400);
console.log('重装后 400ms 读 =', await top());

console.log('--- 再置 0，看它是否被拉回去 ---');
await run('document.getElementById("stage").scrollTop = 0; return true;');
console.log('置 0 后立即读 =', await top());
await sleep(250);
console.log('置 0 后 250ms 读 =', await top());
await run('await window.__heditor.setCaret(0, 0); return true;');
console.log('setCaret(0,0) 后读 =', await top());
await sleep(250);
console.log('setCaret 后 250ms 读 =', await top());
console.log('盒子', JSON.stringify(await box(), null, 1));

const g = await box();
const caret = await ev('window.__heditor.caret');
const toClient = (pt, dx, dy) => ({
  x: g.proxyLeft + (pt.x + dx) * g.zoom * (g.proxyW / g.cvW),
  y: g.proxyTop + (pt.y + dy) * g.zoom * (g.proxyH / g.cvH),
});
const from = toClient(caret, 1, caret.h / 2);
const out = { x: (g.stageLeft + g.stageRight) / 2, y: g.stageBottom + 30 };
console.log('按下点', JSON.stringify(from), ' 容器外点', JSON.stringify(out));

const mouse = async (type, at, over) => {
  await c.send('Input.dispatchMouseEvent', {
    type, x: Math.round(at.x), y: Math.round(at.y), button: 'left',
    buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1, ...(over || {}),
  });
};

console.log('--- 按下 + 移到容器外 ---');
await mouse('mousePressed', from);
await sleep(30);
console.log('按下后 drag=', JSON.stringify(await ev('window.__heditor.drag')));
console.log('按下后光标=', JSON.stringify(await ev('window.__heditor.caret')));
await mouse('mouseMoved', out);
await sleep(80);
console.log('移出后 drag=', JSON.stringify(await ev('window.__heditor.drag')));
console.log('移出后盒=', JSON.stringify(await box(), null, 1));
await sleep(700);
console.log('等 700ms 后盒=', JSON.stringify(await box(), null, 1));
console.log('等 700ms 后 drag=', JSON.stringify(await ev('window.__heditor.drag')));
console.log('选区=', JSON.stringify(await ev('window.__heditor.selection')));
await mouse('mouseReleased', out);

await c.send('Browser.close').catch(() => {});
process.exit(0);
