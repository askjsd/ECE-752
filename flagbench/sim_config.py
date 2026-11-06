"""Fixed gem5 configuration used for EVERY simulation in flagbench (SE mode, X86).

  CPU    : 1x DerivO3CPU (gem5 stdlib CPUTypes.O3 default parameters), 2 GHz
  L1I    : 32 KiB   L1D : 32 KiB   L2 : 256 KiB  (classic caches, PrivateL1PrivateL2CacheHierarchy;
           associativities/latencies are the stdlib defaults, dumped into config.json by gem5)
  Memory : SingleChannelDDR3_1600, 1 GiB
ROI: m5_work_begin() resets stats, m5_work_end() dumps stats (stdlib default exit handlers),
so the FIRST stats block in stats.txt is the region of interest.

usage: gem5.opt -d <outdir> sim_config.py <static_binary>
"""
import sys
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

requires(isa_required=ISA.X86)

binary = sys.argv[1]
cache = PrivateL1PrivateL2CacheHierarchy(l1d_size="32KiB", l1i_size="32KiB", l2_size="256KiB")
memory = SingleChannelDDR3_1600(size="1GiB")
processor = SimpleProcessor(cpu_type=CPUTypes.O3, isa=ISA.X86, num_cores=1)
board = SimpleBoard(clk_freq="2GHz", processor=processor, memory=memory, cache_hierarchy=cache)
board.set_se_binary_workload(BinaryResource(binary))
sim = Simulator(board=board)
sim.run()
