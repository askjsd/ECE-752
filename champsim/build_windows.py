"""Build the existing native Windows checkout, including paths with spaces."""

import os
import re
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path


@contextmanager
def short_drive(path):
    """Give GNU Make a temporary, space-free view of the same files."""
    for letter in "ZYXWVUTSRQPONMLKJIHGFED":
        drive = letter + ":"
        if not os.path.exists(drive + "\\"):
            subprocess.run(["subst", drive, str(path)], check=True)
            try:
                yield Path(drive + "\\")
            finally:
                subprocess.run(["subst", drive, "/D"], check=True)
            return
    raise RuntimeError("No free drive letter is available for the build")


def build_native(upstream, pin, jobs=4, config=None):
    llvm = Path(r"C:\Program Files\LLVM\bin")
    make = Path(r"C:\msys64\usr\bin\make.exe")
    if not make.is_file():
        found = shutil.which("make.exe")
        if not found:
            raise RuntimeError("GNU Make from MSYS2 is required")
        make = Path(found)
    env = os.environ.copy()
    env["PATH"] = os.pathsep.join(
        [str(llvm), str(make.parent), str(Path(sys.executable).parent), env.get("PATH", "")]
    )
    # Keep Windows command switches intact when MSYS invokes cmd.exe.
    env["MSYS2_ARG_CONV_EXCL"] = "*"
    runtime = upstream / "vcpkg_installed" / "x64-windows" / "bin"
    if not (runtime.parent / "include" / "CLI" / "CLI.hpp").is_file():
        raise RuntimeError("Install the x64-windows vcpkg manifest dependencies first")

    with short_drive(upstream) as root, short_drive(pin) as pin_root:
        subprocess.run(
            [sys.executable, "config.sh", str(config or upstream.parent / "champsim_config.json")],
            cwd=root, env=env, check=True,
        )
        # Native Python emits backslashes; preserve Make's line continuations.
        fragment = root / "_configuration.mk"
        generated = re.sub(r"\\(?!\r?$)", "/", fragment.read_text(), flags=re.M)
        generated = re.sub(
            r"(\$\(BIN_ROOT\)/[^\s:]+)",
            lambda match: match[0] if match[0].endswith(".exe") else match[0] + ".exe",
            generated,
        )
        fragment.write_text(generated)
        (root / "absolute.options").write_text(
            "-Iinc -isystem vcpkg_installed/x64-windows/include\n"
        )
        # The small driver includes the upstream rules and leaves source files intact.
        # Test dependencies are loaded only when building tests, avoiding unrelated
        # Catch2 v2-only test headers during a normal simulator build.
        driver = root / ".csconfig" / "windows.mk"
        contents = (root / "Makefile").read_text()
        contents = contents.replace(
            "TEST $(build_ids)", "$(build_ids)"
        ).replace("$(test_base_objs) $(base_module_objs))", "$(base_module_objs))")
        contents = contents.replace("-llibz", "-lzlib")
        # This driver lives below the root, so use the current directory as root.
        contents = re.sub(r"^override ROOT_DIR = .*", "override ROOT_DIR = $(CURDIR)", contents, count=1, flags=re.M)
        driver.write_text(contents)
        subprocess.run(
            [str(make), "-f", ".csconfig/windows.mk", "-o", "absolute.options",
             f"-j{jobs}", "CXX=clang++", "CC=clang",
             "TRIPLET_DIR=vcpkg_installed/x64-windows",
             "CXXFLAGS=-Wno-c++11-narrowing -fms-runtime-lib=dll", "LDLIBS=-lshell32"],
            cwd=root, env=env, check=True,
        )
        subprocess.run(
            [str(make), f"PIN_ROOT={pin_root.as_posix().rstrip('/')}", "CMD=cmd /C", "tools"],
            cwd=root / "tracer" / "pin", env=env, check=True,
        )

    # Make the simulator runnable from an ordinary PowerShell session.
    for dll in runtime.glob("*.dll"):
        shutil.copy2(dll, upstream / "bin" / dll.name)
    print("Built native ChampSim and the Intel PIN tracer.", flush=True)
