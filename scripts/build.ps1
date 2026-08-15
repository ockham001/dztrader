<#
.SYNOPSIS
    dztrader 构建脚本（Windows，Ninja 单配置）
    构建 C++ + 前端（含 dist 复制到 build/.../web/），一次搞定。

    config 来源优先级: -Config 参数 > .dztrader_dev/.config 粘性文件 > Release

.PARAMETER Config
    构建配置：Release 或 Debug（默认读 .dztrader_dev/.config，无则 Release）

.PARAMETER Target
    构建指定 target（可选，如 dzmd_ctp）

.EXAMPLE
    .\scripts\build.ps1                    # 跟随粘性 config（默认 Release）
    .\scripts\build.ps1 -Config Debug      # 临时构建 Debug
    .\scripts\build.ps1 -Target dzmd_ctp   # 只构建 dzmd_ctp target
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config,

    [string]$Target
)

$ErrorActionPreference = 'Stop'

# 读粘性 config（.dztrader_dev/.config），-Config 优先覆盖
if (-not $Config) {
    $stickyFile = "$PSScriptRoot\..\.dztrader_dev\.config"
    if (Test-Path $stickyFile) {
        $Config = (Get-Content $stickyFile -Raw).Trim()
    }
    if (-not $Config) { $Config = 'Release' }
}

# 激活 MSVC 环境（Ninja 生成器必需）
. "$PSScriptRoot\Activate-MSVC.ps1"

$preset = "win-$($Config.ToLower())"

$cmakeArgs = @('--build', '--preset', $preset)
if ($Target) { $cmakeArgs += @('--target', $Target) }
& cmake @cmakeArgs
