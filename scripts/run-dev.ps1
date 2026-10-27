<#
.SYNOPSIS
  调试启动脚本：build → install → 从 install 目录启动 master

.DESCRIPTION
  为什么先 install 再启动（#168）：
    build/ 是中间产物目录，二进制在那里会被下次构建重链；Windows 上这会导致
    LNK1168（文件被运行中的进程占用）。install/ 是唯一产物源：运行、部署、
    打包同源。

  DZTRADER_HOME 默认 <repo>\.dztrader_dev；已设置则尊重调用方。

.PARAMETER Stop
  停止所有 dztrader 进程（不 build 不启动）
.PARAMETER NoBuild
  跳过 build/install，直接启动
.PARAMETER Config
  Release 或 Debug（默认 Release；Debug 仅本地）
.EXAMPLE
  .\scripts\run-dev.ps1                          # build + install + 启动 master
  .\scripts\run-dev.ps1 -NoBuild                 # 直接启动
  .\scripts\run-dev.ps1 -Config Debug            # Debug（仅本地）
  .\scripts\run-dev.ps1 -Stop                    # 停止所有 dztrader 进程
#>
param(
    [switch]$Stop,

    [switch]$NoBuild,

    [ValidateSet('Release','Debug')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

$project_root = (Resolve-Path "$PSScriptRoot/..").Path
$install_dir = Join-Path $project_root "install\windows\x86_64\$Config"
$master_exe = Join-Path $install_dir "dztraderd.exe"

# DZTRADER_HOME 必须在任何 dz* 可执行文件之前设置（含 --cleanup-orphans）：
# 漏设会让进程在真实 home 下建 shm/db/configs（AGENTS.md 硬门禁 2）。
# 优先尊重调用方已设置的值：并行跑多个栈时靠它隔离。
if (-not $env:DZTRADER_HOME) {
    $env:DZTRADER_HOME = Join-Path $project_root ".dztrader_dev"
}

# -Stop: 强杀主进程 + 用 dztraderd --cleanup-orphans 清理僵尸子进程
if ($Stop) {
    # 1. 强杀主进程（Windows 下 SIGINT 无法可靠传递给 boost::asio::signal_set）
    $master_procs = Get-Process -Name "dztraderd" -ErrorAction SilentlyContinue
    if ($master_procs) {
        $master_procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host "已强制终止 master (dztraderd)" -ForegroundColor Green
    } else {
        Write-Host "未发现运行中的 dztraderd" -ForegroundColor Yellow
    }

    # 2. 用 dztraderd --cleanup-orphans 清理僵尸子进程
    #    该 CLI 会读 children.db，比对 pid+exe_path，精确清理残留子进程。
    #    路径跟随 $Config，不再硬编码 Release（此前粘性 Debug 时停不掉）。
    if (Test-Path $master_exe) {
        Write-Host "清理僵尸子进程..." -ForegroundColor Cyan
        & $master_exe --cleanup-orphans 2>&1 | Out-Host
    } else {
        Write-Host "dztraderd.exe 不存在，跳过僵尸清理 | path=$master_exe" -ForegroundColor Yellow
    }
    exit 0
}

# 默认: build → install → 启动
if (-not $NoBuild) {
    # install 会替换 install/ 下文件（web/ 是逐文件写入），运行中替换会让 dzweb
    # 读到半更新状态。先停栈 → 再 install → 再启动。
    $running = Get-Process -Name "dztraderd" -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host "检测到 master 正在运行，先停止再重建（避免 install 覆盖运行中的文件）" -ForegroundColor Yellow
        & "$PSScriptRoot\run-dev.ps1" -Stop -Config $Config
    }

    & "$PSScriptRoot\build.ps1" -Config $Config
    if ($LASTEXITCODE -ne 0) {
        Write-Error "build failed (config=$Config)"
        exit 1
    }

    Write-Host "install → $install_dir" -ForegroundColor Cyan
    $build_dir = Join-Path $project_root "build\windows\x86_64\$Config"
    & cmake --install $build_dir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "install failed (build-dir=$build_dir)"
        exit 1
    }
}

if (-not (Test-Path $master_exe)) {
    Write-Error "可执行文件不存在: $master_exe`n请先运行: .\scripts\setup.ps1 -Config $Config"
    exit 1
}

if ($Config -eq 'Debug') {
    Write-Host ""
    Write-Host "=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===" -ForegroundColor Yellow
    Write-Host ""
}

Write-Host "启动 master | config=$Config home=$env:DZTRADER_HOME exe=$master_exe" -ForegroundColor Cyan
& $master_exe
