// 页签条与面板：一条页签 + 若干面板，同一时刻只露一张。
//
// 面板都在 DOM 里（不是切一次就重建）：切页签只改一个修饰类，回显与焦点因此不会丢。
//   代价是所有面板都要在 update 里刷一遍 —— 一页最多几十个控件，换的是"切页签时
//   不会闪一下"。
import { createGroup } from './group';
import type { Component, Group, Tab } from './types';

/**
 * 造功能区。
 * @param tabs 页签表
 * @returns 组件（update 转发给所有面板里的所有组）
 */
export function createRibbon(tabs: Tab[]): Component {
  const el = document.createElement('div');

  const head = document.createElement('div');
  head.className = 'heditor-ribbon__head';
  const tabBar = document.createElement('div');
  tabBar.className = 'heditor-ribbon__tabs';
  head.appendChild(tabBar);
  el.appendChild(head);

  const body = document.createElement('div');
  body.className = 'heditor-ribbon__body';
  el.appendChild(body);

  const tabBtns: HTMLElement[] = [];
  const panes: HTMLElement[] = [];
  const groups: Component[] = [];

  const show = (index: number): void => {
    tabBtns.forEach((b, i) => b.classList.toggle('heditor-ribbon__tab--active', i === index));
    panes.forEach((p, i) => p.classList.toggle('heditor-ribbon__pane--active', i === index));
  };

  tabs.forEach((tab, i) => {
    const btn = document.createElement('button');
    btn.className = 'heditor-ribbon__tab';
    btn.dataset.tab = String(i);
    btn.textContent = tab.label;
    btn.addEventListener('click', () => show(i));
    tabBar.appendChild(btn);
    tabBtns.push(btn);

    const pane = document.createElement('div');
    pane.className = 'heditor-ribbon__pane';
    pane.dataset.pane = String(i);
    renderGroups(tab.groups, pane, groups);
    body.appendChild(pane);
    panes.push(pane);
  });

  if (tabs.length) show(0);

  return {
    el,
    /**
     * 重读所有组里每一项的回显。
     */
    update(): void {
      for (const g of groups) g.update();
    },
  };
}

/**
 * 把一页的组摆进面板，组与组之间放一道竖线。
 * @param list 这一页的组
 * @param pane 面板元素
 * @param sink 收集子组件（update 要转发给它们）
 */
function renderGroups(list: Group[], pane: HTMLElement, sink: Component[]): void {
  list.forEach((g, i) => {
    if (i > 0) {
      const sep = document.createElement('div');
      sep.className = 'heditor-ribbon__sep';
      pane.appendChild(sep);
    }
    const made = createGroup(g);
    sink.push(made);
    pane.appendChild(made.el);
  });
}
