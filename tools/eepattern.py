#!/usr/bin/env python3
"""Find SH2 Enhancements byte patterns ("33 DB ? ? C3") in game/sh2pc.exe and print their VAs.

Usage: eepattern.py "33 DB 3B C3 74 33" ["A1 ? ? ? ? 85 C0" ...]
In this exe the file offset of every section equals its RVA, so VA = 0x400000 + offset.
"""
import os, re, struct, sys

EXE = os.path.join(os.path.dirname(__file__), '..', 'game', 'sh2pc.exe')
BASE = 0x400000


def find(data, pat):
    rx = b''.join(b'.' if t == '?' else re.escape(bytes([int(t, 16)])) for t in pat.split())
    return [m.start() + BASE for m in re.finditer(rx, data, re.S)]


if __name__ == '__main__':
    data = open(EXE, 'rb').read()
    for pat in sys.argv[1:]:
        hits = find(data, pat)
        print(f'{pat:48} -> {", ".join(hex(h) for h in hits) or "NOT FOUND"}')
