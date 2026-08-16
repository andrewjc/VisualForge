<#
.SYNOPSIS
Drives Fallout 4 automatically and collects live renderer captures.

.DESCRIPTION
Installs the built plugin, forces a small windowed display, disables
autosaving, launches the game through F4SE, uses the in-game console to
travel into a world cell, arms one capture at a time through the plugin's
capture-request file, screenshots the window as visual evidence, and then
quits the game with the console. Every file it touches is backed up first and
restored afterwards, and it refuses to adopt a game process it did not start.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameRoot,
    [Parameter(Mandatory = $true)][string]$PluginDll,
    [Parameter(Mandatory = $true)][string]$ArtifactDirectory,
    [string]$BackendDll = '',
    [string]$CellCommand = 'coc SanctuaryExt',
    # Turns the player by a large, unmistakable amount. Rotation is the
    # discriminator because the engine's view matrices are camera-relative
    # and therefore always carry a zero translation.
    [string]$MoveCommand = 'player.setangle z 90',
    # Armed before travel so the cell load's creation traffic feeds the
    # one-shot capture hooks.
    [string[]]$PreTravelCaptures = @('texture', 'mesh'),
    # Armed after travel because they sample a presented frame.
    [string[]]$PostTravelCaptures = @('trace'),
    [int]$MenuTimeoutSeconds = 180,
    [int]$AttractScreenSeconds = 20,
    # The attract screen ignores input until it is ready, so one press is not
    # enough to guarantee the main menu is reached.
    [int]$AttractKeyPresses = 8,
    # Hands menu navigation to the operator. Synthetic input depends on the
    # game holding the foreground, which is unreliable on a machine in use.
    # The harness waits until a camera capture actually succeeds, which is the
    # only signal that proves a world is loaded rather than a menu, then does
    # the rest automatically.
    [switch]$ManualWorldEntry,
    [int]$ManualWorldEntrySeconds = 300,
    # Written to Fallout4Custom.ini as sStartingConsoleCommand:General. The
    # engine executes it itself at startup, so no key is ever injected and the
    # attract screen never has to be dismissed by the harness.
    [string]$StartingConsoleCommand = '',
    # How long to wait for the engine to run that command and load a world,
    # proven by a camera capture actually finding a camera.
    [int]$StartupWorldSeconds = 240,
    # The loading screen has its own camera — a model viewer with a narrow FOV
    # — so the first camera that appears is not the player's. Settling after
    # detection is what stops the experiment comparing two loading screens.
    # The DXGI adapter index the engine should use. Negative leaves whatever
    # the machine already has, which is right whenever one adapter is the only
    # candidate.
    [int]$AdapterIndex = -1,
    [int]$WorldSettleSeconds = 75,
    [int]$LoadSettleSeconds = 75,
    [int]$CaptureTimeoutSeconds = 90,
    [int]$Width = 1280,
    [int]$Height = 720,
    # Loads the Vulkan backend inside the game and lets it composite a
    # Vulkan-rendered image into the live swapchain. Without this the plugin
    # only observes and every pixel on screen is the game's own renderer.
    [switch]$EnableBackend,
    [switch]$EnableBridgePattern,
    # Renders a scene with the Vulkan backend driven by the live engine world
    # camera and presents it through the bridge. Unlike the bridge pattern,
    # what reaches the screen is produced by the replacement renderer rather
    # than a fixed test image.
    [switch]$EnableMirror
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

if (-not ('Vf.Native' -as [type])) {
    Add-Type -Namespace 'Vf' -Name 'Native' -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)]
public struct INPUT { public uint type; public KEYBDINPUT ki; public int pad1; public int pad2; }
[DllImport("user32.dll", SetLastError = true)]
public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
[DllImport("user32.dll")]
public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")]
public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")]
public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr processId);
[DllImport("user32.dll")]
public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
[DllImport("kernel32.dll")]
public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")]
public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")]
public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
[DllImport("user32.dll")]
public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
[StructLayout(LayoutKind.Sequential)]
public struct POINT { public int X; public int Y; }
'@ | Out-Null
}

function Send-VfInput($Structures)
{
    $typed = [Vf.Native+INPUT[]]$Structures
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf($typed[0])
    [Vf.Native]::SendInput([uint32]$typed.Length, $typed, $size) | Out-Null
}

function Send-VfScanKey([int]$Scan, [int]$HoldMilliseconds = 40)
{
    # Fallout 4 reads scan codes, so virtual-key-only injection is ignored.
    $down = New-Object 'Vf.Native+INPUT'
    $down.type = 1
    $down.ki.wScan = [uint16]$Scan
    $down.ki.dwFlags = 0x0008 # KEYEVENTF_SCANCODE
    $up = New-Object 'Vf.Native+INPUT'
    $up.type = 1
    $up.ki.wScan = [uint16]$Scan
    $up.ki.dwFlags = 0x0008 -bor 0x0002 # SCANCODE | KEYUP
    Send-VfInput @($down)
    Start-Sleep -Milliseconds $HoldMilliseconds
    Send-VfInput @($up)
    Start-Sleep -Milliseconds $HoldMilliseconds
}

