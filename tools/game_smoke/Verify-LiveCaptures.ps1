<#
.SYNOPSIS
Turns a live capture directory into promotion evidence.

.DESCRIPTION
Inspects every captured artifact with the offline tools: traces through the
trace reader, and captured meshes through the Vulkan replay with the captured
texture bound. Reports sizes, hashes, and replay metrics so a phase gate can
cite live evidence rather than a synthetic fixture.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CaptureDirectory,
    [Parameter(Mandatory = $true)][string]$ReplayExe,
    [Parameter(Mandatory = $true)][string]$BackendDll,
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$captureFull = [System.IO.Path]::GetFullPath($CaptureDirectory)
if (-not (Test-Path -LiteralPath $captureFull -PathType Container)) {
    throw "Capture directory missing: $captureFull"
}
$outputFull = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $captureFull
} else {
    [System.IO.Path]::GetFullPath($OutputDirectory)
}
if (-not (Test-Path -LiteralPath $outputFull)) {
    New-Item -ItemType Directory -Path $outputFull | Out-Null
}

$report = [ordered]@{
    captureDirectory = $captureFull
    artifacts = @()
    traceSummaries = @()
    meshReplays = @()
}

foreach ($file in Get-ChildItem -LiteralPath $captureFull -File | Sort-Object Name) {
    $report.artifacts += [pscustomobject]@{
        name = $file.Name
        bytes = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    }
}

foreach ($trace in Get-ChildItem -LiteralPath $captureFull -Filter '*.vftrace' -File) {
    $text = & $ReplayExe --inspect $trace.FullName 2>&1 | Out-String
    $report.traceSummaries += [pscustomobject]@{
        name = $trace.Name
        exitCode = $LASTEXITCODE
        summary = $text.Trim()
    }
}

$texture = Get-ChildItem -LiteralPath $captureFull -Filter '*.vftex' -File |
    Sort-Object Length -Descending | Select-Object -First 1
foreach ($mesh in Get-ChildItem -LiteralPath $captureFull -Filter '*.vfmesh' -File) {
    $output = Join-Path $outputFull ($mesh.BaseName + '-replay.ppm')
    $arguments = @('--render-mesh', $mesh.FullName, '--backend', $BackendDll,
        '--output', $output, '--validation')
    if ($null -ne $texture) { $arguments += @('--texture', $texture.FullName) }
    $text = & $ReplayExe @arguments 2>&1 | Out-String
    $report.meshReplays += [pscustomobject]@{
        mesh = $mesh.Name
        texture = if ($null -ne $texture) { $texture.Name } else { $null }
        exitCode = $LASTEXITCODE
        output = $output
        outputSha256 = if (Test-Path -LiteralPath $output -PathType Leaf) {
            (Get-FileHash -Algorithm SHA256 -LiteralPath $output).Hash
        } else { $null }
        report = $text.Trim()
    }
}

$reportPath = Join-Path $outputFull 'live-verification.json'
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "Wrote $reportPath"
foreach ($entry in $report.traceSummaries) { Write-Output "trace $($entry.name): $($entry.summary)" }
foreach ($entry in $report.meshReplays) { Write-Output "mesh $($entry.mesh): $($entry.report)" }
