# 拉取构建工具链：gn / ninja / emsdk。
#
# 这三件不属于"依赖"（判据见 docs/依赖与工具链管理.md 第二节），它们是工具：不进版本库、
# 换机器要重新就位。正因为它不在库里，版本更不能靠人记 —— pin 全部写在这个文件里，
# 换机器跑一个命令就到位。
#
# gn 是唯一取不到上游制品的：本仓库用的那一份是 gn 源码 commit 3c7785ec 的构建产物，
# CIPD 的 gn/gn/windows-amd64 里没有这个实例（按 git_revision 取回 no such tag），
# Skia 自己 fetch-gn 钉的是更旧的 2175。所以这一步只校验、不下载，缺了就报错 ——
# 而且排在最前，免得白等十几分钟下完 emsdk 才发现 gn 没位。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\toolchain_init.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\toolchain_init.ps1 -SkipEmsdk
#
# 网络：走系统代理；设了 HTTPS_PROXY / HTTP_PROXY 就按它走（emsdk.py 也认这两个）。

param(
    [switch]$SkipEmsdk
)

$ErrorActionPreference = 'Stop'
# 下载进度条在 5.1 的 IWR 上会把控制台刷成一片，且明显拖慢小文件。
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Root      = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $Root '.toolchain'
$NinjaDir  = Join-Path $Toolchain 'ninja'
$NinjaExe  = Join-Path $NinjaDir 'ninja.exe'
$EmsdkDir  = Join-Path $Toolchain 'emsdk'
$Emitter   = Join-Path $EmsdkDir 'upstream\emscripten\emscripten-version.txt'
$GnExe     = Join-Path $Toolchain 'gn\out\gn.exe'

# gn 的判据是 --version 的原样输出，它同时含内部版本号与源码 commit 前 12 位。
$GnVersion = '2222 (3c7785ec2008)'
$GnCommit  = '3c7785ec2008e43c9efeae4ade0522cd36873833'

# ninja 有官方发布包，按版本号下载并校验 zip 的 SHA-256。
$NinjaVersion = '1.13.1'
$NinjaBase    = "https://github.com/ninja-build/ninja/releases/download/v$NinjaVersion"
$NinjaUrl     = "$NinjaBase/ninja-win.zip"
$NinjaSha     = '26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf'

# emsdk 是"安装器"，真正下载的是它 manifest 里那个 emscripten-releases 版本。
# 所以钉两样：安装器自身的 commit（决定能认出哪些 SDK）与要装的 SDK 版本号。
$EmsdkSdk  = '6.0.12'
$EmsdkRepo = '35ff8a6d150541276abbc6bae512ca90bcfbe220'
$EmsdkUrl  = "https://codeload.github.com/emscripten-core/emsdk/zip/$EmsdkRepo"

function Say([string]$m) { Write-Host "[toolchain] $m" }

# 用 .NET 的 ZipFile 而不是 Expand-Archive：后者在 5.1 上会把进度条刷满控制台，
# 速度也慢一个数量级。
Add-Type -AssemblyName System.IO.Compression.FileSystem

# 把 zip 解到一个新目录。目标已存在就报错，不做覆盖 —— 半解包的树比没有更难查。
function Expand-Zip([string]$Zip, [string]$Dest) {
    if (Test-Path $Dest) { throw "$Dest 已存在，请先删掉再重跑" }
    [System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dest)
}

# 下载到指定路径，顺带挡掉"拿到了但是空的"这种假成功。
function Get-Remote([string]$Url, [string]$Dest) {
    Say "下载 $Url"
    $iwr = @{ Uri = $Url; OutFile = $Dest; UseBasicParsing = $true }
    $proxy = if ($env:HTTPS_PROXY) { $env:HTTPS_PROXY } elseif ($env:HTTP_PROXY) { $env:HTTP_PROXY }
    if ($proxy) { $iwr.Proxy = $proxy }
    Invoke-WebRequest @iwr
    if (-not (Test-Path $Dest) -or (Get-Item $Dest).Length -lt 1024) {
        throw "下载失败或内容过短：$Dest"
    }
}

# ── 1. gn（只校验）───────────────────────────────────────────
function Get-GnVersion {
    if (-not (Test-Path $GnExe)) { return $null }
    $v = & $GnExe --version 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return ("$v").Trim()
}

$have = Get-GnVersion
if ($have -eq $GnVersion) {
    Say "gn $have 已在位"
} else {
    $now = if ($have) { $have } else { '缺失' }
    # 分两段输出：说明用 Write-Host（换行保得住），throw 只留一行结论。
    Write-Host "[toolchain] gn 不在位或版本不对（当前：$now，应为：$GnVersion）" -ForegroundColor Red
    Write-Host @"
这一份没有可下载的制品：它是 gn 源码 commit $GnCommit 的构建产物，
CIPD 的 gn/gn/windows-amd64 里查不到这个 commit（Skia 自己 fetch-gn 钉的是更旧的 2175）。
两条路，都要人来做：
  从已经装好的机器上拷 .toolchain\gn\out\gn.exe（2.3 MB），拷完跑 gn --version 复核；
  或者现编：clone 出 gn、检出 $GnCommit，python build/gen.py 之后用 ninja -C out gn，
  编译环境与 native 那套相同（CMakeUserPresets.json 里的 INCLUDE / LIB / Path）。
两条路做完都重跑本脚本，版本对上才算数。
"@ -ForegroundColor Yellow
    throw "gn 版本校验未通过"
}

