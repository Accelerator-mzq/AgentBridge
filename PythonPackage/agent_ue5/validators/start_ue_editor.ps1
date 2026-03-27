[CmdletBinding()]
param(
    [string]$EngineRoot = "",
    [string]$ProjectPath = "",
    [string]$RcUrl = "http://localhost:30010/remote/info",
    [int]$RcTimeoutSec = 300,
    [switch]$ForceCloseExisting,
    [switch]$ClearPackageRestoreData,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"

function Get-UnrealEditorProcessInfo {
    # 读取进程标题和命令行，便于识别项目浏览器与目标工程实例。
    $cimByPid = @{}
    Get-CimInstance Win32_Process |
        Where-Object { $_.Name -like "UnrealEditor*" } |
        ForEach-Object {
            $cimByPid[[int]$_.ProcessId] = $_
        }

    $processList = Get-Process -ErrorAction SilentlyContinue |
        Where-Object { $_.ProcessName -like "UnrealEditor*" }

    foreach ($proc in $processList) {
        $cim = $cimByPid[[int]$proc.Id]
        [PSCustomObject]@{
            ProcessId       = [int]$proc.Id
            ProcessName     = $proc.ProcessName
            MainWindowTitle = $proc.MainWindowTitle
            CommandLine     = if ($cim) { $cim.CommandLine } else { "" }
        }
    }
}

function Test-IsProjectBrowser {
    param(
        [string]$WindowTitle
    )

    if ([string]::IsNullOrWhiteSpace($WindowTitle)) {
        return $false
    }

    return (
        $WindowTitle -like "*项目浏览器*" -or
        $WindowTitle -like "*Project Browser*" -or
        $WindowTitle -like "*虚幻项目浏览器*"
    )
}

function Test-RcReady {
    param(
        [string]$Url
    )

    try {
        $response = Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 5
        return [PSCustomObject]@{
            Ready      = $true
            StatusCode = [int]$response.StatusCode
            Body       = [string]$response.Content
            Error      = ""
        }
    }
    catch {
        return [PSCustomObject]@{
            Ready      = $false
            StatusCode = 0
            Body       = ""
            Error      = $_.Exception.Message
        }
    }
}

function Wait-RcReady {
    param(
        [int]$TimeoutSec,
        [string]$Url,
        [int]$ExpectedProcessId
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSec)

    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 5

        $editorProcess = Get-Process -Id $ExpectedProcessId -ErrorAction SilentlyContinue
        if (-not $editorProcess) {
            throw "UnrealEditor 进程已退出，RC API 未就绪。"
        }

        if (Test-IsProjectBrowser -WindowTitle $editorProcess.MainWindowTitle) {
            throw "检测到 UnrealEditor 停在项目浏览器页面，未真正进入 .uproject。"
        }

        $rcState = Test-RcReady -Url $Url
        if ($rcState.Ready) {
            return $rcState
        }
    }

    throw "等待 RC API 超时（${TimeoutSec}s），地址：$Url"
}

function Close-UnrealEditorProcesses {
    param(
        [array]$ProcessInfos
    )

    foreach ($info in $ProcessInfos) {
        Write-Host "[UE-Start] Closing UnrealEditor PID=$($info.ProcessId) Title='$($info.MainWindowTitle)'" -ForegroundColor Yellow
        Stop-Process -Id $info.ProcessId -Force -ErrorAction Stop
    }

    Start-Sleep -Seconds 2
}

function Clear-PackageRestoreArtifacts {
    param(
        [string]$ResolvedProjectPath
    )

    $projectRoot = Split-Path $ResolvedProjectPath -Parent
    $autosaveRoot = Join-Path $projectRoot "Saved\\Autosaves"
    $tempAutosaveRoot = Join-Path $autosaveRoot "Temp"
    $targets = New-Object System.Collections.Generic.List[string]

    $packageRestoreData = Join-Path $autosaveRoot "PackageRestoreData.json"
    if (Test-Path $packageRestoreData) {
        $targets.Add($packageRestoreData)
    }

    if (Test-Path $tempAutosaveRoot) {
        Get-ChildItem -LiteralPath $tempAutosaveRoot -Filter "Untitled_*_Auto*.umap" -ErrorAction SilentlyContinue |
            ForEach-Object {
                $targets.Add($_.FullName)
            }
    }

    if ($targets.Count -eq 0) {
        Write-Host "[UE-Start] No package-restore artifacts found." -ForegroundColor DarkGray
        return
    }

    foreach ($target in $targets) {
        if (Test-Path $target) {
            Remove-Item -LiteralPath $target -Force -ErrorAction Stop
            Write-Host "[UE-Start] Cleared package-restore artifact: $target" -ForegroundColor Yellow
        }
    }
}

function Resolve-DefaultEngineRoot {
    # 优先使用环境变量，再尝试常见安装路径。
    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($env:UE_ENGINE_ROOT)) {
        $candidates += $env:UE_ENGINE_ROOT
    }

    $candidates += @(
        "E:\\Epic Games\\UE_5.5",
        "D:\\Epic Games\\UE_5.5",
        "C:\\Program Files\\Epic Games\\UE_5.5"
    )

    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }

        $editorExe = Join-Path $candidate "Engine\\Binaries\\Win64\\UnrealEditor.exe"
        if (Test-Path $editorExe) {
            return (Resolve-Path $candidate).Path
        }
    }

    return ""
}

