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
    const form = await p.read<any>(`return (await H.form()).map((c) => ({ label: c.label }));`);
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
