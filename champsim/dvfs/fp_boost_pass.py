#!/usr/bin/env python3
"""Assembly-level DVFS hint pass (gcc -S output, x86-64 AT&T syntax).

Finds loops (a backward jump to an earlier label in the same function), scores each one for
floating-point density, and wraps the outermost qualifying loops in clock-boost hints:

    nopl 0x44560000+LEVEL(%rax)    on every path INTO the loop   (boost)
    nopl 0x44560000(%rax)          on every path OUT of the loop (restore base clock)

Entry paths: fall-through into the header, plus any jump from outside the loop to a label inside it
(gcc's inverted loops jump into the middle). Exit paths: fall-through after the back-edge, jumps to
labels outside the loop, and `ret`. Each such jump is retargeted through a small out-of-line stub,
so no flags/registers are touched (nopl and jmp preserve both).

Knobs (the CARBS search space):
    --fp-min      minimum fraction of loop instructions that are FP arithmetic     (default 0.25)
    --min-instrs  minimum static loop size, a proxy for region length / amortising the
                  transition penalty                                               (default 8)
    --mem-max     maximum fraction of loop instructions with a memory operand      (default 0.60)
    --level       hint level to boost to (0 = base clock; table in CHAMPSIM_DVFS_PERIODS_PS)

usage: fp_boost_pass.py in.s -o out.s [knobs] [--report]
"""
import argparse
import re
import sys

MAGIC = 0x44560000

LABEL = re.compile(r"^([A-Za-z_.$][\w.$]*):")
INSN = re.compile(r"^\s+([a-z][\w.]*)\b\s*(.*)$")
FP_ARITH = re.compile(
    r"^(v?(add|sub|mul|div|sqrt|max|min|rcp|rsqrt|round|hadd|hsub|dp|fmadd\d*|fmsub\d*|fnmadd\d*|fnmsub\d*|fmaddsub\d*|fmsubadd\d*)(s|p)(s|d)"
    r"|vf(n?m)(add|sub)\d+(s|p)(s|d)"
    r"|f(add|sub|subr|mul|div|divr|sqrt|sin|cos|xch|chs|abs)p?"
    r"|fi(add|sub|mul|div)"
    r"|v?cvt\w*(s|p)(s|d)\w*|v?u?comis(s|d))$")
SKIP_MEM = re.compile(r"^(lea\w*|nop\w*|prefetch\w*)$")


def parse(lines):
    """Return list of dicts: kind in {label, insn, other}; plus function ranges."""
    recs = []
    for n, raw in enumerate(lines):
        line = raw.split("#", 1)[0].rstrip("\n")
        m = LABEL.match(line)
        if m:
            recs.append(("label", n, m.group(1), ""))
            continue
        m = INSN.match(line)
        if m and not m.group(1).startswith("."):
            recs.append(("insn", n, m.group(1), m.group(2).strip()))
    return recs


def functions(lines):
    """(start_line, end_line) of each function body: label after '.type X,@function' .. '.cfi_endproc'."""
    out, start = [], None
    for n, raw in enumerate(lines):
        s = raw.strip()
        if s.startswith(".cfi_startproc"):
            start = n
        elif s.startswith(".cfi_endproc") and start is not None:
            out.append((start, n))
            start = None
    return out


def jump_target(mn, ops):
    if not mn.startswith("j"):
        return None, False
    if ops.startswith("*"):
        return None, True                      # indirect
    return ops.split()[0] if ops else None, False


def find_loops(lines, recs, fstart, fend):
    labels = {r[2]: r[1] for r in recs if r[0] == "label" and fstart <= r[1] <= fend}
    insns = [r for r in recs if r[0] == "insn" and fstart <= r[1] <= fend]
    loops = {}                                  # header label -> [first_line, last_line]
    for _, n, mn, ops in insns:
        tgt, _ = jump_target(mn, ops)
        if tgt in labels and labels[tgt] < n:
            lo = labels[tgt]
            ent = loops.setdefault(tgt, [lo, n])
            ent[1] = max(ent[1], n)
    return labels, insns, loops


def score(insns, lo, hi):
    body = [i for i in insns if lo <= i[1] <= hi]
    total = len(body)
    fp = sum(1 for _, _, mn, _ in body if FP_ARITH.match(mn))
    mem = sum(1 for _, _, mn, ops in body if "(" in ops and not SKIP_MEM.match(mn))
    indirect = any(jump_target(mn, ops)[1] for _, _, mn, ops in body)
    return total, fp, mem, indirect


