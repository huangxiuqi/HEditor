// 浏览器宿主（WASM 验证台）—— 档 1：可输入。
//
// 这个文件仍然刻意保持"薄"，而且现在要守住一条更难的纪律：
//
//   宿主不持有文档，也不做任何字符索引。
//     它不保存文本副本、不算 insert 位置、不把 offset 在 JS 字符串上加减。
//     原因是偏移的单位在引擎里是码点，而 JS 字符串是 UTF-16 码元：
//     一个 "⁹" 在引擎里是 1 个偏移、在 JS 里是 1 个码元但 3 个 UTF-8 字节，
//     在 "𝄞" 这种星平面字符上更会差成 2 个码元对 1 个码点。
//     只要 JS 侧碰过 offset 算术，这类 bug 就只在中文/上标/emoji 上出现，
//     而英文测试全过 —— 所以 JS 侧不碰偏移算术。
//
//   键盘 -> 一次跨界调用（typeText/backspace/moveCaret），
//     引擎内部完成 编辑 → 重排 → 光栅，回来的是"有没有变"+ 一组统计 + 一块位图。
//     循环留在引擎内，跨边界只有输入事件与位图 —— 这就是 Figma 那条纪律。
//
//   IME 组合期间一次都不调引擎。合成事件（compositionupdate）每秒能来几十次，
//     每次重排就是又卡又闪 —— 这正是 WASM 路线最容易被攻击的地方。
//     预编辑串先在状态栏本地预览，commit 时才脏化文档、只重排一次。
'use strict';

const BASE = new URL('../', location.href);
const WASM_GLUE = new URL('build/wasm/web/heditor.js', BASE);

const FONTS = [
  { role: 'cjk',    label: 'Noto Sans SC',        path: 'assets/fonts/NotoSansSC-VF.ttf' },
  { role: 'mongol', label: 'Noto Sans Mongolian', path: 'assets/fonts/NotoSansMongolian-Regular.ttf' },
  { role: 'latin',  label: 'Noto Sans (Latin)',   path: 'assets/fonts/NotoSans-Latin-VF.ttf' },
];

const SCENARIOS = [
  { id: 0, label: '完整样例（4 页）' },
  { id: 1, label: '蒙文竖排 vertical-lr（侧倒）' },
  { id: 2, label: '中文竖排 vertical-rl（正立）' },
  { id: 3, label: '中文长文本（横排）' },
  { id: 4, label: '中英混排 + 字体回退' },
  { id: 5, label: '蒙文竖排 vertical-rl（对照）' },
  { id: 6, label: ' 可编辑（敲字 / 光标 / 增量）' },
  { id: 7, label: '长文档 200 段（增量排版）' },
];

const DEFAULT_SCENARIO = 6;   // 直接进可编辑场景，省得还要先切一下

const $ = (id) => document.getElementById(id);
const cv = $('cv');
const ctx = cv.getContext('2d', { alpha: false });
const ta = $('ime');

let mod = null;
let busy = false;
let ready = false;
let currentZoom = 1.5;
let lastEdit = null;
// 上一次设给 canvas 的 CSS 尺寸（缓存起来，避免每帧都写 style 触发样式重算）
let cvCssW = 0;
let cvCssH = 0;
let composing = false;
let caretOn = true;
let blinkTimer = null;
const pageErrors = [];

window.addEventListener('error', (e) => pageErrors.push('error: ' + String(e.message)));
window.addEventListener('unhandledrejection', (e) =>
  pageErrors.push('unhandledrejection: ' + String(e.reason)));

const fmt = (n, d = 2) => Number(n).toFixed(d);
const kb = (n) => (n / 1024).toFixed(1) + ' KB';

function setStatus(text, isErr) {
  const el = $('status');
  el.textContent = text;
  el.classList.toggle('err', !!isErr);
}

function kv(id, rows) {
  $(id).innerHTML = rows
    .map(([k, v, cls]) => `<dt>${k}</dt><dd class="${cls || ''}">${v}</dd>`)
    .join('');
}

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = src;
    s.onload = resolve;
    s.onerror = () =>
      reject(new Error(`无法加载 ${src}\n先执行：cmake --workflow --preset wasm`));
    document.head.appendChild(s);
  });
}

/**
 * 启动
 */
