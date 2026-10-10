// 诊断探针（临时）：把"三连可编辑字段"的用户动作逐步走一遍，每一步打印
// 光标位置、各字段区间、光标归属哪个字段，以及"点画布"能否进到那个空字段里。
//
// 用法：scripts\verify_web.ps1 -Probe scripts\diag_chain3.mjs
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, sleep } from './cdp.mjs';

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

const key = (k) => `
  window.__ced.textarea.dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true,
                                   cancelable: true }));
  return true;`;

async function main() {
  mkdirSync(OUT, { recursive: true });
  const target = await CDP.findPageTarget(PORT);
  const c = await CDP.connect(target.webSocketDebuggerUrl);
  await c.send('Page.enable');
  await c.send('Runtime.enable');
  await c.disableHttpCache();
  await c.send('Emulation.setDeviceMetricsOverride',
               { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  await c.send('Page.navigate', { url: URL_ });
  await c.waitFor('return !!(window.__ced && window.__ced.ready)', 'window.__ced.ready');
  await sleep(300);

  const state = async (tag) => {
    const s = await c.eval(`
      const ctl = window.__ced.controls;
      return { caret: window.__ced.caret.offset, owner: ctl.caret,
               ranges: ctl.items.map(i => '[' + i.start[1] + ',' + i.end[1] + ')').join(' '),
               rects: ctl.items.map(i => i.rects.length ? i.rects.map(q => Math.round(q.x) + '+' + Math.round(q.w)).join(',') : '-').join(' | ') };`);
    console.log(`${tag.padEnd(30)} caret=${s.caret} owner=${s.owner} ${s.ranges}   rects=${s.rects}`);
    return s;
  };

  console.log('=== 三连可编辑字段（variant 2）：把所有用户路径走一遍 ===');
  await c.eval('return window.__ced.loadFormSample(2);');
  await c.eval('return window.__ced.stopBlink();');
  // 页面上实际请求到的资源名：带 ?v= 后缀 = 命中"版本号随文件变"的加载方式。
  const res = await c.eval(`
    return performance.getEntriesByType('resource')
      .map(e => e.name.replace(location.origin, ''))
      .filter(n => n.includes('app.js') || n.includes('ced_editor'))
      .join('\\n    ');`);
  console.log('实际加载的资源：\n    ' + res);
  const stamp = await c.eval('return window.__ced && document.getElementById("build")'
                             + ' ? document.getElementById("build").textContent : "(无)";');
  console.log('工具条构建时间：' + stamp);
  await state('载入样例');

  await c.eval('return window.__ced.setCaret(0, 9);');
  await state('光标放到中间字段末尾 9');
  await c.eval(key('Backspace'));
  await state('退格一次');
  await c.eval(key('Backspace'));
  await state('退格两次（中间清空）');
  await c.eval(key('ArrowRight'));
  await state('右方向键（在空字段上）');
  await c.eval(key('ArrowLeft'));
  await state('左方向键（在空字段上）');
  await c.eval(key('痛'));
  await state('打字 痛');

  // 点画布：把光标定在空字段的那一对括号上（左右各试几个像素）
  await c.eval('return window.__ced.loadFormSample(2);');
  await c.eval('return window.__ced.setCaret(0, 9);');
  await c.eval(key('Backspace'));
  await c.eval(key('Backspace'));
  await state('重新删空中间字段');
  for (const dx of [-8, -4, -2, 0, 2, 4, 8, 14]) {
    const got = await c.eval(`
      const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
      const sx = r.width / cv.width, sy = r.height / cv.height;
      const c0 = window.__ced.caret;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left + (c0.x + ${dx}) * sx, clientY: r.top + (c0.y + 5) * sy,
          bubbles: true, cancelable: true }));
      const ctl = window.__ced.controls;
      return { caret: window.__ced.caret.offset, owner: ctl.caret };`);
    console.log(`点 光标x ${dx > 0 ? '+' : ''}${dx} px → caret=${got.caret} owner=${got.owner}` +
                (got.owner === 1 ? '   ← 进了空字段' : ''));
  }

  await c.send('Browser.close').catch(() => {});
  process.exit(0);
}

main().catch((e) => {
  console.error('诊断探针失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
