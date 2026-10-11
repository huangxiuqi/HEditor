// 交互压力探针：随机/密集地在正文各处点击 + 键盘动作，每一步都当场问两个问题：
//
//   引擎帧本身对不对？—— __heditor.auditFrame()：把当前帧与"整帧重画一遍"的结果逐字节
//      比。这是覆盖最广的"该消失却没消失"判据（旧表单标记残留就是这一类）。
//      不能只用 selfCheck（画布 vs 引擎帧）：引擎帧自己错了时画布会忠实反映它，
//      两边一起错、判据照样绿。
//   画布贴得对不对？—— __heditor.selfCheck()。顺序要紧：auditFrame 会把这一帧修成真值，
//      必须先跑 selfCheck。
//
// 用法：scripts\verify_web.ps1 -Probe scripts\stress_probe.mjs
// 退出码：0 = 全部一致；1 = 有不一致（明细打在 stdout，现场帧存到 out/web/stress-*.png）

import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, ensureDir, sleep } from './cdp.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..');
const argv = process.argv.slice(2);
const opt = (n, d) => {
  const i = argv.indexOf('--' + n);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : d;
};
const PORT = Number(opt('port', '9222'));
const URL_ = opt('url', 'http://localhost:8781/web/index.html');
const OUT = join(ROOT, opt('out-dir', 'out/web'));
ensureDir(OUT);

// 在页面上点某个"帧磅坐标"（走真实的 mousedown，就是用户点画布那条路）
const CLICK_AT_PT = (xPt, yPt) => `
return (function () {
  const cv = window.__heditor.canvas;
  const r = cv.getBoundingClientRect();
  const z = window.__heditor.zoomLevel;
  const x = r.left + (${xPt} * z) * (r.width / cv.width);
  const y = r.top + (${yPt} * z) * (r.height / cv.height);
  const ev = new MouseEvent('mousedown', { clientX: x, clientY: y, bubbles: true, cancelable: true, button: 0 });
  cv.dispatchEvent(ev);
  return true;
})()`;

