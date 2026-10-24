# 取 Skia 源码与它的六个外部依赖（版本钉在 scripts/skia_rev.ps1）。
#
# 产物：.toolchain/skia/src/skia-main/（含 third_party/externals 下六个依赖）
#
# 这是**唯一的取数入口**：native 与 wasm 两份静态库都从这棵树编出来。两个构建脚本
# 在树与 pin 不符时先调本脚本，所以平时不用单独跑它。
#
# 它不需要 gn / ninja / emsdk，只要 curl 与 tar（Windows 10 起自带）。这条边界是刻意的：
# native 侧只需要 gn + ninja + 这棵树，取数不该反过来被 2 GB 的 emsdk 卡住。
#
# 可反复跑：树已与 pin 一致就什么都不做；下载落 .part、断了续传（curl -C -）；
#   解不开的包按原因分开处置 —— 见下面那段注释。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\skia_fetch.ps1
#   （或 powershell -ExecutionPolicy Bypass -File scripts\toolchain_init.ps1 -WithSkia）

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$Toolchain = Join-Path $Root '.toolchain'
$SkiaPkg   = Join-Path $Toolchain 'skia'
$SkiaSrc   = Join-Path $SkiaPkg 'src\skia-main'
$Externals = Join-Path $SkiaSrc 'third_party\externals'

function Say([string]$m) { Write-Host "[skia] $m" }

# Windows 自带的 tar 建不了符号链接（要开发者模式或管理员权限），碰到一条就整次解包失败。
# 上游归档里这类条目只服务别的构建系统（Skia 那条是 Rust 移植用的 Cargo.toml），本仓库
# 用不到，所以按路径逐个 exclude，跳过它们而不是让整包解不出来。
# 列表的列随 locale 变（中文 Windows 上是 "10月 09 18:19"，还多一列链接数），所以只拿
# 时间 HH:MM 当地锚，不按列号切。
# 返回 tar 的退出码，由调用方判定。
function Expand-TarSkippingSymlinks([string]$Tar, [string]$Dest) {
    $skip = @(& tar.exe -tvzf $Tar | ForEach-Object {
        if ($_ -match '^l\S+\s.*?\d{1,2}:\d{2}\s+(.+?) -> ') { "--exclude=$($Matches[1])" }
    })
    if ($skip.Count) { Say ("跳过 {0} 个符号链接（Windows 建不了）" -f $skip.Count) }
    & tar.exe -xzf $Tar -C $Dest @skip
    return $LASTEXITCODE
}

# ── 1. 源码（版本钉在 scripts/skia_rev.ps1）─────────────────
#   "树不在"与"树是别的版本"走同一条路：重取。不做差量替换 —— 差量未知，
#   而 out/ 里的产物必须跟着源码一起换，留着半新半旧只会得到解释不了的报错。
. (Join-Path $PSScriptRoot 'skia_rev.ps1')
Say "Skia pin: $SkiaCommit ($SkiaCommitDate)"

