// 示例页的外壳：Word 风格的功能区与状态栏。
//
// 一件组件一个文件，这一份只做出口：
//   types      描述层（按钮、下拉框、组、页签、组件接口）
//   icons      图标（web/app/icons 下的 .svg 按原文读进来）
//   button     按钮
//   choice     下拉框
//   grid-picker 表格尺寸的网格选择器
//   menu       会开面板的按钮
//   popover    弹层（定位与关闭时机）
//   group      一排动作 + 组名
//   ribbon     页签条与面板
//   statusbar  底部状态栏
//   chrome     装配（摆位置、连回调、转发 refresh）
//
// 分出这一层不是为了"看起来像框架"：加一个按钮只该改动作表，不该动 DOM 或事件；
//   而动作表要能在没有浏览器的地方读（它同时是"这一页能做什么"的清单）。
export { mountChrome } from './chrome';
export { createGridPicker } from './grid-picker';
export type { Button, Choice, Chrome, Component, Group, Item, Menu, Stack, Tab } from './types';
