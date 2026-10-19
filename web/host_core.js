// 两个 Web 宿主共用的输入层：键位映射、命中归宿、连击计数、组合提交、指针记账、
// 屏幕像素比跟随。
//
// 这一层能共用的判据是"它决定的是这一下算哪条命令"。至于画布归谁、帧往哪贴、
// 命中要不要等一次往返，那是传输层的事，从环境对象进来（见 createInput）。
//
// 两条纪律：
//   这里不出现引擎句柄，也不出现具体画布元素 —— 一旦出现，两个宿主又各有一套。
//   写入一律按"同步值或 Promise"处理（then1），不把同步宿主逼成微任务：
//   单线程宿主靠"调用即生效"作为对照物，那条性质不能为了统一形状而丢掉。
(function (global) {
  'use strict';

  /**
   * 把同步值与 Promise 折成一条链。
   *
   * 不用 Promise.resolve 包一层：那会把同步宿主也推进微任务，于是"输入事件处理完
   * 就已生效"这条性质消失，探针的同步假设连带失效（一轮下来耗时数倍、增量断言变红）。
   * 同步值当场回调，Promise 才等 —— 两个宿主的执行时序各自保持原样。
   */
  function then1(v, fn) {
    return (v && typeof v.then === 'function') ? v.then(fn) : fn(v);
  }

  /**
   * 连击计数。st 由调用方持有（{ seq, time, x, y }）。
   *
   * 单线程宿主的 mousedown 本来带 detail，仍然自己数：另一侧挂在 pointerdown 上，
   * detail 恒为 0。两处各判一套，迟早出现"这边三击能选、那边没反应"。
   */
  function clickCountOf(ev, st) {
    const now = (typeof performance !== 'undefined' ? performance.now() : Date.now());
    const near = st.seq > 0 && now - st.time <= 520 &&
                 Math.abs(ev.clientX - st.x) <= 4 && Math.abs(ev.clientY - st.y) <= 4;
    st.seq = near ? st.seq + 1 : 1;
    st.time = now;
    st.x = ev.clientX;
    st.y = ev.clientY;
    return st.seq;
  }

  /**
   * markdown 还原成纯段落。
   *
   * 病历样例走的是"服务端下发一份文本"，两份实现一旦分叉，同一份样例在两个宿主上
   * 会排出不同段数 —— 而段落数是探针的基准量之一。
   */
  function cleanMarkdownLines(raw) {
    return raw
      .split('\n')
      .map((l) =>
        l
          .replace(/\*\*/g, '')
          .replace(/\s*\\$/, '')
          .replace(/^#+\s*/, '')
          .replace(/^\d+[.、]\s*/, '')
          .trim())
      .filter((l) => l.length > 0)
      .join('\n');
  }

  /**
   * 跟随屏幕像素比。返回摘除函数。
   *
   * 判据取 devicePixelRatio 本身，不取 resize 事件：拖窗口大小也会来 resize，
   * 那时 DPR 没变，重排一遍纯属白花。媒体查询按 Ndppx 精确匹配，每变一次要重挂，
   * 否则下一次不再触发。
   */
  function armDprWatch(onChange) {
    let q = null;
    const arm = () => {
      if (q) q.removeEventListener('change', onRehit);
      q = global.matchMedia('(resolution: ' + (global.devicePixelRatio || 1) + 'dppx)');
      q.addEventListener('change', onRehit);
    };
    function onRehit() {
      arm();
      onChange(global.devicePixelRatio || 1);
    }
    arm();
    return () => { if (q) q.removeEventListener('change', onRehit); q = null; };
  }

  /**
   * 键位到命令。
   *
   * ctx.vertical / ctx.blockReversed 由宿主的热读给出（单线程读引擎、异步宿主读镜像）。
   * 返回 [命令名, 参数] 或 null；null 表示放行给浏览器。
   *
   * 方向键按物理方向映射：横排的行内轴是水平的、块轴是竖直的，所以左右走字、上下换行。
   * 竖排正好翻过来：上下走字、左右换列。换列往哪一边由列序定 —— 中日竖排列自右向左
   * （左 = 下一列），蒙文列自左向右（右 = 下一列）。只按"是竖排吗"映射，蒙文的左右
   * 就会按反。参考行为是 Chrome 在 vertical-rl 的 contenteditable 里的实测结果。
   */
  function keyToOp(ev, ctx) {
    if ((ev.ctrlKey || ev.metaKey) && !ev.altKey) {
      const k = (ev.key || '').toLowerCase();
      // 重做收两个键位：Ctrl+Y 与 Ctrl+Shift+Z。比较前转小写 —— shift 按下时给的是大写 Z。
      // Cmd 一并认下：Mac 上 Command 才是惯用修饰键。
      if (k === 'z' || k === 'y') return [ev.shiftKey || k === 'y' ? 'redo' : 'undo', {}];
      // 全选与"到文档首尾"必须在这里拦下。放行给浏览器时，全选选中的是承接层里那一行
      // 假文本（空的），与文档毫无关系；Ctrl+Home/End 则会把整页滚到顶/底。
      if (k === 'a') return ['selectAll', {}];
      if (ev.key === 'Home') return ['docStart', { extend: ev.shiftKey }];
      if (ev.key === 'End') return ['docEnd', { extend: ev.shiftKey }];
    }
    if (ev.ctrlKey || ev.metaKey || ev.altKey) return null;
    const c = ctx || {};
    const dirLeft = c.blockReversed ? 1 : -1;   // 物理向左 = 行序 +1 还是 -1
    switch (ev.key) {
      case 'Backspace': return ['backspace', {}];
      case 'Delete': return ['deleteForward', {}];
      // extend = Shift：锚点不动，选区从锚点那一端继续长。
      case 'ArrowLeft':
        return c.vertical ? ['vertical', { dir: dirLeft, extend: ev.shiftKey }]
                          : ['move', { dcp: -1, extend: ev.shiftKey }];
      case 'ArrowRight':
        return c.vertical ? ['vertical', { dir: -dirLeft, extend: ev.shiftKey }]
                          : ['move', { dcp: 1, extend: ev.shiftKey }];
      case 'ArrowUp':
        return c.vertical ? ['move', { dcp: -1, extend: ev.shiftKey }]
                          : ['vertical', { dir: -1, extend: ev.shiftKey }];
      case 'ArrowDown':
        return c.vertical ? ['move', { dcp: 1, extend: ev.shiftKey }]
                          : ['vertical', { dir: 1, extend: ev.shiftKey }];
      // 行首行尾走引擎的现成命令，不用 moveCaret(-1e9) 凑：那个表达的语义是段首段尾。
      case 'Home': return ['lineStart', { extend: ev.shiftKey }];
      case 'End': return ['lineEnd', { extend: ev.shiftKey }];
      // 回车 = 分段（跨段编辑），不是"确认/提交"。段落模型是文档流的单位，
      // 回车在光标处断段，后面那段整体继承样式。软换行要的是"行内换行"这种与段落
      // 不同的东西，现在没有，就不假装有。
      case 'Enter': return ['splitParagraph', {}];
      // Tab = 缩进（编辑器语义），不是"跳到下一个控件"。
      case 'Tab': return [ev.shiftKey ? 'outdent' : 'indent', {}];
      default:
        // 可打印字符（ASCII / 标点 / 空格）直接在 keydown 里提交，并挡掉浏览器往
        // 承接层里的插入。不能指望 input 事件：合成 KeyboardEvent 根本不产生 input，
        // 真键盘下又因为这里挡掉了插入 —— 两头都不成立，打字会静默地什么都不发生。
        if (ev.key && ev.key.length === 1) return ['type', { s: ev.key }];
        return null;
    }
  }

  /**
   * 命中结果到落点。
   *
   * 三种归宿，判据都在引擎给出的字段上：
   *   cellPara 有值 —— 点的是格子边距带，这一下是"选整格"，不是落光标；
   *   side 有值 —— 落光标并带上侧别；相邻字段共享边界时同一个偏移上压着好几枚括号，
   *               光有偏移分不出点的是哪一侧，两个控件之间整段都点不进去；
   *   line 与 side 同时给 —— 折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
   *               不带行别，点下一行行首会画到上一行行尾。
   * 搬的是段号而不是流区间：空格的内容长度为 0，流区间分不出是哪一格。
   */
  function hitToAction(hit) {
    if (!hit) return null;
    if (typeof hit.cellPara === 'number') return { kind: 'cell', para: hit.cellPara | 0 };
    const act = { kind: 'caret', para: hit.para, offset: hit.offset };
    if (typeof hit.side === 'number') {
      act.side = hit.side;
      if (typeof hit.line === 'number') act.line = hit.line | 0;
    }
    return act;
  }

  /**
   * 缩放换算。口径只有一条：引擎收"密度"（设备像素 / 磅，1 = 72 dpi），
   * 界面收"倍数"（1 = 100% = 96 dpi），两者差 4 / 3。
   *
   * 两个 DPR 必须分开记，这是这一层唯一的难点：
   *   liveDpr   当下屏幕的像素比。算"基准密度"与"倍数转密度"用它 —— 换屏之后新算的
   *             密度就该按新屏幕来。
   *   storedDpr 现有这个密度是按哪个屏幕算出来的。反推倍数用它 —— 换屏那一拍两者不同，
   *             拿新 DPR 去除会读到一个平白缩掉一半的假倍数，照它反推回去等于空操作。
   * 所以 set(density) 要同时把 storedDpr 记成"算这一刻用的那个屏幕"。
   */
  function createZoom(opts) {
    const base = (opts && opts.base) || 4 / 3;
    const liveDpr = (opts && opts.dpr) || (() => global.devicePixelRatio || 1);
    let storedDpr = liveDpr();
    let density = base * storedDpr;
    return {
      // 当前密度（设备像素 / 磅）。
      get density() { return density; },
      // 现有密度是按哪个屏幕算的。换屏判断要跟它比，不要跟当下 DPR 比。
      get dpr() { return storedDpr; },
      // 当下屏幕的基准密度：倍数 1 对应的密度。
      base() { return base * liveDpr(); },
      // 倍数转密度。
      ofFactor(f) { return Number(f) * base * liveDpr(); },
      // 密度转倍数。
      factorOf(d) { return Number(d) / (base * storedDpr); },
      // 改密度。返回新密度，宿主只管把它存进自己的变量。
      set(d) { storedDpr = liveDpr(); density = Number(d); return density; },
    };
  }

  /**
   * 输入层。环境对象（宿主的差异全部从这里进来）：
   *
   *   isReady() / isBusy() / ta / hot()           就绪、忙、承接层、{ vertical, blockReversed }
   *   write(name, args) / hit(xPt, yPt)           发命令、命中；返回布尔/对象或 Promise
   *   pointOf(ev) / zoom()                        客户端坐标到文档设备像素、折磅除数
   *   capturePointer() / beginDrag(ev) / focus()  指针记账、起手、把焦点交给承接层
   *   commit(changed)                             键位与组合结束后的收尾
   *   place(act) / selectCellPara(para) / cellSelected()   落光标、落整格、三击收尾
   *   status(text) / onChain(p)                   状态栏、登记在途链（可选）
   */
  function createInput(env) {
    const click = { seq: 0, time: 0, x: 0, y: 0 };
    let composing = false;
    const isBusy = () => (typeof env.isBusy === 'function' ? env.isBusy() : false);
    const keep = (p) => { if (typeof env.onChain === 'function') env.onChain(p); return p; };
    const beginDrag = (ev) => { if (typeof env.beginDrag === 'function') env.beginDrag(ev); };
    const armDrag = (on) => { if (env.drag) env.drag.arm(on); };

    // 命中之后的两条去向：定位、以及"这一下要不要起拖选"。
    // 落在正文之外时不起手 —— 从页外空白拖起不该选中任何东西。
    //
    // 定位这一步必须等：异步宿主的落光标是一次往返，不等它，clickAt 这类入口会在
    // 光标还没摆好时就 resolve，调用方读到的光标是上一拍的。
    const placeAt = (pt) => then1(env.hit(pt.x / env.zoom(), pt.y / env.zoom()), (h) => {
      const act = hitToAction(h);
      if (!act) { armDrag(false); return null; }
      if (act.kind === 'cell') {
        // 整格选区之后不再拖：这一下表达的是"我要这一格"，继续拖会把焦点端
        // 搬到别的格上去，与文字选区那条路混成一件说不清的事。
        const done = env.selectCellPara(act.para);
        armDrag(false);
        return then1(done, () => h);
      }
      const done = env.place(act);
      armDrag(true);
      return then1(done, () => h);
    });

    function onKeyDown(ev) {
      if (!env.isReady() || isBusy()) return undefined;
      // 组合期间一律不动文档：输入法在组合时会持续发 keydown，每次都重排会让画面
      // 又卡又闪，这是 WASM 编辑器最容易出问题的一处。
      if (composing || ev.isComposing) return undefined;
      const op = keyToOp(ev, env.hot());
      if (!op) return undefined;              // 不认识的键（含 IME 的 Process 键）放行
      ev.preventDefault();
      return keep(then1(env.write(op[0], op[1]), (changed) => env.commit(!!changed)));
    }

    function onCompositionStart() {
      composing = true;
      env.ta.value = '';
      env.status('输入法组合中…（此期间不触碰引擎）');
    }

    function onCompositionUpdate(ev) {
      // 预编辑串先在状态栏本地预览。渲染进正文需要"组合期间不改布局"的完整版本，
      // 现在没有那一档。
      env.status('组合中：' + (ev.data || ''));
    }

    function onCompositionEnd(ev) {
      composing = false;
      const text = ev.data || env.ta.value || '';
      env.ta.value = '';
      const ready = env.isReady() && !isBusy();
      const w = (text && ready) ? env.write('type', { s: text }) : false;
      return keep(then1(w, (changed) => {
        if (changed) env.commit(true);
        env.status('就绪');
      }));
    }

    function onPointerDown(ev) {
      if (!env.isReady() || isBusy()) return undefined;
      if (ev.button !== 0) return undefined;
      const pt = env.pointOf(ev);
      if (!pt) return undefined;
      env.capturePointer();
      const z = env.zoom();
      const triple = clickCountOf(ev, click) === 3;
      let chain;
      if (triple) {
        click.seq = 0;
        // 三击：选整格。走命令而不是查询 —— 选整格会重画帧，查询只回一个值、不重贴
        // 脏页，画面于是停在旧帧上，看起来是"点了要等一秒才选中"。点不在表格里时
        // 退回正常落光标，所以 beginDrag 放在回调里而不是前面。
        chain = then1(env.write('cellSelectAt', { x: pt.x / z, y: pt.y / z }), (ok) => {
          if (ok) { env.cellSelected(); return null; }
          beginDrag(ev);
          return placeAt(pt);
        });
      } else {
        // 监听必须当场挂上：命中在异步宿主上是往返，等它回来才起手的话，
        // 按下与第一次移动之间的移动会被整段丢掉，而那一段往往正是"按下去就往右拖"。
        beginDrag(ev);
        chain = placeAt(pt);
      }
      // 末尾那次 focus 是这条路的必要条件：canvas 与 div 都不可聚焦，浏览器在 mousedown
      // 的默认行为里会把 activeElement 打回 body，此后 keydown 再也到不了宿主。
      env.focus();
      ev.preventDefault();
      return keep(chain);
    }

    return {
      onKeyDown, onCompositionStart, onCompositionUpdate, onCompositionEnd,
      onPointerDown, placeAt, clickState: click,
      get composing() { return composing; },
    };
  }

  global.HEditorCore = {
    then1, clickCountOf, cleanMarkdownLines, armDprWatch, keyToOp, hitToAction,
    createZoom, createInput,
  };
})(typeof self !== 'undefined' ? self : this);
