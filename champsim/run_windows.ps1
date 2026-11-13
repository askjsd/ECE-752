[CmdletBinding()]
param(
    [string]$Kernel = "k_stencil",
    [string]$PinRoot = "",
    [string]$Output = "",
    [int]$TraceInstructions = 2000000,
    [int]$WarmupInstructions = 100000,
    [int]$EvaluationSeeds = 100,
    [string]$EvaluationBudgets = "10,20,50,100,200",
    [int]$NFlags = 12,
    [switch]$NoGraphs,
    [switch]$Build
)

$ErrorActionPreference = "Stop"
$evaluationBudgetValues = @($EvaluationBudgets -split "," | ForEach-Object {
        $value = 0
        if (-not [int]::TryParse($_.Trim(), [ref]$value) -or $value -lt 1) {
            throw "Evaluation budgets must be positive integers separated by commas."
        }
        $value
    })
$champsimDir = (Get-Item $PSScriptRoot).FullName
$repo = (Get-Item (Join-Path $champsimDir "..")).FullName
$upstream = Join-Path $champsimDir ".upstream-windows"
$tracerDir = Join-Path $upstream "tracer\pin"
$tracer = Join-Path $tracerDir "obj-intel64\champsim_tracer.dll"
$simulator = Join-Path $upstream "bin\champsim.exe"

function Require-Path([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$Description was not found: $Path"
    }
}

function Find-CommandPath([string]$Name, [string]$Description) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw "$Description was not found on PATH: $Name"
    }
    return $command.Source
}

Require-Path $upstream "Windows ChampSim checkout"

if ([string]::IsNullOrWhiteSpace($PinRoot)) {
    $pinCandidate = Get-ChildItem (Join-Path $env:USERPROFILE "Downloads") -Directory -Filter "pin-external-*-windows" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -ne $pinCandidate) {
        $nested = Join-Path $pinCandidate.FullName $pinCandidate.Name
        $PinRoot = if (Test-Path (Join-Path $nested "pin.exe")) { $nested } else { $pinCandidate.FullName }
    }
}
Require-Path (Join-Path $PinRoot "pin.exe") "Intel PIN installation"

$standardLlvmBin = "C:\Program Files\LLVM\bin"
if (Test-Path (Join-Path $standardLlvmBin "clang.exe")) {
    $env:Path = "$standardLlvmBin;$env:Path"
}

$null = Find-CommandPath "python" "Python"
$null = Find-CommandPath "clang" "LLVM Clang"
$null = Find-CommandPath "llvm-objcopy" "LLVM objcopy"

$llvmBin = Split-Path (Find-CommandPath "clang" "LLVM Clang") -Parent
$runtimeBin = Join-Path $upstream "vcpkg_installed\x64-windows\bin"
$env:Path = "$llvmBin;$runtimeBin;$env:Path"

if ($Build) {
    $make = Get-Command make -ErrorAction SilentlyContinue
    if ($null -eq $make) {
        throw "GNU Make was not found on PATH; install the native build prerequisites or omit -Build."
    }
    Push-Location $upstream
    try {
        & $make.Source "CXX=clang++" "CC=clang" "CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll" "LDLIBS=-lshell32"
        if ($LASTEXITCODE -ne 0) { throw "ChampSim build failed." }
    } finally {
        Pop-Location
    }

    Push-Location $tracerDir
    try {
        & $make.Source "PIN_ROOT=$PinRoot"
        if ($LASTEXITCODE -ne 0) { throw "PIN tracer build failed." }
    } finally {
        Pop-Location
    }
}

Require-Path $simulator "Native ChampSim executable"
Require-Path $tracer "Windows ChampSim PIN tracer"

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $champsimDir ("results\" + $Kernel + "-windows")
}

& python (Join-Path $champsimDir "run_carbs.py") `
    --repo $repo `
    --champsim-root $upstream `
    --pin $PinRoot `
    --tracer $tracer `
    --out $Output `
    --kernel $Kernel `
    --nflags $NFlags `
    --trace-instructions $TraceInstructions `
    --warmup-instructions $WarmupInstructions

if ($LASTEXITCODE -ne 0) {
    throw "Native ChampSim CARBS run failed."
}

if (-not $NoGraphs) {
    $graphs = Join-Path $Output "graphs"
    $flagbenchDir = Join-Path $repo "flagbench"
    $evaluationArgs = @(
        (Join-Path $flagbenchDir "evaluate.py"),
        "--luts", $Output,
        "--out", $graphs,
        "--kernels", $Kernel,
        "--seeds", $EvaluationSeeds,
        "--budgets"
    ) + @($evaluationBudgetValues | ForEach-Object { [string]$_ }) + @(
        "--min-headroom", "0",
        "--min-denom-frac", "0"
    )
    Push-Location $flagbenchDir
    try {
        & python @evaluationArgs
    } finally {
        Pop-Location
    }
    if ($LASTEXITCODE -ne 0) {
        throw "FlagBench graph/evaluation generation failed."
    }
}

Write-Host ""
Write-Host "Native ChampSim CARBS run complete:"
Write-Host "  Results: $Output"
Write-Host "  LUT:     $(Join-Path $Output 'champsim.csv')"
Write-Host "  CARBS:   $(Join-Path $Output 'carbs.json')"
if (-not $NoGraphs) {
    Write-Host "  Graphs:  $(Join-Path $Output 'graphs')"
}
