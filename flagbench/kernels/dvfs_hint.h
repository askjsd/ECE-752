/* DVFS hints for the ChampSim clock-scaling experiment.
 * DVFS_HINT(level) emits `nopl (0x44560000|level)(%rax)`: a no-op on real hardware that
 * champsim/dynamorio/dr_trace/to_champsim.py turns into a clock-change record.
 * level 0 = base clock; 1..3 = higher clocks (see CHAMPSIM_DVFS_PERIODS_PS). */
#ifndef DVFS_HINT_H
#define DVFS_HINT_H
#define DVFS_HINT(level) __asm__ volatile(".byte 0x0f,0x1f,0x80\n\t.long %c0" :: "i"(0x44560000 | (level)) : "memory")
#endif
