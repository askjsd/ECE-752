from compiler_env import *
from utils import *
import argparse
import pickle
import random
import copy
from optimizers import *



if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--env', type=str, default='gcc')
    parser.add_argument('--run_dir', type=str, default='')
    parser.add_argument('--algorithm', type=str, default='uniform')
    parser.add_argument('--random_seed', type=int, default=42)
    parser.add_argument('--steps', type = int, default = 100000)
    parser.add_argument('--time_limitation', type = int, default = None)
    parser.add_argument('--lr', type = float, default = 1)
    parser.add_argument('--top', type = int, default = None)
    parser.add_argument('--multi', type = int, default = None)
    parser.add_argument('--window', type = int, default = 5)
    parser.add_argument('--low_bound', type = float, default = 0.1)
    parser.add_argument('--sp_threshold', type = float, default = 0.05)
    parser.add_argument('--p_test', type = int, default = 0)
    args = parser.parse_args()
    
    random.seed(args.random_seed % 5)

    #decide the type of optimizer
    if 'uniform' in args.algorithm:
        op_tuner = uniform_1_optimizer(ALL_OPTIMIZATIONS)
    elif 'multi' in args.algorithm:
        if 'liver' in args.run_dir.lower():
            ALL_OPTIMIZATIONS.remove('-fpack-struct')
        op_tuner = multivalue_optimizer(ALL_OPTIMIZATIONS)
        if args.multi != None:
            op_tuner.optimizations = op_tuner.optimizations[:args.multi-3]
    else:
        op_tuner = uniform_1_optimizer(ALL_OPTIMIZATIONS)
    
    op_tuner.low_bound = args.low_bound
    op_tuner.sp_threshold = args.sp_threshold
    op_tuner.p_test = args.p_test
    op_tuner.env = args.env

        
    env = CompilerEnv(args.env, ALL_OPTIMIZATIONS, args.run_dir, args.time_limitation)

    if 'perlbench' in args.run_dir.lower():
        base_optimization = '-O1'
        base_optim_vec = [0 for op in ALL_OPTIMIZATIONS]
    else:
        base_optimization = '-O3'
        base_optim_vec = [1 if op in O3_ENABLED_OPTIMIZATIONS else 0 for op in ALL_OPTIMIZATIONS]

    env.begin_time = time.time()
    compile_time, run_time = env.run_single(base_optimization)[0]
    env.state.append([base_optimization, compile_time, run_time, time.time() - env.begin_time])

    op_tuner.op_his=env.state
    op_tuner.lr = args.lr
    op_tuner.op_his[0].append(copy.deepcopy(op_tuner.op_proba))
    if 'multi' in args.algorithm:
        if args.multi != None:
            op_tuner.op_vec.append(base_optim_vec + [1, 2, 1][:args.multi-3])
        else:
            op_tuner.op_vec.append(base_optim_vec + [1, 2, 1])
    else:
        op_tuner.op_vec.append(base_optim_vec)
    op_tuner.run_dir = args.run_dir
    
    if args.time_limitation:
        if args.top:
            filename = f'{args.run_dir}/{args.env}_proba_{args.algorithm}_{args.top}_{args.random_seed}_{args.time_limitation}_{args.lr}_state.pkl'
        elif args.multi != None:
            filename = f'{args.run_dir}/{args.env}_proba_{args.algorithm}_m{args.multi}_{args.random_seed}_{args.time_limitation}_{args.lr}_state.pkl'
        else:
            filename = f'{args.run_dir}/{args.env}_proba_{args.algorithm}_{args.random_seed}_{args.time_limitation}_{args.lr}_state.pkl'
    else:
        filename = f'{args.run_dir}/{args.env}_proba_{args.algorithm}_{args.random_seed}_{args.steps}_{args.lr}_state.pkl'

    steps = 0
    while 1:
        with open(filename, 'wb') as f:
            pickle.dump(op_tuner.op_his, f)
        if args.time_limitation and time.time() - env.begin_time > args.time_limitation:
            break
        else:
            if steps > args.steps:
                break
        
        op_seq = []
        op_seq.append(op_tuner.generate_op())
        
        res = env.run(op_seq)

        for i in range(len(op_seq)):
            op_tuner.op_his.append([op_seq[i], res[i][0], res[i][1], time.time() - env.begin_time, copy.deepcopy(op_tuner.op_proba)])
        #update probability
        if 'new_combine' in args.algorithm:
            op_tuner.update_extend_linear(1, args.lr)
        elif 'history' in args.algorithm:
            op_tuner.update_history(args.window, args.lr)
        steps += 1
