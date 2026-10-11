# 构建 wasm 版 Skia（给 HEDITOR_USE_SKIA 的引擎链接用）。
#
# 产物：.toolchain/skia/src/skia-main/out/wasm/libskia.a
#
# ─────────────────────────────────────────────────────────────
#  这条链路为什么这么长 —— 四个坑，都记下来免得下次重踩
# ─────────────────────────────────────────────────────────────
#
# ① 外部依赖不在源码 tar 里
#    Skia 的 third_party/externals/ 是**空目录**：真正的外部依赖
#    （freetype / harfbuzz / zlib / libpng / libjpeg-turbo / libwebp）要按 DEPS
#    里钉住的 commit 单独取。上游用 tools/git-sync-deps 拉（内部就是 git clone），
#    但本工程全局禁止 git 写入命令，所以改成取 googlesource 的只读归档：
#        https://<host>/<repo>/+archive/<sha>.tar.gz
#    这个接口返回的就是该 commit 的整棵树，解压即用，且不创建 .git。
#
# ② gn 在 Windows 上把编译器硬编码成 emcc.bat，而 .bat 会被 cmd 二次解释
#    ninja 在 Windows 上执行 .bat 要经过 cmd.exe，而 .bat 里的 `%*` 展开后会
#    **再次解释重定向字符**。Skia 恰好给 freetype 传了
#        -DFT_CONFIG_MODULES_H=<freetype-no-type1/freetype/config/ftmodule.h>
#    这个尖括号被 cmd 当成"从哪个文件读输入"，于是 541 个编译步骤**全部**报
#    "系统找不到指定的路径。"（CP936 输出，看起来像乱码）。
#    解法：把工具链里的 cc/cxx/ar 从 .bat 换成 emsdk 提供的 .exe 启动器 ——
#    CreateProcess 直连，不经过 shell。见下面的 Patch-Toolchain。
#
# ③ //gn/toolchain/emsdk:activate 会再下一份 ~1GB 的 emsdk
#    wasm 工具链的 deps 里有这个 action，它跑的是 bin/activate-emsdk，内容是
#        emsdk.py install --permanent 4.0.7   +   activate --permanent 4.0.7
#    本工程已经把 emsdk 放在 .toolchain/emsdk，并用 gn 参数 skia_emsdk_dir
#    指过去（那个 emsdk 是 6.0.12，能用），不需要这一步。解法：去掉该 deps。
#
# ④ is_official_build = true 会让所有 third_party 去找"系统库"
#    third_party/*/BUILD.gn 里几乎每一条都是
#        skia_use_system_X = is_official_build && !is_canvaskit
#    交叉编译到 wasm 时**根本没有系统库**，于是 zlib.h / png.h / jpeglib.h
#    全部找不到。解法：在 args.gn 里把每条 skia_use_system_* 显式关掉，
#    改回从 third_party/externals 编源码（源码就是第 ① 步拉下来的）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\build_skia_wasm.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build_skia_wasm.ps1 -Fetch     # 只补依赖
#   powershell -ExecutionPolicy Bypass -File scripts\build_skia_wasm.ps1 -Clean     # 删掉 out/wasm 重建
param(
    [switch]$Fetch,
    [switch]$Clean,
    [int]$Jobs = 0
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $Root '.toolchain'
$SkiaPkg   = Join-Path $Toolchain 'skia'
$SkiaSrc   = Join-Path $SkiaPkg 'src\skia-main'
$Externals = Join-Path $SkiaSrc 'third_party\externals'
$Emsdk     = Join-Path $Toolchain 'emsdk'
$Gn        = Join-Path $Toolchain 'gn\out\gn.exe'
$Ninja     = Join-Path $Toolchain 'ninja\ninja.exe'
$OutDir    = Join-Path $SkiaSrc 'out\wasm'
$Lib       = Join-Path $OutDir 'libskia.wasm.a'

function Say([string]$m) { Write-Host "[skia] $m" }

# ── 0. 工具链体检 ───────────────────────────────────────────
foreach ($p in @($Emsdk, $Gn, $Ninja)) {
    if (-not (Test-Path $p)) { throw "缺少工具链：$p（gn / ninja / emsdk 需要先就位）" }
}

# ── 1. Skia 源码 ────────────────────────────────────────────
if (-not (Test-Path (Join-Path $SkiaSrc 'BUILD.gn'))) {
    $tar = Join-Path $SkiaPkg 'skia-main.tar.gz'
    if (-not (Test-Path $tar)) {
        Say "下载 Skia 源码（约 80 MB，走 codeload）"
        New-Item -ItemType Directory -Force -Path $SkiaPkg | Out-Null
        & curl.exe -sSL --max-time 3600 -o $tar 'https://codeload.github.com/google/skia/tar.gz/refs/heads/main'
        if ($LASTEXITCODE -ne 0) { throw "下载 Skia 源码失败" }
    }
    Say "解压 Skia 源码"
    New-Item -ItemType Directory -Force -Path (Join-Path $SkiaPkg 'src') | Out-Null
    & tar.exe -xzf $tar -C (Join-Path $SkiaPkg 'src')
    if ($LASTEXITCODE -ne 0) { throw "解压 Skia 源码失败" }
}

# ── 2. 外部依赖（DEPS 里钉住的 commit，只读归档，不碰 git）──
#    host 路径前不带斜杠：googlesource 的 +archive 要求这样。
$Deps = [ordered]@{
    'freetype'       = 'https://chromium.googlesource.com/chromium/src/third_party/freetype2/+archive/264b5fbf5b912b39f98d038bf75d39be0a73f21b.tar.gz'
    'harfbuzz'       = 'https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz/+archive/9cb1fee51069b206effb4736e443b038d230789d.tar.gz'
    'zlib'           = 'https://chromium.googlesource.com/chromium/src/third_party/zlib/+archive/646b7f569718921d7d4b5b8e22572ff6c76f2596.tar.gz'
    'libpng'         = 'https://skia.googlesource.com/third_party/libpng/+archive/d5515b5b8be3901aac04e5bd8bd5c89f287bcd33.tar.gz'
    'libjpeg-turbo'  = 'https://chromium.googlesource.com/chromium/deps/libjpeg_turbo/+archive/e14cbfaa85529d47f9f55b0f104a579c1061f9ad.tar.gz'
    'libwebp'        = 'https://chromium.googlesource.com/webm/libwebp/+archive/845d5476a866141ba35ac133f856fa62f0b7445f.tar.gz'
}
# 每个依赖判"是否已就位"的哨兵文件（目录存在但为空也会被骗，所以按文件判）
$Sentinels = @{
    'freetype'      = 'include\freetype\freetype.h'
    'harfbuzz'      = 'src\hb.h'
    'zlib'          = 'zlib.h'
    'libpng'        = 'png.h'
    'libjpeg-turbo' = 'src\jpeglib.h'
    'libwebp'       = 'src\webp\decode.h'
}

New-Item -ItemType Directory -Force -Path $Externals | Out-Null
$Tmp = Join-Path $env:TEMP 'heditor_skia_deps'
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

foreach ($name in $Deps.Keys) {
    $dir = Join-Path $Externals $name
    $sentinel = Join-Path $dir $Sentinels[$name]
    if (Test-Path $sentinel) { continue }
    $tar = Join-Path $Tmp "$name.tar.gz"
    Say "取依赖 $name"
    & curl.exe -sSL --max-time 1800 -o $tar $Deps[$name]
    if ($LASTEXITCODE -ne 0) { throw "下载 $name 失败" }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    & tar.exe -xzf $tar -C $dir
    if ($LASTEXITCODE -ne 0) { throw "解压 $name 失败" }
    if (-not (Test-Path (Join-Path $dir $Sentinels[$name]))) { throw "$name 解压后仍找不到哨兵文件" }
}
Say "外部依赖就位"

if ($Fetch) { Say "-Fetch：只补依赖，到此为止"; exit 0 }

# ── 3. emsdk 挂进树内（gn 的 wasm 工具链按固定相对路径找它）──
$EmsdkLink = Join-Path $Externals 'emsdk'
if (-not (Test-Path $EmsdkLink)) {
    Say "创建 emsdk junction → $Emsdk"
    New-Item -ItemType Junction -Path $EmsdkLink -Target $Emsdk | Out-Null
}

# ── 4. Patch Skia 的 gn 文件（幂等）─────────────────────────
function Patch-File([string]$path, [string]$old, [string]$new, [string]$tag) {
    $text = [System.IO.File]::ReadAllText($path)
    if ($text.Contains($new)) { Say "已打过补丁：$tag"; return }
    if (-not $text.Contains($old)) { throw "补丁点找不到（$tag）：$path — 上游可能改了结构" }
    [System.IO.File]::WriteAllText($path, $text.Replace($old, $new))
    Say "打补丁：$tag"
}

$ToolchainGn = Join-Path $SkiaSrc 'gn\toolchain\BUILD.gn'

Patch-File $ToolchainGn @'
    if (host_os == "win") {
      ar = "$skia_emsdk_dir/upstream/emscripten/emar.bat"
      cc = "$skia_emsdk_dir/upstream/emscripten/emcc.bat"
      cxx = "$skia_emsdk_dir/upstream/emscripten/em++.bat"
    } else {
'@ @'
    if (host_os == "win") {
      # 本工程改动（见 scripts/build_skia_wasm.ps1 顶部坑 ②）：
      # 用 emsdk 原生 .exe 启动器，不要上游的 .bat。
      # ninja 经 cmd.exe 跑 .bat 时 %* 会再次解释重定向字符，而 Skia 给 freetype 传的
      # -DFT_CONFIG_MODULES_H=<...> 里的尖括号会被当成输入重定向，编译全灭。
      ar = "$skia_emsdk_dir/upstream/emscripten/emar.exe"
      cc = "$skia_emsdk_dir/upstream/emscripten/emcc.exe"
      cxx = "$skia_emsdk_dir/upstream/emscripten/em++.exe"
    } else {
'@ '工具链改用 emcc.exe'

Patch-File $ToolchainGn @'
    link = cxx

    if (!is_canvaskit) {
      deps = [ "//gn/toolchain/emsdk:activate(//gn/toolchain:$host_toolchain)" ]
    }
  }
}
'@ @'
    link = cxx

    # 本工程改动（见 scripts/build_skia_wasm.ps1 顶部坑 ③）：
    # 去掉对 //gn/toolchain/emsdk:activate 的依赖 —— 它会跑
    # `emsdk.py install --permanent 4.0.7`，再下一份 ~1GB。
    # 本工程的 emsdk 由 .toolchain/emsdk 提供，经 gn 参数 skia_emsdk_dir 指过来。
  }
}
'@ '去掉 emsdk activate 依赖'

