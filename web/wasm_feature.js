// 运行环境能力探测：WebAssembly SIMD。
//
// 为什么要有它：产物是用 -msimd128 编的（见 scripts/build_skia_wasm.ps1 里 SIMD 那一段）。
//   二进制里只要有一条 v128 指令，整个模块就要求引擎支持 —— 不支持的浏览器在实例化
//   那一步抛 CompileError，而胶水脚本本身能正常加载（错误发生在它自己去取 wasm 时），
//   所以 script 标签的 onerror 根本不触发：报出来的话术与真实原因完全对不上。
//
// 探针是一段含 v128 的最小 wasm（133 字节）。它的字节必须真的含 SIMD 指令：
//   全常量的写法会被 -O3 折叠成 i32.const，那种探针在旧引擎上照样返回 true，
//   也就是一个恒绿的假探针。当前这份反汇编可见
//   v128.const / v128.load / v128.store / i32x4.add。
//
// 三张页（index / multi / worker）都在各自的主脚本之前加载它。
(function () {
  'use strict';

  // 来源：一次性源码经
  //   clang --target=wasm32 -msimd128 -O0 -c
  //   wasm-ld --no-entry --strip-all --export=heditorSimdProbe
  // 编出后逐字节抄进来。源码要点有两个，缺一个探针就废：
  //   其一，函数参数来自外部（写成全常量会被常量折叠掉）；
  //   其二，用 -O0（优化档会把 v128 运算拆成标量）。
  const SIMD_PROBE = new Uint8Array([
    0, 97, 115, 109, 1, 0, 0, 0, 1, 6, 1, 96, 1, 123, 1, 123,
    3, 2, 1, 0, 5, 3, 1, 0, 1, 6, 8, 1, 127, 1, 65, 128,
    128, 4, 11, 7, 29, 2, 6, 109, 101, 109, 111, 114, 121, 2, 0, 16,
    104, 101, 100, 105, 116, 111, 114, 83, 105, 109, 100, 80, 114, 111, 98, 101,
    0, 0, 10, 65, 1, 63, 1, 1, 127, 35, 128, 128, 128, 128, 0, 65,
    32, 107, 33, 1, 32, 1, 32, 0, 253, 11, 4, 16, 32, 1, 253, 12,
    5, 0, 0, 0, 6, 0, 0, 0, 7, 0, 0, 0, 8, 0, 0, 0,
    253, 11, 4, 0, 32, 1, 253, 0, 4, 16, 32, 1, 253, 0, 4, 0,
    253, 174, 1, 15, 11,
  ]);

  let cached = null;

  // 引擎是否支持 128 位 SIMD。结果记在闭包里，一张页只探测一次。
  //   validate 对语法不合法与"指令不被支持"都返回 false 且不抛，
  //   但 WebAssembly 整个缺失的老引擎会在读属性时就抛，所以整段包住。
  function simdSupported() {
    if (cached !== null) return cached;
    cached = false;
    try {
      cached = typeof WebAssembly === 'object' && WebAssembly !== null &&
               typeof WebAssembly.validate === 'function' &&
               WebAssembly.validate(SIMD_PROBE) === true;
    } catch (err) {
      cached = false;
    }
    return cached;
  }

  // 不支持就抛，带上能看懂的标题与下一步，交给宿主的启动包裹层去显示。
  //   报错面是宿主建的，探测层不依赖它存在 —— 这样这一层还能被单测或探针单独调。
  function assertSimd() {
    if (simdSupported()) return;
    const err = new Error(
      '探测到 WebAssembly.validate() 对一段含 128 位 SIMD 指令的最小模块返回 false。');
    err.heditorTitle = '这台浏览器的 WebAssembly 不支持 SIMD';
    err.heditorHint = '产物是用 -msimd128 编的，需要支持 WebAssembly SIMD 的浏览器' +
                      '（2022 年起主流浏览器均支持；Safari 需 16.4 及以上）。请升级浏览器。' +
                      '要支持旧环境需另出一份不带 SIMD 的标量产物，目前尚未构建。';
    throw err;
  }

  window.heditorWasmFeature = {
    simdSupported,
    assertSimd,
    // 探针字节也给出来：判据可以拿它自己跑一次 validate，不必只信上面那个结论。
    simdProbe: SIMD_PROBE,
  };
})();