const SNAP = `
return (function () {
  const sc = window.__heditor.selfCheck();
  // 先问"引擎帧本身对不对"（增量结果 vs 整帧重画），再问"画布贴得对不对"。
  //   顺序要紧：auditFrame 会把这一帧修成真值，放在 selfCheck 之后跑，
  //     否则它会顺带把上屏错误也"治好"，selfCheck 就再也抓不到上屏问题了。
  const au = window.__heditor.auditFrame();
  const st = window.__heditor.editStats;
  const ct = window.__heditor.controls;
  return { screen: sc.screenVsEngineDiffPx, screenBox: sc.screenVsEngineDiffBox,
           frame: au.diff, frameBox: au.box,
           repaint: sc.repainted, caretCtrl: ct ? ct.caret : -1,
           caretOff: st.caretOffset, pages: st.repaintedPages };
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
  await c.waitFor('return !!(window.__heditor && window.__heditor.ready)', 'ready', 30000);

  // 不 stopBlink —— 用户那边闪烁是一直开着的，这必须包含在内
  await c.eval('return window.__heditor.loadFormSample(0);');

  // 采样的点击点（帧磅坐标）：field0 / gap / field1(只读) / gap / field2 / 行首 / 行尾
  const pts = [
    [63, 62, '行首(患)'],
    [70, 62, '：'],
    [75, 62, 'field0 内 反'],
    [80, 62, 'field0 内 心'],
    [88, 62, 'field0 末 悸'],
    [96, 62, '只读 field1 三天'],
    [110, 62, '，'],
    [120, 62, 'field2 加'],
    [128, 62, 'field2 重'],
    [140, 62, 'field2 末 天'],
    [150, 62, '行尾 。'],
  ];

  let bad = 0, steps = 0;
  const log = [];
  const runCase = async (label, waitMs) => {
    for (const [x, y, name] of pts) {
      await c.eval(CLICK_AT_PT(x, y));
      await sleep(waitMs);
      const s = await c.eval(SNAP);
      ++steps;
      if (s.diff > 0 || s.frame > 0) {
        ++bad;
        log.push(` [${label}] 点「${name}」(${x},${y}) wait=${waitMs}ms → ` +
                 `引擎帧自审差 ${s.frame} px 盒=${JSON.stringify(s.frameBox)}；` +
                 `画布vs引擎差 ${s.screen} px 盒=${JSON.stringify(s.screenBox)}；` +
                 `上报脏矩形=${JSON.stringify(s.repaint)} 光标控件=${s.caretCtrl} off=${s.caretOff}`);
        // 存一张现场
        const dataUrl = await c.eval(`
          return (function () {
            const f = window.__heditor.rawFrame();
            const W = window.__heditor.frameW(), H = window.__heditor.frameH();
            const cv2 = document.createElement('canvas'); cv2.width = W; cv2.height = H;
            const ctx = cv2.getContext('2d');
            const img = ctx.createImageData(W, H);
            for (let i = 0; i < W*H*4; ++i) img.data[i] = f[i];
            ctx.putImageData(img, 0, 0);
            return cv2.toDataURL('image/png');
          })()`);
        writeFileSync(join(OUT, `stress-engine-${bad}.png`),
                      Buffer.from(String(dataUrl).split(',')[1], 'base64'));
        await c.screenshot(join(OUT, `stress-screen-${bad}.png`));
      }
    }
  };

  // 三种点击间隔：0ms（连点）、120ms、600ms（跨过闪烁 tick）
  await runCase('连点', 0);
  await runCase('间隔120ms', 120);
  await runCase('间隔600ms(跨闪烁)', 600);

  // 键盘动作也纳入：方向键（会跨字段）、Tab（缩进）、敲字、删字、切模式
  const KEY = `
    window.__heditor.textarea.dispatchEvent(new KeyboardEvent('keydown', { key: KEYNAME, bubbles: true, cancelable: true }));
    return true;`;
  const actions = [
    ['ArrowRight', 'ArrowRight'], ['ArrowRight', 'ArrowRight'], ['ArrowLeft', 'ArrowLeft'],
    ['Tab', 'Tab'], ['Tab', 'Tab'], ['a', 'a'], ['Backspace', 'Backspace'],
  ];
  for (let round = 0; round < 4; ++round) {
    for (const [label, k] of actions) {
      await c.eval(KEY.replace('KEYNAME', JSON.stringify(k)));
      await sleep(30);
      const s = await c.eval(SNAP);
      ++steps;
      if (s.screen > 0 || s.frame > 0) {
        ++bad;
        log.push(` [键盘第${round + 1}轮] 按 ${label} → 引擎帧自审差 ${s.frame} px ` +
                 `盒=${JSON.stringify(s.frameBox)}；画布vs引擎差 ${s.screen} px ` +
                 `盒=${JSON.stringify(s.screenBox)}；上报=${JSON.stringify(s.repaint)}`);
      }
    }
    // 轮末切一次模式再切回来（模式参与占位/标记）
    await c.eval('return window.__heditor.setEditMode(0);');
    const sd = await c.eval(SNAP); ++steps;
    if (sd.screen > 0 || sd.frame > 0) {
      ++bad;
      log.push(` [切到文档模式] 引擎帧自审差 ${sd.frame} 盒=${JSON.stringify(sd.frameBox)}；` +
               `画布差 ${sd.screen} 盒=${JSON.stringify(sd.screenBox)}`);
    }
    await c.eval('return window.__heditor.setEditMode(1);');
    const sf = await c.eval(SNAP); ++steps;
    if (sf.screen > 0 || sf.frame > 0) {
      ++bad;
      log.push(` [切回表单模式] 引擎帧自审差 ${sf.frame} 盒=${JSON.stringify(sf.frameBox)}；` +
               `画布差 ${sf.screen} 盒=${JSON.stringify(sf.screenBox)}`);
    }
  }

  console.log(`\n完成 ${steps} 次点击，不一致 ${bad} 次`);
  for (const l of log.slice(0, 20)) console.log('  ' + l);
  if (!log.length) console.log('  （全部一致）');

  await c.send('Browser.close').catch(() => {});
  process.exit(bad ? 1 : 0);
}

main().catch(e => { console.error('失败:', e.message); process.exit(2); });
