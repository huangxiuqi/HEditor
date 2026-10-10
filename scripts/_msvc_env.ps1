# MSVC + Windows SDK 工具链环境（**供其它脚本 dot-source**，不单独运行）。
#
#   . "$PSScriptRoot\_msvc_env.ps1"
#
# 为什么手工设环境变量、而不是调 vcvars64.bat：
#   本机 vcvars64.bat 跑不起来 —— 它内部会调 reg.exe，而 reg.exe 被安全策略拦截。
#
# 为什么还需要它、而不是每个脚本各写一份：
#   verify_header_deps.ps1 一开始就是漏了这段，于是它调 ninja 时 INCLUDE 是空的，
#   cl 连 <cstddef> 都找不到（fatal error C1083），构建静默失败，
#   断言报出来的却是"没重编" —— 一个由**环境缺失**伪装成的"依赖追踪坏了"。
#   这类假现场比真 bug 更费时间，所以环境只留一份真源。

$vsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC'
if (-not (Test-Path $vsRoot)) {
    $vsRoot = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC'
}
if (-not (Test-Path $vsRoot)) {
    $vsRoot = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC'
}
if (-not (Test-Path $vsRoot)) {
    throw '找不到 MSVC 工具集目录（尝试过 Professional / Community / BuildTools）'
}
$MsvcVer = (Get-ChildItem -Path $vsRoot -Directory |
            Sort-Object { [version]$_.Name } -Descending |
            Select-Object -First 1).FullName

$sdkRoot = 'C:\Program Files (x86)\Windows Kits\10'
if (-not (Test-Path "$sdkRoot\Include")) {
    throw "找不到 Windows SDK: $sdkRoot\Include"
}
$SdkVer = (Get-ChildItem -Path "$sdkRoot\Include" -Directory |
           Where-Object { $_.Name -match '^10\.' } |
           Sort-Object { [version]$_.Name } -Descending |
           Select-Object -First 1).Name

$env:VCToolsInstallDir = "$MsvcVer\"
$env:WindowsSdkDir     = "$sdkRoot\"
$env:WindowsSDKVersion = "$SdkVer\"

$env:INCLUDE = @(
    "$MsvcVer\include"
    "$sdkRoot\Include\$SdkVer\ucrt"
    "$sdkRoot\Include\$SdkVer\shared"
    "$sdkRoot\Include\$SdkVer\um"
) -join ';'

$env:LIB = @(
    "$MsvcVer\lib\x64"
    "$sdkRoot\Lib\$SdkVer\ucrt\x64"
    "$sdkRoot\Lib\$SdkVer\um\x64"
) -join ';'

# ★ 用 $env:Path 而不是 $env:PATH。
#   PowerShell 里两者是同一个变量（不区分大小写），但显式写 Path 与
#   build_native.ps1 的历史写法一致，也和"进程里别同时出现 PATH 和 Path"这条
#   本机踩过的坑对得上（MSBuild 会因为重复的大小写变体炸掉）。
$env:Path = @(
    "$MsvcVer\bin\Hostx64\x64"
    "$sdkRoot\bin\$SdkVer\x64"
    $env:Path
) -join ';'
