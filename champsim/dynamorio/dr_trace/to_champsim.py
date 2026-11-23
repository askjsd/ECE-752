#!/usr/bin/env python3
"""
Convert a ptrace-free x86-64 instruction trace into ChampSim's 64-byte input_instr format.

Front ends (read from stdin or a file):
  --from drview   text output of: drrun -t drmemtrace -indir DIR -tool view
                  (instruction bytes come from the trace itself)
  --from lackey   output of: valgrind --tool=lackey --trace-mem=yes
                  (instruction bytes are read from --elf, so the binary must be non-PIE,
                   which is what `gcc -static` produces by default)

Register operands come from Capstone (pip install --user capstone). Branch register sets are
synthesized so that ChampSim's register-based branch classifier (inc/instruction.h) puts each
branch in the right category; taken/not-taken comes from the next instruction's PC.

Output is uncompressed; pipe through `xz -T0` (ChampSim reads .xz/.gz directly).
"""
import argparse, re, struct, sys
import capstone
from capstone import x86 as X

REG_SP, REG_FLAGS, REG_IP = 6, 25, 26
REG_DVFS = 254                                  # champsim::REG_DVFS_MARKER
DVFS_MAGIC = 0x44560000                         # nopl (0x44560000|level)(%rax) == DVFS hint; level 0 = base clock          # champsim::REG_* in inc/trace_instruction.h
NSRC, NDST = 4, 2                              # register slots
NSMEM, NDMEM = 4, 2                            # memory slots
REC = struct.Struct("<QBB2B4B2Q4Q")            # == sizeof(input_instr) == 64
assert REC.size == 64

# ---- register naming -> small integer ids (anything except 0, 6, 25, 26) -------------------
_GPR = {}
for full, subs in {
    "rax": "eax ax al ah", "rbx": "ebx bx bl bh", "rcx": "ecx cx cl ch", "rdx": "edx dx dl dh",
    "rsi": "esi si sil", "rdi": "edi di dil", "rbp": "ebp bp bpl", "rsp": "esp sp spl",
}.items():
    for s in [full] + subs.split():
        _GPR[s] = full
for n in range(8, 16):
    for suf in ("", "d", "w", "b"):
        _GPR[f"r{n}{suf}"] = f"r{n}"
# Fixed ids so traces from different runs/front ends are byte-comparable.
_ids = {"rdi": 3, "rsi": 4, "rbp": 5, "rsp": REG_SP, "rbx": 7, "rdx": 8, "rcx": 9, "rax": 10,
        **{f"r{n}": n + 3 for n in range(8, 16)},                    # r8..r15 -> 11..18
        "cs": 19, "ss": 20, "ds": 21, "es": 22, "fs": 23, "gs": 24,
        "rflags": REG_FLAGS, "rip": REG_IP,
        **{f"vec{n}": 100 + n for n in range(32)},                  # xmm/ymm/zmm n -> 100+n
        **{f"k{n}": 132 + n for n in range(8)},
        **{f"st({n})": 140 + n for n in range(8)}, **{f"mm{n}": 140 + n for n in range(8)}}
_next = [160]

def reg_id(name):
    name = _GPR.get(name, name)
    if name in ("eflags", "flags"):
        name = "rflags"
    if name in ("eip", "ip"):
        name = "rip"
    m = re.fullmatch(r"[xyz]mm(\d+)", name)
    if m:
        name = "vec" + m.group(1)
    if name not in _ids:
        if _next[0] > 250:
            raise SystemExit("too many distinct registers")
        _ids[name] = _next[0]
        _next[0] += 1
    return _ids[name]

# ---- decoding + per-PC cache --------------------------------------------------------------
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
_cache = {}
BAD = [0]
COND_NOFLAGS = {"jrcxz", "jecxz", "jcxz", "loop", "loope", "loopne"}

