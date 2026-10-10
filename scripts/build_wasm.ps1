# 构建 WASM 目标（Emscripten）。
#
# 产物：build/wasm/web/ced_editor.js + ced_editor.wasm
#
# 本机没有把 emsdk 装进 PATH，脚本自己拼环境：
#   EM_CONFIG 指向 .toolchain/emsdk/.emscripten
#   （.emscripten 里用 $CFGDIR 指到 emsdk 自带的 node / python / LLVM）
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1 -Configure   # 强制重跑 cmake 配置
#   powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1 -NoSkia      # 用自写光栅后端（对照用）
#
# 默认走 Skia 后端（CED_USE_SKIA=ON）—— 光栅化的活交给 Skia，见 src/core/render/bitmap.h。
# Skia 的 wasm 静态库要用 scripts\build_skia_wasm.ps1 先构建出来。
param(
    [string]$BuildType = 'Release',
    [switch]$Configure,
    [switch]$NoSkia
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Emsdk = Join-Path $Root '.toolchain\emsdk'
$EmscriptenDir = Join-Path $Emsdk 'upstream\emscripten'
$Ninja = Join-Path $Root '.toolchain\ninja\ninja.exe'
$BuildDir = Join-Path $Root 'build\wasm'
$SkiaWasOn = -not $NoSkia
$SkiaFlag = if ($NoSkia) { '-DCED_USE_SKIA=OFF' } else { '-DCED_USE_SKIA=ON' }

foreach ($p in @($EmscriptenDir, $Ninja)) {
    if (-not (Test-Path $p)) { throw "缺少工具链：$p" }
}

# 切换后端是**编译期**的事：源码列表不同（bitmap.cpp vs bitmap_skia.cpp），
# 光靠默认的"cache 存在就不重配"会静默沿用上一次的后端。所以这里读一眼 cache，
# 值不一致就强制重配。
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    $cached = (Select-String -Path $cacheFile -Pattern '^CED_USE_SKIA:BOOL=(ON|OFF)$' |
               Select-Object -First 1).Line
    $want = if ($SkiaWasOn) { 'CED_USE_SKIA:BOOL=ON' } else { 'CED_USE_SKIA:BOOL=OFF' }
    if ($cached -and $cached -ne $want) {
        Write-Host "[wasm] 后端开关变了（$cached → $want），强制重新配置"
        $Configure = $true
    }
}

# emsdk 里的 node 目录名带版本号（如 node\24.19.0_64bit），用通配符找
$NodeDir = Get-ChildItem -Path (Join-Path $Emsdk 'node') -Directory | Select-Object -First 1
if (-not $NodeDir) { throw "emsdk 里找不到 node 目录" }

$env:EM_CONFIG = Join-Path $Emsdk '.emscripten'
$env:PATH = "$EmscriptenDir;$Emsdk;$($NodeDir.FullName);$env:PATH"

# ── 配置 ────────────────────────────────────────────────────
if ($Configure -or -not (Test-Path $cacheFile)) {
    Write-Host "[wasm] 配置 emcmake + Ninja ($BuildType, Skia=$SkiaWasOn)"
    & (Join-Path $EmscriptenDir 'emcmake.exe') cmake `
        -S $Root -B $BuildDir -G Ninja `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        "-DCMAKE_MAKE_PROGRAM=$Ninja" `
        $SkiaFlag
    if ($LASTEXITCODE -ne 0) { throw "emcmake 配置失败：exit $LASTEXITCODE" }
}

# ── 构建 ────────────────────────────────────────────────────
Write-Host "[wasm] 构建 ced_wasm"
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "构建失败：exit $LASTEXITCODE" }

# ── 报告产物 ────────────────────────────────────────────────
Write-Host ""
Write-Host "[wasm] 产物："
$total = 0
foreach ($n in @('ced_editor.js', 'ced_editor.wasm')) {
    $f = Join-Path $BuildDir "web\$n"
    if (Test-Path $f) {
        $len = (Get-Item $f).Length
        $total += $len
        Write-Host ("  {0,-18} {1,10:N1} KB" -f $n, ($len / 1KB))
    } else {
        Write-Host "  缺失: $f" -ForegroundColor Red
    }
}
Write-Host ("  {0,-18} {1,10:N1} KB" -f '合计', ($total / 1KB))
Write-Host ""
Write-Host "起服务： powershell -ExecutionPolicy Bypass -File scripts\serve_web.ps1"