function Send-VfText([string]$Text)
{
    foreach ($character in $Text.ToCharArray()) {
        $down = New-Object 'Vf.Native+INPUT'
        $down.type = 1
        $down.ki.wScan = [uint16][char]$character
        $down.ki.dwFlags = 0x0004 # KEYEVENTF_UNICODE
        $up = New-Object 'Vf.Native+INPUT'
        $up.type = 1
        $up.ki.wScan = [uint16][char]$character
        $up.ki.dwFlags = 0x0004 -bor 0x0002
        Send-VfInput @($down)
        Start-Sleep -Milliseconds 15
        Send-VfInput @($up)
        Start-Sleep -Milliseconds 15
    }
}

# Injected input goes to whatever window owns the foreground, so every send
# is gated on the game actually being focused. Refusing is far better than
# typing console commands into someone else's window.
# A hung window still has a handle, and AttachThreadInput against a stalled
# input queue can block the caller indefinitely. Every foreground, input, and
# screenshot path has to check this first or the harness hangs with the game
# and never reaches its own cleanup.
function Test-VfGameResponsive([System.Diagnostics.Process]$Game)
{
    if ($null -eq $Game) { return $false }
    try {
        $Game.Refresh()
        if ($Game.HasExited) { return $false }
        return $Game.Responding
    } catch { return $false }
}

function Confirm-VfForeground([System.Diagnostics.Process]$Game)
{
    if (-not (Test-VfGameResponsive $Game)) { return $false }
    $handle = $Game.MainWindowHandle
    if ($handle -eq [IntPtr]::Zero) { return $false }
    for ($attempt = 0; $attempt -lt 5; ++$attempt) {
        if ([Vf.Native]::GetForegroundWindow() -eq $handle) { return $true }
        [Vf.Native]::ShowWindow($handle, 9) | Out-Null   # SW_RESTORE
        [Vf.Native]::BringWindowToTop($handle) | Out-Null
        $target = [Vf.Native]::GetWindowThreadProcessId($handle, [IntPtr]::Zero)
        $self = [Vf.Native]::GetCurrentThreadId()
        [Vf.Native]::AttachThreadInput($self, $target, $true) | Out-Null
        [Vf.Native]::SetForegroundWindow($handle) | Out-Null
        [Vf.Native]::AttachThreadInput($self, $target, $false) | Out-Null
        Start-Sleep -Milliseconds 600
    }
    return ([Vf.Native]::GetForegroundWindow() -eq $handle)
}

function Invoke-VfKeyPress([System.Diagnostics.Process]$Game, [int]$Scan)
{
    if (-not (Confirm-VfForeground $Game)) { return $false }
    Send-VfScanKey $Scan
    return $true
}

function Invoke-VfConsoleCommand([System.Diagnostics.Process]$Game, [string]$Command)
{
    if (-not (Confirm-VfForeground $Game)) { return $false }
    Start-Sleep -Milliseconds 500
    Send-VfScanKey 0x29                              # grave/tilde opens console
    Start-Sleep -Milliseconds 700
    Send-VfText $Command
    Start-Sleep -Milliseconds 400
    Send-VfScanKey 0x1C                              # Enter
    Start-Sleep -Milliseconds 700
    Send-VfScanKey 0x29                              # close console
    Start-Sleep -Milliseconds 400
    return $true
}

. $PSScriptRoot/VfImage.ps1


function Save-VfWindowImage([System.Diagnostics.Process]$Game, [string]$Path)
{
    try {
        $handle = $Game.MainWindowHandle
        if ($handle -eq [IntPtr]::Zero) { return $false }
        # This is a screen scrape of the game's client rectangle, so anything
        # covering that rectangle would be captured instead of the game. A
        # screenshot of another application recorded as run evidence is worse
        # than no screenshot, so refuse unless the game is actually in front.
        if (-not (Confirm-VfForeground $Game)) { return $false }
        $rect = New-Object 'Vf.Native+RECT'
        if (-not [Vf.Native]::GetClientRect($handle, [ref]$rect)) { return $false }
        $origin = New-Object 'Vf.Native+POINT'
        if (-not [Vf.Native]::ClientToScreen($handle, [ref]$origin)) { return $false }
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        if ($width -le 0 -or $height -le 0) { return $false }
        $bitmap = New-Object System.Drawing.Bitmap($width, $height)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
        $graphics.Dispose()
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
        return $true
    } catch {
        return $false
    }
}

function Wait-VfLogMarker([string]$LogPath, [string]$Pattern, [int]$TimeoutSeconds, [System.Diagnostics.Process]$Game)
{
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    # A cell load legitimately pumps no messages for a while, so brief
    # unresponsiveness is normal and must not abort the wait. A game that
    # stays unresponsive this long is hung, not loading.
    $unresponsiveLimit = [TimeSpan]::FromSeconds(180)
    $unresponsiveSince = $null
    while ((Get-Date) -lt $deadline) {
        if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
            $text = Get-Content -LiteralPath $LogPath -Raw -ErrorAction SilentlyContinue
            if ($null -ne $text -and $text -match $Pattern) { return $true }
        }
        if ($null -ne $Game) {
            try { if ($Game.HasExited) { return $false } } catch { return $false }
            if (Test-VfGameResponsive $Game) {
                $unresponsiveSince = $null
            } else {
                if ($null -eq $unresponsiveSince) {
                    $unresponsiveSince = Get-Date
                } elseif (((Get-Date) - $unresponsiveSince) -gt $unresponsiveLimit) {
                    Write-Warning ('Game stopped responding for over ' +
                        "$($unresponsiveLimit.TotalSeconds)s; abandoning wait.")
                    return $false
                }
            }
        }
        Start-Sleep -Milliseconds 750
    }
    return $false
}