# ── 5. gn 参数 ──────────────────────────────────────────────
#   ★ 每一条 skia_use_system_* 都是必须的，理由见文件顶部坑 ④。
$ArgsGn = @'
# 由 scripts/build_skia_wasm.ps1 生成 —— 请改脚本，不要手改这里。
target_os = "wasm"
target_cpu = "wasm"
is_debug = false
is_official_build = true

# 外部依赖一律从源码编（is_official_build 默认会去找系统库，交叉编译时找不到）
skia_use_system_zlib = false
skia_use_system_libpng = false
skia_use_system_libjpeg_turbo = false
skia_use_system_libwebp = false
skia_use_system_harfbuzz = false
skia_use_system_freetype2 = false
skia_use_system_icu = false

# 文本：FreeType 后端 + HarfBuzz 整形
skia_use_freetype = true
skia_use_freetype_svg = false
skia_use_harfbuzz = true
skia_use_icu = false
skia_use_expat = false

# 编解码：全部关掉（编辑器只要 CPU 光栅 + 文本，顺带省包体）
skia_use_libpng_decode = false
skia_use_libjpeg_turbo_decode = false
skia_use_libwebp_decode = false
skia_use_libavif = false
skia_use_dng_sdk = false
skia_use_piex = false
skia_use_wuffs = false

