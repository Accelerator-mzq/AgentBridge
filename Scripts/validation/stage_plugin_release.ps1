# 只打包 / 只暂存 AgentBridge 插件正式文件。
# 设计目标：避免本地 reports、Saved、日志和构建缓存混入插件仓上传内容。

[CmdletBinding()]
param(
    [ValidateSet('Stage', 'Package', 'Both')]
    [string]$Mode = 'Both',

    [switch]$ResetStage,

    [switch]$DryRun,

    [switch]$KeepStagingDir,

    [string]$OutputDir,

    [string]$PackageName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptRoot '..\..')).Path
$GitTopLevel = (& git -C $RepoRoot rev-parse --show-toplevel).Trim()

function Get-NormalizedPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    return [System.IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
}

$NormalizedRepoRoot = Get-NormalizedPath -Path $RepoRoot
$NormalizedGitTopLevel = Get-NormalizedPath -Path $GitTopLevel
if ($NormalizedRepoRoot -ne $NormalizedGitTopLevel) {
    throw "当前脚本必须在 AgentBridge 插件仓根目录运行。RepoRoot=$RepoRoot, GitTopLevel=$GitTopLevel"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $RepoRoot 'Saved\Upload'
}

$TopLevelWhitelist = @(
    '.gitignore',
    'AGENTS.md',
    'README.md',
    'AgentBridge.uplugin',
    'Docs',
    'Schemas',
    'Scripts',
    'Specs',
    'Source',
    'AgentBridgeTests',
    'Gauntlet',
    'roadmap'
)

$ExcludedDirNames = @(
    '.git',
    '.vs',
    '.idea',
    'Binaries',
    'Intermediate',
    'DerivedDataCache',
    'Saved',
    'reports',
    '__pycache__',
    '.pytest_cache',
    '.mypy_cache',
    '.ruff_cache',
    '.venv',
    'venv',
    'bin',
    'obj'
)

$ExcludedRelativeDirs = @(
    'Gauntlet/bin',
    'Gauntlet/obj'
)

$ExcludedFilePatterns = @(
    '*.log',
    '*.tmp',
    '*.temp',
    '*.pyc',
    '*.pyo',
    '*.VC.db',
    '*.VC.opendb',
    '*.csproj.user',
    '*.user',
    '*.suo',
    '.coverage',
    'local_config.py',
    'project_local.env'
)

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string]$FullPath
    )

    $BaseUri = New-Object System.Uri((Get-NormalizedPath -Path $BasePath) + [System.IO.Path]::DirectorySeparatorChar)
    $FullUri = New-Object System.Uri((Get-NormalizedPath -Path $FullPath))
    $RelativeUri = $BaseUri.MakeRelativeUri($FullUri)
    return [System.Uri]::UnescapeDataString($RelativeUri.ToString()).Replace('/', '\')
}

