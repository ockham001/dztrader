<#
.SYNOPSIS
  调试启动脚本：build → install → 从 install 目录启动 master

.DESCRIPTION
  为什么先 install 再启动（#168）：
    build/ 是中间产物目录，二进制在那里会被下次构建重链；Windows 上这会导致
    LNK1168（文件被运行中的进程占用）。install/ 是唯一产物源：运行、部署、
    打包同源。

  DZTRADER_HOME 默认 <repo>\.dztrader_dev；已设置则尊重调用方。

  启动前比对 install\<plat>\<arch>\<cfg>\BUILD_INFO 与当前 HEAD（#515）：不一致或缺失
  时打 warning（含两侧 commit 与 built_at）并提示重建，**不阻断启动**——旧产物仍可启动，
  但「跑的是哪一版」必须可见。校验逻辑在 scripts\check_build_info.py（与 run-dev.sh 共用）。

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

# 构建身份校验（#515）：install 树里的二进制/前端是否对应当前 HEAD。
# 背景（#508 关单时实测）：产物原先没有任何 commit 标识，只能靠 mtime/hash 手工推断，
# 结论是宿主栈跑着旧二进制、前端静态文件却已被刷新——新旧混用难以解释。
# 只 warning 不阻断（#515 AC2）：旧产物仍可启动，但「跑的是哪一版」必须可见。
# 与 run-dev.sh 同一份判定实现（scripts/check_build_info.py），行为跨平台一致。
function Test-DzBuildIdentity {
    $py = $null
    foreach ($candidate in @('python3', 'python', 'py')) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { $py = $cmd.Source; break }
    }
    if (-not $py) {
        Write-Host "warning: no python interpreter; build identity check skipped" -ForegroundColor Yellow
        return
    }
    $infoPath = Join-Path $install_dir "BUILD_INFO"
    & $py (Join-Path $project_root "scripts\check_build_info.py") check `
        --info $infoPath --repo $project_root
    # 结论行（ok/stale/missing/unknown）与 warning 均直接落到控制台：
    # 这份输出就是「启动栈含新二进制」的取证。
    if ($LASTEXITCODE -ne 0) {
        Write-Host "warning: install tree does not match HEAD; the running stack may be stale" -ForegroundColor Yellow
    }
    # 校验永不阻断启动（#515 AC2）：check_build_info 的退出码只用于日志分类。
    $global:LASTEXITCODE = 0
}

Test-DzBuildIdentity

if ($Config -eq 'Debug') {
    Write-Host ""
    Write-Host "=== DEBUG BUILD - 仅本地调试，禁止部署生产 ===" -ForegroundColor Yellow
    Write-Host ""
}

Write-Host "启动 master | config=$Config home=$env:DZTRADER_HOME exe=$master_exe" -ForegroundColor Cyan
& $master_exe
