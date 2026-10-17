// 拖选：按下、移动、抬起这一条链，两套宿主共用一份。
//
// 为什么共享：这条链里有三处容易分叉的地方 —— 起始阈值、边缘自动滚动的节拍、
// 以及异步宿主下"同一时刻只允许一次命中测试在飞"的合并。各写一份，迟早会出现
// "单线程能选、Worker 上选不动"这类只在一边复现的偏差。
//
// 宿主注入三件事（按下时的定位由宿主自己做，模块只管"按下之后"），模块不碰引擎门面：
//   pointAt(cx, cy)   客户端坐标 -> 文档设备像素
//   hitAt(x, y)       命中：返回 {para, offset, side}；同步值或 Promise 都收
//   extendTo(hit)     移动时的焦点端搬运（只动 focus）
// 另加一个滚动容器（container），边缘自动滚动用它。

(function () {
  'use strict';

  // 起始阈值（CSS 像素）：超过它才算真的在拖。没有它，手抖一像素就多出一格选区。
  //   用 CSS 像素而不是设备像素：阈值是手感，不该随 DPR 变。
  const THRESHOLD_CSS = 3;

  // 边缘自动滚动的内缩：留一点，不要正好停在容器边框上。
  const EDGE_INSET_CSS = 6;

  // 每帧滚动量（CSS 像素）：越界 RAMP_CSS 达到上限。再快就看不清选到哪了。
  const MAX_SCROLL_CSS = 24;
  const RAMP_CSS = 60;

  // 收尾信号："按下结束了"不止一条来源，缺一条就会出现"松开了还在拖"。
  //   mouseup 是主路径。pointerup / pointercancel 覆盖笔与触摸 —— 浏览器会给触摸补一套
  //   鼠标事件，但补得不全（拖动被识别成滚动时只剩 pointercancel）。
  //   dragend / drop 是"浏览器把按下接管成了原生拖放"：那种情况下鼠标事件流会被
  //   直接截断，页面再也收不到 mouseup，只留下这几个 —— 症状正是"松开了还在拖"。
  const END_EVENTS = ['mouseup', 'pointerup', 'pointercancel', 'dragend', 'drop'];

  /**
   * 造一个拖选控制器。
   *
   * begin 由宿主的按下处理调用（命中与定位都由宿主做完，这里只管"按下之后"）；
   * 之后的移动与抬起由模块挂在 window 上 —— 指针离开画布、离开滚动容器都要继续收，
   * 挂在画布上会在拖出边界时断掉。
   */
  window.createDragSelect = function createDragSelect(opts) {
    const container = opts.container;
    const threshold = opts.thresholdCss === undefined ? THRESHOLD_CSS : opts.thresholdCss;

    let st = null;          // 活动状态；null = 没在拖
    let raf = 0;            // 边缘自动滚动的帧句柄
    let pending = null;     // 最近一个待处理的文档坐标（合并用）
    let inFlight = false;   // 有一次命中还没回来
    let waiters = [];       // settled() 的等待者

    function flushSettled() {
      const w = waiters;
      waiters = [];
      for (let i = 0; i < w.length; ++i) w[i]();
    }

    function idle() { return !st && !inFlight && !pending && !raf; }

    // 诊断读数：探针与状态栏用它看"现在到底在拖什么"。
    function state() {
      if (!st) {
        return { active: false, armed: false, moved: false, extends: 0,
                 autoScroll: false, x: 0, y: 0, scrollTop: container.scrollTop };
      }
      return { active: true, armed: st.armed, moved: st.moved, extends: st.extends,
               autoScroll: st.held, x: st.lastCx, y: st.lastCy,
               scrollTop: container.scrollTop };
    }

    // 全部在途的命中都回来了才落地。异步宿主上没有它就只能靠睡觉等。
    function settled() {
      if (idle()) return Promise.resolve();
      return new Promise((fn) => { waiters.push(fn); });
    }

    /**
     * 把一个点交给命中测试，回来就搬焦点端。
     *
     * 同一时刻只允许一次在飞：边缘自动滚动每帧都会发起一次，不合并的话异步宿主上
     *   会排起长队，选区看起来追着鼠标跑却总是慢几拍。
     */
    function pump() {
      if (!st || inFlight || !pending) return;
      const pt = pending;
      pending = null;
      inFlight = true;
      const done = (hit) => {
        inFlight = false;
        if (st && hit) { opts.extendTo(hit); st.extends++; }
        if (pending) pump();
        else if (!raf) flushSettled();
      };
      let r;
      try {
        r = opts.hitAt(pt.x, pt.y);
      } catch (e) {
        inFlight = false;
        throw e;
      }
      Promise.resolve(r).then(done, () => done(null));
    }

    function queueAt(cx, cy) {
      if (!st) return;
      const pt = opts.pointAt(cx, cy);
      if (!pt) return;
      pending = pt;
      pump();
    }

    // 容器内容框（已内缩），一次取好省得同一帧里反复触发布局。
    function contentBox() {
      const rect = container.getBoundingClientRect();
      return { top: rect.top + EDGE_INSET_CSS, bottom: rect.bottom - EDGE_INSET_CSS };
    }

    // 指针越界多少像素换算成这一帧该滚多少；0 = 在框内。
    function edgeDeltaPx(box) {
      if (st.lastCy < box.top) return -ramp(box.top - st.lastCy);
      if (st.lastCy > box.bottom) return ramp(st.lastCy - box.bottom);
      return 0;
    }

    function ramp(over) {
      const v = Math.round((over / RAMP_CSS) * MAX_SCROLL_CSS);
      return Math.max(1, Math.min(MAX_SCROLL_CSS, v));
    }

    function clampedCy(box) {
      return Math.max(box.top, Math.min(box.bottom, st.lastCy));
    }

    /**
     * 边缘自动滚动的一帧。
     *
     * 滚动与命中分在两帧：容器一滚，引擎要等滚动事件跑完才会把窗口重新定锚。
     *   同一帧里既滚又命中，换算用的还是旧窗口，症状是选区往回缩一格。
     *   滚动事件在渲染步里先于帧回调，所以隔一帧再命中就是新窗口了。
     */
    function tick() {
      raf = 0;
      if (!st) { flushSettled(); return; }
      if (st.clampY) {
        const y = st.clampY;
        st.clampY = 0;
        queueAt(st.lastCx, y);
      }
      const box = contentBox();
      const dy = edgeDeltaPx(box);
      if (dy === 0) {
        st.held = false;
        flushSettled();
        return;
      }
      const before = container.scrollTop;
      container.scrollTop = before + dy;
      if (container.scrollTop === before) {
        // 已经滚到两端，指针还停在外面 —— 没有可滚的了，按夹住的点收一次尾就停。
        st.held = false;
        queueAt(st.lastCx, clampedCy(box));
        flushSettled();
        return;
      }
      st.held = true;
      st.clampY = clampedCy(box);
      raf = requestAnimationFrame(tick);
    }

    /**
     * 把一个点落成"焦点端搬到那儿"。
     *
     * 指针在容器外时按夹住的点命中：直接用真实坐标会落到窗口外，命中测试把它夹到
     *   窗口最后一行，选区于是先跳到窗口末尾、再被自动滚动拽回来，看起来是抖的。
     *   指针在外面时顺便把下一帧的自动滚动排上。
     */
    function applyAt(cx, cy) {
      const box = contentBox();
      const outside = cy < box.top || cy > box.bottom;
      if (outside) {
        st.clampY = clampedCy(box);
        if (!raf) raf = requestAnimationFrame(tick);
      }
      st.lastCx = cx;
      st.lastCy = cy;
      queueAt(cx, outside ? clampedCy(box) : cy);
    }

    function onMove(ev) {
      if (!st) return;
      // 按钮已经抬起来了，可 mouseup 没到页面 —— 这是"松开了还在拖"最省的一条自愈路：
      //   mousemove 自带 buttons，下一次移动就能收尾。丢 mouseup 的三种情形
      //   （在浏览器窗口之外松手、被别的窗口抢走、被原生拖放截断）都会走到这里，
      //   只要用户还在动鼠标就一定能自愈。
      if (typeof ev.buttons === 'number' && (ev.buttons & 1) === 0) { end(); return; }
      if (!st.armed) {
        // 还没确定算不算拖（命中在往返中），但位置要记下来 —— 见 arm()。
        st.lastCx = ev.clientX;
        st.lastCy = ev.clientY;
        return;
      }
      if (!st.moved) {
        const dx = ev.clientX - st.downCx;
        const dy = ev.clientY - st.downCy;
        if (dx * dx + dy * dy < threshold * threshold) return;
        st.moved = true;
      }
      applyAt(ev.clientX, ev.clientY);
      ev.preventDefault();
    }

    function onUp() { end(); }

    // 只有整个窗口失焦（切标签、切应用）才收尾 —— 那之后 mouseup 不会再到达页面。
    //   元素级失焦不能当收尾信号：宿主在按下的同一条链里会 ta.focus()，焦点从别处
    //   挪到输入框必然派发一次 blur，而 blur 不冒泡、捕获阶段却经过 window，
    //   于是第一次按下刚建立的拖选被当场收掉（症状是同一个位置要拖两次）。
    //   注册时不带捕获，再加 target 判据，两道都只放行窗口自己的 blur。
    function onWindowBlur(ev) { if (ev.target === window) end(); }

    // 拖选期间不许浏览器接管成原生拖放：一旦接管，鼠标事件流当场断掉，
    //   页面再也收不到 mouseup（"松开了还在拖"的另一条来源）。dragend 那条补救
    //   只在接管已经发生时才救得回来，能不让它发生就别让它发生。
    function onDragStart(ev) { if (st) ev.preventDefault(); }

    function end() {
      if (!st) return;
      st = null;
      if (raf) { cancelAnimationFrame(raf); raf = 0; }
      window.removeEventListener('mousemove', onMove, true);
      for (const t of END_EVENTS) window.removeEventListener(t, onUp, true);
      window.removeEventListener('dragstart', onDragStart, true);
      window.removeEventListener('blur', onWindowBlur);
      flushSettled();
    }

    /**
     * 按下。armed 为假表示这一下没落在正文上（灰底、页间空隙）—— 那就只定位，不拖选。
     * 定位本身由宿主先做完（setCaretAt），于是 engine 那边的 anchor 已经落在按下点。
     *
     * 监听必须在按下的那一拍就挂上，不能等命中测试回来。Worker 宿主的命中是一次
     *   往返，而按下与第一次移动之间可能只隔几毫秒 —— 等回来再挂，那几毫秒里的移动
     *   会被整段丢掉（状态停在按下点、moved 一直是 false），而且丢不丢全看往返快慢，
     *   同一段代码时对时错。所以这里先挂上、按"还没确定"起手，
     *   命中回来再由宿主调 arm() 补上那半句。
     */
    function begin(ev, armed) {
      end();
      st = { armed: !!armed, moved: false, extends: 0, held: false, clampY: 0,
             downCx: ev.clientX, downCy: ev.clientY, lastCx: ev.clientX, lastCy: ev.clientY };
      window.addEventListener('mousemove', onMove, true);
      for (const t of END_EVENTS) window.addEventListener(t, onUp, true);
      window.addEventListener('dragstart', onDragStart, true);
      window.addEventListener('blur', onWindowBlur);
    }

    /**
     * 按下那一刻没落在正文上就不拖 —— 但从按下到命中回来之间的移动已经记进 lastC 了，
     * 所以打开时要按最近那个点补一次，否则"按下后立刻快拖"的头一段还是丢了。
     */
    function arm(on) {
      if (!st) return;
      st.armed = !!on;
      if (!st.armed) return;
      const dx = st.lastCx - st.downCx;
      const dy = st.lastCy - st.downCy;
      if (dx * dx + dy * dy < threshold * threshold) return;
      st.moved = true;
      applyAt(st.lastCx, st.lastCy);
    }

    return { begin, end, arm, state, settled };
  };
})();
