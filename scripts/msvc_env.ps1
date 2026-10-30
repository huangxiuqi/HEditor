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

# 判据：cl 起得来。起不来就别让后面的构建替它报错。
#
# 探这一下要放在降过 EAP 的子作用域里：cl 无参数会把横幅写 stderr，而 Windows PowerShell
#   5.1 把原生命令的 stderr 当错误记录（`2>&1`、`*>` 都一样），配上本脚本开头的
#   $ErrorActionPreference = 'Stop' 就直接中止 —— 于是"检查 cl"这件事本身成了故障源。
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw "进了开发环境但 Path 上没有 cl" }
$null = & { $ErrorActionPreference = 'Continue'; & cl *> $null }
if ($LASTEXITCODE -ne 0) { throw "cl 起来了但退出码是 $LASTEXITCODE（环境不完整）" }

# 版本不从 cl 的横幅里读（那行在 stderr，且文字随界面语言变）。取工具集自己的名字：
#   VsDevCmd 摆好的 VCToolsVersion（如 14.44.35207），缺席时退回 VCToolsInstallDir 的
#   末段目录名 —— 两者都是目录名，与语言、码页无关。
$tools = $env:VCToolsVersion
if (-not $tools -and $env:VCToolsInstallDir) { $tools = Split-Path $env:VCToolsInstallDir -Leaf }
if (-not $tools) { $tools = '版本未知' }
$sdk = $env:WindowsSDKVersion -replace '\\$', ''

Say ("MSVC {0}{1}" -f $tools, $(if ($sdk) { " + SDK $sdk" } else { '' }))
