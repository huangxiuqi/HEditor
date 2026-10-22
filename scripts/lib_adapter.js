// 迁移垫片：把旧门面（window.__heditor）翻译成新库的公开面 + diag。
//
// 这是**探针专用的兼容层**，不是产品代码：
//   · 它只存在于探针注入里（edit_probe --lib），库产物里没有这个文件；
//   · 每个成员只做一次翻译，翻译漏了会当场抛（不会静默返回 undefined）；
//   · 它的存在是为了让 369 条断言先跑起来找差异，而不是把旧门面复活。
//
// 三条与旧门面**故意不同**的地方，都在注释里标了，需要改断言而不是改这里：
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

    /** 旧门面的 caret 是扁平的（para/offset/x/y/w/h + 全文码点数）。 */
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
        docCodepoints: st.docCodepoints,
      };
    };

    window.__heditor = {
      /* ── 状态读数 ── */
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
      get zoomLevel() { return e().config().zoom; },
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
      get selection() { return e().selection(); },
      // 旧门面的窗口读数。新库没有帧，窗口就是 geometry().window，
      //   可见页范围按滚动位置与页步距算（滚动不动窗口时就靠它）。
      get viewport() {
        const g = e().geometry();
        const dpr = window.devicePixelRatio || 1;
        const step = (g.page.h + g.gap) / dpr;
        const sc = document.querySelector('#ed .heditor-body');
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
      get drag() { return null; },
      dragSettled() { return Promise.resolve(); },
      quiesce() { return e().flush(); },

      /* ── 文档装载 ── */
      run(id, zoom) { return d().loadScenario(id, zoom); },
      reset(id, zoom) { return d().reset(id, zoom); },
      loadTextDocument(text, repeat) { return e().setText(text, { repeat }); },
      loadStressDocument(paras, chars) { return d().stress(paras, chars); },
      loadStressDocumentDiverse(paras, chars) { return d().stressDiverse(paras, chars); },
      loadFormSample(variant) { return d().loadFormSample(variant); },
      loadMedicalSample(repeat) { return d().loadMedicalSample(repeat); },
      setFormControls(list) { return d().setFormControls(list); },

      /* ── 编辑 ── */
      // 旧 type 返回那一次的 editStats（探针要读 relaidParagraphs / suffixShifted）。
      async type(s) {
        const ok = await e().type(s);
        return ok ? d().editStats() : null;
      },
      backspace() { return e().deleteBackward(); },
      deleteForward() { return e().deleteForward(); },
      undo() { return e().undo(); },
      redo() { return e().redo(); },
      // 旧收"移动几个码点"，新收步进：只覆盖 ±1（探针里唯一的用法就是 -1）。
      move(dcp) {
        if (dcp !== 1 && dcp !== -1) missing('move(' + dcp + ')');
        return e().moveCaret(dcp < 0 ? 'left' : 'right');
      },
      selectAll() { return e().selectAll(); },
      clearSelection() { return e().clearSelection(); },

      /* ── 光标 ── */
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

      /* ── 配置 ── */
      // 旧门面收整数（0 文档 / 1 表单 / 2 只读），公开面收三个名字。
      setEditMode(m) {
        return e().configure({ mode: ['document', 'form', 'readOnly'][Number(m)] || 'document' });
      },
      setShowBrackets(on) { return e().configure({ showBrackets: !!on }); },
      setShowControlDots(on) { return e().configure({ showControlDots: !!on }); },
      setUndoLimit(n) { return e().configure({ undoLimit: Number(n) }); },
      // 旧 setter 允许只给一个字段，新 configure 是整份替换：这里补全。
      setCaretStyle(style) {
        const cur = e().config().caretStyle;
        return e().configure({ caretStyle: Object.assign({}, cur, style || {}) });
      },
      setSelectionStyle(style) {
        const cur = e().config().selectionStyle;
        return e().configure({ selectionStyle: Object.assign({}, cur, style || {}) });
      },
      setControlProp(i, prop, value) { return e().setControlProp(i, prop, !!value); },
      focusControl(forward) { return e().focusControl(forward ? 'next' : 'prev'); },
      removeControl(i, keep) { return d().removeControl(i, keep); },

      /* ── 字体与窗口 ── */
      reloadFonts(mode) { return d().reloadFonts(mode); },
      setFontCacheConfig(a, b) { return d().setFontCacheConfig(a, b); },
      setViewportAnchor(first, count) { return d().setViewportAnchor(first, count); },

      /* ── 闪烁与呈现 ── */
      present() { return d().present(); },
      showCaret(on) { return d().showCaret(on); },
      startBlink() { return d().startBlink(); },
      stopBlink() { return d().stopBlink(); },

      /* ── 自审与诊断 ── */
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

      /* ── 宿主 DOM（探针自己派真实事件用） ── */
      get canvas() { return document.querySelector('#pages canvas'); },
      get textarea() { return document.getElementById('ime'); },

      /* ── 错误面：新库没有泛 ask，探针要测"未知命令必须抛"得走 diag ── */
      ask() { return missing('ask（新库没有泛命令入口）'); },
    };
  });
})();
