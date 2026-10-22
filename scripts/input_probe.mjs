// 库页面的输入层判据：走真实浏览器事件，不调 API。
//
// 为什么单独一份：demo_probe 证明的是"公开面只有公开面"，它调 e.type('AB') 走的是
//   命令层；而用户真正用的是键盘、指针、输入法 —— 那三条路在库里的入口是 host_core 那套
//   键位映射与命中归宿。API 面绿了不代表它们接上了（历史上有过 Ctrl+C/V/X 压根没接的缺口）。
//
// 两条传输各跑一遍（--transport inline|worker），最后把每一步的状态打成一行 JSON：
//   两串应当逐位相同 —— 输入层与传输无关，正是不变量里那句话。
//
// 用法：scripts\verify_web.ps1 -Probe scripts\input_probe.mjs -Page demo.html
//       node scripts/input_probe.mjs --port 9333 --transport worker
import { CDP, Checks, sleep } from './cdp.mjs';

const die = (e) => {
  console.log('PROBE FAILED: ' + ((e && (e.stack || e.message)) || String(e)));
  process.exit(1);
};
process.on('uncaughtException', die);
process.on('unhandledRejection', die);

const argv = process.argv.slice(2);
const opt = (name, def) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && argv[i + 1] ? argv[i + 1] : def;
};
const PORT = Number(opt('port', '9222'));
const TRANSPORT = opt('transport', 'inline');
const URL_BASE = opt('url', 'http://127.0.0.1:8781/web/demo.html');
const URL_ = URL_BASE + (URL_BASE.includes('?') ? '&' : '?') + 'transport=' + TRANSPORT;

const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
const page = list.find((t) => t.type === 'page');
if (!page) throw new Error('没有可用的页面 target');
const cdp = await CDP.connect(page.webSocketDebuggerUrl);
await cdp.send('Page.enable');
await cdp.send('Runtime.enable');
await cdp.send('Log.enable');
await cdp.disableHttpCache();
// DPR = 1：位移与页内坐标一一对应，探针里的像素算术才不用带比例因子。
await cdp.send('Emulation.setDeviceMetricsOverride',
               { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });

const chk = new Checks('输入层');
console.log('→ 打开', URL_);
await cdp.send('Page.navigate', { url: URL_ });
const bootMs = await cdp.waitFor('return !!window.__demoReady', 'window.__demoReady', 40000);
console.log(`→ 页面就绪，耗时 ${(bootMs / 1000).toFixed(1)} s`);

const ev = (body) => cdp.eval('return (async () => {\n' + body + '\n})();');

// 每一步之后取一份状态。两串状态相同 = 输入层与传输无关。
const trail = [];
const snap = async (label) => {
  // Worker 传输下按键是往返：不等排空就读，读到的是上一拍（30 ms 的睡眠时对时错）。
  await ev(`await window.__demo.flush();`);
  const s = await ev(`
    const e = window.__demo;
    const c = e.caret();
    const sel = e.selection();
    return { text: e.text(), para: c.pos.para, off: c.pos.offset, page: c.page,
             empty: sel.empty, anchor: sel.anchor, focus: sel.focus };`);
  trail.push({ label, ...s });
  return s;
};

// 文档重置成固定几行：坐标可预期，断言不必依赖示例正文的长度。
const reset = () => ev(`
  const e = window.__demo;
  await e.setText('第一行 abc\\n第二行 def\\n第三行 ghi');
  await e.setCaret({ para: 0, offset: 0 });
  await e.flush?.();
  return true;`);

// 第一页画布在屏幕上的位置（页内坐标加它就是视口坐标）。
const pageBox = () => ev(`
  const c = document.querySelector('#pages canvas');
  if (!c) return null;
  const r = c.getBoundingClientRect();
  return { x: r.left, y: r.top, w: r.width, h: r.height };`);

// 页内坐标 -> 视口坐标。caret().rect 是页内像素，页号另给（TRAPS 140）。
async function caretPoint(dx = 0, dy = 0) {
  const c = await ev(`return window.__demo.caret()`);
  const b = await pageBox();
  if (!b) throw new Error('页面上没有画布');
  return { x: b.x + c.rect.x + dx, y: b.y + c.rect.y + c.rect.h / 2 + dy };
}

async function key(code, k, text) {
  const base = { windowsVirtualKeyCode: code, nativeVirtualKeyCode: code, key: k };
  await cdp.send('Input.dispatchKeyEvent',
                 { type: text ? 'keyDown' : 'rawKeyDown', ...base, ...(text ? { text, unmodifiedText: text } : {}) });
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
  await sleep(30);
}

