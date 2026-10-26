<#
.SYNOPSIS
    dztrader 测试脚本（Windows，Ninja 单配置）

    config 来源优先级: -Config 参数 > .dztrader_dev/.config 粘性文件 > Release
    分层档: 默认 fast（秒级单测）；slow = 穷举/慢速；perf = 无阈值基准

.PARAMETER Config
    测试配置：Release 或 Debug（默认读粘性 config，无则 Release）

.PARAMETER Label
    测试分层：fast（默认）/ slow / perf

.PARAMETER TestName
    只运行匹配的测试（正则，可选）

.EXAMPLE
    .\scripts\test.ps1                      # 跟随粘性 config；默认 fast 档
    .\scripts\test.ps1 -Config Debug        # 临时测试 Debug
    .\scripts\test.ps1 -Label slow          # 只跑 slow 穷举档
    .\scripts\test.ps1 -TestName TimeTest   # 只跑 TimeTest
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config,

    [ValidateSet('fast', 'slow', 'perf')]
    [string]$Label = 'fast',

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

$preset = switch ($Label) {
    'slow' { "win-$($Config.ToLower())-slow" }
    'perf' { "win-$($Config.ToLower())-perf" }
    default { "win-$($Config.ToLower())" }
}

# 测试 DZTRADER_HOME（.cache/test_home，由 CMakePresets.json 的 test-base 注入）
# 每次运行前清空重建：上一轮残留的 shm meta 会让无关测试一起失败（#134）；
# 在 SetUp 内自管临时 home 的测试不受影响。
$testHome = "$PSScriptRoot\..\.cache\test_home"
if (Test-Path $testHome) { Remove-Item -Recurse -Force $testHome }
New-Item -ItemType Directory -Path $testHome -Force | Out-Null

$ctestArgs = @('--preset', $preset)
if ($TestName) { $ctestArgs += @('-R', $TestName) }
& ctest @ctestArgs
