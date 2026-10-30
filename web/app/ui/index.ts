// 示例页的外壳：Word 风格的功能区、状态栏与右键菜单。
//
// 一件组件一个文件，这一份只做出口：
//   types        描述层（按钮、下拉框、组、页签、组件接口）
//   icons        图标（web/app/icons 下的 .svg 按原文读进来）
//   button       按钮
//   select       下拉的浮层与触发器（带搜索；choice 与属性面板共用）
//   slider       滑块（状态栏缩放用）
//   choice       下拉框
//   grid-picker  表格尺寸的网格选择器
//   page-setup   页面设置（纸张、方向、四边页边距）
//   menu         会开面板的按钮
//   dropdown     下拉菜单按钮（弹一列动作）
//   layer        浮层（定位与关闭时机，popover 与右键菜单共用）
//   popover      挂在按钮下面的弹层
//   context-menu 数据驱动的多级菜单（右键菜单）
//   props-panel  属性面板（字段描述 → 表单；字段表由库的 describe 系列给）
//   table-props  表格属性（表 / 行 / 格三层）
//   control-props 控件属性（按类型给字段，字段表在库里）
//   group        一排动作 + 组名
//   ribbon       页签条与面板
//   statusbar    底部状态栏
//   chrome       装配（摆位置、连回调、转发 refresh）
//
// 分出这一层不是为了"看起来像框架"：加一个按钮只该改动作表，不该动 DOM 或事件；
//   而动作表要能在没有浏览器的地方读（它同时是"这一页能做什么"的清单）。
export { mountChrome } from './chrome';
export { createContextMenu, type ContextMenu, type MenuEntry } from './context-menu';
export { createControlProps } from './control-props';
export { createDropdown } from './dropdown';
export { createGridPicker } from './grid-picker';
export { createLayer, type Layer } from './layer';
export { createPageSetup } from './page-setup';
export { createPropsPanel, type PropsPanel, type PropsPanelOptions } from './props-panel';
export { createSelect, selectOptions, type SelectOption } from './select';
export { createSlider } from './slider';
export { createTableProps } from './table-props';
export type {
  Button,
  Choice,
  Chrome,
  Component,
  Dropdown,
  Group,
  Item,
  Menu,
  Stack,
  Tab,
} from './types';