function Test-IsExcludedRelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RelativePath,
        [Parameter(Mandatory = $true)]
        [bool]$IsDirectory
    )

    $NormalizedRelative = $RelativePath.Replace('/', '\').TrimStart('\')

    foreach ($ExcludedRelativeDir in $ExcludedRelativeDirs) {
        $NormalizedExcludedDir = $ExcludedRelativeDir.Replace('/', '\')
        if ($NormalizedRelative.Equals($NormalizedExcludedDir, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        if ($NormalizedRelative.StartsWith($NormalizedExcludedDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
    }

    $Segments = $NormalizedRelative -split '[\\/]'
    foreach ($Segment in $Segments) {
        if ([string]::IsNullOrWhiteSpace($Segment)) {
            continue
        }
        if ($ExcludedDirNames -contains $Segment) {
            return $true
        }
    }

    if (-not $IsDirectory) {
        foreach ($Pattern in $ExcludedFilePatterns) {
            if ($NormalizedRelative -like $Pattern) {
                return $true
            }
            if ((Split-Path $NormalizedRelative -Leaf) -like $Pattern) {
                return $true
            }
        }
    }

    return $false
}

function Get-UploadFileList {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Whitelist
    )

    $Files = New-Object System.Collections.Generic.List[string]

    foreach ($RelativeItem in $Whitelist) {
        $FullItemPath = Join-Path $BasePath $RelativeItem
        if (-not (Test-Path -LiteralPath $FullItemPath)) {
            Write-Host "[WARN] 白名单路径不存在，已跳过: $RelativeItem"
            continue
        }

        $Item = Get-Item -LiteralPath $FullItemPath -Force
        if ($Item.PSIsContainer) {
            if (Test-IsExcludedRelativePath -RelativePath $RelativeItem -IsDirectory $true) {
                Write-Host "[WARN] 白名单目录被排除规则命中，已跳过: $RelativeItem"
                continue
            }

            Get-ChildItem -LiteralPath $FullItemPath -Recurse -Force -File | ForEach-Object {
                $RelativeFile = Get-RelativePath -BasePath $BasePath -FullPath $_.FullName
                if (-not (Test-IsExcludedRelativePath -RelativePath $RelativeFile -IsDirectory $false)) {
                    $Files.Add($RelativeFile)
                }
            }
        }
        else {
            if (-not (Test-IsExcludedRelativePath -RelativePath $RelativeItem -IsDirectory $false)) {
                $Files.Add($RelativeItem)
            }
        }
    }

    return $Files | Sort-Object -Unique
}

function Invoke-StageMode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Whitelist,
        [Parameter(Mandatory = $true)]
        [bool]$ShouldResetStage,
        [Parameter(Mandatory = $true)]
        [bool]$PreviewOnly
    )

    if ($ShouldResetStage) {
        if ($PreviewOnly) {
            Write-Host '[DRYRUN] 将执行: git reset HEAD -- .'
        }
        else {
            & git -C $BasePath reset HEAD -- . | Out-Null
        }
    }

    foreach ($RelativeItem in $Whitelist) {
        $FullItemPath = Join-Path $BasePath $RelativeItem
        if (-not (Test-Path -LiteralPath $FullItemPath)) {
            continue
        }

        if ($PreviewOnly) {
            Write-Host "[DRYRUN] 将暂存: $RelativeItem"
        }
        else {
            & git -C $BasePath add --all -- $RelativeItem
        }
    }

    if ($PreviewOnly) {
        return @()
    }

    return (& git -C $BasePath diff --cached --name-only) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
}

function Invoke-PackageMode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Files,
        [Parameter(Mandatory = $true)]
        [string]$LocalOutputDir,
        [Parameter(Mandatory = $true)]
        [string]$ZipFileName,
        [Parameter(Mandatory = $true)]
        [bool]$PreviewOnly,
        [Parameter(Mandatory = $true)]
        [bool]$PreserveStagingDir
    )

    $Timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $StagingDir = Join-Path $LocalOutputDir ("staging_" + $Timestamp)
    $ManifestPath = Join-Path $LocalOutputDir (([System.IO.Path]::GetFileNameWithoutExtension($ZipFileName)) + '.manifest.txt')
    $ZipPath = Join-Path $LocalOutputDir $ZipFileName

    if ($PreviewOnly) {
        Write-Host "[DRYRUN] 将输出目录: $LocalOutputDir"
        Write-Host "[DRYRUN] 将生成压缩包: $ZipPath"
        Write-Host "[DRYRUN] 将生成清单: $ManifestPath"
        Write-Host ('[DRYRUN] 预计打包文件数: {0}' -f $Files.Count)
        return [PSCustomObject]@{
            ZipPath = $ZipPath
            ManifestPath = $ManifestPath
            StagingDir = $StagingDir
        }
    }

    New-Item -ItemType Directory -Force -Path $LocalOutputDir | Out-Null
    New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null

    foreach ($RelativeFile in $Files) {
        $SourcePath = Join-Path $BasePath $RelativeFile
        $DestinationPath = Join-Path $StagingDir $RelativeFile
        $DestinationDir = Split-Path -Parent $DestinationPath
        if (-not (Test-Path -LiteralPath $DestinationDir)) {
            New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
        }
        Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    }

    Set-Content -Encoding UTF8 $ManifestPath ($Files -join [Environment]::NewLine)

    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    Compress-Archive -Path (Join-Path $StagingDir '*') -DestinationPath $ZipPath -CompressionLevel Optimal

    if (-not $PreserveStagingDir) {
        $NormalizedOutputDir = Get-NormalizedPath -Path $LocalOutputDir
        $NormalizedStagingDir = Get-NormalizedPath -Path $StagingDir
        if (-not $NormalizedStagingDir.StartsWith($NormalizedOutputDir, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "StagingDir 超出 OutputDir 范围，拒绝删除: $StagingDir"
        }
        Remove-Item -LiteralPath $StagingDir -Recurse -Force
    }

    return [PSCustomObject]@{
        ZipPath = $ZipPath
        ManifestPath = $ManifestPath
        StagingDir = $StagingDir
    }
}

$UploadFiles = Get-UploadFileList -BasePath $RepoRoot -Whitelist $TopLevelWhitelist
if (-not $UploadFiles -or $UploadFiles.Count -eq 0) {
    throw '未收集到可上传文件，请检查白名单或排除规则。'
}

Write-Host ('[INFO] RepoRoot: {0}' -f $RepoRoot)
Write-Host ('[INFO] Mode: {0}' -f $Mode)
Write-Host ('[INFO] 正式文件数: {0}' -f $UploadFiles.Count)

if ([string]::IsNullOrWhiteSpace($PackageName)) {
    $PackageName = 'AgentBridge_upload_' + (Get-Date -Format 'yyyyMMdd_HHmmss') + '.zip'
}

$StageResult = @()
$PackageResult = $null

switch ($Mode) {
    'Stage' {
        $StageResult = Invoke-StageMode -BasePath $RepoRoot -Whitelist $TopLevelWhitelist -ShouldResetStage $ResetStage.IsPresent -PreviewOnly $DryRun.IsPresent
    }
    'Package' {
        $PackageResult = Invoke-PackageMode -BasePath $RepoRoot -Files $UploadFiles -LocalOutputDir $OutputDir -ZipFileName $PackageName -PreviewOnly $DryRun.IsPresent -PreserveStagingDir $KeepStagingDir.IsPresent
    }
    'Both' {
        $StageResult = Invoke-StageMode -BasePath $RepoRoot -Whitelist $TopLevelWhitelist -ShouldResetStage $ResetStage.IsPresent -PreviewOnly $DryRun.IsPresent
        $PackageResult = Invoke-PackageMode -BasePath $RepoRoot -Files $UploadFiles -LocalOutputDir $OutputDir -ZipFileName $PackageName -PreviewOnly $DryRun.IsPresent -PreserveStagingDir $KeepStagingDir.IsPresent
    }
}

if ($Mode -in @('Stage', 'Both')) {
    if ($DryRun) {
        Write-Host '[DONE] Stage dry-run 已完成。'
    }
    else {
        Write-Host ('[DONE] 已暂存文件数: {0}' -f $StageResult.Count)
        $StageResult | ForEach-Object { Write-Host ('  + {0}' -f $_) }
    }
}

if ($Mode -in @('Package', 'Both')) {
    if ($DryRun) {
        Write-Host ('[DONE] Package dry-run 已完成，目标压缩包: {0}' -f $PackageResult.ZipPath)
    }
    else {
        Write-Host ('[DONE] 已生成压缩包: {0}' -f $PackageResult.ZipPath)
        Write-Host ('[DONE] 已生成清单: {0}' -f $PackageResult.ManifestPath)
    }
}
