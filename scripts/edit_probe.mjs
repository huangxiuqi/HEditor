// 档 1「可输入」的可证伪验收探针。
//
// 与 web_probe.mjs 的分工：
//   web_probe.mjs  —— 看渲染（各场景的页数/字形数/缺字/耗时）
//   edit_probe.mjs —— 看编辑（这一份），断言四件事：
//
//   键盘路径真的通了：往真实 <textarea> 上派 KeyboardEvent，文档要变、光标要走。
//      （不是调 window.__ced.type() 绕开宿主 —— 那样测的是引擎，不是接线。）
//   档 1 的增量是真的：方向键只重画几百像素，而敲字仍是整页重画。
//      两条都要断言 —— 只断言"很快"是没法证伪的，必须同时钉住"还没做到的那部分"。
//   偏移单位是码点、且会吸附到字素簇：e + U+0301 是一个字符，
//      中间那个偏移不是合法插入点；一次退格删 2 个码点。
//   IME 组合期间一次都不碰引擎，只有 commit 才落文档。
//
// 用法（先起服务 + 带 --remote-debugging-port 的 Chrome）：
//   node scripts/edit_probe.mjs --port 9222 --url http://localhost:8781/web/index.html
import { writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { CDP, Checks, ensureDir, sleep } from './cdp.mjs';

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

const S6 = '患者主诉：反复心悸三天，加重一天。';

// 页面内小工具：把 JS 字符串按码点数长度。
// 不能用 .length —— 那是 UTF-16 码元数，遇到星平面字符会与码点数不等，
//   而这个探针的整个前提就是"引擎的偏移单位是码点"。
const CP_LEN = 'const cplen=(s)=>Array.from(s).length;';

const cdp = { conn: null };

async function boot() {
  const target = await CDP.findPageTarget(PORT);
  const conn = await CDP.connect(target.webSocketDebuggerUrl);
  await conn.send('Page.enable');
  await conn.send('Runtime.enable');
  await conn.send('Log.enable');
  // 必须在 Page.navigate 之前：这个 profile 是跨次运行复用的，
  //   不关缓存就可能加载到上一次的 app.js（注释见 cdp.mjs）。
  await conn.disableHttpCache();
  await conn.send('Emulation.setDeviceMetricsOverride',
                  { width: 1500, height: 1200, deviceScaleFactor: 1, mobile: false });
  console.log('→ 打开', URL_);
  await conn.send('Page.navigate', { url: URL_ });
  const ms = await conn.waitFor('return !!(window.__ced && window.__ced.ready)',
                               'window.__ced.ready');
  console.log(`→ 页面就绪，耗时 ${(ms / 1000).toFixed(1)} s`);
  cdp.conn = conn;
  return conn;
}

/**
 * 切到可编辑场景并拿到"真正的全量"作为分母。
 * 分母只能是首次呈现的写入量：第二次 repaint 本来就是增量（帧尺寸没变，只贴了
 * 一个旧光标），拿它当分母会得出"增量占了 100%"的荒谬结论。
 *
 * 而且必须用 reset() 的返回值，不能用 reset 之后再读一次 editStats：
 * 中间那句 stopBlink() 里的 showCaret 不动像素，但任何一次 present 都会把
 * edit_stats_ 覆盖成"只重画了个光标"。返回值才是那次全量呈现的账。
 */
async function resetAndGetFullBaseline(c) {
  return c.eval(`
    const s = window.__ced.reset(6, ${ZOOM});
    window.__ced.stopBlink();
    return s;`);
}

// 往真实 textarea 派键盘事件（走的是页面的 onKeyDown，不是引擎 API）
const key = (k, extra = '') => `
  window.__ced.textarea.dispatchEvent(
    new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true,
                                   cancelable: true ${extra} }));
  return true;`;

async function main() {
  ensureDir(OUT);
  const c = await boot();
  cdp.conn = c;

  const chk = new Checks('启动');
  const report = { url: URL_, zoom: ZOOM, checks: [], pageErrors: [], consoleErrors: [] };

  chk.section('[1] 全量基准：先量出"整页重画"到底是多少像素');
  {
    const st = await resetAndGetFullBaseline(c);
    report.baseline = st;
    chk.check('切到可编辑场景后确实重排过', st.layoutRan === true);
    // 场景 6 是单段文档：脏段就是第 0 段，前面没有可复用的前缀，
    //   所以这里必然是全量 —— 多段文档的增量见 [9]。不标 ok/warn，
    //   是因为"全量"在这里是唯一可能的结果，不是缺陷。
    chk.check('单段文档的首次排版是全量（没有前缀可复用）', st.layoutWasFull === true);
    chk.check('重画页数 = 总页数', st.repaintedPages === st.totalPages,
              `${st.repaintedPages} / ${st.totalPages}`);
    chk.check('整页重画写入 ≥ 半帧', st.repaintedPixels > st.framePixels / 2,
              `${st.repaintedPixels} / ${st.framePixels}`);
    chk.check('初始文档与预期一致', (await c.eval(CP_LEN + 'return cplen(window.__ced.text);'))
              === Array.from(S6).length);
  }
  const full = report.baseline.repaintedPixels;

  chk.section('[2] 键盘路径：派真实 keydown，文档要变、光标要走（单段文档仍整页重画）');
  {
    const b = await c.eval(CP_LEN + 'return { n: cplen(window.__ced.text), o: window.__ced.caret.offset };');
    await c.eval(key('心'));
    const a = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text), o: window.__ced.caret.offset,
                                              st: window.__ced.editStats };`);
    chk.check('文档码点数 +1', a.n === b.n + 1, `${b.n} → ${a.n}`);
    chk.check('光标停在新字之后（不是字前、也不是跳段首）', a.o === b.o + 1, `${b.o} → ${a.o}`);
    chk.check('插入的就是那个字', (await c.eval('return window.__ced.text.includes("心");')) === true);
    chk.check(' 单段文档：敲字仍是全量重排（脏段就是第一段）',
              a.st.layoutRan === true && a.st.layoutWasFull === true);
    chk.check(' 单段文档：敲字仍是整页重画（所在页内容变了）',
              a.st.repaintedPixels >= full * 0.99,
              `${a.st.repaintedPixels} vs 全量 ${full}`);

    // 改键不该落字：带 ctrl/alt/meta 一律忽略
    //
    // 顺便钉住"该吞的吞、不该吞的不吞"：
    //   dispatchEvent 返回 false 表示监听器调了 preventDefault。
    //   普通可打印字符 → 必须吞掉（否则字符会落进 textarea，和 canvas 上的正文叠成两层）
    //   Ctrl/Alt/Meta 组合 → 必须不吞（否则用户按 Ctrl+C 就被吃掉了）
    //   只断言"文档没变"是测不出后者的 —— 合成事件本来就没有默认行为。
    //
    // 顺序也要小心：下面"该吞的吞"那一条会真的敲进一个 z。
    //   探针自己制造的状态变化必须显式收掉，否则后面每一条断言都被带偏
    //   （否则多出来的 z 会让"退格恢复原样"跟着失败）。
    const beforeMod = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');

    let modSwallowed = 0;
    for (const m of ['ctrlKey', 'altKey', 'metaKey']) {
      const r = await c.eval(`
        const e = new KeyboardEvent('keydown',
          { key: 'x', ${m}: true, bubbles: true, cancelable: true });
        return window.__ced.textarea.dispatchEvent(e);`);
      if (r === false) ++modSwallowed;
    }
    chk.check('Ctrl/Alt/Meta 组合不被吞（否则 Ctrl+C 之类会被吃掉）', modSwallowed === 0,
              `被吞 ${modSwallowed} / 3`);
    const afterMod = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    chk.check('带 Ctrl/Alt/Meta 的按键不落字', afterMod === beforeMod,
              `${beforeMod} → ${afterMod}`);

    const swallow = await c.eval(`
      const e = new KeyboardEvent('keydown', { key: 'z', bubbles: true, cancelable: true });
      return window.__ced.textarea.dispatchEvent(e);   // false = 被 preventDefault
    `);
    chk.check('普通可打印字符的 keydown 被宿主吞掉（不落进 textarea）', swallow === false);
    const afterSwallow = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    chk.check(' 但字确实进了文档一次（"吞掉 keydown" 和 "不落字" 是两回事）',
              afterSwallow === beforeMod + 1, `${beforeMod} → ${afterSwallow}`);

    // 收尾：退格两次，把 z 和前面那个 心 都收掉
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    const back = await c.eval(CP_LEN + 'return { n: cplen(window.__ced.text), o: window.__ced.caret.offset };');
    chk.check('两次退格把文档恢复原样', back.n === b.n && back.o === b.o,
              `${back.n} / offset ${back.o}（期望 ${b.n} / ${b.o}）`);
  }

  chk.section('[3] 方向键 = 真增量：不重排、0 脏页、写入量差两个数量级');
  {
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    await c.eval(key('ArrowRight'));
    const right = await c.eval('return { c: window.__ced.caret, st: window.__ced.editStats };');
    chk.check('右移后光标偏移 +1', right.c.offset === 1, `offset=${right.c.offset}`);
    chk.check(' 光标移动不重排（layoutRan=false）', right.st.layoutRan === false);
    chk.check(' 光标移动重排行数为 0（不是上一次重排的残留）', right.st.relaidLines === 0);
    chk.check(' 光标移动布局耗时为 0', right.st.layoutMs === 0);
    chk.check(' 光标移动 0 个脏页（底图一个字都没变）', right.st.dirtyPageCount === 0);
    chk.check(' 光标移动写入量 < 全量的 1/100',
              right.st.repaintedPixels * 100 < full,
              `${right.st.repaintedPixels} vs ${full}（${(100 * right.st.repaintedPixels / full).toFixed(4)}%）`);

    // 段首再左移：引擎应当返回 false（没动就不该重绘）
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    const at0 = await c.eval('return window.__ced.move(-1);');
    chk.check('已在段首，向左移动不重绘（返回 null）', at0 === null);

    // Home / End
    await c.eval(key('End'));
    const atEnd = await c.eval('return window.__ced.caret.offset;');
    const len = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    chk.check('End 走到段末', atEnd === len, `${atEnd} / 段长 ${len}`);
    await c.eval(key('Home'));
    chk.check('Home 回到段首', (await c.eval('return window.__ced.caret.offset;')) === 0);
  }

  chk.section('[4] 画布点选 = 命中测试往返：正查找出的光标 x，反查必须回到同一个偏移');
  {
    const len = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    const g0 = await c.eval('return window.__ced.caret;');
    const z = await c.eval('return window.__ced.zoomLevel;');

    // 画出来的光标矩形（的东西）应当逐个右移
    const xs = [];
    for (let o = 0; o <= len; ++o) {
      await c.eval(`window.__ced.setCaret(0, ${o}); return true;`);
      xs.push(await c.eval('return window.__ced.caret.x;'));
    }
    let mono = true;
    for (let i = 1; i < xs.length; ++i) if (xs[i] <= xs[i - 1]) mono = false;
    chk.check(`画出的 ${xs.length} 个光标矩形 x 严格递增`, mono,
              `x ∈ [${xs[0]}, ${xs[xs.length - 1]}]`);

    // 从引擎自己报出的几何里取插入点的真实位置。
    //
    //   不要用 caret.x 去算 —— 那是光标矩形的左边缘，不是插入点：
    //   光标以插入点为中心，左边还差半个光标宽。实测差 0.7pt：
    //       行内起点=56.700 pt      光标矩形(页面磅) x=56.000
    //   拿 caret.x 的中点当"势力范围边界"，会让 18 对全错：真正的边界比它靠右
    //   0.7pt，±1px 的探针根本跨不过去。
    //
    //   控件报告里 行内起点 + 本偏移的沿轴位置 才是插入点在帧磅坐标下的位置。
    const repOf = async (o) => {
      await c.eval(`window.__ced.setCaret(0, ${o}); return true;`);
      return c.eval('return window.__ced.caretReport;');
    };
    const rep0 = await repOf(0);
    const mOrigin = /行内起点=([-\d.]+) pt/.exec(rep0);
    if (!mOrigin) throw new Error('caretReport 里找不到「行内起点」，格式变了？');
    const originPt = Number(mOrigin[1]);

    const insPt = [0];
    for (let o = 1; o <= len; ++o) {
      const rep = await repOf(o);
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      if (!m) throw new Error('caretReport 里找不到「本偏移的沿轴位置」，格式变了？');
      insPt.push(Number(m[1]));
    }
    const absPt = insPt.map((v) => originPt + v);   // 帧磅（单页文档，页内磅 = 帧磅）
    let m2 = true;
    for (let i = 1; i < absPt.length; ++i) if (absPt[i] <= absPt[i - 1]) m2 = false;
    chk.check(`引擎报出的 ${absPt.length} 个插入点位置严格递增`, m2,
              `pt ∈ [${absPt[0].toFixed(2)}, ${absPt[absPt.length - 1].toFixed(2)}]`);

    // 在每个插入点处派真实 mousedown，看落点是否就是那个偏移
    let ok = 0;
    for (let o = 0; o <= len; ++o) {
      const hit = await c.eval(`
        const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
        const sx = r.width / cv.width, sy = r.height / cv.height;
        const zz = window.__ced.zoomLevel;
        // 帧磅 → 帧像素（× zoom）→ 客户端像素（× CSS 缩放）
        const clientX = r.left + (${absPt[o] + 0.2} * zz) * sx;
        const clientY = r.top + ${g0.y + g0.h / 2} * sy;
        cv.dispatchEvent(new MouseEvent('mousedown',
          { clientX, clientY, bubbles: true, cancelable: true }));
        return window.__ced.caret.offset;`);
      if (hit === o) ++ok;
    }
    chk.check(` ${len + 1} 个插入点，点画布反查全部回到原位置（画光标与点选共用同一份行盒几何）`,
              ok === len + 1, `成功 ${ok} / ${len + 1}`);

    // 中点规则：两个相邻插入点的中点两侧，应当分别落到左、右两个插入点。
    //   这就是"点 e+U+0301 的右半边，光标跑到它后面，而不是钻进它内部"那条语义 ——
    //   它必须能被证伪，而不是只在注释里写着。
    const yPt = (g0.y + g0.h / 2) / z;
    let bok = 0;
    const bad = [];
    for (let o = 0; o < len; ++o) {
      const mid = (absPt[o] + absPt[o + 1]) / 2;
      const left = await c.eval(`return window.__ced.hit(${mid - 0.2}, ${yPt});`);
      const right = await c.eval(`return window.__ced.hit(${mid + 0.2}, ${yPt});`);
      if (left && right && left.offset === o && right.offset === o + 1) ++bok;
      else bad.push(`o=${o}: 左→${left ? left.offset : 'null'} 右→${right ? right.offset : 'null'}`);
    }
    chk.check(` ${len} 对相邻插入点，中点左半→左边、右半→右边（势力范围 = 到邻居的中点）`,
              bok === len, `成立 ${bok} / ${len}${bad.length ? '；反例 ' + bad.slice(0, 3).join(' | ') : ''}`);

    // 点在画布左边界之外：命中必须失败，而不是胡乱给一个位置。
    // （注意：页内的空白不算失败 —— hitTestPt 的语义是"取同页最近的一行"，
    //   所以"点页面底部空白光标会跳到第一行"是正确行为，不该拿来断言。）
    await c.eval('window.__ced.setCaret(0, 5); return true;');
    const outside = await c.eval(`
      const cv = window.__ced.canvas, r = cv.getBoundingClientRect();
      const sy = r.height / cv.height;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left - 50, clientY: r.top + (window.__ced.caret.y + 5) * sy,
          bubbles: true, cancelable: true }));
      return window.__ced.caret.offset;`);
    chk.check('点画布左边界之外 → 命中失败，光标停在原地（不猜）', outside === 5,
              `offset=${outside}`);
  }

  chk.section('[5] 偏移单位是码点、且吸附到字素簇（经 WASM 边界验证）');
  {
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    await c.eval('return window.__ced.type("e");');
    await c.eval('return window.__ced.type("\\u0301");');   // 组合重音 U+0301
    const st = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                               o: window.__ced.caret.offset };`);
    chk.check('e 类序列：插入 2 个码点，光标偏移 = 2（偏移数的是码点）', st.o === 2, `offset=${st.o}`);

    const posOf = async (o) => {
      await c.eval(`window.__ced.setCaret(0, ${o}); return true;`);
      const rep = await c.eval('return window.__ced.caretReport;');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const p0 = await posOf(0), p1 = await posOf(1), p2 = await posOf(2);
    chk.check(' 偏移 1 不是合法插入点 → 吸附到簇首，位置与偏移 0 相同',
              Math.abs(p1 - p0) < 1e-6, `pos(0)=${p0} pos(1)=${p1}`);
    chk.check('偏移 2 已经是下一个簇 → 位置前进', p2 > p0, `pos(2)=${p2}`);

    const cps = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    await c.eval('window.__ced.setCaret(0, 2); return true;');
    await c.eval('return window.__ced.backspace();');
    const after = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                                 o: window.__ced.caret.offset };`);
    chk.check(' 一次退格删掉 2 个码点（删的是簇，不是码点；否则会留下孤立的组合重音）',
              after.n === cps - 2 && after.o === 0, `${cps} → ${after.n}，offset=${after.o}`);
  }

  chk.section('[5b] 连字 ff：真实按键路径下，两个 f 就是两个字符');
  {
    // 这一节守的是删除语义，只能在这一层验。
    //
    //   原生单测（edit_test.cpp 的 [8]）验的是引擎内部的簇表；而"用户按了两次 f 键"
    //   这条路上还多了一层宿主事件分发 —— 万一宿主把两次 keydown 合并、或者
    //   textarea 的 value 攒了一串才 commit，簇表再对也没用。所以这里必须
    //   派真实的 KeyboardEvent，而不是调 window.__ced.type()。
    //
    //   为什么偏偏是 f：liga 连字表里 ff / fi / fl / ffi / ffl 全是以 f 开头的，
    //   而中文测试一个都盖不到 —— 这正是这个 bug 能活到现在的原因。
    await c.eval(`window.__ced.reset(6, ${ZOOM}); window.__ced.stopBlink(); return true;`);
    await c.eval('window.__ced.setCaret(0, 0); return true;');

    const b = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    await c.eval(key('f'));
    await c.eval(key('f'));
    const a = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                               t: window.__ced.text,
                                               o: window.__ced.caret.offset };`);
    chk.check('两次按 f → 文档 +2 个码点', a.n === b + 2, `${b} → ${a.n}`);
    chk.check('两个 f 都真的写进了文本', Array.from(a.t).slice(0, 2).join('') === 'ff',
              JSON.stringify(Array.from(a.t).slice(0, 4).join('')));
    chk.check('光标停在 2 —— 每个 f 各占一格（不是 1）', a.o === 2, `offset=${a.o}`);

    // 决定性判据：偏移 1（两个 f 之间）必须是一个合法插入点。
    //   连字一旦把 ff 合成一个簇，解析器给出的簇表里就没有偏移 1 的起点，
    //   光标会被吸附回 0 → 这两次取值相等。用 caretReport 的沿轴位置直接比。
    const posOf = async (o) => {
      await c.eval(`window.__ced.setCaret(0, ${o}); return true;`);
      const rep = await c.eval('return window.__ced.caretReport;');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const q0 = await posOf(0), q1 = await posOf(1);
    chk.check(' 偏移 1（两个 f 之间）是合法插入点，位置严格前移（连字一合并这里就相等）',
              q1 > q0 + 1e-6, `pos(0)=${q0} pos(1)=${q1}`);

    await c.eval('window.__ced.setCaret(0, 2); return true;');
    await c.eval(key('Backspace'));
    const d = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                               o: window.__ced.caret.offset };`);
    chk.check(' 一次退格只删掉一个 f（连字一合并这里会一下少两个）',
              d.n === a.n - 1 && d.o === 1, `${a.n} → ${d.n}，offset=${d.o}`);

    // Delete 走的是另一条分支（clusterCeil），也得守。
    // 必须先把光标移回 0：退格之后光标停在 1，而 1 处是原文第一个字，
    //   在那儿按 Delete 删掉的是"患"而不是剩余的那个 f —— 那测的就不是连字了。
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    await c.eval(key('Delete'));
    const fwd = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                                 o: window.__ced.caret.offset };`);
    chk.check(' Delete 也只删掉了那一个 f', fwd.n === d.n - 1 && fwd.o === 0,
              `${d.n} → ${fwd.n}，offset=${fwd.o}`);

    // 收尾：把文档恢复成 reset 后的原样，后面几节才不会被这一节带偏
    await c.eval(`window.__ced.reset(6, ${ZOOM}); window.__ced.stopBlink(); return true;`);
    const restored = await c.eval(CP_LEN + 'return cplen(window.__ced.text);');
    chk.check('收尾：文档已恢复（后续小节的起点干净）', restored === Array.from(S6).length,
              `${restored} / ${Array.from(S6).length}`);
  }

  chk.section('[6] IME：组合期间一次都不碰引擎，只有 commit 才落文档');
  {
    await c.eval('window.__ced.setCaret(0, 0); return true;');
    const before = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text), t: window.__ced.text };`);

    await c.eval(`
      window.__ced.textarea.dispatchEvent(
        new CompositionEvent('compositionstart', { bubbles: true }));
      return true;`);
    // 组合更新来多少次都不该动文档
    for (const d of ['n', 'ni', 'ni3']) {
      await c.eval(`
        window.__ced.textarea.dispatchEvent(
          new CompositionEvent('compositionupdate', { bubbles: true, data: ${JSON.stringify(d)} }));
        return true;`);
    }
    const mid = await c.eval(CP_LEN + 'return { n: cplen(window.__ced.text), t: window.__ced.text };');
    chk.check(' 组合期间（3 次 compositionupdate）文档一个字都没变',
              mid.n === before.n && mid.t === before.t, `${before.n} → ${mid.n}`);

    await c.eval(`
      window.__ced.textarea.dispatchEvent(
        new CompositionEvent('compositionend', { bubbles: true, data: '\\u4f60' }));
      return true;`);
    const after = await c.eval(CP_LEN + `return { n: cplen(window.__ced.text),
                                                  t: window.__ced.text,
                                                  o: window.__ced.caret.offset };`);
    chk.check('commit 后文档 +1 码点', after.n === before.n + 1, `${before.n} → ${after.n}`);
    chk.check('commit 落下的就是那个字（你）', after.t.startsWith('你') && after.t.length > 0);
    chk.check('commit 后光标停在新字之后', after.o === 1, `offset=${after.o}`);
    chk.check('状态栏已退出"组合中"', (await c.eval('return document.getElementById("status").textContent;'))
              .indexOf('组合中') < 0);
  }

  chk.section('[2b] 连打一长串字符：屏幕必须等于引擎那一帧（含闪烁在跑）');
  {
    // 这条断言的存在理由（真机现象）：
    //   "输入时光标往后走，但插入处的文字没有往后移，两个字符叠在一起。"
    //   这类症状只可能出在两处：引擎画错，或者上屏那一小步贴错。
    //   所以下面同时钉两根桩：
    //   屏幕（canvas 的真实像素）必须逐字节等于引擎当前那一帧 —— 抓"上屏贴错"。
    //   一个一个真按键敲出来的那一帧，必须逐字节等于同样文档一次性插入后的那一帧
    //   —— 抓"增量重绘与全量重绘不一致"。
    //
    //   两个刻意的设计，不许省：
    //     闪烁保持开着，并按 ~110ms 的间隔按键 —— 真人是这个节奏，
    //       而闪烁每 530ms 会自己 present 一次。关掉闪烁去测，等于把这条
    //       最容易出问题的交错整个绕过了。
    //     两次采样之间不能有 await 之外的东西：canvas 和引擎帧必须在
    //       同一时刻读，否则光标相位不同，差异里混着闪烁。
    const RUN = 'ssssss' + 'd'.repeat(15) + 'f'.repeat(14) + 'g'.repeat(25);
    const AT = 8;   // 「患者主诉：反复心」之后（8 个码点）

    // 同刻读 canvas 与引擎帧并逐字节比较
    const screenVsFrame = () => c.eval(`
      const cv = document.getElementById('cv');
      const w = window.__ced.frameW(), h = window.__ced.frameH();
      const g = cv.getContext('2d', { willReadFrequently: true });
      const S = g.getImageData(0, 0, w, h).data;
      const F = window.__ced.rawFrame();
      const n = Math.min(S.length, F.length);
      let diff = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
      for (let i = 0; i + 3 < n; i += 4) {
        if (S[i] !== F[i] || S[i+1] !== F[i+1] || S[i+2] !== F[i+2]) {
          ++diff;
          const p = i >> 2, x = p % w, y = (p - x) / w;
          if (x < minx) minx = x; if (x > maxx) maxx = x;
          if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
      }
      return { diff, w, h, minx, miny, maxx, maxy, lenS: S.length, lenF: F.length };
    `);

    const resetTo = async () => c.eval(`
      window.__ced.reset(6, ${ZOOM});
      window.__ced.setCaret(0, ${AT});
      return true;`);

    // A：逐个真按键，闪烁开着，按 ~110ms 的真人节奏
    await resetTo();
    const perKey = [];
    const drift = [];
    for (let i = 0; i < RUN.length; ++i) {
      await c.eval(key(RUN[i]));
      perKey.push(await c.eval('return window.__ced.editStats;'));
      // 每一次按键都查，不是抽样。
      //   "光标走了、文字没动"这种症状只在某一次贴错；抽样会把漏掉。
      drift.push(await screenVsFrame());
      await sleep(110);
    }
    const worstDrift = drift.reduce((m, d) => Math.max(m, d.diff), 0);
    const worstBox = drift.find((d) => d.diff === worstDrift) || {};
    const textA = await c.eval('return window.__ced.text;');
    // 定格成确定的相位再取 A 帧，否则和 B 帧的差异里混着闪烁
    await c.eval(`window.__ced.stopBlink(); window.__ced.present(); return true;`);
    await c.eval('window.__frameA = window.__ced.rawFrame(); return true;');
    await c.screenshot(join(OUT, 'typing-state.png'));

    // B：同样的文档，一次性插入，同样定格
    await resetTo();
    await c.eval(`return window.__ced.type(${JSON.stringify(RUN)});`);
    const textB = await c.eval('return window.__ced.text;');
    await c.eval(`window.__ced.stopBlink(); window.__ced.present(); return true;`);

    const d = await c.eval(`
      const A = window.__frameA;
      const B = window.__ced.rawFrame();
      const w = window.__ced.frameW();
      const n = Math.min(A.length, B.length);
      let diff = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
      for (let i = 0; i + 3 < n; i += 4) {
        if (A[i] !== B[i] || A[i+1] !== B[i+1] || A[i+2] !== B[i+2]) {
          ++diff;
          const p = i >> 2, x = p % w, y = (p - x) / w;
          if (x < minx) minx = x; if (x > maxx) maxx = x;
          if (y < miny) miny = y; if (y > maxy) maxy = y;
        }
      }
      return { diff, w, h: window.__ced.frameH(), minx, miny, maxx, maxy,
               lenA: A.length, lenB: B.length };
    `);

    const full = report.baseline.repaintedPixels;
    const minPx = perKey.reduce((m, s) => Math.min(m, s.repaintedPixels), Infinity);
    chk.check('文档内容一致（逐个敲 vs 一次性插入）',
              textA === textB && Array.from(textA).length > AT,
              `${Array.from(textA).length} vs ${Array.from(textB).length} 码点`);
    chk.check(' 屏幕像素 == 引擎帧（闪烁在跑、110ms 节奏连打，每一次按键都查）',
              worstDrift === 0,
              worstDrift === 0 ? '全部相同'
                : `最大差 ${worstDrift} 像素，包围盒 x∈[${worstBox.minx},${worstBox.maxx}]` +
                  ` y∈[${worstBox.miny},${worstBox.maxy}]（帧 ${worstBox.w}×${worstBox.h}）`);
    chk.check(' 帧逐字节相同：增量重绘的结果 == 重新排一遍的结果',
              d.diff === 0 && d.lenA === d.lenB,
              d.diff === 0
                ? '完全相同'
                : `差 ${d.diff} 像素，包围盒 x∈[${d.minx},${d.maxx}] y∈[${d.miny},${d.maxy}]` +
                  `（帧 ${d.w}×${d.h}）`);
    chk.check(' 每一次按键都整页重画（否则上面那条可能是"两边错得一样"）',
              minPx >= full * 0.99, `最小那次 ${minPx} vs 全量 ${full}`);

    // 把给用户用的取证接口也当场跑一遍 —— 它要是抛异常，用户那边就白跑一趟。
    const sc = await c.eval('return window.__ced.selfCheck();');
    chk.check(' 自检接口可用且当场判为一致（__ced.selfCheck()）',
              !!sc && sc.screenVsEngineDiffPx === 0 && Array.isArray(sc.framePx),
              sc ? `屏幕与引擎帧差 ${sc.screenVsEngineDiffPx} px，帧 ${sc.framePx.join('×')}，` +
                   `dpr=${sc.dpr}，canvas css [${sc.canvas.cssSize}] / rect [${sc.canvas.rectPx}]`
                 : '返回 null');
  }

  chk.section('[8] 表单：控件是「区间」、边界是「策略」');
  {
    // 这一节要证明四件事，缺一不可：
    //   标记真的被画出来了（不是"数据里有控件、画面上什么都没有"）；
    //   括号常驻：每个字段的左括号一直在，且不随光标变（默认档 All）；
    //   字段被删空之后括号仍然在（内容没了、"位置还占着"、括号却消失了）；
    //   切回文档模式标记真的消失。
    //
    // 标记色 = 亮蓝 rgb(56,139,253)，与 EngineConfig::control_mark 一致
    // —— 刻意和光标色 (31,111,235) 不同，否则像素断言里分不清"括号"和"光标"。
    //
    // 判据 = 方向性（"偏蓝得够远、够亮、红通道不太低"），不是围绕标记色的矩形容差：
    //   括号是字形、笔画细，大部分像素是"标记色 ↔ 白"的混色，矩形容差（|r-56|<=20）
    //   会把它们判成"不是标记"；而同一份画面里矩形容差只数到 1 px、方向性判据数到
    //   5~20 px。相位或粗细一变就翻脸的断言等于没有断言。
    //   光标色 (31,111,235) 的 r=31 < 45，被方向性判据排除 —— 这是刻意留的口子，
    //   否则"光标旁边的括号"和"光标本身"分不开。
    //   只读字段的括号是灰的（168,168,168 ⇒ b-r=0），两种判据都不认它。
    const MARK = `(r, g, b) => r > 45 && b - r > 60 && b > 140`;
    const inkMark = `
      const mark = ${MARK};   // 光标色 r=31 被排除
      const f = window.__ced.rawFrame();
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        if (mark(f[i], f[i+1], f[i+2])) ++n;
      }
      return n;
    `;
    // 统计某条竖带里的标记像素 —— 括号就画在控件左缘外侧。
    //
    // 竖带取 [cx-10, cx-1] 磅（全部在左缘左边），这个范围是量出来的：
    //   括号 pen_x = 左缘 - adv - gap，而 adv 随字号走（14pt 下约 4.2pt，
    //   加上 1.5pt 的 gap ⇒ 左缘左侧约 6pt 起步，笔画本身还再往里几像素）。
    //   窗口取 [cx-4, cx+1] 时恰好差 1 像素没框住括号，断言会红而画面其实是对的
    //   （诊断显示左括号在 x=183、控件左缘在 x=190）。
    //   结论：像素断言的窗口要按"实际量出来的位置"定，不能按几何直觉估。
    //   上限取 cx-1（而不是 cx+2）是为了不碰前一个字段的右括号 ——
    //   那会让 Focused 档下"非聚焦字段左缘应为空"这条假阳性。
    // 必须带 return：探针的 eval 直接执行这段表达式，没有 return 的话
    //   结果就是 undefined（断言会以 "undefined px" 的形式失败，很好认）。
    const bandAt = (cxPt) => `
      return (function () {
        const near = ${MARK};
        const f = window.__ced.rawFrame();
        const fw = window.__ced.frameW();
        const lo = ${Math.round((cxPt - 10) * 1.5)}, hi = ${Math.round((cxPt - 1) * 1.5)};
        let n = 0;
        for (let i = 0; i + 3 < f.length; i += 4) {
          const x = (i >> 2) % fw;
          if (x < lo || x > hi) continue;
          if (near(f[i], f[i+1], f[i+2])) ++n;
        }
        return n;
      })()
    `;

    // 一个括号槽的像素宽 = 括号字形 advance（12pt 下实测 4.055pt）
    //   + 排版留的间隙 kControlBracketSlotGapPt。间隙也算宽度，所以槽才是
    //   "一个字被推开多少"的单位 —— 用括号 advance 会系统性偏小 1.5pt。
    // 提到这一节的最外层：下面好几个子块都要用它（含"空字段"那块）。
    const bracketSlotPx = (4.055 + 1.5) * ZOOM;

    await c.eval('return window.__ced.loadFormSample(0);');
    const ct = await c.eval('return window.__ced.controls;');
    chk.check('载入表单样例：3 个控件、已切到表单模式',
              ct.count === 3 && ct.mode === 1, `count=${ct.count} mode=${ct.mode}`);

    // 像素级：括号必须把画出来的字推开，而不只是把几何数字改大。
    //
    //   这条断言的存在理由（一个真实且不易察觉的缺陷）：
    //     行盒的簇位置来自 cluster_adv_pt（含占位宽度），而字形位置只用
    //     pen_x += sg.x_advance 一个个字形累加 —— 占位簇没有字形（g0 == g1），
    //     那个循环一次都不执行。于是光标/控件矩形按"字让开了"算，画出来的字
    //     却没让开，表象就是方括号压在文字上。
    //     而单测里那条"首行宽变大"只查了 inline_end_pt（= cluster_adv_pt 之和），
    //     与"字形画在哪"是两套数 —— 它结构上永远抓不到这个 bug。
    //
    //   做法：同一段文字（scenario 6）两遍，一遍不带表单、一遍带表单且光标在字段里，
    //   只比最右那个墨迹列簇的左缘。它应当整整移开两个括号宽（[ 与 ]）。
    {
      const lastInkLeft = `
        return (function () {
          const f = window.__ced.rawFrame();
          const W = window.__ced.frameW(), H = window.__ced.frameH();
          const at = (x, y) => { const i = (y * W + x) * 4; return [f[i], f[i+1], f[i+2]]; };
          const dark = (r, g, b) => r < 120 && g < 120 && b < 120;
          const rows = [];
          for (let y = 0; y < H; ++y) {
            let n = 0;
            for (let x = 0; x < W; ++x) { const [r,g,b] = at(x,y); if (dark(r,g,b)) ++n; }
            rows.push(n);
          }
          let s = -1; const bands = [];
          for (let y = 0; y < H; ++y) {
            if (rows[y] > 0) { if (s < 0) s = y; }
            else { if (s >= 0) { bands.push([s, y - 1]); s = -1; } }
          }
          if (s >= 0) bands.push([s, H - 1]);
          const band = bands.find(g => g[1] - g[0] > 4) || bands[0] || [0, 0];
          const xs = [];
          for (let x = 0; x < W; ++x) {
            let hit = false;
            for (let y = band[0]; y <= band[1]; ++y) {
              const [r,g,b] = at(x,y); if (dark(r,g,b)) { hit = true; break; }
            }
            if (hit) xs.push(x);
          }
          if (!xs.length) return { left: -1, right: -1 };
          let a = xs[xs.length - 1], p = xs[xs.length - 1];
          for (let i = xs.length - 2; i >= 0; --i) {
            if (p - xs[i] > 3) break;
            a = xs[i]; p = xs[i];
          }
          return { left: a, right: xs[xs.length - 1] };
        })()
      `;
      // 期望位移现在是 6 个槽（不是 2 个）：默认档 All ——
      //   表单样例的 3 个字段（complaint / duration(只读) / aggravate）
      //   每个都常驻一对括号 ⇒ 行尾那个字被推开 3×2 = 6 个槽。
      //   这条数字同时钉死了"默认档 = 全部字段占位"：改回 Focused 档会变成 2 个槽。
      const expectSlots = 6;

      await c.eval(`window.__ced.reset(6, ${ZOOM}); window.__ced.stopBlink(); return true;`);
      await c.eval('window.__ced.present(); return true;');
      const base = await c.eval(lastInkLeft);

      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.stopBlink();');
      await c.eval('return window.__ced.setCaret(0, 6);');
      await c.eval('window.__ced.present(); return true;');
      const form = await c.eval(lastInkLeft);

      // 比的是右缘（最右墨迹列），不是左缘。
      //   量出来过：无表单时最后一组墨迹列是 [402,427]，带表单时是 [441,446] ——
      //   左缘跳了 39px，右缘只跳 19px。原因不是排版错了，是分组变了：
      //   字距一变。 和它前面的字就从"同一组"（间隙 <=3px）变成"两组"。
      //   拿左缘当判据 = 拿一个会重组的东西当尺子。右缘只跟"最后一个字画在哪"
      //   有关，是单调的。
      const dx = form.right - base.right;
      // 为什么给的是"1.5~2.5 个括号槽"这个区间，而不是 ≈ 2 × slot × zoom 的等号：
      //   这里量的是"第一个被判为墨迹的像素列"，它是一次阈值穿越，
      //   受字形边缘抗锯齿影响有 ±2px 抖动。
      //   真正要区分的是三件事：没让开（0）／只让一个槽（≈8）／让两个（≈17）。
      //   区间 (12.5, 20.8) 把前两种都判红，又不会被 AA 抖动误伤。
      // 窗口给的是 ±1 个槽（而不是 ±0.5）：bracketSlotPx 里的 4.055pt 是
      //   native 单测里量出来的括号 advance，而浏览器表单样例用的是同一族字体
      //   的另一个字号/实例，实测槽宽 ≈ 9.33px（= 6.22pt）而不是 8.33px。
      //   真正要区分的是三件事：没让开（0）／只让一对（≈19）／让三对（≈56）——
      //   ±1 槽的窗口把前两种都判红，又不会被这点系统性偏差误伤。
      //   （只写 dx ≈ expectSlots 的话，改回 Focused 档时这里会以"差一点"的形式
      //     含糊地红掉；写成区间 + 打印实测值，一眼就能看出是档位错了还是没画。）
      chk.check(` 行尾字形真被推开了 ${expectSlots} 个括号槽（像素证据，不是只看几何数字）`,
                base.right > 0 && dx > (expectSlots - 1) * bracketSlotPx &&
                  dx < (expectSlots + 1) * bracketSlotPx,
                `行尾墨迹右缘 ${base.right} → ${form.right}（Δ${dx} px，` +
                `期望 ${expectSlots - 1}~${expectSlots + 1} 个括号槽 = ` +
                `${((expectSlots - 1)*bracketSlotPx).toFixed(1)}~${((expectSlots + 1)*bracketSlotPx).toFixed(1)}）`);
    }

    const markForm = await c.eval(inkMark);
    chk.check(' 表单模式下标记真的画出来了（亮蓝像素，像素证据）',
              markForm > 50, `${markForm} px`);

    // 控件是「区间」→ 它投影出的矩形来自行盒几何，短字段应当恰好 1 块
    const r0 = ct.items.length ? ct.items[0].rects : [];
    chk.check('字段 #0 投影出 1 个矩形块，尺寸合法',
              r0.length === 1 && r0[0].w > 20 && r0[0].h > 5,
              r0.length ? `${r0[0].w.toFixed(1)} × ${r0[0].h.toFixed(1)} pt` : '无矩形');

    // 括号常驻：光标在 #0 还是在 #2，两个字段的左括号都应该在，
    //   而且墨迹量完全一样（不随光标变）。
    //   （只统计"总量"是区分不出来的 —— 所以这里逐字段取竖带。）
    //
    // 每一次 setCaret 之后仍然重新读矩形：默认档下它确实不随光标动，
    //   但"不随光标动"本身就是要验的东西 —— 缓存矩形等于把它假设掉。
    if (ct.count >= 3) {
      const rectX = async (i) => {
        const cc = await c.eval('return window.__ced.controls;');
        return cc.items[i] && cc.items[i].rects[0] ? cc.items[i].rects[0].x : 0;
      };

      await c.eval('return window.__ced.setCaret(0, 6);');  // 在 #0 里
      const a0 = await c.eval(bandAt(await rectX(0)));
      const a2 = await c.eval(bandAt(await rectX(2)));

      await c.eval('return window.__ced.setCaret(0, 13);');  // 在 #2 里
      const b0 = await c.eval(bandAt(await rectX(0)));
      const b2 = await c.eval(bandAt(await rectX(2)));

      chk.check(' 光标在 #0 时，#0 与 #2 的左括号都在（常驻，不看光标）',
                a0 > 0 && a2 > 0, `#0 左缘 ${a0} px / #2 左缘 ${a2} px`);
      chk.check(' 光标移到 #2：两个左括号仍然都在，且墨迹量不变（常驻的硬判据）',
                b0 > 0 && b2 > 0 && b0 === a0 && b2 === a2,
                `#0 左缘 ${a0}→${b0} px / #2 左缘 ${a2}→${b2} px`);

      // [诊断] 把 #0 那一行的蓝色列打出来 —— 用来核对"竖带窗口是否真的框住了括号"。
      {
        const d = await c.eval(`
          const f = window.__ced.rawFrame();
          const fw = window.__ced.frameW();
          const near = ${MARK};
          const it = window.__ced.controls.items[0];
          const r0 = it.rects[0];
          const cols = new Set();
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < (r0.y - 2) * 1.5 || y > (r0.y + r0.h + 2) * 1.5) continue;
            if (near(f[i], f[i+1], f[i+2])) cols.add(x);
          }
          return { cxPx: Math.round(r0.x * 1.5), rect: r0,
                   cols: Array.from(cols).sort((a, b) => a - b) };
        `);
        console.log(`     [诊断] #0 内容左缘 ${d.cxPx} px（rect x=${d.rect.x.toFixed(2)} w=${d.rect.w.toFixed(2)}）；`
          + `该行蓝色列 = ${JSON.stringify(d.cols)}`);
      }

      // ⭐ 那个 bug：把字段内容删光，括号不能消失
      //
      //   原话：「文本控件删除最后一个字符后，括号也不渲染了，但是好像括号位置
      //   还占着，输入一个字符后括号出现」。
      //
      //   判据是像素级的，而且要分别验证左右两个括号：
      //     删空后控件矩形必然为空（rects: []），任何"按矩形找括号"的测法都会
      //     失效 —— 所以这里用光标 x 当基准：空字段里光标就停在槽的中间，
      //     左括号在它左边、右括号在它在右边（间隙各 1.5pt）。
      //     光标自己的颜色（r=31）被 mark 判据排除，不会污染计数。
      {
        await c.eval('return window.__ced.loadFormSample(0);');
        await c.eval('return window.__ced.stopBlink();');
        await c.eval('return window.__ced.setCaret(0, 9);');   // complaint [5,9) 的末尾
        for (let i = 0; i < 4; ++i) await c.eval(key('Backspace'));  // 真实键盘删光它
        await c.eval('window.__ced.present(); return true;');

        const cc = await c.eval('return window.__ced.controls;');
        const k0 = cc.items[0];
        const emptyRect = k0 && (!k0.rects || k0.rects.length === 0);
        chk.check('⭐ 前置：字段内容被删空后，它投影不出矩形'
                  + '（老实现正是死在这一步：没有矩形 → 没有标记）',
                  emptyRect, `rects=${JSON.stringify(k0 && k0.rects)}`);

        const slotPx = bracketSlotPx;
        const empty = await c.eval(`
          const st = window.__ced.editStats;
          const f = window.__ced.rawFrame();
          const fw = window.__ced.frameW();
          const mark = (r, g, b) => r > 45 && b - r > 60 && b > 140;   // 光标色 r=31 被排除
          const cx = st.caretX;
          let left = 0, right = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < st.caretY || y >= st.caretY + st.caretH) continue;
            if (!mark(f[i], f[i + 1], f[i + 2])) continue;
            if (x < cx - 3 && x > cx - ${Math.ceil(slotPx + 4)}) ++left;
            if (x > cx + 3 && x < cx + ${Math.ceil(slotPx + 4)}) ++right;
          }
          return { caretX: cx, caretOff: st.caretOffset, left, right };
        `);
        chk.check(' 前置：光标停在空字段里（偏移 5）',
                  empty.caretOff === 5, `offset=${empty.caretOff}`);
        chk.check('⭐ 空字段的左括号照样画出来了（在光标左边）',
                  empty.left > 0, `光标左 ${empty.left} px（caretX=${empty.caretX}）`);
        chk.check('⭐ 空字段的右括号也画出来了（在光标右边）',
                  empty.right > 0, `光标右 ${empty.right} px`);

        // 引擎帧 / 屏幕也要一致：的是"屏幕上没有括号"，
        //   所以只查帧还不够 —— 上屏那一步漏了照样是他报的现象。
        const scEmpty = await c.eval('return window.__ced.selfCheck();');
        const auEmpty = await c.eval('return window.__ced.auditFrame();');
        chk.check(' 删空字段后：引擎帧 == 整帧重画', auEmpty && auEmpty.diff === 0,
                  auEmpty ? `差 ${auEmpty.diff} px` : 'null');
        chk.check(' 删空字段后：屏幕 == 引擎帧（括号真的在屏幕上）',
                  scEmpty.screenVsEngineDiffPx === 0,
                  `差 ${scEmpty.screenVsEngineDiffPx} px`);
      }

      // ⭐ 相邻字段的括号顺序：][ 不是 []
      //
      //  （截图里是 心悸[]三天）：两个字段紧挨着时，前一个字段的
      //  右括号画到了后一个字段左括号的右边 —— 顺序反了。
      //  表单样例正好就是这一对：complaint[5,9)（可编辑→亮蓝）+ duration[9,11)
      //  （只读→灰）—— 所以"谁在左"可以用颜色直接读出来，不必猜字形。
      {
        await c.eval('return window.__ced.loadFormSample(0);');
        await c.eval('return window.__ced.stopBlink();');
        await c.eval('return window.__ced.setCaret(0, 6);');
        await c.eval('window.__ced.present(); return true;');

        const ord = await c.eval(`
          const f = window.__ced.rawFrame();
          const fw = window.__ced.frameW();
          const cc = window.__ced.controls;
          const A = cc.items[0], B = cc.items[1];      // complaint / duration(只读)
          const slotPx = ${bracketSlotPx};
          const x0 = Math.round((A.rects[0].x + A.rects[0].w) * 1.5) - 2;
          const x1 = Math.round((A.rects[0].x + A.rects[0].w) * 1.5 + 2 * slotPx) - 2;
          const isBlue = (r, g, b) => r > 45 && b - r > 60 && b > 140;
          const isGray = (r, g, b) => {
            const drg = Math.abs(r - g), dgb = Math.abs(g - b);
            return r >= 140 && r <= 195 && drg <= 8 && dgb <= 8;
          };
          let blueLo = -1, blueHi = -1, grayLo = -1, grayHi = -1;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw;
            if (x < x0 || x > x1) continue;
            const r = f[i], g = f[i+1], b = f[i+2];
            if (isBlue(r, g, b)) { if (blueLo < 0) blueLo = x; blueHi = x; }
            if (isGray(r, g, b)) { if (grayLo < 0) grayLo = x; grayHi = x; }
          }
          return { x0, x1, blueLo, blueHi, grayLo, grayHi };
        `);
        chk.check('⭐⭐ 相邻字段共享边界：亮蓝 `]`（A 的）整个在灰 `[`（B 的）左边（`][`）',
                  ord.blueLo >= 0 && ord.grayLo >= 0 && ord.blueHi < ord.grayLo,
                  `窗口 x∈[${ord.x0},${ord.x1}]：蓝 ${ord.blueLo}..${ord.blueHi} ｜ ` +
                  `灰 ${ord.grayLo}..${ord.grayHi}` +
                  (ord.blueHi < ord.grayLo ? '' : '  ← 顺序反了，就是截图里的 `[]`'));
      }

      // ⭐ 运行期切换显示范围：__ced.setBracketScope(0|1)
      //
      //   这条测的是"改配置也要重排"：括号进文档流，显示范围一变占位就变，
      //     正文得跟着挪。只重绘不重排的话，括号会按新档画、正文却按旧档排 ——
      //     表象是括号盖在字上/槽里空一条，而且从数字上看不出来。
      //   判据取"切档前后同一个字段的竖带"，加上屏幕/帧一致。
      {
        await c.eval('return window.__ced.loadFormSample(0);');
        await c.eval('return window.__ced.stopBlink();');
        await c.eval('return window.__ced.setCaret(0, 6);');   // 光标在 #0
        await c.eval('window.__ced.present(); return true;');
        const all0 = await c.eval(bandAt(await rectX(0)));
        const all2 = await c.eval(bandAt(await rectX(2)));

        await c.eval('return window.__ced.setBracketScope(1);');   // → Focused
        const foc0 = await c.eval(bandAt(await rectX(0)));
        const foc2 = await c.eval(bandAt(await rectX(2)));
        const scSw = await c.eval('return window.__ced.selfCheck();');
        const auSw = await c.eval('return window.__ced.auditFrame();');

        await c.eval('return window.__ced.setBracketScope(0);');   // → 回 All
        const back0 = await c.eval(bandAt(await rectX(0)));
        const back2 = await c.eval(bandAt(await rectX(2)));

        chk.check(' setBracketScope：All（两个都在）→ Focused（只有光标所在的 #0）→ All（又都在）',
                  all0 > 0 && all2 > 0 && foc0 > 0 && foc2 === 0 && back0 > 0 && back2 > 0,
                  `All #0=${all0}/#2=${all2} ｜ Focused #0=${foc0}/#2=${foc2} ｜ All #0=${back0}/#2=${back2}`);
        chk.check(' 切档之后：引擎帧 == 整帧重画、且屏幕 == 引擎帧（改配置确实重排了）',
                  auSw && auSw.diff === 0 && scSw.screenVsEngineDiffPx === 0,
                  `帧自审差 ${auSw ? auSw.diff : '?'} px，屏幕差 ${scSw.screenVsEngineDiffPx} px`);
      }

      // 换字段之后，引擎帧本身必须等于"整帧重画一遍"的结果。
      //
      //   这条与上面几条的分工：上面查的是"标记画在哪"（局部像素）；
      //     这一条查的是"有没有什么东西该消失却没消失"。
      //     "光标走了、旧字段的方括号还在"就是后者 ——
      //     它可能只残留一个标记、只在某个位置，局部判据很容易漏，
      //     而"和整帧重画逐字节比"是全覆盖的。
      //
      //   为什么需要它而不是只靠 selfCheck（画布 vs 引擎帧）：
      //     如果引擎帧自己就是错的（旧标记没擦掉），画布忠实反映了它，
      //     selfCheck 两边一起错、判据照样绿。实测就是这样漏掉的。
      const aud = await c.eval('return window.__ced.auditFrame();');
      chk.check(' 换字段后引擎帧 == 整帧重画（帧自审，逐字节）—— 覆盖"该消失却没消失"',
                aud && aud.diff === 0,
                aud ? `差 ${aud.diff} px` + (aud.box ? ` 盒=${JSON.stringify(aud.box)}` : '')
                    : '返回 null');
    }

    //  复现那个 bug：闪烁正停在"光标隐藏"那半拍时换字段
    //
    //  这是"有时"二字的来源 —— 相位是 50/50，而所有老断言开头都调过
    //  stopBlink()，正好把这一半概率整个屏蔽掉了（所以它一直没被抓住）。
    //
    //  机制（的两组数字把它钉死了）：
    //    引擎在本次 repaint 里整行重排（占位换字段）→ 脏矩形是整帧；
    //    但 editStats.caretVisible 是 false（相位在隐藏半拍）→ 宿主的
    //      afterEdit() 判定"刚才那次把光标画成隐的"，再 present 一次，
    //      并把上屏矩形换成那一轮的（只含光标与标记那几小块）；
    //    于是第一次那次全帧重绘从头到尾没上屏 ——
    //        auditFrame() 差 0（引擎帧是对的）
    //        selfCheck() 差一千多像素（画布停在旧排版上）。
    //  的就是「光标走了、文字没动、那个字段的方括号还留在原地」。
    //
    //  判据就用他自己跑的那两个：auditFrame 必须 0（引擎侧），
    //  selfCheck 也必须 0（上屏侧）。少任何一条这个 bug 都能溜过去。
    {
      // 这一节显式切到 Focused 档（旧行为）才复现得出来：
      //   触发条件是"换字段让整行重排 → 第一次 present 写整帧"，
      //   而默认的 All 档下换字段不重排（括号常驻、占位与光标无关）——
      //   触发条件消失了，这一节也就测不到东西了。
      //   All 档下"换字段很便宜"另有一条断言，见本节末尾。
      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.setBracketScope(1);');   // Focused
      await c.eval('return window.__ced.stopBlink();');
      await c.eval('return window.__ced.setCaret(0, 6);');   // 光标在 #0（反复心悸）

      // 把相位摆到"隐藏"，并且让引擎的账也进入隐藏态
      await c.eval('window.__ced.showCaret(false); window.__ced.present(); return true;');
      const hiddenPre = await c.eval('return window.__ced.selfCheck();');
      chk.check('前置：把闪烁相位摆到「光标隐藏」之后，屏幕仍等于引擎帧',
                hiddenPre.screenVsEngineDiffPx === 0,
                `差 ${hiddenPre.screenVsEngineDiffPx} px`);

      // 在隐藏半拍里换字段（＝鼠标点「加重一天」走的就是这条路）
      await c.eval('return window.__ced.setCaret(0, 14);');
      await c.eval('return window.__ced.stopBlink();');   // afterEdit 会重启定时器，再停掉
      const scHide = await c.eval('return window.__ced.selfCheck();');
      const auHide = await c.eval('return window.__ced.auditFrame();');

      chk.check(' 隐藏半拍换字段：引擎帧必须等于整帧重画（引擎侧无罪）',
                auHide && auHide.diff === 0,
                auHide ? `差 ${auHide.diff} px` + (auHide.box ? ` 盒=${JSON.stringify(auHide.box)}` : '')
                       : '返回 null');
      chk.check(' 隐藏半拍换字段：屏幕像素也必须等于引擎帧 —— 这就是用户那个 bug',
                scHide.screenVsEngineDiffPx === 0,
                scHide.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${scHide.screenVsEngineDiffPx} px，盒=${JSON.stringify(scHide.screenVsEngineDiffBox)}`
                    + `（引擎写了的区域没被贴上屏）`);
      chk.check(' 隐藏半拍换字段之后，光标必须已经可见（编辑后光标可见是引擎的不变量）',
                scHide.caret.visible === true, `visible=${scHide.caret.visible}`);

      // 对照：默认档（All）下同一个动作不再重排，上屏量远小于整帧
      //
      //  这条钉死"括号常驻"的收益：占位集合与光标无关 ⇒ 换字段不触发布局，
      //  上屏从"整帧"掉到"一块小矩形"。Focused 档下同样动作会写整帧（上面刚量过）。
      {
        await c.eval('return window.__ced.setBracketScope(0);');   // 回默认 All
        await c.eval('return window.__ced.loadFormSample(0);');
        await c.eval('return window.__ced.stopBlink();');
        await c.eval('return window.__ced.setCaret(0, 6);');
        await c.eval('window.__ced.present(); return true;');

        await c.eval('return window.__ced.setCaret(0, 14);');      // 换字段
        const cst = await c.eval('return window.__ced.editStats;');
        const fw = await c.eval('return window.__ced.frameW();');
        const fh = await c.eval('return window.__ced.frameH();');
        chk.check(' 默认档换字段：不重排（layoutRan=false）',
                  cst.layoutRan === false, `layoutRan=${cst.layoutRan}`);
        chk.check(' 默认档换字段：上屏量远小于整帧（括号常驻的收益）',
                  cst.repaintedW > 0 && cst.repaintedW < fw / 2 &&
                    cst.repaintedH < fh / 2,
                  `上屏 ${cst.repaintedW}×${cst.repaintedH} px（整帧 ${fw}×${fh}）`);
      }

      //  光标停在字段末尾时必须落在 ] 之内（在它左边）。
      //
      //光标停在字段最后一个字上，再按一次右方向键，
      //  光标直接跳到 ] 的右边 —— 中间"在 ] 之内"的位置被跳过了，
      //  看着像光标从字里蹿出了框外。
      //
      //  判据是像素级的：量"光标那一行里最靠右的标记墨迹"（＝ ] 的字形），
      //    要求光标的右边缘在它左边。不去拿控件矩形的几何数字比 ——
      //    这一整类 bug 的教训就是"几何元数据 ≠ 绘制结果"，两边必须分别量。
      {
        await c.eval('return window.__ced.loadFormSample(0);');
        await c.eval('return window.__ced.stopBlink();');
        // 表单样例：complaint[5,9) / duration[9,11) 只读 / aggravate[12,16)
        // aggravate 是最右一个可编辑字段 → 它行内最右的标记墨迹就是它的 ]。
        await c.eval('return window.__ced.setCaret(0, 16);');   // 「天」右边、`]` 左边
        await c.eval('window.__ced.present(); return true;');

        const cmp = await c.eval(`
          const st = window.__ced.editStats;
          const f = window.__ced.rawFrame();
          const fw = window.__ced.frameW();
          const near = ${MARK};   // 标记亮蓝；光标色 r=31 被排除
          let bracketX = -1;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < st.caretY || y >= st.caretY + st.caretH) continue;
            if (!near(f[i], f[i + 1], f[i + 2])) continue;
            if (x > bracketX) bracketX = x;
          }
          return { caretX: st.caretX, caretW: st.caretW, bracketX,
                   caretOff: st.caretOffset };
        `);
        chk.check(' 光标确实停在字段末尾（偏移 16）',
                  cmp.caretOff === 16, `offset=${cmp.caretOff}`);
        chk.check(' 字段末尾的光标落在 `]` 之内（光标右边缘在最右标记墨迹左边）',
                  cmp.bracketX > 0 && cmp.caretX + cmp.caretW <= cmp.bracketX + 1,
                  `光标 x∈[${cmp.caretX}, ${cmp.caretX + cmp.caretW}] ｜ ` +
                  `该行最右标记墨迹 x=${cmp.bracketX}` +
                  (cmp.caretX + cmp.caretW <= cmp.bracketX + 1
                    ? '' : '  ← 光标跑到 `]` 右边去了'));
      }

    //  方向键不跨控件：一次一格，走不过去就停住。
    //
    //"光标移动跨两个字符" —— 在 #2（aggravate）起点按一次左键，
    //  光标会跨过只读字段 #1 直接落到 #0 里（实测一次跨 3 个偏移）。
    //  根因是引擎把"走一步"做成了"走完再用 clampToAllowed 投影到最近的合法位置"，
    //  那个投影不认方向、只认距离。
    //
    //  判据是逐步的：单步偏移跨度恒为 1，且整趟走下来从不离开本字段。
    //    只看终点是抓不到的（终点"落在某个合法位置"照样成立）。
    {
      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.stopBlink();');
      await c.eval('return window.__ced.setCaret(0, 12);');   // #2 = aggravate [12,16)

      const start = await c.eval('return window.__ced.caret;');
      await c.eval(key('ArrowLeft'));                          //  真实键盘
      const afterLeft = await c.eval('return window.__ced.caret;');
      chk.check(' 在 #2 起点按左方向键：不许跨控件（光标留在 12）',
                afterLeft.offset === 12,
                `12 → ${afterLeft.offset}` + (afterLeft.offset === 12 ? '' : '  ← 跨到别的字段去了'));

      // 右走 5 步：12→13→14→15→16→16（到边界停住），单步跨度恒为 1
      const seq = [];
      for (let i = 0; i < 5; ++i) {
        await c.eval(key('ArrowRight'));
        seq.push((await c.eval('return window.__ced.caret;')).offset);
      }
      let worst = 0;
      let prev = start.offset;
      for (const o of seq) { worst = Math.max(worst, o - prev); prev = o; }
      chk.check(' 从 #2 起点右走 5 步：每一步 +1，到 16 就停住',
                seq.join(',') === '13,14,15,16,16',
                `12 → ${seq.join(' → ')}`);
      chk.check(' 单步跨度恒为 1（没有任何一步瞬移）',
                worst === 1, `最大单步跨度 ${worst}`);
    }

      // 同一件事用"敲字"再走一遍（这条路径由打字触发）
      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.stopBlink();');
      await c.eval('return window.__ced.setCaret(0, 6);');
      await c.eval('window.__ced.showCaret(false); window.__ced.present(); return true;');
      await c.eval(key('x'));                     //  真实键盘路径
      await c.eval('return window.__ced.stopBlink();');
      const scType = await c.eval('return window.__ced.selfCheck();');
      const auType = await c.eval('return window.__ced.auditFrame();');
      chk.check(' 隐藏半拍敲字：引擎帧自审必须 0',
                auType && auType.diff === 0,
                auType ? `差 ${auType.diff} px` : '返回 null');
      chk.check(' 隐藏半拍敲字：屏幕像素必须等于引擎帧',
                scType.screenVsEngineDiffPx === 0,
                scType.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${scType.screenVsEngineDiffPx} px，盒=${JSON.stringify(scType.screenVsEngineDiffBox)}`);
    }

    // 只读模式：这是"策略生效"最干净的可观测形式
    //   （表单模式下光标被 clamp 在可编辑字段内，反而没法用普通按键走到"区间外"）
    await c.eval('return window.__ced.setEditMode(2);');
    const txt0 = await c.eval('return window.__ced.text;');
    const blk0 = await c.eval('return window.__ced.blockedEdits;');
    for (const ch of 'abc') await c.eval(key(ch));
    const txt1 = await c.eval('return window.__ced.text;');
    const blk1 = await c.eval('return window.__ced.blockedEdits;');
    chk.check(' 只读模式：按键全被拒、文本一字不变、被拒次数 +3',
              txt1 === txt0 && blk1 === blk0 + 3,
              `被拒 ${blk1 - blk0} 次，文本${txt1 === txt0 ? '未变' : '变了'}`);

    // 表单模式：可编辑字段内能打进去，且区间跟着变长
    await c.eval('return window.__ced.loadFormSample(0);');
    await c.eval('return window.__ced.setCaret(0, 6);');
    const len0 = await c.eval('return Array.from(window.__ced.text).length;');
    await c.eval(key('测'));
    const len1 = await c.eval('return Array.from(window.__ced.text).length;');
    const ct2 = await c.eval('return window.__ced.controls;');
    chk.check(' 表单模式：字段内按键成功，且字段区间跟着变长（9 → 10）',
              len1 === len0 + 1 && ct2.items[0].end[1] === 10,
              `码点 ${len0} → ${len1}，字段 #0 end=${ct2.items[0].end[1]}`);

    // 切回文档模式：标记必须一个像素都不剩
    await c.eval('return window.__ced.setEditMode(0);');
    const docMark = await c.eval(inkMark);
    chk.check(' 切回文档模式：标记一个像素都不剩', docMark === 0, `${docMark} px`);

    // 切回表单模式后标记回来（证明不是"切走一次就再也不画"）
    await c.eval('return window.__ced.setEditMode(1);');
    const backMark = await c.eval(inkMark);
    chk.check(' 切回表单模式：标记又出现了', backMark > 0, `${backMark} px`);

    // 穷尽不变式（在这一侧尤其重要：native 用的是自写光栅器，
    //     浏览器是 Skia，两边的字形光栅/度量并不完全相同）。
    //
    //   判据（默认档 All）：光标走遍每一个可停位置，每一步都查每个可编辑
    //     字段的左缘外侧 —— 括号是常驻的，所以每一步都必须有墨迹，而且
    //     每个字段自己的墨迹量必须恒定（不随光标变）。
    //     只查"聚焦字段有、其余为空"是 Focused 档的旧判据，在这里会假红。
    //     为什么值得穷尽：是"有时括号渲染不更新"。单点断言抓不到
    //     "某个位置刚好漏掉"，而漏掉几乎总是"某个矩形和光标重叠、被后画的东西盖掉"
    //     造成的 —— 只有走遍位置才碰得到。
    //
    // 整个循环放在一次 eval 里：否则 18 次 setCaret × N 个字段 = 几十次跨界，
    // 每次还要拷一份 4.5 MB 的帧，慢到没法用。
    const exh = await c.eval(`
      const near = ${MARK};
      const total = Array.from(window.__ced.text).length;
      let checked = 0, bad = 0, firstBad = '';
      // 每字段的 min/max 存成对象（{ci: [lo, hi]}），不能存数组：
      //   只读字段会被跳过 → 稀疏数组在跨进程回传时被序列化成 null
      //   （「Cannot read properties of null (reading '0')」，而且栈里
      //    看不到是哪一行 —— 因为整段是 eval 的字符串）。
      const seen = {};
      for (let off = 0; off <= total; ++off) {
        window.__ced.setCaret(0, off);
        const cc = window.__ced.controls;
        const f = window.__ced.rawFrame();
        const fw = window.__ced.frameW();
        for (let ci = 0; ci < cc.items.length; ++ci) {
          const it = cc.items[ci];
          if (it.ro || !it.rects.length) continue;
          const cx = it.rects[0].x;
          const lo = Math.round((cx - 10) * 1.5), hi = Math.round((cx - 1) * 1.5);
          let n = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const x = (i >> 2) % fw;
            if (x < lo || x > hi) continue;
            if (near(f[i], f[i+1], f[i+2])) ++n;
          }
          ++checked;
          if (n === 0) { ++bad; if (!firstBad) firstBad = \`off=\${off} 字段#\${ci} 应有括号却为 0\`; }
          const cur = seen[ci] || [n, n];
          seen[ci] = [Math.min(cur[0], n), Math.max(cur[1], n)];
        }
      }
      return { checked, bad, total, firstBad, perField: seen };
    `);
    chk.check(' 穷尽不变式：光标走遍全部可停位置，每个字段的括号始终在（All 档常驻）',
              exh.checked > 20 && exh.bad === 0,
              `${exh.checked} 项检查（走 ${exh.total + 1} 个位置），不符 ${exh.bad} 项` +
                (exh.firstBad ? `；首个：${exh.firstBad}` : ''));
    const pf = Object.entries(exh.perField)
      .map(([k, v]) => ({ ci: Number(k), lo: v[0], hi: v[1] }));
    chk.check(' 穷尽不变式：每个字段括号的墨迹量与光标位置恒定（常驻的硬判据）',
              pf.length > 0 && pf.every((p) => p.lo === p.hi && p.lo > 0),
              pf.map((p) => `#${p.ci} ${p.lo}~${p.hi}`).join(' ｜ '));

    // 给用户用的取证接口也当场跑一遍 —— 它要是抛异常，用户那边就白跑了。
    const mk = await c.eval('return window.__ced.markCheck();');
    chk.check(' 用户自检接口可用且当场判为一致（__ced.markCheck()）',
              !!mk && mk.allOk === true && Array.isArray(mk.rows) && mk.rows.length >= 1,
              mk ? `${mk.rows.length} 个可编辑字段，全部一致=${mk.allOk}` : '返回 null');

    // Tab = 缩进（不是跳字段）
    //
    //   语义纠偏：Tab 在编辑器里是缩进。绑成"跳到下一个控件"是表单/网页里
    //     字段之间的约定，放到正文编辑器里会把肌肉记忆打断（用户明确纠正过）。
    //     字段之间用鼠标点、或者方向键（光标本来就会跨字段）。
    //   改完之后"跳字段"只剩程序化入口 __ced.focusControl（探针/自动化用），
    //     下面单独验它还在。
    //
    //   这一段的核心判据是"真实按键 → 屏幕像素 == 引擎帧"。它抓的是这一类：
    //     某个入口改了引擎状态却没给自己收尾，宿主于是拿着上一次呈现的
    //     脏矩形去上屏，画布变成"旧内容 + 局部新内容"的混合体
    //     （的"光标走了、旧括号还在"）。
    {
      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.setCaret(0, 6);');   // k1 内
      await c.eval('window.__ced.stopBlink(); window.__ced.present(); return true;');

      const beforeTab = await c.eval('return window.__ced.selfCheck();');
      chk.check('前置：按 Tab 之前屏幕与引擎帧一致（否则下面的判据没意义）',
                beforeTab.screenVsEngineDiffPx === 0, `差 ${beforeTab.screenVsEngineDiffPx} px`);

      const nBefore = await c.eval('return Array.from(window.__ced.text).length;');
      await c.eval(key('Tab'));                              //  真实键盘路径
      const nAfter = await c.eval('return Array.from(window.__ced.text).length;');
      const postTab = await c.eval('return window.__ced.selfCheck();');
      const liveOff = await c.eval('return window.__ced.caretLive;');
      const auditTab = await c.eval('return window.__ced.auditFrame();');

      chk.check(' 真实 Tab 键 = 缩进：文档 +4 个码点，且是 4 个空格',
                nAfter === nBefore + 4 &&
                  (await c.eval('return window.__ced.text.slice(6, 10) === "    ";')),
                `码点 ${nBefore} → ${nAfter}`);
      chk.check(' Tab 之后光标停在新插入的空格之后（offset=10）',
                postTab.caret.offset === 10, `offset=${postTab.caret.offset}`);
      chk.check(' Tab 之后"编辑统计里的光标"必须等于"引擎实时光标"'
                + ' —— 即这个入口自己把画面收尾了，宿主不会拿到过期脏矩形',
                postTab.caret.offset === liveOff,
                `统计 ${postTab.caret.offset} ／ 实时 ${liveOff}` +
                (postTab.caret.offset === liveOff ? '' : '  ← 有入口改了状态却没重绘'));
      chk.check(' Tab 之后屏幕像素必须等于引擎帧（不许出现"引擎对了、屏幕还是旧的"）',
                postTab.screenVsEngineDiffPx === 0,
                postTab.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${postTab.screenVsEngineDiffPx} px，盒=${JSON.stringify(postTab.screenVsEngineDiffBox)}`);
      chk.check(' Tab 之后引擎帧必须等于"整帧重画一遍"的结果（帧自审，逐字节）',
                auditTab && auditTab.diff === 0,
                auditTab ? `差 ${auditTab.diff} px` + (auditTab.box ? ` 盒=${JSON.stringify(auditTab.box)}` : '')
                         : '返回 null');

      // Shift+Tab 反缩进
      await c.eval(key('Tab', ', shiftKey: true'));
      const nBack = await c.eval('return Array.from(window.__ced.text).length;');
      chk.check(' Shift+Tab = 反缩进：把 4 个空格删回来',
                nBack === nBefore, `码点 ${nAfter} → ${nBack}`);
    }

    // 程序化跳字段（探针/自动化入口，不再绑键盘）
    {
      await c.eval('return window.__ced.loadFormSample(0);');
      await c.eval('return window.__ced.setCaret(0, 5);');
      const f1 = await c.eval('return window.__ced.focusControl(true);');
      const c1 = await c.eval('return window.__ced.caret;');
      const f2 = await c.eval('return window.__ced.focusControl(true);');
      const c2 = await c.eval('return window.__ced.caret;');
      chk.check(' __ced.focusControl（程序化跳字段）：12 → 5 绕回第一个字段',
                !!f1 && !!f2 && c1.offset === 12 && c2.offset === 5,
                `${c1.offset} → ${c2.offset}`);
      const post = await c.eval('return window.__ced.selfCheck();');
      const audit = await c.eval('return window.__ced.auditFrame();');
      chk.check(' 跳字段之后：屏幕==引擎帧、且引擎帧==整帧重画',
                post.screenVsEngineDiffPx === 0 && audit && audit.diff === 0,
                `屏幕差 ${post.screenVsEngineDiffPx} px，帧自审差 ${audit ? audit.diff : '?'} px`);
      const mk2 = await c.eval('return window.__ced.markCheck();');
      chk.check(' 跳字段后每个字段的括号都还在（All 档常驻；markCheck 自检）',
                !!mk2 && mk2.allOk === true && mk2.bracketScope === 0,
                mk2 ? `${mk2.rows.length} 个可编辑字段，scope=${mk2.bracketScope}，`
                      + `全部一致=${mk2.allOk}` : '返回 null');
    }
  }

  chk.section('[9] 长文档（200 段）：增量排版只重排脏段之后的后缀');
  {
    // 单段文档没有可复用的前缀，增量与全量必然相等（见 [1] / [2]）。
    //   多段文档才谈得上"只重排脏段"—— 这一节用 200 段的病历文档量它。
    const base = await c.eval('return window.__ced.run(7, 1.0);');
    chk.check('长文档载入：200 段', base.totalParagraphs === 200,
              `${base.totalParagraphs} 段 / ${base.totalLines} 行`);
    chk.check('首次排版是全量（整篇都脏）', base.layoutWasFull === true);
    report.longDoc = { paragraphs: base.totalParagraphs, lines: base.totalLines,
                       fullMs: base.layoutMs };

    // 在第 150 段中间敲一个字（走真实键盘路径）
    await c.eval('return window.__ced.setCaret(150, 5);');
    await c.eval(key('测'));
    const st = await c.eval('return window.__ced.editStats;');
    chk.check('★ 敲字后是增量排版（不是全量）', st.layoutWasFull === false,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段 / 共 ${st.totalParagraphs} 段`);
    // 行结构没变 → 快路：只重排脏段自己，后缀整块搬回（一个字节都不动）。
    chk.check('★ 只重排第 150 段，其余 49 段原样复用',
              st.relaidParagraphs === 1 && st.reusedParagraphs === 49,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段`);
    chk.check('★ 重排行数真小于总行数', st.relaidLines < st.totalLines,
              `${st.relaidLines} / ${st.totalLines} 行`);
    // 分段账：前缀 [0,150) 一个字都没动、只重排第 150 段、其余 49 段整块搬回。
    // 这条比"重排段数对不对"更强：它把分界点也钉住了（前缀/脏段/复用各占哪一段）。
    chk.check('★ 分段账：前缀 150 段未动 + 脏段 1 段重排 + 后缀 49 段复用 = 200 段',
              st.relaidParagraphs + st.reusedParagraphs === st.totalParagraphs - 150,
              `1 + ${st.reusedParagraphs} = ${st.totalParagraphs} - 150`);
    chk.check('★ 增量耗时小于全量耗时（同一次测量里的两个数）',
              st.layoutMs < report.longDoc.fullMs,
              `增量 ${st.layoutMs.toFixed(3)} ms vs 首次全量 ${report.longDoc.fullMs.toFixed(3)} ms`);

    // 排版自审：增量结果与"不用缓存的全量重排"逐字段比 —— 必须完全相同
    const au = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 增量结果与全量重排逐字段一致（auditLayout diff 0）',
              !!au && au.diff === 0,
              au ? `diff ${au.diff}（字形 ${au.glyphDiff}、行 ${au.lineDiff}、段 ${au.paraDiff}、` +
                `簇表 ${au.startsDiff}、统计 ${au.statsSame ? '同' : '不同'}）` : '返回 null');

    // 相对耗时：分子分母都来自同一次 auditLayout 调用（curMs 是刚才那次增量续排，
    //   fullMs 是本次审计顺便做的全量重排），不掺历史数字。
    if (au) {
      report.longDoc.incrMs = au.curMs;
      report.longDoc.auditFullMs = au.fullMs;
      // 浏览器的 steady_clock 分辨率是 0.1~0.2 ms（emscripten_get_now），
      //   全量太小时比值会失真（0.2 / 0.4 恰好卡在边界）。所以：全量 ≥ 1 ms 时
      //   才判"不到 20%"，否则退成"增量更快"这条不依赖分辨率的判据 —— 宁可少判，
      //   也要避免"机器越快越假红"。
      const ratioOk = au.fullMs >= 1.0 ? au.curMs * 5 < au.fullMs : au.curMs < au.fullMs;
      chk.check('★ 靠后敲字：增量耗时远小于全量（全量 ≥ 1ms 时判 <20%）', ratioOk,
                `增量 ${au.curMs.toFixed(3)} ms ｜ 全量 ${au.fullMs.toFixed(3)} ms ｜ ` +
                `${(100 * au.curMs / Math.max(au.fullMs, 1e-9)).toFixed(1)}%` +
                (au.fullMs < 1.0 ? '（全量太小，只判"更快"）' : ''));
    }

    // 上屏与帧自审：长文档里编辑之后，屏幕 == 引擎帧，且引擎帧 == 整帧重画
    const sc = await c.eval('return window.__ced.selfCheck();');
    chk.check('长文档编辑后：屏幕 == 引擎帧', sc.screenVsEngineDiffPx === 0,
              `差 ${sc.screenVsEngineDiffPx} px`);
    const af = await c.eval('return window.__ced.auditFrame();');
    chk.check('长文档编辑后：引擎帧 == 整帧重画', af && af.diff === 0,
              af ? `差 ${af.diff} px` : 'null');

    // 页数跟着长：200 段不可能只有一页 —— 否则上面那些"重排行数"没有意义
    chk.check('长文档确实排成了多页', st.totalPages > 1, `${st.totalPages} 页`);

    // 后缀平移复用：撑出一行之后，后缀的断行结果照样复用（只挪坐标）。
    //
    // 40 个汉字足以撑出至少一行。走 __ced.type（直连引擎）而不是键盘逐字派发：
    //   键盘路径上面已经验过，这里要的是"一次插入 40 个字"把行结构撑变。
    const widened = await c.eval(
      `return window.__ced.type(${JSON.stringify('测'.repeat(40))});`);
    chk.check('★ 撑出一行 → 后缀整体平移复用（重排 1 段）',
              !!widened && widened.suffixShifted === true && widened.relaidParagraphs === 1,
              widened ? `重排 ${widened.relaidParagraphs} 段 / 复用 ${widened.reusedParagraphs} 段 / ` +
                `平移=${widened.suffixShifted} / 脏页 ${widened.dirtyPageCount}` : 'null');
    chk.check('★ 平移之后后缀段数照旧全部复用',
              !!widened && widened.reusedParagraphs + widened.relaidParagraphs ===
                widened.totalParagraphs - 150,
              widened ? `${widened.relaidParagraphs} + ${widened.reusedParagraphs} = ` +
                `${widened.totalParagraphs} - 150` : 'null');
    const auShift = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 平移复用的排版与全量逐字段一致（auditLayout diff 0）',
              !!auShift && auShift.diff === 0,
              auShift ? `diff ${auShift.diff}（字形 ${auShift.glyphDiff}、行 ${auShift.lineDiff}、` +
                `段 ${auShift.paraDiff}）` : 'null');
    const scShift = await c.eval('return window.__ced.selfCheck();');
    chk.check('平移之后屏幕 == 引擎帧', scShift.screenVsEngineDiffPx === 0,
              `差 ${scShift.screenVsEngineDiffPx} px`);

    // 页指纹：只重算脏页，不是整篇重扫字形。
    //
    // 整篇的页指纹耗时在首次全量里（base.pageFpMs），增量编辑之后应当小一个量级。
    // 两个数都来自同一次会话的同一台机器，所以比值可比。
    const fpFull = base.pageFpMs;
    const fpIncr = st.pageFpMs;
    report.longDoc.pageFpFullMs = fpFull;
    report.longDoc.pageFpIncrMs = fpIncr;
    chk.check('★ 页指纹增量化：增量编辑之后只重算脏页（< 整篇的 1/4）',
              fpFull > 0.05 ? fpIncr * 4 < fpFull : fpIncr <= fpFull,
              `整篇 ${fpFull.toFixed(3)} ms ｜ 增量 ${fpIncr.toFixed(3)} ms` +
                (fpFull <= 0.05 ? '（整篇太小，只判"不更慢"）' : ''));
  }

  chk.section('[10] 跨段编辑：真实回车分段 / 段首退格合并');
  {
    // 用真实的 Enter 按键走一遍（不是直接调引擎）—— 键盘路径与引擎路径是两处接线，
    //   只测引擎会漏掉"宿主没把 Enter 接上"这类问题。
    await c.eval('return window.__ced.run(6, 1.0);');
    await c.eval('return window.__ced.stopBlink();');
    const before = await c.eval('return window.__ced.editStats;');
    const textBefore = await c.eval('return window.__ced.allText;');
    await c.eval('return window.__ced.setCaret(0, 5);');
    await c.eval(key('Enter'));

    const after = await c.eval('return window.__ced.editStats;');
    const textAfter = await c.eval('return window.__ced.allText;');
    report.splits = { parasBefore: before.totalParagraphs, parasAfter: after.totalParagraphs };
    chk.check('★ 真实回车让段数 +1', after.totalParagraphs === before.totalParagraphs + 1,
              `${before.totalParagraphs} → ${after.totalParagraphs} 段`);
    chk.check('★ 分段的字符账守恒（不丢不重）',
              textAfter.replace(/\n/g, '').length === textBefore.length,
              `前 ${textBefore.length} 字符 / 后 ${textAfter.replace(/\n/g, '').length} 字符（去换行）`);
    chk.check('★ 分段之后光标在新段段首', after.caretPara === 1 && after.caretOffset === 0,
              `段 ${after.caretPara} 偏移 ${after.caretOffset}`);
    chk.check('段数变了不让后缀复用（后缀段号已经错位）', after.reusedParagraphs === 0,
              `重排 ${after.relaidParagraphs} 段 / 复用 ${after.reusedParagraphs} 段`);

    // 排版与上屏两个自审都要过：分段走的是"前缀复用 + 从脏段续排"，最容易错的是段号错位
    const auSplit = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 分段之后增量排版 == 全量重排（逐字段）',
              !!auSplit && auSplit.diff === 0,
              auSplit ? `diff ${auSplit.diff}（字形 ${auSplit.glyphDiff}、行 ${auSplit.lineDiff}、` +
                `段 ${auSplit.paraDiff}）` : 'null');
    const scSplit = await c.eval('return window.__ced.selfCheck();');
    chk.check('分段之后屏幕 == 引擎帧', scSplit.screenVsEngineDiffPx === 0,
              `差 ${scSplit.screenVsEngineDiffPx} px`);
    const afSplit = await c.eval('return window.__ced.auditFrame();');
    chk.check('分段之后引擎帧 == 整帧重画', afSplit && afSplit.diff === 0,
              afSplit ? `差 ${afSplit.diff} px` : 'null');

    // 段首退格并回去（同一个往返：文档要么回到原样，要么至少段数 -1）
    await c.eval('return window.__ced.setCaret(1, 0);');
    await c.eval(key('Backspace'));
    const merged = await c.eval('return window.__ced.editStats;');
    chk.check('★ 段首退格把两段并回去（段数 -1）',
              merged.totalParagraphs === after.totalParagraphs - 1,
              `${after.totalParagraphs} → ${merged.totalParagraphs} 段`);
    chk.check('★ 合并之后光标落在合并点上', merged.caretPara === 0 && merged.caretOffset === 5,
              `段 ${merged.caretPara} 偏移 ${merged.caretOffset}`);
    const auMerge = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 合并之后增量排版 == 全量重排（逐字段）', !!auMerge && auMerge.diff === 0,
              auMerge ? `diff ${auMerge.diff}` : 'null');
    chk.check('合并之后文档回到分段之前的字符数',
              (await c.eval('return window.__ced.allText;')).length === textBefore.length,
              `回到 ${textBefore.length} 字符`);
  }

  chk.section('[11] 相邻字段共享边界上打字：字进前一个字段，括号仍是 `][`');
  {
    //  报过的现象（截图：患者主诉：[反悸[▏]三天]，[加重一天]。）：两个相邻控件，
    //  在第一个控件末尾打一个字，共享边界上的括号顺序从 ][ 变成 []。
    //
    //  根因不在排版，在区间伸缩：插入点同时是 A 的末尾与 B 的开头，两条端点规则都命中
    //  —— A 的终点 >= at 右移（字进 A），B 的起点要 > at 才右移（于是原地不动）——
    //  结果一个字同时落进两个控件：A=[5,10)、B=[9,12) 重叠，B 的左括号于是被画到
    //  A 右括号的左边。native 侧的结构性判据见单测 [18]，这里补的是"真实按键路径"。
    await c.eval('return window.__ced.loadFormSample(0);');
    await c.eval('return window.__ced.stopBlink();');
    const b11 = await c.eval('return window.__ced.controls;');
    report.boundary = { before: b11.items.map((it) => [...it.start, ...it.end]) };

    chk.check('前置：字段 #0 的末尾与 #1 的开头是同一个位置（共享边界）',
              b11.count >= 2 && b11.items[0].end[0] === b11.items[1].start[0] &&
                b11.items[0].end[1] === b11.items[1].start[1],
              `#0 [${b11.items[0].start[1]},${b11.items[0].end[1]}) ｜` +
                ` #1 [${b11.items[1].start[1]},${b11.items[1].end[1]})`);

    // 光标停在这个边界上，然后派一个真实按键（不是 __ced.type，见 [4] 的说明）
    await c.eval(`return window.__ced.setCaret(0, ${b11.items[0].end[1]});`);
    const cc11 = await c.eval('return window.__ced.controls;');
    chk.check('光标停在边界上时归前一个字段（末尾算在区间内）', cc11.caret === 0,
              `caret-control=${cc11.caret}`);
    await c.eval(key('诊'));

    const a11 = await c.eval('return window.__ced.controls;');
    report.boundary.after = a11.items.map((it) => [...it.start, ...it.end]);
    const e0 = a11.items[0].end[1], s1 = a11.items[1].start[1];
    chk.check('★ 字进了 #0（#0 变长），#1 整体右移 —— 两段区间不重叠',
              e0 === b11.items[0].end[1] + 1 && s1 === b11.items[1].start[1] + 1,
              `#0 末尾 ${b11.items[0].end[1]} → ${e0} ｜ ` +
                `#1 开头 ${b11.items[1].start[1]} → ${s1}`);
    chk.check('★ 重叠就是那个 `[]`：一个字同时属于两个字段',
              e0 <= s1, `#0 末尾 ${e0} vs #1 开头 ${s1}`);

    // 几何证据：控件的矩形是从行盒几何投影出来的，重叠的区间投影出重叠的矩形。
    //   #0 的右缘越过 #1 的左缘 = 两个字段的框在屏幕上叠在一起。
    const r0b = a11.items[0].rects[0], r1b = a11.items[1].rects[0];
    const right0 = r0b ? r0b.x + r0b.w : -1;
    const left1 = r1b ? r1b.x : -1;
    report.boundary.rects = { right0, left1 };
    chk.check('★ #0 的内容矩形不越过 #1 的左缘（两个字段的框不叠在一起）',
              right0 > 0 && left1 > 0 && right0 <= left1 + 0.01,
              `#0 右缘 ${right0.toFixed(2)} pt ≤ #1 左缘 ${left1.toFixed(2)} pt`);

    const au11 = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 边界上打字之后增量排版 == 全量重排（逐字段）',
              !!au11 && au11.diff === 0,
              au11 ? `diff ${au11.diff}（字形 ${au11.glyphDiff}、行 ${au11.lineDiff}、` +
                `段 ${au11.paraDiff}）` : 'null');
    const sc11 = await c.eval('return window.__ced.selfCheck();');
    chk.check('边界上打字之后屏幕 == 引擎帧', sc11.screenVsEngineDiffPx === 0,
              `差 ${sc11.screenVsEngineDiffPx} px`);

    await c.eval('return window.__ced.setCaret(0, 10);');
    await c.eval('window.__ced.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'shared-boundary.png'));

    // 三连可编辑字段（演示样例 variant 2）：三条边界两侧都可写 —— 前两个样例的边界
    //   一侧是只读的，只能在一个方向试。这里在边界上打字、再把中间那个删空，
    //   看区间和矩形会不会互相穿过。
    await c.eval('return window.__ced.loadFormSample(2);');
    await c.eval('return window.__ced.stopBlink();');
    const v3 = await c.eval('return window.__ced.controls;');
    report.chain3 = { before: v3.items.map((it) => [...it.start, ...it.end]) };
    chk.check('三连样例：3 个字段都紧挨着（两条共享边界）',
              v3.count === 3 && v3.items.every((it) => !it.ro) &&
                v3.items[0].end[1] === v3.items[1].start[1] &&
                v3.items[1].end[1] === v3.items[2].start[1],
              v3.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    await c.eval('return window.__ced.setCaret(0, 7);');  // 第一条共享边界
    await c.eval(key('痛'));
    const v3b = await c.eval('return window.__ced.controls;');
    report.chain3.afterType = v3b.items.map((it) => [...it.start, ...it.end]);
    // 字进前一个字段、后两个整体右移；三段首尾相接就是"不穿过"的最强形式
    const chain = (r) =>
      r.items.every((it, i) => i + 1 >= r.items.length || it.end[1] === r.items[i + 1].start[1]);
    chk.check('★ 三连样例：在第一条边界上打字之后，三段区间仍然首尾相接（不重叠、不穿线）',
              chain(v3b) && v3b.items[0].end[1] === 8 && v3b.items[1].start[1] === 8 &&
                v3b.items[1].end[1] === 10 && v3b.items[2].start[1] === 10,
              v3b.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    // 把中间那个删空：它变成零宽的空字段，右边界上与"左字段的闭 + 右字段的开"并列
    await c.eval('return window.__ced.setCaret(0, 10);');
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    const v3c = await c.eval('return window.__ced.controls;');
    report.chain3.afterEmpty = v3c.items.map((it) => [...it.start, ...it.end]);
    const mid = v3c.items[1];
    const right = v3c.items[2];
    chk.check('★ 中间字段被删成零宽的空字段，右边那个左移并保持紧挨着',
              mid.start[1] === mid.end[1] && right.start[1] === mid.end[1],
              v3c.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));
    const au3 = await c.eval('return window.__ced.auditLayout();');
    chk.check('★★ 三连样例的每一步增量排版都等于全量重排', !!au3 && au3.diff === 0,
              au3 ? `diff ${au3.diff}` : 'null');
    const sc3 = await c.eval('return window.__ced.selfCheck();');
    chk.check('三连样例：屏幕 == 引擎帧', sc3.screenVsEngineDiffPx === 0,
              `差 ${sc3.screenVsEngineDiffPx} px`);
    await c.eval('return window.__ced.setCaret(0, 7);');
    await c.eval('window.__ced.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'three-adjacent-fields.png'));
  }

  chk.section('[7] 无页面异常 / 无 console 错误');
  {
    report.pageErrors = await c.eval('return window.__ced.errors;');
    report.consoleErrors = cdp.conn.consoleErrors;
    chk.check('无 page error', report.pageErrors.length === 0, JSON.stringify(report.pageErrors));
    chk.check('无 console error', report.consoleErrors.length === 0,
              JSON.stringify(report.consoleErrors));
  }

  // 收尾：载入表单样例 + 恢复闪烁，截一张给人看
  //   刻意停在表单模式（而不是纯文档场景）：截图要能一眼看出
  //     "字段的边界长什么样、只读的那个是灰的"。切回文档模式它们就消失了。
  await c.eval('window.__ced.loadFormSample(0); window.__ced.startBlink(); return true;');
  await sleep(400);
  await c.screenshot(join(OUT, 'edit-scene.png'));

  report.checks = chk.rows;
  report.failures = chk.failures;
  report.total = chk.total;
  report.finalStats = await c.eval('return window.__ced.editStats;');
  writeFileSync(join(OUT, 'edit-probe.json'), JSON.stringify(report, null, 2), 'utf8');

  console.log('\n──────────────────────────────');
  console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  console.log(`截图： ${join(OUT, 'edit-scene.png')}`);
  console.log(`明细： ${join(OUT, 'edit-probe.json')}`);

  await c.send('Browser.close').catch(() => {});
  process.exit(chk.failures ? 1 : 0);
}

main().catch((e) => {
  // 带上栈：探针里 90% 的代码是拼好的字符串（eval 到页面里跑），
  //   只打印 message 的话，定位只能靠猜"是哪个 eval 块"。
  console.error('探针失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
