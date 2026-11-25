<#
.SYNOPSIS
  Build the knobbed prefetcher for Windows and tune its knobs on one trace with CARBS,
  random search and hill climbing. One script, start to finish.

.DESCRIPTION
  *** UNTESTED. *** Written on a Linux machine by following what run_windows.ps1 already does
  (clang + GNU make + the vcpkg DLLs + python). The Linux version of every step is tested; this
  file is the Windows translation. If a step fails, the message says which one.

  What it does:
    1. Checks python (with numpy), clang and make are on PATH.
    2. Copies champsim\source-windows and champsim\modules into a path WITHOUT spaces
       (ChampSim's Makefile breaks on spaces; "ece 752" has one) and builds
       champsim_knobbed.exe there from champsim\champsim_config_knobbed.json.
    3. Runs champsim\knob_carbs.py on your trace (knob search; optional exhaustive sweep).
    4. Prints the results table.

  Prerequisites (same as run_windows.ps1, plus numpy):
    - Python 3 with numpy:            python -m pip install numpy
    - LLVM (clang, llvm-objcopy):     C:\Program Files\LLVM\bin  (found automatically)
    - GNU Make on PATH
    - ChampSim's vcpkg runtime DLLs:  champsim\source-windows\vcpkg_installed\x64-windows\bin
      (and the vcpkg_installed folder itself, which is not in git)
    - .xz traces (the SPEC ones) work as downloaded; no xz.exe needed (ChampSim has built-in lzma)
    - A trace file made for ChampSim (PIN or DynamoRIO trace, .champsimtrace/.trace/.xz)

.EXAMPLE
  # a trace fetched by download_spec_traces.ps1 (looked up by name in $HOME\spec-traces)
  .\champsim\run_knob_tuning_windows.ps1 -Trace 605.mcf_s-1554B

.EXAMPLE
  # every downloaded trace, or a different folder
  .\champsim\run_knob_tuning_windows.ps1 -All
  .\champsim\run_knob_tuning_windows.ps1 -All -TraceDir D:\traces

.EXAMPLE
  .\champsim\run_knob_tuning_windows.ps1 -Trace C:\traces\605.mcf_s-1554B.champsimtrace.xz

.EXAMPLE
  # ground truth: simulate all 2,305 distinct configs first (hours), 8 at a time
  .\champsim\run_knob_tuning_windows.ps1 -Trace C:\traces\k_stencil.trace -Exhaustive -Jobs 8

.EXAMPLE
  # only print the table for an existing results folder
  .\champsim\run_knob_tuning_windows.ps1 -SummarizeOnly -Output .\champsim\results\knob-carbs-spec-v2
#>
[CmdletBinding()]
param(
    [string]$Trace = "",                  # path, or a bare name like 605.mcf_s-1554B (looked up in -TraceDir)
    [string]$TraceDir = "$HOME\spec-traces",   # where download_spec_traces.ps1 puts the traces
    [switch]$All,                         # tune on every *.champsimtrace.xz in -TraceDir
    [string]$Kernel = "",                 # name used in the results file; default = trace file name
    [string]$Output = "",                 # results folder; default champsim\results\knob-carbs-windows
    [int]$Budget = 20,                    # evaluations per tuner run
    [int]$Seeds = 2,
    [int]$WarmupInstructions = 1000000,
    [int]$SimInstructions = 3000000,      # must fit in the trace after the warmup
    [switch]$Exhaustive,                  # simulate every distinct config first (ground truth)
    [int]$Jobs = 4,                       # parallel simulations for -Exhaustive
    [string]$BuildDir = "C:\champsim-knobbed-win",   # must not contain spaces
    [switch]$Rebuild,                     # force a rebuild even if the .exe exists
    [switch]$SummarizeOnly
)

$ErrorActionPreference = "Stop"
$champsimDir = (Get-Item $PSScriptRoot).FullName
$repo = (Get-Item (Join-Path $champsimDir "..")).FullName
$sourceWin = Join-Path $champsimDir "source-windows"
$config = Join-Path $champsimDir "champsim_config_knobbed.json"
$tuner = Join-Path $champsimDir "knob_carbs.py"

function Find-CommandPath([string]$Name, [string]$Hint) {
    $c = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $c) { throw "$Name was not found on PATH. $Hint" }
    return $c.Source
}

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $champsimDir "results\knob-carbs-windows"
}

# ---- 0. only print a table ------------------------------------------------------------
if ($SummarizeOnly) {
    & python $tuner --summarize $Output
    exit $LASTEXITCODE
}

# ---- resolve traces: -All, a full path, or a bare name inside -TraceDir ----------------------
function Resolve-TraceName([string]$t) {
    if (Test-Path -LiteralPath $t -PathType Leaf) { return (Get-Item -LiteralPath $t).FullName }
    foreach ($suffix in @("", ".champsimtrace.xz", ".champsimtrace", ".trace", ".xz")) {
        $cand = Join-Path $TraceDir ($t + $suffix)
        if (Test-Path -LiteralPath $cand -PathType Leaf) { return (Get-Item -LiteralPath $cand).FullName }
    }
    throw "Trace not found: $t (also looked in $TraceDir). Download with champsim\download_spec_traces.ps1."
}
if ($All) {
    if (-not (Test-Path -LiteralPath $TraceDir)) { throw "-TraceDir not found: $TraceDir" }
    $traceList = @(Get-ChildItem -LiteralPath $TraceDir -Filter "*.champsimtrace.xz" | Sort-Object Name | ForEach-Object { $_.FullName })
    if ($traceList.Count -eq 0) { throw "No *.champsimtrace.xz files in $TraceDir" }
} elseif (-not [string]::IsNullOrWhiteSpace($Trace)) {
    $traceList = @(Resolve-TraceName $Trace)
} else {
    throw "Pass -Trace <path or name> or -All (traces in $TraceDir)."
}
if ($traceList.Count -gt 1 -and -not [string]::IsNullOrWhiteSpace($Kernel)) { throw "-Kernel only applies to a single trace." }
if ($BuildDir -match "\s") { throw "-BuildDir must not contain spaces: $BuildDir" }

