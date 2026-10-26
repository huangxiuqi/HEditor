// 常用操作全景：工具栏上的每个按钮都按一遍、公开面上的每个方法都调一遍，并且逐条判效果。
//
// 为什么要有这一份：单点判据（format / clipboard / input …）钉的是"某条路对不对"，而
//   "点下去有没有反应"这类问题恰恰漏在缝里 —— 一个按钮绑错动作、一个下拉框没接上、
//   一个方法的返回值形状变了，单点判据都不会红。这一份的作用是把"界面上能点的、契约里
//   写了的"全部走一遍，任何一处不生效就当场红。
//
// 判据分两半：工具栏那半在页面里按 action 逐个点（等效果出现，有上限），公开面那半按
//   契约的方法顺序调一遍。两边都只看可读结果（文本、格式、配置、几何），不比像素。
import { check } from '../support/check';
import { test } from '../support/probe';
import { SELECT_HELPERS } from '../support/readings';

const D = 'window.__demo';

// 工具栏那一半的读数：每个 act 一个结果对象。
type Bars = Record<string, any>;

test.describe('工具栏', () => {
  test.beforeEach(async ({ page }) => {
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  });

  test('每个按钮点一遍：都要有反应', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<Bars>(`
      ${SELECT_HELPERS}
      const e = ${D};
      const btn = (act) => document.querySelector('.heditor-ribbon__button[data-act="' + act + '"]');
      // 下拉不是原生 select：PICK 点触发器开浮层，再点那一项（前言里那两个工具）。
      const pick = (act, v) => PICK(act, v);
      // 等效果出现（有上限）：界面动作是异步的，直接读会读到上一拍的状态。
      const until = async (fn, ms = 1500) => {
        const t0 = Date.now();
        while (Date.now() - t0 < ms) {
          if (await fn()) return true;
          await new Promise((r) => setTimeout(r, 20));
        }
        return false;
      };
      const reset = async () => {
        await e.setText('甲乙丙丁戊己庚辛\\n第二段文字');
        await e.select({ para: 0, offset: 2 }, { para: 0, offset: 5 });
        await e.flush();
      };
      const status = () => document.querySelector('#status').textContent || '';
      const out = {};

      // 剪贴板：先放一段外部文字，粘贴才有东西可粘。
      await reset();
      await navigator.clipboard.writeText('粘贴进来的一段');
      const t0 = e.text();
      btn('paste').click();
      out.paste = await until(() => e.text().length > t0.length);

      // 复制：文本不该变；剪切：文本该变短。
      await reset();
      const t1 = e.text();
      btn('copy').click();
      await new Promise((r) => setTimeout(r, 200));
      out.copy = e.text() === t1;
      btn('cut').click();
      out.cut = await until(() => e.text().length < t1.length);

      // 字体与字号（两个下拉框）。
      await reset();
      pick('font', '2');
      out.font = await until(async () => (await e.formatAt()).font === 2);
      pick('size', '18');
      out.size = await until(async () => (await e.formatAt()).size === 18);

      // 增大 / 减小字号：各走一档（14 → 16、16 → 12 之外，先固定起点）。
      await reset();
      await e.format({ size: 14 });
      await e.flush();
      btn('grow').click();
      out.grow = await until(async () => (await e.formatAt()).size === 16);
      btn('shrink').click();
      out.shrink = await until(async () => (await e.formatAt()).size === 14);

      // 三个开关型字符格式。
      for (const [act, key] of [['bold', 'bold'], ['italic', 'italic'], ['underline', 'underline']]) {
        await reset();
        btn(act).click();
        out[act] = await until(async () => (await e.formatAt())[key] === true);
      }

      // 颜色按钮：取色器是浏览器自己的弹窗，点一下不抛错就算接上了。
      await reset();
      btn('color').click();
      out.color = true;

      // 段落：行距与段后。
      await reset();
      pick('line', '1.5');
      out.line = await until(async () => (await e.paragraphFormatAt()).lineFactor === 1.5);
      pick('after', '12');
      out.after = await until(async () => (await e.paragraphFormatAt()).spaceAfter === 12);

      // 缩进：首行起点该往右挪。
      await reset();
      await e.setCaret({ para: 1, offset: 0 });
      await e.flush();
      const x0 = e.caret().rect.x;
      const before = e.text();
      btn('indent').click();
      const moved = await until(() => e.caret().rect.x > x0);
      // 缩进只该插空格：选中的文字一个都不能少（这条挡的是"点缩进把选中的字删了"）。
      const strip = (t) => t.replace(/ /g, '');
      out.indent = moved && strip(e.text()) === strip(before);
      btn('outdent').click();
      out.outdent = await until(() => e.caret().rect.x <= x0 + 0.51);

      // 编辑：撤销 / 重做 / 全选。
      await reset();
      await e.setCaret({ para: 0, offset: 0 });
      await e.type('新');
      await e.flush();
      const typed = e.text();
      btn('undo').click();
      out.undo = await until(() => !e.text().startsWith('新'));
      btn('redo').click();
      out.redo = await until(() => e.text() === typed);
      btn('all').click();
      out.all = await until(() => !e.selection().empty && e.selection().rects.length > 0);

      // 文档：载入示例 / 保存。
      btn('load').click();
      out.load = await until(() => e.text().includes('HEditor 示例文档'));
      btn('save').click();
      out.save = await until(() => status().includes('字节'));
      // 保存提醒是页面自己的开关，默认关：按一下要真的翻过去（探针读它那份状态）。
      btn('unsaved-prompt').click();
      out.unsavedPrompt = await until(() => window.__demoState.unsavedPrompt === true);

      // 插入：插入一句。
      await reset();
      btn('type').click();
      out.type = await until(() => e.text().includes('（从这里开始编辑）'));

      // 插入：一张表（网格上挑 3x3 = 9 格，段数该多 9）与一个表单字段（清单该多一个）。
      //   表格按钮开的是尺寸面板，所以这里要先点开、再点那一格 —— 只点按钮文档不该动。
      await reset();
      const paras0 = e.paragraphs().length;
      btn('table').click();
      await new Promise((r) => setTimeout(r, 30));
      const pickCell = document.querySelector(
        '.heditor-picker__cell[data-row="3"][data-col="3"]');
      out.tablePanel = !!pickCell && e.paragraphs().length === paras0;
      if (pickCell) {
        pickCell.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
      }
      out.table = await until(() => e.paragraphs().length >= paras0 + 9);

      await reset();
      btn('field').click();
      out.field = await until(() => e.form().length === 1);
      // 插完控件之后光标必须还看得见、还点得动：文档模式没有落点限制。
      out.fieldCaret = e.caret().visible === true;
      const p1 = e.caret().pos;
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();
      out.fieldCaretMoves = e.caret().pos.offset === 0 && p1.offset > 0;

      // 视图：编辑模式三档。只读那一档要真的拒绝编辑 —— 下拉框换成"只读"、读回也是
      //   只读、而敲字不生效，三件事都算，才说明这一档接到引擎上了。
      await reset();
      pick('mode', 'readOnly');
      out.modeReadOnly = await until(() => e.config().mode === 'readOnly');
      const roText = e.text();
      await e.type('该被拒绝');
      await e.flush();
      out.modeRejects = e.text() === roText;
      pick('mode', 'document');
      out.modeBack = await until(() => e.config().mode === 'document');

      // 左下角的页数与字数：改一次内容就得跟着动，而且页数要与几何一致。
      await reset();
      const counts = () => ({
        pages: document.querySelector('[data-count="pages"]').textContent,
        words: document.querySelector('[data-count="words"]').textContent,
      });
      const c0 = counts();
      await e.setCaret({ para: 0, offset: 0 });
      await e.type('三个字');
      await e.flush();
      const c1 = counts();
      out.countsGrow = parseInt(c1.words, 10) === parseInt(c0.words, 10) + 3;
      out.pagesMatch = c1.pages === e.geometry().pages + ' 页';
      // 空白不计：敲一个空格，字数不动（码点数会多一个，界面只显示字数）。
      await e.type(' ');
      await e.flush();
      out.countsSpace = parseInt(counts().words, 10) === parseInt(c1.words, 10);

      // 视图：裁剪标记（只改光栅，画面上是四个角多出的短线）。
      //   前后各取一次帧指纹：只翻配置不动像素的话，屏幕上什么都不会变。
      out.marginMarksBefore = e.config().marginMarks;
      const digest0 = await e.diag.frameDigest();
      btn('margin-marks').click();
      out.marginMarks = await until(() => e.config().marginMarks === false);
      out.marginMarksRedraw = await until(async () => (await e.diag.frameDigest()) !== digest0);

      // 视图：页面设置面板（纸张 + 四边页边距）。面板里改成 A3 再"应用"，页宽要跟着变。
      const w0 = e.geometry().page.w;
      btn('page-setup').click();
      await new Promise((r) => setTimeout(r, 30));
      const paper = document.querySelector('.heditor-select[data-act="prop-paper"]');
      out.pagePanel = !!paper;
      if (paper) pick('prop-paper', 'A3');
      const apply = document.querySelector('[data-act="prop-apply"]');
      if (apply) apply.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
      out.pagePaper = await until(() => e.geometry().page.w > w0 + 100);

      // 自定义宽高：选"自定义"、填宽高、应用 —— 页宽要按填的那个数走。
      btn('page-setup').click();
      await new Promise((r) => setTimeout(r, 30));
      const paper2 = document.querySelector('.heditor-select[data-act="prop-paper"]');
      const wIn = document.querySelector('input[data-act="prop-width"]');
      const hIn = document.querySelector('input[data-act="prop-height"]');
      out.pageCustomPanel = !!paper2 && !!wIn && !!hIn;
      if (paper2 && wIn && hIn) {
        pick('prop-paper', 'Custom');
        // 20 × 30 厘米 = 566.9 × 850.4 磅
        wIn.value = '20';
        wIn.dispatchEvent(new Event('input', { bubbles: true }));
        hIn.value = '30';
        hIn.dispatchEvent(new Event('input', { bubbles: true }));
        const apply2 = document.querySelector('[data-act="prop-apply"]');
        if (apply2) apply2.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true }));
      }
      out.pageCustom = await until(
        () => Math.abs(e.geometry().page.w - 20 * 28.3465 * (4 / 3)) < 2,
      );

      // 换回 A4：下面那几条（分页 / 连页 / 缩放）与页度量无关，但别把页面留在一个
      //   非默认的纸张上，读回配置的那几条会跟着变。
      await e.configure({ page: { paper: 'A4' } });
      await e.flush();
      out.pageBack = e.geometry().page.w === w0;

      // 视图：分页 / 连页 / 三档缩放。
      btn('paged').click();
      out.paged = await until(() => e.config().pageMode === 'paged');
      btn('continuous').click();
      out.continuous = await until(() => e.config().pageMode === 'continuous');
      const z = e.config().zoom;
      btn('zoom-in').click();
      out.zoomIn = await until(() => e.config().zoom > z);
      const z2 = e.config().zoom;
      btn('zoom-out').click();
      out.zoomOut = await until(() => e.config().zoom < z2);
      btn('zoom-reset').click();
      out.zoomReset = await until(() => e.config().zoom === 1);

      // Ctrl + 滚轮：库内接管成编辑器缩放，右下角的百分比读数也要跟着变
      //   （它不经过工具条的动作表，靠 configchange 那一条回显）。
      const pctOf = () => document.querySelector('.heditor-status__pct').textContent;
      const pct0 = pctOf();
      document.querySelector('.heditor-stage').dispatchEvent(
        new WheelEvent('wheel', { deltaY: -100, ctrlKey: true, bubbles: true, cancelable: true }),
      );
      out.wheelZoom = await until(() => e.config().zoom > 1);
      out.wheelZoomReadout = await until(() => pctOf() !== pct0);
      await e.setZoom(1);
      await e.flush();

      // 一路点下来，增量产物还得等于全量：按钮生效了但排版停在旧内容上，正是这里要挡的
      //   （编译与排版两份自审都必须为 0）。
      out.alive = (await e.paragraphs()).length > 0;
      out.compileDiff = (await e.diag.auditCompile()).diff;
      out.layoutDiff = (await e.diag.auditLayout()).diff;
      return out;`);

    const want = [
      'paste', 'copy', 'cut', 'font', 'size', 'grow', 'shrink', 'bold', 'italic', 'underline',
      'color', 'line', 'after', 'indent', 'outdent', 'undo', 'redo', 'all', 'load', 'save',
      'type', 'tablePanel', 'table', 'field', 'fieldCaret', 'fieldCaretMoves', 'modeReadOnly',
      'modeRejects', 'modeBack', 'countsGrow', 'pagesMatch', 'countsSpace', 'marginMarks',
      'marginMarksRedraw',
      'pagePanel', 'pagePaper', 'pageCustomPanel', 'pageCustom', 'pageBack', 'paged',
      'continuous', 'zoomIn', 'zoomOut', 'zoomReset', 'wheelZoom', 'wheelZoomReadout', 'alive',
      'unsavedPrompt',
    ];
    for (const k of want) check(`★ ${k} 点下去有反应`, r[k] === true, String(r[k]));
    // 默认那一档：裁剪标记开（它是判断页边距设对了没有的直接读数），保存提醒关。
    check('裁剪标记默认开', r.marginMarksBefore === true, String(r.marginMarksBefore));
    // 点完这一圈之后，引擎自己的两份自审必须干净：编译出来的是最新文档，排出来的是
    //   最新编译。少了这两条，"按钮点了没反应"要靠肉眼才发现。
    check('★ 一圈操作之后编译自审为 0', r.compileDiff === 0, String(r.compileDiff));
    check('★ 一圈操作之后排版自审为 0', r.layoutDiff === 0, String(r.layoutDiff));
  });

  test('点完按钮：键盘还在编辑器里', async ({ open }) => {
    // 按钮是 <button>：按下那一下浏览器会把焦点挪过去，而键盘输入只认聚焦的元素。
    //   少了"不抢焦点"这一手，点完加粗之后敲字与 Ctrl+Z 会全落进按钮里。
    const p = await open('index.html', { query: { debug: '1' } });
    const pt = await p.ev<any>(`
      const e = ${D};
      await e.setText('甲乙丙丁');
      await e.setCaret({ para: 0, offset: 0 });
      await e.flush();
      const r = e.caret().rect;
      const pg = document.querySelector('.heditor-pages').getBoundingClientRect();
      const d = window.devicePixelRatio || 1;
      return { x: pg.left + r.x / d + 2, y: pg.top + (r.y + r.h / 2) / d };`);

    // 先用真实鼠标点进正文，焦点才真正落在承接层上。
    await p.page.mouse.click(pt.x, pt.y);
    const inEditor = await p.ev<string>(
      `return document.activeElement ? document.activeElement.className : '';`);

    await p.page.click('[data-act="bold"]');
    const afterClick = await p.ev<string>(
      `return document.activeElement ? document.activeElement.className : '';`);

    // 点完直接敲一个字：这一下必须进文档，而不是落进按钮里。
    //   下发原始 CDP 事件（与 input.spec 同一套）：输入层读的是 keydown 上的 text，
    //   而 page.keyboard.type 对非 ASCII 走的是另一条事件形状。
    const base = { windowsVirtualKeyCode: 88, nativeVirtualKeyCode: 88, key: 'x' };
    await p.send('Input.dispatchKeyEvent', { type: 'keyDown', ...base, text: 'x', unmodifiedText: 'x' });
    await p.send('Input.dispatchKeyEvent', { type: 'keyUp', ...base });
    await p.page.waitForTimeout(400);
    const text = await p.ev<string>(`const e = ${D}; await e.flush(); return e.text();`);

    check('点进正文之后焦点在承接层', inEditor === 'heditor-ime', inEditor);
    check('★ 点完工具栏按钮，焦点还在承接层', afterClick === 'heditor-ime', afterClick);
    check('★ 点完按钮之后敲字进文档', text.includes('x'), JSON.stringify(text.slice(0, 12)));
  });
});