async function boot() {
  setStatus('加载 WASM 胶水层…');
  // 先问一次"磁盘上那份 wasm 是什么时候构建的"。
  //   加它的理由：重新编译之后"改了没生效"的假象，绝大多数不是没编译，而是浏览器
  //   拿了旧副本（静态服务不发 Cache-Control 时，浏览器按启发式新鲜度直接复用缓存）。
  //   稍后会和 wasm 自己报出来的构建时间对照显示在工具条上，一眼可判。
  //   配 scripts/dev_server.py 的 no-store 才是根治：开发期每次刷新都拿真文件。
  let fileStampRaw = '';
  let fileStamp = '未知';
  try {
    const head = await fetch(WASM_GLUE.href, { method: 'HEAD', cache: 'no-store' });
    fileStampRaw = head.headers.get('last-modified') || '';
    const d = new Date(fileStampRaw);
    if (!Number.isNaN(d.getTime())) {
      const p = (n) => String(n).padStart(2, '0');
      fileStamp = `${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
                  `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
    }
  } catch (err) {
    console.warn('读不到 wasm 的构建时间（不影响启动）', err);
  }

  // 用文件自己的时间当版本号加载胶水层与 wasm。
  //   为什么值得这么绕：静态服务不发 Cache-Control 时，浏览器会按"启发式新鲜度"复用
  //   磁盘缓存 —— 重新编译了却还在跑旧副本，是排查里最费时间的一种假象。
  //   带上 ?v=<文件时间> 之后，URL 随文件本身变化：文件没变就命中缓存（快），
  //   文件变了就是全新 URL（不可能命中旧副本）。
  const cacheBust = fileStampRaw ? '?v=' + encodeURIComponent(fileStampRaw) : '';
  await loadScript(WASM_GLUE.href + cacheBust);
  if (typeof CreateHEditor !== 'function') {
    throw new Error('胶水层已加载，但没有导出 CreateHEditor');
  }

  setStatus('实例化 WASM…');
  const tWasm = performance.now();
  mod = await CreateHEditor({
    // 胶水层是从自己所在的目录拼 wasm 路径的（不含 query），所以这里显式补上 ——
    //   否则会出现"胶水是新的、wasm 是缓存里的旧的"这种最难查的组合。
    locateFile: (path) => new URL(path, WASM_GLUE.href).href + cacheBust,
  });
  const wasmMs = performance.now() - tWasm;

  // 页面跑的到底是哪一版：两处数字对照着看。
  //   runStamp 来自 wasm 自己（编进去的构建时间），fileStamp 来自磁盘上那个文件的
  //   Last-Modified。两者一致 = 浏览器加载的就是磁盘上这份；runStamp 明显更旧 =
  //   浏览器拿的是缓存里的旧副本（此时工具条会变红并给出硬刷新的提示）。
  const runStamp = typeof mod.buildStamp === 'function' ? mod.buildStamp() : '未知';
  const parsedFile = new Date(fileStampRaw);
  const staleByMs = Number.isNaN(parsedFile.getTime())
    ? 0
    : parsedFile.getTime() - (Date.parse(runStamp.replace(' ', 'T')) || 0);
  const staleSeed = staleByMs > 5 * 60 * 1000;  // 超过 5 分钟 = 磁盘上换了、跑的还是旧的
  const buildEl2 = document.getElementById('build');
  if (buildEl2) {
    buildEl2.textContent = `wasm ${runStamp} ｜ 磁盘 ${fileStamp}`;
    buildEl2.className = staleSeed ? 'stale' : '';
    buildEl2.title = staleSeed
      ? '页面跑的是缓存里的旧副本（比磁盘上的构建旧）—— 请 Ctrl+Shift+R 硬刷新'
      : '页面实际加载的那份 wasm 的构建时间，以及磁盘上那个文件的时间';
  }

  // 字体：JS fetch 进来，字节穿过边界，wasm 包里一个字体都没有
  const indices = {};
  const fontRows = [];
  const tFont = performance.now();
  let fontBytes = 0;

  for (const f of FONTS) {
    setStatus(`加载字体 ${f.label}…`);
    const res = await fetch(new URL(f.path, BASE));
    if (!res.ok) throw new Error(`${f.path} → HTTP ${res.status}`);
    const buf = new Uint8Array(await res.arrayBuffer());
    fontBytes += buf.length;

    const ptr = mod.allocBuffer(buf.length);   // ① 在 wasm 堆里要一块地
    mod.HEAPU8.set(buf, ptr);                  // ② 字节穿边界（alloc 之后才读 HEAPU8，避开 grow 失效）
    const idx = mod.loadFont(f.label, ptr, buf.length);  // ③ wasm 侧解析
    mod.freeBuffer(ptr);

    if (idx < 0) throw new Error(`字体解析失败：${f.path}`);
    indices[f.role] = idx;
    fontRows.push([f.label, `${kb(buf.length)} → #${idx}`]);
  }
  const fontMs = performance.now() - tFont;

  mod.setFontRoles(indices.cjk, indices.mongol, indices.latin);

  kv('boot', [
    ['WASM 构建时间（wasm 自己报的）', runStamp],
    ['WASM 文件时间（磁盘上）', fileStamp],
    ['WASM 实例化', `${fmt(wasmMs, 0)} ms`],
    ['字体 fetch + 解析', `${fmt(fontMs, 0)} ms`],
    ['字体合计', kb(fontBytes)],
    ...fontRows,
    ['wasm 包体', `${kb(wasmBytes())} (js+wasm)`],
  ]);

  // UI 接线
  const sel = $('scenario');
  sel.innerHTML = SCENARIOS.map((s) => `<option value="${s.id}">${s.label}</option>`).join('');
  sel.value = String(DEFAULT_SCENARIO);
  sel.disabled = false;
  sel.addEventListener('change', () => {
    mod.setScenario(Number(sel.value));
    relayoutAndBlit();
    ta.focus();
  });

  const zoom = $('zoom');
  zoom.disabled = false;
  zoom.addEventListener('input', () => {
    currentZoom = Number(zoom.value);
    $('zoomVal').textContent = currentZoom.toFixed(2).replace(/0$/, '');
    mod.setZoom(currentZoom);
    relayoutAndBlit();
  });

  $('paint').disabled = false;
  $('paint').addEventListener('click', () => relayoutAndBlit());

  $('focus').disabled = false;
  $('focus').addEventListener('click', () => ta.focus());

  $('bench').disabled = false;
  $('bench').addEventListener('click', runBench);

  // 表单：模式切换 + 样例
  //
  // 策略判断全在 C++ 侧（编辑能力表），这里只是把开关递过去。
  //   如果"能不能改"由 JS 决定，那它就只是个 UI 提示，不是边界。
  const modeSel = $('editMode');
  modeSel.disabled = false;
  modeSel.addEventListener('change', () => {
    mod.setEditMode(Number(modeSel.value));
    relayoutAndBlit();
    ta.focus();
  });

  // 表单样例有三份：相邻的一可编辑加一只读、跨行大字段内嵌只读、三个可编辑字段连排。
  //   三连那一份是给"共享边界"用的：边界两侧都可写，所以在边界上打字、把某个字段删空、
  //   在字段之间退格，全都能当场试出来。
  const variantSel = $('formVariant');
  variantSel.disabled = false;
  const formHint = [
    '表单样例：只有两个字段能改，中间的病程只读 —— 试着往别处敲字',
    '表单样例：一个大字段跨行，里面嵌着一个只读小字段',
    '表单样例：三个可编辑字段紧挨着 —— 在边界上打字 / 把中间那个删空，看括号怎么跟',
    '表单样例：三层嵌套（都可编辑）—— 点最里层的开头打字，三层一起变长',
  ];
  $('formSample').disabled = false;
  $('formSample').addEventListener('click', () => {
    const v = Number(variantSel.value);
    mod.loadFormSample(v);
    modeSel.value = String(mod.editMode());
    relayoutAndBlit();
    ta.focus();
    setStatus(formHint[v] || formHint[0]);
  });

  // 键盘与 IME：全部由 textarea 承接（canvas 收不到键盘，也收不到合成事件）
  ta.addEventListener('keydown', onKeyDown);
  ta.addEventListener('compositionstart', onCompositionStart);
  ta.addEventListener('compositionupdate', onCompositionUpdate);
  ta.addEventListener('compositionend', onCompositionEnd);
  cv.addEventListener('mousedown', onCanvasMouseDown);
  $('stage').addEventListener('mousedown', (e) => {
    if (e.target === cv || e.target === ta) return;
  });

  currentZoom = Number(zoom.value);
  mod.setZoom(currentZoom);
  mod.setScenario(DEFAULT_SCENARIO);
  relayoutAndBlit();
  ta.focus();

  ready = true;
  // 旧副本要显眼地说出来：这种情况下下面所有"改了没生效"的观感都是假象，
  //   不提示的话会一路查到底层代码，白费时间。
  if (staleSeed) {
    setStatus(`⚠ 页面跑的是旧副本（跑的 ${runStamp}，磁盘上已是 ${fileStamp}）` +
              ` —— 按 Ctrl+Shift+R 硬刷新`, true);
  } else {
    setStatus('就绪 · 点画布定位光标，然后敲字');
  }
  resetBlink();
}

/**
 * 呈现：一次跨界，然后只贴脏矩形
 */