$state = Get-SkiaTreeState $SkiaSrc
if ($state -ne 'ok') {
    if (Test-Path $SkiaSrc) {
        # 不删：本仓库的删除守卫会拦，而且删错了不可逆。同卷改名是一次 rename
        #   （连 out/ 一起挪也很快），留着还能对照旧版本编出来的产物。
        #   确认无用后由人手工删 —— 脚本不替人做不可逆的事。
        $staleName = '_stale-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        $stale = Join-Path (Join-Path $SkiaPkg 'src') $staleName
        Say "现有源码树与 pin 不符（$state），挪到 $stale"
        Move-Item -LiteralPath $SkiaSrc -Destination $stale
    }
    $tar = Join-Path $SkiaPkg "skia-$SkiaCommit.tar.gz"
    $part = "$tar.part"
    # 下载落 .part、验完再改名：断在半路的那份不会冒充完整包，重跑时 curl -C - 从断点接着下。
    if (-not (Test-Path $tar)) {
        Say "下载 Skia 源码 $SkiaCommit（约 80 MB，走 codeload；断了重跑会续传）"
        New-Item -ItemType Directory -Force -Path $SkiaPkg | Out-Null
        & curl.exe -sSL --retry 5 --retry-delay 3 -C - --max-time 3600 `
            -o $part "https://codeload.github.com/google/skia/tar.gz/$SkiaCommit"
        if ($LASTEXITCODE -ne 0) {
            throw "下载 Skia 源码失败（已下的部分在 $part，重跑会续传）"
        }
        Move-Item -LiteralPath $part -Destination $tar
    }
    # codeload 对不存在的 commit 回一张 404 的 HTML，而 curl 的退出码仍是 0。
    #   体积是这里唯一便宜的判据（真包 80 MB，错误页几 KB）。检查放在下载分支之外：
    #   上一次失败留下的坏包同样会被 Test-Path 放过，而 tar 只会报"解压失败"。
    $tarLen = (Get-Item $tar).Length
    if ($tarLen -lt 20MB) {
        # 改名而不是删：删除守卫会拦，而且留着（换个名字）比消失更好查。
        Move-Item -LiteralPath $tar -Destination "$tar.bad" -Force
        throw "这份 tar 只有 $tarLen 字节，不是 Skia 源码（已改名 $tar.bad）：$SkiaCommit 在 codeload 上存在吗？"
    }
    # 解到自己的暂存目录再整体挪进去：解压中途失败时，src\ 下不会留半棵树拖累下一次。
    Say "解压 Skia 源码"
    $unpack = Join-Path $SkiaPkg '_unpack'
    if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
    New-Item -ItemType Directory -Force -Path $unpack | Out-Null
    if ((Expand-TarSkippingSymlinks -Tar $tar -Dest $unpack) -ne 0) {
        # 分清是谁的错：清单都读不出来，说明包是坏的（多为上次没下完），改名让它重下；
        # 清单读得完却解不开，就不是包的问题（文件系统拒绝某类条目），别白扔这 80 MB。
        & tar.exe -tzf $tar 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) {
            if (Test-Path $unpack) { Remove-Item -Recurse -Force $unpack }
            Move-Item -LiteralPath $tar -Destination "$tar.bad" -Force
            throw "这份 tar 读不出清单（多半是没下完，已改名 $tar.bad），重跑会重下"
        }
        throw "解压 Skia 源码失败，但包是完好的（看上面 tar 的报错）：$tar"
    }
    # 归档顶层目录改名成 skia-main —— 下游路径全按它写死（选取规则见 skia_rev.ps1）。
    $extracted = Find-ExtractedSkia $unpack
    if (-not $extracted) { throw "解压后找不到含 BUILD.gn 的目录：$unpack" }
    New-Item -ItemType Directory -Force -Path (Join-Path $SkiaPkg 'src') | Out-Null
    Move-Item -LiteralPath $extracted.FullName -Destination $SkiaSrc
    Remove-Item -Recurse -Force $unpack
    [System.IO.File]::WriteAllText((Join-Path $SkiaSrc '.heditor-skia-rev'),
        "$SkiaCommit  $SkiaCommitDate`n")
    $state = Get-SkiaTreeState $SkiaSrc
    if ($state -ne 'ok') { throw "源码就位后仍与 pin 不符（$state）：$SkiaSrc" }
    Say "源码就位：$SkiaCommit"
} else {
    Say "源码已是这一版（$state），跳过下载"
}

# ── 2. 外部依赖（DEPS 里钉住的 commit，只读归档，不碰 git）──
#    host 路径前不带斜杠：googlesource 的 +archive 要求这样。
$Deps = [ordered]@{
    'freetype'       = 'https://chromium.googlesource.com/chromium/src/third_party/freetype2/+archive/264b5fbf5b912b39f98d038bf75d39be0a73f21b.tar.gz'
    'harfbuzz'       = 'https://chromium.googlesource.com/external/github.com/harfbuzz/harfbuzz/+archive/9cb1fee51069b206effb4736e443b038d230789d.tar.gz'
    'zlib'           = 'https://chromium.googlesource.com/chromium/src/third_party/zlib/+archive/646b7f569718921d7d4b5b8e22572ff6c76f2596.tar.gz'
    'libpng'         = 'https://skia.googlesource.com/third_party/libpng/+archive/d5515b5b8be3901aac04e5bd8bd5c89f287bcd33.tar.gz'
    'libjpeg-turbo'  = 'https://chromium.googlesource.com/chromium/deps/libjpeg_turbo/+archive/e14cbfaa85529d47f9f55b0f104a579c1061f9ad.tar.gz'
    'libwebp'        = 'https://chromium.googlesource.com/webm/libwebp/+archive/845d5476a866141ba35ac133f856fa62f0b7445f.tar.gz'
}
# 每个依赖判"是否已就位"的哨兵文件（目录存在但为空也会被骗，所以按文件判）
$Sentinels = @{
    'freetype'      = 'include\freetype\freetype.h'
    'harfbuzz'      = 'src\hb.h'
    'zlib'          = 'zlib.h'
    'libpng'        = 'png.h'
    'libjpeg-turbo' = 'src\jpeglib.h'
    'libwebp'       = 'src\webp\decode.h'
}

New-Item -ItemType Directory -Force -Path $Externals | Out-Null
$Tmp = Join-Path $env:TEMP 'heditor_skia_deps'
New-Item -ItemType Directory -Force -Path $Tmp | Out-Null

foreach ($name in $Deps.Keys) {
    $dir = Join-Path $Externals $name
    $sentinel = Join-Path $dir $Sentinels[$name]
    if (Test-Path $sentinel) { continue }
    # 半截目录不能留：它里面可能已经解出了哨兵文件，下一次就会被当成"已就位"而跳过。
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }

    $tar = Join-Path $Tmp "$name.tar.gz"
    $part = "$tar.part"
    Say "取依赖 $name"
    & curl.exe -sSL --retry 5 --retry-delay 3 -C - --max-time 1800 -o $part $Deps[$name]
    if ($LASTEXITCODE -ne 0) { throw "下载 $name 失败（已下的部分在 $part，重跑会续传）" }
    Move-Item -LiteralPath $part -Destination $tar -Force

    # 同样解到暂存目录再整体挪：解压失败不会在 externals 下留半棵树。
    $stage = Join-Path $Tmp "$name-unpack"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    if ((Expand-TarSkippingSymlinks -Tar $tar -Dest $stage) -ne 0) {
        # 这几个包都只有几 MB，解不开就当坏的丢掉重下，不值得像 Skia 那样分情况。
        Remove-Item -LiteralPath $tar -Force
        throw "解压 $name 失败（包已删，重跑会重下）：$tar"
    }
    if (-not (Test-Path (Join-Path $stage $Sentinels[$name]))) {
        throw "$name 解压后仍找不到哨兵文件：$stage"
    }
    Move-Item -LiteralPath $stage -Destination $dir
}
Say "外部依赖就位"
Say "取数完成：$SkiaSrc"
