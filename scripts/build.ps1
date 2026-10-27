<#
.SYNOPSIS
    dztrader 构建脚本（Windows，Ninja 单配置）
    构建 C++ + 前端（含 dist 复制到 build/.../web/），一次搞定。

    config: 无参数即 Release（preset 名本身已编码 config，无需粘性文件）

.PARAMETER Config
    构建配置：Release 或 Debug（默认 Release；Debug 仅本地）

.PARAMETER Target
    构建指定 target（可选，如 dzmd_ctp）

.EXAMPLE
    .\scripts\build.ps1                    # Release
    .\scripts\build.ps1 -Config Debug      # Debug（仅本地）
    .\scripts\build.ps1 -Target dzmd_ctp   # 只构建 dzmd_ctp target
#>
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config,

    [string]$Target
)

$ErrorActionPreference = 'Stop'

if (-not $Config) { $Config = 'Release' }

# 激活 MSVC 环境（Ninja 生成器必需）
. "$PSScriptRoot\Activate-MSVC.ps1"

# 构建锁：ninja 不锁构建树，两个并发 ninja 会交错写 .ninja_deps / .ninja_log。
# 锁只在叶子脚本取；gate.sh 不持锁（否则父子互等死锁，见 scripts/gate.sh 注释）。
. "$PSScriptRoot\build-lock.ps1"
$buildLock = Get-DzBuildLock -RepoRoot (Resolve-Path "$PSScriptRoot\..") -Label 'build'

try {
    $preset = "win-$($Config.ToLower())"

    $cmakeArgs = @('--build', '--preset', $preset)
    if ($Target) { $cmakeArgs += @('--target', $Target) }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
finally {
    $buildLock.Dispose()
}