function blitFrame(st) {
  const w = mod.frameWidth();
  const h = mod.frameHeight();
  const view = mod.frameRgba();
  if (view.byteLength !== w * h * 4) {
    throw new Error(`帧长度不符：${view.byteLength} ≠ ${w}×${h}×4`);
  }

  const resized = cv.width !== w || cv.height !== h;
  if (resized) {
    cv.width = w;
    cv.height = h;
  }

  // CSS 尺寸必须按设备像素比折算，否则浏览器会把整块画布重采样一遍。
  //
  //   画布的 backing store 是 w×h 个帧像素，而 CSS 像素在 dpr≠1 的屏上是
  //   更大的单位：不折算出 style.width，浏览器就会把 893 px 的位图铺到 893 CSS px
  //   （在 125% 缩放的屏上 = 1116 设备像素）→ 每个帧像素被插值成 1.25 个，
  //   整页文字发虚、笔画变浅 —— 这就是"字好浅"。
  //
  //   折算成 w/dpr 之后：1 帧像素 == 1 设备像素，浏览器不做任何重采样。
  //   引擎那边的 zoom 才是"分辨率"的唯一旋钮，DPI 不该再叠一层。
  //
  //   顺带：positionIme / 点击命中都用 cr.width / cv.width 求比例，
  //   折算后这个比例正好是 1/dpr，比更准。
  const dpr = window.devicePixelRatio || 1;
  const cssW = w / dpr;
  const cssH = h / dpr;
  if (cvCssW !== cssW || cvCssH !== cssH) {
    cvCssW = cssW;
    cvCssH = cssH;
    cv.style.width = cssW + 'px';
    cv.style.height = cssH + 'px';
  }

  const img = new ImageData(
    new Uint8ClampedArray(view.buffer, view.byteOffset, view.byteLength), w, h);

  // 上屏矩形来自引擎的待上屏累加器（读后即清零），而不是 editStats 的
  //    repaintedX/Y/W/H。这两个数字的用途不同：
  //      editStats 那几个  = "这一轮写了多少"（面板要的就是它）
  //      takeBlitBox()     = "自上次上屏以来一共写过哪些区域"（上屏要的是它）
  //
  //   它们在一件事上分道扬镳：一次用户操作可能触发两次 present。
  //   最典型的是 afterEdit() 里那条补救路径 —— 引擎返回 caretVisible == false
  //   时会再 present 一次把光标拉回来。第二次只写了光标与标记那几小块，
  //   若拿它的脏矩形去 putImageData，第一次那次全帧重绘就从没上屏。
  //   症状：引擎帧完全正确（auditFrame 差 0），画布却差一千多像素
  //   ——「光标走了、文字没动、那个字段的方括号还留在原地」。
  //
  //   必须每次都读（哪怕下面走了全帧分支），否则累加器不会清零。
  const box = (mod.takeBlitBox) ? JSON.parse(mod.takeBlitBox()) : null;

  // 上屏也只贴脏矩形 —— JS 这一侧同样不做全帧搬运。
  //   引擎报了"写了哪一块"，这里就用 putImageData 的 dirty-rect 参数只更新那一块。
  const canPatch = !resized && box && box.w > 0 && box.h > 0 &&
                   box.x >= 0 && box.y >= 0 && box.x < w && box.y < h;
  if (canPatch) {
    const dx = box.x;
    const dy = box.y;
    const dw = Math.min(box.w, w - dx);
    const dh = Math.min(box.h, h - dy);
    ctx.putImageData(img, 0, 0, dx, dy, dw, dh);
  } else {
    ctx.putImageData(img, 0, 0);
  }
  positionIme(st);
}

/**
 * 把承接键盘的 textarea 摆到光标处。
 *
 * 这不是 UI 细节，而是架构依赖：IME 的候选框位置由输入元素的屏幕位置决定。
 * 所以"光标几何"（caretRect）是"中文能输得舒服"的前置条件 ——
 * 也就是为什么档 1 不能跳过、必须先把行盒几何做出来。
 */
function positionIme(st) {
  if (!st || !st.caretW) {
    ta.style.width = '1px';
    ta.style.height = '1px';
    return;
  }
  const stage = $('stage');
  const cr = cv.getBoundingClientRect();
  const sr = stage.getBoundingClientRect();
  const sx = cr.width / cv.width;
  const sy = cr.height / cv.height;

  const left = cr.left - sr.left + stage.scrollLeft + st.caretX * sx;
  const top = cr.top - sr.top + stage.scrollTop + st.caretY * sy;
  const lineH = Math.max(12, Math.round(st.caretH * sy));

  ta.style.left = Math.round(left) + 'px';
  ta.style.top = Math.round(top) + 'px';
  ta.style.width = Math.max(24, Math.round(cr.width - st.caretX * sx)) + 'px';
  ta.style.height = lineH + 'px';
  ta.style.lineHeight = lineH + 'px';
  ta.style.fontSize = Math.max(10, Math.round(lineH * 0.62)) + 'px';
}

function afterEdit() {
  // 顺序有讲究：先在编辑引起的那次重绘之后把统计取下来，
  //   这才是"敲一个字写了多少像素"。若先做别的事再读，读到的就是别的账。
  const st = JSON.parse(mod.editStatsJson());
  lastEdit = st;

  // 不变量：编辑之后光标必须立即可见。
  //   闪烁相位可能正好停在"隐藏"那半拍上。
  //   现在这条不变量由引擎保证（repaintAfterEdit() 会先把 caret_visible_
  //     置真再重绘），所以下面这个分支正常情况下不会被走到。
  //     留着它是因为它是个真 bug 的现场，而且它现在也真的安全了：
  //     上屏走的是 takeBlitBox() 的累加器，第二次 present 的脏矩形不会把
  //     第一次那次全帧重绘挤掉（见 blitFrame 顶部那段注释）。
  if (!st.caretVisible) {
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    blitFrame(JSON.parse(mod.editStatsJson()));
  } else {
    blitFrame(st);
  }

  renderEdit(st);
  renderForm(st);
  // "本次布局"面板的字段来自 statsJson（页数/字形数/缺字），
  //   不是 editStatsJson。之前这里传的是编辑统计，于是那一栏的页数字形数
  //   全是 undefined —— 面板照样渲染，所以不看就发现不了。
  renderLayout(JSON.parse(mod.statsJson()));
  resetBlink();
  return st;
}

function relayoutAndBlit() {
  if (!ready && !mod) return;
  mod.relayoutAndPaint();
  const st = JSON.parse(mod.editStatsJson());
  lastEdit = st;
  blitFrame(st);
  renderEdit(st);
  renderForm(st);
  renderLayout(JSON.parse(mod.statsJson()));
  return st;
}

