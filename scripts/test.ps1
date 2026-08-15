<#
.SYNOPSIS
    dztrader 测试脚本（Windows，Ninja 单配置）

    config 来源优先级: -Config 参数 > .dztrader_dev/.config 粘性文件 > Release

.PARAMETER Config
    测试配置：Release 或 Debug（默认读粘性 config，无则 Release）

.PARAMETER TestName
    只运行匹配的测试（正则，可选）

.EXAMPLE
    .\scripts\test.ps1                      # 跟随粘性 config
    .\scripts\test.ps1 -Config Debug        # 临时测试 Debug
    .\scripts\test.ps1 -TestName TimeTest   # 只跑 TimeTest
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config,

    [string]$TestName
)

$ErrorActionPreference = 'Stop'

# 读粘性 config，-Config 优先覆盖
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

$ctestArgs = @('--preset', $preset)
if ($TestName) { $ctestArgs += @('-R', $TestName) }
& ctest @ctestArgs
