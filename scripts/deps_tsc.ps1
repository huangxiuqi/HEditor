# 把 TypeScript 装进 emsdk 的 emscripten 目录 —— 那是 emcc 找 tsc 的第一顺位。
#
# 为什么装在那里：emcc 的 --emit-tsd 走 shared.get_npm_cmd('tsc')，它只看
#   <emscripten>/node_modules/.bin/tsc（找不到才回落到 PATH）。装进仓库自己的
#   node_modules 它看不见；靠 PATH 又要每个新终端各配一次（构建脚本、CI、IDE 三处各来一次）。
#
# 幂等：现有版本 >= 5.0 就直接返回。
#
# 用法：powershell -ExecutionPolicy Bypass -File scripts\deps_tsc.ps1

param([string]$Version = '5.6')

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Emsdk = Join-Path $Root '.toolchain\emsdk\upstream\emscripten'
$Tsc = Join-Path $Emsdk 'node_modules\.bin\tsc.cmd'

if (-not (Test-Path $Emsdk)) {
    throw "找不到 emsdk 的 emscripten 目录：$Emsdk（工具链没初始化？）"
}

# tsc --version 输出的是 "Version 5.6.3"，取里面的三段数字。
function Get-TscVersion([string]$exe) {
    if (-not (Test-Path $exe)) { return $null }
    $out = & $exe --version 2>$null
    if ($out -match '(\d+\.\d+\.\d+)') { return [version]$Matches[1] }
    return $null
}

$have = Get-TscVersion $Tsc
if ($have -and $have -ge [version]'5.0.0') {
    Write-Host "[tsc] 已有 $have（$Tsc），跳过"
    exit 0
}

Write-Host "[tsc] 安装 typescript@$Version 到 $Emsdk"
& npm install --prefix $Emsdk "typescript@$Version" --no-audit --no-fund
if ($LASTEXITCODE -ne 0) { throw "npm install 失败（退出码 $LASTEXITCODE）" }

$now = Get-TscVersion $Tsc
if (-not $now) { throw "安装后仍找不到 $Tsc" }
Write-Host "[tsc] 完成：$now"