def decode(pc, code):
    key = (pc, code)
    d = _cache.get(key)
    if d is not None:
        return d
    try:
        ins = next(md.disasm(code, pc))
    except StopIteration:
        BAD[0] += 1
        d = (len(code), None, (), ())          # undecodable: emit as a plain instruction
        _cache[key] = d
        return d
    if ins.mnemonic == "nop" and len(ins.operands) == 1 and ins.operands[0].type == X.X86_OP_MEM:
        disp = ins.operands[0].mem.disp & 0xFFFFFFFF
        if disp & 0xFFFFFF00 == DVFS_MAGIC:       # DVFS hint: reserved dest regs, no deps, no memory
            d = (ins.size, None, (), (REG_DVFS, (disp & 0xFF) + 1))
            _cache[key] = d
            return d
    rd, wr = ins.regs_access()
    rd = [reg_id(ins.reg_name(r)) for r in rd]
    wr = [reg_id(ins.reg_name(r)) for r in wr]
    other_r = [r for r in rd if r not in (REG_SP, REG_IP, REG_FLAGS)]
    g = set(ins.groups)
    mn = ins.mnemonic
    kind = None
    if X.X86_GRP_RET in g or X.X86_GRP_IRET in g:
        kind, src, dst = "ret", [REG_SP], [REG_SP, REG_IP]
    elif X.X86_GRP_CALL in g:
        if X.X86_GRP_BRANCH_RELATIVE in g:
            kind, src = "call", [REG_SP, REG_IP]
        else:                                   # call reg / call [mem]
            kind, src = "icall", [REG_SP, REG_IP] + other_r[:2]
        dst = [REG_SP, REG_IP]
    elif mn in COND_NOFLAGS:
        kind, src = "cond", [REG_IP] + other_r[:3]
        dst = [REG_IP] + [r for r in wr if r not in (REG_IP, REG_SP)][:1]
    elif X.X86_GRP_JUMP in g:
        if mn == "jmp":
            if X.X86_GRP_BRANCH_RELATIVE in g:
                kind, src = "jmp", []
            else:                               # jmp reg / jmp [mem]; must NOT list rip as a source
                kind, src = "ijmp", other_r[:4] or [reg_id("__ijmp_dummy")]
        else:
            kind, src = "cond", [REG_IP, REG_FLAGS]
        dst = [REG_IP]
    else:
        src = [r for r in rd if r != REG_IP]
        dst = [r for r in wr if r != REG_IP]
    src = list(dict.fromkeys(src))[:NSRC]
    dst = list(dict.fromkeys(dst))[:NDST]
    d = (ins.size, kind, tuple(src), tuple(dst))
    _cache[key] = d
    return d

# ---- writer -------------------------------------------------------------------------------
class Writer:
    def __init__(self, out, skip, count, start_pc=None):
        self.out, self.skip, self.count = out, skip, count
        self.start_pc = start_pc               # if set, -s counts from the first execution of this PC
        self.armed = start_pc is None
        self.seen = 0
        self.written = 0
        self.pending = None                    # (pc, size, kind, src, dst, smem, dmem)

    def will_keep(self, pc):
        """True if the next push() will need the instruction bytes (lets front ends skip decoding)."""
        armed = self.armed or pc == self.start_pc
        return armed and self.seen + 1 > self.skip

    def done(self):
        return self.written >= self.count

    def push(self, pc, code_or_none, smem, dmem):
        """code_or_none is None while still inside the skip window (avoids decoding)."""
        if self.pending is not None:
            self._emit(self.pending, pc)
            self.pending = None
        if not self.armed:
            if pc != self.start_pc:
                return
            self.armed = True
        self.seen += 1
        if self.seen <= self.skip or self.done():
            return
        size, kind, src, dst = decode(pc, code_or_none)
        self.pending = (pc, size, kind, src, dst, smem, dmem)

    def _emit(self, p, next_pc):
        pc, size, kind, src, dst, smem, dmem = p
        is_br = kind is not None
        taken = is_br and (kind != "cond" or (next_pc is not None and next_pc != pc + size))
        s = list(src) + [0] * (NSRC - len(src))
        d = list(dst) + [0] * (NDST - len(dst))
        sm = list(dict.fromkeys(a for a in smem if a))[:NSMEM]
        dm = list(dict.fromkeys(a for a in dmem if a))[:NDMEM]
        sm += [0] * (NSMEM - len(sm))
        dm += [0] * (NDMEM - len(dm))
        self.out.write(REC.pack(pc, int(is_br), int(taken), *d, *s, *dm, *sm))
        self.written += 1

    def finish(self):
        if self.pending is not None:
            self._emit(self.pending, None)
            self.pending = None

# ---- front end: DynamoRIO drmemtrace `-tool view` ------------------------------------------
IFETCH = re.compile(r"\bifetch\s+(\d+) byte\(s\) @ 0x([0-9a-f]+) (.*)$")
MEM = re.compile(r"\b(read|write|prefetch\S*)\s+\d+ byte\(s\) @ 0x([0-9a-f]+) by PC")
CONT = re.compile(r"^\s*\d+\s+\d+:\s+\S+\s+((?:[0-9a-f]{2} ?)+)\s*$")
HEX2 = re.compile(r"[0-9a-f]{2}")

