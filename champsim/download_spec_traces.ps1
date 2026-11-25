# Download SPEC CPU 2017 ChampSim traces (DPC-3) on Windows.
# Usage:  powershell -ExecutionPolicy Bypass -File .\download_spec_traces.ps1
#         .\download_spec_traces.ps1 -Dest D:\traces -Only 605.mcf_s-1554B,619.lbm_s-2676B
# Resumable: rerun after an interruption and partial files continue. Already-verified files are skipped.
param(
  [string]$Dest = "$HOME\spec-traces",
  [string[]]$Only
)

$base  = "https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu"
$names = @(
 "605.mcf_s-1554B","605.mcf_s-1536B","620.omnetpp_s-874B","620.omnetpp_s-141B",
 "623.xalancbmk_s-592B","623.xalancbmk_s-165B","619.lbm_s-2676B","619.lbm_s-4268B",
 "603.bwaves_s-5359B","603.bwaves_s-3699B","649.fotonik3d_s-1B","649.fotonik3d_s-1176B",
 "602.gcc_s-734B","602.gcc_s-2375B","607.cactuBSSN_s-4248B","607.cactuBSSN_s-3477B",
 "654.roms_s-1021B","654.roms_s-294B"
)
if ($Only) { $names = $names | Where-Object { $Only -contains $_ } }

if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) { throw "curl.exe not found (needs Windows 10 1803+)." }
$haveTar = [bool](Get-Command tar.exe -ErrorAction SilentlyContinue)

New-Item -ItemType Directory -Force $Dest | Out-Null
$log = Join-Path $Dest "dl.log"

foreach ($n in $names) {
  $f   = "$n.champsimtrace.xz"
  $out = Join-Path $Dest $f
  $url = "$base/$f"

  # Skip if file is already present and passes the integrity test
  if ((Test-Path $out) -and $haveTar) {
    tar.exe -tJf $out *> $null
    if ($LASTEXITCODE -eq 0) { "SKIP (verified) $f" | Tee-Object -FilePath $log -Append; continue }
  }

  curl.exe -L --fail --retry 5 --retry-delay 5 -C - -o $out $url
  if ($LASTEXITCODE -ne 0) { "FAIL download $f" | Tee-Object -FilePath $log -Append; continue }

  if ($haveTar) {
    tar.exe -tJf $out *> $null   # full decompress test, like `xz -t`
    if ($LASTEXITCODE -ne 0) { "FAIL verify $f" | Tee-Object -FilePath $log -Append; continue }
  }
  "OK $f" | Tee-Object -FilePath $log -Append
}
