import sys

import m5
from m5.objects import *

# 1. System & Clock Configuration
system = System()
system.clk_domain = SrcClockDomain(
    clock="2GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("2GB")]

# 2. Out-of-Order CPU Core Microarchitecture (DerivO3CPU)
system.cpu = DerivO3CPU()

system.cpu.fetchWidth = 4
system.cpu.decodeWidth = 4
system.cpu.renameWidth = 4
system.cpu.dispatchWidth = 4
system.cpu.issueWidth = 4
system.cpu.wbWidth = 4
system.cpu.commitWidth = 4
system.cpu.squashWidth = 4

system.cpu.numROBEntries = 128
system.cpu.LQEntries = 32
system.cpu.SQEntries = 32
system.cpu.numPhysIntRegs = 96
system.cpu.numPhysFloatRegs = 96


# 3. Cache Hierarchy (L1I, L1D, L2)
class L1ICache(Cache):
    size = "32kB"
    assoc = 8
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 4
    tgts_per_mshr = 20


class L1DCache(Cache):
    size = "32kB"
    assoc = 8
    tag_latency = 2
    data_latency = 2
    response_latency = 2
    mshrs = 8
    tgts_per_mshr = 20


class L2Cache(Cache):
    size = "256kB"
    assoc = 8
    tag_latency = 10
    data_latency = 10
    response_latency = 10
    mshrs = 20
    tgts_per_mshr = 12


system.cpu.icache = L1ICache()
system.cpu.dcache = L1DCache()

system.l2bus = L2XBar()
system.membus = SystemXBar()

system.cpu.icache.cpu_side = system.cpu.icache_port
system.cpu.icache.mem_side = system.l2bus.cpu_side_ports
system.cpu.dcache.cpu_side = system.cpu.dcache_port
system.cpu.dcache.mem_side = system.l2bus.cpu_side_ports

system.l2cache = L2Cache()
system.l2cache.cpu_side = system.l2bus.mem_side_ports
system.l2cache.mem_side = system.membus.cpu_side_ports

# 4. Memory Setup
system.cpu.createInterruptController()
system.system_port = system.membus.cpu_side_ports

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# 5. Explicit RISC-V Workload Assignment
binary = sys.argv[1]
process = Process()
process.executable = binary
process.cmd = [binary]

system.workload = SEWorkload.init_compatible(binary)
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate()
print(f"Exited at tick {m5.curTick()} because {exit_event.getCause()}")
