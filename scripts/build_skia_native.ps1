# 构建 x64 版 Skia（给 native 宿主链接用）。
#
# 产物：.toolchain/skia/src/skia-main/out/win-x64/skia.lib
#
# 与 build_skia_wasm.ps1 的差别只有三处：target_os / target_cpu、运行库换成 /MD、
# 不需要 emsdk。其余 feature 开关逐条对齐 —— 两边都只要 CPU 光栅 + 文本。
#
# 为什么 /MD 必须显式写：cl 不带运行库参数时默认 /MT，而本工程用 /MD，
# 混链会在链接期报 LNK2038（RuntimeLibrary=MT_StaticRelease vs MD_DynamicRelease）。
# 库与主程序必须是同一套 CRT。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\build_skia_native.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\build_skia_native.ps1 -Clean
#
# 首次跑（或源码树与 pin 不符）会先调 scripts\skia_fetch.ps1 取源码与六个外部依赖
# （约 80 MB），不需要 emsdk；只需 gn 与 ninja 在位（scripts\toolchain_init.ps1）。
param(
    [string]$VcRoot = "",
    [int]$Jobs = 0,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $Root '.toolchain'
$SkiaSrc   = Join-Path $Toolchain 'skia\src\skia-main'
$Gn        = Join-Path $Toolchain 'gn\out\gn.exe'
$Ninja     = Join-Path $Toolchain 'ninja\ninja.exe'
$OutDir    = Join-Path $SkiaSrc 'out\win-x64'
$Lib       = Join-Path $OutDir 'skia.lib'

function Say([string]$m) { Write-Host "[skia-native] $m" }

foreach ($p in @($Gn, $Ninja)) {
    if (-not (Test-Path $p)) { throw "缺少工具链：$p" }
}

# ── 0. 源码树版本（钉在 scripts/skia_rev.ps1）───────────────
#   两份库（wasm 与 native）必须出自同一棵树：宏与实现一旦分叉，症状是
#   "两边像素不一样"或更糟的 ODR 违规 —— 而不会有任何一处会报错。
#   取数只有一条路（scripts/skia_fetch.ps1，native 与 wasm 共用）；它不需要 emsdk，
#   所以 native 这条路不被那 2 GB 挡着。
. (Join-Path $PSScriptRoot 'skia_rev.ps1')
if ((Get-SkiaTreeState $SkiaSrc) -ne 'ok') {
    & (Join-Path $PSScriptRoot 'skia_fetch.ps1')
}
$state = Get-SkiaTreeState $SkiaSrc
if ($state -ne 'ok') { throw "取数之后源码树仍与 pin 不符（$state）：$SkiaSrc" }
Say "Skia $SkiaCommit ($SkiaCommitDate)"

# ── 1. 找 MSVC 与 Windows SDK ───────────────────────────────
# 不走 vcvars（本机 reg.exe 被拦），按标准安装目录现找，与 gn 的 find_msvc.py 同一口径。
if ($VcRoot -eq "") {
    $candidates = @()
    foreach ($base in @("$env:ProgramFiles\Microsoft Visual Studio",
                        "${env:ProgramFiles(x86)}\Microsoft Visual Studio")) {
        if (-not (Test-Path $base)) { continue }
        foreach ($rel in @('2022', '2019', '2017')) {
            foreach ($ed in @('Enterprise', 'Professional', 'Community', 'BuildTools', 'Preview')) {
                $p = Join-Path $base "$rel\$ed\VC"
                if (Test-Path $p) { $candidates += $p }
            }
        }
    }
    if ($candidates.Count -eq 0) { throw "找不到 Visual Studio 的 VC 目录，用 -VcRoot 指定" }
    $VcRoot = $candidates[0]
}
Say "VC: $VcRoot"

$msvcRoot = Join-Path $VcRoot 'Tools\MSVC'
if (-not (Test-Path $msvcRoot)) { throw "缺少 $msvcRoot" }
$msvcVer = Get-ChildItem $msvcRoot -Directory |
           Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
           Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($null -eq $msvcVer) { throw "在 $msvcRoot 下找不到 MSVC 工具集" }
$Msvc = $msvcVer.FullName
Say "MSVC: $($msvcVer.Name)"

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
$sdkVer = Get-ChildItem (Join-Path $sdkRoot 'Include') -Directory |
          Where-Object { Test-Path (Join-Path $_.FullName 'um\Windows.h') } |
          Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($null -eq $sdkVer) { throw "在 $sdkRoot\Include 下找不到 Windows SDK" }
$SdkInc = $sdkVer.FullName
$SdkLib = Join-Path $sdkRoot "Lib\$($sdkVer.Name)"
Say "SDK: $($sdkVer.Name)"

$env:INCLUDE = "$Msvc\include;$SdkInc\ucrt;$SdkInc\shared;$SdkInc\um"
$env:LIB     = "$Msvc\lib\x64;$SdkLib\ucrt\x64;$SdkLib\um\x64"
$env:Path    = "$Msvc\bin\Hostx64\x64;$sdkRoot\bin\$($sdkVer.Name)\x64;$env:Path"

# ── 2. gn 参数 ──────────────────────────────────────────────
# 与 wasm 那份逐条对齐，只有 target 与 /MD 不同。is_official_build=true 会让
#   third_party 去找系统库，所以下面每条 skia_use_system_* 都显式关掉。
#
# ★ 参数写成 <out>/args.gn，不要拼 "--args=..." 塞在命令行上。这些值必须带引号
#   （target_os="win"、extra_cflags=["/MD"]），而 PowerShell 把实参递给 native 程序时，
#   **5.1 不转义内嵌引号**：gn 收到的是 target_os=win / extra_cflags=[/MD]，报
#   `ERROR at the command-line "--args": Invalid token`。PS 7 的 Windows 参数模式反倒会
#   保留引号，所以在 7 上试不出来（本机就是这么漏过去的）。写文件这条路过的是 gn 自己的
#   解析，与 shell 引号规则无关；顺带 args 还留在 out/ 里能直接看（wasm 那份早就是这个写法）。
$ArgsGn = @'
target_os = "win"
target_cpu = "x64"
is_debug = false
is_official_build = true

# 外部依赖一律从源码编（is_official_build 默认会去找系统库）
skia_use_system_zlib = false
skia_use_system_libpng = false
skia_use_system_libjpeg_turbo = false
skia_use_system_libwebp = false
skia_use_system_freetype2 = false

# 文本：FreeType 后端 + HarfBuzz 整形
skia_use_freetype = true
skia_use_freetype_svg = false
skia_use_harfbuzz = true
skia_use_icu = false
skia_use_expat = false

# 编解码：全部关掉（编辑器只要 CPU 光栅 + 文本）
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

# 运行库 /MD：cl 不带这一项时默认 /MT，与本工程混链会报 LNK2038
extra_cflags = [ "/MD" ]
'@

Push-Location $SkiaSrc
try {
    if ($Clean -and (Test-Path $OutDir)) {
        Say "清理 $OutDir"
        & $Ninja -C $OutDir -t clean | Out-Null
        Remove-Item $OutDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $OutDir 'args.gn'), $ArgsGn)
    Say "gn gen"
    & $Gn gen $OutDir
    if ($LASTEXITCODE -ne 0) { throw "gn gen 失败（exit=$LASTEXITCODE）" }

    Say "ninja skia"
    if ($Jobs -gt 0) {
        & $Ninja -C $OutDir -j $Jobs skia
    } else {
        & $Ninja -C $OutDir skia
    }
    if ($LASTEXITCODE -ne 0) { throw "ninja 失败（exit=$LASTEXITCODE）" }
} finally {
    Pop-Location
}

if (-not (Test-Path $Lib)) { throw "构建结束但产物不存在：$Lib" }
$mb = [math]::Round((Get-Item $Lib).Length / 1MB, 1)
Say "完成：$Lib（$mb MiB）"
