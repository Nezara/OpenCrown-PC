# Copy the current save into Work/story/snap_NN_<label>/ (next free NN). PowerShell twin of snap.py.
# usage: powershell -ExecutionPolicy Bypass -File Work/tools/snap.ps1 <label>
param([Parameter(Mandatory = $true)][string]$Label)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'story'
$saves = if ($env:DCP_SAVE_ROOT) { $env:DCP_SAVE_ROOT } else { Join-Path $env:USERPROFILE 'Documents\Dragons Crown Pro\saves' }
$src = Join-Path $saves 'SAVEDATA00\SAVE0.DAT'
$n = 0
if (Test-Path $root) {
    Get-ChildItem $root -Directory -Filter 'snap_*' | ForEach-Object {
        if ($_.Name -match '^snap_(\d\d)_') { $n = [Math]::Max($n, [int]$Matches[1]) }
    }
}
$dst = Join-Path $root ('snap_{0:D2}_{1}' -f ($n + 1), $Label)
New-Item -ItemType Directory -Path $dst | Out-Null
Copy-Item $src $dst
Write-Output $dst
