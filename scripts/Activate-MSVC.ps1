<#
.SYNOPSIS
    激活 MSVC 编译器环境（Ninja 生成器必需）

.DESCRIPTION
    Ninja 生成器不像 VS 生成器那样自动激活 MSVC 环境，需手动调用 vcvarsall.bat。
    本脚本用 vswhere 查找 Visual Studio 安装路径，调用 vcvarsall.bat x64，
    把环境变量导入当前 PowerShell 进程。

    setup.ps1 / build.ps1 / test.ps1 开头 dot-source 本脚本：
        . "$PSScriptRoot\Activate-MSVC.ps1"

    已激活（cl.exe 在 PATH）则跳过，重复调用无开销。
#>
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe 未找到: $vswhere`n请安装 Visual Studio 2022（含 C++ 桌面开发工作负载）"
}

$vsPath = & $vswhere -latest -prerelease -property installationPath
if (-not $vsPath) {
    throw "未检测到 Visual Studio 安装，请安装 VS 2022（含 C++ 桌面开发工作负载）"
}

# 已激活且路径匹配则跳过
$expectedVcvars = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
$clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
if ($clPath -and $clPath.StartsWith($vsPath, [System.StringComparison]::OrdinalIgnoreCase)) {
    return
}

$vcvars = "$vsPath\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat 未找到: $vcvars"
}

Write-Host "激活 MSVC 环境: $vcvars x64" -ForegroundColor DarkGray

# 调用 vcvarsall.bat x64，捕获其 set 输出，导入环境变量到当前 PowerShell 进程
cmd /c "`"$vcvars`" x64 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2]
    }
}

# 强制控制台输出代码页为 UTF-8 (chcp 65001)。必须在 vcvarsall.bat 之后设置，
# 避免被其重置。
# 必需：CMakeLists.txt 显式设置 CMAKE_CL_SHOWINCLUDES_PREFIX = "注意: 包含文件:" (UTF-8)，
# CMake 用 Encoding::ConsoleOutput 把前缀写入 rules.ninja 的 msvc_deps_prefix，
# cl.exe /showIncludes 也按控制台代码页编码输出。
# ninja 字节级比较 rules.ninja 中的 msvc_deps_prefix 与 cl.exe 实际输出，
# 两者必须用同一编码。chcp 65001 保证 configure 和 build 时编码一致 (均为 UTF-8)。
# 注意：VSLANG=1033 在 VS 18 (2026) 上对 cl.exe /showIncludes 输出无效 (clui.dll 仍
# 加载中文资源)，不能用于强制英文输出，所以必须用 chcp 65001 + 中文前缀方案。
chcp 65001 > $null
$env:PYTHONUTF8 = '1'  # Python 工具（如 conan）也用 UTF-8
