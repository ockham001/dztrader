<#
.SYNOPSIS
    build-lock.ps1 — 被 build.ps1 / test.ps1 dot-source（不直接执行）

.DESCRIPTION
    构建锁：ninja 不锁构建树，两个并发 ninja 写同一棵树会交错写 .ninja_deps /
    .ninja_log，产出陈旧或损坏的 .o。Linux 侧用 flock（见 scripts/build-lock.sh）；
    Windows 上 shell 没有 flock，这里用 .NET 的全局命名互斥体。

    锁只在叶子脚本取（build.ps1 / test.ps1）。gate.sh 早先持锁后再调叶子脚本会
    父子互等死锁（#168 审查发现），所以 gate.sh 已移除锁。

.EXAMPLE
    . "$PSScriptRoot\build-lock.ps1"
    $lock = Get-DzBuildLock -RepoRoot $repoRoot -Label 'build'
    try { ... } finally { $lock.Dispose() }
#>

function Get-DzBuildLock {
    <#
    .SYNOPSIS
        取得该仓的构建互斥体（阻塞等待）。
    .DESCRIPTION
        互斥体名按仓库路径哈希，保证同一 worktree 串行、不同 worktree 互不影响。
        返回一个 IDisposable；必须在 finally 中 Dispose 以释放。
    #>
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [string]$Label = 'build'
    )

    # 路径规范化后再哈希：同一仓的不同写法（大小写、尾斜杠）应映射到同一互斥体
    $normalized = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/').ToLowerInvariant()
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($normalized))
    }
    finally {
        $sha.Dispose()
    }
    $key = -join ($hash[0..7] | ForEach-Object { $_.ToString('x2') })

    # Global\ 前缀：跨会话可见（同一用户的多个终端 / agent 会话）
    $mutex = [System.Threading.Mutex]::new($false, "Global\dztrader-build-$key")

    if (-not $mutex.WaitOne(0)) {
        Write-Host "waiting for build lock | label=$Label holder=`"another build/test in this worktree`"" -ForegroundColor Yellow
        try {
            $mutex.WaitOne() | Out-Null
        }
        catch [System.Threading.AbandonedMutexException] {
            # 上一个持有者异常退出（崩溃/强杀），互斥体被内核标记为放弃。
            # 此时锁已归本进程所有，继续即可。
            Write-Host "build lock was abandoned by a previous holder; continuing | label=$Label" -ForegroundColor Yellow
        }
        Write-Host "build lock acquired | label=$Label" -ForegroundColor Green
    }

    return $mutex
}