function Find-NearestUProject {
    param(
        [string]$StartPath = (Get-Location).Path
    )

    $resolved = Resolve-Path $StartPath -ErrorAction Stop
    $currentPath = if ((Get-Item -LiteralPath $resolved.Path).PSIsContainer) {
        $resolved.Path
    }
    else {
        Split-Path $resolved.Path -Parent
    }

    while (-not [string]::IsNullOrWhiteSpace($currentPath)) {
        $match = Get-ChildItem -LiteralPath $currentPath -Filter *.uproject -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($match) {
            return $match.FullName
        }

        $parent = Split-Path $currentPath -Parent
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $currentPath) {
            break
        }
        $currentPath = $parent
    }

    return ""
}


# 优先使用显式参数，其次使用环境变量和默认检测。
if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    $EngineRoot = Resolve-DefaultEngineRoot
}
else {
    $EngineRoot = (Resolve-Path $EngineRoot).Path
}

if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    throw "未找到有效 EngineRoot。请通过 -EngineRoot 或环境变量 UE_ENGINE_ROOT 指定。"
}

if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    $ProjectPath = Find-NearestUProject
}
else {
    $ProjectPath = (Resolve-Path $ProjectPath).Path
}

if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
    throw "未找到 .uproject。请通过 -ProjectPath 显式指定，或在项目目录内执行本脚本。"
}

$resolvedProjectPath = (Resolve-Path $ProjectPath).Path
$editorExe = Join-Path $EngineRoot "Engine\\Binaries\\Win64\\UnrealEditor.exe"

if (-not (Test-Path $editorExe)) {
    throw "UnrealEditor.exe 不存在：$editorExe"
}

if (-not (Test-Path $resolvedProjectPath)) {
    throw ".uproject 不存在：$resolvedProjectPath"
}

$quotedProjectArgument = '"' + $resolvedProjectPath + '"'
$existingEditors = @(Get-UnrealEditorProcessInfo)
$projectBrowserEditors = @(
    $existingEditors | Where-Object {
        Test-IsProjectBrowser -WindowTitle $_.MainWindowTitle
    }
)
$matchingEditors = @(
    $existingEditors | Where-Object {
        $_.CommandLine -like "*$resolvedProjectPath*" -and
        -not (Test-IsProjectBrowser -WindowTitle $_.MainWindowTitle)
    }
)

if ($ValidateOnly) {
    Write-Host "[UE-Start] ValidateOnly=TRUE" -ForegroundColor Cyan
    Write-Host "[UE-Start] EditorExe=$editorExe"
    Write-Host "[UE-Start] ProjectPath=$resolvedProjectPath"
    Write-Host "[UE-Start] QuotedArgument=$quotedProjectArgument"
    Write-Host "[UE-Start] RcUrl=$RcUrl"
    Write-Host "[UE-Start] ExistingEditorCount=$($existingEditors.Count)"

    foreach ($info in $existingEditors) {
        Write-Host "[UE-Start] Existing PID=$($info.ProcessId) Title='$($info.MainWindowTitle)'"
        Write-Host "[UE-Start] Existing CommandLine=$($info.CommandLine)"
    }

    exit 0
}

if ($ClearPackageRestoreData) {
    Clear-PackageRestoreArtifacts -ResolvedProjectPath $resolvedProjectPath
}

if ($existingEditors.Count -gt 0) {
    if ($ForceCloseExisting) {
        Close-UnrealEditorProcesses -ProcessInfos $existingEditors
        $existingEditors = @()
        $projectBrowserEditors = @()
        $matchingEditors = @()
    }
    elseif ($projectBrowserEditors.Count -gt 0) {
        throw "检测到 UnrealEditor 当前停在项目浏览器页面。请关闭该进程，或使用 -ForceCloseExisting 重新拉起项目。"
    }
    elseif ($matchingEditors.Count -gt 0) {
        Write-Host "[UE-Start] 目标项目对应的 UnrealEditor 已在运行，开始检查 RC API..." -ForegroundColor Yellow
        $existingRcState = Test-RcReady -Url $RcUrl
        if ($existingRcState.Ready) {
            Write-Host "[UE-Start] RC API 已就绪，无需重复启动。" -ForegroundColor Green
            exit 0
        }

        throw "目标项目进程已存在，但 RC API 尚未就绪。为避免重复拉起，请稍后重试，或使用 -ForceCloseExisting。"
    }
    else {
        throw "检测到其他 UnrealEditor 进程正在运行。为避免误关用户会话，本脚本默认不再启动第二个实例。可改用 -ForceCloseExisting。"
    }
}

Write-Host "[UE-Start] Launching: $editorExe $quotedProjectArgument" -ForegroundColor Cyan
$launchedProcess = Start-Process -FilePath $editorExe -ArgumentList $quotedProjectArgument -WorkingDirectory (Split-Path $resolvedProjectPath -Parent) -PassThru

$rcState = Wait-RcReady -TimeoutSec $RcTimeoutSec -Url $RcUrl -ExpectedProcessId $launchedProcess.Id

Write-Host "[UE-Start] UnrealEditor started successfully." -ForegroundColor Green
Write-Host "[UE-Start] PID=$($launchedProcess.Id)"
Write-Host "[UE-Start] RC StatusCode=$($rcState.StatusCode)"
Write-Host "[UE-Start] RC Url=$RcUrl"
