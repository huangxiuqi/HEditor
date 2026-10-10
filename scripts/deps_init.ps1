# 拉取源码依赖：HarfBuzz 走 git submodule，钉到 tag 并裁剪工作区。
#
# 为什么不用压缩包：版本要能用 git 表达（版本等于 gitlink 里的 commit），升级就是改一条
# gitlink，不再依赖"目录名里的版本号加手工解压"这两样都没法核对的东西。判断依据与配套
# 约定写在 docs/依赖与工具链管理.md。
#
# 本脚本执行 git 写入命令（submodule add、fetch、checkout、sparse-checkout），
# 按本仓库约定只能由人运行，智能体不执行。跑完请自行提交 .gitmodules 与那条 gitlink。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File scripts\deps_init.ps1
#   powershell -ExecutionPolicy Bypass -File scripts\deps_init.ps1 -NoSparse

param(
    [switch]$NoSparse
)

$ErrorActionPreference = 'Stop'

$Root        = Split-Path -Parent $PSScriptRoot
$Rel         = 'third_party/harfbuzz'
$Dir         = Join-Path $Root 'third_party\harfbuzz'
$HbUrl       = 'https://github.com/harfbuzz/harfbuzz.git'
# 版本用 tag 表达，因为名字比 SHA 好读；但 submodule 真正钉住的是 tag 解析出来的
# commit —— gitlink 只能存 SHA，存不下 tag 名。所以两个都写：下面是入口（要拉的 ref），
$HbTag       = '14.6.0'
# 下面是它的解析结果，也是末尾用来断言"取到的确实是这个版本"的期望值。
# 上游若重新打过同名 tag，这里会报错而不是静默换版本。
# 升级 HarfBuzz 就是同时改这两个常量，并在文档里记一条。
$HbTagCommit = 'c7a7457b7385f33178e8cf87615ca077a810bbe7'

function Say([string]$m) { Write-Host "[deps] $m" }

# git 把进度与提示写在 stderr。Windows PowerShell 在 ErrorActionPreference 为 Stop 时
# 会把它当成 terminating error，脚本会在命令其实成功的情况下中断。所以全部 git 调用
# 走下面两个包装：临时放宽容错、把两路输出转成字符串，只按退出码判定成败。
function Invoke-Git {
    param([string[]]$GitArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git @GitArgs 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    foreach ($line in $out) { Write-Host "    $line" -ForegroundColor DarkGray }
    return $code
}

function Get-GitOut {
    param([string[]]$GitArgs)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & git @GitArgs 2>$null
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne 0) { throw "git $($GitArgs -join ' ') 失败，退出码 $code" }
    return ($out | Out-String).Trim()
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "PATH 上找不到 git"
}

# ── 0. 现状检查 ──────────────────────────────────────────────
# 目录已存在但不是 submodule（例如手工解压出来的旧目录），不擅自删除，直接报错让人处理。
$isSubmodule = $false
if (Test-Path $Dir) {
    $status = & git -C $Root submodule status -- $Rel 2>$null
    if ($LASTEXITCODE -eq 0 -and $status) { $isSubmodule = $true }
    if (-not $isSubmodule) {
        throw "$Dir 已存在但不是 submodule。请先自行处理（旧目录可改名保留），再重跑本脚本。"
    }
    Say "已存在 submodule，改为更新"
}

# ── 1. 添加或更新 submodule ──────────────────────────────────
if (-not $isSubmodule) {
    Say "添加 submodule：$HbUrl -> $Rel（浅克隆）"
    if ((Invoke-Git @('-C', $Root, 'submodule', 'add', '--depth', '1', $HbUrl, $Rel)) -ne 0) {
        throw "git submodule add 失败"
    }
} else {
    Say "更新 submodule（浅克隆）"
    if ((Invoke-Git @('-C', $Root, 'submodule', 'update', '--init', '--depth', '1', '--', $Rel)) -ne 0) {
        throw "git submodule update 失败"
    }
}

