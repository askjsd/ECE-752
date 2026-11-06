from glob import glob
import json
import os 
import pickle
import numpy as np
import subprocess
import time
import copy

def build_env(idx):
    #prepare parallel run environment, build local directory
    os.popen(f'mkdir -p /temp/_test{idx}').read()
    if 'spec' in os.getcwd().lower():
        os.popen(f'cp -r ../../spec_programs/{idx}/* /temp/_test{idx}').read()
    else:
        os.popen(f'rsync -av --exclude="*.pkl" ./ /temp/_test{idx}/').read()
    os.chdir(f'/temp/_test{idx}')

def O_optimization(level):
    lines = [i.strip() for i in os.popen(f'gcc -O{level} -Q --help=optimizers | grep enabled | cut -d\'[\' -f1').readlines()]
    return lines

def get_all_optimization():
    lines = [i.strip() for i in os.popen(f'gcc -O0 -Q --help=optimizers | cut -d\'[\' -f1').readlines()]
    lines = [i for i in lines if '-f' in i and '=' not in i and i!='']
    #remove
    lines = [i for i in lines if '-fno-threadsafe-statics' not in i]
    return lines

ALL_OPTIMIZATIONS = get_all_optimization()
O1_OPTIMIZATIONS = O_optimization(1)
options = [i for i in ALL_OPTIMIZATIONS if i not in O1_OPTIMIZATIONS]

to_do = [['RunBench/MediaBench_h264enc'], ['RunBench/PolyBench_3mm'], ['RunBench/MiBench_office_ghostscript', 'RunBench/MiBench_automotive_basicmath', 'RunBench/PolyBench_2d', 'RunBench/VersaBench_bmm', 'RunBench/MediaBench_unepic', 'RunBench/livermorec_livermorec1', 'RunBench/livermorec_livermorec17', 'RunBench/MiBench_security_rijndael_d', 'RunBench/livermorec_livermorec19', 'RunBench/WCETBench_compress', 'RunBench/PolyBench_doitgen'], ['RunBench/PolyBench_2mm', 'RunBench/MiBench_bzip2e', 'RunBench/MiBench_office_rsynth', 'RunBench/PolyBench_gemver', 'RunBench/MiBench_telecomm_gsm_u', 'RunBench/MediaBench_jpg2000enc', 'RunBench/PolyBench_bicg', 'RunBench/MiBench_network_patricia', 'RunBench/MediaBench_g721_e', 'RunBench/MiBench_network_dijkstra', 'RunBench/VersaBench_corner_turn', 'RunBench/PolyBench_imper'], ['RunBench/PolyBench_symm', 'RunBench/PolyBench_trmm', 'RunBench/PolyBench_durbin', 'RunBench/VersaBench_dbms', 'RunBench/PolyBench_cholesky', 'RunBench/MediaBench_epic', 'RunBench/PolyBench_gesummv', 'RunBench/livermorec_livermorec12', 'RunBench/MediaBench_g721_d', 'RunBench/MiBench_automotive_qsort', 'RunBench/WCETBench_statemate', 'RunBench/WCETBench_ndes', 'RunBench/WCETBench_fir'], ['RunBench/MediaBench_h263enc', 'RunBench/livermorec_livermorec15', 'RunBench/MiBench_consumer_tiffmedian', 'RunBench/PolyBench_ludcmp', 'RunBench/MediaBench_h263dec', 'RunBench/MiBench_telecomm_adpcm_d', 'RunBench/livermorec_livermorec11', 'RunBench/livermorec_livermorec10', 'RunBench/MiBench_automotive_susan_e', 'RunBench/MiBench_security_rijndael_e', 'RunBench/livermorec_livermorec5', 'RunBench/WCETBench_nsichneu', 'RunBench/VersaBench_ecbdes', 'RunBench/VersaBench_8b10b'], ['RunBench/PolyBench_syr2k', 'RunBench/PolyBench_warshall', 'RunBench/MiBench_consumer_tiff2bw', 'RunBench/MediaBench_mpeg2dec', 'RunBench/livermorec_livermorec16', 'RunBench/livermorec_livermorec23', 'RunBench/livermorec_livermorec14', 'RunBench/VersaBench_beamformer', 'RunBench/MiBench_automotive_susan_c', 'RunBench/MiBench_automotive_bitcount', 'RunBench/livermorec_livermorec4', 'RunBench/livermorec_livermorec20', 'RunBench/WCETBench_edn', 'RunBench/VersaBench_802_11a'], ['RunBench/MediaBench_mpeg2enc', 'RunBench/MiBench_consumer_lame', 'RunBench/MiBench_consumer_jpeg_d', 'RunBench/MiBench_bzip2d', 'RunBench/MiBench_telecomm_gsm', 'RunBench/MiBench_telecomm_adpcm_c', 'RunBench/livermorec_livermorec7', 'RunBench/PolyBench_lu', 'RunBench/livermorec_livermorec9', 'RunBench/livermorec_livermorec2', 'RunBench/MiBench_telecomm_FFT', 'RunBench/MiBench_telecomm_FFT_i', 'RunBench/VersaBench_fmradio', 'RunBench/WCETBench_lms', 'RunBench/MiBench_telecomm_CRC32'], ['RunBench/PolyBench_gemm', 'RunBench/PolyBench_syrk', 'RunBench/MiBench_consumer_tiff2rgba', 'RunBench/livermorec_livermorec18', 'RunBench/livermorec_livermorec13', 'RunBench/PolyBench_dynprog', 'RunBench/livermorec_livermorec3', 'RunBench/livermorec_livermorec22', 'RunBench/MiBench_security_blowfish_d', 'RunBench/PolyBench_atax', 'RunBench/PolyBench_trisolv', 'RunBench/MiBench_security_sha', 'RunBench/WCETBench_whet'], ['RunBench/MediaBench_h264dec', 'RunBench/PolyBench_adi', 'RunBench/MiBench_consumer_jpeg_c', 'RunBench/PolyBench_gramschmidt', 'RunBench/livermorec_livermorec8', 'RunBench/PolyBench_mvt', 'RunBench/livermorec_livermorec21', 'RunBench/livermorec_livermorec6', 'RunBench/MiBench_automotive_susan_s', 'RunBench/MiBench_security_blowfish_e', 'RunBench/MediaBench_jpg2000dec', 'RunBench/PolyBench_apml', 'RunBench/MiBench_office_stringsearch', 'RunBench/PolyBench_detect', 'RunBench/WCETBench_cnt']]


