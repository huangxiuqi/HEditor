// 迁移垫片：把旧门面（window.__heditor）翻译成新库的公开面 + diag。
//
// 这是探针专用的兼容层，不是产品代码：
//   一、它只存在于探针注入里（edit_probe --lib），库产物里没有这个文件；
//   二、每个成员只做一次翻译，翻译漏了会当场抛（不会静默返回 undefined）；
//   三、它的存在是为了让 369 条断言先跑起来找差异，而不是把旧门面复活。
//
// 三条与旧门面故意不同的地方，都在注释里标了，需要改断言而不是改这里：
//   caret / selection 的形状、setEditMode(1)、ask。
(function () {
  'use strict';

  const missing = (name) => {
    throw new Error('迁移垫片：旧门面成员 ' + name + ' 在新库上没有等价物');
  };

  const ready = () => new Promise((res) => {
    const t = setInterval(() => {
      if (window.__demoReady && window.__demo) {
        clearInterval(t);
        res();
      }
    }, 8);
  });

  ready().then(() => {
    const e = () => window.__demo;
    const d = () => window.__demo.diag;

    /**
     * 装载类调用之后把焦点放回承接层。
     *
     * 旧页面里那是工具栏按钮的职责（每次装载完都 ta.focus()）；探针的"真实按键"
     *   走 CDP，没有焦点就一个键都到不了输入层。库本身不在装载时抢焦点 ——
     *   那是产品语义，只有诊断面提供 focus。
     */
    const focused = (p) => Promise.resolve(p).then((r) => { d().focus(); return r; });

    // 旧门面的 caret 是扁平的（para/offset/x/y/w/h + 全文码点数）。
    const flatCaret = () => {
      const c = e().caret();
      const st = d().editStats() || {};
      return {
        para: c.pos.para,
        offset: c.pos.offset,
        page: c.page,
        x: c.rect.x,
        y: c.rect.y,
        w: c.rect.w,
        h: c.rect.h,
        visible: c.visible,
        // 侧别：同一偏移上压着好几枚括号时，光标挂在哪一侧（旧门面从 editStats 读）。
        side: st.caretSide,
        docCodepoints: st.docCodepoints,
      };
    };

    /**
     * 旧门面里的 canvas 是帧（窗口那一整块），不是某一页的画布。
     *
     * 旧的两个宿主都给它套了一层代理：几何按"窗口帧在屏幕上占的那块区域"算（顶边是
     *   窗口首页的顶边），事件转发给页容器。探针的整条坐标链都建立在这个口径上 ——
     *   它把 r.top 当"窗口首页的顶边"，再按页号加偏移。
     * 新库没有"帧元素"（帧在本轮里已经是页位图的读视图），页画布也按槽复用、DOM 顺序
     *   与窗口顺序无关，所以这里必须把那个代理重建出来，而不是把某一张页画布交出去。
     */
    const frameProxy = () => {
      const pagesEl = () => document.querySelector('#ed #pages');
      return {
        get width() { return d().frameW(); },
        get height() { return d().frameH(); },
        get style() { return pagesEl().style; },
        getBoundingClientRect() {
          const el = pagesEl();
          const r = el.getBoundingClientRect();
          const g = e().geometry();
          const dpr = window.devicePixelRatio || 1;
          const step = g.page.h + g.gap;
          const top = r.top + g.window.first * step;
          const width = this.width / dpr;
          const height = this.height / dpr;
          return {
            left: r.left, top, right: r.left + width, bottom: top + height,
            width, height, x: r.left, y: top,
          };
        },
        dispatchEvent(ev) { return pagesEl().dispatchEvent(ev); },
        addEventListener(...a) { return pagesEl().addEventListener(...a); },
        getContext() { return null; },
      };
    };
    const frameCanvas = frameProxy();

    window.__heditor = {
      /**
       * 这一份门面下面挂着的是库页面而不是旧宿主页。
       *
       * 有几条断言查的是页面自己的 UI（场景下拉里有没有某一项）—— 那是旧页面的
       *   东西，库不带 UI。探针按这个标记跳过它们，而不是把 UI 塞进库。
       */
      isLib: true,

      // 状态读数
      get ready() { return true; },
      get errors() { return d().errors(); },
      // 旧 text 只给第 0 段（整篇叫 allText）。
      get text() { return e().paragraph(0); },
      get allText() { return e().text(); },
      paragraphText(i) { return e().paragraph(i); },
      get caret() { return flatCaret(); },
      get caretReport() { return d().caretReport(); },
      get caretLive() { return d().caretLive(); },
      get editStats() { return d().editStats(); },
      get stats() { return d().stats(); },
      get blockedEdits() { return d().blockedEdits(); },
      get bracketSlots() { return d().bracketSlots(); },
      get controls() { return d().controls(); },
      get fonts() { return d().env(); },
      get blitStats() { return d().blitStats(); },
      // 旧门面的 zoomLevel 是密度（设备像素 / 磅），探针拿它把磅折成 CSS 像素。
      //   新库的 config().zoom 是倍数（1 = 100%），换算关系与装载时写进引擎的一致。
      get zoomLevel() { return e().config().zoom * (4 / 3) * (window.devicePixelRatio || 1); },
      undoLimit() { return e().config().undoLimit; },
      get showBrackets() { return e().config().showBrackets; },
      get showControlDots() { return e().config().showControlDots; },
      get caretStyle() { return e().config().caretStyle; },
      get selectionStyle() { return e().config().selectionStyle; },
      // 旧 pagePx 是数组：pagePx[1] 是页高。
      get pagePx() {
        const p = d().pagePx();
        return [p.w, p.h, p.gap];
      },
      // 旧门面的两段是全局流位置（数字），不是 {段号, 段内偏移} —— 探针整套
      //   断言都在比较数字。新库对外给段内位置，raw 载荷里才有流位置。
      get selection() {
        const r = d().selectionRaw() || {};
        return { anchor: r.anchor, focus: r.focus, empty: !!r.empty, rects: r.rects || [] };
      },
      // 旧门面的窗口读数。新库没有帧，窗口就是 geometry().window，
      //   可见页范围按滚动位置与页步距算（滚动不动窗口时就靠它）。
      get viewport() {
        const g = e().geometry();
        const dpr = window.devicePixelRatio || 1;
        const step = (g.page.h + g.gap) / dpr;
        const sc = document.querySelector('#ed #stage');
        const top = sc ? sc.scrollTop : 0;
        const h = sc ? sc.clientHeight : 0;
        return {
          engineFirst: g.window.first,
          engineCount: g.window.count,
          canvases: d().pageCanvases(),
          visible: {
            first: Math.floor(top / Math.max(step, 1)),
            last: Math.floor((top + Math.max(h - 1, 0)) / Math.max(step, 1)),
          },
        };
      },
      get drag() { return d().dragState(); },
      dragSettled() { return d().dragSettled(); },
      quiesce() { return e().flush(); },

      // 文档装载
      run(id, zoom) { return focused(d().loadScenario(id, zoom)); },
      reset(id, zoom) { return focused(d().reset(id, zoom)); },
      loadTextDocument(text, repeat) { return focused(e().setText(text, { repeat })); },
      loadStressDocument(paras, chars) { return focused(d().stress(paras, chars)); },
      loadStressDocumentDiverse(paras, chars) { return focused(d().stressDiverse(paras, chars)); },
      loadFormSample(variant) { return focused(d().loadFormSample(variant)); },
      loadMedicalSample(repeat) { return focused(d().loadMedicalSample(repeat)); },
      setFormControls(list) { return focused(d().setFormControls(list)); },

      // 编辑
      // 旧 type 的返回值是本次编辑的 editStats（探针要读 relaidParagraphs / suffixShifted）。
      async type(s) {
        const ok = await e().type(s);
        return ok ? d().editStats() : null;
      },
      backspace() { return e().deleteBackward(); },
      deleteForward() { return e().deleteForward(); },
      undo() { return e().undo(); },
      redo() { return e().redo(); },
      // 旧收"移动几个码点"，新收步进：只覆盖 ±1（探针里唯一的用法就是 -1）。
      async move(dcp) {
        if (dcp !== 1 && dcp !== -1) missing('move(' + dcp + ')');
        const before = d().caretLive();
        await e().moveCaret(dcp < 0 ? 'left' : 'right');
        // 旧门面：引擎没动就返回 null（"不重绘"就是从这个 null 读出来的）。
        return d().caretLive() === before ? null : d().editStats();
      },
      selectAll() { return e().selectAll(); },
      clearSelection() { return e().clearSelection(); },

      // 光标
      // 旧三个 setCaret* 都返回扁平 caret（探针拿 cr.page / cr.x 算窗口坐标）。
      async setCaret(para, offset) {
        await e().setCaret({ para, offset });
        return flatCaret();
      },
      async setCaretAt(para, offset, field) {
        await d().setCaretAt(para, offset, field);
        return flatCaret();
      },
      async setCaretAtSide(para, offset, side) {
        await e().setCaret({ para, offset }, { side: side > 0 ? 'after' : 'before' });
        return flatCaret();
      },
      async setCaretOnly(para, offset) {
        await Promise.resolve(d().setCaretOnly(para, offset));
        return flatCaret();
      },
      clickAt(x, y) { return d().clickAt({ x, y }); },
      hit(x, y) { return d().hit(x, y); },

      // 配置
      // 旧门面收整数（0 文档 / 1 表单 / 2 只读），公开面收三个名字。
      setEditMode(m) {
        return e().configure({ mode: ['document', 'form', 'readOnly'][Number(m)] || 'document' });
      },
      setShowBrackets(on) { return e().configure({ showBrackets: !!on }); },
      setShowControlDots(on) { return e().configure({ showControlDots: !!on }); },
      async setUndoLimit(n) {
        const cfg = await e().configure({ undoLimit: Number(n) });
        return cfg.undoLimit;
      },
      // 旧 setter 允许只给一个字段，新 configure 是整份替换：这里补全。
      //   返回值是那一拍的 editStats（探针拿它读 layoutRan / dirtyPageCount / repaintedPixels）。
      async setCaretStyle(style) {
        const cur = e().config().caretStyle;
        await e().configure({ caretStyle: Object.assign({}, cur, style || {}) });
        return d().editStats();
      },
      async setSelectionStyle(style) {
        const cur = e().config().selectionStyle;
        await e().configure({ selectionStyle: Object.assign({}, cur, style || {}) });
        return d().editStats();
      },
      setControlProp(i, prop, value) { return e().setControlProp(i, prop, !!value); },
      async focusControl(forward) {
        await e().focusControl(forward ? 'next' : 'prev');
        return d().editStats();
      },
      removeControl(i, keep) { return d().removeControl(i, keep); },

      // 字体与窗口
      reloadFonts(mode) { return d().reloadFonts(mode); },
      setFontCacheConfig(a, b) { return d().setFontCacheConfig(a, b); },
      setViewportAnchor(first, count) { return d().setViewportAnchor(first, count); },

      // 闪烁与呈现
      present() { return d().present(); },
      showCaret(on) { return d().showCaret(on); },
      startBlink() { return d().startBlink(); },
      stopBlink() { return d().stopBlink(); },

      // 自审与诊断
      auditFrame() { return d().auditFrame(); },
      auditBase() { return d().auditBase(); },
      auditLayout() { return d().auditLayout(); },
      auditCompile() { return d().auditCompile(); },
      auditGlyphPages() { return d().auditGlyphPages(); },
      selfCheck() { return d().selfCheck(); },
      markCheck() { return d().markCheck(); },
      rawFrame() { return d().rawFrame(); },
      frameW() { return d().frameW(); },
      frameH() { return d().frameH(); },
      bench(runs) { return d().bench(runs); },
      prewarmPage(p) { return d().prewarmPage(p); },

      // 宿主 DOM（探针自己派真实事件用）
      get canvas() { return frameCanvas; },
      get textarea() { return document.getElementById('ime'); },

      // 错误面：新库没有泛 ask，探针要测"未知命令必须抛"得走 diag
      ask() { return missing('ask（新库没有泛命令入口）'); },
    };
  });
})();
