"""Build three L1I policies and compare them on fixed, non-replaying traces.

This is a bounded port validation, not a reproduction of the ISCA evaluation.
All three policies share the same core, caches, and virtual-address interface.
"""
import argparse
import gzip
import hashlib
import json
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
UPSTREAM=ROOT/'champsim/.upstream-windows'


def make_trace(path):
    record=struct.Struct('<QBB2B4B2Q4Q')
    assert record.size==64
    with gzip.open(path,'wb') as out:
        # 1024 two-line regions: 128 KiB of instructions, larger than L1I.
        # A fixed permutation creates repeatable nonsequential transitions.
        for repeat in range(16):
            for visit in range(1024):
                region=(visit*37)%1024
                for instruction in range(32):
                    ip=0x100000+region*128+instruction*4
                    branch=int(instruction==31)
                    out.write(record.pack(ip,branch,branch,26 if branch else 0,0,0,0,0,0,0,0,0,0,0,0))


def run(sim,trace,warmup,instructions,log):
    args=[str(sim),'--warmup-instructions',str(warmup),
          '--simulation-instructions',str(instructions),str(trace)]
    with log.open('w') as stream:
        result=subprocess.run(args,stdout=stream,stderr=subprocess.STDOUT,timeout=180)
    output=log.read_text()
    if result.returncode or 'Reached end of trace' in output:
        raise RuntimeError(f'Failed or replaying simulation: {log}')
    matches=re.findall(r'CPU 0 cumulative IPC: ([0-9.]+) instructions: (\d+) cycles: (\d+)',output)
    if not matches:
        raise RuntimeError(f'Missing statistics: {log}')
    ipc,ins,cycles=matches[-1]
    metrics={'instructions':int(ins),'cycles':int(cycles),'ipc':float(ipc)}
    for label,pattern in {
        'l1i_load_misses':r'cpu0_L1I\s+LOAD\s+ACCESS:\s*\d+\s+HIT:\s*\d+\s+MISS:\s*(\d+)',
        'prefetch_issued':r'cpu0_L1I PREFETCH REQUESTED:\s*\d+ ISSUED:\s*(\d+)',
        'prefetch_useful':r'cpu0_L1I PREFETCH REQUESTED:.*?USEFUL:\s*(\d+)',
        'prefetch_useless':r'cpu0_L1I PREFETCH REQUESTED:.*?USELESS:\s*(\d+)',
    }.items():
        found=re.findall(pattern,output)
        if found: metrics[label]=int(found[-1])
    diagnostic=re.search(r'\[entangling\] ([^\n]+)',output)
    if diagnostic:
        metrics['entangling']={key:int(value) for key,value in re.findall(r'(\w+)=(\d+)',diagnostic[1])}
    return metrics


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-build',action='store_true')
    parser.add_argument('--real-trace',type=Path,default=ROOT/'champsim/results/windows-setup-20261120/trace-format-check/window.champsim')
    parser.add_argument('--out',type=Path,default=ROOT/'champsim/results/entangling-review-20261120')
    args=parser.parse_args()
    out=args.out.resolve()
    out.mkdir(parents=True,exist_ok=True)
    base=json.loads((ROOT/'champsim/champsim_config.json').read_text())
    binaries={}
    for policy,module in [('none','no'),('next-line','next_line'),('entangling','entangling')]:
        config=json.loads(json.dumps(base))
        config['executable_name']='champsim-check-'+policy
        config['L1I'].update(prefetcher=module,virtual_prefetch=True,prefetch_activate='LOAD')
        cfg=out/(policy+'.json')
        cfg.write_text(json.dumps(config,indent=2)+'\n')
        binaries[policy]=UPSTREAM/'bin'/(config['executable_name']+'.exe')
        if not args.skip_build:
            with (out/('build-'+policy+'.log')).open('w') as log:
                subprocess.run([sys.executable,str(ROOT/'champsim/run_windows.py'),
                                '--build-only','--build-config',str(cfg)],cwd=ROOT,
                               stdout=log,stderr=subprocess.STDOUT,check=True)
        if not binaries[policy].is_file():
            raise RuntimeError('Missing simulator: '+str(binaries[policy]))
    synthetic=out/'instruction-footprint-128k.champsim.gz'
    make_trace(synthetic)
    workloads=[('synthetic-128k',synthetic,32000,400000)]
    if args.real_trace.is_file():
        workloads.append(('windows-stencil-window',args.real_trace,10000,200000))
    report={'description':'Bounded port validation; synthetic trace is not a benchmark result',
            'reference_commit':'75c77536ac4b1fa1a801587df444ee1501f1b3aa',
            'results':{},'binary_sha256':{k:hashlib.sha256(p.read_bytes()).hexdigest() for k,p in binaries.items()}}
    for name,trace,warmup,instructions in workloads:
        policies={}
        for policy,sim in binaries.items():
            metrics=run(sim,trace,warmup,instructions,out/(name+'-'+policy+'.txt'))
            policies[policy]=metrics
            print(name,policy,metrics,flush=True)
        rerun=run(binaries['entangling'],trace,warmup,instructions,out/(name+'-entangling-repeat.txt'))
        if rerun!=policies['entangling']:
            raise RuntimeError('Entangling repeat changed results: '+name)
        report['results'][name]={'warmup':warmup,'requested_instructions':instructions,
                                 'policies':policies,'entangling_deterministic':True}
    (out/'comparison.json').write_text(json.dumps(report,indent=2)+'\n')
    with (out/'comparison.csv').open('w') as stream:
        stream.write('workload,policy,instructions,cycles,ipc,l1i_load_misses,prefetch_issued,prefetch_useful,prefetch_useless\n')
        for name,result in report['results'].items():
            for policy,m in result['policies'].items():
                stream.write(','.join(map(str,[name,policy]+[m.get(k,'') for k in
                  ['instructions','cycles','ipc','l1i_load_misses','prefetch_issued','prefetch_useful','prefetch_useless']]))+'\n')
    print('Saved comparison:',out/'comparison.json')


if __name__=='__main__':
    main()
