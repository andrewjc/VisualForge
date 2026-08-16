[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot,

    [Parameter(Mandatory = $true)]
    [string]$PluginDll,

    [Parameter(Mandatory = $true)]
    [string]$ArtifactDirectory,

    [string]$BackendDll = '',

    [switch]$ExpectBackendReady,

    [int]$TimeoutSeconds = 45,

    [ValidateSet('Off', 'Observe', 'Mirror', 'Takeover', 'Native')]
    [string]$ExpectedRendererMode = 'Off',

    [ValidateSet('absent', 'loaded')]
    [string]$ExpectedRendererBackend = 'absent',

    [switch]$ExpectBridgePattern,

    [switch]$ExpectMeshCapture,

    [string]$ExpectedMeshCapturePath = '',

    [switch]$ExpectTextureCapture,

    [string]$ExpectedTextureCapturePath = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-RequiredFile([string]$Path, [string]$Description)
{
    $resolved = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Description does not exist: $resolved"
    }
    return $resolved
}

function Save-TrackedFile([string]$Path, [string]$BackupDirectory, [int]$Index)
{
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $backup = Join-Path $BackupDirectory ("{0:D2}-{1}" -f $Index, [System.IO.Path]::GetFileName($Path))
    if ($exists) {
        Copy-Item -LiteralPath $Path -Destination $backup
    }

    return [pscustomobject]@{
        Path = $Path
        Existed = $exists
        Backup = $backup
    }
}

function Restore-TrackedFile($Tracked)
{
    if ($Tracked.Existed) {
        Copy-Item -LiteralPath $Tracked.Backup -Destination $Tracked.Path -Force
    } elseif (Test-Path -LiteralPath $Tracked.Path -PathType Leaf) {
        Remove-Item -LiteralPath $Tracked.Path -Force
    }
}

function Find-StartedGameProcess([datetime]$StartedAfter)
{
    $candidates = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue)
    foreach ($candidate in $candidates) {
        try {
            if ($candidate.StartTime -ge $StartedAfter.AddSeconds(-2)) {
                return $candidate
            }
        } catch {
            # The process can exit between enumeration and reading StartTime.
        }
    }
    return $null
}

function Stop-TrackedProcess($Process, [string]$Description)
{
    if ($null -eq $Process) {
        return
    }

    try {
        if ($Process.HasExited) {
            return
        }

        $closed = $Process.CloseMainWindow()
        if ($closed -and $Process.WaitForExit(10000)) {
            return
        }

        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        $Process.WaitForExit(5000) | Out-Null
    } catch {
        Write-Warning "Failed to stop $Description process $($Process.Id): $_"
    }
}

function Stop-RunOwnedGameProcesses([datetime]$StartedAfter)
{
    # f4se_loader can exit before Steam completes its handoff. Keep watching for
    # delayed Fallout4 processes from this run so no loaded test DLL survives into
    # restoration. The preflight check guarantees there were no pre-existing game
    # processes to adopt.
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline) {
        $owned = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue | Where-Object {
            try {
                $_.StartTime -ge $StartedAfter.AddSeconds(-2)
            } catch {
                $false
            }
        })
        foreach ($process in $owned) {
            Stop-TrackedProcess $process 'run-owned Fallout4'
        }
        Start-Sleep -Milliseconds 250
    }
}

$gameRootFull = [System.IO.Path]::GetFullPath($GameRoot)
$pluginDllFull = Resolve-RequiredFile $PluginDll 'Test plugin DLL'
$backendDllFull = $null
if (-not [string]::IsNullOrWhiteSpace($BackendDll)) {
    $backendDllFull = Resolve-RequiredFile $BackendDll 'Test renderer backend DLL'
}
$loaderPath = Resolve-RequiredFile (Join-Path $gameRootFull 'f4se_loader.exe') 'F4SE loader'
$artifactFull = [System.IO.Path]::GetFullPath($ArtifactDirectory)