mapping = {}

CC = 'gcc -w'
def build(cnnfig, op_seq, compiler = 'gcc'):
    if compiler == 'gcc':
        return build_gcc(op_seq)


def build_gcc(op_seq):
    #clean and build executable file
    m = os.popen(f'rm -f {config["exe_file"]}; rm -f *.o').read()
    link_lib = ''
    if os.path.exists('libfunc.so'):
        link_lib = '-L. -lfunc'
    for f in config['files']:
        if f.endswith('.cpp') or f.endswith('.cc'):
            CC = 'g++ -w '
            op_seq = op_seq.replace('-fpack-struct', '').replace('-fno-pack-struct', '')
        else:
            CC = 'gcc -w '
        m = os.system(f'{CC} {op_seq} {config["lib"]} -c {f} -o {f.replace("/", "_").replace("..", "").split(".")[0]}.o {link_lib} > tmp')
        if m != 0:
            return False
    return os.system(f'{CC} {op_seq} *.o -o {config["exe_file"]} {config["link_lib"]} {link_lib} > tmp') == 0

def get_timing_result(error):
    res = []
    tmp = [i.strip() for i in str(error)[:-1].strip().split('\n') if 'timing' in i]
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
    res.append(sum(t))
    return sum(res)
    
def select_best_ops(op_his, time_limit = None, iteration_limit = None):
    if time_limit:
        #only select the best within time limitation
        ops = [i for i in op_his if i[-1] < time_limit]
    if iteration_limit:
        ops = op_his[:iteration_limit]
    min_idx = np.argmax([np.median(i[1]) for i in ops])
    op_config = ops[min_idx][0]
    return [op_config, min_idx, ops[min_idx][-1]]

