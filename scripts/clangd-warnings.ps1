# 诊断脚本：用 clang-cl 重现 clangd 对 webui C++ 文件的警告
# 用途: clangd 报的警告与 MSVC 编译结果不一致时, 用 clang-cl -fsyntax-only
#       按 compile_commands.json 的真实编译参数复现, 逐文件统计。
# 依赖: LLVM (clang-cl)。安装: winget install LLVM.LLVM 或 scoop install llvm
$ErrorActionPreference = 'Continue'
. "$PSScriptRoot\Activate-MSVC.ps1"

# 自动定位 clang-cl: PATH -> 常见 scoop/默认安装位置
$clangcl = (Get-Command clang-cl -ErrorAction SilentlyContinue).Source
if (-not $clangcl) {
    $candidates = @(
        "$env:USERPROFILE\scoop\apps\llvm\current\bin\clang-cl.exe",
        "$env:USERPROFILE\apps\scoop\apps\llvm\current\bin\clang-cl.exe",
        'C:\Program Files\LLVM\bin\clang-cl.exe'
    )
    $clangcl = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $clangcl) { Write-Error 'clang-cl not found. Install LLVM: winget install LLVM.LLVM'; exit 1 }

$ccdb = "$PSScriptRoot\..\compile_commands.json"
if (-not (Test-Path $ccdb)) { Write-Error "compile_commands.json not found: $ccdb (先跑 setup)"; exit 1 }

$db = Get-Content $ccdb -Raw | ConvertFrom-Json
$webui = $db | Where-Object { $_.file -like '*apps\webui\*.cpp' -or $_.file -like '*apps/webui/*.cpp' }

$totalWarn = 0
foreach ($e in $webui) {
    # 替换程序路径为 clang-cl，并清洗输出相关 flags
    $cmd = $e.command
    $cmd = $cmd -replace '^\S+\\cl\.exe', $clangcl
    $tokens = $cmd -split ' '
    $clean = @()
    foreach ($t in $tokens) {
        if ($t -eq '/Zi' -or $t -eq '/FS' -or $t -eq '/WX' -or $t -eq '/nologo') { continue }
        if ($t -like '/Fo*' -or $t -like '/Fd*') { continue }
        $clean += $t
    }
    $clean += '-fsyntax-only'
    $out = & $clean[0] @($clean[1..($clean.Count - 1)]) 2>&1
    $warnings = $out | ForEach-Object { "$_" } | Where-Object { $_ -match 'warning' -and $_ -notmatch 'unused during compilation' }
    if ($warnings) {
        Write-Host "===== FILE: $($e.file) ====="
        foreach ($w in $warnings) { Write-Host "  $w"; $totalWarn++ }
    }
}
Write-Host "`nTOTAL WARNINGS: $totalWarn"
