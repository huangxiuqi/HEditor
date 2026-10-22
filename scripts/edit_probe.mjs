// 档 1「可输入」的可证伪验收探针。
//
// 与 web_probe.mjs 的分工：
//   web_probe.mjs  —— 看渲染（各场景的页数/字形数/缺字/耗时）
//   edit_probe.mjs —— 看编辑（这一份），断言四件事：
//
//   键盘路径真的通了：往真实 <textarea> 上派 KeyboardEvent，文档要变、光标要走。
//      （不是调 window.__heditor.type() 绕开宿主 —— 那样测的是引擎，不是接线。）
//   档 1 的增量是真的：方向键只重画几百像素，而敲字仍是整页重画。
//      两条都要断言 —— 只断言"很快"是没法证伪的，必须同时钉住"还没做到的那部分"。
//   偏移单位是码点、且会吸附到字素簇：e + U+0301 是一个字符，
//      中间那个偏移不是合法插入点；一次退格删 2 个码点。
//   IME 组合期间一次都不碰引擎，只有 commit 才落文档。
//
// 用法（先起服务 + 带 --remote-debugging-port 的 Chrome）：
//   node scripts/edit_probe.mjs --port 9222 --url http://localhost:8781/web/index.html
import { readFileSync, writeFileSync } from 'node:fs';
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
// --lib：跑在新库的 demo 页上。页面启动后由迁移垫片把旧门面名翻成公开面 + diag，
//   断言一条不改 —— 这样红/绿只反映"新库能不能替代旧宿主"，不掺杂改写本身的手误。
const LIB = argv.includes('--lib') || /demo\.html/.test(URL_);

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
  if (LIB) {
    // 注入必须早于 navigate：垫片自己等 __demoReady，再挂上 window.__heditor。
    const src = readFileSync(join(HERE, 'lib_adapter.js'), 'utf8');
    await conn.send('Page.addScriptToEvaluateOnNewDocument', { source: src });
  }
  await conn.send('Page.navigate', { url: URL_ });
  const ms = await conn.waitFor('return !!(window.__heditor && window.__heditor.ready)',
                               'window.__heditor.ready');
  console.log(`→ 页面就绪，耗时 ${(ms / 1000).toFixed(1)} s`);
  cdp.conn = conn;

  // 把 eval 包成"表达式里可用 await + 跑完等宿主排空"。两件事缺一不可：
  //
  //   一是表达式统一包 async IIFE。Worker 宿主的门面是异步的（引擎不在主线程上），
  //      表达式里必须能写 await。语法上它两边都合法，单线程宿主照跑。
  //   二是跑完等宿主排空。单线程宿主全同步，它没有 quiesce，跳过即可；Worker 宿主必须
  //      等那一拍的消息处理完 —— 否则读到的还是上一拍的状态，而且是静默的错：
  //      镜像还没更新，断言会拿旧值判定，看起来只是"某条莫名红了"。
  //
  // await 对普通值是恒等操作，所以这套改写对单线程宿主是行为保持的 ——
  //   这也正是验收方式：改完先跑单线程，仍要 279 项全绿。
  const RAW = conn.eval.bind(conn);
  const asBody = (b) => 'return (async () => {\n' + b + '\n})();';
  // 表达式先过一遍语法检查：这 279 项里绝大多数是模板拼出来的长表达式，
  //   页面上只会回一句 "SyntaxError: await is only valid in async functions"，
  //   完全看不出是哪一段。在这里编译一次，就能把表达式原文一起带出来。
  const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
  conn.eval = async (expr) => {
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
    const s = await window.__heditor.reset(6, ${ZOOM});
    await window.__heditor.stopBlink();
    return s;`);
}

// 往真实 textarea 派键盘事件（走的是页面的 onKeyDown，不是引擎 API）
const key = (k, extra = '') => `
  window.__heditor.textarea.dispatchEvent(
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
    // 参照是窗口内那几页，不是整个帧：帧按页号分槽之后，帧高 = 槽数 x 步距，
    //   而槽数可以多于窗口页数（多出来的是空槽）—— 拿 framePixels 当分母，判据会
    //   随槽数一起松掉。
    const pageH0 = await c.eval('return window.__heditor.pagePx[1];');
    const winPx = st.windowPages * st.frameW * pageH0;
    chk.check('整页重画写入 ≥ 半窗（窗口内几页，不是整个帧）',
              st.repaintedPixels > winPx / 2,
              `${st.repaintedPixels} / ${winPx}`);
    chk.check('初始文档与预期一致', (await c.eval(CP_LEN + 'return cplen(window.__heditor.text);'))
              === Array.from(S6).length);
  }
  const full = report.baseline.repaintedPixels;

  chk.section('[2] 键盘路径：派真实 keydown，文档要变、光标要走（单段文档仍整页重画）');
  {
    const b = await c.eval(CP_LEN + 'return { n: cplen(window.__heditor.text), o: window.__heditor.caret.offset };');
    await c.eval(key('心'));
    const a = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text), o: window.__heditor.caret.offset,
                                              st: window.__heditor.editStats };`);
    chk.check('文档码点数 +1', a.n === b.n + 1, `${b.n} → ${a.n}`);
    chk.check('光标停在新字之后（不是字前、也不是跳段首）', a.o === b.o + 1, `${b.o} → ${a.o}`);
    chk.check('插入的就是那个字', (await c.eval('return window.__heditor.text.includes("心");')) === true);
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
    //   例外只有 Ctrl+Z / Ctrl+Y 这一对（撤销键位，见 [16] (g)）—— 这里用 'x'
    //     探的就是"修饰键本身不构成吞掉的理由"，不是"任何一个组合都不吞"。
    //   只断言"文档没变"是测不出后者的 —— 合成事件本来就没有默认行为。
    //
    // 顺序也要小心：下面"该吞的吞"那一条会真的敲进一个 z。
    //   探针自己制造的状态变化必须显式收掉，否则后面每一条断言都被带偏
    //   （否则多出来的 z 会让"退格恢复原样"跟着失败）。
    const beforeMod = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');

    let modSwallowed = 0;
    for (const m of ['ctrlKey', 'altKey', 'metaKey']) {
      const r = await c.eval(`
        const e = new KeyboardEvent('keydown',
          { key: 'x', ${m}: true, bubbles: true, cancelable: true });
        return window.__heditor.textarea.dispatchEvent(e);`);
      if (r === false) ++modSwallowed;
    }
    chk.check('Ctrl/Alt/Meta 组合不被吞（否则 Ctrl+C 之类会被吃掉）', modSwallowed === 0,
              `被吞 ${modSwallowed} / 3`);
    const afterMod = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    chk.check('带 Ctrl/Alt/Meta 的按键不落字', afterMod === beforeMod,
              `${beforeMod} → ${afterMod}`);

    const swallow = await c.eval(`
      const e = new KeyboardEvent('keydown', { key: 'z', bubbles: true, cancelable: true });
      return window.__heditor.textarea.dispatchEvent(e);   // false = 被 preventDefault
    `);
    chk.check('普通可打印字符的 keydown 被宿主吞掉（不落进 textarea）', swallow === false);
    const afterSwallow = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    chk.check(' 但字确实进了文档一次（"吞掉 keydown" 和 "不落字" 是两回事）',
              afterSwallow === beforeMod + 1, `${beforeMod} → ${afterSwallow}`);

    // 收尾：退格两次，把 z 和前面那个 心 都收掉
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    const back = await c.eval(CP_LEN + 'return { n: cplen(window.__heditor.text), o: window.__heditor.caret.offset };');
    chk.check('两次退格把文档恢复原样', back.n === b.n && back.o === b.o,
              `${back.n} / offset ${back.o}（期望 ${b.n} / ${b.o}）`);
  }

  chk.section('[3] 方向键 = 真增量：不重排、0 脏页、写入量差两个数量级');
  {
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const beforeMove = await c.eval('return window.__heditor.editStats;');
    await c.eval(key('ArrowRight'));
    const right = await c.eval('return { c: window.__heditor.caret, st: window.__heditor.editStats };');
    chk.check('右移后光标偏移 +1', right.c.offset === 1, `offset=${right.c.offset}`);
    chk.check(' 光标移动不重排（layoutRan=false）', right.st.layoutRan === false);
    chk.check(' 光标移动不抹掉排版账（仍是上一轮的重排行数）',
              right.st.relaidLines === beforeMove.relaidLines,
              `${right.st.relaidLines} / 上一轮 ${beforeMove.relaidLines}`);
    chk.check(' 光标移动不抹掉布局耗时', right.st.layoutMs === beforeMove.layoutMs);
    chk.check(' 光标移动 0 个脏页（底图一个字都没变）', right.st.dirtyPageCount === 0);
    chk.check(' 光标移动写入量 < 全量的 1/100',
              right.st.repaintedPixels * 100 < full,
              `${right.st.repaintedPixels} vs ${full}（${(100 * right.st.repaintedPixels / full).toFixed(4)}%）`);

    // 上屏面积：引擎在 present 里算出的脏矩形，必须真的落到宿主的 putImageData 上。
    //   这一条挡的是"引擎给了矩形、宿主还是整页贴"——那种情况下像素判据全都照样绿，
    //   只有这个数看得出来。整页约 1.1 Mpx，一次光标移动是几十像素。
    const blit3 = await c.eval('return window.__heditor.blitStats;');
    chk.check('★ 光标移动的上屏面积是几十像素，不是整页（宿主按脏矩形贴）',
              typeof blit3.px === 'number' && blit3.px > 0 && blit3.px < 2000,
              `px=${blit3.px}`);

    // 段首再左移：引擎应当返回 false（没动就不该重绘）
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const at0 = await c.eval('return await window.__heditor.move(-1);');
    chk.check('已在段首，向左移动不重绘（返回 null）', at0 === null);

    // Home / End
    await c.eval(key('End'));
    const atEnd = await c.eval('return window.__heditor.caret.offset;');
    const len = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    chk.check('End 走到段末', atEnd === len, `${atEnd} / 段长 ${len}`);
    await c.eval(key('Home'));
    chk.check('Home 回到段首', (await c.eval('return window.__heditor.caret.offset;')) === 0);
  }

  chk.section('[4] 画布点选 = 命中测试往返：正查找出的光标 x，反查必须回到同一个偏移');
  {
    const len = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g0 = await c.eval('return window.__heditor.caret;');
    const z = await c.eval('return window.__heditor.zoomLevel;');

    // 画出来的光标矩形（的东西）应当逐个右移
    const xs = [];
    for (let o = 0; o <= len; ++o) {
      await c.eval(`await window.__heditor.setCaret(0, ${o}); return true;`);
      xs.push(await c.eval('return window.__heditor.caret.x;'));
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
      await c.eval(`await window.__heditor.setCaret(0, ${o}); return true;`);
      return c.eval('return window.__heditor.caretReport;');
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
        const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
        const sx = r.width / cv.width, sy = r.height / cv.height;
        const zz = window.__heditor.zoomLevel;
        // 帧磅 → 帧像素（× zoom）→ 客户端像素（× CSS 缩放）
        const clientX = r.left + (${absPt[o] + 0.2} * zz) * sx;
        const clientY = r.top + ${g0.y + g0.h / 2} * sy;
        cv.dispatchEvent(new MouseEvent('mousedown',
          { clientX, clientY, bubbles: true, cancelable: true }));
        if (window.__heditor.quiesce) await window.__heditor.quiesce();  // 异步宿主：命中+落光标要走往返，读之前必须先排空
        return window.__heditor.caret.offset;`);
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
      const left = await c.eval(`return await window.__heditor.hit(${mid - 0.2}, ${yPt});`);
      const right = await c.eval(`return await window.__heditor.hit(${mid + 0.2}, ${yPt});`);
      if (left && right && left.offset === o && right.offset === o + 1) ++bok;
      else bad.push(`o=${o}: 左→${left ? left.offset : 'null'} 右→${right ? right.offset : 'null'}`);
    }
    chk.check(` ${len} 对相邻插入点，中点左半→左边、右半→右边（势力范围 = 到邻居的中点）`,
              bok === len, `成立 ${bok} / ${len}${bad.length ? '；反例 ' + bad.slice(0, 3).join(' | ') : ''}`);

    // 点在画布左边界之外：命中必须失败，而不是胡乱给一个位置。
    // （注意：页内的空白不算失败 —— hitTestPt 的语义是"取同页最近的一行"，
    //   所以"点页面底部空白光标会跳到第一行"是正确行为，不该拿来断言。）
    await c.eval('await window.__heditor.setCaret(0, 5); return true;');
    const outside = await c.eval(`
      const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
      const sy = r.height / cv.height;
      // caret.y 是页内像素（页号另给），画布代理的 rect 是窗口坐标：
      //   窗口内 y = (光标页 - 窗口首页) x 步距 + 页内 y。
      const H = window.__heditor;
      const step = H.pagePx[1] + H.pagePx[2];
      const wy = (H.caret.page - H.editStats.windowFirstPage) * step + H.caret.y;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left - 50, clientY: r.top + (wy + 5) * sy,
          bubbles: true, cancelable: true }));
      if (window.__heditor.quiesce) await window.__heditor.quiesce();  // 异步宿主：命中+落光标要走往返
      return window.__heditor.caret.offset;`);
    chk.check('点画布左边界之外 → 命中失败，光标停在原地（不猜）', outside === 5,
              `offset=${outside}`);
  }

  chk.section('[5] 偏移单位是码点、且吸附到字素簇（经 WASM 边界验证）');
  {
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval('return await window.__heditor.type("e");');
    await c.eval('return await window.__heditor.type("\\u0301");');   // 组合重音 U+0301
    const st = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                               o: window.__heditor.caret.offset };`);
    chk.check('e 类序列：插入 2 个码点，光标偏移 = 2（偏移数的是码点）', st.o === 2, `offset=${st.o}`);

    const posOf = async (o) => {
      await c.eval(`await window.__heditor.setCaret(0, ${o}); return true;`);
      const rep = await c.eval('return window.__heditor.caretReport;');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const p0 = await posOf(0), p1 = await posOf(1), p2 = await posOf(2);
    chk.check(' 偏移 1 不是合法插入点 → 吸附到簇首，位置与偏移 0 相同',
              Math.abs(p1 - p0) < 1e-6, `pos(0)=${p0} pos(1)=${p1}`);
    chk.check('偏移 2 已经是下一个簇 → 位置前进', p2 > p0, `pos(2)=${p2}`);

    const cps = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    await c.eval('await window.__heditor.setCaret(0, 2); return true;');
    await c.eval('return await window.__heditor.backspace();');
    const after = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                                 o: window.__heditor.caret.offset };`);
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
    //   派真实的 KeyboardEvent，而不是调 window.__heditor.type()。
    //
    //   为什么偏偏是 f：liga 连字表里 ff / fi / fl / ffi / ffl 全是以 f 开头的，
    //   而中文测试一个都盖不到 —— 这正是这个 bug 能活到现在的原因。
    await c.eval(`await window.__heditor.reset(6, ${ZOOM}); await window.__heditor.stopBlink(); return true;`);
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');

    const b = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    await c.eval(key('f'));
    await c.eval(key('f'));
    const a = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                               t: window.__heditor.text,
                                               o: window.__heditor.caret.offset };`);
    chk.check('两次按 f → 文档 +2 个码点', a.n === b + 2, `${b} → ${a.n}`);
    chk.check('两个 f 都真的写进了文本', Array.from(a.t).slice(0, 2).join('') === 'ff',
              JSON.stringify(Array.from(a.t).slice(0, 4).join('')));
    chk.check('光标停在 2 —— 每个 f 各占一格（不是 1）', a.o === 2, `offset=${a.o}`);

    // 决定性判据：偏移 1（两个 f 之间）必须是一个合法插入点。
    //   连字一旦把 ff 合成一个簇，解析器给出的簇表里就没有偏移 1 的起点，
    //   光标会被吸附回 0 → 这两次取值相等。用 caretReport 的沿轴位置直接比。
    const posOf = async (o) => {
      await c.eval(`await window.__heditor.setCaret(0, ${o}); return true;`);
      const rep = await c.eval('return window.__heditor.caretReport;');
      const m = /本偏移的沿轴位置=([-\d.]+) pt/.exec(rep);
      return m ? Number(m[1]) : NaN;
    };
    const q0 = await posOf(0), q1 = await posOf(1);
    chk.check(' 偏移 1（两个 f 之间）是合法插入点，位置严格前移（连字一合并这里就相等）',
              q1 > q0 + 1e-6, `pos(0)=${q0} pos(1)=${q1}`);

    await c.eval('await window.__heditor.setCaret(0, 2); return true;');
    await c.eval(key('Backspace'));
    const d = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                               o: window.__heditor.caret.offset };`);
    chk.check(' 一次退格只删掉一个 f（连字一合并这里会一下少两个）',
              d.n === a.n - 1 && d.o === 1, `${a.n} → ${d.n}，offset=${d.o}`);

    // Delete 走的是另一条分支（clusterCeil），也得守。
    // 必须先把光标移回 0：退格之后光标停在 1，而 1 处是原文第一个字，
    //   在那儿按 Delete 删掉的是"患"而不是剩余的那个 f —— 那测的就不是连字了。
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval(key('Delete'));
    const fwd = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                                 o: window.__heditor.caret.offset };`);
    chk.check(' Delete 也只删掉了那一个 f', fwd.n === d.n - 1 && fwd.o === 0,
              `${d.n} → ${fwd.n}，offset=${fwd.o}`);

    // 收尾：把文档恢复成 reset 后的原样，后面几节才不会被这一节带偏
    await c.eval(`await window.__heditor.reset(6, ${ZOOM}); await window.__heditor.stopBlink(); return true;`);
    const restored = await c.eval(CP_LEN + 'return cplen(window.__heditor.text);');
    chk.check('收尾：文档已恢复（后续小节的起点干净）', restored === Array.from(S6).length,
              `${restored} / ${Array.from(S6).length}`);
  }

  chk.section('[6] IME：组合期间一次都不碰引擎，只有 commit 才落文档');
  {
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const before = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text), t: window.__heditor.text };`);

    await c.eval(`
      window.__heditor.textarea.dispatchEvent(
        new CompositionEvent('compositionstart', { bubbles: true }));
      return true;`);
    // 组合更新来多少次都不该动文档
    for (const d of ['n', 'ni', 'ni3']) {
      await c.eval(`
        window.__heditor.textarea.dispatchEvent(
          new CompositionEvent('compositionupdate', { bubbles: true, data: ${JSON.stringify(d)} }));
        return true;`);
    }
    const mid = await c.eval(CP_LEN + 'return { n: cplen(window.__heditor.text), t: window.__heditor.text };');
    chk.check(' 组合期间（3 次 compositionupdate）文档一个字都没变',
              mid.n === before.n && mid.t === before.t, `${before.n} → ${mid.n}`);

    await c.eval(`
      window.__heditor.textarea.dispatchEvent(
        new CompositionEvent('compositionend', { bubbles: true, data: '\\u4f60' }));
      return true;`);
    const after = await c.eval(CP_LEN + `return { n: cplen(window.__heditor.text),
                                                  t: window.__heditor.text,
                                                  o: window.__heditor.caret.offset };`);
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

    // 同刻读 canvas 与引擎帧并逐字节比较。
    //   帧切成一页一张画布之后，这件事交给 __heditor.selfCheck()：它按页读、按页比，
    //   口径与"整篇一张画布"时相同 —— 屏幕上每一页的像素都必须等于引擎那一帧，
    //   页与页之间的间隙不比（宿主没有它的画布，它只是引擎帧上的背景色）。
    const screenVsFrame = () => c.eval(`
      const sc = await window.__heditor.selfCheck();
      const b = sc.screenVsEngineDiffBox || [0, 0, 0, 0];
      return { diff: sc.screenVsEngineDiffPx, w: sc.framePx[0], h: sc.framePx[1],
               minx: b[0], miny: b[1], maxx: b[2], maxy: b[3] };
    `);

    const resetTo = async () => c.eval(`
      await window.__heditor.reset(6, ${ZOOM});
      await window.__heditor.setCaret(0, ${AT});
      return true;`);

    // A：逐个真按键，闪烁开着，按 ~110ms 的真人节奏
    await resetTo();
    const perKey = [];
    const drift = [];
    for (let i = 0; i < RUN.length; ++i) {
      await c.eval(key(RUN[i]));
      perKey.push(await c.eval('return window.__heditor.editStats;'));
      // 每一次按键都查，不是抽样。
      //   "光标走了、文字没动"这种症状只在某一次贴错；抽样会把漏掉。
      drift.push(await screenVsFrame());
      await sleep(110);
    }
    const worstDrift = drift.reduce((m, d) => Math.max(m, d.diff), 0);
    const worstBox = drift.find((d) => d.diff === worstDrift) || {};
    const textA = await c.eval('return window.__heditor.text;');
    // 定格成确定的相位再取 A 帧，否则和 B 帧的差异里混着闪烁
    await c.eval(`await window.__heditor.stopBlink(); await window.__heditor.present(); return true;`);
    await c.eval('window.__frameA = await window.__heditor.rawFrame(); return true;');
    await c.screenshot(join(OUT, 'typing-state.png'));

    // B：同样的文档，一次性插入，同样定格
    await resetTo();
    await c.eval(`return await window.__heditor.type(${JSON.stringify(RUN)});`);
    const textB = await c.eval('return window.__heditor.text;');
    await c.eval(`await window.__heditor.stopBlink(); await window.__heditor.present(); return true;`);

    const d = await c.eval(`
      const A = window.__frameA;
      const B = await window.__heditor.rawFrame();
      const w = window.__heditor.frameW();
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
      return { diff, w, h: window.__heditor.frameH(), minx, miny, maxx, maxy,
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
    // 这一条一度断言"每一次按键都整页重画"，用来证明上面那条"增量 == 从零"不是
    //   "两边错得一样"。行带重画之后它不再成立：改一行只重画那一条带。
    //   "不是空话"这件事改由上面两条更强的保证 —— 屏幕 == 引擎帧、增量帧 == 从零重画的帧，
    //   两条都是逐像素比。这里只留一个上界：写入量不许超过整页（带 + 光标 + 标记）。
    const maxPx = perKey.reduce((m, s) => Math.max(m, s.repaintedPixels), 0);
    chk.check(' 每一次按键的写入量都不超过整页（行带重画的上界）', maxPx <= full * 1.05,
              `最大那次 ${maxPx} vs 整页 ${full}`);

    // 把给用户用的取证接口也当场跑一遍 —— 它要是抛异常，用户那边就白跑一趟。
    const sc = await c.eval('return await window.__heditor.selfCheck();');
    chk.check(' 自检接口可用且当场判为一致（__heditor.selfCheck()）',
              !!sc && sc.screenVsEngineDiffPx === 0 && Array.isArray(sc.framePx),
              sc ? `屏幕与引擎帧差 ${sc.screenVsEngineDiffPx} px，帧 ${sc.framePx.join('×')}，` +
                   `dpr=${sc.dpr}，窗口 第${sc.window.first}页起 ${sc.window.count} 页` +
                   `（比了 ${sc.window.comparedPages} 页 / 页画布 ${sc.pageCanvases} 张），` +
                   `页 css [${sc.pages.cssSize}] / rect [${sc.pages.rectPx}]`
                 : '返回 null');
  }

  chk.section('[8] 表单：控件是「区间」、边界是「策略」');
  {
    // 这一节要证明四件事，缺一不可：
    //   标记真的被画出来了（不是"数据里有控件、画面上什么都没有"）；
    //   括号常驻：每个字段的左括号一直在，且不随光标变（默认档 All）；
    //   字段被删空之后括号仍然在（内容没了、"位置还占着"、括号却消失了）；
    //   只读模式标记真的消失（文档与表单都画，只读是呈现与打印态，不画也不占位）。
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
      const f = await window.__heditor.rawFrame();
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
      return (async function () {
        const near = ${MARK};
        const f = await window.__heditor.rawFrame();
        const fw = window.__heditor.frameW();
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

    await c.eval('return await window.__heditor.loadFormSample(0);');
    const ct = await c.eval('return window.__heditor.controls;');
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
        return (async function () {
          const f = await window.__heditor.rawFrame();
          const W = window.__heditor.frameW(), H = window.__heditor.frameH();
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

      await c.eval(`await window.__heditor.reset(6, ${ZOOM}); await window.__heditor.stopBlink(); return true;`);
      await c.eval('await window.__heditor.present(); return true;');
      const base = await c.eval(lastInkLeft);

      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.stopBlink();');
      await c.eval('return await window.__heditor.setCaret(0, 6);');
      await c.eval('await window.__heditor.present(); return true;');
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
        const cc = await c.eval('return window.__heditor.controls;');
        return cc.items[i] && cc.items[i].rects[0] ? cc.items[i].rects[0].x : 0;
      };

      await c.eval('return await window.__heditor.setCaret(0, 6);');  // 在 #0 里
      const a0 = await c.eval(bandAt(await rectX(0)));
      const a2 = await c.eval(bandAt(await rectX(2)));

      await c.eval('return await window.__heditor.setCaret(0, 13);');  // 在 #2 里
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
          const f = await window.__heditor.rawFrame();
          const fw = window.__heditor.frameW();
          const near = ${MARK};
          const it = window.__heditor.controls.items[0];
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
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.stopBlink();');
        await c.eval('return await window.__heditor.setCaret(0, 9);');   // complaint [5,9) 的末尾
        for (let i = 0; i < 4; ++i) await c.eval(key('Backspace'));  // 真实键盘删光它
        await c.eval('await window.__heditor.present(); return true;');

        const cc = await c.eval('return window.__heditor.controls;');
        const k0 = cc.items[0];
        const emptyRect = k0 && (!k0.rects || k0.rects.length === 0);
        chk.check('⭐ 前置：字段内容被删空后，它投影不出矩形'
                  + '（老实现正是死在这一步：没有矩形 → 没有标记）',
                  emptyRect, `rects=${JSON.stringify(k0 && k0.rects)}`);

        const slotPx = bracketSlotPx;
        const empty = await c.eval(`
          const st = window.__heditor.editStats;
          const f = await window.__heditor.rawFrame();
          const fw = window.__heditor.frameW();
          const H2 = window.__heditor;
          const winY = (st.caretPage - H2.editStats.windowFirstPage) * (H2.pagePx[1] + H2.pagePx[2]) +
                       st.caretY;
          const mark = (r, g, b) => r > 45 && b - r > 60 && b > 140;   // 光标色 r=31 被排除
          const cx = st.caretX;
          let left = 0, right = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < winY || y >= winY + st.caretH) continue;
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
        const scEmpty = await c.eval('return await window.__heditor.selfCheck();');
        const auEmpty = await c.eval('return await window.__heditor.auditFrame();');
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
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.stopBlink();');
        await c.eval('return await window.__heditor.setCaret(0, 6);');
        await c.eval('await window.__heditor.present(); return true;');

        const ord = await c.eval(`
          const f = await window.__heditor.rawFrame();
          const fw = window.__heditor.frameW();
          const cc = window.__heditor.controls;
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

      // ⭐ 运行期开关括号：__heditor.setShowBrackets(false|true)
      //
      //   这条测的是"改配置也要重排"：括号进文档流，显示与否一变占位就变，
      //     正文得跟着挪。只重绘不重排的话，括号会按新的开关画、正文却按旧口径排 ——
      //     表象是括号盖在字上/槽里空一条，而且从数字上看不出来。
      //   判据取"开关前后同一个字段的竖带"，加上屏幕/帧一致。
      {
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.stopBlink();');
        await c.eval('return await window.__heditor.setCaret(0, 6);');   // 光标在 #0
        await c.eval('await window.__heditor.present(); return true;');
        const on0 = await c.eval(bandAt(await rectX(0)));
        const on2 = await c.eval(bandAt(await rectX(2)));

        await c.eval('return await window.__heditor.setShowBrackets(false);');   // 括号全关
        const off0 = await c.eval(bandAt(await rectX(0)));
        const off2 = await c.eval(bandAt(await rectX(2)));
        const scSw = await c.eval('return await window.__heditor.selfCheck();');
        const auSw = await c.eval('return await window.__heditor.auditFrame();');

        await c.eval('return await window.__heditor.setShowBrackets(true);');    // 再打开
        const back0 = await c.eval(bandAt(await rectX(0)));
        const back2 = await c.eval(bandAt(await rectX(2)));

        chk.check(' setShowBrackets：开（两个都在）→ 关（两个都没有）→ 开（又都在）',
                  on0 > 0 && on2 > 0 && off0 === 0 && off2 === 0 && back0 > 0 && back2 > 0,
                  `开 #0=${on0}/#2=${on2} ｜ 关 #0=${off0}/#2=${off2} ｜ ` +
                  `开 #0=${back0}/#2=${back2}`);
        chk.check(' 开关括号之后：引擎帧 == 整帧重画、且屏幕 == 引擎帧（改配置确实重排了）',
                  auSw && auSw.diff === 0 && scSw.screenVsEngineDiffPx === 0,
                  `帧自审差 ${auSw ? auSw.diff : '?'} px，屏幕差 ${scSw.screenVsEngineDiffPx} px`);
      }

      // 小点：独立开关，与括号无关。它不占宽度 → 开关不该重排，只改像素。
      {
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.setCaret(0, 6);');
        await c.eval('await window.__heditor.present(); return true;');
        await c.eval('return await window.__heditor.setShowControlDots(true);');
        const stDot = await c.eval('return window.__heditor.editStats;');
        const scDot = await c.eval('return await window.__heditor.selfCheck();');
        await c.eval('return await window.__heditor.setShowControlDots(false);');
        const scOff = await c.eval('return await window.__heditor.selfCheck();');
        chk.check(' 小点开关：不触发重排（它不占宽度、不进文档流）', stDot.layoutRan === false,
                  `layoutRan=${stDot.layoutRan}`);
        chk.check(' 小点开关：开与关之后屏幕都等于引擎帧',
                  scDot.screenVsEngineDiffPx === 0 && scOff.screenVsEngineDiffPx === 0,
                  `开差 ${scDot.screenVsEngineDiffPx} px，关差 ${scOff.screenVsEngineDiffPx} px`);
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
      const aud = await c.eval('return await window.__heditor.auditFrame();');
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
      // 这一节要"第一次 present 写整帧"：
      //   触发条件是"一次配置变更让整窗重排 → present 写整帧"，
      //   用开关括号来制造（它和换字段是同一类事件：排版输入变了）。
      //   旧的 Focused 档已经删掉，不能再靠"换字段"制造重排。
      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.stopBlink();');
      await c.eval('return await window.__heditor.setCaret(0, 6);');   // 光标在 #0（反复心悸）

      // 把相位摆到"隐藏"，并且让引擎的账也进入隐藏态
      await c.eval('await window.__heditor.showCaret(false); await window.__heditor.present(); return true;');
      const hiddenPre = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('前置：把闪烁相位摆到「光标隐藏」之后，屏幕仍等于引擎帧',
                hiddenPre.screenVsEngineDiffPx === 0,
                `差 ${hiddenPre.screenVsEngineDiffPx} px`);

      // 在隐藏半拍里关掉括号（＝设置面板上点一下「显示括号」）
      await c.eval('return await window.__heditor.setShowBrackets(false);');
      await c.eval('return await window.__heditor.stopBlink();');   // afterEdit 会重启定时器，再停掉
      const hiddenStat = await c.eval('return window.__heditor.editStats;');
      chk.check(' 前提：关括号那一拍光标确实处于隐藏相位（宿主正是靠它决定走补救路径）',
                hiddenStat.caretVisible === false, `caretVisible=${hiddenStat.caretVisible}`);

      // 宿主补救：看到 caretVisible=false 就把光标点亮再呈现一次（app.js 走的就是这条）。
      //   配置变更不像编辑那样自动点亮光标（setShowBrackets 不算编辑），所以要显式补。
      await c.eval('await window.__heditor.showCaret(true); await window.__heditor.present(); return true;');
      await c.eval('return await window.__heditor.stopBlink();');
      const scHide = await c.eval('return await window.__heditor.selfCheck();');
      const auHide = await c.eval('return await window.__heditor.auditFrame();');

      chk.check(' 隐藏半拍关括号：引擎帧必须等于整帧重画（引擎侧无罪）',
                auHide && auHide.diff === 0,
                auHide ? `差 ${auHide.diff} px` + (auHide.box ? ` 盒=${JSON.stringify(auHide.box)}` : '')
                       : '返回 null');
      chk.check(' 隐藏半拍关括号：屏幕像素也必须等于引擎帧 —— 这就是用户那个 bug',
                scHide.screenVsEngineDiffPx === 0,
                scHide.screenVsEngineDiffPx === 0
                  ? '一致'
                  : `差 ${scHide.screenVsEngineDiffPx} px，盒=${JSON.stringify(scHide.screenVsEngineDiffBox)}`
                    + `（引擎写了的区域没被贴上屏）`);
      chk.check(' 补救之后光标必须可见（宿主把隐藏半拍收尾了）',
                scHide.caret.visible === true, `visible=${scHide.caret.visible}`);

      // 对照：换字段不再重排，上屏量远小于整帧
      //
      //  这条钉死"括号常显"的收益：占位集合与光标无关 ⇒ 换字段不触发布局，
      //  上屏从"整帧"掉到"一块小矩形"。
      {
        await c.eval('return await window.__heditor.setShowBrackets(true);');   // 括号开回来
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.stopBlink();');
        await c.eval('return await window.__heditor.setCaret(0, 6);');
        await c.eval('await window.__heditor.present(); return true;');

        await c.eval('return await window.__heditor.setCaret(0, 14);');      // 换字段
        const cst = await c.eval('return window.__heditor.editStats;');
        const fw = await c.eval('return window.__heditor.frameW();');
        const fh = await c.eval('return window.__heditor.frameH();');
        chk.check(' 换字段：不重排（layoutRan=false）',
                  cst.layoutRan === false, `layoutRan=${cst.layoutRan}`);
        chk.check(' 换字段：上屏量远小于整帧（括号常驻的收益）',
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
        await c.eval('return await window.__heditor.loadFormSample(0);');
        await c.eval('return await window.__heditor.stopBlink();');
        // 表单样例：complaint[5,9) / duration[9,11) 只读 / aggravate[12,16)
        // aggravate 是最右一个可编辑字段 → 它行内最右的标记墨迹就是它的 ]。
        await c.eval('return await window.__heditor.setCaret(0, 16);');   // 「天」右边、`]` 左边
        await c.eval('await window.__heditor.present(); return true;');

        const cmp = await c.eval(`
          const st = window.__heditor.editStats;
          const f = await window.__heditor.rawFrame();
          const fw = window.__heditor.frameW();
          const H2 = window.__heditor;
          const winY = (st.caretPage - H2.editStats.windowFirstPage) * (H2.pagePx[1] + H2.pagePx[2]) +
                       st.caretY;
          const near = ${MARK};   // 标记亮蓝；光标色 r=31 被排除
          let bracketX = -1;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const p = i >> 2, x = p % fw, y = (p - x) / fw;
            if (y < winY || y >= winY + st.caretH) continue;
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

    //  方向键一次一格：光标位置不受控件约束，所以能走出字段、也能跨过只读字段。
    //  约束编辑的是编辑能力表（插 / 删），不是光标 —— 两者混起来会得到
    //  "字段外的正文连点都点不进去"这类与编辑边界无关的怪现象。
    //
    //  判据是逐步的：一次只跨一枚簇（括号占位也算一格），到文档末尾就停住。
    //    只看终点抓不到"投影式移动"：投影也能给出一个看着合法的终点。
    {
      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.stopBlink();');
      await c.eval('return await window.__heditor.setCaret(0, 12);');   // #2 = aggravate [12,16)

      await c.eval(key('ArrowLeft'));                          //  真实键盘
      const afterLeft = await c.eval('return window.__heditor.caret;');
      chk.check('★ 在 #2 起点按左方向键：先退到 [ 之前（括号占一格，不瞬移）',
                afterLeft.offset === 12 && afterLeft.side === 0,
                `12 → ${afterLeft.offset}/${afterLeft.side}`);

      // 右走 6 步：13 14 15 16 16(跨 ]) 17（到文档末尾就停住），单步跨度恒为 1
      await c.eval('return await window.__heditor.setCaret(0, 12);');
      const seq = [];
      for (let i = 0; i < 6; ++i) {
        await c.eval(key('ArrowRight'));
        seq.push((await c.eval('return window.__heditor.caret;')).offset);
      }
      let worst = 0;
      let prev = 12;
      for (const o of seq) { worst = Math.max(worst, o - prev); prev = o; }
      chk.check('★ 从 #2 起点右走 6 步：一次一格，到文档末尾就停住',
                seq.join(',') === '13,14,15,16,16,17',
                `12 → ${seq.join(' → ')}`);
      chk.check(' 单步跨度恒为 1（没有任何一步瞬移）',
                worst === 1, `最大单步跨度 ${worst}`);
    }

      // 同一件事用"敲字"再走一遍（这条路径由打字触发）
      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.stopBlink();');
      await c.eval('return await window.__heditor.setCaret(0, 6);');
      await c.eval('await window.__heditor.showCaret(false); await window.__heditor.present(); return true;');
      await c.eval(key('x'));                     //  真实键盘路径
      await c.eval('return await window.__heditor.stopBlink();');
      const scType = await c.eval('return await window.__heditor.selfCheck();');
      const auType = await c.eval('return await window.__heditor.auditFrame();');
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
    await c.eval('return await window.__heditor.setEditMode(2);');
    const txt0 = await c.eval('return window.__heditor.text;');
    const blk0 = await c.eval('return window.__heditor.blockedEdits;');
    for (const ch of 'abc') await c.eval(key(ch));
    const txt1 = await c.eval('return window.__heditor.text;');
    const blk1 = await c.eval('return window.__heditor.blockedEdits;');
    chk.check(' 只读模式：按键全被拒、文本一字不变、被拒次数 +3',
              txt1 === txt0 && blk1 === blk0 + 3,
              `被拒 ${blk1 - blk0} 次，文本${txt1 === txt0 ? '未变' : '变了'}`);

    // 表单模式：可编辑字段内能打进去，且区间跟着变长
    await c.eval('return await window.__heditor.loadFormSample(0);');
    await c.eval('return await window.__heditor.setCaret(0, 6);');
    const len0 = await c.eval('return Array.from(window.__heditor.text).length;');
    await c.eval(key('测'));
    const len1 = await c.eval('return Array.from(window.__heditor.text).length;');
    const ct2 = await c.eval('return window.__heditor.controls;');
    chk.check(' 表单模式：字段内按键成功，且字段区间跟着变长（9 → 10）',
              len1 === len0 + 1 && ct2.items[0].end[1] === 10,
              `码点 ${len0} → ${len1}，字段 #0 end=${ct2.items[0].end[1]}`);

    // 只读模式：标记必须一个像素都不剩。
    //   文档模式与表单模式都画括号（编辑态要一眼看出哪块是控件），只有只读不画 ——
    //   它是要印出来的呈现态，画上去就印出来了。三档的这一条口径见 [15]。
    await c.eval('return await window.__heditor.setEditMode(2);');
    const roMark = await c.eval(inkMark);
    chk.check('★ 只读模式：标记一个像素都不剩', roMark === 0, `${roMark} px`);

    // 文档模式：标记照常画出来（不再是"只有表单模式才看得见控件边界"）
    await c.eval('return await window.__heditor.setEditMode(0);');
    const docMark = await c.eval(inkMark);
    chk.check('★ 文档模式：标记照常出现（编辑态看得见哪块是控件）', docMark > 0,
              `${docMark} px`);

    // 切回表单模式后标记仍在（证明不是"切走一次就再也不画"）
    await c.eval('return await window.__heditor.setEditMode(1);');
    const backMark = await c.eval(inkMark);
    chk.check(' 切回表单模式：标记也没有丢', backMark > 0, `${backMark} px`);

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
      const total = Array.from(window.__heditor.text).length;
      let checked = 0, bad = 0, firstBad = '';
      // 每字段的 min/max 存成对象（{ci: [lo, hi]}），不能存数组：
      //   只读字段会被跳过 → 稀疏数组在跨进程回传时被序列化成 null
      //   （「Cannot read properties of null (reading '0')」，而且栈里
      //    看不到是哪一行 —— 因为整段是 eval 的字符串）。
      const seen = {};
      for (let off = 0; off <= total; ++off) {
        await window.__heditor.setCaret(0, off);
        const cc = window.__heditor.controls;
        const f = await window.__heditor.rawFrame();
        const fw = window.__heditor.frameW();
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
    const mk = await c.eval('return await window.__heditor.markCheck();');
    chk.check(' 用户自检接口可用且当场判为一致（__heditor.markCheck()）',
              !!mk && mk.allOk === true && Array.isArray(mk.rows) && mk.rows.length >= 1,
              mk ? `${mk.rows.length} 个可编辑字段，全部一致=${mk.allOk}` : '返回 null');

    // Tab = 缩进（不是跳字段）
    //
    //   语义纠偏：Tab 在编辑器里是缩进。绑成"跳到下一个控件"是表单/网页里
    //     字段之间的约定，放到正文编辑器里会把肌肉记忆打断（用户明确纠正过）。
    //     字段之间用鼠标点、或者方向键（光标本来就会跨字段）。
    //   改完之后"跳字段"只剩程序化入口 __heditor.focusControl（探针/自动化用），
    //     下面单独验它还在。
    //
    //   这一段的核心判据是"真实按键 → 屏幕像素 == 引擎帧"。它抓的是这一类：
    //     某个入口改了引擎状态却没给自己收尾，宿主于是拿着上一次呈现的
    //     脏矩形去上屏，画布变成"旧内容 + 局部新内容"的混合体
    //     （的"光标走了、旧括号还在"）。
    {
      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.setCaret(0, 6);');   // k1 内
      await c.eval('await window.__heditor.stopBlink(); await window.__heditor.present(); return true;');

      const beforeTab = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('前置：按 Tab 之前屏幕与引擎帧一致（否则下面的判据没意义）',
                beforeTab.screenVsEngineDiffPx === 0, `差 ${beforeTab.screenVsEngineDiffPx} px`);

      const nBefore = await c.eval('return Array.from(window.__heditor.text).length;');
      await c.eval(key('Tab'));                              //  真实键盘路径
      const nAfter = await c.eval('return Array.from(window.__heditor.text).length;');
      const postTab = await c.eval('return await window.__heditor.selfCheck();');
      const liveOff = await c.eval('return window.__heditor.caretLive;');
      const auditTab = await c.eval('return await window.__heditor.auditFrame();');

      chk.check(' 真实 Tab 键 = 缩进：文档 +4 个码点，且是 4 个空格',
                nAfter === nBefore + 4 &&
                  (await c.eval('return window.__heditor.text.slice(6, 10) === "    ";')),
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
      const nBack = await c.eval('return Array.from(window.__heditor.text).length;');
      chk.check(' Shift+Tab = 反缩进：把 4 个空格删回来',
                nBack === nBefore, `码点 ${nAfter} → ${nBack}`);
    }

    // 程序化跳字段（探针/自动化入口，不再绑键盘）
    {
      await c.eval('return await window.__heditor.loadFormSample(0);');
      await c.eval('return await window.__heditor.setCaret(0, 5);');
      const f1 = await c.eval('return await window.__heditor.focusControl(true);');
      const c1 = await c.eval('return window.__heditor.caret;');
      const f2 = await c.eval('return await window.__heditor.focusControl(true);');
      const c2 = await c.eval('return window.__heditor.caret;');
      chk.check(' __heditor.focusControl（程序化跳字段）：12 → 5 绕回第一个字段',
                !!f1 && !!f2 && c1.offset === 12 && c2.offset === 5,
                `${c1.offset} → ${c2.offset}`);
      const post = await c.eval('return await window.__heditor.selfCheck();');
      const audit = await c.eval('return await window.__heditor.auditFrame();');
      chk.check(' 跳字段之后：屏幕==引擎帧、且引擎帧==整帧重画',
                post.screenVsEngineDiffPx === 0 && audit && audit.diff === 0,
                `屏幕差 ${post.screenVsEngineDiffPx} px，帧自审差 ${audit ? audit.diff : '?'} px` +
                `，盒=${JSON.stringify(post.screenVsEngineDiffBox)}` +
                `，上屏=${JSON.stringify(await c.eval('return window.__heditor.blitStats;'))}`);
      const mk2 = await c.eval('return await window.__heditor.markCheck();');
      chk.check(' 跳字段后每个字段的括号都还在（常显；markCheck 自检）',
                !!mk2 && mk2.allOk === true && mk2.showBrackets === true,
                mk2 ? `${mk2.rows.length} 个可编辑字段，showBrackets=${mk2.showBrackets}，`
                      + `全部一致=${mk2.allOk}` : '返回 null');
    }
  }

  chk.section('[9] 长文档：增量排版只重排脏段之后的后缀');
  {
    // 单段文档没有可复用的前缀，增量与全量必然相等（见 [1] / [2]）。
    //   多段文档才谈得上"只重排脏段"—— 这一节用演示的长文档量它。
    //   段数不写死：那张文书是演示数据，随时可能被调大调小，而这一节要钉的是
    //   "重排了几段、复用了几段"这个结构，不是那个数本身。
    const DIRTY = 150;
    const base = await c.eval('return await window.__heditor.run(7, 1.0);');
    chk.check('长文档载入（段数 ≥ 200）', base.totalParagraphs >= 200,
              `${base.totalParagraphs} 段 / ${base.totalLines} 行`);
    chk.check('首次排版是全量（整篇都脏）', base.layoutWasFull === true);
    report.longDoc = { paragraphs: base.totalParagraphs, lines: base.totalLines,
                       fullMs: base.layoutMs };

    // 在脏段中间敲一个字（走真实键盘路径）
    await c.eval(`return await window.__heditor.setCaret(${DIRTY}, 5);`);
    await c.eval(key('测'));
    const st = await c.eval('return window.__heditor.editStats;');
    chk.check('★ 敲字后是增量排版（不是全量）', st.layoutWasFull === false,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段 / 共 ${st.totalParagraphs} 段`);
    // 行结构没变 → 快路：只重排脏段自己，后缀整块搬回（一个字节都不动）。
    chk.check(`★ 只重排第 ${DIRTY} 段，其余原样复用`,
              st.relaidParagraphs === 1 &&
                st.reusedParagraphs === st.totalParagraphs - DIRTY - 1,
              `重排 ${st.relaidParagraphs} 段 / 复用 ${st.reusedParagraphs} 段` +
                `（应为 1 / ${st.totalParagraphs - DIRTY - 1}）`);
    chk.check('★ 重排行数真小于总行数', st.relaidLines < st.totalLines,
              `${st.relaidLines} / ${st.totalLines} 行`);
    // 分段账：前缀 [0,DIRTY) 一个字都没动、只重排第 DIRTY 段、其余整块搬回。
    // 这条比"重排段数对不对"更强：它把分界点也钉住了（前缀/脏段/复用各占哪一段）。
    chk.check('★ 分段账：前缀未动 + 脏段 1 段重排 + 后缀全部复用',
              st.relaidParagraphs + st.reusedParagraphs === st.totalParagraphs - DIRTY,
              `1 + ${st.reusedParagraphs} = ${st.totalParagraphs} - ${DIRTY}`);
    chk.check('★ 增量耗时小于全量耗时（同一次测量里的两个数）',
              st.layoutMs < report.longDoc.fullMs,
              `增量 ${st.layoutMs.toFixed(3)} ms vs 首次全量 ${report.longDoc.fullMs.toFixed(3)} ms`);

    // 排版自审：增量结果与"不用缓存的全量重排"逐字段比 —— 必须完全相同
    const au = await c.eval('return await window.__heditor.auditLayout();');
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
    const sc = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('长文档编辑后：屏幕 == 引擎帧', sc.screenVsEngineDiffPx === 0,
              `差 ${sc.screenVsEngineDiffPx} px`);
    const af = await c.eval('return await window.__heditor.auditFrame();');
    chk.check('长文档编辑后：引擎帧 == 整帧重画', af && af.diff === 0,
              af ? `差 ${af.diff} px` : 'null');

    // 页数跟着长：演示长文档不可能只有一页 —— 否则上面那些"重排行数"没有意义
    chk.check('长文档确实排成了多页', st.totalPages > 1, `${st.totalPages} 页`);

    // 后缀平移复用：撑出一行之后，后缀的断行结果照样复用（只挪坐标）。
    //
    // 40 个汉字足以撑出至少一行。走 __heditor.type（直连引擎）而不是键盘逐字派发：
    //   键盘路径上面已经验过，这里要的是"一次插入 40 个字"把行结构撑变。
    const widened = await c.eval(
      `return await window.__heditor.type(${JSON.stringify('测'.repeat(40))});`);
    chk.check('★ 撑出一行 → 后缀整体平移复用（重排 1 段）',
              !!widened && widened.suffixShifted === true && widened.relaidParagraphs === 1,
              widened ? `重排 ${widened.relaidParagraphs} 段 / 复用 ${widened.reusedParagraphs} 段 / ` +
                `平移=${widened.suffixShifted} / 脏页 ${widened.dirtyPageCount}` : 'null');
    chk.check('★ 平移之后后缀段数照旧全部复用',
              !!widened && widened.reusedParagraphs + widened.relaidParagraphs ===
                widened.totalParagraphs - 150,
              widened ? `${widened.relaidParagraphs} + ${widened.reusedParagraphs} = ` +
                `${widened.totalParagraphs} - 150` : 'null');
    const auShift = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 平移复用的排版与全量逐字段一致（auditLayout diff 0）',
              !!auShift && auShift.diff === 0,
              auShift ? `diff ${auShift.diff}（字形 ${auShift.glyphDiff}、行 ${auShift.lineDiff}、` +
                `段 ${auShift.paraDiff}）` : 'null');
    const scShift = await c.eval('return await window.__heditor.selfCheck();');
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

    // 点第二页：不许乱滚。
    //   这一格坏在"caretY 是窗口帧内的 y"被当成了页内 y —— 页偏移算了两遍，
    //   光标坐标落到下一页去，屏幕跟着往下冲，随后窗口重算又滚一次。
    //   取点选在"光标当前所在的那一行"：它一定看得见，所以根本不该把页面挪走。
    const clickP2 = await c.eval(`
      const H = window.__heditor;
      const stage = document.getElementById('stage');
      const dpr = window.devicePixelRatio || 1;
      const cr0 = await H.setCaret(20, 5);      // 第 20 段在 16 页的那一份里是第二页
      const before = stage.scrollTop;
      const cv = H.canvas, r = cv.getBoundingClientRect();
      const sx = r.width / cv.width, sy = r.height / cv.height;
      // 画布代理的 rect 是"窗口帧"的坐标，而 cr0.y 是页内 y —— 加上页号偏移才是窗口内 y。
      const step = H.pagePx[1] + H.pagePx[2];
      const wy0 = (cr0.page - H.editStats.windowFirstPage) * step + cr0.y;
      cv.dispatchEvent(new MouseEvent('mousedown',
        { clientX: r.left + (cr0.x + 2) * sx, clientY: r.top + (wy0 + 2) * sy,
          bubbles: true, cancelable: true }));
      if (window.__heditor.quiesce) await window.__heditor.quiesce();  // 异步宿主：命中+落光标要走往返
      const sc2 = await H.selfCheck();
      return { pageBefore: cr0.page, pageAfter: H.caret.page,
               before, after: stage.scrollTop,
               stepCss: (H.pagePx[1] + H.pagePx[2]) / dpr,
               wf: H.editStats.windowFirstPage, caretY: cr0.y,
               frameDiff: sc2.screenVsEngineDiffPx, box: sc2.screenVsEngineDiffBox,
               caretNow: H.caret, blit: H.blitStats };
    `);
    chk.check('★ 点第二页光标所在那一行：页不变、滚动量小于半页（不乱滚）',
              clickP2.pageBefore >= 1 && clickP2.pageAfter === clickP2.pageBefore &&
                Math.abs(clickP2.after - clickP2.before) <= clickP2.stepCss / 2,
              `光标页 ${clickP2.pageBefore} → ${clickP2.pageAfter} ｜ 滚动 ` +
                `${clickP2.before.toFixed(0)} → ${clickP2.after.toFixed(0)} px` +
                `（半页 ${(clickP2.stepCss / 2).toFixed(0)}）` +
                ` ｜ 窗口首页 ${clickP2.wf}，光标帧内 y=${clickP2.caretY}`);
    chk.check(' 点第二页之后屏幕 == 引擎帧', clickP2.frameDiff === 0,
              `差 ${clickP2.frameDiff} px，盒=${JSON.stringify(clickP2.box)}` +
              `，上屏=${JSON.stringify(clickP2.blit)}`);

    // 滚到下面几页：窗口必须跟着可见页走，可见页必须有画布。
    //   这一格坏在"窗口被光标页钉住"——把光标页无条件并进窗口、再从 min 那一端起算，
    //   用户把屏幕滚到别处之后窗口仍锁在光标那几页：可见页没进窗口 ⇒ 一片空白，
    //   而且算出的锚点与上一轮相同 ⇒ applyWindow 返回 false、连 present 都不做。
    //   症状正是"滚动时下面的画布不生成，点一下才出来"（点击把光标带到那一页）。
    const scrollWin = JSON.parse(await c.eval(`
      return (async function () {
        const H = window.__heditor;
        const stage = document.getElementById('stage');
        const dpr = window.devicePixelRatio || 1;
        const step = (H.pagePx[1] + H.pagePx[2]) / dpr;
        const wait = () => new Promise((r) => requestAnimationFrame(
          () => requestAnimationFrame(r)));
        // 跳页之后要给窗口一个对齐的机会：滚动快过"渲染能力"时宿主会主动放弃追窗口
        //   （app.js 的 tooFastToFollow），等安静 140 ms 再对齐（alignAfterScroll）。
        //   这里每次跳 3 页、三跳连着做，正是会被判成"追不上"的形态 —— 不等就会读到
        //   还没对齐的那一帧。等的是"滚动停下来"，不是"等到断言成立"：安静 180 ms 之后
        //   仍旧读一次状态，对齐没发生的话下面照红。
        const settle = () => new Promise((r) => setTimeout(r, 180));
        const rows = [];
        for (const p of [3, 6, 9]) {
          stage.scrollTop = p * step;
          await settle();
          await wait();
          const vp = H.viewport;
          rows.push({ want: p, first: vp.engineFirst, count: vp.engineCount,
                      canvases: vp.canvases, visFirst: vp.visible.first,
                      visLast: vp.visible.last });
        }
        // 逐页下滚：这一步才是"平移复用"那一支（上面每次跳 3 页，重叠为 0，
        //   走的是整窗重建）。逐拍记下重画页数 —— 平移复用时它应当小于窗口页数。
        //   滚 8 页而不是 4 页：窗口带 3 页余量、且余量偏向滚动方向，所以往下滚 3 页
        //   才换一次窗（见 app.js 的 kWindowLookaheadPages）—— 滚 4 页只赶上一次换窗，
        //   而判据要求至少两次。页数取多少是"被测策略的余量"决定的，不是随手写的：
        //   余量再调大时这里要跟着调，否则判据会变成"恒红"而不是"恒绿"。
        stage.scrollTop = 0;
        await wait();
        const shiftSteps = [];
        for (let k = 1; k <= 8; ++k) {
          stage.scrollTop = k * step;
          await wait();
          const st = H.editStats;
          shiftSteps.push({ page: k, first: st.windowFirstPage, pages: st.windowPages,
                            repainted: st.repaintedPages, bands: st.bandY1 >= 0 });
        }
        return JSON.stringify({ rows, shiftSteps });
      })();
    `));
    for (const r of scrollWin.rows) {
      const covered = r.first <= r.visFirst && r.visLast <= r.first + r.count - 1;
      chk.check(` 滚到第 ${r.want} 页：窗口跟着可见页走（${r.first}+${r.count}）`,
                covered, `可见 ${r.visFirst}~${r.visLast}｜窗口 ${r.first}+${r.count}` +
                         `｜画布 ${r.canvases} 张`);
    }

    // 逐页下滚时确实在"平移复用"：重画页数严格小于窗口页数。
    //   这一条是下面那两条自审的前提检查 —— 没有它，一次"跳 3 页"的滚动
    //   会让窗口重叠为 0、退化成整窗重建，于是底图自审恒绿，什么也没测到
    //   （反证做过：把平移量故意少搬 3 px，只有自审的那一版照样全绿）。
    //   判"至少有 2 步在复用"而不是"每一步都是"：回滚到文档顶部那一步位移可能
    //   超过一整个窗口，重叠为 0 时本来就该整窗重建。
    const reusedSteps = scrollWin.shiftSteps.filter((s) => s.repainted > 0 && s.repainted < s.pages);
    chk.check('逐页下滚走的是平移复用（重画页数 < 窗口页数）',
              reusedSteps.length >= 2,
              scrollWin.shiftSteps.map((s) =>
                `第${s.page}页 窗${s.first}+${s.pages} 重画${s.repainted}`).join(' ｜ '));

    // 换窗之后底图必须与"从零全建"逐字节相同。
    //   滚动换窗走的是平移复用（把重叠页的像素 memmove 过去、只重画新露出的页），
    //   最容易错的三种：搬错行、上一帧的光标/标记/选区跟着搬走变成幽灵、指纹没重新对齐。
    //   为什么不能用 auditFrame 顶替：那一条比的是帧，而帧是从底图搬的 ——
    //   底图整个错了，帧与"整帧重贴"照样一致，两条都绿而画面是错的。
    const abScroll = await c.eval('return await window.__heditor.auditBase();');
    chk.check('★★ 滚动换窗之后底图 == 从零全建（逐字节）',
              !!abScroll && abScroll.diff === 0,
              abScroll ? `差 ${abScroll.diff} px` +
                (abScroll.diff > 0 ? `（框 ${JSON.stringify(abScroll.box)}）` : '') : 'null');
    const afScroll = await c.eval('return await window.__heditor.auditFrame();');
    chk.check('★★ 滚动换窗之后引擎帧 == 整帧重画（逐字节）',
              !!afScroll && afScroll.diff === 0,
              afScroll ? `差 ${afScroll.diff} px` : 'null');
    const scScroll = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('滚动换窗之后屏幕 == 引擎帧', scScroll.screenVsEngineDiffPx === 0,
              `差 ${scScroll.screenVsEngineDiffPx} px`);
  }

  chk.section('[10] 跨段编辑：真实回车分段 / 段首退格合并');
  {
    // 用真实的 Enter 按键走一遍（不是直接调引擎）—— 键盘路径与引擎路径是两处接线，
    //   只测引擎会漏掉"宿主没把 Enter 接上"这类问题。
    await c.eval('return await window.__heditor.run(6, 1.0);');
    await c.eval('return await window.__heditor.stopBlink();');
    const before = await c.eval('return window.__heditor.editStats;');
    const textBefore = await c.eval('return window.__heditor.allText;');
    await c.eval('return await window.__heditor.setCaret(0, 5);');
    await c.eval(key('Enter'));

    const after = await c.eval('return window.__heditor.editStats;');
    const textAfter = await c.eval('return window.__heditor.allText;');
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
    const auSplit = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 分段之后增量排版 == 全量重排（逐字段）',
              !!auSplit && auSplit.diff === 0,
              auSplit ? `diff ${auSplit.diff}（字形 ${auSplit.glyphDiff}、行 ${auSplit.lineDiff}、` +
                `段 ${auSplit.paraDiff}）` : 'null');
    const scSplit = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('分段之后屏幕 == 引擎帧', scSplit.screenVsEngineDiffPx === 0,
              `差 ${scSplit.screenVsEngineDiffPx} px`);
    const afSplit = await c.eval('return await window.__heditor.auditFrame();');
    chk.check('分段之后引擎帧 == 整帧重画', afSplit && afSplit.diff === 0,
              afSplit ? `差 ${afSplit.diff} px` : 'null');

    // 段首退格并回去（同一个往返：文档要么回到原样，要么至少段数 -1）
    await c.eval('return await window.__heditor.setCaret(1, 0);');
    await c.eval(key('Backspace'));
    const merged = await c.eval('return window.__heditor.editStats;');
    chk.check('★ 段首退格把两段并回去（段数 -1）',
              merged.totalParagraphs === after.totalParagraphs - 1,
              `${after.totalParagraphs} → ${merged.totalParagraphs} 段`);
    chk.check('★ 合并之后光标落在合并点上', merged.caretPara === 0 && merged.caretOffset === 5,
              `段 ${merged.caretPara} 偏移 ${merged.caretOffset}`);
    const auMerge = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 合并之后增量排版 == 全量重排（逐字段）', !!auMerge && auMerge.diff === 0,
              auMerge ? `diff ${auMerge.diff}` : 'null');
    chk.check('合并之后文档回到分段之前的字符数',
              (await c.eval('return window.__heditor.allText;')).length === textBefore.length,
              `回到 ${textBefore.length} 字符`);

    // 把缩放还回去：[9] 与本节的 run(7/6, 1.0) 只是为了让长文档的窗口页数可控，
    //   而缩放是页面级状态。不还原的话，从 [11] 往后的每一节都在 1.0 下跑，
    //   report.zoom 里记的却是 1.5。后面各节都读实时的实际缩放来算坐标（自洽），
    //   只有依赖"文档有多高"的小节会失真：拖选出可视区那一节就是这样，文档矮了
    //   三分之一，自动滚动很快滚到底，"还在继续滚"于是永远量不出来。
    //   判据必须自己钉住前提，不靠前一节留下的状态。
    await c.eval(`return await window.__heditor.run(6, ${ZOOM});`);
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
    await c.eval('return await window.__heditor.loadFormSample(0);');
    await c.eval('return await window.__heditor.stopBlink();');
    const b11 = await c.eval('return window.__heditor.controls;');
    report.boundary = { before: b11.items.map((it) => [...it.start, ...it.end]) };

    chk.check('前置：字段 #0 的末尾与 #1 的开头是同一个位置（共享边界）',
              b11.count >= 2 && b11.items[0].end[0] === b11.items[1].start[0] &&
                b11.items[0].end[1] === b11.items[1].start[1],
              `#0 [${b11.items[0].start[1]},${b11.items[0].end[1]}) ｜` +
                ` #1 [${b11.items[1].start[1]},${b11.items[1].end[1]})`);

    // 光标停在这个边界上，然后派一个真实按键（不是 __heditor.type，见 [4] 的说明）
    await c.eval(`return await window.__heditor.setCaret(0, ${b11.items[0].end[1]});`);
    const cc11 = await c.eval('return window.__heditor.controls;');
    chk.check('光标停在边界上时归前一个字段（末尾算在区间内）', cc11.caret === 0,
              `caret-control=${cc11.caret}`);
    await c.eval(key('诊'));

    const a11 = await c.eval('return window.__heditor.controls;');
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

    const au11 = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 边界上打字之后增量排版 == 全量重排（逐字段）',
              !!au11 && au11.diff === 0,
              au11 ? `diff ${au11.diff}（字形 ${au11.glyphDiff}、行 ${au11.lineDiff}、` +
                `段 ${au11.paraDiff}）` : 'null');
    const sc11 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('边界上打字之后屏幕 == 引擎帧', sc11.screenVsEngineDiffPx === 0,
              `差 ${sc11.screenVsEngineDiffPx} px`);

    await c.eval('return await window.__heditor.setCaret(0, 10);');
    await c.eval('await window.__heditor.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'shared-boundary.png'));

    // 三连可编辑字段（演示样例 variant 2）：三条边界两侧都可写 —— 前两个样例的边界
    //   一侧是只读的，只能在一个方向试。这里在边界上打字、再把中间那个删空，
    //   看区间和矩形会不会互相穿过。
    await c.eval('return await window.__heditor.loadFormSample(2);');
    await c.eval('return await window.__heditor.stopBlink();');
    const v3 = await c.eval('return window.__heditor.controls;');
    report.chain3 = { before: v3.items.map((it) => [...it.start, ...it.end]) };
    chk.check('三连样例：3 个字段都紧挨着（两条共享边界）',
              v3.count === 3 && v3.items.every((it) => !it.ro) &&
                v3.items[0].end[1] === v3.items[1].start[1] &&
                v3.items[1].end[1] === v3.items[2].start[1],
              v3.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    await c.eval('return await window.__heditor.setCaret(0, 7);');  // 第一条共享边界
    await c.eval(key('痛'));
    const v3b = await c.eval('return window.__heditor.controls;');
    report.chain3.afterType = v3b.items.map((it) => [...it.start, ...it.end]);
    // 字进前一个字段、后两个整体右移；三段首尾相接就是"不穿过"的最强形式
    const chain = (r) =>
      r.items.every((it, i) => i + 1 >= r.items.length || it.end[1] === r.items[i + 1].start[1]);
    chk.check('★ 三连样例：在第一条边界上打字之后，三段区间仍然首尾相接（不重叠、不穿线）',
              chain(v3b) && v3b.items[0].end[1] === 8 && v3b.items[1].start[1] === 8 &&
                v3b.items[1].end[1] === 10 && v3b.items[2].start[1] === 10,
              v3b.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    // 把中间那个删空：它变成零宽的空字段，边界上与"左字段的闭 + 右字段的开"并列。
    //   这里重新载入一次样例，拿回干净的三连区间（[5,7) [7,9) [9,11)）—— 上面那一步
    //   已经往左字段里打过字，它变长成了 [5,8)，量出来的就不是这条边界了。
    await c.eval('return await window.__heditor.loadFormSample(2);');
    await c.eval('return await window.__heditor.stopBlink();');
    await c.eval('return await window.__heditor.setCaret(0, 9);');
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    const v3c = await c.eval('return window.__heditor.controls;');
    report.chain3.afterEmpty = v3c.items.map((it) => [...it.start, ...it.end]);
    const mid = v3c.items[1];
    const right = v3c.items[2];
    chk.check('★ 中间字段被删成零宽的空字段，右边那个左移并保持紧挨着',
              mid.start[1] === mid.end[1] && right.start[1] === mid.end[1],
              v3c.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    // 删空之后那个零宽字段必须还能进、还能打字：
    //   它是自己位置上的最内层（区间树要跟着编辑重算），否则位置被左邻居抢走，
    //   表现就是"中间的控件删掉最后一个字之后光标再也进不去、字也打不进去"。
    //   注意顺序：这一段必须在"往左字段里打字"之前做，否则左字段已经变长（[5,8)），
    //   量出来的边界就不是 [5,7] 那一处了。
    const caretAt = await c.eval('return window.__heditor.caret.offset;');
    chk.check('★ 光标停在那个零宽字段的位置上（位置归属没被左邻居抢走）',
              caretAt === mid.start[1], `光标 ${caretAt}，空字段 ${mid.start[1]}`);
    // 方向键一次一格 —— 括号也是格（占位簇各占一枚），所以共享边界 ][ 要按两下：
    //   一格一格地走过"字与开括号之间""两枚括号之间"那些位置，不许整段跳过去。
    await c.eval('return await window.__heditor.setCaret(0, 5);');
    const walk = [];
    for (let i = 0; i < 5; ++i) {
      await c.eval(key('ArrowRight'));
      const a = await c.eval(`
        const q = window.__heditor.caret;
        return { o: q.offset, s: q.side };`);
      walk.push(`${a.o}/${a.s}`);
    }
    report.chain3.walk = walk;
    chk.check('★ 方向键一次一格（括号占格：][][] 上要按好几下才过去）',
              JSON.stringify(walk) === JSON.stringify(['6/0', '7/0', '7/1', '7/2', '7/3']),
              `落点 ${walk.join(' ')}`);
    // 回到那个零宽字段的位置上：打字应该进它自己，左右两个字段不许被撑大
    await c.eval('return await window.__heditor.setCaret(0, 7);');
    await c.eval(key('痛'));
    const v3d = await c.eval('return window.__heditor.controls;');
    report.chain3.afterFill = v3d.items.map((it) => [...it.start, ...it.end]);
    chk.check('★ 在空字段的位置上打字 → 字进它自己（左右两个字段都没被撑大）',
              v3d.items[1].start[1] === 7 && v3d.items[1].end[1] === 8 &&
                v3d.items[0].end[1] === 7 && v3d.items[2].start[1] === 8,
              v3d.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    const au3 = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 三连样例的每一步增量排版都等于全量重排', !!au3 && au3.diff === 0,
              au3 ? `diff ${au3.diff}` : 'null');
    const sc3 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('三连样例：屏幕 == 引擎帧', sc3.screenVsEngineDiffPx === 0,
              `差 ${sc3.screenVsEngineDiffPx} px`);
    await c.eval('return await window.__heditor.setCaret(0, 7);');
    await c.eval('await window.__heditor.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'three-adjacent-fields.png'));
  }

  chk.section('[12] 真实鼠标点击的归属：点后一个字段的开头要进后一个字段');
  {
    // 为什么必须用真实鼠标事件（CDP Input.dispatchMouseEvent）：
    //   这一格坏在"点画布 → 命中测试 → 归属"这条链上，而合成 MouseEvent 与真实点击
    //   走的虽然都是同一个监听器，但只有真实事件连坐标换算、命中、dpr 一起过一遍。
    //   之前所有探针都用合成事件，所以这格一直没被钉住。
    await c.eval('return await window.__heditor.loadFormSample(2);');
    await c.eval('return await window.__heditor.stopBlink();');
    const geo = await c.eval(`
      const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
      const c0 = window.__heditor.controls.items[0].rects[0];
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               zoom: window.__heditor.zoomLevel, y: c0.y + c0.h / 2 };`);
    const click = async (xPt, yPt) => {
      const x = geo.left + xPt * geo.zoom * geo.sx;
      const y = geo.top + yPt * geo.zoom * geo.sy;
      const p = { x, y, button: 'left', clickCount: 1, buttons: 1 };
      await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await c.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      await sleep(30);
    };

    // 控件自己投影出来的内容框（磅）：点框内任意一点都必须归它自己。
    //   这就是用户眼里的"我点的是这个框"。
    const boxes = await c.eval(`
      const cs = window.__heditor.controls;
      return cs.items.map((it, i) => (it.rects || []).map(q => ({ i, x: q.x, w: q.w, y: q.y })))
        .flat();`);
    let probed = 0;
    const wrong = [];
    for (const b of boxes) {
      for (let x = b.x + 1; x <= b.x + b.w - 1; x += 2) {
        await click(x, geo.y);
        const st = await c.eval('return window.__heditor.controls.caret;');
        ++probed;
        if (st !== b.i) wrong.push(`x=${Math.round(x)}→字段${st}(应${b.i})`);
      }
    }
    report.clickOwner = { probed, wrong: wrong.slice(0, 5) };
    chk.check('★★ 真实点击：点任意字段内容框内的点，归属都是它自己',
              probed > 20 && wrong.length === 0,
              `采样 ${probed} 点，错的 ${wrong.length} 个 ${wrong.slice(0, 3).join(' ')}`);

    // 落笔也跟着归属走：点中间字段的开头再打字，字必须进中间那个字段。
    await c.eval('return await window.__heditor.loadFormSample(2);');
    await c.eval('return await window.__heditor.stopBlink();');
    const midBox = await c.eval(`
      const q = window.__heditor.controls.items[1].rects[0];
      return { x: q.x, y: q.y, w: q.w, h: q.h };`);
    await click(midBox.x + 1, geo.y);
    const ownerAtStart = await c.eval('return window.__heditor.controls.caret;');
    const caretAtStart = await c.eval('return window.__heditor.caret.offset;');
    await c.eval(key('痛'));
    const v12 = await c.eval('return window.__heditor.controls;');
    report.clickOwner.afterType = v12.items.map((it) => [...it.start, ...it.end]);
    chk.check('★ 真实点击中间字段的开头 → 光标停在那里且归属它',
              ownerAtStart === 1 && caretAtStart === 7,
              `归属 ${ownerAtStart}，光标 ${caretAtStart}`);
    chk.check('★ 接着打字 → 字进中间那个字段（左字段没被撑大）',
              v12.items[0].end[1] === 7 && v12.items[1].end[1] === 10,
              v12.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));

    const au12 = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 点击 + 打字之后增量排版 == 全量重排', !!au12 && au12.diff === 0,
              au12 ? `diff ${au12.diff}` : 'null');
    const sc12 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('点击之后屏幕 == 引擎帧', sc12.screenVsEngineDiffPx === 0,
              `差 ${sc12.screenVsEngineDiffPx} px`);
  }

  chk.section('[13] 三层嵌套可编辑字段（演示样例 variant 3）');
  {
    await c.eval('return await window.__heditor.loadFormSample(3);');
    await c.eval('return await window.__heditor.stopBlink();');
    const n = await c.eval('return window.__heditor.controls;');
    report.nested = { before: n.items.map((it) => [...it.start, ...it.end]) };
    chk.check('三层嵌套：深度 0/1/2，父链 外→中→内',
              n.count === 3 && n.items.every((it) => !it.ro) && n.items[0].depth === 0 &&
                n.items[1].depth === 1 && n.items[2].depth === 2 && n.items[1].parent === 0 &&
                n.items[2].parent === 1,
              n.items.map((it) => `#${it.depth}p${it.parent}[${it.start[1]},${it.end[1]})`).join(' '));

    // 归属取最内层：在内层中间打字，三层一起变长（父子区间不许互相穿过）。
    await c.eval('return await window.__heditor.setCaret(0, 8);');
    const ownerN = await c.eval('return window.__heditor.controls.caret;');
    await c.eval(key('痛'));
    const n2 = await c.eval('return window.__heditor.controls;');
    report.nested.afterType = n2.items.map((it) => [...it.start, ...it.end]);
    chk.check('★ 光标在内层里打的字归最内层（不是外层）', ownerN === 2, `归属 ${ownerN}`);
    chk.check('★ 内层变长 → 中/外层一起变长（区间不许互相穿过）',
              n2.items[2].end[1] === 10 && n2.items[1].end[1] === 11 && n2.items[0].end[1] === 12 &&
                n2.items[2].start[1] === 7 && n2.items[1].start[1] === 6 && n2.items[0].start[1] === 5,
              n2.items.map((it) => `[${it.start[1]},${it.end[1]})`).join(' '));
    // 三层都可编辑 → 内层也必须能改（嵌套最容易被误伤的一格：内层被外层"吃掉"）
    chk.check('★ 内层可编辑（嵌套没有把内层的编辑权吃掉）',
              n2.items[2].end[1] === n2.items[2].start[1] + 3,
              `内层 = [${n2.items[2].start[1]},${n2.items[2].end[1]})，打进了 1 个字`);

    const au13 = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 嵌套样例增量排版 == 全量重排（括号占位嵌套时最容易错位）',
              !!au13 && au13.diff === 0, au13 ? `diff ${au13.diff}` : 'null');
    const sc13 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('嵌套样例：屏幕 == 引擎帧', sc13.screenVsEngineDiffPx === 0,
              `差 ${sc13.screenVsEngineDiffPx} px`);

    await c.eval('return await window.__heditor.loadFormSample(3);');
    await c.eval('return await window.__heditor.setCaret(0, 8);');
    await c.eval('await window.__heditor.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'nested-three-levels.png'));
  }

  chk.section('[14] 控件边界总覆盖：真实鼠标点选 + 真实按键（相邻 / 嵌套 / 换行 / 删除 / 添加 / 光标）');
  {
    //  这一节是"控件边界"在浏览器里的主场。判据与 [12] 同源：凡是用户用手指和
    //  键盘做出来的动作，都必须走真实事件（CDP 的 mousePressed/mouseReleased、
    //  派到真实 textarea 上的 keydown），不能调引擎 API 代替 —— 坏点往往就在
    //  "坐标换算 → 命中 → 归属 → 落笔"这条链的接缝上，而引擎 API 把接缝绕过去了。
    //
    //  括号的排布只存在于排版结果里（区间数据看不出来：][][] 与 [[ ]] 的区间
    //  一模一样），所以用 __heditor.bracketSlots 把槽清单读出来核对：每一枚括号的
    //  偏移、开闭、归属、所在行、行内 x 都在里面。
    // 换算因子读门面的 zoomLevel（密度：设备像素 / 磅）。不要读 #zoom 输入框 ——
    //   那一格是倍数（1 = 100% = 96 dpi），两者差 4 / 3，读错就是整片点不准。
    const zoomNow = async () => c.eval('return window.__heditor.zoomLevel;');
    const geo = async () => c.eval(`
      const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height };`);
    // 帧磅 → CSS 像素：与页面 onCanvasMouseDown 里那套换算同源（先乘 zoom 再乘缩放比）
    const clickPt = async (g, xPt, yPt, z) => {
      const p = { x: g.left + xPt * z * g.sx, y: g.top + yPt * z * g.sy,
                  button: 'left', clickCount: 1, buttons: 1 };
      await c.send('Input.dispatchMouseEvent', { type: 'mousePressed', ...p });
      await c.send('Input.dispatchMouseEvent', { type: 'mouseReleased', ...p });
      // 8 ms 只够同步宿主。Worker 宿主的点击是一条链（命中测试一次往返 → 落光标 →
      //   推状态），读早了拿到的是上一个点的结果 —— 而横扫是 x 逐 1 磅前进的，
      //   "上一个点的结果"恰好差一个簇，看起来就像引擎在边界上算错了一格。
      //   排队排空之后再读，两边才可比。
      await sleep(8);
      await c.eval('if (window.__heditor.quiesce) await window.__heditor.quiesce(); return true;');
    };
    const load = async (v) => {
      await c.eval(`return await window.__heditor.loadFormSample(${v});`);
      await c.eval('return await window.__heditor.stopBlink();');
    };
    const caretOf = () => c.eval(`
      const q = window.__heditor.caret;
      return { o: q.offset, s: q.side, f: window.__heditor.controls.caret };`);
    const items = () => c.eval('return window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]);');
    const slotsOf = (cp) => c.eval(`
      return (await window.__heditor.bracketSlots || [])
        .filter(s => s.para === 0 && s.cp === ${cp})
        .map(s => ({ side: s.side, owner: s.owner, ro: s.ro, line: s.line, x: s.x }));`);
    // BracketSide：1 = 开，2 = 闭（枚举在 layout.h）
    const sidesOf = (arr) => arr.map((s) => (s.side === 1 ? '[' : ']')).join('');
    // 某个控件内容框左缘内侧 2 磅处（帧磅）。那里只被这个字段自己（以及它的外层）
    //   覆盖，所以归属必须是最内层 —— 也就是这个字段自己。
    //   不用"第一个字的中点"：相邻字段共享边界时，槽带把两个字的几何挤在一起，
    //   量出来的中点可能落到里层字段的框里（判据本身就不可靠，见 [19]）。
    const fieldSpot = (ci) => c.eval(`
      const q = window.__heditor.controls.items[${ci}].rects[0];
      return { x: q.x + 2, y: q.y + q.h / 2 };`);

    // 某个控件在某偏移上的"内侧"插入点（帧磅）。
    //   caret.x / caret.w 是帧像素（乘过 zoom），除以 zoom 换成帧磅；x 是光标矩形的左缘，
    //   插入点还要再往右半个光标宽 —— 少算这半个，点选位置就会落在括号格的正中偏左。
    const innerCaretPt = async (cp, ci) => {
      const r = JSON.parse(await c.eval(`
        await window.__heditor.setCaretAt(0, ${cp}, ${ci});
        const q = window.__heditor.caret;
        return JSON.stringify({ x: q.x, w: q.w,
                                z: window.__heditor.zoomLevel });`));
      return r.x / r.z + (r.w / r.z) * 0.5;
    };
    // 点"某字段开括号的内侧"那一格：落在括号与它后面那个字之间、靠右的一小段。
    //   括号占位簇的势力范围按左右半边分 —— 左半边是"括号之前"（控件之外）、右半边是
    //   "括号之后"（控件之内）。要"点进这个字段"，落点得在右半边，所以取内侧插入点左侧
    //   半磅：那里既在右半边之内，又紧贴着字段的第一个字。
    const clickOpenInner = async (g, z, y, cp, ci) => {
      const inner = await innerCaretPt(cp, ci);
      await clickPt(g, inner - 0.5, y, z);
    };
    chk.section('[14.1] 相邻单共享边界：点 ] 归前一个、点 [ 归后一个（variant 0）');
    {
      await load(0);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s9 = await slotsOf(9);
      chk.check('边界 9 上是 ][，两枚括号各归各的字段',
                s9.length === 2 && sidesOf(s9) === '][' && s9[0].owner === 0 && s9[1].owner === 1,
                `${sidesOf(s9)} ｜ 归属 ${s9.map((s) => s.owner).join(' ')}`);
      chk.check('只读字段的括号是灰的（颜色跟着字段走，不跟着位置走）',
                s9.length === 2 && !s9[0].ro && s9[1].ro,
                `ro ${s9.map((s) => (s.ro ? 1 : 0)).join(' ')}`);
      const pitch = s9.length === 2 ? Math.max(1, s9[1].x - s9[0].x) : 6;

      await clickPt(g, s9[0].x + pitch * 0.5, y, z);
      const a = await caretOf();
      chk.check('★ 真实鼠标点 ] → 光标停在 9 且归前一个字段', a.o === 9 && a.f === 0,
                `offset ${a.o} 归属 ${a.f}`);
      await clickOpenInner(g, z, y, 9, 1);
      const b = await caretOf();
      chk.check('★ 真实鼠标点 [ → 同一个偏移 9，归属换成后一个字段（这就是点得进去）',
                b.o === 9 && b.f === 1, `offset ${b.o} 归属 ${b.f}`);

      // 只读字段里打字必须被拒，而且要记账
      const blk0 = await c.eval('return window.__heditor.blockedEdits;');
      await c.eval(key('痛'));
      const blk1 = await c.eval('return window.__heditor.blockedEdits;');
      const it0 = await items();
      chk.check('★ 在只读字段里打字被拒，且记了账',
                blk1 === blk0 + 1 && JSON.stringify(it0[1]) === '[9,11]',
                `blocked ${blk0}→${blk1} ｜ #1 ${JSON.stringify(it0[1])}`);

      // 点前一个字段的闭括号再打字：字进前一个字段
      await clickPt(g, s9[0].x + pitch * 0.5, y, z);
      await c.eval(key('痛'));
      const it1 = await items();
      chk.check('★ 点 ] 之后真实按键 → 字进前一个字段，后一个整体右移',
                JSON.stringify(it1) === '[[5,10],[10,12],[13,17]]', JSON.stringify(it1));
      const au = await c.eval('return await window.__heditor.auditLayout();');
      chk.check('★★ 点选 + 打字之后增量排版等于全量重排', !!au && au.diff === 0,
                au ? `diff ${au.diff}` : 'null');
    }

    chk.section('[14.2] 相邻双共享边界（三连）：两条边界都是 ][，落笔跟着归属（variant 2）');
    {
      await load(2);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s7 = await slotsOf(7);
      const s9 = await slotsOf(9);
      chk.check('★ 两条共享边界都是 ][', sidesOf(s7) === '][' && sidesOf(s9) === '][',
                `7:${sidesOf(s7)} 9:${sidesOf(s9)}`);
      const p7 = s7.length === 2 ? Math.max(1, s7[1].x - s7[0].x) : 6;

      await clickOpenInner(g, z, y, 7, 1);
      const a = await caretOf();
      chk.check('★ 真实鼠标点中间那个字段的开括号 → 归属 1', a.o === 7 && a.f === 1,
                `offset ${a.o} 归属 ${a.f}`);
      await c.eval(key('痛'));
      const it1 = await items();
      chk.check('★ 接着打字 → 字进中间那个字段，左邻居一个字都没变',
                JSON.stringify(it1) === '[[5,7],[7,10],[10,12]]', JSON.stringify(it1));

      await load(2);
      const s7b = await slotsOf(7);
      await clickPt(g, s7b[0].x + p7 * 0.5, y, z);
      const b = await caretOf();
      chk.check('★ 同一条边界上点 ] → 归属 0', b.o === 7 && b.f === 0,
                `offset ${b.o} 归属 ${b.f}`);
      await c.eval(key('痛'));
      const it2 = await items();
      chk.check('★ 接着打字 → 字进前一个字段，后两个整体右移',
                JSON.stringify(it2) === '[[5,8],[8,10],[10,12]]', JSON.stringify(it2));

      // 真实方向键：一次一格 —— 括号也占一格，所以共享边界 ][ 要按两下。
      await c.eval('return await window.__heditor.setCaret(0, 5);');
      const walk = [];
      for (let k = 0; k < 6; ++k) {
        await c.eval(key('ArrowRight'));
        const a = await caretOf();
        walk.push(`${a.o}/${a.s}`);
      }
      chk.check('★ 真实方向键一次一格（括号占格：][ 边界要按两下）',
                JSON.stringify(walk) === JSON.stringify(['6/0', '7/0', '8/0', '8/1', '8/2', '9/0']),
                `落点 ${walk.join(' ')}`);
      await c.eval('return await window.__heditor.setCaret(0, 12);');
      const back = [];
      for (let k = 0; k < 4; ++k) {
        await c.eval(key('ArrowLeft'));
        const a = await caretOf();
        back.push(`${a.o}/${a.s}`);
      }
      chk.check('★ 反向同样一次一格（12/0 → 11/0 → 10/2 10/1 10/0）',
                JSON.stringify(back) === JSON.stringify(['11/0', '10/2', '10/1', '10/0']),
                `落点 ${back.join(' ')}`);

      // 归属随位置换人（重新载入样例，让偏移回到干净的三连区间）
      await load(2);
      await c.eval('return await window.__heditor.setCaret(0, 7);');
      const f7 = (await caretOf()).f;
      await c.eval(key('ArrowRight'));
      const mid = await caretOf();
      await c.eval(key('ArrowRight'));
      const inB = await caretOf();
      chk.check('★ 跨过共享边界归属依次换人（#0 → 括号之间 → #1）',
                f7 === 0 && mid.s === 1 && inB.o === 7 && inB.f === 1,
                `7→${f7} ｜ 中间 ${mid.o}/${mid.s}→归 ${mid.f} ｜ 再一步→${inB.o}/${inB.f}`);

      // Home / End 是行首行尾，不是字段首尾
      const ncp = await c.eval('return window.__heditor.caret.docCodepoints;');
      await c.eval('return await window.__heditor.setCaret(0, 9);');
      await c.eval(key('Home'));
      const h = (await caretOf()).o;
      await c.eval(key('End'));
      const e2 = (await caretOf()).o;
      chk.check('★ 真实 Home/End 到行首行尾（不是字段首尾）', h === 0 && e2 === ncp,
                `Home→${h} End→${e2}（文档 ${ncp} 个码点）`);
    }

    chk.section('[14.3] 删除文字：真实退格把后两个字段删空，括号必须交替成 ][][]（variant 2）');
    {
      await load(2);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      // 用真实 Backspace 键把 #2、#1 依次删空（每删一次都用删除前的端点重新定位光标）
      for (const ci of [2, 1]) {
        for (let guard = 0; guard < 8; ++guard) {
          const cur = await items();
          if (cur[ci][0] === cur[ci][1]) break;
          await c.eval(`return await window.__heditor.setCaretAt(0, ${cur[ci][1]}, ${ci});`);
          await c.eval(key('Backspace'));
        }
      }
      const it = await items();
      chk.check('两个字段被真实退格删成零宽，仍收在 7 上',
                JSON.stringify(it) === '[[5,7],[7,7],[7,7]]', JSON.stringify(it));
      const s = await slotsOf(7);
      chk.check('★ 该处五枚括号交替成 ][][]（不是互相嵌套成 [[ ]]）',
                s.length === 5 && sidesOf(s) === '][][]', `${sidesOf(s)}（${s.length} 枚）`);
      chk.check('★ 五个槽各归各的字段（0 1 1 2 2）',
                s.map((q2) => q2.owner).join(' ') === '0 1 1 2 2',
                s.map((q2) => q2.owner).join(' '));

      // 颜色跟着字段走：把 #2 标成只读，它那一对括号必须变灰且排在右侧。
      //   只读标记只有建控件时给得进去，所以整份重建一次（区间不变，只翻 ro）。
      await c.eval(`
        const it2 = window.__heditor.controls.items;
        return await window.__heditor.setFormControls(it2.map((x, i) => ({
          start: x.start, end: x.end, kind: 0, id: x.id, ro: i === 2 })));`);
      const sr = await slotsOf(7);
      chk.check('★ 只读字段的一对括号变灰（左三枚可编辑、右两枚只读）',
                sr.length === 5 && sr.map((q2) => (q2.ro ? 1 : 0)).join('') === '00011',
                sr.map((q2) => (q2.ro ? 1 : 0)).join(''));

      // 删空之后那个零宽字段还得能进、能打：真实鼠标点它那一对 ([ ]) 里的 [ 位置
      await c.eval(key('痛'));  // 先确认当前光标（应停在 #1 的零宽位置上）
      const cur = await items();
      chk.check('★ 删空之后光标仍停在那个零宽字段上，字进它自己（左右邻居没被撑大）',
                JSON.stringify(cur) === '[[5,7],[7,8],[8,8]]', JSON.stringify(cur));

      // 真实鼠标点 #2 的开括号 → 归属 2（两个零宽字段不许被认成同一个）
      await load(2);
      for (const ci of [2, 1]) {
        for (let guard = 0; guard < 8; ++guard) {
          const k = await items();
          if (k[ci][0] === k[ci][1]) break;
          await c.eval(`return await window.__heditor.setCaretAt(0, ${k[ci][1]}, ${ci});`);
          await c.eval(key('Backspace'));
        }
      }
      const s2 = await slotsOf(7);
      // 槽序是 ] [ ] [ ]：#2 的开括号在下标 3
      await clickOpenInner(g, z, y, 7, 2);
      const at2 = await caretOf();
      chk.check('★ 真实鼠标点最右那枚 [（#2 的开括号）→ 归属 2',
                at2.o === 7 && at2.f === 2, `offset ${at2.o} 归属 ${at2.f}`);
      await c.eval(key('痛'));
      const it3 = await items();
      chk.check('★ 接着打字 → 字进 #2 自己（#1 还是零宽）',
                JSON.stringify(it3) === '[[5,7],[7,7],[7,8]]', JSON.stringify(it3));
      const au3 = await c.eval('return await window.__heditor.auditLayout();');
      chk.check('★★ 删空与回填的每一步增量排版都等于全量重排', !!au3 && au3.diff === 0,
                au3 ? `diff ${au3.diff}` : 'null');
    }

    chk.section('[14.4] 嵌套：三层各自点得进、内层优先、删空后仍是最内层（variant 3）');
    {
      await load(3);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const six = {};
      for (const cp of [5, 6, 7, 9, 10, 11]) six[cp] = await slotsOf(cp);
      chk.check('★ 六处边界各一枚括号：开在前三、闭在后三（[外[中[内 … ]内]中]外）',
                sidesOf(six[5]) === '[' && sidesOf(six[6]) === '[' && sidesOf(six[7]) === '[' &&
                sidesOf(six[9]) === ']' && sidesOf(six[10]) === ']' && sidesOf(six[11]) === ']',
                [5, 6, 7, 9, 10, 11].map((cp) => `${cp}:${sidesOf(six[cp])}`).join(' '));

      // 真实鼠标点三层各自内容框的左缘内侧 → 归属必须是最内层
      const owners = [];
      for (const ci of [0, 1, 2]) {
        const m = await fieldSpot(ci);
        await clickPt(g, m.x, m.y, z);
        owners.push((await caretOf()).f);
      }
      chk.check('★ 真实鼠标点三层各自的第一个字 → 归属 0 / 1 / 2（内层优先）',
                owners.join(' ') === '0 1 2', owners.join(' '));

      // 真实按键在内层打字：外中内三层一起变长
      await c.eval('return await window.__heditor.setCaretAt(0, 8, 2);');
      await c.eval(key('痛'));
      const it = await items();
      chk.check('★ 真实按键在内层打字 → 外中内三层一起变长（区间不许互相穿过）',
                JSON.stringify(it) === '[[5,12],[6,11],[7,10]]', JSON.stringify(it));

      // 真实退格把内层删空
      for (let guard = 0; guard < 6; ++guard) {
        const cur = await items();
        if (cur[2][0] === cur[2][1]) break;
        await c.eval(`return await window.__heditor.setCaretAt(0, ${cur[2][1]}, 2);`);
        await c.eval(key('Backspace'));
      }
      const it2 = await items();
      const s7b = await slotsOf(7);
      chk.check('★ 删空的内层画成一对相邻的 []，仍嵌在中层的边界之内',
                JSON.stringify(it2) === '[[5,9],[6,8],[7,7]]' && sidesOf(s7b) === '[]',
                `${JSON.stringify(it2)} ｜ 7:${sidesOf(s7b)}`);
      await c.eval('return await window.__heditor.setCaret(0, 7);');
      const f7 = (await caretOf()).f;
      await c.eval(key('痛'));
      const it3 = await items();
      chk.check('★ 真实按键在删空的内层上打字 → 字进它自己，外中两层一起外推',
                f7 === 2 && JSON.stringify(it3) === '[[5,10],[6,9],[7,8]]',
                `归属 ${f7} ｜ ${JSON.stringify(it3)}`);
      const au4 = await c.eval('return await window.__heditor.auditLayout();');
      chk.check('★★ 嵌套的每一步增量排版都等于全量重排', !!au4 && au4.diff === 0,
                au4 ? `diff ${au4.diff}` : 'null');

      await load(3);
      await c.eval('return await window.__heditor.setCaretAt(0, 8, 2);');
      await c.eval('await window.__heditor.present(); return true;');
      await sleep(150);
      await c.screenshot(join(OUT, 'boundary-nested.png'));
    }

    chk.section('[14.5] 换行：跨行字段的两个括号落在不同行，嵌套的只读内层点得进打不进（variant 1）');
    {
      await load(1);
      const cc = await c.eval('return window.__heditor.controls;');
      chk.check('长字段跨行（矩形块 > 1），只读小字段嵌在它里面',
                cc.items[0].rects.length >= 2 && cc.items[1].depth === 1 && cc.items[1].ro,
                `#0 块数 ${cc.items[0].rects.length} ｜ #1 depth ${cc.items[1].depth} ro ${cc.items[1].ro}`);
      const s5 = await slotsOf(5);
      const s60 = await slotsOf(60);
      chk.check('★ 跨行字段的开括号与闭括号落在不同的行上',
                s5.length === 1 && s60.length === 1 && s5[0].line !== s60[0].line,
                `开括号行 ${s5.length ? s5[0].line : '-'} ｜ 闭括号行 ${s60.length ? s60[0].line : '-'}`);

      const g = await geo();
      const z = await zoomNow();
      const mB = await fieldSpot(1);
      await clickPt(g, mB.x, mB.y, z);
      const inB = await caretOf();
      const blk0 = await c.eval('return window.__heditor.blockedEdits;');
      await c.eval(key('痛'));
      const blk1 = await c.eval('return window.__heditor.blockedEdits;');
      const itb = await items();
      chk.check('★ 点嵌套里的只读内层 → 归属是内层、打字被拒（外层可编辑也改不动它）',
                inB.f === 1 && blk1 === blk0 + 1 && JSON.stringify(itb[1]) === '[20,26]',
                `归属 ${inB.f} ｜ blocked ${blk0}→${blk1} ｜ #1 ${JSON.stringify(itb[1])}`);

      const mA = await fieldSpot(0);
      await clickPt(g, mA.x, mA.y, z);
      const inA = await caretOf();
      await c.eval(key('痛'));
      const ita = await items();
      chk.check('★ 点外层（不在内层框里）→ 归属 0，字进外层、内层整体右移',
                inA.f === 0 && JSON.stringify(ita) === '[[5,61],[21,27]]', JSON.stringify(ita));
      const au5 = await c.eval('return await window.__heditor.auditLayout();');
      chk.check('★★ 跨行字段编辑之后增量排版等于全量重排', !!au5 && au5.diff === 0,
                au5 ? `diff ${au5.diff}` : 'null');
    }

    chk.section('[14.6] 真实鼠标横扫：命中测试说什么，点击就落在什么上（含无死区）');
    {
      await load(2);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const lo = await c.eval(
        'return Math.min(...window.__heditor.controls.items.map(i => i.rects[0].x));');
      const hi = await c.eval(
        'return Math.max(...window.__heditor.controls.items.map(i => i.rects[0].x + i.rects[0].w));');
      let probed = 0;
      let mismatch = 0;
      let nonmono = 0;
      let prev = -1;
      const covered = new Set();
      const bad = [];
      // 关键：CDP 把鼠标坐标按整型 CSS 像素投递，而且是截断不是四舍五入
      //   （实测 465.85 落到 465）—— 这一点必须与模型一致：用 round 建模时，
      //   计划点 505.85 → 506 → 140.00 磅，而实际投递 505 → 139.33 磅，
      //   差一个像素，簇边界上正好翻一格（症状：命中说 6、实际落 5，而且
      //   偏移是逐 1 磅前进的，看着像引擎在边界上算错）。
      //   所以命中要用"实际投递的那个磅值"去问，x 与 y 都要折算。
      const quantizedPt = (xPt) => {
        const cx = Math.trunc(g.left + xPt * z * g.sx);
        return (cx - g.left) / z;
      };
      // y 也要同样折算。上面那条注释只提了 x，但 CDP 把 clientY 一并取整 ——
      //   1 磅 = 1.5 像素 ⇒ y 也能差半个磅。只对 x 建模时，命中的是"计划点"，
      //   而点击实际落在"投递点"，簇边界上两者会差一格（症状：横扫里几个点
      //   命中说 6、实际落 5，而偏移是逐 1 磅前进的，看着像引擎在边界上算错）。
      const quantizedY = () => {
        const cy = Math.trunc(g.top + y * z * g.sy);
        return (cy - g.top) / z;
      };
      const yq = quantizedY();
      for (let x = lo - 4; x <= hi + 4; x += 1) {
        const pt = quantizedPt(x);
        const h = await c.eval(`return await window.__heditor.hit(${pt.toFixed(3)}, ${yq.toFixed(3)});`);
        if (!h || h.offset > 17) continue;
        await clickPt(g, x, y, z);
        const st = await caretOf();
        ++probed;
        // 归属只在命中确实给出了"某一枚括号的主"时才比：命中返回 -1 表示没点在括号上，
        //   那时面板上的归属是"这个偏移该往哪写"（末尾算在区间内），口径不同不是矛盾。
        const ownerOk = h.field < 0 || st.f === h.field;
        if (st.o !== h.offset || !ownerOk) {
          ++mismatch;
          if (bad.length < 3) {
            bad.push(`x=${x.toFixed(1)} 命中(${h.offset},${h.field}) 实际(${st.o},${st.f})`);
          }
        }
        if (st.o < prev) ++nonmono;
        prev = st.o;
        if (st.o >= 5 && st.o <= 11) covered.add(st.o);
      }
      report.sweep = { probed, mismatch, nonmono, covered: [...covered] };
      chk.check('★★ 真实鼠标横扫：命中偏移与归属都等于点击后的实际落点',
                probed > 50 && mismatch === 0,
                `采样 ${probed} 点，不一致 ${mismatch} 个 ${bad.join(' ')}`);
      chk.check('★ 横扫时落点单调不减（没有回跳的死区）', nonmono === 0, `非单调 ${nonmono} 处`);
      chk.check('★ 三个字段里每一个光标位置都能点出来（5..11 全覆盖）',
                covered.size === 7,
                `覆盖 ${[...covered].sort((a, b) => a - b).join(' ')}`);

      // 光标画在哪里：逐字点"这个字的中点"，画出来的光标必须就在点击点附近。
      //   这是整条坐标链（CSS 像素 → 帧像素 → 帧磅）的可证伪判据：dpr 或 zoom
      //   少乘一次，落点就会差一整格。
      //
      // 中点量出来的是帧像素（caret.x / caret.h 乘过 zoom），clickPt 收的是帧磅，
      //   所以要先除以 z 再交出去；少这一步只在 zoom 为 1 时看不出（恒等）。
      // "偏得远不远"的尺子要用实测字格宽，不能写死像素值：点的是字格中点，画出
      //   来的光标只能落在这一格的两端之一，上限就是半个字宽（跨到隔壁字才叫偏）。
      //   写死的像素值只在某个特定缩放下等于半个字宽，缩放一变就不是那个意思了。
      let drawnProbed = 0;
      let drawnBad = 0;
      const drawnWorst = [];
      for (let ci = 0; ci < 3; ++ci) {
        const a = await c.eval(`return window.__heditor.controls.items[${ci}].start[1];`);
        const b = await c.eval(`return window.__heditor.controls.items[${ci}].end[1];`);
        for (let k = a; k < b; ++k) {
          const m = await c.eval(`
            await window.__heditor.setCaretAt(0, ${k}, ${ci});
            const c1 = window.__heditor.caret;
            await window.__heditor.setCaretAt(0, ${k + 1}, ${ci});
            const c2 = window.__heditor.caret;
            return { x: (c1.x + c2.x) / 2, y: c2.y + c2.h / 2, w: Math.abs(c2.x - c1.x) };`);
          await clickPt(g, m.x / z, m.y / z, z);
          const st = await c.eval('return window.__heditor.caret;');
          ++drawnProbed;
          const off = Math.abs(st.x - m.x);
          if (off > m.w / 2 + 2 || st.offset < a || st.offset > b) {
            ++drawnBad;
            if (drawnWorst.length < 3) {
              drawnWorst.push(`字段${ci} 偏移${k}: 偏 ${off.toFixed(1)}px` +
                              `（半个字宽 ${(m.w / 2).toFixed(1)}）落 ${st.offset}`);
            }
          }
        }
      }
      chk.check('★ 逐字点选：落点在该字范围内、且光标画在点击处附近（整条坐标链）',
                drawnProbed === 6 && drawnBad === 0,
                `${drawnProbed} 个字，偏的 ${drawnBad} 个 ${drawnWorst.join(' ')}`);

      const sc = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('横扫之后屏幕 == 引擎帧', sc.screenVsEngineDiffPx === 0,
                `差 ${sc.screenVsEngineDiffPx} px`);
      await c.eval('await window.__heditor.present(); return true;');
      await sleep(150);
      await c.screenshot(join(OUT, 'boundary-three-adjacent.png'));
    }

    chk.section('[14.7] 括号外侧那一段也点得进去：开括号之前 / 闭括号之后（variant 3）');
    {
      // 同一个偏移在控件边界上对应两个位置（flow.h：控件之前与进入控件），而括号占位簇
      //   的势力范围按左右半边分。判据落在可证伪的数字上：点左半边，光标必须画在括号【之外】。
      await load(3);
      const y = await c.eval(`
        const q = window.__heditor.controls.items[0].rects[0];
        return q.y + q.h / 2;`);
      const g = await geo();
      const z = await zoomNow();
      const s6 = await slotsOf(6);
      const s10 = await slotsOf(10);

      await clickPt(g, s6[0].x + 0.5, y, z);  // 中层开括号那一格的左半边
      const a = await caretOf();
      const ax = await c.eval('return window.__heditor.caret.x;');
      chk.check('★★ 点开括号的左半边 → 光标落在括号之前（不是被贴回括号之内）',
                a.o === 6 && ax / z < s6[0].x + 1.0,
                `偏移 ${a.o} ｜ 光标 x=${(ax / z).toFixed(2)} ｜ 括号左缘 ${s6[0].x.toFixed(2)}`);

      const afterPt = await innerCaretPt(10, -2);  // 闭括号之后那个插入点（＝天 的左缘）
      // 落在它左边半磅：那里既是闭括号那一格的右半边（"括号之后"），也紧贴天 的左半边。
      await clickPt(g, afterPt - 0.5, y, z);
      const b = await caretOf();
      const bx = await c.eval('return window.__heditor.caret.x;');
      chk.check('★★ 点闭括号的右半边 → 光标落在括号之后（字与闭括号之间）',
                b.o === 10 && bx / z > s10[0].x + 1.0,
                `偏移 ${b.o} ｜ 光标 x=${(bx / z).toFixed(2)} ｜ 括号左缘 ${s10[0].x.toFixed(2)}`);
      const sc7 = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('点括号外侧之后屏幕 == 引擎帧', sc7.screenVsEngineDiffPx === 0,
                `差 ${sc7.screenVsEngineDiffPx} px`);
    }

    chk.section('[14.8] 方向键在括号边界上一格一格走（嵌套样例：反字前 / 天字前）');
    {
      // 报过的两条现象：光标在「反」前面按左键会一步跳到「冒号」前；在「天」前面按左键
      //   会一步跳到「悸」后面 —— 都是"整段跳过括号那一格"。
      // 判据是像素上的位移：一格 = 一枚括号的宽度（约 5.5pt），跳一整段 = 一个字 + 括号。
      await load(3);
      await c.eval('return await window.__heditor.stopBlink();');
      const z = await zoomNow();

      // 先看开括号那一侧：外层 [ 之后（＝反字前面）→ 按一下左键，只该退到 [ 之前
      await c.eval('return await window.__heditor.setCaretAtSide(0, 5, 1);');
      const x1 = (await c.eval('return window.__heditor.caret.x;')) / z;
      await c.eval(key('ArrowLeft'));
      const a1 = await caretOf();
      const x0 = (await c.eval('return window.__heditor.caret.x;')) / z;
      chk.check('★★ 反字前按左键 → 只退一格到左括号之前（不是一步跳到冒号前）',
                a1.o === 5 && a1.s === 0 && (x1 - x0) > 2 && (x1 - x0) < 12,
                `偏移 ${a1.o} 侧别 ${a1.s} ｜ 光标 x ${x1.toFixed(2)} → ${x0.toFixed(2)}` +
                  `（退 ${(x1 - x0).toFixed(2)} pt）`);

      // 闭括号那一侧同样：中层 ] 之后（＝天字前面）→ 按一下左键，只该退到 ] 之前
      await c.eval('return await window.__heditor.setCaretAtSide(0, 10, 1);');
      const y1 = (await c.eval('return window.__heditor.caret.x;')) / z;
      await c.eval(key('ArrowLeft'));
      const a2 = await caretOf();
      const y0 = (await c.eval('return window.__heditor.caret.x;')) / z;
      chk.check('★★ 天字前按左键 → 只退一格到闭括号之前（不是一步跳到悸字后面）',
                a2.o === 10 && a2.s === 0 && (y1 - y0) > 2 && (y1 - y0) < 12,
                `偏移 ${a2.o} 侧别 ${a2.s} ｜ 光标 x ${y1.toFixed(2)} → ${y0.toFixed(2)}` +
                  `（退 ${(y1 - y0).toFixed(2)} pt）`);

      // 再按一下才跨过「三」那一格（一次一格，不跳）
      await c.eval(key('ArrowLeft'));
      const a3 = await caretOf();
      chk.check('★ 再按一下才退到「三」之前（一次一格，不跳格）', a3.o === 9,
                `偏移 ${a3.o} 侧别 ${a3.s}`);

      // 反向：从「三」前按右键，依次是 三 之前 → 三 之后（＝] 之前）→ ] 之后
      const seq = [];
      for (let i = 0; i < 3; ++i) {
        await c.eval(key('ArrowRight'));
        const r = await caretOf();
        seq.push(`${r.o}/${r.s}`);
      }
      chk.check('★ 反向同样一格一格（9/2 → 10/0 10/1 11/0）',
                JSON.stringify(seq) === JSON.stringify(['10/0', '10/1', '11/0']),
                `落点 ${seq.join(' ')}`);
      await c.screenshot(join(OUT, 'arrow-boundary-nested.png'));
    }

    chk.section('[14.9] 删除之后的落点：一次只退一个字，不跨括号（嵌套 / 三连）');
    {
      // 两条现象：嵌套删「天」之后光标落到 ]中 之前（该在它之后，＝被删那个字所在的位置）；
      //   三连删「悸」之后光标跑进第三个控件（该停在第一个字段末尾）。
      // 判据落在 (偏移, 侧别) 上 —— 侧别就是"这个偏移上跨过了几枚括号"，是落点的完整身份；
      //   再加一条像素证据：光标要落在 ]中 之后，不是它之前。
      await load(3);
      await c.eval('return await window.__heditor.setCaret(0, 11);');
      await c.eval(key('Backspace'));
      const a = await caretOf();
      const s10 = await slotsOf(10);
      const z9 = await zoomNow();
      const ax = (await c.eval('return window.__heditor.caret.x;')) / z9;
      chk.check('★★ 嵌套删「天」→ 落点＝「天」原来的位置（cp 10 跨过 ]中，侧别 1）',
                a.o === 10 && a.s === 1 && s10.length === 2 && ax > s10[0].x + 1,
                `落点 ${a.o}/${a.s} ｜ 光标 x=${ax.toFixed(2)} ｜ cp10 两枚 ] 的 x=` +
                  `${s10.map((q) => q.x.toFixed(2)).join(' / ')}`);

      await load(2);
      // 删控件是结构操作：表单模式默认被 structureLock 锁住（[16](c) 是它的对照），
      //   这一段要验"空控件被删掉"，所以切文档模式。
      await c.eval('return await window.__heditor.setEditMode(0);');
      await c.eval('return await window.__heditor.setCaret(0, 11);');
      for (let i = 0; i < 3; ++i) {
        await c.eval(key('Backspace'));
        await sleep(20);
      }
      const b0 = await caretOf();
      const it0 = await items();
      chk.check('★★ 三连：第三个字段删空之后再退一格，删掉的是这个空控件（落点＝它原来那一格）',
                b0.o === 9 && b0.s === 1 && JSON.stringify(it0) === '[[5,7],[7,9]]',
                `落点 ${b0.o}/${b0.s} ｜ ${JSON.stringify(it0)}`);
      await c.eval(key('Backspace'));
      await sleep(20);
      const b = await caretOf();
      chk.check('★★ 再退一格删「悸」→ 落点＝第一个字段末尾（cp 8 侧别 0），不许进别的控件',
                b.o === 8 && b.s === 0, `落点 ${b.o}/${b.s}`);
      await c.eval(key('痛'));
      const it = await items();
      chk.check('★ 接着打字进的是第二个字段（不是别的控件）',
                JSON.stringify(it) === '[[5,7],[7,9]]', JSON.stringify(it));
      await c.screenshot(join(OUT, 'delete-caret-landing.png'));
    }

    chk.section('[14.10] 在字段末尾回车（字段因此跨段）之后再退格：整段与控件都不许丢');
    {
      // 现象：三连可编辑字段（variant 2）里把光标放到「天」后面回车，再把光标移到
      //   「悸」后面退格 —— 结果「患者主诉：反复心悸」九个字与三个控件整个消失，只剩一行
      //   「三天」。根因不在删除：回车把段落标记插进了控件内部（字段因此跨段，这是正常形态，
      //   见 compile.h 的 BlockSource），而"只重编译被改的那一段"是按标记的父容器取子节点
      //   区间的 —— 控件里的半截被当成整段编译，前半段的字与控件边界一起丢掉，block_src_ /
      //   block_flow_ 也被覆盖，控件集合随之空掉。
      // 判据三层：文本（只少了「悸」）、控件区间（三条都还在，跨段那条的终点落在第二段）、
      //   以及屏幕 == 引擎帧 —— 半截产物会当场把括号画错。
      await load(2);
      await c.eval('return await window.__heditor.setCaret(0, 11);');
      await c.eval(key('Enter'));
      await sleep(30);
      const p1 = Array.from(await c.eval('return window.__heditor.allText;')).join('');
      chk.check('回车在字段内容里分段：两段的文本都对（字段因此跨段）',
                p1 === '患者主诉：反复心悸三天\n，加重一天。', JSON.stringify(p1));
      await c.eval('return await window.__heditor.setCaret(0, 9);');
      await c.eval(key('Backspace'));
      await sleep(30);
      const p2 = Array.from(await c.eval('return window.__heditor.allText;')).join('');
      chk.check('★★ 退格只删掉「悸」一个字（曾经把前面九个字与所有控件整批抹掉）',
                p2 === '患者主诉：反复心三天\n，加重一天。', JSON.stringify(p2));
      const it2 = await c.eval(`
        return JSON.stringify(window.__heditor.controls.items
          .map(i => [i.start[0], i.start[1], i.end[0], i.end[1]]));`);
      chk.check('★★ 三个控件都还在，且跨段那条的终点落在第二段第 0 位',
                it2 === '[[0,5,0,7],[0,7,0,8],[0,8,1,0]]', it2);
      const au = await c.eval('return await window.__heditor.auditCompile();');
      chk.check('★ 产物与整树全量编译逐字段一致（diff 0）', !!au && au.diff === 0,
                au ? `diff ${au.diff}` : 'null');
      const sc = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('★ 屏幕 == 引擎帧（半截产物会把括号画丢/画错）',
                sc.screenVsEngineDiffPx === 0, `差 ${sc.screenVsEngineDiffPx} px`);
      await c.screenshot(join(OUT, 'ctl-split-then-backspace.png'));
    }

    chk.section('[14.11] 两次回车都落进字段内部之后再退格：段首那两枚边界不许丢');
    {
      // 现象：三连字段里「天」后回车、再「悸」后回车，然后把光标放到「天」后面退格 ——
      //   第二段段首的两枚边界（第一个字段的闭括号、第二个字段的开括号）成对消失，
      //   控件从 3 个掉到 1 个。根因见 edit_test [32]：段首第一枚内容是闭括号时，编译层
      //   没把它记进这段的来源身份，而增量重编译的形状判据正是靠那个身份判的。
      // 判据三层：文本与三条区间、产物与整树全量一致、屏幕 == 引擎帧。
      await load(2);
      await c.eval('return await window.__heditor.setCaret(0, 11);');
      await c.eval(key('Enter'));
      await sleep(30);
      await c.eval('return await window.__heditor.setCaret(0, 9);');
      await c.eval(key('Enter'));
      await sleep(30);
      await c.eval('return await window.__heditor.setCaret(1, 2);');
      await c.eval(key('Backspace'));
      await sleep(30);
      const p3 = Array.from(await c.eval('return window.__heditor.allText;')).join('');
      chk.check('★★ 退格只删掉「天」一个字（曾经把第二段的控件边界整对抹掉）',
                p3 === '患者主诉：反复心悸\n三\n，加重一天。', JSON.stringify(p3));
      const it3 = await c.eval(`
        return JSON.stringify(window.__heditor.controls.items
          .map(i => [i.start[0], i.start[1], i.end[0], i.end[1]]));`);
      chk.check('★★ 三个控件都还在，后两条各自跨到下一段段首',
                it3 === '[[0,5,0,7],[0,7,1,0],[1,0,2,0]]', it3);
      const au3 = await c.eval('return await window.__heditor.auditCompile();');
      chk.check('★ 产物与整树全量编译逐字段一致（diff 0）', !!au3 && au3.diff === 0,
                au3 ? `diff ${au3.diff}` : 'null');
      const sc3 = await c.eval('return await window.__heditor.selfCheck();');
      chk.check('★ 屏幕 == 引擎帧', sc3.screenVsEngineDiffPx === 0,
                `差 ${sc3.screenVsEngineDiffPx} px`);
      await c.screenshot(join(OUT, 'ctl-split-twice-then-backspace.png'));
    }
  }
  chk.section('[15] 文档模式也画控件边界（方括号、与表单一致），只读才不画不占位');
  {
    //  用户的原话："编辑模式下不显示控件边界了，我都不知道哪块是控件"。
    //  口径（引擎的 bracketedControls / bracketPlanId）：文档模式与表单模式都占位、都画；
    //  只读模式是呈现与打印态，一个都不占 —— 占位会把正文凭空推开一截，而它又不画。
    //  判据落在占位槽清单上：占位与不占位的区别是"正文有没有被推开"，画的都是同一对括号，
    //  截图分不出来。槽清单里每枚括号的偏移、开闭、归属、所在行都在，这是唯一看得见它的地方。
    const load15 = async (v) => {
      await c.eval(`return await window.__heditor.loadFormSample(${v});`);
      await c.eval('return await window.__heditor.stopBlink();');
    };
    const slots15 = () => c.eval('return (await window.__heditor.bracketSlots || []).length;');
    const text15 = () => c.eval('return Array.from(await window.__heditor.allText).join("");');

    await load15(0);
    await c.eval('return await window.__heditor.setShowBrackets(true);');  // 显式打开括号
    const cnt15 = await c.eval('return window.__heditor.controls.count;');
    const form15 = await slots15();
    // 每个字段自己的那一对：一枚开、一枚闭，归属不许串台（串台就是几何投影错位）
    const owned15 = await c.eval(`
      const m = {};
      for (const s of (await window.__heditor.bracketSlots || [])) {
        m[s.owner] = m[s.owner] || { open: 0, close: 0 };
        if (s.side === 1) m[s.owner].open++; else m[s.owner].close++;
      }
      return m;`);
    const ownedOk = Object.keys(owned15).length === cnt15 &&
      Object.values(owned15).every((v) => v.open === 1 && v.close === 1);
    report.docBrackets = { count: cnt15, form: form15, owned: owned15 };

    chk.check('前置：表单模式下每个字段两枚括号',
              form15 === cnt15 * 2, `${form15} 槽 / ${cnt15} 字段`);
    chk.check('表单模式：每个字段各一枚开括号 + 一枚闭括号（归属不串台）', ownedOk,
              JSON.stringify(owned15));

    const tForm15 = await text15();
    await c.eval('return await window.__heditor.setEditMode(0);');  // 文档模式
    const doc15 = await slots15();
    report.docBrackets.doc = doc15;
    chk.check('★ 文档模式：括号照常占位（不再"编辑模式下什么都看不见"）',
              doc15 === cnt15 * 2, `${doc15} 槽 / ${cnt15} 字段`);
    chk.check('★ 文档模式：正文一个字都没变（括号是占位簇，不是插进去的字符）',
              (await text15()) === tForm15, JSON.stringify(await text15()));

    // All 档下括号常驻：光标跨字段移动不改变占位集合，也不该重排（这是 All 档的意义）
    await c.eval('return await window.__heditor.setCaret(0, 6);');
    chk.check('文档模式：光标跨字段移动不改变占位集合（All 档括号常驻）',
              (await slots15()) === cnt15 * 2, `${await slots15()} 槽`);

    await c.eval('return await window.__heditor.setEditMode(2);');  // 只读
    const ro15 = await slots15();
    chk.check('★ 只读模式：一个括号都不占、也不画（它是要印出来的呈现态）',
              ro15 === 0, `${ro15} 槽`);
    chk.check('只读模式：正文也没变（"不占位"不等于"删了字"）',
              (await text15()) === tForm15, JSON.stringify(await text15()));

    await c.eval('return await window.__heditor.setEditMode(0);');
    const sc15 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('★ 三档来回切之后屏幕 == 引擎帧（标记改了、底图没改，靠整帧重贴收口）',
              sc15.screenVsEngineDiffPx === 0, `差 ${sc15.screenVsEngineDiffPx} px`);
    await c.eval('await window.__heditor.present(); return true;');
    await sleep(150);
    await c.screenshot(join(OUT, 'brackets-in-document-mode.png'));
  }

  chk.section('[16] 控件边界上的退格（真实按键）+ 引擎命令');
  {
    //  四条规则（见 engine.cpp 的 bracketBackspace / bracketDeleteForward）。
    //  三连可编辑字段（variant 2）：part1=[5,7) 反复、part2=[7,9) 心悸、part3=[9,11) 三天。
    //  字段末尾那个码点偏移上压着两格 —— 侧别 0 还在内容里（退的是字），侧别 1 已经跨过
    //  闭括号（退的是控件内容里那一项）。任何"按码点偏移猜"的实现都过不了下面 (a)/(b) 这一对。
    //  结构操作要文档模式：表单模式默认被 structureLock 锁住，(c) 那一条正是它的对照。
    const load16 = async (v) => {
      await c.eval(`return await window.__heditor.loadFormSample(${v});`);
      await c.eval('return await window.__heditor.stopBlink();');
    };
    const text16 = () => c.eval('return Array.from(await window.__heditor.allText).join("");');
    const ids16 = () => c.eval('return window.__heditor.controls.items.map(i => i.id);');
    const n16 = () => c.eval('return window.__heditor.controls.count;');
    const blocked16 = () => c.eval('return window.__heditor.blockedEdits;');
    const side16 = () => c.eval('return window.__heditor.caret.side;');

    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    const base16 = { t: await text16(), ids: await ids16() };
    report.removeControl = { base: base16 };
    chk.check('前置：三连可编辑字段 part1 / part2 / part3',
              JSON.stringify(base16.ids) === '["part1","part2","part3"]',
              JSON.stringify(base16.ids));

    // (a) 对照组：侧别 0 —— 还在 part2 的内容里，退格删的是「悸」这个字
    await c.eval('return await window.__heditor.setCaretAtSide(0, 9, 0);');
    chk.check('前置：侧别 0 是 part2 的内容末尾', (await side16()) === 0, `侧别 ${await side16()}`);
    await c.eval(key('Backspace'));
    chk.check('★ 侧别 0（内容末尾）退格删的是「悸」，控件一个都没少',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复心三天，加重一天。',
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);

    // (b) 侧别 1 —— 已经跨过 part2 的闭括号：按规则先进 part2 删掉它内容里的最后一项，
    //   控件本身要等它空了才删得掉（"删一个字符或空控件"）。
    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    await c.eval('return await window.__heditor.setCaretAtSide(0, 9, 1);');
    chk.check('前置：侧别 1 是 part2 的闭括号之后', (await side16()) === 1, `侧别 ${await side16()}`);
    await c.eval(key('Backspace'));
    report.removeControl.afterBackspace = { ids: await ids16(), t: await text16() };
    chk.check('★★ 闭括号之后退格：进 part2 删掉「悸」，控件一个都没少',
              JSON.stringify(await ids16()) === '["part1","part2","part3"]' &&
                (await text16()) === '患者主诉：反复心三天，加重一天。',
              `${JSON.stringify(await ids16())} ｜ ${JSON.stringify(await text16())}`);
    chk.check('★ 光标进到了 part2 里（内容末尾，侧别 0）', (await side16()) === 0,
              `侧别 ${await side16()}`);
    const lv16 = await c.eval('return await window.__heditor.auditLayout();');
    chk.check('★★ 删字之后增量排版 == 全量重排', !!lv16 && lv16.diff === 0,
              lv16 ? `diff ${lv16.diff}` : 'null');
    const sc16 = await c.eval('return await window.__heditor.selfCheck();');
    chk.check('★ 删字之后屏幕 == 引擎帧（括号与占位都得跟着走）',
              sc16.screenVsEngineDiffPx === 0, `差 ${sc16.screenVsEngineDiffPx} px`);

    // part2 的内容删光（两下）之后光标停在它内部 ⇒ 第三下删掉的是这个空控件本身
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    report.removeControl.afterBackspace2 = { ids: await ids16(), t: await text16() };
    chk.check('★★ 空控件上再退一格：删掉的是 part2 这个控件（不是字）',
              JSON.stringify(await ids16()) === '["part1","part3"]', JSON.stringify(await ids16()));
    // 区间不许被撑大：part2 整个消失之后 part1 / part3 各守自己的位置。
    //   单列这一条是因为"删掉一个字段、前一个字段把它吞了"也能让上面那条全绿，
    //   视觉上就是区间被撑大，不量区间看不出来。
    const rng16 = await c.eval('return window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]);');
    report.removeControl.ranges = rng16;
    chk.check('★★ 剩下两个字段的区间仍是 [5,7) 与 [7,9)（前一个字段没把它吞进去）',
              JSON.stringify(rng16) === '[[5,7],[7,9]]', JSON.stringify(rng16));
    chk.check('★★ 文本只剩 part1 与 part3 的内容（控件里本来就空了）',
              (await text16()) === '患者主诉：反复三天，加重一天。', JSON.stringify(await text16()));
    await c.screenshot(join(OUT, 'ctl-remove-by-backspace.png'));

    // 撤销要把控件整棵装回去，而且光标回到按键之前那一格 —— 不是"选中整个控件"
    //   （那样撤销之后光标会落到闭括号右边，屏幕上还留着一圈选区）。
    chk.check('★★ 撤销把 part2 装回来（空的、还在原来那一格），光标归位且不留选区',
              (await c.eval('return await window.__heditor.undo() ? 1 : 0;')) === 1 &&
                JSON.stringify(await ids16()) === '["part1","part2","part3"]' &&
                (await text16()) === '患者主诉：反复三天，加重一天。' &&
                (await c.eval('return window.__heditor.selection.empty ? 1 : 0;')) === 1 &&
                JSON.stringify(await c.eval(
                  'return window.__heditor.controls.items.map(i => [i.start[1], i.end[1]]);')) ===
                  '[[5,7],[7,7],[7,9]]',
              JSON.stringify(await ids16()) + ' ｜ 侧别 ' + (await side16()));

    // (c) 表单模式：结构被 structureLock 锁住 —— 删字那一下照样生效，删控件那一下被拒
    await load16(2);
    const b16 = await blocked16();
    await c.eval('return await window.__heditor.setCaretAtSide(0, 9, 1);');
    await c.eval(key('Backspace'));
    chk.check('★ 表单模式：闭括号之后退格照样进 part2 删掉「悸」（删的是内容，不是结构）',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复心三天，加重一天。' &&
                (await blocked16()) === b16,
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);
    await c.eval(key('Backspace'));
    await c.eval(key('Backspace'));
    chk.check('★ 空控件上那一下被结构锁拒了：控件还在、按键没被吞掉（光标退了出去）',
              (await n16()) === 3 && (await text16()) === '患者主诉：反复三天，加重一天。',
              `${await n16()} 个字段 ｜ ${JSON.stringify(await text16())}`);
    chk.check('★ 那次被拒的结构操作记了账', (await blocked16()) === b16 + 1,
              `blockedEdits ${b16} → ${await blocked16()}`);

    // (d) 引擎命令：连内容一起删（keep_content = false）
    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    const rm16 = await c.eval('return await window.__heditor.removeControl(1, false) ? 1 : 0;');
    chk.check('★ 引擎命令 removeControl(1,false)：连内容一起删',
              rm16 === 1 && JSON.stringify(await ids16()) === '["part1","part3"]' &&
                (await text16()) === '患者主诉：反复三天，加重一天。',
              `${JSON.stringify(await ids16())} ｜ ${JSON.stringify(await text16())}`);
    chk.check('★ 撤销把它整棵装回来',
              (await c.eval('return await window.__heditor.undo() ? 1 : 0;')) === 1 &&
                JSON.stringify(await ids16()) === JSON.stringify(base16.ids) &&
                (await text16()) === base16.t,
              JSON.stringify(await ids16()));

    // (e) 锁是逐控件的：表单模式里给 part2 开锁之后它删得掉，没开锁的 part1 仍被拒
    await load16(2);
    const unlock16 = await c.eval('return await window.__heditor.setControlProp(1, 3, false) ? 1 : 0;');
    const rmLocked16 = await c.eval('return await window.__heditor.removeControl(0, true) ? 1 : 0;');
    const rmOpen16 = await c.eval('return await window.__heditor.removeControl(1, true) ? 1 : 0;');
    chk.check('★ 表单模式：开锁的那个删得掉、没开锁的仍被拒（锁挂在控件自己身上）',
              unlock16 === 1 && rmLocked16 === 0 && rmOpen16 === 1 &&
                JSON.stringify(await ids16()) === '["part1","part3"]',
              `开锁 ${unlock16} 删被锁的 ${rmLocked16} 删已开锁的 ${rmOpen16} ｜ ` +
                JSON.stringify(await ids16()));

    // (f) 属性入口幂等：拿同一个值再设一次，撤销栈上不留记录
    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    await c.eval('return await window.__heditor.setControlProp(0, 3, false);');  // 锁 → 开：记一条
    const before16 = await text16();
    await c.eval('return await window.__heditor.setControlProp(0, 3, false);');  // 再设一次：不记
    chk.check('★ 属性入口幂等：同一个值设两次只留一条撤销记录',
              (await c.eval('return await window.__heditor.undo() ? 1 : 0;')) === 1 &&
                (await c.eval('return await window.__heditor.undo() ? 1 : 0;')) === 0 &&
                (await text16()) === before16,
              JSON.stringify(await text16()));

    // (g) 默认键位：Ctrl+Z 撤销、Ctrl+Y（与 Ctrl+Shift+Z）重做。
    //   上面几条走的是宿主门面 undo() —— 那是同一件事的另一条路。键位这条路要真的
    //   派按键才算验过，而且它多担一条："该吞的吞"。页面没有可撤销的 DOM 编辑
    //   （键盘的承接层是个只读 textarea），漏给浏览器只会让 Ctrl+Z 表现成"按了没反应"。
    //   这也是 [2] 段那条"Ctrl/Alt/Meta 一律不吞"的例外，例外必须写进判据里。
    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    const L16 = (await text16()).length;
    await c.eval(`return await window.__heditor.setCaret(0, ${L16});`);
    await c.eval(key('X'));
    await c.eval(key('Y'));
    const typed16 = await text16();
    chk.check('前置：真实按键敲进两个字（各是一笔事务）',
              typed16 === base16.t + 'XY', JSON.stringify(typed16));

    const ctrl = (k, extra = '') => c.eval(`
      const e = new KeyboardEvent('keydown',
        { key: ${JSON.stringify(k)}, ${extra}bubbles: true, cancelable: true });
      return window.__heditor.textarea.dispatchEvent(e);   // false = 被 preventDefault
    `);
    // 摆前置状态一律用门面 undo()，不借上一条键位判据的结果 —— 判据要各自承重：
    //   若靠上一条把状态摆好，键位整段没绑时"重做回原样"照样是通过的。
    const undoFacade = () => c.eval('return await window.__heditor.undo() ? 1 : 0;');

    // 撤销粒度 = 一笔编辑一步，不做合并：敲进去两个字符就要撤两次。
    chk.check('★ Ctrl+Z 被宿主吞掉（不吞 = 这个键位等于没绑）',
              (await ctrl('z', 'ctrlKey: true, ')) === false);
    chk.check('★ Ctrl+Z 一次只退掉最后那个字符（撤销不做合并）',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    await ctrl('z', 'ctrlKey: true, ');
    chk.check('★ 再撤一次，回到载入时的文本（栈空）',
              (await text16()) === base16.t, JSON.stringify(await text16()));

    // 重做同样一步一个字。此时状态是 base、重做栈里有两项，所以这一对判据各自承重。
    await ctrl('y', 'ctrlKey: true, ');
    chk.check('★ Ctrl+Y 重做一步：退掉的字回来一个',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    await ctrl('y', 'ctrlKey: true, ');
    chk.check('★ 再重做一次：两个字都回来了', (await text16()) === typed16,
              JSON.stringify(await text16()));

    // Ctrl+Shift+Z 也是重做。真键盘在 shift 按下时给的是大写 'Z' ——
    //   判据必须走这一格，否则"只在 key === 'z' 上判"的实现会漏掉它。
    chk.check('前置：门面撤销一次（文本退到 base + 一个字）',
              (await undoFacade()) === 1 && (await text16()) === base16.t + 'X',
              JSON.stringify(await text16()));
    await ctrl('Z', 'ctrlKey: true, shiftKey: true, ');
    chk.check('★ Ctrl+Shift+Z 也当重做（大写 Z 同样要认）',
              (await text16()) === typed16, JSON.stringify(await text16()));

    // 撤到底：栈空之后再一次 Ctrl+Z 不许改文档，也不许被当成"没处理"漏给浏览器
    //   —— 那条路落到 resetBlink()，吞掉动作在调引擎之前就做完了。
    chk.check('前置：门面撤销两次，栈空、文本回到载入时',
              (await undoFacade()) === 1 && (await undoFacade()) === 1 &&
                (await text16()) === base16.t,
              JSON.stringify(await text16()));
    chk.check('★ 栈空时 Ctrl+Z 仍然被吞、且文档一个字不改',
              (await ctrl('z', 'ctrlKey: true, ')) === false && (await text16()) === base16.t,
              JSON.stringify(await text16()));

    // (h) 撤销上限：宿主的门面把它一路传到引擎（默认 100，与 Office 系和 Quill 的
    //   maxStack 同值）。这条走的是 setUndoLimit → wasm → EngineConfig → UndoStack 一整条线，
    //   任何一环断了这里都会露出来。
    chk.check('默认撤销上限 100（Office 系与 Quill 的 maxStack 同值）',
              (await c.eval('return await window.__heditor.undoLimit();')) === 100,
              String(await c.eval('return await window.__heditor.undoLimit();')));

    await load16(2);
    await c.eval('return await window.__heditor.setEditMode(0);');
    await c.eval('return await window.__heditor.setUndoLimit(2);');
    await c.eval(`return await window.__heditor.setCaret(0, ${L16});`);
    for (const ch of ['X', 'Y', 'Z']) await c.eval(key(ch));
    chk.check('前置：敲进三个字符', (await text16()) === base16.t + 'XYZ',
              JSON.stringify(await text16()));

    let undoneH = 0;
    while (await c.eval('return await window.__heditor.undo() ? 1 : 0;')) ++undoneH;
    chk.check('★ 上限 2：三笔只撤得动两次', undoneH === 2, String(undoneH));
    chk.check('★ 丢的是最旧的 —— 第一个字符撤不回去了',
              (await text16()) === base16.t + 'X', JSON.stringify(await text16()));
    // 还原成默认，免得影响后面的判据。
    chk.check('还原成默认上限', (await c.eval('return await window.__heditor.setUndoLimit(100);')) === 100);
  }

  chk.section('[7] 无页面异常 / 无 console 错误');
  {
    report.pageErrors = await c.eval('return window.__heditor.errors;');
    report.consoleErrors = cdp.conn.consoleErrors;
    chk.check('无 page error', report.pageErrors.length === 0, JSON.stringify(report.pageErrors));
    chk.check('无 console error', report.consoleErrors.length === 0,
              JSON.stringify(report.consoleErrors));
  }

  chk.section('[17] 字体按需读：命中率 / 每页落存储 / 每帧 IO');
  {
    // 整份装载时这六个数应当全是零 —— 没有数据源，也就没有按需读的账。
    //   这一条同时是"按需这条路没被误开"的判据。
    const full = await c.eval('return window.__heditor.editStats;');
    chk.check('整份装载时按需的账全为零（没有数据源）',
              Number(full.fontStorageReads) === 0 && Number(full.fontResidentBytes) === 0 &&
                Number(full.fontCacheHits) === 0,
              `reads=${full.fontStorageReads} resident=${full.fontResidentBytes}`);

    const switched = await c.eval('return await window.__heditor.reloadFonts("lazy");');
    chk.check('能切到按需装载（字体写进 MEMFS，引擎按路径装）',
              !!(switched && switched.mode === 'lazy'), JSON.stringify(switched && switched.mode));

    // 三个数要在真实排版上量：装 2000 段并把它排一遍。
    //   用 diverse 那一个入口 —— stress 文档整篇是同一个字，字形集合只有一个，
    //   字形缓存与块缓存都会命中到失真（量出来的数完全不反映真文档）。
    await c.eval('await window.__heditor.loadStressDocumentDiverse(2000, 20); return true;');
    await c.eval('return await window.__heditor.present();');
    // 再换几个窗口位置。只量"当前窗口那 3 页"会低估浏览成本 —— 换到新区域
    //   要读它自己的字形。这里直接设视口锚点（比模拟滚动稳），前中后各一次。
    for (const first of [0, 25, 50]) {
      await c.eval(`return await window.__heditor.setViewportAnchor(${first}, 3);`);
    }
    const st = await c.eval('return window.__heditor.editStats;');

    const hits = Number(st.fontCacheHits);
    const misses = Number(st.fontCacheMisses);
    const reads = Number(st.fontStorageReads);
    const ioBytes = Number(st.fontStorageBytes);
    const ioMs = Number(st.fontIoMs);
    const resident = Number(st.fontResidentBytes);
    const glyphs = Number(st.glyphCount);
    const pages = Math.max(1, Number(st.totalPages));
    const hitRate = hits + misses > 0 ? hits / (hits + misses) : 0;
    const storageFailures = Number(st.fontStorageFailures);
    const storageEofs = Number(st.fontStorageEofs);

    report.fontLazy = {
      hits, misses, reads, ioBytes, ioMs, resident, glyphs, pages,
      hitRate: +hitRate.toFixed(4),
      readsPerPage: +(reads / pages).toFixed(2),
      residentMB: +(resident / 1048576).toFixed(2),
      ioPerPageMs: +(ioMs / pages).toFixed(3),
      storageFailures, storageEofs,
    };
    chk.check('按需读真的发生了（未命中不为零）', misses > 0, `misses=${misses}`);
    chk.check(`块缓存真的在挡：命中率 ${(hitRate * 100).toFixed(1)}%`,
              hitRate > 0.5, `hits=${hits} misses=${misses}`);
    chk.check(`落存储次数 ${reads} 远小于字形数 ${glyphs}`, reads < glyphs,
              `${reads} vs ${glyphs}`);
    chk.check(`每页落存储 ${(reads / pages).toFixed(2)} 次`, reads / pages < 200,
              `pages=${pages}`);
    chk.check(`每页存储 IO ${(ioMs / pages).toFixed(3)} ms（上限 1 ms）`,
              ioMs / pages < 1.0, `ioMs=${ioMs.toFixed(3)}`);
    // 常驻的两个上界：不会超过累计读进来的字节，也不会超过"数据源个数乘单源上限"。
    chk.check(`常驻 ${(resident / 1048576).toFixed(2)} MB 不超过累计读入`,
              resident <= ioBytes, `${resident} vs ${ioBytes}`);
    const residentCap = 3 * 4 * 1024 * 1024;  // 三张按需脸，每张的上限 4 MB
    chk.check(
      `常驻 ${(resident / 1048576).toFixed(2)} MB（上限 ${(residentCap / 1048576).toFixed(1)} MB）`,
      resident <= residentCap, `resident=${resident}`);
    // 这条才是收益本身：整份三个字体约 20 MB，按需的常驻该只有它的零头。
    chk.check('常驻不到整份字体的四分之一', resident < 5 * 1024 * 1024,
              `${(resident / 1048576).toFixed(2)} MB vs 约 20 MB`);
    chk.check('按需装载后不缺字（骨架没漏掉排版要用的表）',
              Number(st.missingGlyphCount) === 0, `missing=${st.missingGlyphCount}`);
    // 两个诊断字段必须真的接在链条上：接到别处（或没接）时是 undefined，
    //   而 Number(undefined) 是 NaN —— 这一条既查"接上了"也查"是零"。
    chk.check('存储失败计数已接到诊断口且为零（MEMFS 下不该有真失败）',
              Number.isFinite(storageFailures) && storageFailures === 0,
              `fontStorageFailures=${st.fontStorageFailures}`);
    chk.check('文件尾计数已接到诊断口且为有限数',
              Number.isFinite(storageEofs), `fontStorageEofs=${st.fontStorageEofs}`);

    // 最要紧的一条：帧上必须真的有字。读数字再漂亮，画不出来就是没走通 ——
    //   而且这一条不能只看"非零"，要跟整份模式画同一页的墨迹比。
    //
    //   两次取墨迹必须先钉住同一个窗口、同一页：上面那几段把锚点挪到过第 50 页，
    //   而这里比的是"同一页"。不钉的话就成了"第 50 页 vs 第 0 页"（整帧版曾靠
    //   "这段语料只有 20 个词、每页长得差不多"侥幸过关，改成只数第一页就露馅）。
    //   只数第一页那几行也正是判据名字里的"同一页"：帧是窗口大的，按整帧数会掺进
    //   "窗口盖着哪几页"，那个数随窗口策略变，测的就成了窗口在哪而不是字画没画出来。
    const INK = `
      const H = window.__heditor;
      const f = await H.rawFrame();
      const rows = H.frameW() * H.pagePx[1] * 4;
      let n = 0;
      for (let i = 0; i + 3 < rows; i += 4) {
        if (f[i] < 200 || f[i+1] < 200 || f[i+2] < 200) ++n;
      }
      return n;
    `;
    await c.eval('return await window.__heditor.setViewportAnchor(0, 3);');
    const inkLazy = await c.eval(INK);
    await c.eval('return await window.__heditor.reloadFonts("full");');
    await c.eval('await window.__heditor.loadStressDocumentDiverse(2000, 20); return true;');
    await c.eval('return await window.__heditor.setViewportAnchor(0, 3);');
    const inkFull = await c.eval(INK);
    report.inkCompare = { lazy: inkLazy, full: inkFull };
    chk.check('按需模式下帧上有字', inkLazy > 1000, `${inkLazy} px`);
    chk.check(`按需与整份渲染同一页的墨迹一致（${inkLazy} vs ${inkFull}）`,
              inkFull > 1000 && Math.abs(inkLazy - inkFull) / Math.max(1, inkFull) < 0.02,
              `${inkLazy} vs ${inkFull}`);
  }

  chk.section('[18] 字体块大小：块越小带宽越省、次数越贵');
  {
    // 三档块大小，常驻上限固定 3 MB（块数 = 上限 / 块大小）—— 只让"粒度"这一个变量变。
    //   换档之后必须重装字体：块缓存的配置在建脸时就定了，而且 FreeType 自己缓存过
    //   的字形不会因为换了配置就失效（不重装的话第二次量到的是"没读"）。
    const CAP = 3 * 1024 * 1024;
    const rows = [];
    for (const bs of [4096, 16384, 65536]) {
      const blocks = Math.floor(CAP / bs);
      await c.eval(`return await window.__heditor.setFontCacheConfig(${bs}, ${blocks});`);
      await c.eval('return await window.__heditor.reloadFonts("lazy");');
      await c.eval('await window.__heditor.loadStressDocumentDiverse(2000, 20); return true;');
      await c.eval('return await window.__heditor.present();');
      const s = await c.eval('return window.__heditor.editStats;');
      const hits = Number(s.fontCacheHits);
      const misses = Number(s.fontCacheMisses);
      rows.push({
        blockKB: bs / 1024,
        blocks,
        hitRate: hits + misses > 0 ? +(hits / (hits + misses)).toFixed(4) : 0,
        reads: Number(s.fontStorageReads),
        ioMB: +(Number(s.fontStorageBytes) / 1048576).toFixed(2),
        ioMs: +Number(s.fontIoMs).toFixed(2),
        residentMB: +(Number(s.fontResidentBytes) / 1048576).toFixed(2),
      });
    }
    report.blockSizes = rows;
    for (const r of rows) {
      console.log(`     块 ${String(r.blockKB).padStart(3)} KB x ${String(r.blocks).padStart(5)}` +
                  `（上限 3 MB）：命中 ${(r.hitRate * 100).toFixed(1)}%，` +
                  `读 ${r.reads} 次 / ${r.ioMB} MB，IO ${r.ioMs} ms，常驻 ${r.residentMB} MB`);
    }
    const small = rows[0];
    const big = rows[rows.length - 1];
    chk.check('三档都真的读过存储（配置生效了）', rows.every((r) => r.reads > 0),
              rows.map((r) => r.reads).join(' / '));
    chk.check(
      `块越小、读进来的字节越少（${big.blockKB} KB 的 ${big.ioMB} MB -> ${small.blockKB} KB 的 ${small.ioMB} MB）`,
      small.ioMB < big.ioMB, `${big.ioMB} -> ${small.ioMB} MB`);
    chk.check(
      `块越小、命中率越低（${(small.hitRate * 100).toFixed(1)}% vs ${(big.hitRate * 100).toFixed(1)}%）`,
      small.hitRate <= big.hitRate, `${small.hitRate} vs ${big.hitRate}`);
  }

  chk.section('[19] 真病历文本：用字更分散，缓存账更紧');
  {
    // 真文本由门面读文件并清洗（清洗逻辑只此一份，页面下拉用的是同一条路）。
    const textInfo = await c.eval('return await window.__heditor.loadMedicalSample(1);');
    console.log(`     病历文本：${textInfo.paragraphs} 段 / ${textInfo.chars} 字`);
    // 这一条查的是页面自己的下拉：旧宿主页把场景做成了 UI，库不带 UI。
    //   在库页面上跳过它（而不是为了它给库加一个 select —— 那是把页面塞进库）。
    if (await c.eval('return !!(window.__heditor && window.__heditor.isLib);')) {
      chk.note('页面下拉里有真病历这一项：库页面没有场景下拉，跳过');
    } else {
      chk.check('页面下拉里有真病历这一项（场景入了 UI，不只是探针能用）',
                await c.eval(`return [...document.querySelectorAll('#scenario option')]
                                .some((o) => o.value === '8');`), 'id=8');
    }

    // 四个档位：块大小与常驻上限各变一次 —— 真文本的工作集比合成文本大，
    //   所以除了"块多大"，还要看"上限给多少"。
    const VARIANTS = [
      { kb: 4, capMB: 3 },
      { kb: 4, capMB: 6 },
      { kb: 16, capMB: 3 },
      { kb: 64, capMB: 3 },
    ];
    const rows = [];
    for (const v of VARIANTS) {
      const bs = v.kb * 1024;
      const blocks = Math.floor((v.capMB * 1024 * 1024) / bs);
      await c.eval(`return await window.__heditor.setFontCacheConfig(${bs}, ${blocks});`);
      await c.eval('return await window.__heditor.reloadFonts("lazy");');
      await c.eval('return await window.__heditor.loadMedicalSample(7);');
      await c.eval('return await window.__heditor.present();');
      const s = await c.eval('return window.__heditor.editStats;');
      const hits = Number(s.fontCacheHits);
      const misses = Number(s.fontCacheMisses);
      rows.push({
        label: `${v.kb} KB x ${blocks} 块（上限 ${v.capMB} MB）`,
        blockKB: v.kb,
        capMB: v.capMB,
        hitRate: hits + misses > 0 ? +(hits / (hits + misses)).toFixed(4) : 0,
        reads: Number(s.fontStorageReads),
        ioMB: +(Number(s.fontStorageBytes) / 1048576).toFixed(2),
        residentMB: +(Number(s.fontResidentBytes) / 1048576).toFixed(2),
        glyphs: Number(s.glyphCount),
        pages: Number(s.totalPages),
      });
    }
    report.medicalDoc = { text: textInfo, variants: rows };
    for (const r of rows) {
      console.log(`     ${r.label.padEnd(26)} 命中 ${(r.hitRate * 100).toFixed(1)}%，` +
                  `读 ${String(r.reads).padStart(5)} 次 / ${String(r.ioMB).padStart(6)} MB，` +
                  `常驻 ${String(r.residentMB).padStart(5)} MB（${r.pages} 页 / ${r.glyphs} 字形）`);
    }
    chk.check('真文本装载成功并排出了页', rows.every((r) => r.glyphs > 0 && r.pages > 1),
              rows.map((r) => `${r.pages}页`).join(' / '));
    chk.check('真文本下块越小读入越少', rows[0].ioMB < rows[3].ioMB,
              `4KB=${rows[0].ioMB} 64KB=${rows[3].ioMB} MB`);
    chk.check('上限从 3 MB 提到 6 MB 后读入不上升（工作集越界会淘汰重读）',
              rows[1].ioMB <= rows[0].ioMB, `3MB=${rows[0].ioMB} 6MB=${rows[1].ioMB} MB`);
    // 真文本用字分散、重复少，命中率比合成文本低是正常的；关键是"块越小越省"的关系
    // 不但保持，而且差距拉大 —— 工作集一旦越界，大块会雪崩式淘汰重读。
    const synth = report.blockSizes || [];
    if (synth.length) {
      const rm = rows[3].ioMB / Math.max(0.01, rows[0].ioMB);
      const rs = synth[2].ioMB / Math.max(0.01, synth[0].ioMB);
      chk.check(`真文本下"大块比小块多读"的倍数更大（${rm.toFixed(1)}x vs ${rs.toFixed(1)}x）`,
                rm > rs, `${rm.toFixed(1)}x vs ${rs.toFixed(1)}x`);
    }
  }

  chk.section('[18] 选区与移动族：Shift 扩展、上下键、Ctrl+A、选区上屏');
  {
    // 这一节判的是"用户按键这一路"：全部派真实 keydown，不走门面调用 ——
    //   门面调用绕过的正是要判的那一段（宿主把 key + 修饰键翻译成引擎命令）。
    // 文档换成长段：上下键与"跨行拆矩形"必须真的跨行，一行装得下的文档测不出来。
    await c.eval(
      'await window.__heditor.loadTextDocument(' +
      JSON.stringify('甲乙丙丁戊己庚辛壬癸子丑寅卯辰巳午未申酉戌亥') +
      ', 3); return true;');

    // (a) 装载之后没有选区 —— 反向一半：没有选区就没有要画的东西
    const s0 = await c.eval('return window.__heditor.selection;');
    chk.check('装载之后没有选区（两端重合）', !!s0 && s0.empty === true, JSON.stringify(s0));
    chk.check('★ 空选区零矩形（反向：没选区就没东西可画）',
              !!s0 && s0.rects.length === 0, JSON.stringify(s0 && s0.rects));

    // (b) Shift+右：选区从锚点那一端长
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const anchor0 = await c.eval('return window.__heditor.selection.anchor;');
    await c.eval(key('ArrowRight', ', shiftKey: true'));
    await c.eval(key('ArrowRight', ', shiftKey: true'));
    const s1 = await c.eval('return window.__heditor.selection;');
    chk.check('★ Shift+右 之后有选区', s1.empty === false, JSON.stringify(s1));
    chk.check('★ 锚点留在起点没动', s1.anchor === anchor0, `anchor=${s1.anchor} 原=${anchor0}`);
    chk.check('焦点端前进两格', s1.focus === anchor0 + 2, `focus=${s1.focus}`);

    // (c) 连续扩展：锚点还是那一个（不是"每次重新起头"）
    await c.eval(key('ArrowRight', ', shiftKey: true'));
    const s2 = await c.eval('return window.__heditor.selection;');
    chk.check('★ 连续 Shift+右 锚点仍不动', s2.anchor === anchor0, `anchor=${s2.anchor}`);
    chk.check('焦点端再前进一格', s2.focus === s1.focus + 1, `focus=${s2.focus}`);

    // (d) Shift+左：反向扩展，锚点仍不动
    await c.eval(key('ArrowLeft', ', shiftKey: true'));
    const s3 = await c.eval('return window.__heditor.selection;');
    chk.check('★ 反向扩展锚点也不动', s3.anchor === anchor0, `anchor=${s3.anchor}`);
    chk.check('焦点端回退一格', s3.focus === s2.focus - 1, `focus=${s3.focus}`);

    // (e) 选区上屏：矩形真的落到了页面上
    chk.check('★ 选区落到了矩形上', s3.rects.length >= 1, `rects=${s3.rects.length}`);
    if (s3.rects.length) {
      const r = s3.rects[0];
      chk.check('矩形有正的宽高', r.w > 0 && r.h > 0, JSON.stringify(r));
    }
    // 像素级证据：同一份文档、同一相位，只看"有选区 / 无选区"两张帧。
    //
    //   尺子不能用"非白像素数（任一通道 < 200）"：选区填充是留给文字透出来的浅蓝
    //   （alpha 90），混到纸面上是 228/241/255 —— 三个通道都够亮，那把尺子按定义
    //   量不出它（实测差 +2，那只是字形抗锯齿边缘被压过阈值的那几个像素）。
    //   填充的指纹是"偏蓝"：b 明显高于 r。空选区时这个计数只来自次像素抗锯齿的边缘。
    //
    //   两条一起才算数，而且它们各自只钉一件事，能独立地翻红：
    //     偏蓝的像素变多 —— 有东西被涂上去了（没画 ⇒ 这条红）；
    //     变暗的像素基本不涨 —— 涂上去的是"洗"不是"块"，纸面与文字都还亮着
    //       （改成实心填充 ⇒ 这条红，而上一条照样绿）。
    //   所以门限取 r >= 170：实心填充的 r 是 178，仍然算"偏蓝"，
    //   两条才能各自只钉一件事，而不是一起红。
    const scanExpr = `
      const f = await window.__heditor.rawFrame();
      let ink = 0, blue = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const r = f[i], g = f[i + 1], b = f[i + 2];
        if (r < 200 || g < 200 || b < 200) ++ink;
        if (b - r >= 8 && r >= 170) ++blue;
      }
      return { ink, blue };`;
    await c.eval(
      'await window.__heditor.setCaret(0, 0); await window.__heditor.stopBlink(); return true;');
    const pxNo = await c.eval(scanExpr);
    await c.eval(key('ArrowRight', ', shiftKey: true'));
    await c.eval(key('ArrowRight', ', shiftKey: true'));
    const pxSel = await c.eval(scanExpr);
    const dBlue = Number(pxSel.blue) - Number(pxNo.blue);
    const dInk = Number(pxSel.ink) - Number(pxNo.ink);
    chk.check('★ 选区真的画进了帧（偏蓝的像素数变多）', dBlue >= 200,
              `有选区 ${pxSel.blue} vs 无选区 ${pxNo.blue}（差 ${dBlue}）`);
    chk.check('★ 涂上去的是混合不是实心（纸面没被压暗）', dInk <= 50,
              `有选区 ${pxSel.ink} vs 无选区 ${pxNo.ink}（差 ${dInk}）`);
    const stSel = await c.eval('return window.__heditor.editStats;');
    chk.check('这一拍确实重画过（不是"只算了矩形没画"）', Number(stSel.repaintedPixels) > 0,
              `${stSel.repaintedPixels}`);

    // (f) 裸方向键把选区折叠
    await c.eval(key('ArrowRight'));
    const s4 = await c.eval('return window.__heditor.selection;');
    chk.check('★ 裸方向键把选区折叠（不留看不见的选区）', s4.empty === true, JSON.stringify(s4));

    // (g) Ctrl+A 全选
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval(key('a', ', ctrlKey: true'));
    const sa = await c.eval('return window.__heditor.selection;');
    chk.check('★ Ctrl+A 全选（走真事件）', sa.empty === false, JSON.stringify(sa));
    chk.check('★ 全选跨行 ⇒ 多个矩形（整篇确实被选中）', sa.rects.length >= 2,
              `rects=${sa.rects.length}`);

    // (h) 上下键：↓↑ 回原位
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const cb = await c.eval('return window.__heditor.caret;');
    await c.eval(key('ArrowDown'));
    const cd = await c.eval('return window.__heditor.caret;');
    chk.check('★ ↓ 走到了别的行（位置变了）', !(cd.para === cb.para && cd.offset === cb.offset),
              `${JSON.stringify(cb)} → ${JSON.stringify(cd)}`);
    await c.eval(key('ArrowUp'));
    const cu = await c.eval('return window.__heditor.caret;');
    chk.check('★ ↓↑ 回到原位', cu.para === cb.para && cu.offset === cb.offset,
              `${JSON.stringify(cb)} → ${JSON.stringify(cu)}`);

    // (i) Shift+↓ 扩展（与 Shift+左右 共用同一个 collapse 口径）
    await c.eval(key('ArrowDown', ', shiftKey: true'));
    const sd = await c.eval('return window.__heditor.selection;');
    chk.check('★ Shift+↓ 扩展出选区', sd.empty === false, JSON.stringify(sd));

    // (j) 文档首尾：Ctrl+Home / Ctrl+End（裸 Home/End 是行首行尾，见 [3]）
    const tp = await c.eval('return window.__heditor.editStats.totalParagraphs;');
    await c.eval(key('End', ', ctrlKey: true'));
    const ce = await c.eval('return window.__heditor.caret;');
    chk.check('Ctrl+End 落在最后一段', ce.para === Number(tp) - 1, `para=${ce.para} 共 ${tp} 段`);
    await c.eval(key('Home', ', ctrlKey: true'));
    const chh = await c.eval('return window.__heditor.caret;');
    chk.check('Ctrl+Home 回到第一段段首', chh.para === 0 && chh.offset === 0,
              JSON.stringify(chh));
  }

  chk.section('[19] 编辑入口吃掉选区：替换、退格只删选区、跨段删除、撤销回两端');
  {
    // 与 [18] 同一条纪律：全走真 keydown（含 Ctrl+A / Ctrl+Z / Ctrl+Y），
    //   不走门面调用 —— 门面调用绕过的正是"宿主把按键翻译成引擎命令"那一段。
    const DOC19 = '零一二三四五六七八九十';
    const load19 = () => c.eval(
      'await window.__heditor.loadTextDocument(' + JSON.stringify(DOC19) + ', 3); return true;');
    // 从 (0, 2) 起 Shift+右 两格 = 选中第 2、3 两个字。
    const selectTwo19 = async () => {
      await c.eval('await window.__heditor.setCaret(0, 2); return true;');
      await c.eval(key('ArrowRight', ', shiftKey: true'));
      await c.eval(key('ArrowRight', ', shiftKey: true'));
      return c.eval('return window.__heditor.selection;');
    };

    // (a) 选中再打字 = 原文 − 选区 + 新字
    await load19();
    const s19 = await selectTwo19();
    chk.check('★ Shift+右 选中两个字（走真事件）',
              s19.empty === false && s19.focus === s19.anchor + 2, JSON.stringify(s19));
    await c.eval(key('测'));
    const t19 = await c.eval('return window.__heditor.paragraphText(0);');
    chk.check('★ 选中再打字 = 原文 − 选区 + 新字', t19 === '零一测四五六七八九十', t19);
    const r19 = await c.eval('return window.__heditor.selection;');
    chk.check('★ 替换之后没有残选区（反向：不许留一条看不见的选区）', r19.empty === true,
              JSON.stringify(r19));

    // (b) 撤销一次：文字与选区的两端一起回来
    await c.eval(key('z', ', ctrlKey: true'));
    const u19 = await c.eval(
      'return { t: await window.__heditor.paragraphText(0), s: window.__heditor.selection };');
    chk.check('★ 撤销把文字还回来了', u19.t === DOC19, u19.t);
    chk.check('★ 撤销回到编辑前两端（不是只剩一个光标）',
              u19.s.empty === false && u19.s.anchor === s19.anchor && u19.s.focus === s19.focus,
              `期望 ${s19.anchor}..${s19.focus}，实际 ${u19.s.anchor}..${u19.s.focus}`);
    chk.check('★ 回来的选区画得出来（有矩形）', u19.s.rects.length >= 1,
              `rects=${u19.s.rects.length}`);

    // (c) 重做：替换那一下整套回来
    await c.eval(key('y', ', ctrlKey: true'));
    const re19 = await c.eval(
      'return { t: await window.__heditor.paragraphText(0), s: window.__heditor.selection };');
    chk.check('★ 重做回到替换之后', re19.t === '零一测四五六七八九十', re19.t);
    chk.check('重做之后没有选区', re19.s.empty === true, JSON.stringify(re19.s));

    // (d) 退格在选区非空时只删选区，不再往前多删一个字
    await load19();
    await selectTwo19();
    await c.eval(key('Backspace'));
    const b19 = await c.eval('return window.__heditor.paragraphText(0);');
    chk.check('★ 退格只删掉选中的两个字（不多吃一个字）', b19 === '零一四五六七八九十', b19);

    // (e) 反向一半：没有选区时退格照旧删一个字。
    //     没有这一条，"退格什么都不做"也能让上面那条绿。
    await load19();
    await c.eval('await window.__heditor.setCaret(0, 3); return true;');
    await c.eval(key('Backspace'));
    const b19b = await c.eval('return window.__heditor.paragraphText(0);');
    chk.check('★ 空选区时退格照旧删一个字', b19b === '零一三四五六七八九十', b19b);

    // (f) 前删同理
    await load19();
    await selectTwo19();
    await c.eval(key('Delete'));
    const d19 = await c.eval('return window.__heditor.paragraphText(0);');
    chk.check('★ 前删也只删选区', d19 === '零一四五六七八九十', d19);

    // (g) 跨段：选区跨过段落标记，退格把两段并起来
    await load19();
    await c.eval('await window.__heditor.setCaret(0, 8); return true;');
    for (let i = 0; i < 5; ++i) await c.eval(key('ArrowRight', ', shiftKey: true'));
    const x19 = await c.eval('return window.__heditor.selection;');
    chk.check('★ 选区跨过了段落标记（焦点进了下一段）', x19.empty === false, JSON.stringify(x19));
    await c.eval(key('Backspace'));
    const xa = await c.eval(
      'return { t: await window.__heditor.allText,' +
      '         n: window.__heditor.editStats.totalParagraphs };');
    chk.check('★ 跨段退格把两段并成一段、内容接得上',
              xa.t === '零一二三四五六七一二三四五六七八九十\n零一二三四五六七八九十', xa.t);
    chk.check('★ 段数少了一段（3 → 2）', Number(xa.n) === 2, `${xa.n}`);

    // (h) Ctrl+A 再退格：整篇删空，只剩一个空段
    await load19();
    await c.eval(key('a', ', ctrlKey: true'));
    await c.eval(key('Backspace'));
    const e19 = await c.eval(
      'return { t: await window.__heditor.allText,' +
      '         n: window.__heditor.editStats.totalParagraphs,' +
      '         s: window.__heditor.selection };');
    chk.check('★ Ctrl+A 再退格把整篇删空（只剩一个空段）',
              e19.t === '' && Number(e19.n) === 1, `${JSON.stringify(e19.t)} / ${e19.n} 段`);
    chk.check('删空之后没有残选区', e19.s.empty === true, JSON.stringify(e19.s));

    // (i) 全选再打字 = 整篇只剩那个字
    await load19();
    await c.eval(key('a', ', ctrlKey: true'));
    await c.eval(key('测'));
    const f19 = await c.eval(
      'return { t: await window.__heditor.allText,' +
      '         n: window.__heditor.editStats.totalParagraphs };');
    chk.check('★ 全选再打字 = 整篇只剩那个字', f19.t === '测' && Number(f19.n) === 1,
              `${JSON.stringify(f19.t)} / ${f19.n} 段`);

    // (j) 回车（带选区）：先删掉选区，再在落点分段
    await load19();
    await selectTwo19();
    await c.eval(key('Enter'));
    const g19 = await c.eval(
      'return { p0: await window.__heditor.paragraphText(0),' +
      '         p1: await window.__heditor.paragraphText(1),' +
      '         n: window.__heditor.editStats.totalParagraphs };');
    chk.check('★ 回车（带选区）：先删选区再分段（3 → 4 段）', Number(g19.n) === 4, `${g19.n}`);
    chk.check('前半段是选区之前的内容', g19.p0 === '零一', g19.p0);
    chk.check('后半段是选区之后的内容', g19.p1 === '四五六七八九十', g19.p1);

    // 收尾：把文档换回去，别把这一节的痕迹留给后面的节
    await c.eval(
      'await window.__heditor.loadTextDocument(' + JSON.stringify(S6) + ', 1); return true;');
  }

  chk.section('[20] 拖选：按下、移动、抬起（真鼠标事件）');
  {
    // 为什么必须走 CDP 的真鼠标事件（与 [12] 同一条理由）：拖选这一格坏在
    //   "按下 → 命中 → 定位 → 移动 → 焦点端跟着走"这条链上，合成 MouseEvent 虽然
    //   也能打到监听器上，但只有真事件连坐标换算、命中、目标元素（画布）一起过一遍。
    //   另外两套宿主对"按下目标"的判断不同（单线程看坐标，Worker 看 event.target），
    //   合成事件很容易只在一侧成立。
    //
    // 本节自己钉住缩放：坐标换算读的是实时的实际缩放（自洽），但 (i) 那一节还依赖
    //   "文档有多高"（自动滚动要跑得够久），那是缩放决定的。不钉住的话，前一节把
    //   缩放留在别的值上，这一节就在另一种高度下跑，红项看起来像拖选坏了。
    await c.eval(`return await window.__heditor.reset(6, ${ZOOM});`);
    await c.eval('return await window.__heditor.stopBlink();');

    const DOC20 = '零一二三四五六七八九十';
    const load20 = () => c.eval(
      'await window.__heditor.loadTextDocument(' + JSON.stringify(DOC20) + ', 3); return true;');

    // 光标几何 -> 客户端坐标。geo 每次现取：换文档、换窗口都会动。
    const geo20 = async () => c.eval(`
      const cv = window.__heditor.canvas, r = cv.getBoundingClientRect();
      const s = document.getElementById('stage').getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               stageTop: s.top, stageBottom: s.bottom, stageRight: s.right, stageMidX: (s.left + s.right) / 2 };`);

    // caret.x / caret.y / caret.h 是帧像素（引擎那一侧乘过 zoom，见 [14] 的
    //   innerCaretPt），所以这里只差"帧像素转 CSS 像素"这一道换算，不能再乘 zoom；
    //   多乘一次只在 zoom 为 1 时是恒等。对照组：[14] 用的是 controls.items[].rects，
    //   那是帧磅，所以那边要乘 zoom。两种单位混用会让点击整体偏若干行，
    //   症状像命中测试点错了字。
    const toClient = (g, pt, dx, dy) => ({
      x: g.left + (pt.x + (dx || 0)) * g.sx,
      y: g.top + (pt.y + (dy || 0)) * g.sy,
    });
    const mouse = async (type, at, over) => {
      const p = { x: Math.round(at.x), y: Math.round(at.y), button: 'left',
                  buttons: type === 'mouseReleased' ? 0 : 1, clickCount: 1, ...(over || {}) };
      await c.send('Input.dispatchMouseEvent', { type, ...p });
    };
    // 走完一次完整手势：按下 → 若干次移动 → 抬起。中间那次 sleep 给异步宿主留往返。
    const dragGesture = async (from, to, steps = 4) => {
      await mouse('mousePressed', from);
      for (let i = 1; i <= steps; ++i) {
        await mouse('mouseMoved', {
          x: from.x + (to.x - from.x) * (i / steps),
          y: from.y + (to.y - from.y) * (i / steps),
        });
        await sleep(20);
      }
      await mouse('mouseReleased', to);
      await c.eval('return await window.__heditor.dragSettled();');
      await sleep(30);
    };
    const sel20 = () => c.eval('return window.__heditor.selection;');
    const caret20 = () => c.eval('return window.__heditor.caret;');

    // (a) 量两个端点：文档末尾一个点、文档开头一个点。端点用键盘把光标摆过去再量，
    //     量完把选区收回来（setCaret 会清掉 anchor），免得量测本身影响了待测的手势。
    await load20();
    const g20 = await geo20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval(key('End', ', ctrlKey: true, shiftKey: true'));
    const ref20 = await sel20();                       // 键盘扩到文档末尾的答案
    const caretEnd = await caret20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const caretStart = await caret20();
    chk.check('★ 参考值：Ctrl+Shift+End 扩到文档末尾（真 keydown）',
              ref20.empty === false && Number(ref20.focus) > Number(ref20.anchor),
              JSON.stringify(ref20));

    const ptStart = toClient(g20, caretStart, 1, caretStart.h / 2);
    const ptEnd = toClient(g20, caretEnd, 2, caretEnd.h / 2);

    // (b) 按下不拖：空选区（单击不该选出任何东西）
    await mouse('mousePressed', ptStart);
    await mouse('mouseReleased', ptStart);
    const s20a = await sel20();
    const c20a = await caret20();
    chk.check('★ 按下不拖：没有选区（单击不等于选一个字）', s20a.empty === true,
              JSON.stringify(s20a));
    chk.check('按下点就是光标位置', Number(c20a.para) === 0 && Number(c20a.offset) === 0,
              `${c20a.para},${c20a.offset}`);

    // (c) 按下 → 拖到文档末尾 → 抬起：两端与键盘扩到同一处逐值相同
    await dragGesture(ptStart, ptEnd);
    const s20b = await sel20();
    const c20b = await caret20();
    chk.check('★ 拖出来的选区有内容', s20b.empty === false, JSON.stringify(s20b));
    chk.check('★ 拖到文档末尾与键盘扩到文档末尾逐值相同（两端与方向）',
              Number(s20b.anchor) === Number(ref20.anchor) &&
              Number(s20b.focus) === Number(ref20.focus),
              `拖 ${s20b.anchor}..${s20b.focus} vs 键盘 ${ref20.anchor}..${ref20.focus}`);
    chk.check('★ 焦点端落在文档末尾（光标跟着焦点端走）',
              Number(c20b.para) === 2 && Number(c20b.offset) === DOC20.length,
              `${c20b.para},${c20b.offset}（末段长 ${DOC20.length}）`);

    // (d) 反向拖：从末尾按下、拖回开头 —— 两端不交换（选区带方向）
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g20d = await geo20();
    await dragGesture(toClient(g20d, caretEnd, 2, caretEnd.h / 2),
                      toClient(g20d, caretStart, 1, caretStart.h / 2));
    const s20c = await sel20();
    chk.check('★ 反向拖：anchor 在末尾、focus 在开头（方向没被交换）',
              Number(s20c.anchor) === Number(ref20.focus) &&
              Number(s20c.focus) === Number(ref20.anchor),
              JSON.stringify(s20c));

    // (e) 拖出去再拖回来：抬手时落在按下点，选区收成空（反向的一半：不许留下看不见的选区）
    const back = toClient(g20d, caretStart, 1, caretStart.h / 2);
    await mouse('mousePressed', back);
    await mouse('mouseMoved', toClient(g20d, caretEnd, 2, caretEnd.h / 2));
    await sleep(20);
    await mouse('mouseMoved', back);
    await mouse('mouseReleased', back);
    await c.eval('return await window.__heditor.dragSettled();');
    const s20d = await sel20();
    chk.check('★ 拖出去再拖回来收成空选区', s20d.empty === true, JSON.stringify(s20d));

    // (f) 抬起之后再移动鼠标不再改选区（反向的一半：拖动只在按着的时候有效）
    await mouse('mouseMoved', { x: ptEnd.x, y: ptEnd.y }, { buttons: 0 });
    await sleep(40);
    const s20e = await sel20();
    chk.check('★ 抬起之后再移动不再改选区', s20e.empty === true, JSON.stringify(s20e));

    // (g) 拖选之后打字 = 选区被吃掉（与第 5 刀接上：拖出来的与键盘扩出来的是同一件东西）
    await load20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g20g = await geo20();
    const cs20 = await caret20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval(key('End', ', ctrlKey: true, shiftKey: true'));
    const ce20 = await caret20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await dragGesture(toClient(g20g, cs20, 1, cs20.h / 2),
                      toClient(g20g, ce20, 2, ce20.h / 2));
    await c.eval(key('测'));
    const g20h = await c.eval(
      'return { t: await window.__heditor.allText,' +
      '         n: window.__heditor.editStats.totalParagraphs };');
    chk.check('★ 拖选之后打字：整篇被替换成一个字', g20h.t === '测' && Number(g20h.n) === 1,
              `${JSON.stringify(g20h.t)} / ${g20h.n} 段`);

    // (h) 页外的灰底起手不选中：按下点没落在正文上，拖动不该选中任何东西
    await load20();
    const g20i = await geo20();
    const outside = { x: g20i.stageRight - 8, y: g20i.stageTop + 30 };
    await dragGesture(outside, { x: outside.x - 4, y: outside.y + 60 });
    const s20f = await sel20();
    chk.check('★ 页外灰底起手不选中（按下点必须在正文上）', s20f.empty === true,
              JSON.stringify(s20f));

    // (i) 拖出可视区：指针停在容器下缘之外，容器自己滚起来、选区继续长
    await c.eval('await window.__heditor.loadStressDocument(200, 20); return true;');
    // 显式回到文档顶部：这一节要的是"往下还有得滚"，视口停在哪一层不该由前面的节决定。
    await c.eval('document.getElementById("stage").scrollTop = 0; return true;');
    await sleep(250);
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g20j = await geo20();
    const cj = await caret20();
    const fromJ = toClient(g20j, cj, 1, cj.h / 2);
    const outJ = { x: g20j.stageMidX, y: g20j.stageBottom + 30 };
    // 前置：按下点要落在容器内（否则"拖出可视区"这一节无从发生），而且视口要真的
    //   在文档顶部、下面还有可滚的余量。这三条不成立时后面的断言会量到空值，
    //   那时再去看红项只会往命中测试上想。
    //
    // 第一条必须钉"接近 0"，不能只写"离底部还远"：top 接近上限时"离底部还远"照样
    //   成立，整节就会在已经滚到底的状态下跑，两次读数自然一模一样。
    const room = await c.eval(`
      const s = document.getElementById('stage');
      return { top: s.scrollTop, max: s.scrollHeight - s.clientHeight };`);
    chk.check('前置：视口钉在文档顶部、下面还有可滚余量，按下点在容器内',
              Number(room.top) <= 50 && Number(room.max) > 1000 &&
              fromJ.y > g20j.stageTop && fromJ.y < g20j.stageBottom,
              `scrollTop ${room.top}/${room.max}，按下点 y=${Math.round(fromJ.y)}，` +
              `容器 ${Math.round(g20j.stageTop)}..${Math.round(g20j.stageBottom)}`);

    // 量测必须落在自动滚动还没停的时候，不能先 await 收尾再量。
    //   dragSettled 等的是自动滚动停下来（滚到两端、或指针回到框内），而自动滚动
    //   一直把 raf 挂满，所以 await 它等于等到滚到底；那时量到的 j0 已经是终值，
    //   j1 自然一模一样，"还在继续滚"就量不出来了。
    const readStage = () => c.eval(`
      const s = document.getElementById('stage');
      return { top: s.scrollTop, max: s.scrollHeight - s.clientHeight,
               scrollH: s.scrollHeight, clientH: s.clientHeight,
               zoom: window.__heditor.zoomLevel,
               sel: window.__heditor.selection,
               drag: window.__heditor.drag };`);

    await mouse('mousePressed', fromJ);
    await mouse('mouseMoved', outJ);
    await sleep(220);                                   // 让它先跑几帧，但不到底
    const j0 = await readStage();
    await sleep(900);                                   // 继续滚、继续长
    const j1 = await readStage();
    await mouse('mouseReleased', outJ);
    await c.eval('return await window.__heditor.dragSettled();');

    // 诊断面本身也要有内容：拖动中它必须说"在拖、已经移动过、发过若干次焦点端搬运"。
    chk.check('★ 拖动中的诊断读数：在拖、移动过、搬过焦点端',
              j0.drag && j0.drag.active === true && j0.drag.moved === true &&
              Number(j0.drag.extends) > 0,
              JSON.stringify(j0.drag));
    chk.check('★ 拖出可视区：容器自己滚起来了（指针没动）',
              Number(j0.top) > 0, `scrollTop ${j0.top}（上限 ${j0.max}）`);
    chk.check('★ 拖出可视区：还在继续滚（不是一次到位）',
              Number(j1.top) > Number(j0.top),
              `${j0.top} -> ${j1.top}（上限 ${j1.max}，滚动高 ${j1.scrollH}，` +
              `视口高 ${j1.clientH}，zoom ${j1.zoom}，指针 y=${Math.round(outJ.y)}）`);
    chk.check('★ 拖出可视区仍继续扩展：焦点端继续往后走',
              Number(j1.sel.focus) > Number(j0.sel.focus),
              `${j0.sel.focus} -> ${j1.sel.focus}`);
    chk.check('★ 这一节没滚到底（滚到底是靠自动滚动、不是被别的东西拽过去）',
              Number(j1.top) < Number(j1.max),
              `${j1.top} / ${j1.max}`);
    const d20 = await c.eval('return window.__heditor.drag;');
    chk.check('★ 抬手之后拖选收尾（诊断面回到不在拖）', d20 && d20.active === false,
              JSON.stringify(d20));

    // (j) 抬起事件丢了也要收尾：用户报过"有时鼠标都松开了还在拖"。
    //   真实成因都是"页面收不到 mouseup"（在浏览器窗口之外松手、被别的窗口抢走、
    //   被浏览器接管成原生拖放），那些场景 CDP 造不出来。能造的是它的后果：
    //   移动事件里已经带着"按钮抬起了"。
    //
    //   这一条的刺激必须是页内合成的：给 CDP 的 mouseMoved 传 buttons=0 没用 ——
    //   Chrome 用它自己的按钮状态覆盖那个字段（实测读到的还是 1，于是那条移动把选区
    //   又扩了一格）。被测的只是"onMove 读到按钮已抬起就收尾"这一条，
    //   坐标换算与命中都不在它里面，所以合成事件在这里是恰当的刺激而不是绕过接线。
    await c.eval('await window.__heditor.loadStressDocument(200, 20); return true;');
    await c.eval('document.getElementById("stage").scrollTop = 0; return true;');
    await sleep(250);
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g20k = await geo20();
    const ck20 = await caret20();
    const pk20 = toClient(g20k, ck20, 1, ck20.h / 2);
    const outK = { x: g20k.stageMidX, y: g20k.stageBottom + 30 };
    // 页内派一条移动事件（按钮位可指定）—— 见上面为什么必须合成。
    const synthMove = (x, y, buttons) => c.eval(
      'window.dispatchEvent(new MouseEvent("mousemove", { clientX: ' + Math.round(x) +
      ', clientY: ' + Math.round(y) + ', buttons: ' + buttons +
      ', bubbles: true, cancelable: true })); return true;');
    const readK = () => c.eval(
      'const s = document.getElementById("stage");' +
      'return { drag: window.__heditor.drag, top: s.scrollTop };');
    await mouse('mousePressed', pk20);
    await mouse('mouseMoved', outK);                      // 拖到容器外：自动滚动起来
    await sleep(300);
    const k0 = await readK();
    await synthMove(outK.x, outK.y, 0);                   // 抬起丢了，但人还在动鼠标
    await sleep(80);
    const k1 = await readK();
    await sleep(300);
    const k2 = await readK();
    chk.check('★ 前置：拖到容器外确实在自动滚动（否则这一节测了个空）',
              k0.drag && k0.drag.active === true && k0.drag.autoScroll === true,
              JSON.stringify(k0.drag));
    chk.check('★ 抬起事件丢失时，下一次移动（按钮已抬起）也收尾',
              k1.drag && k1.drag.active === false,
              `收尾前 ${JSON.stringify(k0.drag)} → 之后 ${JSON.stringify(k1.drag)}`);
    chk.check('★ 收尾之后自动滚动也停了（松手后画面不再自己滚）',
              k2.top === k1.top, `${k1.top} → ${k2.top}`);
    await mouse('mouseReleased', outK);                   // 收尾：把按下状态也放掉

    // (k) pointercancel 也要收尾：触摸被识别成滚动、或笔被系统抢走时只剩这一个信号。
    await load20();
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    const g20m = await geo20();
    const cm20 = await caret20();
    await mouse('mousePressed', toClient(g20m, cm20, 1, cm20.h / 2));
    await mouse('mouseMoved', { x: g20m.stageMidX, y: g20m.stageTop + 120 });
    await sleep(60);
    const m0 = await c.eval('return window.__heditor.drag;');
    await c.eval('window.dispatchEvent(new PointerEvent("pointercancel",' +
                 ' { bubbles: true, cancelable: true })); return true;');
    await sleep(60);
    const m1 = await c.eval('return window.__heditor.drag;');
    chk.check('★ pointercancel 也收尾（触摸/笔被系统接管）',
              m0 && m0.active === true && m1 && m1.active === false,
              `${JSON.stringify(m0)} → ${JSON.stringify(m1)}`);
    await mouse('mouseReleased', { x: g20m.stageMidX, y: g20m.stageTop + 120 });

    // 收尾：把文档换回去，别把这一节的痕迹留给后面的节
    await c.eval(
      'await window.__heditor.loadTextDocument(' + JSON.stringify(S6) + ', 1); return true;');
  }

  // 光标与选区的样式：宿主可配置的参数
  //
  // 两件事一起钉。正面：改了样式，帧上那个像素真的变成新颜色。
  // 反面：改这一拍不会整窗重光栅 —— 把样式塞进 setConfig 的实现（每拍作废底图）
  //   在正面也全绿，而那条路每一拍都要把窗口里的每一页重新光栅。所以两条缺一不可。
  chk.section('[21] 光标与选区的样式：可配置的参数');
  {
    await c.eval(
      'await window.__heditor.loadTextDocument(' + JSON.stringify(S6) + ', 1); return true;');
    await c.eval('await window.__heditor.setCaret(0, 0); return true;');
    await c.eval('return await window.__heditor.stopBlink();');

    // 采样点取光标矩形中心偏下 3 px：一定在光标内，又躲开上缘的取整边界。
    //   读的是引擎帧（rawFrame 是快照），不是画布 —— 两个宿主的画布形状不同。
    const FRAME_PIX = `
      const H = window.__heditor;
      const f = await H.rawFrame();
      const w = H.frameW(), h = H.frameH();
      const cr = H.caret;
      const x = cr.x + Math.max(0, Math.floor(cr.w / 2));
      const y = cr.y + Math.min(3, Math.max(0, cr.h - 1));
      if (!(x >= 0 && y >= 0 && x < w && y < h)) return null;
      const i = (y * w + x) * 4;
      return [f[i], f[i + 1], f[i + 2], f[i + 3]];`;
    const frameArea = await c.eval('return window.__heditor.frameW() * window.__heditor.frameH();');

    const defCaret = await c.eval('return window.__heditor.caretStyle;');
    const defSel = await c.eval('return window.__heditor.selectionStyle;');
    const p0 = await c.eval(FRAME_PIX);
    chk.check('默认光标色就是配置里那一份（31,111,235）',
              !!p0 && p0[0] === 31 && p0[1] === 111 && p0[2] === 235,
              `采样 ${JSON.stringify(p0)}，配置 ${JSON.stringify(defCaret.color)}`);

    const st1 = await c.eval(
      'return await window.__heditor.setCaretStyle({ color: [220, 20, 60, 255] });');
    const p1 = await c.eval(FRAME_PIX);
    const back1 = await c.eval('return window.__heditor.caretStyle;');
    chk.check('改光标色：帧上那个像素变成新颜色（不只是改了参数）',
              !!p1 && p1[0] === 220 && p1[1] === 20 && p1[2] === 60 &&
              !!p0 && (p0[0] !== p1[0] || p0[1] !== p1[1] || p0[2] !== p1[2]),
              `${JSON.stringify(p0)} → ${JSON.stringify(p1)}`);
    chk.check(' 读回来的光标样式与设进去的一致',
              back1.color[0] === 220 && back1.color[1] === 20 && back1.color[2] === 60 &&
              back1.color[3] === 255 && back1.widthPt === defCaret.widthPt,
              JSON.stringify(back1));

    // 参考臂：整篇重排重绘那一拍写多少像素。没有它，"改样式那拍写得少"这句无从比较
    //   —— 一个恒报 0 的计数器也会让它成立。
    //   先切到另一个场景再切回来：run(同 id) 那一步在"这个场景已经装着"时是空操作，
    //   读回来的会变成上一拍（改样式那一拍）的 186 px —— 那样参考臂就恒等于被测对象，
    //   判据恒红。换文档是保证这一拍真的是整篇重排重绘的唯一稳当办法。
    const fullPix = await c.eval(`
      await window.__heditor.setCaretStyle({ color: [31, 111, 235, 255] });
      await window.__heditor.run(7, ${ZOOM});
      const s = await window.__heditor.run(6, ${ZOOM});
      return s.repaintedPixels;`);
    // 分母是窗口内几页的像素，不是整个帧：帧按页号分槽之后帧高 = 槽数 x 步距，
    //   槽数可以多于窗口页数（多出来的是空槽，永远不写），拿 frameArea 当分母会让这条
    //   判据随槽数一起松掉（空槽越多越松，最后恒真）。
    const winArea = await c.eval(`
      const H = window.__heditor;
      const st = H.editStats || {};
      return (st.windowPages || 1) * H.frameW() * H.pagePx[1];`);
    chk.check('参考臂：整篇重排重绘确实写了大半个窗口（否则下面那条判据是恒真的）',
              fullPix > winArea * 0.5, `写 ${fullPix} / 窗口 ${winArea} px（帧 ${frameArea}）`);
    const st2 = await c.eval(
      'return await window.__heditor.setCaretStyle({ color: [0, 128, 0, 255] });');
    chk.check('★ 改样式那一拍的写入量比整篇重绘少一个量级以上（没有整窗重光栅）',
              st2.repaintedPixels * 10 < fullPix,
              `改样式写 ${st2.repaintedPixels} px，整篇重绘写 ${fullPix} px，` +
              `帧 ${frameArea} px`);
    chk.check(' 改样式不脏页（底图一个字都没变）', st2.dirtyPageCount === 0,
              `脏页 ${st2.dirtyPageCount}`);
    chk.check(' 改样式不重排', st2.layoutRan === false && st1.layoutRan === false,
              `layoutRan ${st1.layoutRan} / ${st2.layoutRan}`);

    // 宽度下限：引擎把它夹到 0.4 —— 读回来必须是夹过的那一份，不是递进去的那一份。
    await c.eval('return await window.__heditor.setCaretStyle({ widthPt: 0.1 });');
    const wClamp = await c.eval('return window.__heditor.caretStyle;');
    chk.check('光标宽度下限被夹到 0.4 磅（递 0.1 进去）', wClamp.widthPt === 0.4,
              `读回 ${wClamp.widthPt}`);
    await c.eval('return await window.__heditor.setCaretStyle({ widthPt: 3 });');
    const wSet = await c.eval('return window.__heditor.caretStyle;');
    chk.check(' 宽度在范围内的取值原样保留（不是恒被夹到下限）', wSet.widthPt === 3,
              `读回 ${wSet.widthPt}`);

    // 宽度参数确实改变了画出来的东西：数整帧上等于光标色的像素，那个面积就是光标的
    //   可见大小。只读回 widthPt 只能证明引擎存下了这个数，证明不了它被画出来 ——
    //   而"存下来但没画"正是这类参数最常见的失败方式。
    const caretArea = async (w) => c.eval(`
      const H = window.__heditor;
      await H.setCaretStyle({ color: [0, 128, 0, 255], widthPt: ${w} });
      const f = await H.rawFrame();
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        if (f[i] === 0 && f[i + 1] === 128 && f[i + 2] === 0) ++n;
      }
      return n;`);
    const areaThin = await caretArea(0.4);
    const areaThick = await caretArea(6);
    chk.check('光标宽度参数改变了画出来的粗细（整帧上光标色的像素面积）',
              areaThin > 0 && areaThick > areaThin * 5,
              `0.4 磅 ${areaThin} px，6 磅 ${areaThick} px`);

    // 通道越界：递进去 300 / -5，必须夹到 255 / 0 而不是回绕。
    await c.eval('return await window.__heditor.setSelectionStyle({ color: [300, -5, 0, 0] });');
    const cClamp = await c.eval('return window.__heditor.selectionStyle;');
    chk.check('色通道越界被夹到 0~255（不回绕）',
              JSON.stringify(cClamp.color) === JSON.stringify([255, 0, 0, 0]),
              JSON.stringify(cClamp.color));

    // 选区：alpha 255 + 纯红 ⇒ 选区矩形内任一点都是那个红。用实心色是为了可判 ——
    //   半透明混出来的值依赖 blend 的取整口径，那不该由这条判据来钉。
    //
    // 单位：selection.rects 的 x/y/w/h 是页面磅（另带 page），而帧是帧像素 ——
    //   两套单位混用只在 zoom=1 时是恒等，取中心点会取到行外去（读回一片白）。见名词对照。
    const selPix = await c.eval(`
      const H = window.__heditor;
      await H.setSelectionStyle({ color: [255, 0, 0, 255] });
      await H.selectAll();
      const f = await H.rawFrame();
      const w = H.frameW(), h = H.frameH();
      const z = H.stats.zoom;
      const step = H.pagePx[1] + H.pagePx[2];
      const first = H.editStats.windowFirstPage;
      const r = H.selection.rects[0];
      const x = Math.floor((r.x + r.w / 2) * z);
      const y = Math.floor((r.page - first) * step + (r.y + r.h / 2) * z);
      const i = (y * w + x) * 4;
      return { px: [f[i], f[i + 1], f[i + 2], f[i + 3]], n: H.selection.rects.length,
               at: [x, y], inFrame: x >= 0 && y >= 0 && x < w && y < h, z, step, first };`);
    chk.check('改选区色：选区矩形内那个像素就是新颜色',
              selPix.inFrame && selPix.n > 0 &&
              JSON.stringify(selPix.px) === JSON.stringify([255, 0, 0, 255]),
              `${selPix.n} 条矩形，取 ${JSON.stringify(selPix.at)}（zoom ${selPix.z}）` +
              ` 得到 ${JSON.stringify(selPix.px)}`);
    const selBack = await c.eval(`
      const H = window.__heditor;
      await H.setSelectionStyle({ color: ${JSON.stringify(defSel.color)} });
      const f = await H.rawFrame();
      const i = (${selPix.at[1]} * H.frameW() + ${selPix.at[0]}) * 4;
      return [f[i], f[i + 1], f[i + 2], f[i + 3]];`);
    chk.check(' 换回默认的半透明色之后同一点不再是纯红（说明上一条读的是真像素）',
              !(selBack[0] === 255 && selBack[1] === 0 && selBack[2] === 0),
              `读回 ${JSON.stringify(selBack)}`);

    // 收尾：把两处样式恢复成引擎的默认值，别把这一节的痕迹留给后面的节和截图。
    await c.eval(
      'await window.__heditor.setCaretStyle({ color: ' + JSON.stringify(defCaret.color) +
      ', widthPt: ' + defCaret.widthPt + ' });' +
      'await window.__heditor.setSelectionStyle({ color: ' + JSON.stringify(defSel.color) +
      ' });' +
      'await window.__heditor.clearSelection(); return true;');
    const restored = await c.eval(
      'return { c: window.__heditor.caretStyle, s: window.__heditor.selectionStyle };');
    chk.check('收尾：两处样式已恢复成默认',
              JSON.stringify(restored.c.color) === JSON.stringify(defCaret.color) &&
              JSON.stringify(restored.s.color) === JSON.stringify(defSel.color),
              JSON.stringify(restored));
  }

  // 收尾：载入表单样例 + 恢复闪烁，截一张给人看
  //   刻意停在表单模式（而不是纯文档场景）：截图要能一眼看出
  //     "字段的边界长什么样、只读的那个是灰的"。文档模式也画括号，只有只读模式不画。
  await c.eval('await window.__heditor.loadFormSample(0); await window.__heditor.startBlink(); return true;');
  await sleep(400);
  await c.screenshot(join(OUT, 'edit-scene.png'));

  report.checks = chk.rows;
  report.failures = chk.failures;
  report.total = chk.total;
  report.finalStats = await c.eval('return window.__heditor.editStats;');
  writeFileSync(join(OUT, 'edit-probe.json'), JSON.stringify(report, null, 2), 'utf8');

  console.log('\n──────────────────────────────');
  console.log(`共 ${chk.total} 项断言，失败 ${chk.failures} 项`);
  console.log(`截图： ${join(OUT, 'edit-scene.png')}`);
  console.log(`明细： ${join(OUT, 'edit-probe.json')}`);

  // 批量验证时由 scripts/verify_web.ps1 统一收浏览器（它按 profile 杀整棵进程树）。
  //   这里各自关一次的话，一条命令跑十个探针就只能跑第一个：第二个连不上 9222。
  if (!process.env.HEDITOR_KEEP_BROWSER) await c.send('Browser.close').catch(() => {});
  process.exit(chk.failures ? 1 : 0);
}

main().catch((e) => {
  // 带上栈：探针里 90% 的代码是拼好的字符串（eval 到页面里跑），
  //   只打印 message 的话，定位只能靠猜"是哪个 eval 块"。
  console.error('探针失败：', e.message);
  console.error(e.stack || '(无栈)');
  process.exit(2);
});