/**
 * 移动光标并立即呈现。
 * setCaret 只改引擎里的位置，不动像素 —— 漏掉 present() 的症状是
 * "点了画布，光标过一会儿才跳过去"（下次闪烁 tick 才重绘）。
 *
 * 呈现前先把闪烁相位摆到可见，理由与引擎的 repaintAfterEdit() 相同：否则
 * editStats.caretVisible 为 false，afterEdit() 会判定"刚才那次重绘把光标画成隐的"，
 * 再 present 一次并把上屏矩形换成那一轮的（那一轮只写了光标与标记那几小块），
 * 于是引擎帧正确而画布停在旧排版上。
 * 上屏现已由 takeBlitBox() 的累加器保证，这里只是去掉那层冗余：一次点击不该产生两次 present。
 */
function setCaretAndPresent(para, offset) {
  mod.setCaret(para, offset);
  caretOn = true;
  mod.showCaret(true);
  mod.present();
  return afterEdit();
}

/**
 * 键盘
 */
function onKeyDown(ev) {
  if (!ready || busy) return;
  // 组合期间一律不动文档。中文输入法在组合时会持续发 keydown，
  //   若每次都去重排，画面就会又卡又闪 —— WASM 编辑器最容易在这里出问题。
  if (composing || ev.isComposing) return;
  if (ev.ctrlKey || ev.metaKey || ev.altKey) return;

  let changed = false;
  let handled = true;

  switch (ev.key) {
    case 'Backspace':
      changed = mod.backspace();
      break;
    case 'Delete':
      changed = mod.deleteForward();
      break;
    case 'ArrowLeft':
      changed = mod.moveCaret(-1);
      break;
    case 'ArrowRight':
      changed = mod.moveCaret(1);
      break;
    case 'Home':
      // 走引擎的"到行首"，不要用 moveCaret(-1e9) 凑。
      //   之所以"能用"，只是因为 moveCaretBy 走不动时会提前 break；
      //   而且 1e9 表达的是段首段尾，Home/End 要的是行首行尾，语义并不一样。
      changed = mod.caretToLineStart();
      break;
    case 'End':
      changed = mod.caretToLineEnd();
      break;
    case 'Enter':
      // 回车 = 分段（跨段编辑），不是"确认/提交"。
      //   段落模型是文档流的单位，回车在光标处断段，后面那段整体继承样式。
      //   Shift+Enter 暂时与 Enter 同义（软换行留给以后：它需要"行内换行"这种
      //   与段落不同的东西，现在没有，就不要假装有）。
      changed = mod.splitParagraph();
      break;
    case 'Tab':
      // Tab = 缩进（编辑器语义），不是"跳到下一个控件"。
      //   表单字段之间用鼠标点、或者方向键（光标本来就跨字段）。
      //   Shift+Tab = 反缩进（删掉光标前最多 tab_spaces 个空格）。
      //   走 mod.indent/outdent（*AndRender 路径）：入口改状态就必须收尾，
      //     否则宿主会拿上一次的脏矩形上屏，画布变成新旧混合体。
      changed = ev.shiftKey ? mod.outdent() : mod.indent();
      break;
    default:
      // 可打印字符（中文走 IME，这里处理 ASCII/标点/空格）
      if (ev.key.length === 1) changed = mod.typeText(ev.key);
      else handled = false;
  }

  if (!handled) return;
  ev.preventDefault();          // 别让字符再落进 textarea（那里只是键盘的承接层）

  if (changed) afterEdit();
  else resetBlink();
}

function onCompositionStart() {
  composing = true;
  ta.value = '';
  setStatus('输入法组合中…（此期间不触碰引擎）');
}

function onCompositionUpdate(ev) {
  // 预编辑串先在状态栏本地预览。
  // 把它渲染到正文位置属于档 3 —— 那时才需要"组合期间不改布局"的完整版本。
  setStatus(`组合中：${ev.data || ''}`);
}

function onCompositionEnd(ev) {
  composing = false;
  const text = ev.data || ta.value || '';
  ta.value = '';
  if (text && ready && !busy) {
    if (mod.typeText(text)) afterEdit();
  }
  setStatus('就绪');
}

function onCanvasMouseDown(ev) {
  if (!ready || busy) return;
  const r = cv.getBoundingClientRect();
  const xPx = (ev.clientX - r.left) * (cv.width / r.width);
  const yPx = (ev.clientY - r.top) * (cv.height / r.height);
  // hitTest 收的是帧磅坐标，而画布上量到的是帧像素 —— 除以 zoom 换算。
  const hit = mod.hitTest(xPx / currentZoom, yPx / currentZoom);
  if (hit !== 'null') {
    const p = JSON.parse(hit);
    // 连"这一下点的是哪一个位置"一起带过去。
    //   相邻两个字段共享边界时（[反复]│[心悸]，两枚括号并排在同一列上），落点是同一个
    //   偏移，光有偏移分不出用户点的是哪一侧；同一个偏移上还可能压着好几枚括号（相邻的
    //   空字段就是 ][][ ），于是"字与括号之间""两个控件之间"整段都点不进去。
    //   引擎在命中测试里已经算出了侧别（括号占位各占一格，数格子即可），这里只负责搬运。
    //   p.field 是它的投影，只用来显示"当前在哪个字段"，搬运请用 p.side。
    if (typeof p.side === 'number' && mod.setCaretAtSide) {
      mod.setCaretAtSide(p.para, p.offset, p.side);
      caretOn = true;
      mod.showCaret(true);
      mod.present();
      afterEdit();
    } else {
      setCaretAndPresent(p.para, p.offset);
    }
  }
  ta.focus();
  ev.preventDefault();
}

/**
 * 光标闪烁
 *
 * 这个函数只负责"把定时器重置到可见相位"。呈现与记账在 tick 里做。
 * （在这里也 present + blit 一次，结果是编辑后多跑一趟，
 * 还把面板上的数字覆盖成闪烁那一笔。）
 */
function resetBlink() {
  caretOn = true;
  if (blinkTimer) clearInterval(blinkTimer);
  blinkTimer = setInterval(() => {
    if (!ready || busy || composing) return;
    caretOn = !caretOn;
    mod.showCaret(caretOn);
    mod.present();
    // 统计必须在 present() 之后读。
    //   present 之前拿到的是上一帧的脏矩形，拿它去 putImageData 会贴错位置 ——
    //   症状是"光标走了，残影留在原地"。所以这里只读一次，且读完就用。
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    renderBlink(st);
  }, 530);
}

/**
 * 面板
 */
