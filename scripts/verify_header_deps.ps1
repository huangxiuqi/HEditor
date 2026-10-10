# 可证伪地验证「改头文件会不会触发重编译」。
#
# 为什么需要这个脚本：
#   本机（中文 Windows / CP936 + CMake 4.1 + MSVC + Ninja + C++20）上，
#   头文件依赖追踪**开箱就是坏的**，而且坏得没有任何征兆：
#     · ninja 的 `deps = msvc` 拿 rules.ninja 里那条 UTF-8 的 msvc_deps_prefix
#       去匹配 cl.exe 吐出的 CP936 字节 → 匹配不上 → `ninja -t deps` 全是 `#deps 0`；
#     · CMake 4.1 只要 C++20 就给 MSVC 生成带 `-scanDependencies` 的 dyndep 规则，
#       但那个 .dd 里**只有模块依赖、一个头文件都没有**。
#   两条路都不记头文件 ⇒ 改头文件不重编 ⇒ 用旧类布局的 .obj 配新布局的 .lib 混链
#   ⇒ 段错误（不是编译错误）。实测过：给 EditStats 加一个 bool 字段后，
#   shaping_test.cpp 没重编，链接出的 ced_tests.exe 直接段错误，
#   而它打印的仍是「55 项断言，失败 0 项」——"测试通过"这句话直接失去可信度。
#
#   最小复现（纯 CMake 行为，与本项目无关）：
#       set(CMAKE_CXX_STANDARD 20)  +  MSVC  +  Ninja  +  一个被两个 TU include 的头
#       → ninja -t deps 报 #deps 0；改那个头 → `ninja` 说 "no work to do"，一个都不重编；
#         跑出来的 exe 还是旧行为。
#       加上 set_property(SOURCE a.cpp b.cpp PROPERTY OBJECT_DEPENDS <那个头>)
#       → 同一个改动立刻重编两个 TU 并重链接，exe 输出跟着变。
#
#   CMakeLists.txt 里用 ced_track_header_deps()（OBJECT_DEPENDS）绕过去了。
#   本脚本就是那个绕法的**回归测试**：删掉 OBJECT_DEPENDS，这里必须变红。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\verify_header_deps.ps1
#
# ★ 为什么靠比对文件 mtime，而不是解析 ninja 的输出 / 用 `ninja -n`：
#   (1) `ninja -n` 在本项目里压根给不出计划。GLOB 带了 CONFIGURE_DEPENDS，
#       而干跑模式不会执行 glob 检查、也就更新不了它的 stamp，ninja 于是悲观地
#       认为 manifest 陈旧，打印 "Re-running CMake..." 就停住（实测连跑 3 次都一样）。
#   (2) 解析 ninja 的控制台输出本身也不可靠：实测一次 17 步的构建，
#       捕获到的文本只到 `[10/17]`，后面 5 条 Linking 行整段丢了 ——
#       据此断言会得到"没重链接"的假失败。
#   比对 mtime 不经过任何文本层，是文件系统事实，也就没有这两个问题。

$ErrorActionPreference = 'Stop'

$Root     = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root 'build\native'
$NinjaExe = Join-Path $Root '.toolchain\ninja\ninja.exe'
$Hdr      = Join-Path $Root 'src\core\engine.h'

if (-not (Test-Path $NinjaExe)) { throw "缺少 ninja: $NinjaExe" }
if (-not (Test-Path $Hdr))      { throw "缺少头文件: $Hdr" }
if (-not (Test-Path (Join-Path $BuildDir 'build.ninja'))) {
    throw "还没配置过：$BuildDir（先跑 scripts\build_native.ps1）"
}

# ★ 必须自己把 MSVC 环境准备好。
#   漏了这一步的症状极具误导性：cl 连 <cstddef> 都找不到 → 构建静默失败 →
#   断言报出来的是"头文件改了但它没重编"，看起来像依赖追踪又坏了，
#   其实是环境没设。假现场比真 bug 更费时间。
. "$PSScriptRoot\_msvc_env.ps1"
Write-Host "[deps] MSVC     : $MsvcVer"
Write-Host "[deps] SDK      : $SdkVer"

# ── 断言收集（与浏览器探针同一套口径：打印 + 计数 + 退出码）──
$script:rows = @()
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    $script:rows += [pscustomobject]@{ Name = $name; Ok = $ok; Detail = $detail }
    $tag = if ($ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}{2}" -f $tag, $name, $(if ($detail) { "   $detail" } else { '' }))
}

function Run-Ninja([string[]]$NinjaArgs) {
    # 原生进程的 stderr 走 2>&1 会被包成 NativeCommandError，
    # 直接赋值/Tee 会把真正的输出丢掉 —— 先拍平成字符串。
    # 顺手把 cl 那 1700 行「注意: 包含文件:」滤掉，只留真信号。
    $out = & $NinjaExe -C $BuildDir @NinjaArgs 2>&1 |
           ForEach-Object { $_.ToString() } |
           Where-Object { $_ -notmatch '包含文件' }
    # 退出码要在管道之后立刻取，否则会被后续命令覆盖。
    $script:lastNinjaExit = $LASTEXITCODE
    return ($out -join "`n")
}

