#!/usr/bin/env python3
"""Sanity-check a ChampSim trace: apply ChampSim's register-based branch classifier
(inc/instruction.h) and report counts, taken rates and memory-operand totals."""
import lzma, gzip, struct, sys, collections
REC = struct.Struct("<QBB2B4B2Q4Q"); SP, FL, IP = 6, 25, 26
op = lzma.open if sys.argv[1].endswith(".xz") else gzip.open if sys.argv[1].endswith(".gz") else open
kinds = collections.Counter(); taken = collections.Counter(); n = smem = dmem = 0; pcs = collections.Counter()
with op(sys.argv[1], "rb") as f:
    while (b := f.read(64)) and len(b) == 64:
        r = REC.unpack(b); n += 1
        ip, isb, tk = r[0], r[1], r[2]; dst = [x for x in r[3:5] if x]; src = [x for x in r[5:9] if x]
        smem += sum(1 for x in r[11:15] if x); dmem += sum(1 for x in r[9:11] if x)
        wsp, wip = SP in dst, IP in dst; rsp, rfl, rip = SP in src, FL in src, IP in src
        oth = any(x not in (SP, FL, IP) for x in src)
        if not rsp and not rfl and wip and not oth: k = "DIRECT_JUMP"
        elif not rsp and not rip and not rfl and wip and oth: k = "INDIRECT"
        elif not rsp and rip and not wsp and wip and (rfl or oth): k = "CONDITIONAL"
        elif rsp and rip and wsp and wip and not rfl and not oth: k = "DIRECT_CALL"
        elif rsp and rip and wsp and wip and not rfl and oth: k = "INDIRECT_CALL"
        elif rsp and not rip and wsp and wip: k = "RETURN"
        elif wip: k = "OTHER"
        else: k = None
        if k:
            kinds[k] += 1; taken[k] += tk
            if not isb: kinds["!! writes IP but is_branch=0"] += 1
        elif isb: kinds["!! is_branch=1 but not classified"] += 1
print(f"{n} records, {smem} source-mem operands, {dmem} dest-mem operands")
for k, c in kinds.most_common(): print(f"  {k:32} {c:8}  taken={taken[k]/c:.3f}" if k in taken else f"  {k:32} {c:8}")
