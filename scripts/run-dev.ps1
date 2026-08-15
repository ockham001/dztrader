<#
.SYNOPSIS
  调试启动脚本，自动设置 DZTRADER_HOME 指向项目内 .dztrader_dev
  默认: build + 启动 master（拉起 dzweb 等子进程）

  config 来源优先级: -Config 参数 > .dztrader_dev/.config 粘性文件 > Release
.PARAMETER Stop
  停止所有 dztrader 进程（不 build 不启动）
.PARAMETER NoBuild
  跳过 build，直接启动（默认会先增量 build）
.PARAMETER Config
  Release 或 Debug（默认读粘性 config，无则 Release）
.EXAMPLE
  .\scripts\run-dev.ps1                          # build + 启动 master
  .\scripts\run-dev.ps1 -NoBuild                 # 不 build，直接启动
  .\scripts\run-dev.ps1 -Config Debug            # 临时用 Debug
  .\scripts\run-dev.ps1 -Stop                    # 停止所有 dztrader 进程
#>
param(
    [switch]$Stop,

    [switch]$NoBuild,

    [ValidateSet('Release','Debug')]
    [string]$Config
)

$project_root = Resolve-Path "$PSScriptRoot/.."
$env:DZTRADER_HOME = "$project_root\.dztrader_dev"

# 读粘性 config，-Config 优先覆盖
if (-not $Config) {
    $stickyFile = "$project_root\.dztrader_dev\.config"
    if (Test-Path $stickyFile) {
        $Config = (Get-Content $stickyFile -Raw).Trim()
    }
    if (-not $Config) { $Config = 'Release' }
}

# -Stop: 强杀主进程 + 用 dztraderd --cleanup-orphans 清理僵尸子进程
if ($Stop) {
    $master_exe = Join-Path $project_root "build\windows\x86_64\Release\dztraderd.exe"

    # 1. 强杀主进程（Windows 下 SIGINT 无法可靠传递给 boost::asio::signal_set）
    $master_procs = Get-Process -Name "dztraderd" -ErrorAction SilentlyContinue
    if ($master_procs) {
        $master_procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Write-Host "已强制终止 master (dztraderd)" -ForegroundColor Green
    } else {
        Write-Host "未发现运行中的 dztraderd" -ForegroundColor Yellow
    }

    # 2. 用 dztraderd --cleanup-orphans 清理僵尸子进程
    #    该 CLI 会读 children.db，比对 pid+exe_path，精确清理残留子进程
    if (Test-Path $master_exe) {
        Write-Host "清理僵尸子进程..." -ForegroundColor Cyan
        & $master_exe --cleanup-orphans 2>&1 | Out-Host
    } else {
        Write-Host "dztraderd.exe 不存在，跳过僵尸清理" -ForegroundColor Yellow
    }
    exit 0
}

# 默认: 先增量 build，再启动 master
if (-not $NoBuild) {
    & "$PSScriptRoot\build.ps1" -Config $Config
    if ($LASTEXITCODE -ne 0) {
        Write-Error "build failed (config=$Config)"
        exit 1
    }
}

# Debug 警告
if ($Config -eq 'Debug') {
    Write-Host ""
    Write-Host "=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===" -ForegroundColor Yellow
    Write-Host ""
}

$exe = "build\windows\x86_64\$Config\dztraderd.exe"
$exe_path = Join-Path $project_root $exe
if (-not (Test-Path $exe_path)) {
    Write-Error "可执行文件不存在: $exe_path`n请先运行: .\scripts\setup.ps1 -Config $Config"
    exit 1
}
Write-Host "启动 master (config=$Config, DZTRADER_HOME=$env:DZTRADER_HOME)" -ForegroundColor Cyan
& $exe_path