# ---- 1. prerequisites -------------------------------------------------------------------
Write-Host "[1/4] Checking prerequisites"
$standardLlvmBin = "C:\Program Files\LLVM\bin"
if (Test-Path (Join-Path $standardLlvmBin "clang.exe")) { $env:Path = "$standardLlvmBin;$env:Path" }
$python = Find-CommandPath "python" "Install Python 3."
$clang = Find-CommandPath "clang" "Install LLVM (https://releases.llvm.org) and add it to PATH."
$make = Find-CommandPath "make" "Install GNU Make (for example: winget install GnuWin32.Make)."
& python -c "import numpy" 2>$null
if ($LASTEXITCODE -ne 0) { throw "numpy is missing. Run: python -m pip install numpy" }
# .xz traces need no external xz: ChampSim decompresses them with its built-in lzma support.
$vcpkgBin = Join-Path $sourceWin "vcpkg_installed\x64-windows\bin"
if (-not (Test-Path $vcpkgBin)) {
    throw "vcpkg runtime DLLs not found: $vcpkgBin. Build ChampSim's vcpkg dependencies first (see champsim\README.md, Windows prerequisites)."
}
$llvmBin = Split-Path $clang -Parent
$env:Path = "$llvmBin;$vcpkgBin;$env:Path"

# ---- 2. build champsim_knobbed.exe in a space-free copy ------------------------------
$exe = Join-Path $BuildDir "bin\champsim_knobbed.exe"
if ($Rebuild -or -not (Test-Path $exe)) {
    Write-Host "[2/4] Building knobbed ChampSim in $BuildDir (a few minutes)"
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    # Copy the Windows ChampSim tree (including vcpkg_installed) and our modules.
    # robocopy exit codes 0-7 mean success.
    $null = robocopy $sourceWin $BuildDir /E /NFL /NDL /NJH /NJS /NP /XD ".csconfig" "bin" "tracer"
    if ($LASTEXITCODE -ge 8) { throw "Copying source-windows failed (robocopy exit $LASTEXITCODE)." }
    $modules = Join-Path $BuildDir "knobbed_modules"
    if (Test-Path $modules) { Remove-Item -Recurse -Force $modules }
    $null = robocopy (Join-Path $champsimDir "modules") $modules /E /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw "Copying modules failed (robocopy exit $LASTEXITCODE)." }
    foreach ($stale in @("absolute.options", ".csconfig", "inc\cache_modules.h", "inc\ooo_cpu_modules.h")) {
        $p = Join-Path $BuildDir $stale
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }
    Push-Location $BuildDir
    try {
        & python (Join-Path $BuildDir "config.sh") --module-dir $modules $config
        if ($LASTEXITCODE -ne 0) { throw "ChampSim configuration failed (config.sh)." }
        # same as run_windows.ps1: config.sh writes absolute.options; the Windows build wants it gone
        Remove-Item -LiteralPath (Join-Path $BuildDir "absolute.options") -Force -ErrorAction SilentlyContinue
        & $make "CXX=clang++" "CC=clang" "CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll" "LDLIBS=-lshell32"
        if ($LASTEXITCODE -ne 0) { throw "ChampSim build failed." }
    } finally {
        Pop-Location
    }
} else {
    Write-Host "[2/4] Reusing $exe (pass -Rebuild to rebuild)"
}
if (-not (Test-Path $exe)) { throw "Build finished but $exe does not exist. The executable name comes from executable_name in $config." }

# ---- 3. tune --------------------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$env:PYTHONPATH = $repo   # knob_carbs.py imports flagbench.tuners from the repo root
$resultFile = ""
foreach ($tr in $traceList) {
    $k = $Kernel
    if ([string]::IsNullOrWhiteSpace($k)) {
        $k = [System.IO.Path]::GetFileName($tr) -replace "\.(champsimtrace|trace|champsim)?(\.xz)?$", ""
    }
    Write-Host "[3/4] Tuning knobs on $k (budget $Budget x $Seeds seeds, warmup $WarmupInstructions, sim $SimInstructions)"
    $resultFile = Join-Path $Output ($k + ".json")
    $args3 = @($tuner, "--kernel", $k, "--trace", $tr, "--out", $resultFile, "--binary", $exe,
               "--budget", $Budget, "--seeds", $Seeds, "--warmup", $WarmupInstructions, "--sim", $SimInstructions)
    if ($Exhaustive) { $args3 += @("--exhaustive", "--jobs", $Jobs) }
    & python @args3
    if ($LASTEXITCODE -ne 0) { throw "knob_carbs.py failed on $k. Run the simulator by hand to see its output: `\$env:KNOBBED_HINT='source=nextline'; & '$exe' --warmup-instructions 100000 --simulation-instructions 1000000 '$tr'" }
}

# ---- 4. results -----------------------------------------------------------------------
Write-Host "[4/4] Results"
& python $tuner --summarize $Output
Write-Host ""
Write-Host "Results dir:  $Output"
Write-Host "Prefetcher:   $exe"