# 非核心功能：全部关掉
skia_enable_pdf = false
skia_enable_skottie = false
skia_enable_svg = false
skia_enable_ganesh = false
skia_enable_graphite = false

skia_use_partition_alloc = false

# emsdk 由 .toolchain/emsdk 提供（注意用正斜杠，gn 里反斜杠是转义符）
skia_emsdk_dir = "SKIA_EMSDK_DIR_PLACEHOLDER"
'@
$ArgsGn = $ArgsGn.Replace('SKIA_EMSDK_DIR_PLACEHOLDER', ($Emsdk -replace '\\', '/'))

if ($Clean -and (Test-Path $OutDir)) { Remove-Item -Recurse -Force $OutDir }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $OutDir 'args.gn'), $ArgsGn)

# ── 6. gn gen ───────────────────────────────────────────────
Say "gn gen out/wasm"
Push-Location $SkiaSrc
try {
    & $Gn gen out/wasm
    if ($LASTEXITCODE -ne 0) { throw "gn gen 失败：exit $LASTEXITCODE" }

    # ── 7. ninja ────────────────────────────────────────────
    $jobs = @()
    if ($Jobs -gt 0) { $jobs = @("-j$Jobs") }
    Say "ninja -C out/wasm skia"
    & $Ninja -C out/wasm @jobs skia
    if ($LASTEXITCODE -ne 0) { throw "ninja 构建 Skia 失败：exit $LASTEXITCODE" }
} finally {
    Pop-Location
}

# ── 8. 报告 ─────────────────────────────────────────────────
if (-not (Test-Path $Lib)) { throw "构建结束但找不到 $Lib" }
$mb = (Get-Item $Lib).Length / 1MB
Say ("libskia.a  {0:N1} MB  ({1})" -f $mb, $Lib)
Say "现在可以跑： powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1"
