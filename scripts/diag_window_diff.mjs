// 一次性取证：长文档编辑之后，"屏幕 ≠ 引擎帧"的那 100 万像素到底在哪一页。
//
// 用法：node scripts/diag_window_diff.mjs --port 9222 --url http://127.0.0.1:8781/web/index.html
import { CDP, sleep } from './cdp.mjs';

const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://127.0.0.1:8781/web/index.html');

const REPORT = `
return (function () {
  const H = window.__heditor;
  const sc = H.selfCheck();
  const vp = H.viewport;
  const pcs = H.pageCanvases.map((p) => ({ page: p.page, w: p.el.width, h: p.el.height,
                                            top: p.el.style.top }));
  return JSON.stringify({
    diff: sc.screenVsEngineDiffPx, box: sc.screenVsEngineDiffBox,
    framePx: sc.framePx, window: sc.window, canvases: sc.pageCanvases,
    compared: sc.window.comparedPages, vp, pcs,
    dirty: H.dirtyPages, caret: sc.caret,
    repainted: sc.repainted,
  });
})()`;

async function main() {
  const target = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(target.webSocketDebuggerUrl);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Emulation.setDeviceMetricsOverride',
               { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready', 60000);
  await c.eval('return window.__heditor.stopBlink();');

  const show = async (tag) => {
    const r = JSON.parse(await c.eval(REPORT));
    console.log(`\n--- ${tag} ---`);
    console.log(`  差 ${r.diff} px  盒 ${JSON.stringify(r.box)}  帧 ${r.framePx.join('×')}`);
    console.log(`  窗口 第${r.window.first}页起 ${r.window.count} 页，比了 ${r.compared} 页；` +
                `宿主画布 ${r.canvases} 张`);
    console.log(`  引擎认为窗口 ${r.vp.engineFirst}+${r.vp.engineCount}，宿主想要 ` +
                `${r.vp.desired.first}+${r.vp.desired.count}，可见 ${r.vp.visible.first}..${r.vp.visible.last}`);
    console.log(`  画布：${r.pcs.map((p) => `#${p.page}@${p.top}(${p.w}×${p.h})`).join(' ')}`);
    console.log(`  脏页 ${JSON.stringify(r.dirty)}  光标 段${r.caret.para} 页${r.caret.page} ` +
                `y=${r.caret.y}`);
    console.log(`  本轮重画 ${JSON.stringify(r.repainted)}`);
  };

  await c.eval('return window.__heditor.reset(7, 1.5);');
  await c.eval('return window.__heditor.stopBlink();');
  await sleep(200);
  await show('装载 200 段');

  await c.eval('return window.__heditor.setCaret(150, 5);');
  await c.eval('return window.__heditor.stopBlink();');
  await sleep(150);
  await show('光标到第 150 段');

  await c.eval('return window.__heditor.type("字");');
  await sleep(100);
  await show('敲一个字之后');

  await c.eval('return window.__heditor.present();');
  await sleep(60);
  await show('再 present 一次之后');

  c.ws.close();
  process.exit(0);
}

main().catch((e) => { console.error('失败:', e && e.stack ? e.stack : e); process.exit(1); });
