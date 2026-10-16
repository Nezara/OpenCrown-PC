# Ghidra headless wrapper for the DCP project (read-only; the project must not be open in the GUI or another run).
#   .\ghidra.ps1 decomp <out.c> FUN_00123450 0x42b470 ...   -> pseudo-C, prints the file
#   .\ghidra.ps1 index                                       -> refresh Work/index/*.tsv
#   .\ghidra.ps1 script <Script.java> [args...]               -> any script in Work/ghidra_scripts, read-only
# Prints only errors and script output lines, not Ghidra's startup noise.
param([Parameter(Mandatory)][string]$Cmd, [Parameter(ValueFromRemainingArguments)][string[]]$Rest)
$work = Split-Path $PSScriptRoot
# Tool locations: JAVA_HOME (JDK 21) and GHIDRA_HOME (Ghidra install) from the environment, or from an untracked
# paths.local.ps1 next to this script that sets them.
$local = Join-Path $PSScriptRoot 'paths.local.ps1'
if (Test-Path $local) { . $local }
if (-not $env:JAVA_HOME -or -not $env:GHIDRA_HOME) { throw 'Set JAVA_HOME (JDK 21) and GHIDRA_HOME (Ghidra folder), e.g. in paths.local.ps1.' }
$ah = Join-Path $env:GHIDRA_HOME 'support\analyzeHeadless.bat'
$common = @((Join-Path $work 'ghidra'), 'DragonsCrownPro', '-process', 'eboot_ghidra.elf', '-noanalysis', '-readOnly',
            '-scriptPath', (Join-Path $work 'ghidra_scripts'))
function Run([string]$script, [string[]]$args_) {
    & $ah @common -postScript $script @args_ 2>&1 | ForEach-Object { "$_" } |
        Where-Object { $_ -match 'ERROR|Exception|\.java> ' } | ForEach-Object { $_ -replace '^INFO\s+', '' }
}
switch ($Cmd) {
    'decomp' {
        $out = [IO.Path]::GetFullPath($Rest[0])
        Run 'DecompileFuncs.java' (@($out) + $Rest[1..($Rest.Count - 1)])
        Get-Content $out
    }
    'index' { Run 'ExportIndex.java' @((Join-Path $work 'index')) }
    'script' { Run $Rest[0] $Rest[1..($Rest.Count - 1)] }
    default { throw "unknown command $Cmd" }
}
