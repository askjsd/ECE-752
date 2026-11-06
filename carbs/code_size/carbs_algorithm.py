import random
import numpy as np
import copy
from scipy import stats


class single_optimizer():
    def __init__(self, optimizations):
        super().__init__()
        self.optimizations = copy.deepcopy(optimizations)
        #probabilities of each optimization
        self.op_proba = [0.5 for i in optimizations]
        #[flag, compile time, run time, total time, current op proba]
        self.op_his = []
        self.op_vec = []
    
    def generate_op(self):
        op_vec = []
        for i in range(len(self.op_proba)):
            if random.random() < self.op_proba[i]:
                op_vec.append(1)
            else:
                op_vec.append(0)
        if self.env == 'gcc':
            if 'livermore' in self.run_dir and '-fpack-struct' in self.optimizations:
                op_vec[self.optimizations.index('-fpack-struct')] = 0
            op_seq = vec2op(op_vec, important_ops = self.optimizations)
        else:
            #op_seq = vec2op(op_vec, op_level = '-O0',important_ops = self.optimizations, always_enable = True)
            op_seq = [self.optimizations[i] for i in range(len(op_vec)) if op_vec[i] == 1]
        self.op_vec.append(op_vec)
        return op_seq

    def generate_top_op(self, top_k = 20):
        op_vec = []
        if top_k > 0:
            top_k_idx = np.argsort(self.op_proba)[-top_k:]
        else:
            top_k_idx = []
        for i in range(len(self.op_proba)):
            if i in top_k_idx:
                op_vec.append(1)
            else:
                if random.random() < self.op_proba[i]:
                    op_vec.append(1)
                else:
                    op_vec.append(0)

        op_seq = [self.optimizations[i] for i in range(len(op_vec)) if op_vec[i] == 1]
        self.op_vec.append(op_vec)
        return op_seq        

    def update(self, update_proba, update_epoch):
        for j in range(len(self.op_proba)):
            if self.op_vec[update_epoch][j]:
                self.op_proba[j] = self.op_proba[j] + update_proba[j]
            self.op_proba[j] = min(1-self.low_bound, self.op_proba[j])
            self.op_proba[j] = max(self.low_bound, self.op_proba[j])
        
        #rebalance the probabilities, limit the max length <= 20
        '''
        sum_probability = sum(self.op_proba)
        if sum_probability > 20:
            alpha = 20 / sum_probability
            for j in range(len(self.op_proba)):
                self.op_proba[j] = alpha * self.op_proba[j]
        '''

    def update_single(self):
        #update only based on the latest performance
        pass
    
    def update_history(self):
        #update based on the whole history
        pass


class uniform_1_optimizer(single_optimizer):
    def __init__(self, optimizations):
        super().__init__(optimizations)
        self.similar_pair = []
        self.proba_update = np.array([0 for i in optimizations])
        self.v = np.array([0 for i in optimizations])        

    def update_history(self, update_window = 5, lr = 1):
        baseline2, baseline1 = sorted([np.median(i[2]) for i in self.op_his[:2]])
        baseline_median = np.median([np.median(i[2]) for i in self.op_his if np.median(i[2]) > 0])
        prev_best = min([np.median(i[2]) for i in self.op_his if np.median(i[2]) > 0])

        for i in range(2, 2 + update_window):
            if len(self.op_his) < i:
                break
            update_epoch = 0 - i
            #linear scale from O1 to O3 to previous best
            #better than prev best
            curr_timing = np.median(self.op_his[-1][2])
            prev_timing = np.median(self.op_his[update_epoch][2])
            op_diff = sum(abs(np.array(self.op_vec[-1]) - np.array(self.op_vec[update_epoch])))
            if op_diff == 0:
                continue
            op_common = sum(np.array(self.op_vec[-1]) * np.array(self.op_vec[update_epoch]))
            lr = self.lr ** (i-1)
            better_flag = False
            if prev_timing == np.inf or curr_timing == np.inf:
                #both fail
                proba, proba1, proba2 = 0, 0, 0
                if prev_timing == np.inf and curr_timing == np.inf:
                    proba = 1
                #lower the op enabled in curr but disabled in prev
                elif curr_timing == np.inf:
                    proba1 = 1
                #lower the op enabled in prev but disabled in curr
                else:
                    proba2 = 1
                
                c2 = sum(abs((1-np.array(self.op_vec[-1])) * (1-np.array(self.op_vec[update_epoch]))))
                op_diff1 = sum(np.array(self.op_vec[-1]) > np.array(self.op_vec[update_epoch]))
                op_diff2 = sum(np.array(self.op_vec[-1]) < np.array(self.op_vec[update_epoch]))
                for j in range(len(self.optimizations)):
                    if self.op_vec[-1][j] == 1 and self.op_vec[update_epoch][j] == 1:
                        self.op_proba[j] -= lr * proba / op_common
                    if self.op_vec[-1][j] == 0 and self.op_vec[update_epoch][j] == 0:
                        self.op_proba[j] += lr * proba / c2

                    #enabled in curr but not in prev
                    if self.op_vec[-1][j] > self.op_vec[update_epoch][j]:
                        self.op_proba[j] -= lr * proba1 / op_diff1
                    #enabled in prev but not in curr
                    if self.op_vec[-1][j] < self.op_vec[update_epoch][j]:
                        self.op_proba[j] -= lr * proba2 / op_diff2
                self.update([0 for i in self.op_proba], -1)
            else:
                better_flag = False
                if type(self.op_his[-1][2]) is not list or len(self.op_his[-1][2]) < 2:
                    if abs(1 - curr_timing / prev_timing) > 0.001:
                        better_flag = True
                    else:
                        better_flag = False
                elif self.p_test:
                    _, p = stats.ttest_rel(sorted(self.op_his[-1][2]), sorted(self.op_his[update_epoch][2]))
                    if p<0.05:
                        better_flag = True
                else:
                    if curr_timing < (1 - self.sp_threshold) * prev_timing or curr_timing > (1 + self.sp_threshold) * prev_timing:
                        better_flag = True
                if better_flag:
                    if curr_timing < prev_timing:
                        proba = (prev_timing / curr_timing) / op_diff
                    else:
                        proba = -(curr_timing / prev_timing) / op_diff
                    for j in range(len(self.optimizations)):
                        #should turn on
                        if self.op_vec[-1][j] > self.op_vec[update_epoch][j]:
                            self.op_proba[j] += lr * proba
                        #should turn off
                        if self.op_vec[-1][j] < self.op_vec[update_epoch][j]:
                            self.op_proba[j] -= lr * proba
                #similiar performance but better than O3, turn on the common optimizations
                elif curr_timing < prev_best:
                    proba = (baseline2 / curr_timing) / op_common
                    for j in range(len(self.optimizations)):
                        if self.op_vec[-1][j] == 1 and self.op_vec[update_epoch][j] == 1:
                            self.op_proba[j] += lr * proba
                else:
                    c2 = sum(abs((1-np.array(self.op_vec[-1])) * (1-np.array(self.op_vec[update_epoch]))))
                    proba = (curr_timing / baseline2)
                    for j in range(len(self.optimizations)):
                        if self.op_vec[-1][j] == 1 and self.op_vec[update_epoch][j] == 1:
                            self.op_proba[j] -= lr * proba / op_common
                        if self.op_vec[-1][j] == 0 and self.op_vec[update_epoch][j] == 0:
                            self.op_proba[j] += lr * proba / c2
                self.update([0 for i in self.op_proba], -1)