def hint_line(level):
    return f"\tnopl\t{hex(MAGIC | level)}(%rax)\t# dvfs hint level {level}\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--fp-min", type=float, default=0.25)
    ap.add_argument("--min-instrs", type=int, default=8)
    ap.add_argument("--mem-max", type=float, default=0.60)
    ap.add_argument("--hoist-fp-min", type=float, default=0.05,
                    help="climb to enclosing loops whose FP density is at least this (1 = never hoist)")
    ap.add_argument("--level", type=int, default=3)
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()

    lines = open(a.input).readlines()
    recs = parse(lines)
    before = {}   # line -> text to insert before that line
    after = {}    # line -> text to insert after that line
    replace = {}  # line -> new text
    stubs = {}    # line (function end) -> list of stub text
    report, uid = [], 0

    for fstart, fend in functions(lines):
        labels, insns, loops = find_loops(lines, recs, fstart, fend)
        cands = []
        for hdr, (lo, hi) in loops.items():
            total, fp, mem, indirect = score(insns, lo, hi)
            ok = (not indirect and total >= a.min_instrs and total and fp / total >= a.fp_min and mem / total <= a.mem_max)
            report.append((hdr, lo + 1, hi + 1, total, fp, mem, ok))
            if ok:
                cands.append((hi - lo, hdr, lo, hi))
        def hoist(lo, hi):
            while True:
                enc = [(h2 - l2, l2, h2) for l2, h2 in loops.values() if l2 <= lo and hi <= h2 and (l2, h2) != (lo, hi)]
                if not enc:
                    return lo, hi
                _, l2, h2 = min(enc)
                t, f, _, ind = score(insns, l2, h2)
                if ind or not t or f / t < a.hoist_fp_min:
                    return lo, hi
                lo, hi = l2, h2
        cands = [(h - l, hdr, *hoist(l, h)) for _, hdr, l, h in cands]
        chosen = []
        for _, hdr, lo, hi in sorted(cands, reverse=True):       # biggest first => outermost wins
            if not any(c_lo <= lo and hi <= c_hi for _, c_lo, c_hi in chosen):
                chosen.append((hdr, lo, hi))

        for hdr, lo, hi in chosen:
            uid += 1
            in_range = lambda n: lo <= n <= hi
            stub_text = []
            # entry: fall-through into the header
            before.setdefault(lo, []).append(hint_line(a.level))
            # jumps/rets inside and outside
            for _, n, mn, ops in insns:
                tgt, _ = jump_target(mn, ops)
                if mn == "ret" and in_range(n):
                    before.setdefault(n, []).append(hint_line(0))
                    continue
                if tgt is None:
                    continue
                inside_src = in_range(n)
                tgt_line = labels.get(tgt)
                tgt_inside = tgt_line is not None and in_range(tgt_line)
                if not inside_src and tgt_inside:                      # outside -> into loop: boost stub
                    stub = f".LDVFS{uid}_in_{n}"
                    replace[n] = lines[n].replace(tgt, stub, 1)
                    stub_text.append(f"{stub}:\n{hint_line(a.level)}\tjmp\t{tgt}\n")
                elif inside_src and not tgt_inside:                    # loop -> outside: restore stub
                    stub = f".LDVFS{uid}_out_{n}"
                    replace[n] = lines[n].replace(tgt, stub, 1)
                    stub_text.append(f"{stub}:\n{hint_line(0)}\tjmp\t{tgt}\n")
            # exit: fall-through after the last back-edge; stubs sit after it, jumped over
            done = f".LDVFS{uid}_done"
            after.setdefault(hi, []).append(hint_line(0) + (f"\tjmp\t{done}\n" + "".join(stub_text) + f"{done}:\n" if stub_text else ""))

    out = []
    for n, raw in enumerate(lines):
        out.extend(before.get(n, []))
        out.append(replace.get(n, raw))
        out.extend(after.get(n, []))
    with open(a.output, "w") as f:
        f.writelines(out)

    if a.report:
        print(f"{'header':<14}{'lines':<14}{'instrs':>7}{'fp':>5}{'mem':>5}  boosted", file=sys.stderr)
        for hdr, lo, hi, t, fp, mem, ok in sorted(report, key=lambda r: r[1]):
            print(f"{hdr:<14}{lo}-{hi:<10}{t:>7}{fp:>5}{mem:>5}  {'yes' if ok else 'no'}", file=sys.stderr)
    print(f"{a.input}: {len(report)} loops, boosted {uid} region(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