function Save-VfTrackedFile([string]$Path, [string]$BackupDirectory, [int]$Index)
{
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $backup = Join-Path $BackupDirectory ("{0:D2}-{1}" -f $Index, [System.IO.Path]::GetFileName($Path))
    if ($exists) { Copy-Item -LiteralPath $Path -Destination $backup -Force }
    return [pscustomobject]@{ Path = $Path; Existed = $exists; Backup = $backup }
}

function Restore-VfTrackedFile($Tracked)
{
    if ($Tracked.Existed) {
        Copy-Item -LiteralPath $Tracked.Backup -Destination $Tracked.Path -Force
    } elseif (Test-Path -LiteralPath $Tracked.Path -PathType Leaf) {
        Remove-Item -LiteralPath $Tracked.Path -Force
    }
}

# Where an unfinished run leaves its restore instructions. A `finally` block
# does not run when the process is killed, and a run that is interrupted after
# it has edited the game's INIs otherwise leaves them edited forever -- which
# is exactly what happened: `sStartingConsoleCommand` and the disabled
# autosaves survived into the player's real configuration, and every later run
# then backed the polluted file up as though it were the original. The journal
# below makes the repair survive the process rather than depend on it.
$script:VfPendingRestorePath = Join-Path $env:LOCALAPPDATA 'VisualForge\restore-pending.json'

function Write-VfPendingRestore($Tracked)
{
    $directory = Split-Path -Parent $script:VfPendingRestorePath
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    # Written before the first edit, so an interruption at any point after this
    # is recoverable.
    $Tracked | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath $script:VfPendingRestorePath -Encoding utf8
}

function Clear-VfPendingRestore
{
    if (Test-Path -LiteralPath $script:VfPendingRestorePath -PathType Leaf) {
        Remove-Item -LiteralPath $script:VfPendingRestorePath -Force
    }
}

# Completes the restore an earlier run did not finish. Called before this run
# touches anything, so a backup taken now records the player's own file rather
# than the previous run's leftovers.
function Complete-VfPendingRestore
{
    if (-not (Test-Path -LiteralPath $script:VfPendingRestorePath -PathType Leaf)) {
        return
    }
    Write-Host 'An earlier run did not finish restoring; completing it first.'
    $pending = Get-Content -LiteralPath $script:VfPendingRestorePath -Raw |
        ConvertFrom-Json
    $failures = @()
    foreach ($tracked in @($pending)) {
        if (-not (Test-Path -LiteralPath $tracked.Backup -PathType Leaf) -and
            $tracked.Existed) {
            # The backup is gone, so restoring would delete a file the player
            # still has. Reported rather than guessed at.
            $failures += "Backup missing for $($tracked.Path)"
            continue
        }
        try { Restore-VfTrackedFile $tracked }
        catch { $failures += "Failed to restore $($tracked.Path): $_" }
    }
    if ($failures.Count -ne 0) {
        throw (($failures + 'Resolve these before running again.') -join
            [Environment]::NewLine)
    }
    Clear-VfPendingRestore
    Write-Host 'Previous run restored.'
}

function Set-VfIniValue([string]$Path, [string]$Section, [string]$Key, [string]$Value)
{
    $lines = New-Object System.Collections.ArrayList
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        foreach ($line in @(Get-Content -LiteralPath $Path)) { [void]$lines.Add($line) }
    }
    $sectionIndex = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i].Trim() -eq "[$Section]") { $sectionIndex = $i; break }
    }
    if ($sectionIndex -lt 0) {
        [void]$lines.Add("[$Section]")
        [void]$lines.Add("$Key=$Value")
        Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
        return
    }
    for ($i = $sectionIndex + 1; $i -lt $lines.Count; ++$i) {
        $line = $lines[$i].Trim()
        if ($line.StartsWith('[')) { break }
        if ($line -match "^\s*$([regex]::Escape($Key))\s*=") {
            $lines[$i] = "$Key=$Value"
            Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
            return
        }
    }
    $lines.Insert($sectionIndex + 1, "$Key=$Value")
    Set-Content -LiteralPath $Path -Value $lines -Encoding utf8
}

$gameRootFull = [System.IO.Path]::GetFullPath($GameRoot)
$pluginDllFull = [System.IO.Path]::GetFullPath($PluginDll)
if (-not (Test-Path -LiteralPath $pluginDllFull -PathType Leaf)) { throw "Plugin missing: $pluginDllFull" }
$backendDllFull = if ([string]::IsNullOrWhiteSpace($BackendDll)) { $null } else { [System.IO.Path]::GetFullPath($BackendDll) }
$loaderPath = Join-Path $gameRootFull 'f4se_loader.exe'
if (-not (Test-Path -LiteralPath $loaderPath -PathType Leaf)) { throw "F4SE loader missing: $loaderPath" }
$artifactFull = [System.IO.Path]::GetFullPath($ArtifactDirectory)
if (Test-Path -LiteralPath $artifactFull) { throw "Artifact directory already exists: $artifactFull" }

