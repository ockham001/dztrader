<#
.SYNOPSIS
    dztrader 项目 setup 脚本（Windows，Ninja 单配置）
    一键执行 conan install + cmake configure

    Ninja 单配置生成器：每 config 独立 output-folder + binaryDir，
    toolchain 锁定 build_type。装 Release 或 Debug 由 -Config 决定。

.PARAMETER Config
    构建配置：Release 或 Debug（默认 Release）

.PARAMETER SkipConfigure
    跳过 cmake configure 步骤（仅跑 conan install）

.EXAMPLE
    .\scripts\setup.ps1                          # 装 Release + configure Release
    .\scripts\setup.ps1 -Config Debug            # 装 Debug + configure Debug
    .\scripts\setup.ps1 -SkipConfigure           # 只装依赖不配置
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [switch]$SkipConfigure
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# 激活 MSVC 环境（Ninja 生成器必需）
. "$PSScriptRoot\Activate-MSVC.ps1"

$profile = 'profiles/win-msvc-static'
$outputFolder = "build/windows/x86_64/$Config"

Write-Host "=== dztrader setup (Windows, Ninja 单配置) ===" -ForegroundColor Cyan
Write-Host "Config:        $Config"
Write-Host "Profile:       $profile"
Write-Host "OutputFolder:  $outputFolder"
Write-Host ""

# Step 1: conan install
Write-Host "[1/2] conan install" -ForegroundColor Yellow
$conanArgs = @(
    'install', '.',
    '-pr:b', $profile,
    '-pr:h', $profile,
    '--output-folder', $outputFolder,
    '--build=missing'
)
if ($Config -eq 'Debug') {
    $conanArgs += @('-s', 'build_type=Debug', '-s', 'compiler.runtime_type=Debug')
}

# profile 锁定的 compiler.version 只对"命中预编译缓存"的机器成立;
# 从源码编译依赖时 CMakeToolchain 按该值映射找 VS 安装 (194 -> VS 17, vswhere),
# 与实际安装不符会报 "VS non-existing installation: Visual Studio 17"
# (CI runner 装的是 VS 18/195, 本地缓存命中才一直没暴露)。
# 以 conan 自身检测为准, 不一致时追加 -s 覆盖 (仅影响本机包 id; 换机后依赖重建一次)。
$detectName = 'dz_setup_detect'
$detectProfile = Join-Path $env:USERPROFILE ".conan2\profiles\$detectName"
& conan profile detect --force --name $detectName 2>$null | Out-Null
if ($LASTEXITCODE -eq 0 -and (Test-Path $detectProfile)) {
    $detectedVer = (Select-String -Path $detectProfile -Pattern '^compiler\.version=(\d+)').Matches.Groups[1].Value
    Remove-Item $detectProfile -ErrorAction SilentlyContinue
    $pinnedVer = (Select-String -Path "$repoRoot\profiles\win-msvc-static" -Pattern '^compiler\.version=(\d+)').Matches.Groups[1].Value
    if ($detectedVer -and $detectedVer -ne $pinnedVer) {
        Write-Host "compiler.version override: profile=$pinnedVer -> detected=$detectedVer (依赖将按本机编译器重建)" -ForegroundColor Yellow
        $conanArgs += @('-s', "compiler.version=$detectedVer")
    }
}
Write-Host "conan $($conanArgs -join ' ')"
Push-Location $repoRoot
try {
    & conan @conanArgs
    if ($LASTEXITCODE -ne 0) { throw "conan install failed with exit code $LASTEXITCODE" }
}
finally { Pop-Location }

# Step 2: cmake configure
if (-not $SkipConfigure) {
    Write-Host ""
    Write-Host "[2/2] cmake configure" -ForegroundColor Yellow
    $preset = "win-$($Config.ToLower())"
    Write-Host "cmake --preset $preset"
    Push-Location $repoRoot
    try {
        & cmake --preset $preset
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
    }
    finally { Pop-Location }

    # 拷贝 compile_commands.json 到项目根（clangd 用）
    $cc = "$repoRoot\$outputFolder\compile_commands.json"
    if (Test-Path $cc) {
        Copy-Item $cc "$repoRoot\compile_commands.json" -Force
        Write-Host "compile_commands.json → 项目根（clangd）" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=== Setup done ===" -ForegroundColor Green

# 写粘性 config 文件（build/run-dev/clean 读这个决定默认 config）
$stickyDir = "$repoRoot\.dztrader_dev"
if (-not (Test-Path $stickyDir)) { New-Item -ItemType Directory -Path $stickyDir -Force | Out-Null }
$stickyFile = "$stickyDir\.config"
$Config | Out-File -FilePath $stickyFile -Encoding ascii -NoNewline
Write-Host "已写入粘性 config: $stickyFile = $Config" -ForegroundColor DarkGray

if ($Config -eq 'Debug') {
    Write-Host ""
    Write-Host "=== 已切到 Debug，后续 build/run-dev 默认 Debug ===" -ForegroundColor Yellow
    Write-Host "=== DEBUG 仅用于本地调试，禁止部署生产 ===" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Next steps:"
Write-Host "  Build:  .\scripts\build.ps1                    # 自动跟随 config"
Write-Host "  Run:    .\scripts\run-dev.ps1                  # build + 启动"
Write-Host "  Stop:   .\scripts\run-dev.ps1 -Stop"
Write-Host "  Clean:  .\scripts\clean.ps1"
