# Native Windows setup

This setup uses the existing `champsim/.upstream-windows` source tree. No
second ChampSim or ECE-752 checkout is required. Run the commands below from
the ECE-752 repository root in PowerShell.

## Tools

- Python 3.12, NumPy, and Matplotlib run the configuration and experiment scripts.
- MSYS2 GNU Make builds ChampSim and the tracer.
- LLVM Clang builds the native Windows simulator.
- Visual Studio 2022 Build Tools, with the C++ workload and Windows SDK,
  supply the Windows headers, standard library, and linker used by Clang.
- Intel PIN 4.4 records Windows executable instruction traces.
- Ubuntu WSL with `gcc-mingw-w64-x86-64` compiles Windows benchmark executables
  using the GCC optimization flags expected by FlagBench. ChampSim and PIN
  themselves run on Windows.

The tools installed for this machine are located at:

| Tool | Location |
| --- | --- |
| LLVM | `C:\Program Files\LLVM\bin` |
| GNU Make | `C:\msys64\usr\bin\make.exe` |
| Microsoft C++ tools | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| Intel PIN | `%USERPROFILE%\Downloads\pin-external-4.4-99977-g54fbb8814-clang-windows` |
| vcpkg | `C:\champsim-deps\vcpkg` |
| pkg-config | `C:\champsim-deps\pkgconf\mingw64\bin\pkg-config.exe` |

Ubuntu is the default WSL distribution, because the experiment runner invokes
`wsl.exe` without selecting a distribution.

## C++ libraries

The dependency manager uses the vcpkg `2024.11.16` snapshot. Its own tools and
build files live outside OneDrive to avoid Windows' 260-character path limit.
The libraries are installed inside the existing ChampSim tree.

To reinstall or verify the manifest dependencies:

```powershell
$env:PKG_CONFIG = 'C:\champsim-deps\pkgconf\mingw64\bin\pkg-config.exe'
$env:VCPKG_KEEP_ENV_VARS = 'PKG_CONFIG'
Push-Location .\champsim\.upstream-windows
try {
    & C:\champsim-deps\vcpkg\vcpkg.exe install --triplet x64-windows --disable-metrics
    if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
} finally {
    Pop-Location
}
```

The explicit pkg-config path avoids a retired MSYS2 runtime download referenced
by this vcpkg snapshot. The native pkg-config executable was extracted from the
MSYS2 package downloaded and checksum-checked by vcpkg.

## Build

```powershell
python .\champsim\run_windows.py --build-only
```

`build_windows.py` temporarily assigns unused drive letters to the existing
source and PIN directories. It normalizes generated Make paths, builds the
simulator and tracer, copies the required release DLLs next to the simulator,
and removes its temporary drive mappings. This supports the checkout's spaces
without copying or relocating its source.

The simulator build does not load unrelated C++ test dependencies. The original
upstream Makefile is preserved. The raw and compressed trace readers explicitly
open files in binary mode, preventing Windows text-mode EOF and newline handling
from truncating or changing instruction records.
The cache miss lookup also avoids comparing iterators from different queues,
which caused a deadlock with the Windows standard library.

Outputs:

- `champsim/.upstream-windows/bin/champsim.exe`
- `champsim/.upstream-windows/tracer/pin/obj-intel64/champsim_tracer.dll`

## Baseline

Run a bounded `k_stencil` baseline and one additional flag configuration:

```powershell
$pin = Join-Path $env:USERPROFILE 'Downloads\pin-external-4.4-99977-g54fbb8814-clang-windows'
python .\champsim\run_carbs.py `
  --repo . `
  --champsim-root .\champsim\.upstream-windows `
  --pin $pin `
  --tracer .\champsim\.upstream-windows\tracer\pin\obj-intel64\champsim_tracer.dll `
  --out .\champsim\results\windows-baseline-20261120 `
  --kernel k_stencil --nflags 1 --config-ids 0 --jobs 1 `
  --trace-instructions 2000000 --trace-skip-instructions 1000 `
  --warmup-instructions 100000 --simulation-timeout 300 --determinism 1
```

The runner separately compiles a plain `-O3` baseline and an `-O0` correctness
reference. Configuration 0 adds `-fno-unroll-loops` to `-O3`. It checks native
program checksums, records traces, and simulates the configured architecture.
The determinism option repeats a measured configuration's saved trace.

`champsim.json` contains `o3_cycles`, `o3_instructions`, and `o3_ipc` for the
plain baseline. The CSV contains the sampled flag configuration. Full simulator
output is retained under `simulations`. Generated files are ignored by Git.

These are bounded trace measurements for validating the installation. They are
not a full-program timing result or an exhaustive flag-space study.

## Verified results: November 20, 2026

| Check | Result |
| --- | --- |
| Plain `-O3` kernel | `k_stencil` |
| Measured retired instructions | 2,000,002 |
| Simulated cycles | 1,567,427 |
| Reported IPC | 1.276 |
| Reference and optimized checksum | `4819639163407726757` |
| Same-trace determinism | Passed; identical cycle count |
| Plain `-O3` vs. configuration 0 | Identical `.text` hash; one simulation reused |
| Python tuner checks | 5 passed |
| Raw/gzip/xz/bzip2 equivalence | All four: 200,001 instructions, 156,077 cycles, IPC 1.281 |

Warmup retired 100,003 instructions. ChampSim retires instructions in groups,
so the instruction counts can slightly exceed the requested thresholds. The
legacy runner records 2,000,000 instructions and replays the trace once to cover
warmup plus the measurement window. The compressed-format check uses a longer
input than its requested window and does not replay it. Its input includes both
the Windows text EOF byte and CRLF bytes.

Baseline data: `results/windows-baseline-20261120/champsim.json` and
`results/windows-baseline-20261120/champsim.csv`. Build logs, format validation,
and diagnostic output are under `results/windows-setup-20261120`. The earlier
text-mode measurement is explicitly stored as `invalid-text-mode-baseline`
there and must not be used as a performance result.

Official installation references:
[Microsoft C++ build tools](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=visualstudio),
[Intel PIN downloads](https://www.intel.com/content/www/us/en/developer/articles/tool/pin-a-binary-instrumentation-tool-downloads.html).