async function chord(code, k) {
  // modifiers: 1=Alt 2=Ctrl 4=Meta 8=Shift
  const base = { windowsVirtualKeyCode: code, nativeVirtualKeyCode: code, key: k, modifiers: 2 };
  await cdp.send('Input.dispatchKeyEvent', { type: 'rawKeyDown', ...base });
  await cdp.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
  await sleep(40);
}

async function mouse(type, x, y, extra = {}) {
  await cdp.send('Input.dispatchMouseEvent',
                 { type, x: Math.round(x), y: Math.round(y), button: 'left', clickCount: 1, ...extra });
  await sleep(25);
}

// 一、落光标：点的是画布，落点必须由命中测试决定
chk.section('一、真实点击落光标');
await reset();
{
  const before = await snap('reset');
  chk.check('重置后光标在段 0 段首', before.para === 0 && before.off === 0,
            `para=${before.para} off=${before.off}`);
  // 段首往右 40 px：同一个 y、不同的 x，落点必然往后挪。
  const p = await caretPoint(40);
  await mouse('mousePressed', p.x, p.y);
  await mouse('mouseReleased', p.x, p.y);
  const after = await snap('click+40');
  chk.check('点击后光标仍在这一段', after.para === 0, `para=${after.para}`);
  chk.check('点击位置在段首右侧 ⇒ 偏移变大', after.off > 0, `off=${after.off}`);
  const ta = await ev(`return document.activeElement && document.activeElement.id`);
  chk.check('键盘承接层拿到了焦点', ta === 'ime', String(ta));
}

// 二、真实键盘：字符、退格、方向键、组合键
chk.section('二、真实键盘');
await reset();
{
  await ev(`await window.__demo.setCaret({ para: 0, offset: 7 });`);   // "第一行 abc" 的段尾
  await key(65, 'a', 'a');
  await key(66, 'b', 'b');
  await key(67, 'c', 'c');
  const typed = await snap('type abc');
  chk.check('真实按键进入文档', typed.text.includes('第一行 abcabc'), typed.text.split('\n')[0]);
  chk.check('光标跟着走了三格', typed.off === 10, `off=${typed.off}`);

  await key(8, 'Backspace');
  const bs = await snap('backspace');
  chk.check('退格删掉一个字符', bs.text.includes('第一行 abcab') && !bs.text.includes('abcabc'),
            bs.text.split('\n')[0]);
  chk.check('退格后光标回退一格', bs.off === 9, `off=${bs.off}`);

  await chord(90, 'z');
  const undone = await snap('ctrl+z');
  chk.check('Ctrl+Z 撤回上一次编辑（退格）', undone.text.includes('第一行 abcabc'),
            undone.text.split('\n')[0]);

  await chord(89, 'y');
  const redone = await snap('ctrl+y');
  chk.check('Ctrl+Y 重做（恢复的是刚撤掉的退格）',
            redone.text.includes('第一行 abcab') && !redone.text.includes('abcabc'),
            redone.text.split('\n')[0]);

  await key(36, 'Home');   // Home
  const home = await snap('home');
  chk.check('Home 回到段首', home.off === 0 && home.empty === true, `off=${home.off}`);

  await key(39, 'ArrowRight');
  const right = await snap('arrow-right');
  chk.check('方向键右移一格', right.off === 1, `off=${right.off}`);

  // Shift+Right：modifiers 里带 8
  await cdp.send('Input.dispatchKeyEvent',
                 { type: 'rawKeyDown', windowsVirtualKeyCode: 39, nativeVirtualKeyCode: 39,
                   key: 'ArrowRight', modifiers: 8 });
  await cdp.send('Input.dispatchKeyEvent',
                 { type: 'keyUp', windowsVirtualKeyCode: 39, nativeVirtualKeyCode: 39,
                   key: 'ArrowRight', modifiers: 8 });
  await sleep(40);
  const shifted = await snap('shift-right');
  chk.check('Shift+方向键产生选区', shifted.empty === false, `empty=${shifted.empty}`);
  chk.check('选区从第 1 格拉到第 2 格',
            shifted.anchor.offset === 1 && shifted.focus.offset === 2,
            JSON.stringify([shifted.anchor, shifted.focus]));
}

