# pfbench build.
#   make            pfbench (HW + SW prefetch variants) and pfbench_noswpf (no prefetch insns)
#   make m5         pfbench_m5: ROI also emits gem5 m5_work_begin/end
#                   (needs M5_LIB=path/to/libm5.a built for the target ISA)
#   make disasm     check that pattern loops kept one scalar load per access
#
# -fno-tree-vectorize / -fno-tree-loop-distribute-patterns / -fno-unroll-loops keep
# the access pattern scalar and in source order (no SIMD gathers, no memset/memcpy
# substitution, no unrolling that would spread one pattern over several load PCs).
# Non-PIE + symbols so trace_dr.sh -S pfb_roi_begin can find the ROI.
CC      ?= gcc
OPT     ?= -O2
CFLAGS  += $(OPT) -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           -fno-tree-vectorize -fno-tree-loop-distribute-patterns -fno-unroll-loops \
           -fno-pie -fno-omit-frame-pointer
LDFLAGS += -no-pie
REPO    := $(abspath ..)
M5_LIB  ?= $(REPO)/util/m5/build/x86/out/libm5.a

SRC := main.c pfb_common.c k_pointer.c k_indirect.c k_stride.c k_stream.c k_mix.c
HDR := pfb.h

all: pfbench pfbench_noswpf

pfbench: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

pfbench_noswpf: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -DPFB_NO_SWPF -o $@ $(SRC) $(LDFLAGS)

m5: pfbench_m5
pfbench_m5: $(SRC) $(HDR)
	@test -f "$(M5_LIB)" || { echo "M5_LIB=$(M5_LIB) not found; build util/m5 for x86 (scons build/x86/out/m5)"; exit 1; }
	$(CC) $(CFLAGS) -DPFB_M5 -I$(REPO)/include -o $@ $(SRC) $(M5_LIB) $(LDFLAGS)

disasm: pfbench
	objdump -d --no-show-raw-insn pfbench > pfbench.dis
	@echo "wrote pfbench.dis"

clean:
	rm -f pfbench pfbench_noswpf pfbench_m5 pfbench.dis

.PHONY: all m5 disasm clean
