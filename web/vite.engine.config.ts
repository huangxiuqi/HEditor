import { defineConfig } from 'vite';

/**
 * 把 `web/src/engine/api.ts` 打成一份**经典脚本**（IIFE）：`web/engine_api.js`。
 *
 * 为什么还要这一份产物：
 *   · 旧页面（index.html / multi.html）用 `<script src="engine_api.js">` 装它；
 *   · Worker 壳（engine_worker.js）用 `importScripts('engine_api.js')` 装它 ——
 *     经典 worker 没有 ESM 装载这条路。
 *   这两处都靠全局 `self.__heditorEngineApi`，而模块作用域里那一行赋值在 IIFE 里照样跑。
 *
 * 于是 `web/engine_api.js` 从"源码"变成了"构建产物"：真源只有 src/engine/api.ts 一份，
 *   两份命令表分叉这件事就不可能再发生。产物不压缩 —— 它要能被 diff 与 review。
 */
export default defineConfig({
  build: {
    outDir: '.',
    emptyOutDir: false,
    target: 'es2020',
    minify: false,
    rollupOptions: {
      output: {
        banner:
          '// 由 web/src/engine/api.ts 构建生成（vite.engine.config.ts），勿手改。\n' +
          '// 这一份是给经典脚本与经典 Worker（importScripts）用的：它把 __heditorEngineApi\n' +
          '//   挂在全局上，命令表与引擎语义的真源始终是 src/engine/api.ts 那一份。',
      },
    },
    lib: {
      entry: 'src/engine/api.ts',
      name: '__heditorEngineApiExports',
      formats: ['iife'],
      fileName: () => 'engine_api.js',
    },
  },
});
