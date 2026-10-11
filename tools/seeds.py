#!/usr/bin/env python3
"""Function entry points the disassembler cannot reach by calls: code addresses that appear as
data (callback tables) or as immediates (push offset thread_proc), and that start right after
padding or a ret, the way MSVC lays functions out. Writes build/seeds.json for tools.disasm.

    python tools/seeds.py
"""
import bisect
import itertools
import json
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM

EXE, XREFS, OUT = 'game/sh2pc.exe', 'build/disasm/xrefs.json', 'build/seeds.json'
d = open(EXE, 'rb').read()
BASE, T0, T1 = 0x400000, 0x401000, 0x629000


def byte_at(va):
    return d[va - BASE]  # this exe's file offsets equal its RVAs up to .data


def looks_like_entry(va):
    return T0 <= va < T1 and byte_at(va - 1) in (0xCC, 0x90, 0xC3) and byte_at(va) not in (0xCC, 0x90, 0x00)


# Inside a function the first pass found, an address is usually a label of it (a loop MSVC aligned with
# a nop, a switch case): seeding 0x4A9100 split 0x4A8D70 in two and skipped half of it.
funcs = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open('build/disasm/functions.json')))
starts = [a for a, _ in funcs]


def containing(va):
    i = bisect.bisect_right(starts, va) - 1
    return funcs[i] if i >= 0 and va < funcs[i][1] else None


md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def follows_transfer(va, fn_start):
    """Decoding the function from its start, va is an instruction boundary whose last non-padding
    predecessor is a ret or an unconditional jmp: no path falls into it, so it starts a function
    the first pass swallowed (they sit after `jmp tail; nop...` too)."""
    last = None
    for ins in md.disasm(d[fn_start - BASE:va - BASE], fn_start):
        if ins.mnemonic not in ('nop', 'int3'):
            last = ins
        end = ins.address + ins.size
    return last is not None and end == va and last.mnemonic in ('ret', 'jmp')


def padded_entry(va):
    """16-byte aligned after a run of 2+ nop/int3: how MSVC pads between functions. Inside a function it
    aligns loops with one nop or with lea filler, never a run of nops."""
    run = 0
    while byte_at(va - 1 - run) in (0x90, 0xCC):
        run += 1
    return va % 16 == 0 and run >= 2


_reach_cache = {}


def reached(fn):
    """Instruction addresses reachable from the function's start by falling through and direct jumps
    (calls not followed, nothing past the function's end): its own code, unlike a linear sweep that
    runs out of phase on an embedded jump table."""
    if fn in _reach_cache:
        return _reach_cache[fn]
    start, end = fn
    seen, todo = set(), [start]
    while todo:
        a = todo.pop()
        while start <= a < end and a not in seen:
            ins = next(md.disasm(d[a - BASE:a - BASE + 16], a), None)
            if ins is None:
                break
            seen.add(a)
            seen.update(range(a + 1, a + ins.size))  # inside an instruction counts as reached code too
            m = ins.mnemonic
            if m.startswith('j') and ins.operands and ins.operands[0].type == X86_OP_IMM:
                todo.append(ins.operands[0].imm)
                if m == 'jmp':
                    break
            elif m == 'jmp' and ins.operands and ins.operands[0].type == X86_OP_MEM:
                mem = ins.operands[0].mem
                if not mem.base and mem.index and mem.scale == 4:  # switch: jmp [reg*4 + table]
                    t = mem.disp & 0xFFFFFFFF
                    while T0 <= t < T1:
                        target = struct.unpack_from('<I', d, t - BASE)[0]
                        if not start <= target < end:
                            break
                        todo.append(target)
                        t += 4
                break
            elif m in ('ret', 'jmp', 'int3') or m.startswith('ret'):
                break
            a += ins.size
    _reach_cache[fn] = seen
    return seen


def boundary_in_code(va, fn_start):
    """The function's own code reaches va, as a label or inside an instruction (a constant in .data
    that happens to equal an address in the middle of a jne): not a function of its own. Every
    function spanning va counts: a jcc alias (0x530200) overlaps its parent (0x52FE90), and only the
    parent's switch reaches the call at 0x5307FF that a .data constant 0x530800 points into."""
    return any(va in reached(f) for f in funcs if f[0] <= va < f[1])


def gap_entry(va):
    return containing(va) is None and T0 <= va < T1 and va % 16 == 0 and byte_at(va) not in (0xCC, 0x90, 0x00)


def gap_label(va):
    """An earlier seed in the same gap reaches va: a label of that undetected function, like 0x4B7690,
    the target of a jmp inside 0x4B75F0."""
    i = bisect.bisect_right(starts, va)
    hi = starts[i] if i < len(starts) else T1
    lo = ends_before[i - 1] if i else T0
    return any(va in reached((g, hi)) for g in gap_entries[bisect.bisect_left(gap_entries, lo):
                                                           bisect.bisect_left(gap_entries, va)])


def is_seed(va):
    fn = containing(va)
    if fn is not None and va in data_ptrs and va % 16 == 0 and not boundary_in_code(va, fn[0]):
        return True  # a function pointer table names it and decoding never reaches it: data sits before it
    if gap_entry(va):
        return not gap_label(va)  # aligned like a function in a gap: a jmp thunk after a jump table (0x54D6A0)
    if not looks_like_entry(va):
        return False
    return fn is None or padded_entry(va) or follows_transfer(va, fn[0])


data_ptrs = set()
for name, va, raw, size in (('.rdata', 0x629000, 0x229000, 0x16F494), ('.data', 0x799000, 0x399000, 0x199000)):
    for o in range(0, size - 3, 4):
        data_ptrs.add(struct.unpack_from('<I', d, raw + o)[0])
cands = set(data_ptrs)
xrefs = json.load(open(XREFS))
cands |= {int(x['to'], 16) for x in xrefs if x['type'] in ('data_imm', 'call', 'jump')}
# A jcc out of a function into a gap: code after a ret that the function branches back into (0x4D0E85, the
# state-0 path of an effect's update at 0x4D0E20). Unseeded, the jcc lifts as a tail call to a stub.
# Only jumps the function's own code reaches count: decoding a jump table yields jccs into gaps too.
cands |= {int(x['to'], 16) for x in xrefs if x['type'] == 'cond_jump' and containing(int(x['to'], 16)) is None
          and containing(int(x['from'], 16)) and int(x['from'], 16) in reached(containing(int(x['from'], 16)))}
ends_before = list(itertools.accumulate((e for _, e in funcs), max))  # aliases overlap their parent
gap_entries = sorted(a for a in cands if gap_entry(a))
observed = {int(l.split()[0], 16) for l in open('tools/observed_seeds.txt') if l.strip() and not l.startswith('#')}
seeds = sorted(a for a in cands if is_seed(a))
trusted = {a for a in seeds if containing(a) and a in data_ptrs} | observed  # may split a mis-swept function
json.dump([{'start': f'0x{a:08X}', **({'observed': True} if a in trusted else {})}
           for a in sorted(set(seeds) | observed)], open(OUT, 'w'), indent=0)
print(f'{len(seeds)} seeds -> {OUT}')