def select_best_ops1(op_his, time_limit = None, iteration_limit = None):
    if time_limit:
        #only select the best within time limitation
        ops = [i for i in op_his if i[-1] < time_limit]
    if iteration_limit:
        ops = op_his[:iteration_limit]
    min_idx = np.argmin([np.median(i[1]) for i in ops])
    op_config = ops[min_idx][0]
    return [op_config, min_idx, ops[min_idx][-1]]


def build_opentuner_ops(op_config, d1, d2):
    opts = f'-O1 '
    for op in op_config:
        if op == 'opt_level':
            continue
        else:
            if op_config[op] == 'on':
                opts += f'{op} '
            elif op_config[op] == 'off':
                opts += f'-fno-{op[2:]} '
            else:
                continue
    return [opts, d1, d2]

                

if __name__ == '__main__':
    cwd = os.getcwd()
    hostname = os.popen('hostname').read().strip()
    for task in mapping[hostname]:
        details = {}
        os.chdir(f'{cwd}/{task}')
        build_env(task.split('/')[-1])
        os.popen(f'rm -f *.out').read()
        config = json.load(open('new_config.f'))
        recommend_config = {}
        if 'perlbench' in task or 'office_rsynth' in task:
            recommend_config['O3'] = ['-O1', 0, 0]
        else:
            recommend_config['O3'] = ['-O3', 0, 0]
        recommend_config['all'] = ['-O1 ' + ' '.join(ALL_OPTIMIZATIONS), 0, 0]
        timeout = 60
        if 'MediaBench_h264enc' in task:
            timeout = 300
            run_count = 3
        elif 'PolyBench_3mm' in task:
            run_count = 5
        elif 'PolyBench_2mm' in task:
            run_count = 7
        else:
            run_count = 15
        '''
        tmp = pickle.load(open(f'{cwd}/{task}/gcc_proba_random_0_6000_1.0_state.pkl', 'rb'))
        O3_RUN_TIME = tmp[0][3]
        for repeat in range(5):            
            for algo in ['random']:
                proba = pickle.load(open(f'{cwd}/{task}/gcc_proba_{algo}_{repeat}_6000_1.0_state.pkl', 'rb'))

                if len(proba) == 0:
                    continue
                for limit in [100]:
                    tmp_proba = proba[:limit]
                    tmp_proba = [i[0:1] + i[2:-1] for i in tmp_proba if np.median(i[-2]) > 0 and np.median(i[-2]) is not np.inf]
                    recommend_config[f'{algo}_{repeat}_iter_{limit}'] = select_best_ops1(tmp_proba, iteration_limit = limit)
                
                proba = [i[0:1] + i[2:-1] for i in proba if np.median(i[-2]) > 0 and np.median(i[-2]) is not np.inf]
                for limit in [2000, 4000, 6000]:
                    recommend_config[f'{algo}_{repeat}_time_{limit}'] = select_best_ops1(proba, time_limit = limit)
                
                for limit in [100, 200, 400]:
                    try:
                        recommend_config[f'{algo}_{repeat}_time_{limit}'] = select_best_ops1(proba, time_limit = limit * O3_RUN_TIME)
                    except:
                        recommend_config[f'{algo}_{repeat}_time_{limit}'] = ' -O3 '
                

            for algo in ['compTuner', 'opentuner', 'boca', 'SRTuner', 'CFSCA']:
                p = pickle.load(open(f'{cwd}/{task}/gcc_6000_{algo}_{repeat}.pkl', 'rb'))[1:]
                if algo in ['SRTuner', 'compTuner', 'CFSCA']:
                    p = [i[:1] + i[2:] for i in p]

                for limit in [100]:
                    tmp_proba = p[:limit]
                    tmp_proba = [i for i in tmp_proba if type(i[1]) is not str and i[1] != 0 and np.median(i[1]) is not np.inf]
                    if algo == 'opentuner':
                        recommend_config[f'{algo}_{repeat}_iter_{limit}'] = build_opentuner_ops(*select_best_ops1(tmp_proba, iteration_limit = limit))
                    elif algo == 'compTuner' or algo == 'CFSCA':
                        recommend_config[f'{algo}_{repeat}_iter_{limit}'] = select_best_ops(tmp_proba, iteration_limit = limit)
                    else:
                        recommend_config[f'{algo}_{repeat}_iter_{limit}'] = select_best_ops1(tmp_proba, iteration_limit = limit)

                p = [i for i in p if type(i[1]) is not str and i[1] != 0 and np.median(i[1]) is not np.inf]
                for limit in [100, 200, 400, 2000, 4000, 6000]:
                    if limit < 1000:
                        time_limit = limit * O3_RUN_TIME
                    else:
                        time_limit = limit
                    try:
                        if algo == 'opentuner':
                            recommend_config[f'{algo}_{repeat}_time_{limit}'] = build_opentuner_ops(*select_best_ops1(p, time_limit = time_limit))
                        elif algo == 'compTuner' or algo == 'CFSCA':
                            recommend_config[f'{algo}_{repeat}_time_{limit}'] = select_best_ops(p, time_limit = time_limit)
                        else:
                            recommend_config[f'{algo}_{repeat}_time_{limit}'] = select_best_ops1(p, time_limit = time_limit)
                    except:
                        recommend_config[f'{algo}_{repeat}_time_{limit}'] = [' -O3 ', 0, 0]
        
        with open(f'{cwd}/{task}/{task.split("/")[-1]}_optim.pkl', 'wb') as f:
            pickle.dump(recommend_config, f)
        '''
        recommend_config = pickle.load(open(f'{cwd}/{task}/{task.split("/")[-1]}_optim_new2.pkl', 'rb'))
        
        #compile corresponding configs
        for c in recommend_config:
            build(config, recommend_config[c][0])
            if 'gsm_u' in task:
                os.popen(f'mv {config["exe_file"]} un{c}.out').read()
            else:
                os.popen(f'mv {config["exe_file"]} {c}.out').read()
        
        build(config, '')
        if 'gsm_u' in task:
            os.popen(f'mv {config["exe_file"]} un{c}.out').read()
        else:
            os.popen(f'mv {config["exe_file"]} {c}.out').read()
        
        if 'PolyBench' in task or 'livermorec' in task:
            check_cmd = ' 1 '
        else:
            check_cmd = ' '

        if 'gsm_u' in task:
            process = subprocess.Popen(f'LD_LIBRARY_PATH=. ./un{c}.out {check_cmd} {config["run"]}', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
        else:
            process = subprocess.Popen(f'LD_LIBRARY_PATH=. ./{c}.out {check_cmd} {config["run"]}', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
        output, error = process.communicate(timeout = timeout)
        os.popen(f'rm -f old_*').read()
        for idx, check_file in enumerate(config['check']):
            os.popen(f'cp {check_file} old_{idx}').read()
        
        timing = {}
        build_optim = {}
        for c in recommend_config:
            res = []
            if recommend_config[c][0] in build_optim:
                timing[c] = build_optim[recommend_config[c][0]]
                continue

            fail_flag = 0
            if 'WCETBench' in task:
                check_cmd = ' 1 '
            else:
                check_cmd = ' '
            for i in range(run_count + 1):
                if i > 0:
                    check_cmd = ''
                if 'gsm_u' in task:
                    process = subprocess.Popen(f'LD_LIBRARY_PATH=. ./un{c}.out {check_cmd} {config["run"]}', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
                else:
                    process = subprocess.Popen(f'LD_LIBRARY_PATH=. ./{c}.out {check_cmd} {config["run"]}', stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell = True)
                try:
                    output, error = process.communicate(timeout = timeout)
                except:
                    continue

                if i < 1:
                    for idx, check_file in enumerate(config['check']):
                        try:
                            tmp = os.popen(f'diff old_{idx} {check_file}').read()
                        except:
                            fail_flag = 1
                            break
                        if tmp != '':
                            fail_flag = 1
                            break
                    continue
                if fail_flag:
                    res.append([-1])
                    break

                res.append(get_timing_result(error))
            timing[c] = res
            build_optim[recommend_config[c][0]] = copy.deepcopy(res)
        with open(f'{cwd}/{task}/{task.split("/")[-1]}_repeat_new2.pkl', 'wb') as f:
            pickle.dump(timing, f)