$preexisting = @(Get-Process -Name Fallout4, f4se_loader -ErrorAction SilentlyContinue)
if ($preexisting.Count -ne 0) {
    throw 'Fallout4 or f4se_loader is already running; refusing to adopt or stop an existing process'
}

$documentsRoot = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Documents\My Games\Fallout4'
$f4seLogRoot = Join-Path $documentsRoot 'F4SE'
$visualForgeLog = Join-Path $f4seLogRoot 'VisualForge.log'
$visualForgeCrashLog = Join-Path $f4seLogRoot 'VisualForge-crash.log'
$prefsIni = Join-Path $documentsRoot 'Fallout4Prefs.ini'
$customIni = Join-Path $documentsRoot 'Fallout4Custom.ini'
$pluginDirectory = Join-Path $gameRootFull 'Data\F4SE\Plugins'
$installedPlugin = Join-Path $pluginDirectory 'VisualForge.dll'
$installedBackend = Join-Path $pluginDirectory 'VisualForgeRenderer.dll'

New-Item -ItemType Directory -Path $artifactFull | Out-Null
$backupDirectory = Join-Path $artifactFull 'backup'
New-Item -ItemType Directory -Path $backupDirectory | Out-Null
$captureDirectory = Join-Path $artifactFull 'captures'
New-Item -ItemType Directory -Path $captureDirectory | Out-Null

$trackedPaths = @($installedPlugin, $visualForgeLog, $visualForgeCrashLog, $prefsIni, $customIni,
    (Join-Path $documentsRoot 'Fallout4.ini'))
if ($null -ne $backendDllFull) { $trackedPaths += $installedBackend }
# Before this run backs anything up. A backup taken over an unfinished run's
# leftovers records those leftovers as the original, and every run after that
# faithfully restores them.
Complete-VfPendingRestore

$trackedFiles = @()
for ($index = 0; $index -lt $trackedPaths.Count; ++$index) {
    $trackedFiles += Save-VfTrackedFile $trackedPaths[$index] $backupDirectory $index
}

# Recorded before the first edit, so an interruption at any later point leaves
# instructions the next run can act on.
Write-VfPendingRestore $trackedFiles

# A Steam repair during a run silently removed F4SE once. Fingerprint the
# files a run depends on but must never modify, so any change is reported
# instead of being discovered days later.
function Get-VfInstallFingerprint([string]$Root)
{
    $watched = @(
        (Join-Path $Root 'f4se_loader.exe'),
        (Join-Path $Root 'f4se_1_11_221.dll'),
        (Join-Path $Root 'Fallout4.exe')
    )
    $entries = @()
    foreach ($path in $watched) {
        $entries += [pscustomobject]@{
            path = $path
            present = (Test-Path -LiteralPath $path -PathType Leaf)
            sha256 = if (Test-Path -LiteralPath $path -PathType Leaf) {
                (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
            } else { $null }
        }
    }
    $pluginDir = Join-Path $Root 'Data\F4SE\Plugins'
    $entries += [pscustomobject]@{
        path = $pluginDir
        present = (Test-Path -LiteralPath $pluginDir)
        sha256 = if (Test-Path -LiteralPath $pluginDir) {
            (@(Get-ChildItem -LiteralPath $pluginDir -File -ErrorAction SilentlyContinue |
                Sort-Object Name | ForEach-Object { $_.Name }) -join ';')
        } else { $null }
    }
    return $entries
}

$installBefore = Get-VfInstallFingerprint $gameRootFull

# Saves are the user's; the run must not add or change any. Snapshot both the
# default and the redirected save locations so new files can be reported.
$saveRoots = @((Join-Path $documentsRoot 'Saves'), (Join-Path $documentsRoot '__MO_Saves'))
$savesBefore = @()
foreach ($root in $saveRoots) {
    if (Test-Path -LiteralPath $root) {
        $savesBefore += Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName }
    }
}

$requestPath = Join-Path $artifactFull 'capture-request.txt'
$result = [ordered]@{
    startedUtc = (Get-Date).ToUniversalTime().ToString('O')
    plugin = $pluginDllFull
    pluginSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $pluginDllFull).Hash
    cellCommand = $CellCommand
    requestedCaptures = @($PreTravelCaptures + $PostTravelCaptures)
    backendRequested = [bool]$EnableBackend
    bridgePatternRequested = [bool]$EnableBridgePattern
    mirrorRequested = [bool]$EnableMirror
    vulkanMirrorDisplayed = $false
    # Frames the Vulkan renderer produced, as PNG. Empty means the mirror
    # never drew, which is a failure to report rather than to infer from a
    # screenshot that shows the game rendering itself.
    vulkanFrames = @()
    # False means every pixel on screen came from the game's own renderer and
    # the plugin only observed.
    vulkanBackendReady = $false
    vulkanPixelsDisplayed = $false
    menuReached = $false
    startingConsoleCommand = ''
    attractKeyPresses = 0
    attractDismissed = $false
    # True only when a camera capture actually found a camera, which is the
    # only reliable proof the game reached a loaded world rather than a menu.
    worldReached = $false
    cellCommandSent = $false
    firstCameraCaptured = $false
    moveCommandSent = $false
    exitedCleanly = $false
    forcedKill = $false
    captures = @()
    screenshots = @()
    newSaveFiles = @()
    quitRequested = $false
    success = $false
}

