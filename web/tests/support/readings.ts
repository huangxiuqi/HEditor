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
  // 缩放密度：设备像素 / 磅。界面收倍数，这里按装载时写进引擎的那套换算折回去。
  const ZOOM = () => H.config().zoom * (4 / 3) * (H.geometry().dpr || 1);
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
  const FRAME = () => {
    const pages = EL('.heditor-pages');
    const r = pages.getBoundingClientRect();
    const g = H.geometry();
    const top = r.top + g.window.first * (g.page.h + g.gap);
    const width = D.frameW() / g.dpr;
    const height = D.frameH() / g.dpr;
    return {
      width: D.frameW(), height: D.frameH(),
      getBoundingClientRect() {
        return { left: r.left, top, right: r.left + width, bottom: top + height,
                 width, height, x: r.left, y: top };
      },
      dispatchEvent(ev) { return pages.dispatchEvent(ev); },
    };
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
  const LOAD = {
    scenario: (id, z) => FOCUSED(D.loadScenario(id, z)),
    reset: (id, z) => FOCUSED(D.reset(id, z)),
    stress: (p, c) => FOCUSED(D.stress(p, c)),
    stressDiverse: (p, c) => FOCUSED(D.stressDiverse(p, c)),
    form: (v) => FOCUSED(D.loadFormSample(v)),
    medical: (n) => FOCUSED(D.loadMedicalSample(n)),
    controls: (l) => FOCUSED(D.setFormControls(l)),
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
