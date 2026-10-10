# 起一个本地静态服务，用来在浏览器里跑 WASM 验证台。
#
# 必须走 HTTP：WASM 的胶水层在 http(s) 下才能实例化，
# 直接双击 index.html（file://）会被浏览器的同源策略挡掉。
#
# 服务根目录 = 项目根，所以：
#   页面     http://localhost:8781/web/index.html
#   wasm     /build/wasm/web/ced_editor.js
#   字体     /assets/fonts/*.ttf
#
# 用法： powershell -ExecutionPolicy Bypass -File scripts\serve_web.ps1 [-Port 8781]
param(
    [int]$Port = 8781
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot

# 找一个能用的 python（优先本机 managed 版本）
$py = $null
foreach ($c in @(
        'C:\Users\huang\.workbuddy\binaries\python\versions\3.13.12\python.exe',
        'C:\Program Files\Python313\python.exe')) {
    if (Test-Path $c) { $py = $c; break }
}
if (-not $py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $py = $cmd.Source }
}
if (-not $py) { throw '找不到 python，无法起静态服务' }

$entry = Join-Path $Root 'web\index.html'
if (-not (Test-Path $entry)) { throw "缺少页面：$entry" }

$glue = Join-Path $Root 'build\wasm\web\ced_editor.js'
if (-not (Test-Path $glue)) {
    Write-Host '警告：还没构建 WASM。先执行：' -ForegroundColor Yellow
    Write-Host '  powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1' -ForegroundColor Yellow
    Write-Host ''
}

Write-Host "服务根目录：$Root"
Write-Host "验证台地址：http://localhost:$Port/web/index.html"
Write-Host '按 Ctrl+C 停止'
Write-Host ''

& $py -m http.server $Port --directory $Root