function renderEdit(st) {
  const pct = st.framePixels ? (100 * st.repaintedPixels) / st.framePixels : 0;
  // 三态，不能压成两态：
  //     没重排（移动光标/闪烁） / 重排了且是全量 / 重排了且增量
  //   把它压成"是否全量"的布尔，就会把"根本没重排"误报成"已经增量了"。
  const laid = !!st.layoutRan;
  // 档 2 之后"全量"不再是一个缺陷：整篇标脏（换文档、改页度量）和"脏段就是第一段"
  //   （单段文档、在开头打字）都只能全量 —— 没有前缀可复用。所以这里只报事实，
  //   增量给 ok，全量不加色（加了就成了假警报）。
  const incr = laid && !st.layoutWasFull;
  kv('edit', [
    ['本轮是否重排', laid ? '是' : '否（只重绘）', laid ? undefined : 'ok'],
    ['重排段 / 复用段 / 总段',
      `${st.relaidParagraphs} / ${st.reusedParagraphs} / ${st.totalParagraphs}`, incr ? 'ok' : undefined],
    // 复用有两种：原样搬回（后缀一个字节都没动）与整体平移（位置变了、断行没重算）。
    // 分开报是因为它们对"脏页数"的含义完全不同 —— 平移之后后缀的每一页都可能要重画。
    ['后缀复用方式', !laid ? '—' : (st.suffixShifted ? '整体平移（断行结果复用）' : '原样搬回')],
    ['重排行 / 总行', `${st.relaidLines} / ${st.totalLines}`, incr ? 'ok' : undefined],
    ['布局粒度', !laid ? '— 本轮无布局' : (st.layoutWasFull ? '全量' : '增量'),
      incr ? 'ok' : undefined],
    ['脏页 / 总页', `${st.dirtyPageCount} / ${st.totalPages}`],
    ['重画页数', st.repaintedPages],
    ['重画区域', `${st.repaintedW} × ${st.repaintedH} px`, 'hi'],
    ['重画像素', `${fmt(st.repaintedPixels, 0)}`, 'hi'],
    ['占全帧', `${fmt(pct, 3)} %`, 'hi'],
    ['— layout', `${fmt(st.layoutMs)} ms`],
    ['— pageFp', `${fmt(st.pageFpMs)} ms`],
    ['— paint', `${fmt(st.paintMs)} ms`],
    ['— 合计', `${fmt(st.totalMs)} ms`],
    ['光标', `段 ${st.caretPara} · 偏移 ${st.caretOffset} 码点`],
    ['光标矩形', `${st.caretX},${st.caretY} ${st.caretW}×${st.caretH} px`],
    ['全文码点数', st.docCodepoints],
  ]);
}

/**
 * 表单面板。
 *
 * 这一块必须放在界面上，理由是：策略生效时编辑是静默返回 false 的 ——
 * 表现就是"打字没反应"，而那与"程序卡住/出错"在观感上完全一样。
 * 「被拒编辑次数」是这两者之间唯一廉价的分辨器。
 */
function renderForm(st) {
  const MODE = ['文档', '表单', '只读'];
  const m = Number(st.editMode) || 0;
  kv('form', [
    ['编辑模式', MODE[m] || '?', m === 0 ? 'ok' : 'warn'],
    ['控件数', st.controlCount],
    ['光标所在控件', st.caretControl < 0 ? '—（不在任何控件内）' : `#${st.caretControl}`,
      st.caretControl < 0 && m === 1 ? 'warn' : ''],
    ['被拒编辑次数', fmt(st.blockedEdits, 0), st.blockedEdits > 0 ? 'warn' : 'ok'],
  ]);
}

/**
 * 光标闪烁单独一块：它和"敲字"是两条不同的路径，数字不该互相覆盖。
 * 闪烁走的是"底图一个字都没变、只把光标那一小块贴回去"，
 * 所以它的重画像素数应当是敲字的几千分之一 —— 这正是档 1 要证明的增量能力。
 */
function renderBlink(st) {
  const pct = st.framePixels ? (100 * st.repaintedPixels) / st.framePixels : 0;
  kv('blinkOut', [
    ['重画像素', fmt(st.repaintedPixels, 0), 'hi'],
    ['占全帧', `${fmt(pct, 4)} %`, 'hi'],
    ['重画区域', `${st.repaintedW} × ${st.repaintedH} px`],
    ['重画页数', st.repaintedPages],
    ['光标', st.caretVisible ? '可见' : '隐藏'],
    ['— paint', `${fmt(st.paintMs)} ms`],
  ]);
}

/**
 * 面板上"paint"那一行必须写明是谁在光栅化。
 * 两个后端在帧尺寸/重画页数/写入像素数这些指标上完全一致，
 * 写死一个名字就等着误导排查（见 wasm_main.cpp 里 rasterBackend 的注释）。
 */
function rasterBackendLabel() {
  try {
    return (mod && mod.rasterBackend) ? mod.rasterBackend() : '未知后端';
  } catch (e) {
    return '未知后端';
  }
}

function renderLayout(st) {
  kv('layout', [
    ['页数', st.pageCount],
    ['字形数', st.glyphCount, 'hi'],
    ['行 / 列数', st.placedLines],
    ['簇数', st.clusters],
    ['字体回退 run 数', st.fontRunCount],
    ['缺字（.notdef）', st.missingGlyphCount === 0 ? '0 ' : st.missingGlyphCount,
      st.missingGlyphCount === 0 ? 'ok' : 'warn'],
    ['— shape', `${fmt(st.shapeMs)} ms`],
    ['— paint（' + rasterBackendLabel() + '）', `${fmt(st.paintMs)} ms`],
    ['帧尺寸', `${mod.frameWidth()} × ${mod.frameHeight()} px`],
    ['帧字节数', kb(mod.frameByteSize())],
  ]);
}

/**
 * 引擎内循环基准
 */
function runBench() {
  if (!mod || busy) return;
  busy = true;
  $('bench').disabled = true;
  setStatus('引擎内循环 12 次…');
  try {
    const r = mod.benchRelayoutPaint(12);
    kv('benchOut', [
      ['次数', r.runs],
      ['shape 平均', `${fmt(r.shapeMs)} ms`],
      ['layout 平均', `${fmt(r.layoutMs)} ms`],
      ['paint 平均', `${fmt(r.paintMs)} ms`, 'hi'],
      ['每帧合计', `${fmt(r.totalMs)} ms`, 'hi'],
      ['理论帧率（仅排版+光栅）', `${fmt(1000 / r.totalMs, 0)} fps`],
    ]);
    setStatus(`基准完成 · 每帧 ${fmt(r.totalMs, 0)} ms`);
  } catch (e) {
    setStatus(e.message, true);
    console.error(e);
  } finally {
    busy = false;
    $('bench').disabled = false;
  }
}

function wasmBytes() {
  let n = 0;
  for (const e of performance.getEntriesByType('resource')) {
    if (e.name.includes('heditor')) n += e.transferSize || e.decodedBodySize || 0;
  }
  return n;
}