def run_drview(f, w):
    cur = None                                 # [pc, bytes, need, smem, dmem]
    def flush():
        if cur is not None:
            w.push(cur[0], bytes(cur[1]) if w.will_keep(cur[0]) else None, cur[3], cur[4])
    for line in f:
        m = IFETCH.search(line)
        if m:
            flush()
            if w.done():
                break
            size = int(m.group(1))
            toks = []
            for t in m.group(3).split():
                if HEX2.fullmatch(t) and len(toks) < size:
                    toks.append(int(t, 16))
                else:
                    break
            cur = [int(m.group(2), 16), toks, size, [], []]
            continue
        if cur is None:
            continue
        m = MEM.search(line)
        if m:
            (cur[4] if m.group(1) == "write" else cur[3]).append(int(m.group(2), 16))
            continue
        if len(cur[1]) < cur[2]:
            m = CONT.match(line)
            if m:
                cur[1].extend(int(t, 16) for t in m.group(1).split())
    else:
        flush()
    w.finish()

# ---- front end: Valgrind lackey ------------------------------------------------------------
def load_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit("--elf must be a 64-bit ELF")
    e_type = struct.unpack_from("<H", data, 16)[0]
    if e_type != 2:
        raise SystemExit("--elf must be ET_EXEC (non-PIE); build with gcc -static (not -static-pie)")
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsz, phnum = struct.unpack_from("<HH", data, 54)
    segs = []
    for i in range(phnum):
        p_type, p_flags, p_off, p_vaddr, _, p_filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * phentsz)
        if p_type == 1 and (p_flags & 1):      # PT_LOAD, executable
            segs.append((p_vaddr, p_vaddr + p_filesz, p_off))
    def fetch(pc, n):
        for lo, hi, off in segs:
            if lo <= pc < hi:
                return data[off + pc - lo: off + pc - lo + n]
        return None
    return fetch

def run_lackey(f, w, fetch):
    cur = None                                 # [pc, size, smem, dmem]
    unknown = 0
    def flush():
        nonlocal unknown
        if cur is None:
            return
        code = None
        if w.will_keep(cur[0]):
            code = fetch(cur[0], cur[1])
            if code is None:
                unknown += 1
                code = b"\x90"                 # outside the ELF (e.g. vDSO): emit as a nop
        w.push(cur[0], code, cur[2], cur[3])
    for line in f:
        c = line[:2]
        if c == "I ":
            flush()
            if w.done():
                break
            a, s = line[3:].split(",")
            cur = [int(a, 16), int(s), [], []]
        elif cur is not None and c in (" L", " S", " M"):
            a = int(line[3:].split(",")[0], 16)
            if c != " S":
                cur[2].append(a)
            if c != " L":
                cur[3].append(a)
    else:
        flush()
    w.finish()
    if unknown:
        print(f"warning: {unknown} traced instructions were outside --elf", file=sys.stderr)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="src", choices=["drview", "lackey"], required=True)
    ap.add_argument("--elf", help="the traced binary (required for --from lackey)")
    ap.add_argument("-s", "--skip", type=int, default=0, help="instructions to drop first (like the Pin tracer's -s)")
    ap.add_argument("-t", "--count", type=int, default=1_000_000, help="instructions to write (like -t)")
    ap.add_argument("--start-pc", type=lambda x: int(x, 0),
                    help="start counting -s/-t at the first execution of this address, e.g. "
                         "$(nm BIN | awk '$3==\"main\"{print \"0x\"$1}') (like the Pin tracer's -start_symbol)")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("input", nargs="?", default="-")
    a = ap.parse_args()
    f = sys.stdin if a.input == "-" else open(a.input, errors="replace")
    with open(a.output, "wb") as out:
        w = Writer(out, a.skip, a.count, a.start_pc)
        if a.src == "drview":
            run_drview(f, w)
        else:
            if not a.elf:
                ap.error("--from lackey needs --elf")
            run_lackey(f, w, load_elf(a.elf))
    if BAD[0]:
        print(f"warning: Capstone could not decode {BAD[0]} distinct instructions (written without registers)", file=sys.stderr)
    if not w.armed:
        print("warning: --start-pc was never executed", file=sys.stderr)
    print(f"wrote {w.written} records", file=sys.stderr)

if __name__ == "__main__":
    main()