# ── 2. 钉 tag ────────────────────────────────────────────────
# 浅克隆只有默认分支的末端，tag 未必在本地，所以按 ref 单独取一次。
# 显式 refspec 只写 FETCH_HEAD、不会在本地建出 tag 引用，因此下面用 FETCH_HEAD 解析
# commit，不去引用 refs/tags/<tag>（浅取之后那个引用并不存在，引用它会直接失败）。
Say "取 tag $HbTag"
if ((Invoke-Git @('-C', $Dir, 'fetch', '--depth', '1', 'origin', "refs/tags/$HbTag")) -ne 0) {
    throw "取 tag $HbTag 失败"
}
$resolved = Get-GitOut @('-C', $Dir, 'rev-parse', 'FETCH_HEAD^{commit}')
if ($resolved -ne $HbTagCommit) {
    throw "tag $HbTag 解析到 $resolved，与脚本记录的 $HbTagCommit 不一致（上游可能重新打过这个 tag）"
}
Say "检出 $HbTag（$HbTagCommit）"
if ((Invoke-Git @('-C', $Dir, 'checkout', '--detach', $HbTagCommit)) -ne 0) {
    throw "检出 $HbTag 失败"
}

# ── 3. 稀疏检出 ──────────────────────────────────────────────
# 只要 src/ 与根文件：上游 test/ 占 92 MB，而本工程只编译 src/harfbuzz.cc
# （它的 include 全在 src/ 内，没有 ../）。用 sparse-checkout set 而不手写
# sparse-checkout 文件，避免 Windows 下写出的 CRLF 让排除规则永远匹配不上。
if (-not $NoSparse) {
    Say "裁剪工作区（根文件加 src/）"
    if ((Invoke-Git @('-C', $Dir, 'sparse-checkout', 'set', '--cone', 'src')) -ne 0) {
        Write-Host "[deps] 稀疏检出失败，工作区保持完整（不影响构建，只是多占磁盘）" -ForegroundColor Yellow
    }
}

# ── 4. 校验 ──────────────────────────────────────────────────
# 按文件判，不按目录是否存在判：目录存在但为空也会骗人。
$sentinels = @('src\harfbuzz.cc', 'src\hb-version.h')
foreach ($s in $sentinels) {
    $p = Join-Path $Dir $s
    if (-not (Test-Path $p)) { throw "校验失败：缺少 $p" }
}

$head = Get-GitOut @('-C', $Dir, 'rev-parse', 'HEAD')
if ($head -ne $HbTagCommit) { throw "校验失败：HEAD 是 $head，应为 $HbTagCommit" }

# 工作区必须与 HEAD 一致。只对 HEAD 不对文件，会漏掉"检出落在了别处、文件还是旧内容"。
$dirty = Get-GitOut @('-C', $Dir, 'status', '--porcelain')
if ($dirty) { throw "校验失败：子模块工作区与 HEAD 不一致：`n$dirty" }

if (-not $NoSparse -and (Test-Path (Join-Path $Dir 'test'))) {
    Write-Host "[deps] 警告：test/ 仍存在，稀疏检出未生效（不影响构建，只是多占 92 MB）" -ForegroundColor Yellow
}

$size = [math]::Round((Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue |
                       Measure-Object -Property Length -Sum).Sum / 1MB, 1)
# 别拿工作区文件的 md5 与 git 里的内容比：本机 core.autocrlf=true，工作区是 CRLF 检出，
# 每行比 blob 多一个字节（hb-version.h 是 2692 对 2597，差的 95 就是行数），
# 去 CR 之后才逐字节相同。要比就比 blob 标识。
$verBlob = Get-GitOut @('-C', $Dir, 'rev-parse', 'HEAD:src/hb-version.h')

Say "HEAD    : $head"
Say "工作区  : $size MB（裁剪后约 10 MB）"
Say "版本头  : blob $verBlob"
Say "          （工作区是 CRLF 检出，文件本身比 blob 大，不要拿 md5 直接比）"

Write-Host ""
Write-Host "[deps] 剩下的由人完成：" -ForegroundColor Yellow
Write-Host "  提交 .gitmodules 与 third_party/harfbuzz 这条 gitlink —— 不提交的话，之后任何"
Write-Host "  一次 git submodule update 都会把 HEAD 拉回主分支尖端，这个 tag 就白钉了"
Write-Host "  跑一次 powershell -ExecutionPolicy Bypass -File scripts\build_native.ps1 -Run"
Write-Host "  跑一次 powershell -ExecutionPolicy Bypass -File scripts\build_wasm.ps1"
Write-Host "  旧的 third_party/harfbuzz-14.6.0 先留着待查，确认无碍后再自行清理"
