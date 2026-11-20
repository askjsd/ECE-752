"""Compile focused Entangling tests using the already-built Windows objects."""
import os
import subprocess
from pathlib import Path

repo=Path(__file__).resolve().parents[2]
upstream=repo/'champsim/.upstream-windows'
out=repo/'champsim/results/entangling-review-20261120'
out.mkdir(parents=True,exist_ok=True)
objects=[str(p) for p in (upstream/'.csconfig').rglob('*.o')
         if '_main.o' not in p.name and '/test/' not in p.as_posix()
         and p.name!='entangling.o']
command=[r'C:\Program Files\LLVM\bin\clang++.exe','-std=c++17','-O1',
         '-fms-runtime-lib=dll','-Wno-c++11-narrowing',
         '-I'+str(upstream/'inc'),'-I'+str(upstream/'.csconfig'),
         '-I'+str(upstream/'vcpkg_installed/x64-windows/include'),
         str(Path(__file__).with_name('test_entangling.cc')),*objects,
         '-L'+str(upstream/'vcpkg_installed/x64-windows/lib'),
         '-lCLI11','-llzma','-lzlib','-lbz2','-lfmt','-lshell32',
         '-o',str(out/'test-entangling.exe')]
with (out/'test-build.log').open('w') as log:
    subprocess.run(command,cwd=upstream,stdout=log,stderr=subprocess.STDOUT,check=True)
env=os.environ.copy()
env['PATH']=str(upstream/'bin')+os.pathsep+env['PATH']
result=subprocess.run([str(out/'test-entangling.exe')],env=env,text=True,capture_output=True)
(out/'tests.txt').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr,end='')
raise SystemExit(result.returncode)
