# 校验仓库中是否残留旧测试入口命令。
# 设计目标：用于 CI 拦截「可执行配置文件」中的旧入口，避免团队继续误用。

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ThisScriptPath = $PSCommandPath
$RepoRoot = (Resolve-Path (Join-Path $ScriptRoot "..\..")).Path

# 仅扫描可能被执行/消费的配置与脚本文件。
$AllowedExtensions = @(
    ".ps1",
    ".bat",
    ".cmd",
    ".json",
    ".yml",
    ".yaml",
    ".cs"
)

# 跳过构建产物、日志与报告目录，避免噪音。
$ExcludedDirNames = @(
    ".git",
    "Binaries",
    "Intermediate",
    "DerivedDataCache",
    "Saved",
    "reports",
    "node_modules"
)

function Test-IsExcludedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FullPath
    )

    foreach ($DirName in $ExcludedDirNames) {
        $Pattern = "[\\/]" + [regex]::Escape($DirName) + "[\\/]"
        if ($FullPath -match $Pattern) {
            return $true
        }
    }

    return $false
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$FullPath
    )

    $NormalizedBase = [System.IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/')
    $NormalizedFull = [System.IO.Path]::GetFullPath($FullPath)

    if ($NormalizedFull.StartsWith($NormalizedBase, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $NormalizedFull.Substring($NormalizedBase.Length).TrimStart('\', '/')
    }

    return $NormalizedFull
}

$Patterns = @(
    @{
        Token = "-run=Automation"
        Suggestion = "改为 -run=AgentBridge（或 -ExecCmds=""Automation RunTests ...""）"
    },
    @{
        Token = "RunUAT RunAutomationTests"
        Suggestion = "改为 RunUAT BuildCookRun -run -editortest -RunAutomationTest=..."
    },
    @{
        Token = "RunUAT.bat RunAutomationTests"
        Suggestion = "改为 RunUAT.bat BuildCookRun -run -editortest -RunAutomationTest=..."
    }
)

$FilesToScan = Get-ChildItem -Path $RepoRoot -Recurse -File | Where-Object {
    $Extension = $_.Extension.ToLowerInvariant()
    ($AllowedExtensions -contains $Extension) -and
    (-not (Test-IsExcludedPath -FullPath $_.FullName)) -and
    ($_.FullName -ne $ThisScriptPath)
}

if (-not $FilesToScan -or $FilesToScan.Count -eq 0) {
    Write-Host "[WARN] 未发现可扫描文件（检查扩展名配置）"
    exit 0
}

$Violations = @()
foreach ($PatternItem in $Patterns) {
    $Matches = Select-String -Path $FilesToScan.FullName -SimpleMatch $PatternItem.Token
    foreach ($Match in $Matches) {
        $Violations += [PSCustomObject]@{
            Token = $PatternItem.Token
            File = Get-RelativePath -BasePath $RepoRoot -FullPath $Match.Path
            Line = $Match.LineNumber
            Text = $Match.Line.Trim()
            Suggestion = $PatternItem.Suggestion
        }
    }
}

if ($Violations.Count -gt 0) {
    Write-Host "[FAIL] 发现旧入口命令，请替换后再提交："
    $Violations |
        Sort-Object File, Line |
        ForEach-Object {
            Write-Host (" - {0}:{1}" -f $_.File, $_.Line)
            Write-Host ("   命中: {0}" -f $_.Token)
            Write-Host ("   内容: {0}" -f $_.Text)
            Write-Host ("   建议: {0}" -f $_.Suggestion)
        }
    exit 1
}

Write-Host "[PASS] 未发现旧入口命令（-run=Automation / RunAutomationTests）。"
exit 0


