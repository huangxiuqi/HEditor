var __heditorEngineApiExports = function(exports) {
  "use strict";
  const COMMANDS = [
    // 呈现与装载：整份换文档，引擎自己重排重画
    // 实现是空的：render() 认这个名字去补一次 mod.present()
    ["present", "w", ["present"], "host"],
    ["scenario", "w", ["setScenario"], "self"],
    ["reset", "w", ["setZoom", "setScenario"], "self"],
    ["zoom", "w", ["setZoom"], "self"],
    ["stress", "w", ["loadStressDocument"], "self"],
    ["stressDiverse", "w", ["loadStressDocumentDiverse"], "self"],
    ["textDoc", "w", ["loadTextDocument"], "self"],
    ["formSample", "w", ["loadFormSample"], "self"],
    ["loadDocument", "w", ["loadDocument"], "self"],
    ["fonts", "w", ["loadFont", "loadFontFromFile", "setFontRoles"], "self"],
    ["setFormControls", "w", ["setFormControls"], "self"],
    // 编辑：*AndRender 系在库里就排完画完了，宿主不要再画一遍
    ["insertText", "w", ["insertTextAt"], "self"],
    ["type", "w", ["typeText"], "self"],
    ["backspace", "w", ["backspace"], "self"],
    ["deleteForward", "w", ["deleteForward"], "self"],
    ["splitParagraph", "w", ["splitParagraph"], "self"],
    ["mergePrev", "w", ["mergeWithPreviousParagraph"], "self"],
    ["mergeNext", "w", ["mergeWithNextParagraph"], "self"],
    ["move", "w", ["moveCaret"], "self"],
    ["vertical", "w", ["moveCaretVertical"], "self"],
    ["docStart", "w", ["caretToDocStart"], "self"],
    ["docEnd", "w", ["caretToDocEnd"], "self"],
    ["lineStart", "w", ["caretToLineStart"], "self"],
    ["lineEnd", "w", ["caretToLineEnd"], "self"],
    ["indent", "w", ["indent"], "self"],
    ["outdent", "w", ["outdent"], "self"],
    ["tabCaret", "w", ["tabCaret"], "self"],
    ["undo", "w", ["undoEdit"], "self"],
    ["redo", "w", ["redoEdit"], "self"],
    ["removeControl", "w", ["removeControlAt"], "self"],
    ["setControlProp", "w", ["setControlPropAt"], "self"],
    ["focusControl", "w", ["focusControl"], "self"],
    // 选区与光标：选区的几条自己收尾；落光标、拖选、闪烁只改状态，要宿主补呈现
    ["selectAll", "w", ["selectAll"], "self"],
    ["selectCellAtPara", "w", ["selectCellAtPara"], "self"],
    ["cellSelectAt", "w", ["cellSelectAt"], "self"],
    ["selectPos", "w", ["setSelection", "flowIndexAt"], "self"],
    ["setSelection", "w", ["setSelection"], "self"],
    ["clearSelection", "w", ["clearSelection"], "self"],
    ["setCaret", "w", ["setCaret", "showCaret"], "host"],
    ["setCaretAt", "w", ["setCaretAt", "showCaret"], "host"],
    ["setCaretAtSide", "w", ["setCaretAtSide", "showCaret"], "host"],
    ["setCaretAtSideLine", "w", ["setCaretAtSideLine", "showCaret"], "host"],
    ["setFocusAtSideLine", "w", ["setFocusAtSideLine", "showCaret"], "host"],
    ["setFocusAtSide", "w", ["setFocusAtSide", "showCaret"], "host"],
    ["blinkPhase", "w", ["showCaret"], "host"],
    ["viewport", "w", ["setViewportAnchor"], "host"],
    // 配置：改参数；改到画面的那几条（模式 / 括号 / 小点 / 光标色）要补呈现
    ["setEditMode", "w", ["setEditMode"], "host"],
    // 会重编译整篇，但不重绘
    ["setShowBrackets", "w", ["setShowBrackets"], "host"],
    // 会重排（括号占宽度）
    ["setShowControlDots", "w", ["setShowControlDots"], "host"],
    // 不重排，只改光栅签名
    ["caretStyle", "w", ["caretStyleJson", "setCaretStyle"], "host"],
    ["selectionStyle", "w", ["selectionStyleJson", "setSelectionStyle"], "host"],
    ["setUndoLimit", "w", ["setUndoLimit"], "none"],
    ["setFontCacheConfig", "w", ["setFontCacheConfig"], "none"],
    // 静默写：改引擎状态，但不上屏也不回推（门面的 showCaret / stopBlink）
    ["phase", "s", ["showCaret"], "none"],
    // 查询：文档与结构
    ["ping", "r", [], "none"],
    // 排空屏障：什么都不算，只回一个值
    ["textAll", "r", ["documentTextAll"], "none"],
    ["text", "r", ["documentText"], "none"],
    ["serialize", "r", ["serializeDocument"], "none"],
    ["hit", "r", ["hitTest"], "none"],
    ["controlAt", "r", ["controlAtPoint"], "none"],
    ["describeControls", "r", ["describeControls"], "none"],
    ["controls", "r", ["controlsJson"], "none"],
    ["bracketSlots", "r", ["bracketSlotsJson"], "none"],
    ["lines", "r", ["linesJson"], "none"],
    ["blockedEdits", "r", ["blockedEdits"], "none"],
    ["editMode", "r", ["editMode"], "none"],
    ["showBrackets", "r", ["showBrackets"], "none"],
    ["showControlDots", "r", ["showControlDots"], "none"],
    ["undoLimit", "r", ["undoLimit"], "none"],
    // 查询：几何
    ["frameWidth", "r", ["frameWidth"], "none"],
    ["frameHeight", "r", ["frameHeight"], "none"],
    ["frameByteSize", "r", ["frameByteSize"], "none"],
    ["pagePxWidth", "r", ["pagePxWidth"], "none"],
    ["pagePxHeight", "r", ["pagePxHeight"], "none"],
    ["pageGapPx", "r", ["pageGapPx"], "none"],
    ["windowFirstPage", "r", ["windowFirstPage"], "none"],
    ["windowPageCount", "r", ["windowPageCount"], "none"],
    // 查询：光标与选区
    ["caretOffsetLive", "r", ["caretOffsetLive"], "none"],
    ["caretVerticalNow", "r", ["caretVerticalNow"], "none"],
    ["caretBlockReversedNow", "r", ["caretBlockReversedNow"], "none"],
    ["flowTextPos", "r", ["flowTextPosJson"], "none"],
    ["selectionReport", "r", ["selectionReport"], "none"],
    ["caretStyleJson", "r", ["caretStyleJson"], "none"],
    ["selectionStyleJson", "r", ["selectionStyleJson"], "none"],
    ["caretReport", "r", ["caretReport"], "none"],
    // 查询：像素与账本
    ["ink", "r", ["frameRgba"], "none"],
    ["inkRows", "r", ["frameRgba", "frameWidth", "pageGapPx", "pagePxHeight", "windowFirstPage", "windowPageCount"], "none"],
    ["frameDigest", "r", ["frameRgba"], "none"],
    ["stats", "r", ["statsJson"], "none"],
    ["editStats", "r", ["editStatsJson"], "none"],
    ["dirtyPages", "r", ["dirtyPagesJson"], "none"],
    ["pageDirtyRects", "r", ["pageDirtyRectsJson"], "none"],
    ["pageDirtyPages", "r", ["pageDirtyPagesJson"], "none"],
    ["prewarmPage", "r", ["prewarmPageGlyphs"], "none"],
    ["heap", "r", [], "none"],
    // 读的是模块属性 HEAPU8，不是导出
    ["buildStamp", "r", ["buildStamp"], "none"],
    ["rasterBackend", "r", ["rasterBackend"], "none"],
    ["bench", "r", ["benchRelayoutPaint"], "none"],
    // 查询：自审与诊断
    ["auditFrame", "r", ["auditFrame"], "none"],
    ["auditBase", "r", ["auditBase"], "none"],
    ["auditPage", "r", ["auditPage"], "none"],
    ["auditLayout", "r", ["auditLayout"], "none"],
    ["auditCompile", "r", ["auditCompile"], "none"],
    ["auditGlyphPages", "r", ["auditGlyphPages"], "none"],
    ["selfCheck", "r", ["documentText", "editStatsJson", "frameHeight", "frameWidth", "pagePxHeight", "pagePxWidth", "pageRgba", "rasterBackend", "windowFirstPage", "windowPageCount"], "none"],
    ["markCheck", "r", ["controlsJson", "frameRgba", "frameWidth", "showBrackets"], "none"],
    // 问答特判：帧太大，要转移所有权而不是结构化克隆（见 askOp 里的 rawFrame 分支）
    ["rawFrame", "q", ["frameRgba"], "none"]
  ];
  const OUTSIDE = [
    ["allocBuffer", "glue", "字体的字节从 JS 搬进 wasm 堆"],
    ["freeBuffer", "glue", "同上"],
    ["relayoutAndPaint", "host", "整篇重排重画：paintFull 与 relayout 用它换文档、换缩放、换页度量"],
    ["setPageMetrics", "host", "页度量只在装载时定一次（app.js 与 boot），没有运行期入口"],
    ["setPageGap", "host", "页间隙：由 api.setViewMode 与连页标志一起设"],
    ["setContinuous", "host", "连页标志：同上（改的是排版形状，不是一份配置）"],
    ["contentPx", "state", "geom() 报的内容底边（连页拿它当滚动高度）"],
    ["relayout", "diag", "只排版不画（手工诊断）；目前没有任何调用方"],
    ["shapeReport", "diag", "单段整形报告（手工诊断）；目前没有任何调用方"]
  ];
  function idlByKind(kind) {
    return COMMANDS.filter((c) => c[1] === kind).map((c) => c[0]);
  }
  const IDL = {
    commands: COMMANDS,
    outside: OUTSIDE,
    byKind: idlByKind,
    // 要宿主补一次 present 的命令名。派生，不维护。
    presentAlways: () => new Set(COMMANDS.filter((c) => c[3] === "host").map((c) => c[0]))
  };
  function collect(kind, impl) {
    const want = idlByKind(kind);
    const missing = want.filter((n) => !Object.prototype.hasOwnProperty.call(impl, n));
    const extra = Object.keys(impl).filter((n) => want.indexOf(n) < 0);
    if (missing.length || extra.length) {
      throw new Error("协议表与实现不一致（" + kind + "）：缺 [" + missing.join(", ") + "]，多 [" + extra.join(", ") + "]");
    }
    return impl;
  }
  const PRESENT_ALWAYS = IDL.presentAlways();
  function createEngineApi(cfg) {
    const post = cfg.post;
    const host = cfg.host;
    const err = (e, where) => post({ t: "error", where, message: String(e && e.stack || e) });
    let mod = null;
    let dpr = 1;
    let viewW = 1;
    let viewH = 1;
    let scrollY = 0;
    let scrollLead = 0;
    let lastAnchor = "";
    let scrollReset = false;
    let scrollResetHost = false;
    let bootMs = 0;
    let fontMs = 0;
    let fontBytes = 0;
    let fontList = [];
    let fontMode = "full";
    const kZoomBase = 4 / 3;
    let zoomLevel = kZoomBase;
    const kPageWidthPt = 595.276;
    const kPageHeightPt = 841.89;
    const kPageMarginPt = 56.7;
    async function fetchFont(path) {
      const res = await fetch(path);
      if (!res.ok) throw new Error(path + " -> HTTP " + res.status);
      return new Uint8Array(await res.arrayBuffer());
    }
    async function installFonts(mode) {
      const t0 = performance.now();
      const idx = {};
      let bytes = 0;
      for (const f of fontList) {
        const buf = await fetchFont(f.path);
        bytes += buf.length;
        let ex = -1;
        if (mode === "lazy") {
          const name = "/" + f.path.split("/").pop();
          try {
            mod.FS.unlink(name);
          } catch (e) {
          }
          mod.FS.createDataFile("/", name, buf, true, false, true);
          ex = mod.loadFontFromFile(name);
        } else {
          const ptr = mod.allocBuffer(buf.length);
          mod.HEAPU8.set(buf, ptr);
          ex = mod.loadFont(f.path.split("/").pop(), ptr, buf.length);
          mod.freeBuffer(ptr);
        }
        if (ex < 0) throw new Error("字体解析失败: " + f.path);
        idx[f.role] = ex;
      }
      mod.setFontRoles(idx.cjk, idx.mongol, idx.latin);
      fontMs = performance.now() - t0;
      fontBytes = bytes;
      fontMode = mode;
      return bytes;
    }
    function geom(st) {
      const ph = mod.pagePxHeight();
      const step = ph + mod.pageGapPx();
      const first = mod.windowFirstPage();
      return {
        pageW: mod.pagePxWidth(),
        pageH: ph,
        gap: mod.pageGapPx(),
        first,
        count: mod.windowPageCount(),
        // 页画布在文档里的落点（设备像素）：纵向 = 页号 × 步距，横向由宿主居中。
        pageStep: step,
        frameW: mod.frameWidth(),
        frameH: mod.frameHeight(),
        dpr,
        viewW,
        viewH,
        scrollY,
        // 内容底边（设备像素）。连页下宿主拿它当滚动高度 —— 没有页可数，只有内容。
        contentPx: mod.contentPx ? mod.contentPx() : 0,
        // 槽数的上限（与文档位置无关，只随视口与页度量变）。宿主按它建槽、再把自己实际
        //   建了几个报回来（pageCanvas.slots）—— 一个数、一个来源，两侧不会分叉。
        maxWindowPages: maxWindowPages(),
        // 这一侧实际在用的槽数，供诊断读。它必须等于宿主报回来的那个值。
        slots: host.slotCount(),
        pagesReady: host.pagesMissing() === 0,
        // 与单线程宿主的 viewport 同形的两格：锚点原文与"该装哪几页"。
        //   探针靠它们判"窗口跟着可见页走"，缺了就是一条 .first 的 TypeError。
        anchor: lastAnchor,
        desired: st ? desiredWindow(st) : null,
        visible: st ? visibleRange(st) : null
      };
    }
    function visibleRange(st) {
      const total = Math.max(1, Number(st.totalPages) || 1);
      const step = mod.pagePxHeight() + mod.pageGapPx();
      const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
      const at = scrollY + scrollLead;
      const first = clamp(Math.floor(at / step), 0, total - 1);
      const last = clamp(Math.floor((at + viewH - 1) / step), first, total - 1);
      return { first, last };
    }
    const kWindowMarginPages = 4;
    const kWindowMaxPages = 8;
    const kWindowMaxPixels = 24 * 1024 * 1024;
    function maxWindowPages() {
      const step = mod.pagePxHeight() + mod.pageGapPx();
      const vis = Math.ceil(viewH / step);
      const byPixels = Math.floor(kWindowMaxPixels / Math.max(1, mod.pagePxWidth() * mod.pagePxHeight()));
      return Math.max(vis, Math.min(kWindowMaxPages, byPixels, vis + kWindowMarginPages));
    }
    function desiredWindow(st) {
      const total = Math.max(1, Number(st.totalPages) || 1);
      const vis = visibleRange(st);
      let lo = vis.first;
      let hi = vis.last;
      const cp = Number(st.caretPage);
      if (maxWindowPages() - (hi - lo + 1) > 0 && cp >= 0 && cp < total) {
        if (cp === lo - 1) lo = cp;
        else if (cp === hi + 1) hi = cp;
      }
      let first = Math.max(0, lo);
      const need = hi - lo + 1;
      const count = Math.max(1, Math.min(need, total));
      if (first + count > total) first = Math.max(0, total - count);
      return { first, count };
    }
    const kWindowLookaheadPages = 2;
    let scrollDir = 1;
    function targetWindow(req, total, cap) {
      const count = Math.max(req.count, Math.min(cap || 3, total));
      const spare = count - req.count;
      const behind = scrollDir < 0 ? Math.max(0, spare - 1) : 1;
      let first = req.first - Math.min(behind, spare);
      if (first + count > total) first = total - count;
      if (first < 0) first = 0;
      return { first, count: Math.max(1, Math.min(count, total)) };
    }
    function retarget(st) {
      const total = Math.max(1, Number(st.totalPages) || 1);
      const req = desiredWindow(st);
      const look = kWindowLookaheadPages;
      const needFirst = Math.max(0, req.first - (scrollDir < 0 ? look : 0));
      const needLast = Math.min(
        total - 1,
        req.first + req.count - 1 + (scrollDir > 0 ? look : 0)
      );
      const cur = lastAnchor.split(":").map(Number);
      if (cur.length === 2 && Number.isFinite(cur[0]) && Number.isFinite(cur[1]) && cur[0] <= needFirst && needLast <= cur[0] + cur[1] - 1) {
        return false;
      }
      const w = targetWindow(req, total, maxWindowPages());
      const key = w.first + ":" + w.count;
      if (key === lastAnchor) return false;
      mod.setViewportAnchor(w.first, w.count);
      lastAnchor = key;
      return true;
    }
    const KEEP_ANCHOR = /* @__PURE__ */ new Set(["viewport"]);
    function render(opName) {
      let st = JSON.parse(mod.editStatsJson());
      const moved = KEEP_ANCHOR.has(opName) ? false : retarget(st);
      if (moved || PRESENT_ALWAYS.has(opName)) {
        mod.present();
        st = JSON.parse(mod.editStatsJson());
      }
      host.blitPages();
      return st;
    }
    let packSum = 0, packMax = 0, packN = 0;
    const packAvg = () => packN ? packSum / packN : 0;
    let textAllCache = "";
    function sendState(ts, label, stats, changed, workMs) {
      const tPack = performance.now();
      if (changed) textAllCache = mod.documentTextAll();
      const blit = host.blitStats();
      post({
        t: "state",
        ts: ts || 0,
        label: label || "",
        stats,
        geom: geom(stats),
        scrollReset,
        // 这一下在 Worker 里花了多少毫秒。只有视口那条路会填；0 = 与视口无关的一份状态
        //   （编辑、装载、画布到达）。宿主拿它和往返比（见 host.viewDiag）。
        workMs: workMs || 0,
        // 打包那一段（构造 + 克隆）的近期均值与峰值。宿主只读不判 —— 它只为把
        //   "往返减 work"那段缺口拆成两半，好知道时间花在打包上还是花在路上。
        packAvgMs: packAvg(),
        packMaxMs: packMax,
        // 这一拍重贴了几页画布、传了多少像素、花了多少毫秒。挪窗时贴的是全窗，而真正需要
        //   新像素的只有新露出来那一页 —— 其余页的画布在文档流里位置没动、像素本来就是对的。
        //   px 是"实际 putImageData 的面积"：按脏矩形贴之后它应该比 pages × 页面积小两三个
        //   数量级（光标移动一次几十像素），这是"上屏没有整页搬"的唯一直接证据。
        blitMs: blit.ms,
        blitPages: blit.pages,
        blitPx: blit.px,
        // 这一下到底有没有真的改动。宿主门面的契约是"没改 => 返回 null"（对照单线程宿主
        //   app.js 里 if (!mod.X()) return null; return this.present();），
        //   引擎那一步的返回值就是它，必须一路带到主线程。
        changed: !!changed,
        heapMB: mod.HEAPU8.length / 1048576,
        fontMode,
        // 顺带把主线程要"同步读"的那几样一起推过去 —— 它们都很小，但每拍去 ask 一次
        //   就是一次往返（200 µs）。推一次让宿主侧的门面能保持同步读的形态。
        text0: mod.documentText(0),
        // 整篇文本：主线程的 text() / paragraphs() 是同步读，不能在读的那一刻再问一次。
        textAll: textAllCache,
        controls: JSON.parse(mod.controlsJson()),
        blockedEdits: mod.blockedEdits(),
        dirtyPages: JSON.parse(mod.dirtyPagesJson()),
        caretReport: mod.caretReport(),
        // 选区取证（两端全局流位置 + 页面坐标矩形）。与 caretReport 同档：小字符串，
        //   随 state 推过去，主线程的 host.selection 于是仍是同步读。
        //   两端同时推一份"段内位置"：对外 API 的 Pos 是 {段号, 段内偏移}，而这个换算
        //   只有引擎会（段间分隔符算不算一个位置是引擎的口径），主线程不该拿全文去猜。
        selection: (() => {
          const sel = JSON.parse(mod.selectionReport());
          sel.anchorPos = READ.flowTextPos({ flow: sel.anchor });
          sel.focusPos = READ.flowTextPos({ flow: sel.focus });
          return sel;
        })(),
        // 引擎实时光标偏移（直读，不经过 editStats 那本账）。宿主拿它和 editStats 里的
        //   光标位置比，就能判出"有没有哪个改了状态的入口忘了给自己收尾"。
        caretLive: mod.caretOffsetLive(),
        // 键位映射要同步读这两个：Worker 少一次往返，方向键就不会按错分支。
        caretVertical: !!mod.caretVerticalNow(),
        caretBlockReversed: !!mod.caretBlockReversedNow(),
        // 排版那本账（statsJson）与编辑那本账（editStatsJson）在单线程宿主上是两个入口，
        //   上面那个 stats 走的是编辑那本（宿主的内部逻辑要它：光标矩形、脏页、重画像素）。
        //   这里必须把排版那本也推过去 —— 否则主线程的 stats 会与 editStats 指向同一个对象，
        //   页数 / 字形 run 数 / 分阶段形状耗时这些字段读出来全是 undefined。
        //   undefined 不会让任何断言变红，只会让报告里少几列：那是查错方向都定不下来的那种错。
        layout: JSON.parse(mod.statsJson()),
        // 样式现状。与 selection 同档：小字符串，随 state 推过去，主线程读它是同步的 ——
        //   面板初始化控件要用它，而默认值只在 EngineConfig 里有一份。
        caretStyle: JSON.parse(mod.caretStyleJson()),
        selectionStyle: JSON.parse(mod.selectionStyleJson())
      });
      const pack = performance.now() - tPack;
      packSum += pack;
      ++packN;
      if (pack > packMax) packMax = pack;
      scrollReset = false;
    }
    function push(ts, opName, changed) {
      sendState(ts, opName, render(opName), changed);
    }
    function paintFull() {
      host.invalidateSlots();
      scrollY = 0;
      scrollReset = true;
      scrollResetHost = true;
      const n = maxWindowPages();
      mod.setViewportAnchor(0, n);
      lastAnchor = "0:" + n;
      mod.relayoutAndPaint();
      textAllCache = mod.documentTextAll();
    }
    function loadScenario(id, zoom) {
      if (zoom) {
        mod.setZoom(zoom);
        zoomLevel = zoom;
      }
      mod.setScenario(id);
      paintFull();
    }
    function loadBlocks(fn) {
      fn();
      paintFull();
      return true;
    }
    function glueBytesInWorker() {
      let n = 0;
      for (const e of performance.getEntriesByType("resource")) {
        if (e.name.includes("heditor")) n += e.transferSize || e.decodedBodySize || 0;
      }
      return n;
    }
    async function boot(m) {
      const t0 = performance.now();
      const factory = await host.loadGlue(m.glue, m.exportName || "CreateHEditor");
      if (typeof factory !== "function") {
        throw new Error("胶水层没导出 " + (m.exportName || "CreateHEditor"));
      }
      const wasmPath = String(m.wasm || "");
      const base = m.glue.replace(/[^/]*$/, "");
      mod = await factory({
        locateFile: (p) => wasmPath && /\.wasm$/.test(p) ? wasmPath : base + p + (m.bust || "")
      });
      dpr = m.dpr || 1;
      viewW = m.viewW || 1;
      viewH = m.viewH || 1;
      fontList = m.fonts || [];
      await installFonts(m.fontMode || "full");
      bootMs = performance.now() - t0;
      post({
        t: "ready",
        bootMs,
        fontMs,
        fontBytes,
        fontMode,
        glueBytes: glueBytesInWorker(),
        heapMB: mod.HEAPU8.length / 1048576
      });
      try {
        mod.setPageMetrics(kPageWidthPt, kPageHeightPt, kPageMarginPt);
        if (m.zoom) {
          mod.setZoom(m.zoom);
          zoomLevel = m.zoom;
        }
        if (m.empty) loadBlocks(() => mod.loadTextDocument("", 1));
        else loadScenario(m.scenario === void 0 ? 6 : m.scenario, m.zoom);
        sendState(0, "boot", JSON.parse(mod.editStatsJson()));
      } catch (e) {
        err(e, "boot:layout");
      }
    }
    const WRITE = collect("w", {
      // 什么都不改，只收尾。给宿主的 present() 用 —— 它就是"重画并回推"。
      present: () => {
      },
      scenario: (a) => loadScenario(a.id, a.zoom),
      reset: (a) => loadScenario(a.id === void 0 ? 6 : a.id, a.zoom === void 0 ? kZoomBase : a.zoom),
      zoom: (a) => {
        mod.setZoom(a.zoom);
        zoomLevel = a.zoom;
        if (a.dpr) dpr = a.dpr;
        paintFull();
      },
      stress: (a) => loadBlocks(() => mod.loadStressDocument(a.paras | 0, a.chars | 0)),
      stressDiverse: (a) => loadBlocks(() => mod.loadStressDocumentDiverse(a.paras | 0, a.chars | 0)),
      textDoc: (a) => loadBlocks(() => mod.loadTextDocument(a.text, a.repeat | 0 || 1)),
      formSample: (a) => loadBlocks(() => mod.loadFormSample(a.variant | 0)),
      /**
       * 打开一份保存过的文档（JSON 文本）。
       *
       * 走 loadBlocks 而不是普通 present：换的是整份文档 —— 页数、页度量、窗口锚点全变了，
       *   与 textDoc / stress 同一档。失败时引擎一个字节都没动，所以这里直接抛给宿主显示，
       *   不 paintFull（画面上仍是失败前那份，什么都不用做）。
       */
      loadDocument: (a) => loadBlocks(() => {
        const err2 = mod.loadDocument(a.text || "");
        if (err2) throw new Error(err2);
      }),
      fonts: async (a) => {
        await installFonts(a.mode || "full");
        paintFull();
      },
      insertText: (a) => mod.insertTextAt(a.para | 0, a.offset | 0, a.s),
      type: (a) => mod.typeText(a.s),
      backspace: () => mod.backspace(),
      deleteForward: () => mod.deleteForward(),
      splitParagraph: () => mod.splitParagraph(),
      mergePrev: () => mod.mergeWithPreviousParagraph(),
      mergeNext: () => mod.mergeWithNextParagraph(),
      move: (a) => mod.moveCaret(a.dcp | 0, !!a.extend),
      /**
       * 上下键（dir < 0 上 / > 0 下）与文档首尾。extend = Shift 扩展（锚点不动）。
       *   与 move 同一档：引擎的 *AndRender 版自己收尾，这里不额外 present。
       */
      vertical: (a) => mod.moveCaretVertical(a.dir | 0, !!a.extend),
      docStart: (a) => mod.caretToDocStart(!!a.extend),
      docEnd: (a) => mod.caretToDocEnd(!!a.extend),
      /**
       * 选区三个入口。两端是全局流位置（与事务里那份快照同一把尺子），不是段内偏移。
       */
      selectAll: () => mod.selectAll(),
      // 整格选区：段号来自 hit 的 cellPara 字段（引擎的几何结论），宿主原样搬回来。
      selectCellAtPara: (a) => mod.selectCellAtPara(a.para | 0),
      // 三击：按点选整格。点不在表格里返回 false，宿主退回落光标。
      //   必须留在这里（命令表）：它会重画帧，而查询那条路不重贴脏页 —— 画面要等下一次
      //   闪烁相位才更新，那就是"点了要等一秒才选中"。
      cellSelectAt: (a) => mod.cellSelectAt(a.x, a.y),
      // 外部 API 收 {段号, 段内偏移}；换算成全局流位置只在引擎里做（见 flowIndexAt）。
      selectPos: (a) => mod.setSelection(
        mod.flowIndexAt(a.anchorPara | 0, a.anchorOffset | 0),
        mod.flowIndexAt(a.focusPara | 0, a.focusOffset | 0)
      ),
      setSelection: (a) => mod.setSelection(a.anchor | 0, a.focus | 0),
      clearSelection: () => mod.clearSelection(),
      // 落光标的三个入口都要无条件把相位拉回可见，与单线程宿主的 setCaretAndPresent
      //   逐字对齐（那边就是 mod.setCaret(...); caretOn = true; mod.showCaret(true);
      //   mod.present();）。漏掉的症状：相位停在隐藏那半拍时落一次光标，光标不出现 ——
      //   而这正是"点画布换字段后光标看不见"那个用户 bug 的一半。
      setCaret: (a) => {
        mod.setCaret(a.para | 0, a.offset | 0);
        mod.showCaret(true);
      },
      setCaretAt: (a) => {
        mod.setCaretAt(a.para | 0, a.offset | 0, a.field | 0);
        mod.showCaret(true);
      },
      setCaretAtSide: (a) => {
        mod.setCaretAtSide(a.para | 0, a.offset | 0, a.side | 0);
        mod.showCaret(true);
      },
      // 带行别的版本：折行处"上一行行尾"与"下一行行首"是同一个偏移与侧别，
      //   只有行别分得开（点击落点走它，见 EditorEngine::caretLineAffinity）。
      setCaretAtSideLine: (a) => {
        mod.setCaretAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
        mod.showCaret(true);
      },
      setFocusAtSideLine: (a) => {
        mod.setFocusAtSideLine(a.para | 0, a.offset | 0, a.side | 0, a.line | 0);
        mod.showCaret(true);
      },
      // 拖选的移动端：只搬焦点端，anchor 不动。与 setCaretAtSide 同档 ——
      //   引擎不自己重绘，所以它也在 PRESENT_ALWAYS 里（否则拖动时选区不更新）。
      setFocusAtSide: (a) => {
        mod.setFocusAtSide(a.para | 0, a.offset | 0, a.side | 0);
        mod.showCaret(true);
      },
      indent: () => mod.indent(),
      outdent: () => mod.outdent(),
      tabCaret: (a) => mod.tabCaret(!!a.backward),
      lineStart: (a) => mod.caretToLineStart(!!a.extend),
      lineEnd: (a) => mod.caretToLineEnd(!!a.extend),
      undo: () => mod.undoEdit(),
      redo: () => mod.redoEdit(),
      setUndoLimit: (a) => mod.setUndoLimit(a.n | 0),
      setEditMode: (a) => mod.setEditMode(a.mode | 0),
      setShowBrackets: (a) => mod.setShowBrackets(!!a.on),
      setShowControlDots: (a) => mod.setShowControlDots(!!a.on),
      /**
       * 光标与选区的样式。缺的字段按现状补 —— 与单线程宿主门面同一套补全规则，
       *   两处不能各写一套（面板只改颜色时，宽度必须原样保留）。
       */
      caretStyle: (a) => {
        const s = a.style || {};
        const cur = JSON.parse(mod.caretStyleJson());
        const c = s.color || cur.color;
        mod.setCaretStyle(
          c[0] | 0,
          c[1] | 0,
          c[2] | 0,
          (c[3] === void 0 ? 255 : c[3]) | 0,
          s.widthPt === void 0 ? cur.widthPt : Number(s.widthPt)
        );
      },
      selectionStyle: (a) => {
        const s = a.style || {};
        const cur = JSON.parse(mod.selectionStyleJson());
        const c = s.color || cur.color;
        mod.setSelectionStyle(c[0] | 0, c[1] | 0, c[2] | 0, (c[3] === void 0 ? 255 : c[3]) | 0);
      },
      /**
       * 换字段名单。必须重排，不能只 present —— 括号标记（含只读的灰括号）是排进
       *   布局结果里的，只 present 的话标记还是上一轮的：症状是"控件清单里 ro=true，
       *   可槽清单里 ro 全是 false"（一条自相矛盾的读数，很难往"没重排"上想）。
       *   对照单线程宿主的 setFormControls：它就是 return relayoutAndBlit();。
       */
      setFormControls: (a) => {
        mod.setFormControls((a.list || []).map((it) => ({
          start: it.start,
          end: it.end,
          kind: it.kind || 0,
          id: it.id || "",
          ro: !!it.ro
        })));
        paintFull();
      },
      /**
       * 闪烁那一拍：摆相位并呈现（对照单线程宿主 tick 里的 showCaret + present）。
       *   它必须是"会收尾"的那一类 —— 闪烁要在屏幕上看得见，就得重画。
       *   而门面的 showCaret / stopBlink 走的是 SILENT 那张表（只改状态不呈现），
       *   两者不能混：混了就会把探针的"全量基线"（分母）覆盖掉。
       */
      blinkPhase: (a) => mod.showCaret(!!a.on),
      focusControl: (a) => mod.focusControl(a.forward === void 0 ? true : !!a.forward),
      removeControl: (a) => mod.removeControlAt(a.i | 0, !!a.keepContent),
      setControlProp: (a) => mod.setControlPropAt(a.i | 0, a.prop | 0, !!a.value),
      setFontCacheConfig: (a) => mod.setFontCacheConfig(a.blockSize | 0, a.blocks | 0),
      /**
       * 视口锚点：与单线程宿主的门面 setViewportAnchor 逐字对齐 —— 直接钉锚点（不碰滚动
       *   位置），并且必须呈现。
       * 也不能让它走 retarget：retarget 会按"滚动位置算出来的可见页"把锚点又挪回可见范围。
       *   探针靠这个入口把窗口挪到前/中/后去量按需读的覆盖面 —— 被挪回去就会量到别的页，
       *   而且是静默的（数字看着正常，只是不是那一页）。
       */
      viewport: (a) => {
        mod.setViewportAnchor(a.first | 0, a.count | 0);
        lastAnchor = (a.first | 0) + ":" + (a.count | 0);
      }
    });
    const SILENT = collect("s", {
      phase: (a) => mod.showCaret(!!a.on)
    });
    const READ = collect("r", {
      /**
       * 排空屏障：什么都不算，立刻回一个值。
       *
       * 用途是给宿主一个"顺序保证"—— postMessage 保序，所以这条 reply 回来时，它之前
       *   发出的消息都已经处理完了。这是探针的同步点（交互之后要读到那一拍的结果）。
       * 它刻意不碰 mod、也不走 push：哨兵不能有副作用（一次 present 就会改变帧，
       *   把"某个入口忘了收尾"这类断言直接盖掉）。
       */
      ping: () => 1,
      /**
       * 一页的脏矩形（页内像素坐标），读后清零。
       *
       * 5B 的按页接口：宿主按它决定 putImageData 哪几块。光标移动只应该给出两三个小矩形，
       *   而不是整页 —— 那一条的判据在 edit_probe / edit_test [58]。
       */
      pageDirtyRects: (a) => JSON.parse(mod.pageDirtyRectsJson(a.page | 0)),
      /**
       * 还欠着脏矩形的页号。宿主按它决定贴哪几页 —— 不能用 dirtyPages：
       *   那一份是"本拍写过哪几页"，而矩形是累积的，两者不同步就会永久漏贴几块。
       */
      pageDirtyPages: () => JSON.parse(mod.pageDirtyPagesJson()),
      text: (a) => mod.documentText(a.i | 0),
      // 整篇一次拼好：宿主每拍都要一份全文（change 事件带它），逐段取是 N 次跨界。
      textAll: () => mod.documentTextAll(),
      /**
       * 保存：当前文档 → JSON 文本。空串 = 没装载文档（引擎只有这一种失败）。
       *
       * 走查询而不是命令：它不改任何状态，也不该触发一次呈现 —— 保存不该有副作用。
       */
      serialize: () => mod.serializeDocument(),
      /**
       * 整帧里"非白"的像素数。主线程读不到画布（所有权在 Worker 这边），所以"帧上到底
       *   有没有字"这个判据只能在这里数 —— 与单线程版探针的口径一致（任一通道 < 200 算一个）。
       */
      ink: () => {
        const v = mod.frameRgba();
        let n = 0;
        for (let i = 0; i + 3 < v.length; i += 4) {
          if (v[i] < 200 || v[i + 1] < 200 || v[i + 2] < 200) ++n;
        }
        return n;
      },
      /**
       * 逐行的墨迹与底色，按文档顺序给出窗口内每一条带的那一段。
       *
       * 判据用它回答两个问题，都是"连页到底连不连"的直接读数：
       *   一、条带里有没有"不是白底"的行 —— 有就是还有页间隙或页边框（分页的痕迹）。
       *   二、相邻条带的文字行距是不是与条带内部一样 —— 不一样就是内容在边界上断开了。
       * 为什么在 Worker 侧数：帧的所有权在这一侧（主线程拿不到画布）。这是测量，
       *   走的是与绘制不同的代码路径（只读帧、不做任何布局判断），不会自我循环验证。
       */
      inkRows: () => {
        const f = mod.frameRgba();
        const fw = mod.frameWidth();
        const ph = mod.pagePxHeight();
        const step = ph + mod.pageGapPx();
        const first = mod.windowFirstPage();
        const count = mod.windowPageCount();
        const out = [];
        for (let p = first; p < first + count; ++p) {
          const base = (p - first) * step;
          const ink = [];
          const sel = [];
          const selDeep = [];
          let gray = 0;
          for (let y = 0; y < step; ++y) {
            const off = (base + y) * fw * 4;
            if (off + fw * 4 > f.length) {
              ink.push(-1);
              sel.push(-1);
              selDeep.push(-1);
              continue;
            }
            const l = off + 4;
            if (f[l] < 250 || f[l + 1] < 250 || f[l + 2] < 250) ++gray;
            let n = 0;
            let s = 0;
            let deep = 0;
            for (let x = 0; x < fw; x += 2) {
              const i = off + x * 4;
              if (f[i] < 128 && f[i + 1] < 128 && f[i + 2] < 128) ++n;
              else if (f[i + 2] - f[i] > 15 && f[i] > 150) {
                ++s;
                if (f[i] < 205) ++deep;
              }
            }
            ink.push(n);
            sel.push(s);
            selDeep.push(deep);
          }
          out.push({ page: p, step, ink, sel, selDeep, gray });
        }
        return { first, count, step, ph, slabs: out };
      },
      /**
       * 某几段的行盒与字形（诊断"字画到哪儿去了"）。见 wasm 侧 linesJson 的说明。
       */
      lines: (a) => JSON.parse(mod.linesJson(a.from | 0, a.to | 0)),
      /**
       * 帧的指纹（FNV-1a，只算 RGB 三通道）。
       *
       * 判据用它回答"同一份逻辑状态重画一次，像素是不是一个字都没变" —— 增量绘制最容易犯的
       *   错是"重画时把叠加式的效果又叠了一遍"（选区混色就是这么变深的）。整帧搬回去比太贵
       *   （一份就是窗口像素 x 4，dpr 2 下可达 80 MB），指纹在这一侧算完只回一个数。
       */
      frameDigest: () => {
        const v = mod.frameRgba();
        let h = 2166136261;
        for (let i = 0; i + 3 < v.length; i += 4) {
          h ^= v[i];
          h = Math.imul(h, 16777619);
          h ^= v[i + 1];
          h = Math.imul(h, 16777619);
          h ^= v[i + 2];
          h = Math.imul(h, 16777619);
        }
        return h >>> 0;
      },
      // 帧与页的几何读数：宿主排版、滚动条与自检都要用。都是标量，跨传输安全。
      frameWidth: () => mod.frameWidth(),
      frameHeight: () => mod.frameHeight(),
      frameByteSize: () => mod.frameByteSize(),
      pagePxWidth: () => mod.pagePxWidth(),
      pagePxHeight: () => mod.pagePxHeight(),
      pageGapPx: () => mod.pageGapPx(),
      windowFirstPage: () => mod.windowFirstPage(),
      windowPageCount: () => mod.windowPageCount(),
      // 光标与选区的即时读数。它们不经过 edit_stats 那本账，拿它当分母会读错。
      caretOffsetLive: () => mod.caretOffsetLive(),
      caretVerticalNow: () => mod.caretVerticalNow(),
      caretBlockReversedNow: () => mod.caretBlockReversedNow(),
      selectionReport: () => JSON.parse(mod.selectionReport()),
      // 流位置 -> 段内位置（选区两端的对外形状）。
      flowTextPos: (a) => JSON.parse(mod.flowTextPosJson(a.flow | 0)),
      // 样式现状：写入口是 caretStyle / selectionStyle（缺字段按现状补），读是这两条。
      caretStyleJson: () => JSON.parse(mod.caretStyleJson()),
      selectionStyleJson: () => JSON.parse(mod.selectionStyleJson()),
      // 构建与后端标识：诊断面板与探针报告要用。
      buildStamp: () => typeof mod.buildStamp === "function" ? mod.buildStamp() : "",
      rasterBackend: () => mod.rasterBackend ? mod.rasterBackend() : "?",
      stats: () => JSON.parse(mod.statsJson()),
      editStats: () => JSON.parse(mod.editStatsJson()),
      caretReport: () => mod.caretReport(),
      hit: (a) => JSON.parse(mod.hitTest(a.x, a.y)),
      controlAt: (a) => mod.controlAtPoint(a.x, a.y),
      describeControls: () => mod.describeControls(),
      controls: () => JSON.parse(mod.controlsJson()),
      bracketSlots: () => JSON.parse(mod.bracketSlotsJson()).slots,
      blockedEdits: () => mod.blockedEdits(),
      prewarmPage: (a) => mod.prewarmPageGlyphs(a.page | 0),
      dirtyPages: () => JSON.parse(mod.dirtyPagesJson()),
      editMode: () => mod.editMode(),
      showBrackets: () => mod.showBrackets(),
      showControlDots: () => mod.showControlDots(),
      undoLimit: () => mod.undoLimit(),
      auditFrame: () => JSON.parse(mod.auditFrame()),
      auditBase: () => JSON.parse(mod.auditBase()),
      // 页自审：第 p 页的缓存 vs 从零光栅。帧与底图两条自审读的都是缓存，
      //   缓存本身写错时它们照样全绿 —— 这一条是那一层的唯一参照物。
      auditPage: (a) => JSON.parse(mod.auditPage(a.page | 0)),
      auditLayout: () => mod.auditLayout ? JSON.parse(mod.auditLayout()) : null,
      auditCompile: () => mod.auditCompile ? JSON.parse(mod.auditCompile()) : null,
      auditGlyphPages: () => mod.auditGlyphPages ? JSON.parse(mod.auditGlyphPages()) : null,
      bench: (a) => JSON.parse(mod.benchRelayoutPaint(a.runs || 12)),
      heap: () => mod.HEAPU8.length / 1048576,
      /**
       * 自检：把每页画布上真正的像素与帧里对应的一段逐点比。
       *
       * 返回形状必须与单线程宿主的 selfCheck 一致 —— 它是给用户的取证入口
       *   （F12 里 __heditor.selfCheck() 把对象发出来），也是探针的主判据：
       *   screenVsEngineDiffPx > 0 说明"上屏"贴错了，恒为 0 说明问题在引擎的排版与光栅化。
       *   形状不一致的代价不是"少几个字段"：探针读 sc.framePx[0] 会直接抛，
       *   整段断言连跑都跑不到（表现为一个 TypeError，看不出是这个原因）。
       * 多出来的 compared / ratio / missing / sizeBad 是本侧扩展：缺页与尺寸不符单独报数 ——
       *   "有页还没拿到画布"和"画错了"是两件事，混在一起会让人查错方向。
       */
      selfCheck: () => {
        host.blitPages();
        const fw = mod.frameWidth();
        const fh = mod.frameHeight();
        const pw = mod.pagePxWidth();
        const ph = mod.pagePxHeight();
        const first = mod.windowFirstPage();
        const count = mod.windowPageCount();
        let diff = 0;
        let compared = 0;
        let missing = 0;
        let sizeBad = 0;
        let comparedPages = 0;
        let minx = 1e9;
        let miny = 1e9;
        let maxx = -1;
        let maxy = -1;
        for (let p = first; p < first + count; ++p) {
          const c = host.ctxOfPage(p);
          if (!c) {
            ++missing;
            continue;
          }
          if (c.canvas.width !== pw || c.canvas.height !== ph) {
            ++sizeBad;
            continue;
          }
          const src = mod.pageRgba(p);
          if (src.length !== pw * ph * 4) {
            ++sizeBad;
            continue;
          }
          ++comparedPages;
          const D = c.getImageData(0, 0, pw, ph).data;
          compared += pw * ph;
          for (let i = 0; i + 3 < D.length; i += 4) {
            if (D[i] === src[i] && D[i + 1] === src[i + 1] && D[i + 2] === src[i + 2]) continue;
            ++diff;
            const q = i >> 2;
            const x = q % pw;
            const y = (q - x) / pw;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
          }
        }
        const st = JSON.parse(mod.editStatsJson());
        return {
          text: mod.documentText(0),
          docCodepoints: st.docCodepoints,
          caret: {
            para: st.caretPara,
            offset: st.caretOffset,
            x: st.caretX,
            y: st.caretY,
            w: st.caretW,
            h: st.caretH,
            visible: st.caretVisible,
            page: st.caretPage
          },
          framePx: [fw, fh],
          window: { first, count, comparedPages },
          pageCanvases: host.canvasCount(),
          // cssSize / rectPx 要看主线程上的 DOM，由宿主那边补（见 worker_host.js 的 selfCheck 包装）。
          pages: { backing: [mod.pagePxWidth(), ph], cssSize: null, rectPx: null },
          dpr,
          zoom: zoomLevel,
          backend: mod.rasterBackend ? mod.rasterBackend() : "?",
          screenVsEngineDiffPx: diff,
          screenVsEngineDiffBox: diff ? [minx, miny, maxx, maxy] : null,
          repainted: {
            pages: st.repaintedPages,
            pixels: st.repaintedPixels,
            w: st.repaintedW,
            h: st.repaintedH
          },
          diff,
          compared,
          missing,
          sizeBad,
          ratio: compared ? diff / compared : 1
        };
      },
      /**
       * 每个可编辑字段的左括号在帧上到底画了没有（扫左缘那一列里的"括号蓝"）。
       *   判据与探针一致：偏蓝够远 + 够亮 + 红通道 > 45（把光标色排除掉）。
       *   括号常显之后每个可编辑字段都必须有；"只画聚焦字段"那一档已经删了。
       */
      markCheck: () => {
        const cc = JSON.parse(mod.controlsJson());
        const f = mod.frameRgba();
        const fw = mod.frameWidth();
        const near = (r, g, b) => r > 45 && b - r > 60 && b > 140;
        const rows = [];
        for (let ci = 0; ci < cc.items.length; ++ci) {
          const it = cc.items[ci];
          if (it.ro || !it.rects.length) continue;
          const cx = it.rects[0].x;
          const lo = Math.round((cx - 10) * zoomLevel);
          const hi = Math.round((cx - 1) * zoomLevel);
          let n = 0;
          for (let i = 0; i + 3 < f.length; i += 4) {
            const x = (i >> 2) % fw;
            if (x < lo || x > hi) continue;
            if (near(f[i], f[i + 1], f[i + 2])) ++n;
          }
          rows.push({ control: ci, focused: ci === cc.caret, leftEdgePx: n, ok: n > 0 });
        }
        return {
          mode: cc.mode,
          showBrackets: mod.showBrackets(),
          count: cc.count,
          caretControl: cc.caret,
          zoom: zoomLevel,
          rows,
          allOk: rows.every((r) => r.ok)
        };
      }
    });
    const PREFETCH_PAGES = 1;
    const PREFETCH_DELAY_MS = 90;
    let pfTimer = 0;
    let pfNext = -1;
    let pfLeft = 0;
    let pfDir = 1;
    function schedulePrewarm(dir) {
      if (!mod) return;
      pfDir = dir < 0 ? -1 : 1;
      pfNext = pfDir > 0 ? mod.windowFirstPage() + mod.windowPageCount() : mod.windowFirstPage() - 1;
      pfLeft = PREFETCH_PAGES;
      if (pfTimer) clearTimeout(pfTimer);
      pfTimer = setTimeout(pumpPrewarm, PREFETCH_DELAY_MS);
    }
    function pumpPrewarm() {
      pfTimer = 0;
      if (!mod || pfLeft <= 0) return;
      const total = Math.max(1, JSON.parse(mod.editStatsJson()).totalPages);
      if (pfNext < 0 || pfNext >= total) return;
      mod.prewarmPageGlyphs(pfNext);
      pfNext += pfDir;
      --pfLeft;
    }
    async function doOp(m) {
      const silent = SILENT[m.name];
      if (silent) {
        await silent(m.args || {});
        return void post({ t: "ack", ts: m.ts });
      }
      const fn = WRITE[m.name];
      if (!fn) throw new Error("未知命令 " + m.name);
      const ret = await fn(m.args || {});
      return void push(m.ts, m.name, ret);
    }
    async function askOp(m) {
      if (m.name === "rawFrame") {
        const v = mod.frameRgba();
        const copy = new Uint8ClampedArray(v.length);
        copy.set(v);
        return void post({ t: "reply", id: m.id, value: copy }, [copy.buffer]);
      }
      const fn = READ[m.name];
      if (!fn) throw new Error("未知查询 " + m.name);
      return void post({ t: "reply", id: m.id, value: await fn(m.args || {}) });
    }
    function view(m) {
      const tWork = performance.now();
      const prevScrollY = scrollY;
      scrollY = Math.max(0, m.scrollY || 0);
      scrollLead = Number(m.leadPx) || 0;
      viewW = m.viewW || viewW;
      viewH = m.viewH || viewH;
      if (scrollY !== prevScrollY) scrollDir = scrollY > prevScrollY ? 1 : -1;
      const scrolled = scrollY !== prevScrollY;
      if (!retarget(JSON.parse(mod.editStatsJson()))) {
        return { moved: false, scrolled, workMs: performance.now() - tWork };
      }
      mod.present();
      host.blitPages();
      schedulePrewarm(scrollY - prevScrollY);
      return {
        moved: true,
        scrolled,
        workMs: performance.now() - tWork,
        stats: JSON.parse(mod.editStatsJson())
      };
    }
    function setViewMode(m) {
      if (typeof m.gap === "number") mod.setPageGap(m.gap);
      mod.setContinuous(!!m.continuous);
    }
    function pageCanvas(m) {
      host.addCanvas(m);
      sendState(m.ts, "pageCanvas", JSON.parse(mod.editStatsJson()));
    }
    function viewMode(m) {
      host.setViewMode(m);
      sendState(m.ts, "viewMode", JSON.parse(mod.editStatsJson()));
    }
    function relayout() {
      mod.relayoutAndPaint();
      host.invalidateSlots();
      host.blitPages();
    }
    function settle(opName, m) {
      const prev = scrollY;
      scrollY = Math.max(0, Number(m.scrollY) || 0);
      if (scrollY !== prev) scrollDir = scrollY > prev ? 1 : -1;
      let st = JSON.parse(mod.editStatsJson());
      const moved = retarget(st);
      if (moved || PRESENT_ALWAYS.has(opName)) {
        mod.present();
        st = JSON.parse(mod.editStatsJson());
      }
      host.blitPages();
      return { moved, scrolled: scrollY !== prev, stats: st };
    }
    function read(name, args) {
      if (name === "rawFrame") {
        const v = mod.frameRgba();
        const copy = new Uint8ClampedArray(v.length);
        copy.set(v);
        return copy;
      }
      const fn = READ[name];
      if (!fn) throw new Error("未知读取 " + name);
      return fn(args || {});
    }
    function write(name, args) {
      const fn = WRITE[name] || SILENT[name];
      if (!fn) throw new Error("未知命令 " + name);
      return fn(args || {});
    }
    function attach(engine) {
      mod = engine;
    }
    return {
      boot,
      do: doOp,
      ask: askOp,
      view,
      pageCanvas,
      viewMode,
      setViewMode,
      relayout,
      sendState,
      geom,
      settle,
      attach,
      read,
      write,
      /**
       * 装载类命令把滚动位置归零了：读一次就清（宿主按它追平 scrollTop）。
       */
      takeScrollReset: () => {
        const v = scrollResetHost;
        scrollResetHost = false;
        return v;
      },
      // 引擎句柄：只给宿主取帧视图与装载用，不要拿它绕过命令表。
      engine: () => mod,
      // 当前滚动位置（设备像素）：贴连页时算目标 y 要用。
      scrollY: () => scrollY,
      // 视口尺寸（设备像素）。
      viewSize: () => ({ w: viewW, h: viewH }),
      state: (ts, label, stats, changed, workMs) => sendState(ts, label, stats, changed, workMs)
    };
  }
  globalThis.__heditorEngineApi = {
    createEngineApi,
    IDL
  };
  exports.IDL = IDL;
  exports.createEngineApi = createEngineApi;
  Object.defineProperty(exports, Symbol.toStringTag, { value: "Module" });
  return exports;
}({});
