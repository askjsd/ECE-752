import numpy as np
import time
import os
import json
import subprocess
import torch
import copy
from utils import *
import sys
from glob import glob


def time_parsing(stderr_output):
    tmp = [i.strip() for i in str(stderr_output)[:-1].strip().split('\n') if 'timing' in i]
    tmp = [i.split('timing:')[-1].strip()[:-2] for i in tmp]
    if len(tmp) == 0:
        return 0
    else:
        t = []
        for i in tmp:
            i = i.split(' ')
            try:
                t.append(int(i[1].strip()) - int(i[0].strip()))
            except:
                continue
    return sum(t)


class CompilerEnv():
    def __init__(self, compiler_type, optimizations, run_dir, reward_type = 'run_time', algorithm = 'a2c', seed = 42, steps = 200, time_limitation = None, batch_size = 5, render_mode = None, lr = None):
        super(CompilerEnv, self).__init__()
        if 'liver' in run_dir.lower() and '-fpack-struct' in optimizations:
            optimizations.remove('-fpack-struct')
        self.optimizations = optimizations
        self.compiler_type = compiler_type
        self.run_dir = run_dir
        self.time_limitation = time_limitation
        self.random_seed = seed
        self.state = []
        self.lr = lr
        self.op_his = {}
        pid = self.run_dir.split('/')[-1]
        self.benchmark = pid
        os.chdir(self.run_dir)
        if 'xcnd' in os.popen(f'hostname').read():
            self.build_env(pid)
        if 'spec' not in self.run_dir.lower():
            self.compile_config = json.load(open('new_config.f'))
            self.compile_config['run'] = [self.compile_config['run']]
            self.pre_run()
        self.batch_size = batch_size
        self.parallel = False
        self.begin_time = time.time()
        self.timeout = None

    def spec_parsing(self):
        filename = glob(f'/tmp/_test{self.benchmark}/result/*.rsf')
        if len(filename) == 0:
            return np.inf
        filename = filename[0]
        os.popen(f'cp {filename} Project/spec_bench/{self.benchmark}-{filename.split("/")[-1]}').read()
        tmp = open(filename).readlines()
        report_time = [float(i.split(' ')[-1].strip()) for i in tmp if 'reported_time' in i]
        status = [i.split('.valid')[1:] for i in tmp if '.valid' in i][-1][-1]
        os.popen(f'rm -f {filename}').read()
        if 'S' in status:
            return report_time
        else:
            return np.inf

    def compile(self, op_seq, src_folder):
        if self.compiler_type == 'gcc':
            return self.compile_gcc(op_seq, src_folder)
        else:
            return self.compile_llvm(op_seq)
    
    def run(self, op_seq):
        if len(op_seq) > 1:
            return self.run_parallel(op_seq, len(op_seq))
        else:
            return self.run_single(op_seq[0])

    def run_single(self, op_seq, src_folder = ''):
        if 'spec' in self.run_dir.lower():
            p = open(f'/tmp/_test{self.benchmark}/test.cfg').readlines()
            if self.benchmark in ['pop2', 'wrf', 'cam4']:
                #according to https://www.spec.org/cpu2017/Docs/benchmarks/628.pop2_s.html
                p[309] = f'  OPTIMIZE     = -g {op_seq} -fallow-argument-mismatch  -march=native\n'
            else:
                p[309] = f'  OPTIMIZE     = -g {op_seq}  -march=native\n'
            with open(f'/tmp/_test{self.benchmark}/test.cfg', 'w') as f:
                f.write(''.join(p))
            if self.benchmark in ['cam4']:
                #https://www.spec.org/cpu2017/Docs/faq.html, cam4 needs to reserve 128MB stack space
                #os.popen(f'ulimit -s unlimited && /mnt/scratch/liwei/Project/spec_src/bin/runcpu --size train --config=/tmp/_test{self.benchmark}/test.cfg --iterations=1 --output-root=/tmp/_test{self.benchmark} {self.benchmark}_s').read()
                process = subprocess.Popen(f'ulimit -s unlimited && /Project/spec_src1/bin/runcpu --size train --config=/tmp/_test{self.benchmark}/test.cfg --iterations=1 --output-root=/tmp/_test{self.benchmark} {self.benchmark}_s', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
            else:
                process = subprocess.Popen(f'Project/spec_src1/bin/runcpu --size train --config=/tmp/_test{self.benchmark}/test.cfg --iterations=1 --output-root=/tmp/_test{self.benchmark} {self.benchmark}_s', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
            try:
                output, error = process.communicate(timeout = 36000)
                run_time = self.spec_parsing()
            except:
                run_time = np.inf
            return [(0, run_time)]
        else:
            status, compile_time = self.compile(op_seq, src_folder)
            if status:
                run_time = self.get_timing_result()
            else:
                run_time = 0
            if run_time == 0:
                compile_time = -1
                run_time = np.inf
            return [(compile_time, run_time)]

    def run_sub(self, idx, op_seq):
        src_folder = os.getcwd()
        os.chdir(f'/temp/_test{idx}')
        return {idx: self.run_single(op_seq, src_folder)[0]}

    def pre_run(self):
        status, compile_time = self.compile('', '')
        if not status:
            sys.exit(0)
        if 'WCETBench' in self.run_dir:
            tmp_measure = self.measure(f' 1 ', 1200)
        else:
            tmp_measure = self.measure('', 1200)
        for idx, check_file in enumerate(self.compile_config['check']):
            os.popen(f'cp {check_file} old_{idx}').read()

    def build_env(self, idx):
        #prepare parallel run environment, build local directory
        os.popen(f'mkdir -p /tmp/_test{idx}').read()
        if 'spec' in os.getcwd().lower():
            #os.popen(f'cp /mnt/scratch/liwei/Project/spec_src/config/test.cfg /tmp/_test{idx}/').read()
            os.popen(f'cp /mnt/scratch/e/e0509838/Project/spec_src1/config/test.cfg /tmp/_test{idx}/').read()
        else:
            os.popen(f'rsync -av --exclude="*.pkl" ./ /tmp/_test{idx}/').read()
        os.chdir(f'/tmp/_test{idx}')
    
    def build_parallel_env(self, num_parallel):
        #prepare parallel run environment, build local directory
        for i in range(num_parallel):
            self.build_env(i)
            self.parallel = True
          
    def compile_gcc(self, op_seq, src_folder = ''):
        if 'spec' in self.run_dir.lower():
            src_folder = self.run_dir
            os.popen(f'make clean').read()
            s = time.time()
            os.popen(f"make OPTIMIZE='{op_seq}'").read()
            compile_time = time.time() - s
            return os.path.exists(f'{config["exe_file"]}'), compile_time
        else:
            #clean and build executable file
            load_lib = ''
            if os.path.exists('libfunc.so'):
                load_lib = '-L. -lfunc'
            s = time.time()
            config = self.compile_config
            m = os.popen(f'rm -f {config["exe_file"]}; rm -f *.o').read()
            for f in config['files']:
                #for c++
                if f.endswith('.cpp') or f.endswith('.cc'):
                    CC = 'g++ -w '
                    #op_seq = op_seq.replace('-fpack-struct', '').replace('-fno-pack-struct', '')
                else:
                    CC = 'gcc -w '
                if src_folder:
                    m = os.popen(f'{CC} {op_seq} -I{src_folder}/ {config["lib"].replace("-I", f"-I{src_folder}/")} -c {src_folder}/{f} -o {f.replace("/", "_").replace("..", "").split(".")[0]}.o {load_lib} > tmp').read()
                else:
                    m = os.popen(f'{CC} {op_seq} {config["lib"]} -c {f} -o {f.replace("/", "_").replace("..", "").split(".")[0]}.o {load_lib} > tmp').read()
                
            if src_folder:
                m = os.popen(f'{CC} {op_seq} *.o -o {config["exe_file"]} -I{src_folder}/ {config["link_lib"].replace("-I", f"-I{src_folder}")} {load_lib} > tmp').read()
            else:
                m = os.popen(f'{CC} {op_seq} *.o -o {config["exe_file"]} {config["link_lib"]} {load_lib} > tmp').read()
            compile_time = time.time() - s
            return os.path.exists(f'{config["exe_file"]}'), compile_time

    def measure(self, run_cmd, timeout):
        s = time.time()
        for cmd in self.compile_config['run']:
            process = subprocess.Popen(f'LD_LIBRARY_PATH=. ./{self.compile_config["exe_file"]} {run_cmd} {cmd}', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
            try:
                output, error = process.communicate(timeout = timeout)
                tmp = time_parsing(error)
            except:
                return 0
        e = time.time() - s
        if 'spec' in self.run_dir.lower() or 'origin' in self.run_dir.lower():
            return e
        else:
            return tmp

    def get_timing_result(self):
        if 'spec' in self.run_dir.lower():
            run_count = 1
            timeout = 1000
        elif 'origin' in self.run_dir.lower() or 'MediaBench_h264enc' in self.run_dir:
            run_count = 5
            timeout = 1000
        elif 'PolyBench' in self.run_dir or 'livermorec' in self.run_dir:
            run_count = 10
            timeout = 60
        else:
            run_count = 10
            timeout = 60

        res = []
        run_cmd = self.compile_config['run']

        if 1:
            if 'WCETBench' in self.run_dir:
                tmp_measure = self.measure(f' 1 ', timeout)
            else:
                tmp_measure = self.measure('', timeout)
            
            #check the correctness of the optimization
            for idx, check_file in enumerate(self.compile_config['check']):
                try:
                    tmp = os.popen(f'diff {check_file} old_{idx}').read()
                except:
                    return 0
                if tmp != '':
                    return 0
            
        for i in range(run_count):
            tmp_measure = self.measure('', timeout)
            if tmp_measure == 0:
                return 0
            res.append(tmp_measure)
        os.popen(f'rm -f {self.compile_config["exe_file"]}').read()
        return res