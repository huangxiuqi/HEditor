# 一键跑浏览器端验证：起静态服务 → 起无头 Chrome → 跑探针 → 清理。
#
# 为什么要包一层脚本：
#   · WASM 胶水层在 file:// 下实例化不了，必须走 HTTP；
#   · CDP 探针需要 Chrome 带 --remote-debugging-port 启动，而且**必须**指定
#     独立的 --user-data-dir（否则会去连用户正在用的那个 Chrome 实例，
#     那个实例默认不开调试端口，探针会连不上，或者更糟：干扰用户自己的浏览器）；
#   · 探针跑完要把 Chrome 和静态服务都收掉，不留孤儿进程。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\verify_web.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\verify_web.ps1 -KeepAlive   # 不清理，留着手动看
param(
    [int]$Port = 8781,
    [int]$DebugPort = 9222,
    [string]$Probe = 'scripts\edit_probe.mjs',
    [switch]$KeepAlive
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$OutDir = Join-Path $Root 'out\web'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ── 找 Chrome ──
$chrome = @(
    'C:\Program Files\Google\Chrome\Application\chrome.exe',
    'C:\Program Files (x86)\Google\Chrome\Application\chrome.exe',
    "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) { throw '找不到 Chrome' }

# ── 找 node（探针无第三方依赖，哪个都行；优先 managed）──
$node = @(
    'C:\Users\huang\.workbuddy\binaries\node\versions\22.22.2-6\node.exe',
    "$env:LOCALAPPDATA\Author Software\nvm\.nodejs\node.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $node) {
    $cmd = Get-Command node -ErrorAction SilentlyContinue
    if ($cmd) { $node = $cmd.Source }
}
if (-not $node) { throw '找不到 node' }

$glue = Join-Path $Root 'build\wasm\web\ced_editor.wasm'
if (-not (Test-Path $glue)) { throw "还没构建 WASM：$glue" }

# ── 找 python（与 serve_web.ps1 同一套优先级）──
$py = @(
    'C:\Users\huang\.workbuddy\binaries\python\versions\3.13.12\python.exe',
    'C:\Program Files\Python313\python.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $py) {
    $cmd = Get-Command python -ErrorAction SilentlyContinue
    if ($cmd) { $py = $cmd.Source }
}
if (-not $py) { throw '找不到 python，无法起静态服务' }

$log = Join-Path $Root 'out\verify_web.log'
# ★ 这个 profile 是**跨次运行复用**的（结尾只杀进程，不清目录），
#   于是 Chrome 的 HTTP 磁盘缓存会把上一次的 app.js 留给这一次的探针 ——
#   改了前端脚本、探针却在验证旧代码（实测踩过：改掉"光栅后端"标签后截图仍是旧文案）。
#   修法在探针侧：cdp.mjs 的 disableHttpCache()，在 Page.navigate 之前关掉缓存。
$profile = Join-Path $Root 'out\chrome-profile'
Write-Host "[verify] Chrome : $chrome"
Write-Host "[verify] node   : $node"
Write-Host "[verify] python : $py"
Write-Host "[verify] 探针   : $Probe"

$server = $null
$chromeProc = $null
$exit = 0
try {
    # ── 静态服务：直接持有 python 进程（不套一层 PowerShell，否则收进程时会留孤儿）──
    Write-Host "[verify] 起静态服务 :$Port"
    $server = Start-Process -PassThru -WindowStyle Hidden -FilePath $py -ArgumentList @(
        (Join-Path $PSScriptRoot 'dev_server.py'), '--port', "$Port", '--root', "$Root"
    ) -RedirectStandardOutput (Join-Path $Root 'out\verify_serve.log') `
      -RedirectStandardError  (Join-Path $Root 'out\verify_serve.err')

    # 等服务起来（最多 20s）
    $up = $false
    for ($i = 0; $i -lt 40; $i++) {
        try {
            $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 `
                 -Uri "http://127.0.0.1:$Port/web/index.html"
            if ($r.StatusCode -eq 200) { $up = $true; break }
        } catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $up) { throw "静态服务没起来（:$Port）" }

    # ── 无头 Chrome ──
    Write-Host "[verify] 起无头 Chrome 调试端口 :$DebugPort"
    $chromeArgs = @(
        '--headless=new',
        "--remote-debugging-port=$DebugPort",
        "--user-data-dir=$profile",
        '--no-first-run', '--no-default-browser-check',
        '--disable-extensions', '--disable-background-networking',
        '--disable-gpu',
        '--hide-scrollbars',
        '--window-size=1500,1200',
        'about:blank'
    )
    $chromeProc = Start-Process -PassThru -WindowStyle Hidden -FilePath $chrome `
                    -ArgumentList $chromeArgs `
                    -RedirectStandardOutput (Join-Path $Root 'out\verify_chrome.log') `
                    -RedirectStandardError  (Join-Path $Root 'out\verify_chrome.err')

    # 等调试端口可用（最多 30s）
    $dbg = $false
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $null = Invoke-RestMethod -TimeoutSec 2 -Uri "http://127.0.0.1:$DebugPort/json/version"
            $dbg = $true; break
        } catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $dbg) { throw "Chrome 调试端口没起来（:$DebugPort）" }

    # ── 跑探针 ──
    #
    # ★ 用 ForEach-Object { $_.ToString() } 把 stderr 的 ErrorRecord 拍平成字符串。
    #   直接 `2>&1 | Tee-Object` 时，PowerShell 会把原生进程的 stderr 包成
    #   NativeCommandError，堆栈和真正的错误信息会被截断成一行看不出所以然的东西 ——
    #   探针失败时最需要看到的就是那段信息。
    Write-Host "[verify] 跑探针"
    Push-Location $Root
    try {
        $probeOut = & $node $Probe --port $DebugPort `
                        --url "http://127.0.0.1:$Port/web/index.html" 2>&1 |
                    ForEach-Object { $_.ToString() }
        $exit = $LASTEXITCODE
        $probeOut | Tee-Object -FilePath $log
    } finally {
        Pop-Location
    }
    Write-Host "[verify] 探针退出码 $exit"
} finally {
    if (-not $KeepAlive) {
        # ★ 按 user-data-dir 精确回收 Chrome 的整棵进程树。
        #   Stop-Process 只杀得掉主进程，Chrome 会留下一堆 renderer/gpu 子进程；
        #   而用 profile 路径做匹配只会命中我们自己起的那个实例，不会碰到
        #   用户正在用的浏览器。
        $stray = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
                 Where-Object { $_.CommandLine -and $_.CommandLine.Contains($profile) }
        foreach ($p in $stray) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
        if ($chromeProc -and -not $chromeProc.HasExited) {
            Stop-Process -Id $chromeProc.Id -Force -ErrorAction SilentlyContinue
        }
        if ($server -and -not $server.HasExited) {
            Write-Host "[verify] 关闭静态服务"
            Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        Write-Host "[verify] -KeepAlive：Chrome(pid $($chromeProc.Id)) 与服务(pid $($server.Id)) 保留"
    }
}

Write-Host "[verify] 日志：$log"
exit $exit
