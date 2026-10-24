# 在当前 shell 里进 MSVC 的开发环境（Developer PowerShell 启动时跑的就是这一步）。
#
# 为什么需要：CMake 会自己找到 cl.exe、认出它的版本，但 cl 不像 GCC 那样有内置的头文件
# 搜索路径 —— 标准库头、kernel32.lib、link.exe 全靠 INCLUDE / LIB / Path 三个环境变量。
# 少了它们，编译期报 C1083（找不到 cstdint），链接期报 LNK1181（找不到 kernel32.lib），
# 看着像源码坏了，其实是环境没摆。
#
# 这里直接用微软自己的 Enter-VsDevShell（Common7\Tools\Microsoft.VisualStudio.DevShell.dll），
# 环境由 VS 自己算，所以仓库里不需要任何版本号目录，也不用 vcvars64.bat。本脚本只做两件事：
# 用 vswhere 找到实例 id，以及在已经进过环境时不再进第二次。
#
# 用法（在仓库根目录）：
#   .\scripts\msvc_env.ps1
#   cmake --preset native
#
# 点号引入（. .\scripts\msvc_env.ps1）也一样：它只改环境变量，而环境变量是进程级的，
# 两种调用都留在当前 shell 里。
#
# 你也可以完全不用它：从开始菜单开「Developer PowerShell for VS 2022」效果一样。

$ErrorActionPreference = 'Stop'

function Say([string]$m) { Write-Host "[msvc] $m" }

# 已经在开发环境里就不要再进一次：Enter-VsDevShell 会把 INCLUDE / LIB 越拼越长。
if ((Get-Command cl -ErrorAction SilentlyContinue) -and $env:INCLUDE -like '*Tools\MSVC*') {
    Say "已在 MSVC 环境里（cl: $((Get-Command cl).Source)）"
    return
}

# 用 vswhere 拿实例（它随 VS 安装，不依赖注册表命令）；instanceId 才是 Enter-VsDevShell 认的键。
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "找不到 vswhere：$vswhere —— 这台机器没装 Visual Studio" }

$q = @('-latest', '-products', '*',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64')
$id = (& $vswhere @q -property instanceId) -join ''
$vsPath = (& $vswhere @q -property installationPath) -join ''
if (-not $id -or -not $vsPath) {
    throw "没有找到带 C++ 工具集的 VS 实例（装 VS 时勾「使用 C++ 的桌面开发」）"
}

$devShell = Join-Path $vsPath.Trim() 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
if (-not (Test-Path $devShell)) { throw "找不到 DevShell 模块：$devShell" }

$instanceId = $id.Trim()
Import-Module $devShell
# 它会打一行 VS 横幅（Developer PowerShell 启动时那四行），去不掉，当它是环境就绪的标记。
Enter-VsDevShell $instanceId -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'

# 判据：cl 能起来并报出版本。起不来就别让后面的构建替它报错。
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "进了开发环境但 Path 上没有 cl" }
$banner = & cl 2>&1 | Select-Object -First 1
if ("$banner" -notmatch '(\d+\.\d+\.\d+)') { throw "cl 的输出不认识：$banner" }

Say ("MSVC {0} + SDK {1}" -f $Matches[1], ($env:WindowsSDKVersion -replace '\\$', ''))
