<#
.SYNOPSIS
    dztrader 测试脚本（Windows，Ninja 单配置）

    config: 无参数即 Release（preset 名本身已编码 config，无需粘性文件）
    分层档: 默认 fast（秒级单测）；slow = 穷举/慢速；perf = 无阈值基准

.PARAMETER Config
    测试配置：Release 或 Debug（默认 Release）

.PARAMETER Label
    测试分层：fast（默认）/ slow / perf

.PARAMETER TestName
    只运行匹配的测试（正则，可选）

.EXAMPLE
    .\scripts\test.ps1                      # Release；默认 fast 档
    .\scripts\test.ps1 -Config Debug        # Debug（仅本地）
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

if (-not $Config) { $Config = 'Release' }

# 激活 MSVC 环境（Ninja 生成器必需）
. "$PSScriptRoot\Activate-MSVC.ps1"

# 测试锁：与 build 共用同一把锁，避免「测试跑到一半被并发构建换掉二进制」，
# 也避免两个 test 同时删同一个 test_home 而互相踩。
. "$PSScriptRoot\build-lock.ps1"
$buildLock = Get-DzBuildLock -RepoRoot (Resolve-Path "$PSScriptRoot\..") -Label 'test'

try {
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

    # ctest 并行度（#136）：gtest_discover_tests 把每个用例注册为独立进程，-j 才能并行；
    # 用例级并发安全由「临时资源唯一化」保证（固定名目录会互踩，已全仓整改 + preflight lint 兜底）。
    # 显式设置该变量的调用方优先，否则默认 4。
    if (-not $env:CTEST_PARALLEL_LEVEL) { $env:CTEST_PARALLEL_LEVEL = '4' }

    $ctestArgs = @('--preset', $preset)
    if ($TestName) { $ctestArgs += @('-R', $TestName) }
    Write-Host "ctest $($ctestArgs -join ' ') (CTEST_PARALLEL_LEVEL=$env:CTEST_PARALLEL_LEVEL)"
    & ctest @ctestArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    $buildLock.Dispose()
}
