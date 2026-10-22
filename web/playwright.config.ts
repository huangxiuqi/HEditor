// 默认口径：Vite 直接跑 web/src 源码。改一行就能测，不打包页面与库。
//
// 仍然需要的产物只有两样：wasm（cmake 出的）与 Worker 壳（npm run build:worker，不到一秒）。
//   Worker 壳是经典 worker，源码形态跑不起来。验收基准（构建产物）见 playwright.dist.config.ts。
import { makeConfig } from './tests/support/config';

export default makeConfig('dev');