# ── 2. ninja ────────────────────────────────────────────────
function Get-NinjaVersion {
    if (-not (Test-Path $NinjaExe)) { return $null }
    $v = & $NinjaExe --version 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return ("$v").Trim()
}

$have = Get-NinjaVersion
if ($have -eq $NinjaVersion) {
    Say "ninja $have 已在位"
} else {
    if ($have) { Say "ninja 是 $have，要换成 $NinjaVersion" }
    $zip = Join-Path $env:TEMP "heditor-ninja-$NinjaVersion.zip"
    Get-Remote -Url $NinjaUrl -Dest $zip
    $got = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    if ($got -ne $NinjaSha) {
        throw "ninja 包校验失败：$got，应为 $NinjaSha"
    }
    $stage = Join-Path $env:TEMP "heditor-ninja-$NinjaVersion"
    Expand-Zip -Zip $zip -Dest $stage
    $exe = Join-Path $stage 'ninja.exe'
    if (-not (Test-Path $exe)) { throw "解包后没有 ninja.exe，包结构可能变了" }
    New-Item -ItemType Directory -Force -Path $NinjaDir | Out-Null
    Copy-Item -Path $exe -Destination $NinjaExe -Force
    $have = Get-NinjaVersion
    if ($have -ne $NinjaVersion) { throw "ninja 装完报 $have，应为 $NinjaVersion" }
    Say "ninja $have 就位"
}

# ── 3. emsdk ────────────────────────────────────────────────
# emsdk 的引导脚本自己就是 python，它自带的 python 要 install 之后才有，所以这里先要一个。
function Get-EmsdkVersion {
    if (-not (Test-Path $Emitter)) { return $null }
    return (Get-Content $Emitter -Raw).Trim().Trim('"')
}

if ($SkipEmsdk) {
    Say "跳过 emsdk（-SkipEmsdk）"
} else {
    $have = Get-EmsdkVersion
    if ($have -eq $EmsdkSdk) {
        Say "emsdk $have 已在位"
    } else {
        if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
            throw "PATH 上找不到 python。emsdk 的安装器是 python 脚本，它自带的 python 要装完才有"
        }
        if ($have) {
            # 不删旧树：换版本失败时还得能退回去，而且删除是不可逆的。
            $aside = Join-Path $Toolchain "emsdk-$have"
            Say "已有 emsdk $have，改名保留为 $aside"
            if (Test-Path $aside) { throw "$aside 已存在，请先自行处理" }
            Rename-Item -Path $EmsdkDir -NewName (Split-Path $aside -Leaf)
        }

        # 安装器本身按 commit 取，不校验哈希：GitHub 对同一个 commit 前后两次打包出来的
        # zip 并不逐字节相同，拿哈希当判据会误报。身份由 commit 与 SDK 版本号两样确定。
        $zip = Join-Path $env:TEMP "heditor-emsdk-$EmsdkRepo.zip"
        Get-Remote -Url $EmsdkUrl -Dest $zip
        $stage = Join-Path $env:TEMP "heditor-emsdk-$EmsdkRepo"
        Expand-Zip -Zip $zip -Dest $stage
        $inner = Join-Path $stage "emsdk-$EmsdkRepo"
        if (-not (Test-Path $inner)) { throw "解包结构不认识：$stage" }
        New-Item -ItemType Directory -Force -Path $Toolchain | Out-Null
        Move-Item -Path $inner -Destination $EmsdkDir

        # 安装与激活分开：install 只落盘，activate 才写 .emscripten（emcc 找节点那份配置）。
        Push-Location $EmsdkDir
        try {
            Say "emsdk install $EmsdkSdk（约 2 GB，耗时十几分钟）"
            & python emsdk.py install $EmsdkSdk
            if ($LASTEXITCODE -ne 0) { throw "emsdk install 失败（退出码 $LASTEXITCODE）" }
            Say "emsdk activate $EmsdkSdk"
            & python emsdk.py activate $EmsdkSdk
            if ($LASTEXITCODE -ne 0) { throw "emsdk activate 失败（退出码 $LASTEXITCODE）" }
        } finally {
            Pop-Location
        }

        $have = Get-EmsdkVersion
        if ($have -ne $EmsdkSdk) { throw "emsdk 装完报 $have，应为 $EmsdkSdk" }
        Say "emsdk $have 就位"
    }
}

# ── 4. 汇总 ─────────────────────────────────────────────────
Write-Host ""
Say "工具链就位："
Say ("  gn     {0}   {1}" -f (Get-GnVersion), $GnExe)
Say ("  ninja  {0}   {1}" -f (Get-NinjaVersion), $NinjaExe)
$note = if ($SkipEmsdk) { '（本次跳过）' } else { '' }
Say ("  emsdk  {0}   {1}{2}" -f (Get-EmsdkVersion), $EmsdkDir, $note)
