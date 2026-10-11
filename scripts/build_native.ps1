# 本机（Windows）原生构建脚本。
#
# 为什么不用 VS 生成器 / MSBuild：
#   这台机器上进程环境里同时存在 PATH 与 Path 两个变量（大小写重复），
#   MSBuild 解析环境块时会抛 System.ArgumentException / MSB6001。
#   所以这里走 Ninja + 手工设置 MSVC 环境变量（INCLUDE / LIB / Path / VCToolsInstallDir ...）。
#
# 用法:
#   powershell -File scripts\build_native.ps1              # 只构建
#   powershell -File scripts\build_native.ps1 -Run         # 构建后跑单测 + 出 PNG
#   powershell -File scripts\build_native.ps1 -Rebuild     # 清掉 build 目录重来

param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")]
    [string]$Config = "Release",
    [switch]$Run,
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root "build\native"
$NinjaExe = Join-Path $Root ".toolchain\ninja\ninja.exe"

if (-not (Test-Path $NinjaExe)) {
    throw "缺少 ninja: $NinjaExe  （可从 https://github.com/ninja-build/ninja/releases 取 ninja-win.zip 解到 .toolchain\ninja）"
}

# ── MSVC + SDK 环境（工具链探测与环境变量集中在 _msvc_env.ps1）──────
. "$PSScriptRoot\_msvc_env.ps1"

Write-Host "[toolchain] MSVC   : $MsvcVer"
Write-Host "[toolchain] SDK    : $SdkVer"
Write-Host "[toolchain] Ninja  : $NinjaExe"

if ($Rebuild -and (Test-Path $BuildDir)) {
    Write-Host "[cmake] 清理 $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

# ── 配置 ────────────────────────────────────────────────────────
cmake -S $Root -B $BuildDir -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    "-DCMAKE_C_COMPILER=cl" `
    "-DCMAKE_CXX_COMPILER=cl" `
    "-DHEDITOR_BUILD_STAMP=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败" }

# ── 构建 ────────────────────────────────────────────────────────
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

Write-Host ""
Write-Host "[ok] 产物:"
Get-ChildItem -Path $BuildDir -Filter "heditor_*.exe" | ForEach-Object { Write-Host "     $($_.FullName)" }

if ($Run) {
    # 按名字收集全部断言型单测，不再手写清单 —— 手写过一次，就漏了一个套件
    # （heditor_edit_tests 当时被留在脚本外，"脚本绿了"并不等于"编辑路径也是绿的"）。
    # 命名约定：以 heditor 开头、_tests 结尾；诊断探针不叫这个名字，不会被收进来。
    #
    # 模式必须是 'heditor*_tests' 而不是 'heditor_*_tests'：后者要求两个下划线，收不到
    # heditor_tests.exe（它是 heditor + _tests，中间没有额外一段）—— 那个套件会被静默跳过，
    # 而脚本一路绿灯。收进来的东西只能靠"核对实际跑了几个"发现，脚本不会报错。
    $suites = @(Get-ChildItem -Path $BuildDir -Filter "heditor*_tests.exe" |
                Where-Object { $_.BaseName -like 'heditor*_tests' } |
                Sort-Object Name)
    if ($suites.Count -eq 0) { throw "没找到任何断言型单测（heditor_*_tests.exe），构建可能没产出" }

    $testExit = 0
    foreach ($suite in $suites) {
        Write-Host ""
        Write-Host "=== $($suite.BaseName) ==="
        & $suite.FullName
        if ($LASTEXITCODE -ne 0) { $testExit = $LASTEXITCODE }
    }

    Write-Host ""
    Write-Host "=== 原生渲染 ==="
    $outDir = Join-Path $Root "out"
    Push-Location $Root
    & (Join-Path $BuildDir "heditor_native.exe") --out-dir $outDir
    Pop-Location

    if ($testExit -ne 0) { throw "单测失败（退出码 $testExit）" }
}