function Get-MTime($rel) {
    $p = Join-Path $BuildDir $rel
    if (-not (Test-Path $p)) { return $null }
    return (Get-Item $p).LastWriteTimeUtc
}

# ── 契约 ──
#   ced_track_header_deps(tgt, ... 目录) 注册过的 target，
#   它下面**每一个** .obj 都要因该目录下的头文件变动而重编。
#   粒度是粗的（改一个头文件 = 重编该 target 全部 TU）—— 这是**有意**的取舍：
#   用"粗"换"绝不会静默用旧对象"。本项目每个 target 只有几个到十几个 TU，值。
#
#   harfbuzz 那个巨型 TU（69 个 .cc 聚合成一个翻译单元）**不跟着 core 走**：
#   它没注册 src/core，所以改任意一个 core 头都不会重编它（否则改一行 core 头
#   就要重编整个 HarfBuzz）。但它注册了自己的 src/，所以改 HarfBuzz 自己的头
#   必须重编它 —— 这两条方向相反，第 4 节与第 5 节分别断言。
$expectRebuild = @(
    'CMakeFiles\ced_core.dir\src\core\doc\document.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\text\font.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\text\shaper.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\text\fallback.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\render\bitmap.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\layout\layout.cpp.obj',
    'CMakeFiles\ced_core.dir\src\core\engine.cpp.obj',
    'CMakeFiles\ced_native.dir\src\app\native_main.cpp.obj',
    'CMakeFiles\ced_tests.dir\tests\shaping_test.cpp.obj',
    'CMakeFiles\ced_edit_tests.dir\tests\edit_test.cpp.obj',
    'CMakeFiles\ced_abi_probe.dir\tests\probe_engine_size.cpp.obj'
)
$expectLink = @(
    'ced_core.lib',
    'ced_native.exe',
    'ced_tests.exe',
    'ced_edit_tests.exe',
    'ced_abi_probe.exe'
)
# harfbuzz 的 obj 路径取决于 CMakeLists 里 CED_HB_ROOT 选的是哪一份源码
# （submodule 优先、旧目录回退），所以按"最近被构建维护的那个"找，不写死路径 ——
# 写死会让本脚本去断言一个已经不在构建图里的旧文件，看着通过其实没有保护。
$hbObjFull = @(Get-ChildItem -Path $BuildDir -Recurse -Filter 'harfbuzz.cc.obj' -ErrorAction SilentlyContinue) |
             Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (-not $hbObjFull) { throw "找不到 harfbuzz.cc.obj（先在 $BuildDir 构建一次）" }
$harfbuzzObj = $hbObjFull.FullName.Substring($BuildDir.Length + 1)

# 顺带从它的路径反推 HarfBuzz 源码根的相对位置，免得再假设用的是 submodule 那份。
$hbSrcRootRel = ($harfbuzzObj -replace '^CMakeFiles\\ced_harfbuzz\.dir\\', '') -replace '\\src\\harfbuzz\.cc\.obj$', ''

Write-Host "[deps] 构建目录 : $BuildDir"
Write-Host "[deps] 目标头文件: $Hdr"

# ── 1. 先追到最新，并确认已经收敛 ──
Write-Host "`n[1] 先把工程追到最新"
$null = Run-Ninja @()
if ($script:lastNinjaExit -ne 0) { throw "预热构建失败（退出码 $($script:lastNinjaExit)）：先跑 scripts\build_native.ps1 看看" }
$null = Run-Ninja @()
if ($script:lastNinjaExit -ne 0) { throw "预热构建失败（退出码 $($script:lastNinjaExit)）" }
$pre = @{}
foreach ($r in ($expectRebuild + $expectLink + @($harfbuzzObj))) { $pre[$r] = Get-MTime $r }
$missingPre = @($pre.Keys | Where-Object { $null -eq $pre[$_] })
if ($missingPre.Count) { throw "构建产物缺失，先手工构建一次：$($missingPre -join ', ')" }
Write-Host "     产物齐全（11 个 obj + 5 个链接产物 + harfbuzz）"

# ── 2. 只动 mtime，不改内容 ──
Write-Host "`n[2] 把 engine.h 的 mtime 推到现在（内容一字不改）"
$hashBefore = (Get-FileHash $Hdr -Algorithm SHA256).Hash
(Get-Item $Hdr).LastWriteTime = (Get-Date)
$hdrMt = (Get-Item $Hdr).LastWriteTimeUtc
$hashAfter = (Get-FileHash $Hdr -Algorithm SHA256).Hash
Check '内容没动（只是 mtime 变了）' ($hashBefore -eq $hashAfter) "sha256 $($hashBefore.Substring(0,12))…"