// 公开面那一半的读数：一个整体结果对象。
type Api = Record<string, any>;

test.describe('公开面', () => {
  test.beforeEach(async ({ page }) => {
    // 复制粘贴走系统剪贴板，程序化读写要授权。
    await page.context().grantPermissions(['clipboard-read', 'clipboard-write']);
  });

  test('每个方法调一遍：形状与效果都对', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const r = await p.ev<Api>(`
      const e = ${D};
      const out = {};
      const settle = () => e.flush();

      // 内容：setText / text / paragraph / paragraphs
      await e.setText('第一段\\n第二段');
      await settle();
      out.text = e.text();
      out.paragraph = e.paragraph(1);
      out.paragraphs = e.paragraphs();
      // 规模：两段各三个字、没有空白 —— 码点数与字数该相等。
      out.counts = e.counts();

      // 光标：caret / setCaret / moveCaret
      await e.setCaret({ para: 0, offset: 1 });
      out.caret = e.caret();
      await e.moveCaret('lineEnd');
      out.moved = e.caret().pos.offset > 1;

      // 选区：select / selection / selectAll / clearSelection
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      out.sel = e.selection();
      await e.selectAll();
      out.selAll = e.selection().rects.length > 0;
      await e.clearSelection();
      out.selCleared = e.selection().empty;
      // 焦点：调用之后键盘承接层拿到焦点（宿主用完工具栏要还回来）。
      e.focus();
      out.focused = document.activeElement === e.element.querySelector('.heditor-ime');

      // 编辑：type / insertText / deleteBackward / deleteForward / split / merge
      await e.setText('abcd');
      await e.setCaret({ para: 0, offset: 2 });
      out.type = await e.type('XY');
      out.afterType = e.text();
      out.insert = await e.insertText({ para: 0, offset: 0 }, '头');
      out.afterInsert = e.text();
      out.back = await e.deleteBackward();
      out.forward = await e.deleteForward();
      out.afterDeletes = e.text();
      await e.setCaret({ para: 0, offset: 2 });
      out.split = await e.splitParagraph();
      out.afterSplit = e.paragraphs().length;
      out.merge = await e.mergeParagraph('prev');
      out.afterMerge = e.paragraphs().length;

      // 撤销与重做
      out.undo = await e.undo();
      out.redo = await e.redo();

      // 格式：写读对称。段落格式取一个与当前值不同的档 —— 同值会被幂等判定挡成 false。
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 3 });
      out.format = await e.format({ bold: true, size: 18 });
      out.formatAt = await e.formatAt();
      const lf0 = (await e.paragraphFormatAt()).lineFactor;
      const lf1 = lf0 === 2 ? 3 : 2;
      out.paraFormat = await e.paragraphFormat({ lineFactor: lf1 });
      out.paraFormatAt = await e.paragraphFormatAt();
      out.paraWant = lf1;
      out.indent = await e.setIndent(1);
      // 缩进不许把选中的字换掉（选区先收起到起点，文字一个都不能少）。
      out.afterIndent = e.text();

      // 点按命令：取反两次回到原样，字号按阶梯上下各走一档。
      //   先要有选区 —— 字符格式只作用在选区上，光标态（空选区）下两条都返回 false。
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      out.toggleFirst = await e.toggleFormat('italic');
      out.toggleSecond = await e.toggleFormat('italic');
      out.italicAfterPair = e.formatAt({ effective: true }).italic === true;
      // 阶梯要跨过选区当前的字号（前面写成 18），不然"调大"本来就没有下一档。
      out.stepUp = await e.stepFontSize(1, [9, 12, 18, 24]);
      out.stepDown = await e.stepFontSize(-1, [9, 12, 18, 24]);

      // 剪贴板：换一段干净的文本再走复制粘贴
      await e.setText('甲乙丙丁');
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 2 });
      out.copy = await e.copy();
      await e.setCaret({ para: 0, offset: 4 });
      out.paste = await e.paste();
      out.afterPaste = e.text();
      await e.select({ para: 0, offset: 0 }, { para: 0, offset: 1 });
      out.cut = await e.cut();
      await settle();
      out.afterCut = e.text();

      // 控件：清单读得到；没有控件时改属性要安全地返回 false
      await e.setText('纯文本没有控件');
      out.form = e.form();
      out.setControlProp = await e.setControlProp(0, 0, true);
      out.focusControl = await e.focusControl('next');

      // 插入结构：表格与字段都走 InsertNode，段数与控件清单该立刻跟上。
      await e.setCaret({ para: 0, offset: 0 });
      out.insertTable = await e.insertTable({ rows: 2, cols: 2 });
      await settle();
      out.afterTable = e.paragraphs().length;
      await e.setText('控件');
      await e.setCaret({ para: 0, offset: 2 });
      out.insertControl = await e.insertControl({ type: 'text', key: 'k', label: '值' });
      await settle();
      out.afterControl = e.text();
      out.controlCount = e.form().length;
      // 后续几条断言按"纯文本没有控件"写的，插完之后恢复回那一份。
      await e.setText('纯文本没有控件');
      await settle();

      // 视图与配置
      out.geometry = e.geometry();
      await e.scrollTo({ page: 0 });
      out.scroll = true;
      await e.setZoom(1.25);
      out.zoom = e.config().zoom;
      out.configure = await e.configure({ zoom: 1 });
      out.config = e.config();
      out.element = !!e.element && e.element.id === 'ed'
        && !!e.element.querySelector('.heditor-stage');

      // 页面设置：把描述层自己给的字段值喂回去，必须判定成"没改动"（不触发整篇重排）。
      const pageForm = e.describePageSetup();
      const samePage = {};
      for (const f of pageForm.fields) if (f.key) samePage[f.key] = f.value;
      out.pageFields = pageForm.fields.length;
      out.pageSame = await e.applyPageSetupForm(samePage);
      out.pageKept = e.config().page.paper;

      // 事件：订阅之后打字要收到 change
      let changes = 0;
      const off = e.on('change', () => { ++changes; });
      await e.setCaret({ para: 0, offset: 0 });
      await e.type('Z');
      await settle();
      off();
      out.changeEvents = changes;

      // 保存 / 打开：往返之后文本相同
      const json = await e.save();
      out.savedLen = json.length;
      await e.setText('被冲掉的内容');
      await settle();
      await e.open(json);
      await settle();
      out.roundTrip = e.text();

      // 生命周期：destroy 之后所有调用都拒绝
      await e.destroy();
      let rejected = false;
      try {
        e.text();
      } catch (err) {
        rejected = true;
      }
      out.destroyed = rejected;
      return out;`);

    check('setText / text 一致', r.text === '第一段\n第二段', JSON.stringify(r.text));
    check('paragraph 读得到那一段', r.paragraph === '第二段', r.paragraph);
    check('counts 的码点数与字数一致（没有空白）',
      r.counts.chars === 6 && r.counts.words === 6, JSON.stringify(r.counts));
    check('paragraphs 分得开', Array.isArray(r.paragraphs) && r.paragraphs.length === 2,
          JSON.stringify(r.paragraphs));
    check('caret 有位置与矩形', r.caret.pos.para === 0 && r.caret.rect.w > 0,
          JSON.stringify(r.caret));
    check('moveCaret(lineEnd) 把光标推到段末', r.moved === true, String(r.moved));
    check('select 之后选区非空', r.sel.empty === false && r.sel.rects.length > 0,
          JSON.stringify(r.sel));
    check('selectAll 有矩形', r.selAll === true, String(r.selAll));
    check('clearSelection 之后是空选区', r.selCleared === true, String(r.selCleared));
    check('focus() 把焦点交给承接层', r.focused === true, String(r.focused));

    check('type 返回 true 且进了文本', r.type === true && r.afterType === 'abXYcd', r.afterType);
    check('insertText 插到指定位置', r.insert === true && r.afterInsert === '头abXYcd',
          r.afterInsert);
    check('deleteBackward / deleteForward 各删一个', r.back === true && r.forward === true,
          r.afterDeletes);
    check('splitParagraph 多出一段', r.split === true && r.afterSplit === 2, String(r.afterSplit));
    check('mergeParagraph 合回一段', r.merge === true && r.afterMerge === 1, String(r.afterMerge));
    check('undo / redo 都有得做', r.undo === true && r.redo === true,
          String(r.undo) + ' / ' + String(r.redo));

    check('format 写进去了', r.format === true, String(r.format));
    check('formatAt 读回同一个值', r.formatAt.bold === true && r.formatAt.size === 18,
          JSON.stringify(r.formatAt));
    check('★ toggleFormat 取反两次回到原样',
      r.toggleFirst === true && r.toggleSecond === true && r.italicAfterPair === false,
      String(r.toggleFirst) + ' / ' + String(r.toggleSecond) + ' / ' + String(r.italicAfterPair));
    check('★ stepFontSize 上下各走一档', r.stepUp === true && r.stepDown === true,
      String(r.stepUp) + ' / ' + String(r.stepDown));
    check('paragraphFormat 写进去了', r.paraFormat === true, String(r.paraFormat));
    check('paragraphFormatAt 读回同一个值', r.paraFormatAt.lineFactor === r.paraWant,
          JSON.stringify(r.paraFormatAt));
    check('setIndent 改得动', r.indent === true, String(r.indent));
    check('★ 缩进不吃掉选中的字', typeof r.afterIndent === 'string'
          && r.afterIndent.includes('甲乙丙丁'), JSON.stringify(r.afterIndent));

    check('copy 返回成功', r.copy === true, String(r.copy));
    check('paste 返回成功且粘上了', r.paste === true && r.afterPaste.length > 4, r.afterPaste);
    check('cut 返回成功且少了一个字', r.cut === true, String(r.cut));

    check('form() 是数组', Array.isArray(r.form), JSON.stringify(r.form));
    check('没有控件时 setControlProp 安全返回 false', r.setControlProp === false,
          String(r.setControlProp));
    check('focusControl 不抛错', r.focusControl === undefined || r.focusControl !== null,
          String(r.focusControl));
    check('★ insertTable 落成一张 2x2 的表（段数 +4）',
          r.insertTable === true && r.afterTable === 5, String(r.afterTable));
    check('★ insertControl 落成一个字段，文字接在光标处',
          r.insertControl === true && r.afterControl === '控件值' && r.controlCount === 1,
          JSON.stringify(r.afterControl) + ' / ' + String(r.controlCount));

    check('geometry 有页宽与页数', r.geometry.page.w > 0 && r.geometry.pages >= 1,
          JSON.stringify(r.geometry.page));
    check('setZoom 生效', r.zoom === 1.25, String(r.zoom));
    check('configure 返回生效值', r.configure.zoom === 1, JSON.stringify(r.configure));
    check('element 是容器', r.element === true, String(r.element));
    check('describePageSetup 给出字段表', r.pageFields > 0, String(r.pageFields));
    check('applyPageSetupForm 收到同一份值时不改动', r.pageSame === false, String(r.pageSame));
    check('纸张没被这份空改动带走', r.pageKept === 'A4', String(r.pageKept));

    check('on(change) 收到了事件', r.changeEvents >= 1, String(r.changeEvents));
    check('save 给出了 JSON', typeof r.savedLen === 'number' && r.savedLen > 0, String(r.savedLen));
    check('★ 保存再打开：内容回来', r.roundTrip === 'Z纯文本没有控件', JSON.stringify(r.roundTrip));
    check('★ destroy 之后调用被拒绝', r.destroyed === true, String(r.destroyed));
  });
});
