# 用 clang-format 统一 C/C++ 代码格式。
#
# 风格定义在仓库根目录的 .clang-format（Google 家族基准，见文件内注释）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\format.ps1          # 就地格式化
#   powershell -ExecutionPolicy Bypass -File scripts\format.ps1 -Check   # 只检查，有差异则退出码 1
#
# 只处理 src/ 与 tests/ 下的 C/C++；third_party / .toolchain / build / out 一律不动。
param(
    [switch]$Check,
    [string[]]$Paths
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot

# 找可用的 clang-format。
#
# 先试绝对路径（VS 自带 / LLVM 安装），最后才试 PATH。
# PATH 上可能挂着 depot_tools 的 clang-format 包装脚本 —— 它要求当前目录位于
# Chromium 检出内，否则只打印报错、**不返回任何替换**，会让检查模式误判为合规。
function Find-ClangFormat {
    $cands = @(
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin\clang-format.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-format.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\bin\clang-format.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\bin\clang-format.exe',
        'C:\Program Files\LLVM\bin\clang-format.exe'
    )
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { $cands += $cmd.Source }
    foreach ($c in $cands) {
        if (-not (Test-Path $c)) { continue }
        $ver = & $c --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $ver -match 'clang-format version') { return $c }
    }
    throw "找不到可用的 clang-format（要能输出 clang-format version）。可装 LLVM，或用 VS 自带的 VC\Tools\Llvm\bin。"
}

$cf = Find-ClangFormat
Write-Host "[format] clang-format: $cf"
Write-Host "[format] $(& $cf --version)"

# 收集目标文件：src 与 tests 下的 .h / .cpp（不含构建产物与第三方）
$targets = @()
if ($Paths) {
    $targets = $Paths | ForEach-Object { Join-Path $Root $_ }
} else {
    $roots = @((Join-Path $Root 'src'), (Join-Path $Root 'tests'))
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $targets += Get-ChildItem -Path $r -Recurse -File -Include '*.h', '*.hpp', '*.cpp' |
            ForEach-Object { $_.FullName }
    }
}
if (-not $targets) { throw "没有找到要格式化的文件。" }

$changed = @()
if ($Check) {
    foreach ($f in $targets) {
        $diff = & $cf --style=file --output-replacements-xml $f
        if ($diff -match '<replacement ') { $changed += $f }
    }
    if ($changed.Count -gt 0) {
        Write-Host "[format] 需要格式化（$($changed.Count) / $($targets.Count) 个文件）：" -ForegroundColor Yellow
        $changed | ForEach-Object { Write-Host "  $_" }
        Write-Host "[format] 运行 scripts\format.ps1 就地修复。"
        exit 1
    }
    Write-Host "[format] 全部合规（$($targets.Count) 个文件）。"
    exit 0
}

foreach ($f in $targets) {
    $before = Get-Item $f | Select-Object -ExpandProperty LastWriteTimeUtc
    & $cf --style=file -i $f
    if ($LASTEXITCODE -ne 0) { throw "clang-format 处理失败：$f" }
    $after = Get-Item $f | Select-Object -ExpandProperty LastWriteTimeUtc
    if ($after -ne $before) { $changed += $f }
}
Write-Host "[format] 完成：改动 $($changed.Count) / $($targets.Count) 个文件。"
$changed | ForEach-Object { Write-Host "  $_" }
