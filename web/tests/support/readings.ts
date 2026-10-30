// 判据片段的读数前言：每个页面求值前拼在它前面。
//
// 为什么还需要这一层：公开面给的是"库的语义"，而 367 条判据是按"扁平读数 + 全局流位置"
//   写的 —— 光标要段号/偏移/页号/页内矩形一起读，选区要两端流位置（是数字，不是 Pos），
//   缩放要密度（设备像素 / 磅）而不是倍数。这些摊平属于"测试决定自己要看什么"，所以放在
//   测试侧，不放页面里。
//
// 与刚拆掉的那层迁移垫片的区别，三点都要守住：
//   一、不挂 window 全局、不注入脚本 —— 这些名字只活在一次求值的函数体里；
//   二、成员只有下面这几个，旧门面有 66 个，把公开面与诊断面的边界糊成一片；
//   三、名字是测试语义（CARET / SEL / FRAME / ZOOM），不是旧宿主的方法名。
//
// 页面上的入口只有一个：示例页自己挂的 window.__demo（真实使用者的句柄），
//   诊断面是它的 diag 属性，需要页面带 debug=1 打开。
export const PRELUDE = `
  const H = window.__demo;
  const D = window.__demo.diag;
  // 宿主骨架全部从公开面的 element 里找，不认页面容器叫什么 id ——
  //   .heditor-stage / .heditor-pages / .heditor-ime 是库自己造的（src/host/dom.ts 的 ensureMarkup），
  //   外层容器是页面的，测试不该知道它叫 #ed。
  const EL = (sel) => H.element.querySelector(sel);
  // 承接层：键盘与合成事件都落在这个 textarea 上。
  const IME = () => EL('.heditor-ime');
  const MODES = ['document', 'form', 'readOnly'];
  // 扁平光标：段号 + 段内偏移 + 页号 + 页内插入点矩形 + 侧别 + 全文码点数。
  const CARET = () => {
    const c = H.caret();
    const st = D.editStats() || {};
    return {
      para: c.pos.para, offset: c.pos.offset, page: c.page,
      x: c.rect.x, y: c.rect.y, w: c.rect.w, h: c.rect.h,
      visible: c.visible, side: st.caretSide, docCodepoints: st.docCodepoints,
    };
  };
  // 选区：判据比较的是全局流位置，公开面的 selection() 给的是两端 Pos。
  const SEL = () => {
    const r = D.selectionRaw() || {};
    return { anchor: r.anchor, focus: r.focus, empty: !!r.empty, rects: r.rects || [] };
  };
  // 密度：设备像素 / 磅。按公开面的倍数折回来，不读 diag 的 density —— 那是 float32，
  //   末位差异会让压在字符边界上的采样点翻到另一侧（横扫判据会随机红一个点）。
  const ZOOM = () => H.config().zoom * (4 / 3) * (H.geometry().dpr || 1);
  // 一磅占多少 CSS 像素：密度除以 DPR。鼠标坐标与 DOM 覆盖物位置都用它，别拿 ZOOM 直接算。
  const CSSPT = () => ZOOM() / (H.geometry().dpr || 1);
  // 窗口与可见页：引擎装着的窗口来自 geometry，可见页按滚动位置与页步距算 ——
  //   滚动没动窗口时（还在余量内）两者会不一样，判据要能分别看。滚动在 .heditor-stage 上，
  //   它由库的骨架提供，不是页面的滚动容器。
  const VIEWPORT = () => {
    const g = H.geometry();
    const step = g.page.h + g.gap;
    const sc = EL('.heditor-stage');
    const top = sc ? sc.scrollTop : 0;
    const h = sc ? sc.clientHeight : 0;
    return {
      engineFirst: g.window.first, engineCount: g.window.count,
      canvases: D.pageCanvases(),
      visible: {
        first: Math.floor(top / Math.max(step, 1)),
        last: Math.floor((top + Math.max(h - 1, 0)) / Math.max(step, 1)),
      },
    };
  };
  // 帧：窗口那一整块在屏幕上占的矩形（顶边是窗口首页的顶边）。
  //   库没有帧元素（帧是本轮页位图的读视图），可整条坐标链都是按"帧在屏幕上那块"建立的。
  //   宽高由页度量算：帧宽就是页宽，帧高就是窗口页数 x 步距（见引擎的 frameHeight）。
  const FRAME = () => {
    const pages = EL('.heditor-pages');
    const r = pages.getBoundingClientRect();
    const g = H.geometry();
    const px = D.pagePx();                                   // 设备像素
    const count = Math.max(1, g.window.count);
    const pixelH = count * (px.h + px.gap);
    const top = r.top + g.window.first * (g.page.h + g.gap);
    const width = px.w / g.dpr;
    const height = pixelH / g.dpr;
    return {
      width: px.w, height: pixelH,
      getBoundingClientRect() {
        return { left: r.left, top, right: r.left + width, bottom: top + height,
                 width, height, x: r.left, y: top };
      },
      dispatchEvent(ev) { return pages.dispatchEvent(ev); },
    };
  };
  // 窗口像素：逐页从画布取，按"页号 x 步距"摆成与旧帧同形的一张。
  //   为什么不再有 PIX 之外的"帧"：引擎那条 frame 导出是"把页位图拼起来"的派生物 ——
  //   判据读它等于让被测对象自述。画布跨了 wasm 拷贝、Worker 传递、putImageData 三层，
  //   才是用户看见的那一层。判据按帧坐标写的（扫行、扫列、逐字节比），所以这里拼回同形，
  //   断言逻辑一个字不用改。
  //   页间隙填白：引擎的导出图那一格填的是背景色，默认就是白。
  const PIX = async () => {
    // 呈现挂在 rAF 上：不等两帧读到的是上一拍。selfCheck 比"画布 vs 引擎页像素"时
    //   两边都旧，差额也是 0 —— 判据会假绿，所以这个等待不是保守，是必要条件。
    await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const g = H.geometry();
    const px = D.pagePx();                                   // 设备像素
    const step = px.h + px.gap;
    const count = Math.max(1, g.window.count);
    const w = px.w;
    const h = count * step;
    const out = new Uint8ClampedArray(w * h * 4).fill(255);
    const byPage = {};
    for (const c of EL('.heditor-pages').querySelectorAll('canvas')) {
      byPage[c.dataset.page] = c;
    }
    for (let p = g.window.first; p < g.window.first + count; ++p) {
      const el = byPage[p];
      if (!el) throw new Error('窗口内缺页画布：' + p);
      if (el.width !== w) throw new Error('页画布宽 ' + el.width + ' 与页度量 ' + w + ' 不一致');
      const t = document.createElement('canvas');
      t.width = el.width;
      t.height = el.height;
      const c2 = t.getContext('2d');
      c2.drawImage(el, 0, 0);
      const d = c2.getImageData(0, 0, el.width, el.height).data;
      const base = (p - g.window.first) * step;
      for (let y = 0; y < el.height && base + y < h; ++y) {
        out.set(d.subarray(y * w * 4, (y + 1) * w * 4), (base + y) * w * 4);
      }
    }
    return { w, h, data: out };
  };
  // 帧指纹（FNV-1a，只算 RGB）：回答"同一个状态重画一次，屏幕上是不是一个字都没变"。
  //   判据只比较两次取值是否相等，所以算法本身不必与任何引擎侧的口径一致。
  const PIXH = async () => {
    const f = (await PIX()).data;
    let h = 2166136261;
    for (let i = 0; i + 3 < f.length; i += 4) {
      h ^= f[i]; h = Math.imul(h, 16777619);
      h ^= f[i + 1]; h = Math.imul(h, 16777619);
      h ^= f[i + 2]; h = Math.imul(h, 16777619);
    }
    return h >>> 0;
  };
  // 输入一个字符串，交回这一拍的记账（判据要读重排段数 / 后缀平移）。
  const TYPE = async (s) => {
    const ok = await H.type(s);
    return ok ? D.editStats() : null;
  };
  // 移动光标：引擎没动就交回 null —— "没动就不该重绘"是从这个 null 读出来的。
  const MOVE = async (n) => {
    const before = D.caretLive();
    await H.moveCaret(n < 0 ? 'left' : 'right');
    return D.caretLive() === before ? null : D.editStats();
  };
  // 装载之后把焦点放回 IME 承接层：真实按键没有焦点一个都到不了输入层，
  //   而库不在装载时抢焦点（那是产品语义，不是缺陷）。四类装载都要补。
  const FOCUSED = async (p) => {
    const r = await p;
    D.focus();
    return r;
  };
  // 装载：全部走公开面。
  //   夹具 = 一份 .json 文档 + open()（用户"打开文件"就是这条路）；
  //   样例 = 页面上那组用公开方法搭出来的文档（见 app/samples/）；
  //   批量 = setText 的 repeat（压测用的就是它的产品语义）。
  //   返回的都是这一拍的账 —— 判据拿它当分母。
  const loaded = async (run) => {
    await run();
    return D.editStats();
  };
  // 换文档先回"文档模式"：只读/表单模式下 open 与编辑命令都会被策略拒掉（这是编辑策略的
  //   本意，不是缺陷），而"装载"这件事与当前模式无关。各 LOAD 自己按需要再切回去。
  const DOC_MODE = () => H.configure({ mode: 'document' });
  const LOAD = {
    // 短文档夹具：一段 14 磅正文（判据要"一页放得下"的那份）。
    short: (z) => FOCUSED(loaded(async () => {
      await DOC_MODE();
      const res = await fetch('/assets/samples/fixtures/scenario-6.json', { cache: 'no-store' });
      if (!res.ok) throw new Error('夹具读取失败：scenario-6');
      await H.open(await res.text());
      if (z !== undefined) await H.configure({ zoom: z });
    })),
    // 长文档：2000 段，用 setText 的 repeat 铺（与压测同一条产品语义）。
    long: (z) => FOCUSED(loaded(async () => {
      await DOC_MODE();
      await H.setText('患者主诉：反复心悸三天，加重一天。', { repeat: 2000 });
      if (z !== undefined) await H.configure({ zoom: z });
    })),
    // 批量：paras 段、每段 chars 个字。
    bulk: (paras, chars, z) => FOCUSED(loaded(async () => {
      await DOC_MODE();
      const one = '字'.repeat(Math.max(1, chars | 0));
      await H.setText(one, { repeat: Math.max(1, paras | 0) });
      if (z !== undefined) await H.configure({ zoom: z });
    })),
    // 表单夹具：四份 .json（区间与只读位都在文件里，不是靠"载入场景"造的）。
    //   打开之后切到表单模式：夹具只描述文档，模式是宿主的配置，两件事分开。
    form: (v, z) => FOCUSED(loaded(async () => {
      await DOC_MODE();
      const res = await fetch('/assets/samples/fixtures/form-' + (v | 0) + '.json',
                              { cache: 'no-store' });
      if (!res.ok) throw new Error('夹具读取失败：form-' + v);
      await H.open(await res.text());
      await H.configure({ mode: 'form' });
      if (z !== undefined) await H.configure({ zoom: z });
    })),
    // 页面上的样例：与菜单里点出来的是同一段代码。
    sample: (id, repeat, z) => FOCUSED(loaded(async () => {
      await DOC_MODE();
      await window.__samples.run(id, repeat === undefined ? undefined : { repeat });
      if (z !== undefined) await H.configure({ zoom: z });
    })),
    text: (t, r) => FOCUSED(H.setText(t, { repeat: r })),
  };
  // 编辑模式：旧接口收 0/1/2，公开面收三个名字。
  const SET_MODE = (m) => H.configure({ mode: MODES[Number(m)] || 'document' });
  const SET_BRACKETS = (on) => H.configure({ showBrackets: !!on });
  const SET_DOTS = (on) => H.configure({ showControlDots: !!on });
  const UNDO_LIMIT = async (n) => (await H.configure({ undoLimit: Number(n) })).undoLimit;
  // 样式 setter 收的是"只给要改的字段"，configure 是整份替换，这里补全。
  //   交回这一拍的记账：判据要读它有没有引起重排 / 重画。
  const CARET_STYLE = async (s) => {
    await H.configure({ caretStyle: Object.assign({}, H.config().caretStyle, s || {}) });
    return D.editStats();
  };
  const SEL_STYLE = async (s) => {
    await H.configure({ selectionStyle: Object.assign({}, H.config().selectionStyle, s || {}) });
    return D.editStats();
  };
`;

/**
 * 下拉的页面内工具：p.ev 不拼 PRELUDE，所以要用它们的用例自己拼进脚本。
 *
 * 下拉不是原生 select —— 值是触发器上的字，选项在浮层里，两项都要自己点。
 */
export const SELECT_HELPERS = `
  const SELECTED = (act) => {
    const t = document.querySelector(
      '.heditor-select[data-act="' + act + '"] .heditor-select__text',
    );
    return t ? t.textContent : '';
  };
  const SELECT_VALUE = (act) => {
    const el = document.querySelector('.heditor-select[data-act="' + act + '"]');
    return el ? (el.dataset.value || '') : '';
  };
  const PICK = (act, value) => {
    const trig = document.querySelector('.heditor-select[data-act="' + act + '"]');
    if (!trig) throw new Error('没有这个下拉：' + act);
    trig.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0 }));
    const item = document.querySelector(
      '.heditor-select__item[data-value="' + String(value).replace(/"/g, '\\"') + '"]',
    );
    if (!item) throw new Error('下拉里没有这一项：' + act + ' = ' + value);
    item.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true, button: 0 }));
    return true;
  };
`;
