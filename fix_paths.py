path = r'C:\Users\yogev\Documents\ECE-752\champsim\run_knob_tuning_windows.py'
with open(path, 'r') as f:
    content = f.read()

# Add _win_to_wsl function before the build function
helper = '''def _win_to_wsl(path):
    """Convert a Windows absolute path to a WSL path (C:\\foo -> /mnt/c/foo)."""
    p = str(path)
    if len(p) >= 2 and p[1] == ":":
        drive = p[0].lower()
        rest = p[2:].replace("\\", "/")
        return f"/mnt/{drive}{rest}"
    return p


'''
content = content.replace(
    '# ---- 2. build (via WSL for speed) ------------------------------------------------------',
    helper + '# ---- 2. build (via WSL for speed) ------------------------------------------------------'
)

# Replace the three path conversion lines
content = content.replace(
    "f\"cp -r {str(SOURCE_WINDOWS).replace('\\\\\\\\\\\\\\\\', '/')}/* {wsl_build}/\"",
    "f\"cp -r {_win_to_wsl(SOURCE_WINDOWS)}/* {wsl_build}/\""
)
content = content.replace(
    "f\"cp {str(CONFIG).replace('\\\\\\\\\\\\\\\\', '/')} {wsl_build}/champsim_config_knobbed.json\"",
    "f\"cp {_win_to_wsl(CONFIG)} {wsl_build}/champsim_config_knobbed.json\""
)
content = content.replace(
    "f\"cp -r {str(MODULES).replace('\\\\\\\\\\\\\\\\', '/')} {wsl_build}/knobbed_modules\"",
    "f\"cp -r {_win_to_wsl(MODULES)} {wsl_build}/knobbed_modules\""
)

with open(path, 'w') as f:
    f.write(content)
print('Done')