# ── 3. 构建，然后按 mtime 看谁真的被重做了 ──
Write-Host "`n[3] 构建（下面这段输出只作展示，断言不依赖它）"
$outLog = Join-Path $Root 'out\hdr_deps_build.log'
$buildOut = Run-Ninja @()
$buildExit = $script:lastNinjaExit
$buildOut | Out-File -FilePath $outLog -Encoding utf8
Write-Host "     完整输出：$outLog"

# ★ 先断言"构建本身是成功的"，再谈"谁被重编了"。
#   不先拦这一条的话，构建失败会以"产物没更新"的形式表现出来，
#   于是报告"依赖追踪坏了" —— 那是假现场，真因在别处（往往是环境没设好）。
Check '构建本身成功（没被环境之类的问题拦住）' ($buildExit -eq 0 -and $buildOut -notmatch 'FAILED:') `
      "ninja 退出码 $buildExit"

# ── 4. 断言：重编 ──
Write-Host "`n[4] 断言"
$notRebuilt = @()
foreach ($r in $expectRebuild) {
    $mt = Get-MTime $r
    if ($null -eq $mt -or $mt -lt $hdrMt) { $notRebuilt += $r }
}
Check '注册过 src/core 的 11 个 TU 全部重编' ($notRebuilt.Count -eq 0) `
      $(if ($notRebuilt.Count) { '没重编: ' + ($notRebuilt -join ', ') } else { '11 / 11' })

$notRelinked = @()
foreach ($r in $expectLink) {
    $mt = Get-MTime $r
    if ($null -eq $mt -or $mt -lt $hdrMt) { $notRelinked += $r }
}
Check '5 个链接产物全部重做' ($notRelinked.Count -eq 0) `
      $(if ($notRelinked.Count) { '没重做: ' + ($notRelinked -join ', ') } else { '5 / 5' })

# ★ 反向断言：harfbuzz 的 obj 必须**原封不动**。
#   比"它的 mtime 不大于头文件"更强 —— 直接和快照比，能抓到任何一次多余的重编。
$hbNow = Get-MTime $harfbuzzObj
Check 'harfbuzz 的 obj 原封不动（粒度没失控）' ($hbNow -eq $pre[$harfbuzzObj]) `
      $("快照 $($pre[$harfbuzzObj]) / 现在 $hbNow")

# ── 5. 反向场景：改 HarfBuzz 自己的头，它的聚合 TU 必须重编 ──
#   漏掉这一条的后果不是"慢"，而是"编出来的是上一份源码"：harfbuzz.cc 本身没变，
#   变的都在它 include 进来的 .cc/.hh 里，而 MSVC 侧没有任何自动依赖通道。
Write-Host "`n[5] 把 HarfBuzz 头的 mtime 推一下（内容一字不改）"
$hbHdr = Join-Path $Root ((Join-Path $hbSrcRootRel 'src') + '\hb-version.h')
if (-not (Test-Path $hbHdr)) { throw "缺少 HarfBuzz 头: $hbHdr" }
$hbHashBefore = (Get-FileHash $hbHdr -Algorithm SHA256).Hash
(Get-Item $hbHdr).LastWriteTime = (Get-Date)
$hbHashAfter = (Get-FileHash $hbHdr -Algorithm SHA256).Hash
Check 'HarfBuzz 头的内容没动（只是 mtime 变了）' ($hbHashBefore -eq $hbHashAfter) `
      "sha256 $($hbHashBefore.Substring(0,12))…"

$hbBefore = Get-MTime $harfbuzzObj
$null = Run-Ninja @()
$hbAfter = Get-MTime $harfbuzzObj
Check '改 HarfBuzz 的头会让它的聚合 TU 重编（否则静默沿用上一份源码）' `
      ($null -ne $hbBefore -and $null -ne $hbAfter -and $hbAfter -gt $hbBefore) `
      "之前 $hbBefore / 现在 $hbAfter"

# ── 6. 收敛性：再跑一次不能还有活 ──
Write-Host "`n[6] 收敛性"
$mid = @{}
foreach ($r in ($expectRebuild + $expectLink + @($harfbuzzObj))) { $mid[$r] = Get-MTime $r }
$null = Run-Ninja @()
$changed = @()
foreach ($r in $mid.Keys) { if ((Get-MTime $r) -ne $mid[$r]) { $changed += $r } }
Check '再构建一次，一个产物都没被重做（构建已收敛）' ($changed.Count -eq 0) `
      $(if ($changed.Count) { '又重做了: ' + ($changed -join ', ') } else { '0 个变化' })

$failed = @($script:rows | Where-Object { -not $_.Ok }).Count
Write-Host "`n──────────────────────────────"
Write-Host ("共 {0} 项断言，失败 {1} 项" -f $script:rows.Count, $failed)
if ($failed -gt 0) {
    Write-Host @"

★ 说明头文件依赖追踪又断了。
  查 CMakeLists.txt 里的 ced_track_header_deps()：是不是没被调用、
  或者 GLOB 的目录参数写错了、又或者被谁删掉了。
  断了之后的表现是「改头文件不重编 → .obj 与新 .lib 混链 → 随机段错误」，
  不会报编译错误，所以必须在构建图这一层拦住。
"@
}
exit $failed
