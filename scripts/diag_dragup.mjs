// 诊断：Worker 宿主上"松开鼠标还在拖"的泄漏点在哪一条抬起路径。
//
// 症状是松手之后 host.drag.active 仍为真、且容器还在自己滚。候选路径逐条过一遍：
//   抬起在画布内 / 抬起在视口外 / 自动滚动中抬起 / 极快的按下即抬起 /
//   拖动中换窗之后抬起 / 抬起之后隔一段时间再看。
//
// 用法：node scripts/diag_dragup.mjs --port 9222 --url http://127.0.0.1:8781/web/worker.html
import { CDP, sleep } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/worker.html');

const cdp = { conn: null };
const run = (body) => cdp.conn.eval('return (async () => { ' + body + ' })();');
const ev = (expr) => cdp.conn.eval('return (async () => { return ' + expr + '; })();');

async function main() {
  const target = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(target.webSocketDebuggerUrl);
  cdp.conn = c;
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Emulation.setDeviceMetricsOverride',
               { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready', 120000);
  await run('return await window.__heditor.run(7, 1.5);');
  await run('return window.__heditor.stopBlink();');
  await sleep(400);

  const mouse = async (type, x, y, buttons) => {
    await c.send('Input.dispatchMouseEvent',
                 { type, x: Math.round(x), y: Math.round(y), button: 'left',
                   buttons, clickCount: 1 });
  };
  const geom = async () => JSON.parse(await run(`
    const H = window.__heditor;
    const cv = H.canvas, r = cv.getBoundingClientRect();
    const s = document.getElementById('stage').getBoundingClientRect();
    return JSON.stringify({
      press: { x: Math.round(r.left + 120), y: Math.round(r.top + 130) },
      mid: { x: Math.round((s.left + s.right) / 2) },
      stageTop: Math.round(s.top), stageBottom: Math.round(s.bottom),
      viewH: window.innerHeight, viewW: window.innerWidth });`));
  const snap = async () => JSON.parse(await run(`
    const H = window.__heditor;
    const s = document.getElementById('stage');
    return JSON.stringify({ drag: H.drag, top: s.scrollTop });`));

  const g = await geom();
  console.log(`几何 按下点 ${JSON.stringify(g.press)} 容器 ${g.stageTop}..${g.stageBottom}` +
              ` 视口 ${g.viewW}x${g.viewH}`);

  const reset = async () => {
    await run('return await window.__heditor.setCaret(0, 0);');
    await run('document.getElementById("stage").scrollTop = 0; return true;');
    await sleep(200);
  };

  const cases = [
    ['A 抬起在画布内', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', g.mid.x, g.press.y + 40, 1);
      await sleep(60);
      await mouse('mouseReleased', g.mid.x, g.press.y + 40, 0);
    }],
    ['B 抬起在视口外（下缘之下）', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', g.mid.x, g.viewH + 60, 1);
      await sleep(60);
      await mouse('mouseReleased', g.mid.x, g.viewH + 60, 0);
    }],
    ['C 自动滚动中抬起（在视口外）', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', g.mid.x, g.stageBottom + 30, 1);
      await sleep(450);
      await mouse('mouseReleased', g.mid.x, g.stageBottom + 30, 0);
    }],
    ['D 自动滚动中抬起（回到容器内）', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', g.mid.x, g.stageBottom + 30, 1);
      await sleep(450);
      await mouse('mouseReleased', g.mid.x, g.press.y + 60, 0);
    }],
    ['E 极快按下即抬起（不等往返）', async () => {
      const a = mouse('mousePressed', g.press.x, g.press.y, 1);
      const b = mouse('mouseReleased', g.press.x, g.press.y, 0);
      await Promise.all([a, b]);
    }],
    ['F 按下之后窗口内容被换（拖到跨页再抬）', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', g.mid.x, g.stageBottom + 30, 1);
      await sleep(900);
      await mouse('mouseReleased', g.mid.x, g.stageBottom + 30, 0);
    }],
    ['G 按下后抬起落在滚动容器外（左缘之外）', async () => {
      await mouse('mousePressed', g.press.x, g.press.y, 1);
      await sleep(60);
      await mouse('mouseMoved', -40, g.press.y, 1);
      await sleep(60);
      await mouse('mouseReleased', -40, g.press.y, 0);
    }],
  ];

  for (const [name, act] of cases) {
    await reset();
    await act();
    const after = await snap();
    await sleep(700);
    const later = await snap();
    const topDrift = later.top - after.top;
    const leaked = later.drag && later.drag.active === true;
    console.log(`${leaked ? '泄漏' : '  ok'} | ${name.padEnd(30)}` +
                ` 抬起后 drag=${JSON.stringify(after.drag)}` +
                ` 700ms 后 active=${later.drag && later.drag.active}` +
                ` 还在滚=${topDrift > 0 ? topDrift + 'px' : '否'}`);
    // 泄漏会把状态留给下一条用例，收个尾
    await mouse('mouseReleased', g.mid.x, g.press.y, 0);
    await run('return await window.__heditor.dragSettled();').catch(() => {});
    await sleep(120);
  }

  c.ws.close();
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断失败：', e && e.stack ? e.stack : e);
  process.exit(1);
});