// 对外诊断接口
//
// 这些接口是给无头探针（scripts/edit_probe.mjs）用的，所以刻意分成两组，
//   不要合并：
//     layout*  —— 引擎本次排版的统计（页数/字形数/缺字/分阶段耗时）
//     edit*    —— 引擎本次编辑的统计（重排行数/重画像素/光标矩形）
//   两组的数字来源完全不同；把它们混在一个对象里，探针就会拿"页数"去断言
//   一个根本不存在的字段，然后静默通过。
window.__heditor = {
  get ready() { return ready; },
  get errors() { return pageErrors.slice(); },
  get edit() { return lastEdit; },
  get text() { return mod ? mod.documentText(0) : ''; },
  // 按段取文本 + 整篇拼起来。跨段编辑的探针要核对"字符账守恒"（分段不丢不重），
  //   而 __heditor.text 只给第 0 段。
  paragraphText(i) { return mod ? mod.documentText(i) : ''; },
  get allText() {
    if (!mod) return '';
    const n = JSON.parse(mod.editStatsJson()).totalParagraphs;
    const parts = [];
    for (let i = 0; i < n; ++i) parts.push(mod.documentText(i));
    return parts.join('\n');
  },
  get caret() {
    if (!mod) return null;
    const st = JSON.parse(mod.editStatsJson());
    return { para: st.caretPara, offset: st.caretOffset, index: st.caretIndex,
             side: st.caretSide,
             x: st.caretX, y: st.caretY, w: st.caretW, h: st.caretH,
             visible: st.caretVisible, docCodepoints: st.docCodepoints };
  },
  get caretReport() { return mod ? mod.caretReport() : ''; },
  get env() {
    return { wasmGlue: wasmBytes(), ua: navigator.userAgent, url: location.href };
  },
  // 兼容旧探针的字段名
  get fonts() { return this.env; },
  // 排版统计（= statsJson）
  get stats() { return mod ? JSON.parse(mod.statsJson()) : null; },
  // 编辑统计（= editStatsJson）
  get editStats() { return mod ? JSON.parse(mod.editStatsJson()) : null; },

  // 压力文档：造 N 段、每段 M 字。性能判据只能在大文档上做 —— 演示场景只有一段，
  //   量出来的编译与索引耗时全是 0.0x ms，看不出 O(文档) 的那一层。
  loadStressDocument(paras, chars) {
    if (!ready) return null;
    mod.loadStressDocument(paras, chars);
    relayoutAndBlit();
    return this.editStats;
  },

  // 切场景并按 zoom 重排重绘（供探针用；走的是和 UI 完全相同的路径）
  //
  // 返回值就是"本次全量呈现"的统计 —— 探针要拿它当增量比值的分母。
  //   分母只能是真正的全量，不能取"再 present 一次"的数字：后者本身已经是增量
  //   （帧尺寸没变，只贴了个旧光标），拿它当分母会得出"100%"这种荒谬结论。
  run(id, zoom) {
    if (zoom) {
      currentZoom = Number(zoom);
      $('zoom').value = String(zoom);
      $('zoomVal').textContent = currentZoom.toFixed(2).replace(/0$/, '');
      mod.setZoom(currentZoom);
    }
    mod.setScenario(Number(id));
    mod.relayoutAndPaint();
    const st = JSON.parse(mod.editStatsJson());
    lastEdit = st;
    blitFrame(st);
    renderEdit(st);
    renderForm(st);
    renderLayout(JSON.parse(mod.statsJson()));
    return st;
  },
  reset(id, zoom) { return this.run(id === undefined ? 6 : id, zoom === undefined ? 1.5 : zoom); },

  // 直接调引擎（绕开键盘），走和真人编辑相同的上屏步骤。
  //
  //  这里必须 blitFrame，不能只调引擎。
  //    只调引擎的话，画布还停在旧内容上，而引擎内部已经画完了 ——
  //    紧接着任何一次"只有光标脏"的 present（比如闪烁 tick）只会贴光标那一小块，
  //    画布于是永远停在旧文字上，症状是"光标往后走、插入处的字不动、两个叠在一起"。
  //    探针 [2b] 的 __heditor.selfCheck() 当场量出过 9372 个像素不一致。
  //    探针钩子必须和真人路径一样把画面收尾，否则它自己就是 bug 的制造者。
  type(s) {
    if (!ready || !mod.typeText(s)) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  backspace() {
    if (!ready || !mod.backspace()) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  deleteForward() {
    if (!ready || !mod.deleteForward()) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  // 跨段编辑：段首退格 / 段末 Delete 已经自动走合并（见引擎 backspace/deleteForward），
  //   所以这里只需要给回车与"显式合并"各一个入口。
  splitParagraph() {
    if (!ready || !mod.splitParagraph()) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  mergeWithPreviousParagraph() {
    if (!ready || !mod.mergeWithPreviousParagraph()) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  mergeWithNextParagraph() {
    if (!ready || !mod.mergeWithNextParagraph()) return null;
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },
  // 返回 null 表示"光标没动"（引擎返回 false 就不重绘）——
  //   探针据此断言"已经在段首了，再左移不会重绘"。
  move(dcp) {
    if (!ready || !mod.moveCaret(dcp || 1)) return null;
    return JSON.parse(mod.editStatsJson());
  },
  // 走的是和"点画布"完全相同的路径（改位置 + 呈现），
  //   所以探针拿到的统计和真人操作得到的是一回事。
  setCaret(para, offset) { setCaretAndPresent(para, offset); return this.caret; },
  // 带归属的定位 —— 与真人点画布走的同一条路径（hitTest 给出归属，
  //   宿主原样回传）。探针要验证"点后一个字段的开头"就必须走它，走 setCaret 是另一条路。
  setCaretAt(para, offset, field) {
    if (!ready) return null;
    mod.setCaretAt(para, offset, field);
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    afterEdit();
    return this.caret;
  },
  // 带侧别的定位 —— 与点画布完全相同的那条路（onCanvasMouseDown 搬的就是侧别）。
  //   归属只是侧别的投影，同一个偏移上压着好几枚括号时投影会抹平细节，所以
  //   探针要复现"点在两个控件之间"这类手势，得走这一个而不是 setCaretAt。
  setCaretAtSide(para, offset, side) {
    if (!ready) return null;
    mod.setCaretAtSide(para, offset, side);
    caretOn = true;
    mod.showCaret(true);
    mod.present();
    afterEdit();
    return this.caret;
  },
  // 点画布的真实入口（与 cv 的 mousedown 处理同一套坐标换算），供探针复现用户手势。
  clickAt(xPx, yPx) {
    if (!ready) return null;
    const r = cv.getBoundingClientRect();
    cv.dispatchEvent(new MouseEvent('mousedown', {
      clientX: r.left + xPx * (r.width / cv.width),
      clientY: r.top + yPx * (r.height / cv.height),
      bubbles: true, cancelable: true,
    }));
    return this.caret;
  },
  // 只改位置不呈现 —— 仅用于需要把"改位置"和"呈现"分账的对照实验
  setCaretOnly(para, offset) { mod.setCaret(para, offset); return this.caret; },
  hit(xPt, yPt) { return JSON.parse(mod.hitTest(xPt, yPt)); },
  bench(runs) { return JSON.parse(mod.benchRelayoutPaint(runs || 12)); },

  // 表单：控件是区间，边界是策略
  //
  // 这三个 setter 都走 present() 收尾 —— 和真人路径一样。
  //   只调引擎不收尾，画布就会停在旧内容上（同上，必须在同一个调用里收尾）。
  get editMode() { return mod ? mod.editMode() : 0; },
  get controls() { return mod ? JSON.parse(mod.controlsJson()) : null; },
  // 占位槽清单：每一枚括号落在哪个偏移、是开还是闭、属于哪个字段、在哪一行。
  //   括号的排布只存在于排版结果里（区间数据看不出来），探针要钉住
  //   "两个零宽字段的括号交替成 ][][]、而不是互相嵌套" 就得读这一份。
  get bracketSlots() { return mod ? JSON.parse(mod.bracketSlotsJson()).slots : null; },
  // 直读，不走 editStats：编辑被拒时不会 repaint，而 editStats 是在呈现时刷新的 ——
  //   走那条路读到的永远是上一拍的数字，探针按键后立刻读会差一拍。
  get blockedEdits() { return mod ? mod.blockedEdits() : 0; },
  setEditMode(m) {
    mod.setEditMode(Number(m));
    return this.present();
  },
  // 括号显示范围：0 = All（默认，所有字段常驻）、1 = Focused（旧行为，只显示光标所在字段）。
  // 引擎侧会自己重排（括号进文档流，占位变了），宿主只需上屏。
  setBracketScope(s) {
    mod.setBracketScope(Number(s));
    return this.present();
  },
  get bracketScope() { return mod ? mod.bracketScope() : 0; },
  // 用一份区间清单整批重建控件（探针用）。
  //   为什么不逐个改：区间树的父子关系是全局性质，改一个字段的端点会改变别的
  //   字段的 depth/parent，必须整棵重建。只读标记也只有建的时候给得进去，
  //   所以"把某个字段临时标成只读、看它那一对括号变不变灰"要靠这条。
  //   引擎那边一次建完（换名单 + 重建树在同一次调用里），中间态不存在，
  //   光标连侧别由引擎自己搬 —— 宿主不必再存一份。
  setFormControls(list) {
    mod.setFormControls(list.map((it) => ({
      start: it.start, end: it.end, kind: it.kind || 0,
      id: it.id || '', ro: !!it.ro,
    })));
    return relayoutAndBlit();
  },
  // 走 relayoutAndBlit 而不是 present()：loadFormSample 会换文档，
  //   必须重排。只 present 的话，控件区间会投影到上一次布局的行盒上
  //   （实测症状：4 个字的字段被切成两块，因为它在旧几何里跨了行）。
  loadFormSample(variant) {
    mod.loadFormSample(variant === undefined ? 0 : variant);
    return relayoutAndBlit();
  },
  // 帧自审：当前帧 vs "整帧重画一遍"，逐字节比。
  //   selfCheck 只能抓"上屏贴错"；这个能抓"引擎帧本身就是错的"（旧标记没擦掉）。
  //   跑完会顺带把这一帧修成真值。
  auditFrame() { return mod ? JSON.parse(mod.auditFrame()) : null; },

  // 排版自审：增量续排的结果 vs 一次全量重排，逐字段比对（diff 必须为 0）。
  //   顺便带回两个耗时：curMs 是刚才那次增量，fullMs 是本次审计做的全量 ——
  //   分子分母同一次测量，探针拿它们算"增量是全量的百分之几"。
  auditLayout() { return mod && mod.auditLayout ? JSON.parse(mod.auditLayout()) : null; },

  // 编译自审：本轮实际编译耗时与一次整树全量编译同源对照。
  //   这是"编译这一层有没有做到增量"的唯一证据 —— 只有分子没有分母的耗时数说明不了事。
  auditCompile() { return mod && mod.auditCompile ? JSON.parse(mod.auditCompile()) : null; },

  // 直读"待上屏"矩形（读后清零）—— 探针用它核对"引擎写了多少、宿主该贴多少"。
  get blitBox() { return mod && mod.takeBlitBox ? JSON.parse(mod.takeBlitBox()) : null; },

  // 给探针用的：直接摆光标可见相位（真实闪烁由定时器摆，探针要能自己定住）。
  //   存在的理由：用户的 bug 只在"光标停在隐藏那半拍"时触发 ——
  //   而所有老断言开头都调过 stopBlink()，正好把这一半概率全屏蔽掉了。
  showCaret(on) { if (!mod) return false; mod.showCaret(!!on); return true; },

  // Tab / Shift+Tab = 缩进 / 反缩进（走真实引擎路径，会自己收尾）
  indent() { if (!mod.indent()) return null; return this.present(); },
  outdent() { if (!mod.outdent()) return null; return this.present(); },

  // 返回 null = 光标没动（比如只有一个字段）
  // 注意：Tab 不再绑到它。Tab 是缩进。这个接口留着给探针/自动化用。
  focusControl(forward) {
    if (!mod.focusControl(forward === undefined ? true : forward)) return null;
    return this.present();
  },
  // 策略版 Tab：表单模式在可编辑控件之间前进/后退，到最后一个返回 null（不消费）。
  //   与 focusControl 的区别是"不绕回第一个"且判据走可编辑集合谓词（见 engine.h）。
  //   默认键位同样不绑它 —— 裸 Tab 留给缩进，绑什么由宿主决定。
  tabCaret(backward) {
    if (!mod.tabCaret(backward === undefined ? false : !!backward)) return null;
    return this.present();
  },
  controlAt(xPt, yPt) { return mod.controlAtPoint(xPt, yPt); },
  describeControls() { return mod ? mod.describeControls() : ''; },

  // 给探针用的：把引擎当前那一帧原样拷出来
  //
  //  必须拷一份，不能直接返回 mod.frameRgba()：那返回的是 wasm 堆上的视图，
  //    任何一次 present（哪怕只是光标闪烁的一 tick）都会把它原地覆盖。
  //    探针要拿"一个一个敲进去"和"一次性插入"这两帧做逐字节比较，
  //    不拷的话两次读到的永远是同一块内存 —— 这条断言会永远绿，且毫无意义。
  rawFrame() {
    const v = mod.frameRgba();
    return new Uint8ClampedArray(new Uint8ClampedArray(v.buffer, v.byteOffset, v.byteLength));
  },
  frameW() { return mod.frameWidth(); },
  frameH() { return mod.frameHeight(); },

  // 按当前光标可见相位重画一次并上屏。
  //
  //  探针需要它来"定格相位"：闪烁每 530ms 自己 present 一次，
  //    两次采样若各自落在不同半拍上，比对出来的差异里就混着闪烁，
  //    真正的渲染差异反而被淹掉。定格之后两帧才有可比性。
  present() {
    if (!ready) return null;
    mod.present();
    const st = JSON.parse(mod.editStatsJson());
    blitFrame(st);
    return st;
  },

  // 一键自检：把屏幕上真实的像素和引擎那一帧逐字节比一遍，外加缩放/DPI 现场。
  //
  // 存在的理由："输入时光标走了、插入处的字没走、两个叠在一起"，
  //   在开发机上一比一复现不出来（连打 60 字 + 闪烁在跑 + 110ms 节奏，
  //   屏幕与引擎帧逐字节相同）。既然症状是"看到的和引擎画的不一致"，
  //   那就在出问题的那台机器上当场量一次，比来回猜快得多。
  //
  //   用法：F12 控制台 →  __heditor.selfCheck()  把返回的对象发出来。
  //   screenVsEngineDiffPx > 0 就说明"上屏"这一步贴错了（引擎帧本身是对的）；
  //   恒为 0 就说明问题在引擎的排版/光栅化，而不在上屏。
  selfCheck() {
    if (!ready) return null;
    const w = mod.frameWidth();
    const h = mod.frameHeight();
    const g = cv.getContext('2d', { willReadFrequently: true });
    const S = g.getImageData(0, 0, w, h).data;
    const F = mod.frameRgba();
    const n = Math.min(S.length, F.length);
    let diff = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
    for (let i = 0; i + 3 < n; i += 4) {
      if (S[i] !== F[i] || S[i + 1] !== F[i + 1] || S[i + 2] !== F[i + 2]) {
        ++diff;
        const p = i >> 2, x = p % w, y = (p - x) / w;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
    }
    const st = JSON.parse(mod.editStatsJson());
    const r = cv.getBoundingClientRect();
    return {
      text: mod.documentText(0),
      docCodepoints: st.docCodepoints,
      caret: { para: st.caretPara, offset: st.caretOffset, x: st.caretX, y: st.caretY,
               w: st.caretW, h: st.caretH, visible: st.caretVisible },
      framePx: [w, h],
      canvas: {
        backing: [cv.width, cv.height],
        cssSize: [cv.style.width, cv.style.height],
        rectPx: [Math.round(r.width), Math.round(r.height)],
      },
      dpr: window.devicePixelRatio,
      zoom: currentZoom,
      backend: mod.rasterBackend ? mod.rasterBackend() : '?',
      screenVsEngineDiffPx: diff,
      screenVsEngineDiffBox: diff ? [minx, miny, maxx, maxy] : null,
      repainted: { pages: st.repaintedPages, pixels: st.repaintedPixels,
                   w: st.repaintedW, h: st.repaintedH },
    };
  },

  // 表单标记自检：当场核对"每个字段的括号都画出来了"
  //
  // 给用户的取证入口。是"有时括号渲染不更新" —— 这类间歇性
  //   问题在开发机上复现不出来，所以把判据做成一调用，让他在出问题的那台机器上量。
  //
  // 判据（默认档 All）：每个可编辑字段的左缘外侧都必须有括号墨迹，
  //   与光标在哪无关（括号常驻）。Focused 档（scope=1）下才退回旧判据：
  //   聚焦字段有括号、其余为空。
  //   用法：F12 控制台 →  __heditor.markCheck()   把返回的对象发出来。
  //   allOk=false 就说明确实有字段没画对，rows 里能看到是哪个、差多少。
  markCheck() {
    if (!ready || !mod) return null;
    const cc = JSON.parse(mod.controlsJson());
    const f = this.rawFrame();
    const fw = this.frameW();
    const focusedOnly = this.bracketScope === 1;
    // 与探针同一条判据：偏蓝够远 + 够亮 + 红通道 > 45（把光标色排除掉）
    const near = (r, g, b) => r > 45 && b - r > 60 && b > 140;
    const rows = [];
    for (let ci = 0; ci < cc.items.length; ++ci) {
      const it = cc.items[ci];
      if (it.ro || !it.rects.length) continue;
      const cx = it.rects[0].x;
      const lo = Math.round((cx - 10) * currentZoom);
      const hi = Math.round((cx - 1) * currentZoom);
      let n = 0;
      for (let i = 0; i + 3 < f.length; i += 4) {
        const x = (i >> 2) % fw;
        if (x < lo || x > hi) continue;
        if (near(f[i], f[i + 1], f[i + 2])) ++n;
      }
      const focused = ci === cc.caret;
      rows.push({
        control: ci,
        focused,
        leftEdgePx: n,
        ok: focusedOnly ? (focused ? n > 0 : n === 0) : n > 0,
      });
    }
    return {
      mode: cc.mode,
      bracketScope: this.bracketScope,
      count: cc.count,
      caretControl: cc.caret,
      zoom: currentZoom,
      caret: this.caret,
      rows,
      allOk: rows.every((r) => r.ok),
    };
  },

  // 给探针用的：把闪烁停掉
  //  必须有这个口子。闪烁每 530ms 会 present() 一次并覆盖 edit_stats_；
  //   探针"派事件 → 读统计"之间只要落进一次 tick，读到的就是闪烁那一笔
  //   （几百像素而不是整页），断言会随机失败 —— 那种测试比没有更糟。
  //
  //  这里不做 present()：present 会把 edit_stats_ 覆盖成"只重画了个光标"，
  //   而探针紧接着要读的往往是"上一次全量呈现"的数字（分母）。停表就只停表。
  stopBlink() {
    if (blinkTimer) { clearInterval(blinkTimer); blinkTimer = null; }
    caretOn = true;
    mod.showCaret(true);
    return true;
  },
  startBlink() { resetBlink(); return true; },

  // 引擎实时光标偏移，直读不经过 editStats。
  //   editStats 是"上一次呈现"写下的账；拿它和这个比，就能判出
  //   "有没有哪个改了状态的入口忘了给自己收尾"（Tab 跳字段就栽在这上面）。
  get caretLive() { return mod ? mod.caretOffsetLive() : -1; },

  // 给探针用的 DOM 句柄（键盘路径要往真实元素上派事件，不能直接调引擎）
  get textarea() { return ta; },
  get canvas() { return cv; },
  get zoomLevel() { return currentZoom; },
  get scenario() { return Number($('scenario').value); },
};

main();
async function main() {
  try {
    await boot();
  } catch (e) {
    setStatus(String(e.message || e), true);
    $('note').innerHTML =
      `<b>启动失败。</b><br>${String(e.message || e).replace(/\n/g, '<br>')}`;
    console.error(e);
  }
}