if (Test-Path -LiteralPath $artifactFull) {
    throw "Artifact directory already exists: $artifactFull"
}

if ($TimeoutSeconds -lt 10 -or $TimeoutSeconds -gt 60) {
    throw 'TimeoutSeconds must be between 10 and 60'
}

$preexistingGame = @(Get-Process -Name Fallout4,f4se_loader -ErrorAction SilentlyContinue)
if ($preexistingGame.Count -ne 0) {
    throw 'Fallout4 or f4se_loader is already running; refusing to adopt or stop an existing process'
}

$pluginDirectory = Join-Path $gameRootFull 'Data\F4SE\Plugins'
$installedPlugin = Join-Path $pluginDirectory 'VisualForge.dll'
$installedBackend = Join-Path $pluginDirectory 'VisualForgeRenderer.dll'
$userProfilePath = [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)
$documentsRoot = Join-Path $userProfilePath 'Documents\My Games\Fallout4'
$f4seLogRoot = Join-Path $documentsRoot 'F4SE'
$visualForgeLog = Join-Path $f4seLogRoot 'VisualForge.log'
$visualForgeCrashLog = Join-Path $f4seLogRoot 'VisualForge-crash.log'

New-Item -ItemType Directory -Path $artifactFull | Out-Null
$backupDirectory = Join-Path $artifactFull 'backup'
New-Item -ItemType Directory -Path $backupDirectory | Out-Null

$trackedPaths = @(
    $installedPlugin,
    $visualForgeLog,
    $visualForgeCrashLog,
    (Join-Path $documentsRoot 'Fallout4.ini'),
    (Join-Path $documentsRoot 'Fallout4Prefs.ini'),
    (Join-Path $documentsRoot 'Fallout4Custom.ini')
)
if ($null -ne $backendDllFull) {
    $trackedPaths += $installedBackend
}

$trackedFiles = @()
for ($index = 0; $index -lt $trackedPaths.Count; ++$index) {
    $trackedFiles += Save-TrackedFile $trackedPaths[$index] $backupDirectory $index
}

$loaderProcess = $null
$gameProcess = $null
$startTime = Get-Date
$result = [ordered]@{
    startedUtc = $startTime.ToUniversalTime().ToString('O')
    plugin = $pluginDllFull
    pluginSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $pluginDllFull).Hash
    backend = $backendDllFull
    backendSha256 = if ($null -ne $backendDllFull) {
        (Get-FileHash -Algorithm SHA256 -LiteralPath $backendDllFull).Hash
    } else { $null }
    loaderStarted = $false
    gameStarted = $false
    healthObserved = $false
    hookObserved = $false
    postObserved = $false
    backendObserved = -not $ExpectBackendReady
    bridgeObserved = -not $ExpectBridgePattern
    meshCaptureObserved = -not $ExpectMeshCapture
    textureCaptureObserved = -not $ExpectTextureCapture
    processExitedEarly = $false
    success = $false
}