// 三、真实指针拖选
chk.section('三、真实拖选');
await reset();
{
  const a = await caretPoint(10);
  const b = await caretPoint(50);
  if (process.env.HEDITOR_DEBUG) {
    console.log('DBG caret', JSON.stringify(await ev(`return window.__demo.caret()`)),
                'box', JSON.stringify(await pageBox()), 'a', a, 'b', b);
  }
  await mouse('mousePressed', a.x, a.y);
  await mouse('mouseMoved', (a.x + b.x) / 2, a.y, { buttons: 1 });
  await mouse('mouseMoved', b.x, b.y, { buttons: 1 });
  await mouse('mouseReleased', b.x, b.y);
  const sel = await snap('drag');
  chk.check('拖选产生非空选区', sel.empty === false, `empty=${sel.empty}`);
  chk.check('选区两端都在段 0', sel.anchor.para === 0 && sel.focus.para === 0,
            JSON.stringify([sel.anchor, sel.focus]));
  chk.check('焦点在锚点右侧', sel.focus.offset > sel.anchor.offset,
            `${sel.anchor.offset} → ${sel.focus.offset}`);

  // 双击选词：库里没有这条。clickCountOf 只用来判三击（选整格），seq=2 时照常
  //   落光标。旧宿主也没有 —— 这是产品缺口，不是回归；记成 note，别当成失败把判据钉死
  //   在一个还没实现的行为上。
  await reset();
  const p = await caretPoint(20);
  await mouse('mousePressed', p.x, p.y, { clickCount: 1 });
  await mouse('mouseReleased', p.x, p.y, { clickCount: 1 });
  await mouse('mousePressed', p.x, p.y, { clickCount: 2 });
  await mouse('mouseReleased', p.x, p.y, { clickCount: 2 });
  const dbl = await snap('double-click');
  chk.note(`双击选词：未实现（实测选区 ${dbl.empty ? '空' : '有'}；库只判三击选格）`);
}

// 四、输入法：合成事件走的是库里那条 composition 路径
chk.section('四、输入法');
await reset();
{
  const r = await ev(`
    const e = window.__demo;
    const ta = document.getElementById('ime');
    ta.focus();
    ta.dispatchEvent(new CompositionEvent('compositionstart', { bubbles: true }));
    ta.dispatchEvent(new CompositionEvent('compositionupdate', { bubbles: true, data: 'ni' }));
    await new Promise((r) => setTimeout(r, 20));
    ta.dispatchEvent(new CompositionEvent('compositionend', { bubbles: true, data: '你' }));
    await new Promise((r) => setTimeout(r, 60));
    return { text: e.text(), off: e.caret().pos.offset };`);
  const s = await snap('ime');
  chk.check('输入法上屏的字进了文档', r.text.includes('你'), r.text.split('\n')[0]);
  chk.check('上屏后光标在插入的字之后', r.off >= 1, `off=${r.off}`);
  chk.check('上屏不产生选区', s.empty === true, `empty=${s.empty}`);
}

// 五、滚轮与缩放：滚动不动文档，缩放不动光标所在段
chk.section('五、滚动与缩放');
await reset();
{
  const st0 = await ev(`return document.querySelector('#ed #stage').scrollTop`);
  await cdp.send('Input.dispatchMouseEvent',
                 { type: 'mouseWheel', x: 400, y: 400, deltaX: 0, deltaY: 240 });
  await sleep(120);
  const st1 = await ev(`return document.querySelector('#ed #stage').scrollTop`);
  chk.check('滚轮改变了滚动位置', st1 > st0, `${st0} → ${st1}`);
  const after = await snap('wheel');
  chk.check('滚动不改文档', after.text.includes('第一行 abc'), after.text.split('\n')[0]);

  const z = await ev(`
    const e = window.__demo;
    await e.setZoom(1.5);
    await e.flush?.();
    const g = e.geometry();
    return { w: g.page.w, pages: g.pages, para: e.caret().pos.para, text: e.text() };`);
  chk.check('缩放后页宽变大', z.w > 600, String(z.w));
  chk.check('缩放不动内容', z.text.includes('第一行 abc') && z.para === 0, `para=${z.para}`);
}

// 六、没有 console 错误（输入层自己吞掉的异常也算）
chk.section('六、无错误');
{
  const errs = cdp.consoleErrors.filter((t) => !/favicon/i.test(t));
  chk.check('没有 console 错误', errs.length === 0, errs.slice(0, 2).join(' | '));
}

const fails = chk.rows.filter((x) => !x.ok).length;
// 逐位比对用的一行：两个传输跑出来的 trail 应完全相同。
console.log('TRAIL ' + JSON.stringify(trail.map((t) => [t.label, t.para, t.off, t.empty,
                                                          t.anchor.offset, t.focus.offset])));
console.log(`\n共 ${chk.total} 项断言，失败 ${fails} 项`);
await sleep(50);
if (!process.env.HEDITOR_KEEP_BROWSER) await cdp.send('Browser.close').catch(() => {});
process.exit(fails ? 1 : 0);
