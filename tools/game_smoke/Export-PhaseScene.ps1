<#
.SYNOPSIS
Captures an in-game scene through the Vulkan renderer and writes it as PNG
into a phase's artifact directory.

.DESCRIPTION
Run once per completed phase. It launches the game through the live capture
harness with the Vulkan mirror enabled, collects everything the renderer
produced, renders the captured game mesh through the backend offline, and
converts every result to PNG.

Three different things end up in the phase directory, and they are not
interchangeable:

  scene-live-<slot>.png   Frames the Vulkan renderer produced inside the
                          running game, driven by the live engine camera for
                          that camera slot. This is the replacement renderer
                          drawing while Fallout 4 is running.
  scene-mesh.png          A mesh and texture captured from the running game,
                          replayed through the Vulkan backend offline. This is
                          real game content rendered by the replacement.
  game-window.png         A screenshot of the game window. Evidence of what
                          was on screen, not of what the renderer produced.

What is deliberately NOT claimed here: the live frames draw the mirror's own
geometry, because feeding the whole visible world to the backend needs the
per-frame world capture that the live promotion gates still owe. Until that
lands, "a full in-game scene rendered by Vulkan" means the captured content
above, not an entire cell.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][int]$Phase,
    [Parameter(Mandatory = $true)][string]$GameRoot,
    [string]$PluginDll = 'out/build/vs2022-x64-release/Release/VisualForge.dll',
    [string]$BackendDll = 'out/build/vs2022-x64-release/Release/VisualForgeRenderer.dll',
    [string]$ReplayExe = 'out/build/vs2022-x64-debug/Debug/vf_packet_replay.exe',
    [string]$DebugBackendDll = 'out/build/vs2022-x64-debug/Debug/VisualForgeRenderer.dll',
    [string]$StartingConsoleCommand = 'coc SanctuaryExt',
    [int]$Width = 1280,
    [int]$Height = 720,
    # Reuses an existing live capture instead of launching the game again.
    # A live run costs minutes and disturbs the machine, so re-exporting from
    # one already taken is the normal case when only the PNGs changed.
    [string]$ExistingCapture = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. $PSScriptRoot/VfImage.ps1

$phaseTag = 'phase-{0:00}' -f $Phase
$artifactRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'artifacts'
$phaseDirectory = Join-Path $artifactRoot $phaseTag
New-Item -ItemType Directory -Path $phaseDirectory -Force | Out-Null

if ($ExistingCapture) {
    $captureDirectory = [System.IO.Path]::GetFullPath($ExistingCapture)
    if (-not (Test-Path -LiteralPath $captureDirectory -PathType Container)) {
        throw "No such capture directory: $captureDirectory"
    }
} else {
    # The harness refuses to adopt a process it did not start, but checking
    # here as well keeps the refusal cheap and the reason obvious.
    $running = @(Get-Process -Name Fallout4 -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw "Fallout 4 is already running ($($running.Count) process). Close it before capturing; this harness will not adopt a game process it did not start."
    }
    $captureDirectory = Join-Path $artifactRoot "live-$phaseTag"
    if (Test-Path -LiteralPath $captureDirectory) {
        $captureDirectory = Join-Path $artifactRoot ("live-{0}-{1}" -f $phaseTag, (Get-Random))
    }
    & $PSScriptRoot/Invoke-LiveCapture.ps1 -GameRoot $GameRoot `
        -PluginDll $PluginDll -BackendDll $BackendDll `
        -ArtifactDirectory $captureDirectory `
        -StartingConsoleCommand $StartingConsoleCommand `
        -Width $Width -Height $Height -EnableBackend -EnableMirror | Out-Null
}

$exported = New-Object System.Collections.Generic.List[string]

# The renderer's own frames, one per camera slot it drew for.
foreach ($dump in @(Get-ChildItem -LiteralPath $captureDirectory -Filter 'vulkan-frame.slot*.ppm' -ErrorAction SilentlyContinue)) {
    if ($dump.BaseName -match 'slot(\d+)') {
        $target = Join-Path $phaseDirectory ("scene-live-slot{0}.png" -f $Matches[1])
        if (Convert-VfPpmToPng $dump.FullName $target) { $exported.Add($target) }
    }
}

# Real captured game geometry, replayed through the backend. Prefer the mesh
# captured after the world loaded; fall back to the startup one.
$meshes = @(Get-ChildItem -LiteralPath (Join-Path $captureDirectory 'captures') -Filter '*.vfmesh' -ErrorAction SilentlyContinue |
    Sort-Object -Property Length -Descending)
$textures = @(Get-ChildItem -LiteralPath (Join-Path $captureDirectory 'captures') -Filter '*.vftex' -ErrorAction SilentlyContinue |
    Sort-Object -Property Length -Descending)
if ($meshes.Count -gt 0 -and (Test-Path -LiteralPath $ReplayExe -PathType Leaf)) {
    $meshPpm = Join-Path $captureDirectory 'live-mesh-vulkan.ppm'
    $replayArgs = @('--render-mesh', $meshes[0].FullName, '--backend', $DebugBackendDll,
        '--output', $meshPpm, '--width', $Width, '--height', $Height, '--validation')
    if ($textures.Count -gt 0) { $replayArgs += @('--texture', $textures[0].FullName) }
    & $ReplayExe @replayArgs | Out-Null
    if (Test-Path -LiteralPath $meshPpm -PathType Leaf) {
        $target = Join-Path $phaseDirectory 'scene-mesh.png'
        if (Convert-VfPpmToPng $meshPpm $target) { $exported.Add($target) }
    }
}

# The game's own window, for comparison rather than as renderer evidence.
foreach ($name in @('world.png', 'after-captures.png', 'menu.png')) {
    $shot = Join-Path $captureDirectory $name
    if (Test-Path -LiteralPath $shot -PathType Leaf) {
        $target = Join-Path $phaseDirectory 'game-window.png'
        Copy-Item -LiteralPath $shot -Destination $target -Force
        $exported.Add($target)
        break
    }
}

if ($exported.Count -eq 0) {
    throw "No scene was exported for $phaseTag. The capture produced nothing renderable, which is a failure to investigate rather than an empty phase."
}

[pscustomobject]@{
    phase = $Phase
    capture = $captureDirectory
    exported = $exported.ToArray()
} | ConvertTo-Json -Depth 4
