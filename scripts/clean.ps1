<#
.SYNOPSIS
  清理所有 build 产物（保留 conan cache，下次 setup 不重编依赖）

  清理范围:
    - build/                          # 所有 build 产物（含 dist 复制的 web/）
    - apps/webui/frontend/dist/        # 前端 build 产物
    - apps/webui/frontend/node_modules/  # 前端依赖（可选 -IncludeNodeModules）
    - .dztrader_dev/                   # 调试 home（含 .config 粘性文件）
    - compile_commands.json            # 项目根 clangd 索引

  保留:
    - ~/.conan2/                       # conan cache（避免下次 setup 重编 20 分钟）

.PARAMETER IncludeNodeModules
  同时删除 apps/webui/frontend/node_modules/（下次 build 会自动 npm ci 重装，慢）
.EXAMPLE
  .\scripts\clean.ps1                          # 清理 build 产物
  .\scripts\clean.ps1 -IncludeNodeModules      # 连 node_modules 一起删
#>
param(
    [switch]$IncludeNodeModules
)

$project_root = Resolve-Path "$PSScriptRoot/.."
$ErrorActionPreference = 'SilentlyContinue'

Write-Host "=== dztrader clean ===" -ForegroundColor Cyan

$targets = @(
    "build",
    "apps\webui\frontend\dist",
    ".dztrader_dev",
    "compile_commands.json"
)
if ($IncludeNodeModules) {
    $targets += "apps\webui\frontend\node_modules"
}

foreach ($t in $targets) {
    $full = Join-Path $project_root $t
    if (Test-Path $full) {
        Write-Host "  删除 $t" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $full
    } else {
        Write-Host "  跳过 $t（不存在）" -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=== Clean done ===" -ForegroundColor Green
Write-Host "保留: ~/.conan2/（下次 setup 不重编依赖）"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  Setup:  .\scripts\setup.ps1                    # 重新 configure"
Write-Host "  Build:  .\scripts\build.ps1"
Write-Host "  Run:    .\scripts\run-dev.ps1"
