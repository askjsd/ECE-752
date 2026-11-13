"""Run Windows-targeting MinGW GCC commands through WSL."""

import os
import re
import shlex
import subprocess


def available():
    result = subprocess.run(
        ["wsl.exe", "--", "sh", "-lc",
         "command -v x86_64-w64-mingw32-gcc"],
        capture_output=True, text=True,
    )
    return result.returncode == 0


def path(path):
    absolute = os.path.abspath(path)
    match = re.match(r"^([A-Za-z]):[\\/](.*)$", absolute)
    if not match:
        raise ValueError("WSL MinGW requires a Windows drive path: " + absolute)
    return "/mnt/" + match.group(1).lower() + "/" + match.group(2).replace("\\", "/")


def run(args, **kwargs):
    translated = []
    for arg in args:
        if os.path.isabs(arg) and re.match(r"^[A-Za-z]:[\\/]", arg):
            translated.append(path(arg))
        else:
            translated.append(arg)
    command = " ".join(shlex.quote(str(arg)) for arg in translated)
    return subprocess.run(
        ["wsl.exe", "--", "sh", "-lc", command],
        **kwargs,
    )
