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
#   powershell -ExecutionPolicy Bypass -File scripts\verify_web.ps1 -Wasm wasm-debug   # 跑调试档产物
#   powershell -ExecutionPolicy Bypass -File scripts\verify_web.ps1 -Page worker.html  # 跑 Worker 宿主
param(
    [int]$Port = 8781,
    [int]$DebugPort = 9222,
    # 探针：可以给一条，也可以给一串（逗号分隔）。每条后面可以带 @页面，
    #   例如 "scripts/edit_probe.mjs@index.html,scripts/worker_probe.mjs@worker.html"。
    #   一串的好处只有一条但很大：静态服务与 Chrome 只起一次 —— 每次起一轮要 8~10 s
    #   （起服务、等调试端口、页面 boot、收进程），十轮就是一分半，而它量不到任何东西。
    [string]$Probe = 'scripts\edit_probe.mjs',
    # 产物档：wasm（交付）或 wasm-debug（断言 + 栈检查 + 源码映射）。预设见 CMakePresets.json。
    #   ★ 这是**预设名**，传进页面时要换成产物目录 build\<预设>\web —— 页面的 ?wasm=
    #   收的是目录（相对仓库根），不是预设名。当成预设名传过去会去取 <仓库根>\wasm\heditor.js
    #   （不存在），症状是"页面永远不 ready、探针等待超时"，看不出跟参数有关。
    [string]$Wasm = 'wasm',
    # 跑哪个页面：dist-app/index.html（示例页，验收基准）或 dist-app/multi.html（多实例）。
    #   地址带 ?transport=worker 就让示例页把引擎放进 Worker。
    #   两宿主并存，所以同一份探针必须两边都能跑 —— 这个开关就是那条路。
    [string]$Page = 'dist-app/index.html',
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

$glue = Join-Path $Root "build\$Wasm\web\heditor.wasm"
if (-not (Test-Path $glue)) { throw "还没构建 WASM：$glue（先跑 cmake --workflow --preset $Wasm）" }

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
    # ── 跑探针（一条或一串，共用同一个服务与浏览器）──
    #
    # 每个探针自己 Page.navigate，互不影响；省下的是每轮 8~10 s 的起停开销
    #   （起服务、等调试端口、页面 boot、收进程）—— 短探针的九成时间都是它。
    $jobs = @()
    foreach ($item in $Probe.Split(',')) {
        $one = $item.Trim()
        if (-not $one) { continue }
        $page = $Page
        $file = $one
        $at = $one.IndexOf('@')
        if ($at -ge 0) { $file = $one.Substring(0, $at); $page = $one.Substring($at + 1) }
        $jobs += [pscustomobject]@{ File = $file; Page = $page }
    }

    $exit = 0
    $results = @()
    # 探针默认会自己 Browser.close；批量跑时改由这里的 finally 统一收（按 profile 杀进程树）。
    $env:HEDITOR_KEEP_BROWSER = '1'
    Push-Location $Root
    try {
        foreach ($j in $jobs) {
            Write-Host "[verify] 跑探针 $($j.File) @ $($j.Page)"
            $t0 = Get-Date
            $probeOut = & $node $j.File --port $DebugPort `
                            --url "http://127.0.0.1:$Port/web/$($j.Page)?wasm=build/$Wasm/web" 2>&1 |
                        ForEach-Object { $_.ToString() }
            $code = $LASTEXITCODE
            $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
            $tag = [System.IO.Path]::GetFileNameWithoutExtension($j.File) + '@' + $j.Page
            $probeOut | Tee-Object -FilePath $log -Append
            if ($code -ne 0) { $exit = $code }
            Write-Host "[verify]   $tag 退出码 $code / ${dt}s"
            $results += [pscustomobject]@{ Tag = $tag; Code = $code; Seconds = $dt }
        }
    } finally {
        Pop-Location
        Remove-Item Env://HEDITOR_KEEP_BROWSER -ErrorAction SilentlyContinue
    }
    Write-Host "[verify] ── 本轮汇总 ──"
    foreach ($r in $results) {
        Write-Host ("[verify]   {0,-34} 退出码 {1}  {2,6} s" -f $r.Tag, $r.Code, $r.Seconds)
    }
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
