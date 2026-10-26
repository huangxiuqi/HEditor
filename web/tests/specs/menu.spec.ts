// 右键菜单判据：三份菜单怎么选、动作真的落到文档上、属性面板一进一出、键盘与关闭时机。
//
// 为什么单独一份：这一层的失败形态全是"菜单弹出来了但按下去没反应"—— 落点判错时弹的是
//   正文那份菜单，禁用态算错时"删最后一行"照样能点（引擎会拒，但界面上只是"点了没反应"），
//   面板取值没回填时改一次再打开还是旧值。这三种都不会抛异常，只能按"弹了什么、点了之后
//   文档变没变"来判。
import { check } from '../support/check';
import { expect, test } from '../support/probe';

test.describe('右键菜单', () => {
  test('表格：格内右键认出表格，只剩一行时删行置灰，插行动作真的落到文档', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙');
      await H.flush();
      await H.setCaret({ para: 0, offset: 2 });
      await H.insertTable({ rows: 1, cols: 2 });
      await H.flush();
      // 段号稳定：0 是正文，1、2 是这一行的两格。
      await H.setCaret({ para: 1, offset: 0 });
      await H.flush();
      await D.stopBlink();
      const c = H.caret();
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               x: c.rect.x, y: c.rect.y, h: c.rect.h };
    `);
    // 落在格里（光标之后几个像素，避开格左边那条"选整格"的边距带）。
    const px = geo.left + (geo.x + 8) * geo.sx;
    const py = geo.top + (geo.y + geo.h / 2) * geo.sy;
    await p.page.mouse.click(px, py, { button: 'right' });

    const menu = p.page.locator('.heditor-menu');
    await expect(menu).toBeVisible();
    await expect(menu.locator('.heditor-menu__heading').first()).toHaveText('表格 1 × 2');

    const delRow = menu.locator('.heditor-menu__item', { hasText: '删除本行' });
    check('★ 只剩一行时「删除本行」是灰的',
          (await delRow.getAttribute('aria-disabled')) === 'true',
          String(await delRow.getAttribute('aria-disabled')));
    check('两列的表「删除本列」点得动',
          (await menu.locator('.heditor-menu__item', { hasText: '删除本列' })
            .getAttribute('aria-disabled')) !== 'true', 'disabled');
    check('没有整格选区时「合并单元格」是灰的',
          (await menu.locator('.heditor-menu__item', { hasText: '合并单元格' })
            .getAttribute('aria-disabled')) === 'true', 'not disabled');

    await menu.locator('.heditor-menu__item', { hasText: '在下方插入行' }).click();
    const rows = await p.read<number>(`return (await H.tableState()).rows;`);
    check('★ 菜单里的「插入行」真的插了一行', rows === 2, String(rows));
    check('点完动作菜单就收起来了', !(await menu.isVisible()), '菜单还开着');

    // 撤销要能一次回到插入之前（一个动作 = 一步撤销）。
    const back = await p.read<number>(`await H.undo(); await H.flush();
      return (await H.tableState()).rows;`);
    check('★ 一步撤销回到一行', back === 1, String(back));
  });

  test('控件：右键控件出控件菜单，属性面板回填现状并改得动标签', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙');
      await H.flush();
      await H.setCaret({ para: 0, offset: 1 });
      await H.insertControl({ type: 'checkBox', key: 'c1', label: '同意' });
      await H.flush();
      await D.stopBlink();
      const c = (await D.controls()).items[0];
      const rect = c.rects[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      // 控件矩形是磅；sx 是"CSS 像素 / 设备像素"，所以还要乘 ZOOM() 才到客户端坐标。
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               Z: ZOOM(), x: rect.x, y: rect.y, w: rect.w, h: rect.h };
    `);
    const px = geo.left + (geo.x + geo.w / 2) * geo.Z * geo.sx;
    const py = geo.top + (geo.y + geo.h / 2) * geo.Z * geo.sy;
    await p.page.mouse.click(px, py, { button: 'right' });

    const menu = p.page.locator('.heditor-menu');
    await expect(menu).toBeVisible();
    await expect(menu.locator('.heditor-menu__heading').first()).toHaveText('控件 checkBox');
    // 单控件的勾选态是菜单里的一项（勾选 / 取消勾选），与属性面板分工不同。
    await expect(menu.locator('.heditor-menu__item', { hasText: '勾选' }).first()).toBeVisible();

    await menu.locator('.heditor-menu__item', { hasText: '属性…' }).click();
    const label = p.page.locator('[data-act="prop-label"]');
    await expect(label).toBeVisible();
    check('★ 属性面板回填了现有标签', (await label.inputValue()) === '同意',
          await label.inputValue());
    check('只读开关在面板里（默认关）',
          await p.page.locator('[data-act="prop-readonly"]').isChecked() === false, 'checked');

    await label.fill('同意条款');
    await p.page.locator('[data-act="prop-apply"]').click();
    // 提交是异步下发的：紧接着读会读到提交之前的那一份，等它落到文档上再断言。
    await expect
      .poll(async () => {
        const now = await p.read<any[]>(
          `return (await H.form()).map((c) => ({ label: c.label }));`);
        return now[0]?.label;
      })
      .toBe('同意条款');
    const form = await p.read<any[]>(`return (await H.form()).map((c) => ({ label: c.label }));`);
    check('★ 面板改了标签并落到文档', form[0]?.label === '同意条款', JSON.stringify(form));
    check('面板按完就收起来了', (await p.page.locator('.heditor-props').count()) === 0,
          'panel still there');
  });

  test('键盘与关闭时机：方向键走项、Esc 关、点外面关', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙');
      await H.setCaret({ para: 0, offset: 1 });
      await H.flush();
      await D.stopBlink();
      const c = H.caret();
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, sx: r.width / cv.width, sy: r.height / cv.height,
               x: c.rect.x, y: c.rect.y, h: c.rect.h };
    `);
    const px = geo.left + (geo.x + 2) * geo.sx;
    const py = geo.top + (geo.y + geo.h / 2) * geo.sy;
    const menu = p.page.locator('.heditor-menu');

    await p.page.mouse.click(px, py, { button: 'right' });
    await expect(menu).toBeVisible();
    const first = await p.page.locator('.heditor-menu__item--active').first().textContent();
    check('★ 打开时第一项就高亮（键盘可以直接走）', !!first, String(first));
    await p.page.keyboard.press('ArrowDown');
    const second = await p.page.locator('.heditor-menu__item--active').first().textContent();
    check('★ 方向键换了一项', second !== first, String(first) + ' → ' + String(second));

    await p.page.keyboard.press('Escape');
    await expect(menu).toBeHidden();
    check('Esc 关掉菜单', !(await menu.isVisible()), '还开着');

    await p.page.mouse.click(px, py, { button: 'right' });
    await expect(menu).toBeVisible();
    await p.page.mouse.click(4, 4);
    await expect(menu).toBeHidden();
    check('点菜单外面关掉', !(await menu.isVisible()), '还开着');
  });
});

test.describe('菜单的分组与多级位置', () => {
  test('空白处右键：第一项不是那条空分割线', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    await p.page.mouse.click(600, 500, { button: 'right' });
    const menu = p.page.locator('.heditor-menu');
    await expect(menu).toBeVisible();
    const firstClass = await p.page.evaluate(() => {
      const list = document.querySelector('.heditor-menu > .heditor-menu__list');
      const first = list ? list.firstElementChild : null;
      return first ? first.className : '';
    });
    check('★ 顶上没有多余的分割线', !firstClass.includes('heditor-menu__sep'), firstClass);
  });

  test('二级贴着触发它的那一行（一级很长时也不跑到菜单顶部）', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    // 右键落在页面上部：一级有十七项，"插入"在最后一行，离菜单顶很远。
    await p.page.mouse.click(600, 220, { button: 'right' });
    await expect(p.page.locator('.heditor-menu')).toBeVisible();

    const sub = p.page.locator('.heditor-menu__item', { hasText: '插入' }).first();
    await sub.hover();
    await p.page.waitForTimeout(350);

    const box = await p.page.evaluate(() => {
      const row = Array.from(document.querySelectorAll('.heditor-menu__item'))
        .find((el) => (el.textContent || '').includes('插入'));
      const list = row && row.querySelector('.heditor-menu__list--sub');
      const root = document.querySelector('.heditor-menu');
      if (!row || !list || !root) return null;
      const rr = row.getBoundingClientRect();
      const lr = list.getBoundingClientRect();
      return { rowTop: rr.top, listTop: lr.top, listBottom: lr.bottom,
               menuTop: root.getBoundingClientRect().top,
               vh: window.innerHeight };
    });
    check('二级开出来了', !!box, String(box));
    check('★ 二级贴着触发它的那一行（差在十来个像素内）',
          !!box && Math.abs(box.listTop - box.rowTop) <= 12, JSON.stringify(box));
    check('★ 二级没有跑到一级顶部（两者相差一整段菜单）',
          !!box && box.listTop - box.menuTop > 100, JSON.stringify(box));

    // 移进二级：不该被收掉。
    const leaf = p.page.locator('.heditor-menu__list--sub:not([hidden]) .heditor-menu__item').first();
    await leaf.hover();
    await p.page.waitForTimeout(300);
    const opened = await p.page.locator('.heditor-menu__list--sub:not([hidden])').count();
    check('★ 鼠标移进二级，二级还在', opened === 1, String(opened));
  });

  test('二级比视口还高：整块夹进视口（或内部滚动），不留点不到的项', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' }, viewport: { width: 900, height: 420 } });
    await p.page.mouse.click(450, 330, { button: 'right' });
    await expect(p.page.locator('.heditor-menu')).toBeVisible();

    const sub = p.page.locator('.heditor-menu__item', { hasText: '插入' }).first();
    await sub.hover();
    await p.page.waitForTimeout(350);

    const box = await p.page.evaluate(() => {
      const row = Array.from(document.querySelectorAll('.heditor-menu__item'))
        .find((el) => (el.textContent || '').includes('插入'));
      const list = row && row.querySelector('.heditor-menu__list--sub');
      if (!row || !list) return null;
      const lr = list.getBoundingClientRect();
      return { top: lr.top, bottom: lr.bottom, vh: window.innerHeight,
               overflow: getComputedStyle(list).overflowY, h: lr.height };
    });
    check('二级开出来了', !!box, String(box));
    check('★ 二级的顶边没跑到视口外', !!box && box.top >= 0, JSON.stringify(box));
    check('★ 二级的下边没超出视口（最下面那项点得到）',
          !!box && box.bottom <= box.vh, JSON.stringify(box));
  });

  test('右键下拉控件：只出菜单，不叠一个值面板', async ({ open }) => {
    const p = await open('index.html', { query: { debug: '1' } });
    const geo = await p.read<any>(`
      await H.setText('甲乙丙');
      await H.setCaret({ para: 0, offset: 3 });
      await H.flush();
      await H.insertControl({ type: 'dropdownList', key: 'dd',
                              options: [{ value: 'a', label: '选项甲' }] });
      await H.flush();
      await D.stopBlink();
      const q = (await D.controls()).items[0].rects[0];
      const cv = FRAME();
      const r = cv.getBoundingClientRect();
      return { left: r.left, top: r.top, Z: ZOOM(), cx: q.x + q.w / 2, cy: q.y + q.h / 2 };`);
    await p.page.mouse.click(geo.left + geo.cx * geo.Z, geo.top + geo.cy * geo.Z,
                             { button: 'right' });
    await expect(p.page.locator('.heditor-menu')).toBeVisible();
    const panel = await p.page.locator('.heditor-popover--control').count();
    check('★ 右键不再弹值面板（不叠两块浮层）', panel === 0, String(panel));
  });
});
