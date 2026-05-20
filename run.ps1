# Launch ps1-acid-rom in the local pcsx-redux fork.
#
# Run from a regular PowerShell prompt (not via tool harness). Drops you into
# pcsx-redux with the latest CI-built ps-exe loaded. Use `-Build` to first
# fetch the freshest CI artifact, otherwise re-uses whatever is in artifacts/.

param(
    [switch]$Build,
    [string]$PsExe = "$PSScriptRoot\artifacts\ps1-acid-rom.ps-exe",
    [string]$Pcsx  = "$PSScriptRoot\..\release-test\pcsx-A\pcsx-redux.exe"
)

$ErrorActionPreference = "Stop"

if ($Build -or -not (Test-Path $PsExe)) {
    Write-Host "Fetching latest CI artifact..."
    Push-Location $PSScriptRoot
    try {
        $runId = gh run list --branch main --limit 1 --json databaseId,conclusion `
            | ConvertFrom-Json `
            | Where-Object { $_.conclusion -eq "success" } `
            | Select-Object -First 1 -ExpandProperty databaseId
        if (-not $runId) { throw "no successful CI run on main" }
        New-Item -ItemType Directory -Force -Path "$PSScriptRoot\artifacts" | Out-Null
        gh run download $runId -n ps1-acid-rom-ps-exe -D "$PSScriptRoot\artifacts"
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path $PsExe)) { throw "ps-exe not found: $PsExe" }
if (-not (Test-Path $Pcsx))  { throw "pcsx-redux not found: $Pcsx  (override with -Pcsx)" }

Write-Host "Launching pcsx-redux with $PsExe"
& $Pcsx -loadexe "$PsExe" -fastboot
