import os
import subprocess
import time
import pickle
import json
import random
import subprocess
import numpy as np
import argparse
import copy
from itertools import combinations

CC = 'gcc -w'
def O_optimization(level):
    lines = [i.strip() for i in os.popen(f'gcc -O{level} -Q --help=optimizers | grep enabled | cut -d\'[\' -f1').readlines()]
    return lines

def get_md5(filename):
    m = os.popen(f'md5sum {filename}').read()
    return m.split(' ')[0]

def get_all_optimization():
    lines = [i.strip() for i in os.popen(f'gcc -O0 -Q --help=optimizers | cut -d\'[\' -f1').readlines()]
    lines = [i for i in lines if '-f' in i and '=' not in i and i!='']
    #remove
    lines = [i for i in lines if '-fstack-protector' not in i and '-fno-threadsafe-statics' not in i and 'toplevel-reorder' not in i]
    # Local addition: the flag list is scraped from `gcc -Q --help=optimizers`, which differs across
    # gcc versions. Keep only well-formed flags that this gcc accepts (in both positive and -fno- form),
    # and drop flags that only emit diagnostics/files.
    import re
    def _accepted(flag):
        pos = flag.replace('-fno-', '-f', 1)
        neg = '-fno-' + pos[2:]
        for f in (pos, neg):
            if os.system(f'gcc -w {f} -x c -c /dev/null -o /dev/null > /dev/null 2>&1') != 0:
                return False
        return True
    lines = [i for i in lines if re.fullmatch(r'-f[a-z0-9][a-z0-9+-]*', i)]
    lines = [i for i in lines if not any(k in i for k in ('opt-info', 'save-optimization-record', 'stack-usage', 'dump'))]
    lines = [i for i in lines if _accepted(i)]
    return lines

ALL_OPTIMIZATIONS = get_all_optimization()
O1_ENABLED_OPTIMIZATIONS = O_optimization(1)
O2_ENABLED_OPTIMIZATIONS = O_optimization(2)
O3_ENABLED_OPTIMIZATIONS = O_optimization(3)
O1_DISABLED_OPTIMIZATIONS = [i for i in ALL_OPTIMIZATIONS if i not in O1_ENABLED_OPTIMIZATIONS]
O2_DISABLED_OPTIMIZATIONS = [i for i in ALL_OPTIMIZATIONS if i not in O2_ENABLED_OPTIMIZATIONS]
O3_DISABLED_OPTIMIZATIONS = [i for i in ALL_OPTIMIZATIONS if i not in O3_ENABLED_OPTIMIZATIONS]
OPTIMIZATIONS = {'all_optim': ALL_OPTIMIZATIONS, 'O1_enabled': O1_ENABLED_OPTIMIZATIONS, 'O2_enabled': O2_ENABLED_OPTIMIZATIONS, 'O3_enabled': O3_ENABLED_OPTIMIZATIONS, 'O1_disabled': O1_DISABLED_OPTIMIZATIONS, 'O2_disabled': O2_DISABLED_OPTIMIZATIONS, 'O3_disabled': O3_DISABLED_OPTIMIZATIONS}
ALL_OPTIMIZATIONS = O1_DISABLED_OPTIMIZATIONS
O1_VEC = [1 if i in O1_ENABLED_OPTIMIZATIONS else 0 for i in ALL_OPTIMIZATIONS ]
O3_VEC = [1 if i in O3_ENABLED_OPTIMIZATIONS else 0 for i in ALL_OPTIMIZATIONS ]
DIFF_PROBA = []
LIVERMOREC_REMOVE = ALL_OPTIMIZATIONS.index('-fpack-struct')
for i, op in enumerate(ALL_OPTIMIZATIONS):
    if op in O3_ENABLED_OPTIMIZATIONS:
        DIFF_PROBA.append(0.6)
    elif op in O2_ENABLED_OPTIMIZATIONS:
        DIFF_PROBA.append(0.7)
    else:
        DIFF_PROBA.append(0.5)

def vec2op(op_vec, op_level = 'O1', important_ops = ALL_OPTIMIZATIONS, always_enable = False):
    if always_enable:
        ops = f' -{op_level} ' + ' '.join([important_ops[i] for i in range(len(op_vec)) if op_vec[i] == 1])
    else:
        ops = f' -{op_level} ' + ' '.join([important_ops[i] if op_vec[i] == 1 else '-fno-'+important_ops[i][2:] for i in range(len(op_vec))])
    return ops

def op2vec(op_seq, important_ops = ALL_OPTIMIZATIONS):
    if op_seq == '-O3':
        return O3_VEC
    op_vec = [1 if i in op_seq else 0 for i in important_ops]
    return op_vec