try {
    Copy-Item -LiteralPath $pluginDllFull -Destination $installedPlugin -Force
    if ($null -ne $backendDllFull) {
        Copy-Item -LiteralPath $backendDllFull -Destination $installedBackend -Force
    }

    $loaderProcess = Start-Process -FilePath $loaderPath -WorkingDirectory $gameRootFull -WindowStyle Hidden -PassThru
    $result.loaderStarted = $true

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($null -eq $gameProcess) {
            $gameProcess = Find-StartedGameProcess $startTime
            if ($null -ne $gameProcess) {
                $result.gameStarted = $true
            }
        }

        if (Test-Path -LiteralPath $visualForgeLog -PathType Leaf) {
            $logText = Get-Content -LiteralPath $visualForgeLog -Raw -ErrorAction SilentlyContinue
            if ($null -ne $logText) {
                $result.healthObserved = $logText.Contains(
                    "renderer-health schema=1 mode=$ExpectedRendererMode backend=$ExpectedRendererBackend suppression=off")
                $result.hookObserved = $logText.Contains('hook: initialized')
                $result.postObserved = $logText.Contains('post: shaders compiled')
                if ($ExpectBackendReady) {
                    $result.backendObserved = $logText.Contains(
                        'renderer-backend: ready') -and $logText.Contains(
                        'validation-errors=0 unload-policy=process-lifetime')
                }
                if ($ExpectBridgePattern) {
                    $result.bridgeObserved = $logText.Contains(
                        'renderer-bridge: first-frame displayed') -and
                        $logText.Contains(
                        'validation-errors=0 suppression=off')
                }
                if ($ExpectMeshCapture) {
                    $captureLogged = $logText.Contains(
                        'renderer-mesh-capture: complete') -and
                        $logText.Contains('suppression=off')
                    $captureExists = [string]::IsNullOrWhiteSpace(
                        $ExpectedMeshCapturePath) -or
                        (Test-Path -LiteralPath $ExpectedMeshCapturePath -PathType Leaf)
                    $result.meshCaptureObserved = $captureLogged -and $captureExists
                }
                if ($ExpectTextureCapture) {
                    $textureLogged = $logText.Contains(
                        'renderer-texture-capture: complete') -and
                        $logText.Contains('suppression=off')
                    $textureExists = [string]::IsNullOrWhiteSpace(
                        $ExpectedTextureCapturePath) -or
                        (Test-Path -LiteralPath $ExpectedTextureCapturePath -PathType Leaf)
                    $result.textureCaptureObserved = $textureLogged -and $textureExists
                }
            }
        }

        if ($result.healthObserved -and $result.hookObserved -and
            $result.postObserved -and $result.backendObserved -and
            $result.meshCaptureObserved -and $result.textureCaptureObserved) {
            if (-not $result.bridgeObserved) {
                Start-Sleep -Milliseconds 500
                continue
            }
            $result.success = $true
            break
        }

        if ($null -ne $gameProcess) {
            try {
                if ($gameProcess.HasExited) {
                    $result.processExitedEarly = $true
                    break
                }
            } catch {
                $result.processExitedEarly = $true
                break
            }
        }

        Start-Sleep -Milliseconds 500
    }
} finally {
    if (Test-Path -LiteralPath $visualForgeLog -PathType Leaf) {
        Copy-Item -LiteralPath $visualForgeLog -Destination (Join-Path $artifactFull 'VisualForge-smoke.log') -Force
    }
    if (Test-Path -LiteralPath $visualForgeCrashLog -PathType Leaf) {
        Copy-Item -LiteralPath $visualForgeCrashLog -Destination (Join-Path $artifactFull 'VisualForge-smoke-crash.log') -Force
    }

    # Stop the launcher first so it cannot complete a delayed handoff after the
    # initially observed game process has been closed.
    Stop-TrackedProcess $loaderProcess 'f4se_loader'
    Stop-TrackedProcess $gameProcess 'Fallout4'
    Stop-RunOwnedGameProcesses $startTime

    $restoreErrors = @()
    foreach ($tracked in $trackedFiles) {
        try {
            Restore-TrackedFile $tracked
        } catch {
            $restoreErrors += "Failed to restore $($tracked.Path): $_"
        }
    }

    $result.completedUtc = (Get-Date).ToUniversalTime().ToString('O')
    $result.restoreErrors = $restoreErrors
    if ($restoreErrors.Count -ne 0) {
        $result.success = $false
    }
    $result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $artifactFull 'result.json') -Encoding utf8

    if ($restoreErrors.Count -ne 0) {
        throw ($restoreErrors -join [Environment]::NewLine)
    }
}

if (-not $result.success) {
    throw "Game smoke did not observe all required markers; see $artifactFull"
}

Write-Output "Game smoke passed; artifacts: $artifactFull"