$loaderProcess = $null
$gameProcess = $null
$startTime = Get-Date
try {
    Copy-Item -LiteralPath $pluginDllFull -Destination $installedPlugin -Force
    if ($null -ne $backendDllFull) {
        Copy-Item -LiteralPath $backendDllFull -Destination $installedBackend -Force
    }

    # Small windowed output keeps input injection and screen capture reliable.
    Set-VfIniValue $prefsIni 'Display' 'bFull Screen' '0'
    Set-VfIniValue $prefsIni 'Display' 'bBorderless' '1'
    Set-VfIniValue $prefsIni 'Display' 'bMaximizeWindow' '0'
    Set-VfIniValue $prefsIni 'Display' 'iSize W' "$Width"
    Set-VfIniValue $prefsIni 'Display' 'iSize H' "$Height"
    if ($AdapterIndex -ge 0) {
        # Which DXGI adapter the engine creates its device on. Needed on a
        # machine running a headset or remote-desktop runtime: those install a
        # virtual display driver that enumerates an adapter mirroring the real
        # card -- same name, same memory -- carrying a LUID no Vulkan device
        # has, and it can enumerate first. sD3DDevice cannot disambiguate them
        # because both report the same name. The engine then presents on a
        # device the renderer cannot share images with, and the plugin
        # correctly stays on vanilla.
        Set-VfIniValue $prefsIni 'Display' 'iAdapter' "$AdapterIndex"
    }
    # No autosave may be written by an automated run.
    foreach ($key in @('bSaveOnTravel', 'bSaveOnWait', 'bSaveOnRest', 'bSaveOnPause', 'bSaveOnInteriorExteriorSwitch')) {
        Set-VfIniValue $customIni 'SaveGame' $key '0'
    }
    # The engine runs this console command itself at startup. That removes
    # synthetic keyboard input, window focus, and the attract screen from the
    # critical path entirely: the game executes the command, so nothing has to
    # be typed into it. `sStartingConsoleCommand:General` is a registered
    # Setting in this build (its Setting object is read from code at RVA
    # 0x147652), and the file is backed up and restored like every other INI
    # this harness touches.
    if (-not [string]::IsNullOrWhiteSpace($StartingConsoleCommand)) {
        Set-VfIniValue $customIni 'General' 'sStartingConsoleCommand' $StartingConsoleCommand
        $result.startingConsoleCommand = $StartingConsoleCommand
        # The engine loads the world but then pauses behind a menu the moment
        # the window loses focus, and a paused menu has no active world
        # camera. Keeping the game running unfocused is what lets an
        # unattended run reach a live camera at all.
        Set-VfIniValue $customIni 'General' 'bAlwaysActive' '1'
    }

    $env:VISUALFORGE_CAPTURE_REQUEST = $requestPath
    $env:VISUALFORGE_CAPTURE_MESH_ONCE = '1'
    $env:VISUALFORGE_CAPTURE_MESH_PATH = (Join-Path $captureDirectory 'startup-mesh.vfmesh')
    $env:VISUALFORGE_CAPTURE_TEXTURE_ONCE = '1'
    $env:VISUALFORGE_CAPTURE_TEXTURE_PATH = (Join-Path $captureDirectory 'startup-texture.vftex')
    $env:VISUALFORGE_TRACE_ONCE = '1'
    $env:VISUALFORGE_TRACE_PATH = (Join-Path $captureDirectory 'startup-frame.vftrace')
    if ($EnableBackend) {
        $env:VISUALFORGE_BACKEND_PROBE = '1'
        $env:VISUALFORGE_VULKAN_VALIDATION = '1'
    }
    if ($EnableBridgePattern) {
        $env:VISUALFORGE_BRIDGE_PATTERN = '1'
    }
    if ($EnableMirror) {
        $env:VISUALFORGE_MIRROR = '1'
        # Counts the engine's draw stream so a full-scene mirror has a
        # measured target rather than an assumed one.
        $env:VISUALFORGE_DRAW_CAPTURE = '1'
        # The frame the Vulkan renderer actually produced, written straight
        # from its readback buffer. A window screenshot cannot stand in for
        # it: the screenshot shows whatever is on screen, which is the point
        # being tested, not the evidence for it.
        $env:VISUALFORGE_MIRROR_DUMP = Join-Path $artifactFull 'vulkan-frame'
    }

    $loaderProcess = Start-Process -FilePath $loaderPath -WorkingDirectory $gameRootFull -PassThru
    $deadline = (Get-Date).AddSeconds($MenuTimeoutSeconds)
    while ((Get-Date) -lt $deadline -and $null -eq $gameProcess) {
        $candidates = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue)
        foreach ($candidate in $candidates) {
            try {
                if ($candidate.StartTime -ge $startTime.AddSeconds(-2)) { $gameProcess = $candidate; break }
            } catch { }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($null -eq $gameProcess) { throw 'Game process did not start' }

    $result.menuReached = Wait-VfLogMarker $visualForgeLog 'hook: initialized' $MenuTimeoutSeconds $gameProcess
    if (-not $result.menuReached) { throw 'Plugin never reported hook initialization' }

    # The attract screen swallows the first key, and the console is not
    # available until the main menu itself is up.
    Start-Sleep -Seconds $AttractScreenSeconds
    $gameProcess.Refresh()
    # Injecting the key is not the same as the menu advancing: the attract
    # screen ignores keys until it is ready, and a single press that reports
    # success can still leave the game sitting on "press any key". Press
    # several times, spaced out, and count how many were actually delivered.
    $delivered = 0
    $engineDrivesEntry = -not [string]::IsNullOrWhiteSpace($StartingConsoleCommand)
    if (-not $ManualWorldEntry -and -not $engineDrivesEntry) {
        for ($attempt = 0; $attempt -lt $AttractKeyPresses; ++$attempt) {
            if ($attempt -gt 0) { Start-Sleep -Seconds 3 }
            $gameProcess.Refresh()
            if (Invoke-VfKeyPress $gameProcess 0x39) { ++$delivered }   # Space
        }
        Start-Sleep -Seconds 6
    }
    $result.attractKeyPresses = $delivered
    $result.attractDismissed = $ManualWorldEntry -or $engineDrivesEntry -or
        ($delivered -gt 0)
    $menuShot = Join-Path $artifactFull 'menu.png'
    if (Save-VfWindowImage $gameProcess $menuShot) { $result.screenshots += $menuShot }
    # Without the attract screen dismissed the console never opens, so every
    # console command would silently go nowhere and every capture would be of
    # a menu rather than a world. Stop rather than produce misleading
    # artifacts.
    if (-not $result.attractDismissed) {
        throw ('Attract screen was never dismissed, so the game never reached ' +
            'its menu with the foreground. Another window most likely holds ' +
            'focus; no console command was sent and no capture was armed.')
    }

    $sequence = 0
    $captureResults = @()
    $armCapture = {
        param($capture, $sequenceNumber, $label = '')
        $extension = switch ($capture) {
            'mesh' { '.vfmesh' }
            'texture' { '.vftex' }
            'trace' { '.vftrace' }
            'frame' { '.vfframe' }
            'scene' { '.vfscene' }
            'deformation' { '.vfdeform' }
            default { throw "Unknown capture kind: $capture" }
        }
        $name = if ([string]::IsNullOrWhiteSpace($label)) {
            "live-$capture$extension"
        } else {
            "live-$capture-$label$extension"
        }
        $target = Join-Path $captureDirectory $name
        Set-Content -LiteralPath $requestPath -Encoding ascii -Value @(
            "sequence=$sequenceNumber",
            "kind=$capture",
            "path=$target"
        )
        $armed = Wait-VfLogMarker $visualForgeLog `
            "renderer-capture-request: sequence=$sequenceNumber .*result=complete" 30 $gameProcess
        return [pscustomobject]@{ kind = $capture; sequence = $sequenceNumber; path = $target; armed = $armed }
    }

    # Armed before travel: the cell load is what creates world meshes and
    # streams world textures, which is exactly what the one-shot hooks want.
    $pending = @()
    foreach ($capture in $PreTravelCaptures) {
        ++$sequence
        $pending += & $armCapture $capture $sequence
    }

    if ($engineDrivesEntry) {
        # Nothing is injected here. The engine runs the command on its own, so
        # this only waits for a camera capture to succeed, which is the one
        # signal that distinguishes a loaded world from a menu.
        Write-Host "Engine startup command: $StartingConsoleCommand"
        $probeDeadline = (Get-Date).AddSeconds($StartupWorldSeconds)
        while ((Get-Date) -lt $probeDeadline -and -not $result.worldReached) {
            if ($gameProcess.HasExited) { break }
            ++$sequence
            $probe = & $armCapture 'frame' $sequence "probe$sequence"
            if ($probe.armed) {
                $result.worldReached = Wait-VfLogMarker $visualForgeLog `
                    ('renderer-camera-capture: complete path=' +
                        [regex]::Escape($probe.path)) 25 $gameProcess
            }
            # A screenshot while probing is the only thing that distinguishes
            # "the world never loaded" from "the world loaded but the camera
            # scan window is wrong". Without it both look identical in the log.
            if (-not $result.worldReached -and ($sequence % 3) -eq 1) {
                $probeShot = Join-Path $artifactFull "probe-$sequence.png"
                if (Save-VfWindowImage $gameProcess $probeShot) {
                    $result.screenshots += $probeShot
                }
            }
            if (-not $result.worldReached) { Start-Sleep -Seconds 5 }
        }
        if (-not $result.worldReached) {
            throw ("The engine startup command '$StartingConsoleCommand' did " +
                "not produce a world camera within $StartupWorldSeconds " +
                'seconds. Either the setting is not honoured by this build or ' +
                'the command failed; nothing was captured.')
        }
        # The first camera to appear belongs to the loading screen, so the
        # experiment has to wait for the load to finish before it means
        # anything. world.png is written after the settle so it shows the
        # state the captures were actually taken in.
        Write-Host "World camera detected; settling $WorldSettleSeconds s."
        Start-Sleep -Seconds $WorldSettleSeconds
        $worldShot = Join-Path $artifactFull 'world.png'
        if (Save-VfWindowImage $gameProcess $worldShot) { $result.screenshots += $worldShot }
    } elseif ($ManualWorldEntry) {
        Write-Host ''
        Write-Host '================= OPERATOR ACTION REQUIRED ================='
        Write-Host 'Bring the Fallout 4 window to the front and get into a world:'
        Write-Host '  dismiss "press any key", then load a save or open the'
        Write-Host '  console (`) and type:  coc SanctuaryExt'
        Write-Host ''
        Write-Host 'Then leave the game focused and do not touch it.'
        Write-Host "Waiting up to $ManualWorldEntrySeconds seconds."
        Write-Host '==========================================================='
        Write-Host ''
        # A menu has no world camera, so a successful camera capture is the
        # only signal that actually distinguishes a loaded world from a menu.
        # Each probe uses a distinct path because the log is rescanned from the
        # start and a shared path would match the previous probe's line.
        $probeDeadline = (Get-Date).AddSeconds($ManualWorldEntrySeconds)
        while ((Get-Date) -lt $probeDeadline -and -not $result.worldReached) {
            if ($gameProcess.HasExited) { break }
            ++$sequence
            $probe = & $armCapture 'frame' $sequence "probe$sequence"
            if ($probe.armed) {
                $result.worldReached = Wait-VfLogMarker $visualForgeLog `
                    ('renderer-camera-capture: complete path=' +
                        [regex]::Escape($probe.path)) 25 $gameProcess
            }
            if (-not $result.worldReached) { Start-Sleep -Seconds 5 }
        }
        if (-not $result.worldReached) {
            throw ('No world camera appeared within ' +
                "$ManualWorldEntrySeconds seconds. The game never reached a " +
                'loaded world, so nothing was captured.')
        }
        Write-Host 'World camera detected; continuing automatically.'
        Start-Sleep -Seconds $WorldSettleSeconds
        $result.cellCommandSent = $false
        $worldShot = Join-Path $artifactFull 'world.png'
        if (Save-VfWindowImage $gameProcess $worldShot) { $result.screenshots += $worldShot }
    } elseif (-not [string]::IsNullOrWhiteSpace($CellCommand)) {
        $gameProcess.Refresh()
        $result.cellCommandSent = Invoke-VfConsoleCommand $gameProcess $CellCommand
        Start-Sleep -Seconds $LoadSettleSeconds
        $worldShot = Join-Path $artifactFull 'world.png'
        if (Save-VfWindowImage $gameProcess $worldShot) { $result.screenshots += $worldShot }
    }

    foreach ($capture in $PostTravelCaptures) {
        ++$sequence
        $entry = & $armCapture $capture $sequence
        $pending += $entry
        # A second capture of the same kind after the player has moved is
        # what proves a captured camera tracks the world rather than being a
        # static or secondary record.
        if ($capture -eq 'frame' -and -not [string]::IsNullOrWhiteSpace($MoveCommand)) {
            # The marker has to name the target path. The log is rescanned
            # from the start every poll, so a generic marker would match the
            # previous capture's line and report success immediately.
            $completion = Wait-VfLogMarker $visualForgeLog `
                ('renderer-camera-capture: complete path=' +
                    [regex]::Escape($entry.path)) 60 $gameProcess
            $result.firstCameraCaptured = $completion
            $result.worldReached = $completion
            if (-not $completion) {
                # A menu has no world camera, so a rejected scan almost always
                # means the console command never reached a loaded cell.
                throw ('No camera was found after the cell command, so the ' +
                    'game never reached a loaded world. Check world.png: if ' +
                    'it still shows "press any key", the attract screen was ' +
                    'never actually dismissed.')
            }
            if ($completion) {
                $gameProcess.Refresh()
                $result.moveCommandSent = Invoke-VfConsoleCommand $gameProcess $MoveCommand
                Start-Sleep -Seconds 6
                $movedShot = Join-Path $artifactFull 'world-moved.png'
                if (Save-VfWindowImage $gameProcess $movedShot) {
                    $result.screenshots += $movedShot
                }
                ++$sequence
                $pending += & $armCapture 'frame' $sequence 'moved'
            }
        }
    }

    foreach ($entry in $pending) {
        # Every pattern that can be armed more than once in a run must name
        # its target path, because the log is rescanned from the start.
        $completionPattern = switch ($entry.kind) {
            'mesh' { 'renderer-mesh-capture: complete' }
            'texture' { 'renderer-texture-capture: complete' }
            'trace' { 'renderer-observe: trace complete' }
            'frame' {
                'renderer-camera-capture: complete path=' +
                    [regex]::Escape($entry.path)
            }
            default { $null }
        }
        $completed = $false
        if ($entry.armed -and $null -ne $completionPattern) {
            $completed = Wait-VfLogMarker $visualForgeLog $completionPattern $CaptureTimeoutSeconds $gameProcess
        }
        $captureResults += [pscustomobject]@{
            kind = $entry.kind
            sequence = $entry.sequence
            path = $entry.path
            armed = $entry.armed
            completed = $completed
            exists = (Test-Path -LiteralPath $entry.path -PathType Leaf)
        }
    }
    $result.captures = $captureResults

    if ($EnableBackend) {
        $result.vulkanBackendReady = Wait-VfLogMarker $visualForgeLog 'renderer-backend: ready' 60 $gameProcess
    }
    if ($EnableBridgePattern) {
        $result.vulkanPixelsDisplayed = Wait-VfLogMarker $visualForgeLog `
            'renderer-bridge: first-frame displayed' 90 $gameProcess
    }
    if ($EnableMirror) {
        $result.vulkanMirrorDisplayed = Wait-VfLogMarker $visualForgeLog `
            'renderer-mirror: first-frame displayed' 120 $gameProcess
    }

    if ($EnableMirror) {
        foreach ($dump in @(Get-ChildItem -LiteralPath $artifactFull -Filter 'vulkan-frame.slot*.ppm' -ErrorAction SilentlyContinue)) {
            $png = [System.IO.Path]::ChangeExtension($dump.FullName, '.png')
            if (Convert-VfPpmToPng $dump.FullName $png) {
                $result.vulkanFrames += $png
            }
        }
    }

    $shot = Join-Path $artifactFull 'after-captures.png'
    if (Save-VfWindowImage $gameProcess $shot) { $result.screenshots += $shot }

    # A clean quit matters beyond tidiness: repeatedly force-killing the game
    # is believed to have triggered a Steam file repair that removed F4SE, so
    # every gentler option is exhausted first and a forced kill is recorded.
    $result.quitRequested = Invoke-VfConsoleCommand $gameProcess 'qqq'
    if (-not $gameProcess.WaitForExit(45000)) {
        $result.quitRequested = Invoke-VfConsoleCommand $gameProcess 'qqq'
        $gameProcess.WaitForExit(30000) | Out-Null
    }
    $result.exitedCleanly = $gameProcess.HasExited
} finally {
    foreach ($name in @('VISUALFORGE_CAPTURE_REQUEST', 'VISUALFORGE_CAPTURE_MESH_ONCE',
            'VISUALFORGE_CAPTURE_MESH_PATH', 'VISUALFORGE_CAPTURE_TEXTURE_ONCE',
            'VISUALFORGE_CAPTURE_TEXTURE_PATH', 'VISUALFORGE_TRACE_ONCE', 'VISUALFORGE_TRACE_PATH',
            'VISUALFORGE_BACKEND_PROBE', 'VISUALFORGE_VULKAN_VALIDATION',
            'VISUALFORGE_BRIDGE_PATTERN', 'VISUALFORGE_MIRROR',
            'VISUALFORGE_MIRROR_DUMP', 'VISUALFORGE_DRAW_CAPTURE')) {
        Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $visualForgeLog -PathType Leaf) {
        Copy-Item -LiteralPath $visualForgeLog -Destination (Join-Path $artifactFull 'VisualForge-live.log') -Force
    }
    if (Test-Path -LiteralPath $visualForgeCrashLog -PathType Leaf) {
        Copy-Item -LiteralPath $visualForgeCrashLog -Destination (Join-Path $artifactFull 'VisualForge-live-crash.log') -Force
    }

    # Escalate slowly: ask the window to close and give the game a real
    # chance to shut down before anything forceful. A forced kill is a last
    # resort and is always reported.
    foreach ($process in @($loaderProcess, $gameProcess)) {
        if ($null -eq $process) { continue }
        try {
            if ($process.HasExited) { continue }
            $process.CloseMainWindow() | Out-Null
            if ($process.WaitForExit(45000)) { continue }
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $result.forcedKill = $true
        } catch { }
    }
    $watchdog = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $watchdog) {
        $owned = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue | Where-Object {
            try { $_.StartTime -ge $startTime.AddSeconds(-2) } catch { $false } })
        if ($owned.Count -eq 0) { break }
        foreach ($process in $owned) {
            try { $process.CloseMainWindow() | Out-Null } catch { }
        }
        Start-Sleep -Seconds 2
    }
    $stillOwned = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue | Where-Object {
        try { $_.StartTime -ge $startTime.AddSeconds(-2) } catch { $false } })
    foreach ($process in $stillOwned) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $result.forcedKill = $true
    }

    $savesAfter = @()
    foreach ($root in $saveRoots) {
        if (Test-Path -LiteralPath $root) {
            $savesAfter += Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue |
                ForEach-Object { $_.FullName }
        }
    }
    $result.newSaveFiles = @($savesAfter | Where-Object { $savesBefore -notcontains $_ })

    $restoreErrors = @()
    foreach ($tracked in $trackedFiles) {
        try { Restore-VfTrackedFile $tracked } catch { $restoreErrors += "Failed to restore $($tracked.Path): $_" }
    }
    $result.restoreErrors = $restoreErrors
    # Cleared only once every file is actually back. Clearing it regardless
    # would leave the next run believing there was nothing to repair.
    if ($restoreErrors.Count -eq 0) { Clear-VfPendingRestore }
    $result.completedUtc = (Get-Date).ToUniversalTime().ToString('O')
    $result.success = ($restoreErrors.Count -eq 0) -and $result.menuReached -and
        (@($result.captures | Where-Object { -not $_.completed }).Count -eq 0)
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $artifactFull 'result.json') -Encoding utf8
    if ($restoreErrors.Count -ne 0) { throw ($restoreErrors -join [Environment]::NewLine) }
}

Write-Output "Live capture finished; artifacts: $artifactFull"
if (-not $result.success) { throw "Live capture did not complete every requested capture; see $artifactFull\result.json" }
