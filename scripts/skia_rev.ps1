# Skia 源码版本 —— 唯一来源。两个构建脚本都从这里取，不要再各写一份。
#
# 为什么钉：光栅器是交付物的一部分，换一个上游 commit 就可能换掉抗锯齿、字形
#   栅格化与 SIMD 路径 —— 于是"昨天量到的 3.5 ms"和今天不可比，native 与 wasm
#   两份库也不再同源。取分支还有更直接的问题：换台机器重建会拿到另一棵树，
#   而症状只是"像素和以前不完全一样"，没有任何一处会报错。
#
# 升版本（四步，缺一步等于没升）：
#   ① 改下面的 $SkiaCommit / $SkiaCommitDate / $SkiaBuildGn；
#   ② 跑 scripts\build_skia_wasm.ps1 —— 它发现树不是这个版本会挪开旧的、按新
#      commit 重取源码与六个外部依赖；
#   ③ 重跑 scripts\build_skia_native.ps1 与两个 cmake workflow；
#   ④ native 三套断言 + 浏览器探针全绿后，改 docs\依赖与工具链管理.md 那张表。
#
# $SkiaBuildGn 是**根 BUILD.gn 的 git blob 哈希**（"blob <长度>\0" 前缀那种，
# 不是普通 SHA-1）。它是"树到底是不是这个 commit"的判据：戳记文件可以手改，
# 内容不会。取法：git ls-tree <commit> BUILD.gn。
$SkiaCommit     = 'f98d6562a4df02c9f12255eeedc15c7d2ec78e59'
$SkiaCommitDate = '2026-10-09'
$SkiaBuildGn    = 'b1e29de9151aba92a4a2c4cb8488794c3a3ee5b1'

<#
树的状态，三选一：
  missing  没有源码树（首次，或刚被挪走）
  stale    树在，但不是 $SkiaCommit
  ok       就是它

判据只看内容：根 BUILD.gn 的 blob 哈希。戳记文件（.heditor-skia-rev）由
build_skia_wasm.ps1 解包后写入，供人一眼看出当前是哪一版，不参与判定 ——
它要是参与，改一行文本就能让检查失效。
#>
function Get-GitBlobSha1([string]$Path) {
    $bytes  = [System.IO.File]::ReadAllBytes($Path)
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0")
    $buf    = New-Object 'byte[]' ($header.Length + $bytes.Length)
    [Array]::Copy($header, 0, $buf, 0, $header.Length)
    [Array]::Copy($bytes, 0, $buf, $header.Length, $bytes.Length)
    $sha = [System.Security.Cryptography.SHA1]::Create()
    try { $hash = $sha.ComputeHash($buf) } finally { $sha.Dispose() }
    return (-join ($hash | ForEach-Object { $_.ToString('x2') }))
}

function Get-SkiaTreeState([string]$Tree) {
    $buildGn = Join-Path $Tree 'BUILD.gn'
    if (-not (Test-Path $buildGn)) { return 'missing' }
    if ((Get-GitBlobSha1 $buildGn) -ne $SkiaBuildGn) { return 'stale' }
    return 'ok'
}

<#
解包后哪个目录是源码树。归档的顶层目录名随来源变（分支是 skia-main，commit 是
skia-<sha>），而下游路径（out/、CMake 的 include）全按 skia-main 写死。

★ 按内容找（有没有 BUILD.gn），不按名字猜。但要排除 _stale-*：被挪开的旧树
也有 BUILD.gn，而且 '_' 排在 's' 前面 —— 不排除就会把旧树当成刚解出来的那份。
#>
function Find-ExtractedSkia([string]$Parent) {
    return Get-ChildItem $Parent -Directory |
           Where-Object { $_.Name -notlike '_stale*' -and (Test-Path (Join-Path $_.FullName 'BUILD.gn')) } |
           Select-Object -First 1
}
