import os
import argparse
import pandas as pd
from LLVMEnv.common import get_codesize
from LLVMEnv.SearchMethods.RandomWalk import generate_population
from LLVMEnv.actionspace.codesize_pairs import codesize_pairlist
from multiprocessing import Pool, cpu_count
from carbs_algorithm import *
import pickle
from tqdm import trange
import copy 

parser = argparse.ArgumentParser()
parser.add_argument("--dataset", type=str, required=True, help="Dataset name, it includes: blas-v0,cbench-v1,mibench-v1,npb-v0,opencv-v0")
parser.add_argument("--seed", type=int, default=0, help="Random seed for reproducibility")
parser.add_argument("--top", type=int, default=0, help="Top k operations to select")
parser.add_argument("--random", type = bool, default = False, help="Use random shuffle or not")
parser.add_argument("--dep", type= bool, default = False, help="Use dependency or not")
args = parser.parse_args()
import random
random.seed(args.seed)

dataset = args.dataset
llvm_tools_path = "./llvm_tools/"
ll_code_dir = "./dataset/test/" + dataset + "/"
#population = generate_population(codesize_pairlist, 1000)
ops = ['--add-discriminators', '--adce', '--aggressive-instcombine', '--alignment-from-assumptions', '--always-inline', '--argpromotion', '--attributor', '--barrier', '--bdce', '--break-crit-edges', '--simplifycfg', '--callsite-splitting', '--called-value-propagation', '--canonicalize-aliases', '--consthoist', '--constmerge', '--constprop', '--coro-cleanup', '--coro-early', '--coro-elide', '--coro-split', '--correlated-propagation', '--cross-dso-cfi', '--deadargelim', '--dce', '--die', '--dse', '--reg2mem', '--div-rem-pairs', '--early-cse-memssa', '--early-cse', '--elim-avail-extern', '--ee-instrument', '--flattencfg', '--float2int', '--forceattrs', '--inline', '--insert-gcov-profiling', '--gvn-hoist', '--gvn', '--globaldce', '--globalopt', '--globalsplit', '--guard-widening', '--hotcoldsplit', '--ipconstprop', '--ipsccp', '--indvars', '--irce', '--infer-address-spaces', '--inferattrs', '--inject-tli-mappings', '--instsimplify', '--instcombine', '--instnamer', '--jump-threading', '--lcssa', '--licm', '--libcalls-shrinkwrap', '--load-store-vectorizer', '--loop-data-prefetch', '--loop-deletion', '--loop-distribute', '--loop-fusion', '--loop-guard-widening', '--loop-idiom', '--loop-instsimplify', '--loop-interchange', '--loop-load-elim', '--loop-predication', '--loop-reroll', '--loop-rotate', '--loop-simplifycfg', '--loop-simplify', '--loop-sink', '-loop-reduce', '--loop-unroll-and-jam', '--loop-unroll', '--loop-unswitch', '--loop-vectorize', '--loop-versioning-licm', '--loop-versioning', '--loweratomic', '--lower-constant-intrinsics', '--lower-expect', '--lower-guard-intrinsic', '--lowerinvoke', '--lower-matrix-intrinsics', '--lowerswitch', '--lower-widenable-condition', '--memcpyopt', '--mergefunc', '--mergeicmps', '--mldst-motion', '--sancov', '--name-anon-globals', '--nary-reassociate', '--newgvn', '--pgo-memop-opt', '--partial-inliner', '--partially-inline-libcalls', '--post-inline-ee-instrument', '--functionattrs', '--mem2reg', '--prune-eh', '--reassociate', '--redundant-dbg-inst-elim', '--rpo-functionattrs', '--rewrite-statepoints-for-gc', '--sccp', '--slp-vectorizer', '--sroa', '--scalarizer', '--separate-const-offset-from-gep', '--simple-loop-unswitch', '--sink', '--speculative-execution', '--slsr', '--strip-dead-prototypes', '--strip-debug-declare', '--strip-nondebug', '--strip', '--tailcallelim', '--mergereturn']

current_file_path = os.path.abspath(__file__)
current_directory = os.path.dirname(current_file_path)

df = pd.read_csv(f"/{current_directory}/output/Phase4_graph_test_Sel_Label.csv")
filenames = df['Filename'].tolist()

codesize_mean = []
filecodesize_mean = []

import concurrent.futures


def run_carbs(filename):
    shortname = filename.split("/")[1]
    full_path = os.path.join(ll_code_dir, shortname)
    with open(full_path, 'r') as ll_file:
        ll_code = ll_file.read()

    op_tuner = uniform_1_optimizer(ops)
    op_tuner.low_bound = 0
    op_tuner.sp_threshold = 0.05
    op_tuner.p_test = 1
    op_tuner.env = 'llvm'
    op_tuner.lr = 0.9

    for i, op in enumerate(ops):
        op_tuner.op_proba[i] = 0.2
    O0 = get_codesize(ll_code, ["-O0"], llvm_tools_path=llvm_tools_path)
    Oz = get_codesize(ll_code, ["-Oz"], llvm_tools_path=llvm_tools_path)
    op_tuner.op_his = [['-O0', 0, O0, 0, [0.2 for i in ops]]]
    op_tuner.op_vec.append([0 for i in ops])
    for i in range(999):
        op_seq = op_tuner.generate_top_op(top_k = args.top)
        if args.random:
            random.shuffle(op_seq)
        if args.dep:
            op_seq = ['-O0'] + op_seq + op_seq
        else:
            op_seq = ['-O0'] + op_seq
        codesize = get_codesize(ll_code, op_seq, llvm_tools_path=llvm_tools_path)
        op_tuner.op_his.append([op_seq, 0, codesize, 0, copy.deepcopy(op_tuner.op_proba)])
        op_tuner.update_history(5, 0.9)
    op_tuner.op_his.append(['', 0, Oz, 0, 0])
    
    return {shortname: (Oz - min([i[2] for i in op_tuner.op_his[:1000]])) / Oz}


dataset_filenames = []
for idx, filename in enumerate(filenames):
    if filename.split("/")[0] == dataset:
        dataset_filenames.append(filename)


cpus_map = {
    'cbench-v1': 11,
    'blas-v0': 8,
    'mibench-v1': 20,
    'npb-v0': 41,
    'opencv-v0': 8
}

with Pool(processes=cpus_map[dataset]) as pool:
    results = pool.map(run_carbs, dataset_filenames)
final_res = {}
for i in results:
    final_res.update(i)
mean_reduction = sum(final_res.values()) / len(final_res)
print(f"{dataset} Mean: ",mean_reduction)
