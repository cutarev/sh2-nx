#!/usr/bin/env python3
"""Lays out the SD card folder: <out>/switch/sh2-nx/ with the NRO, the exe, data/, the Enhanced Edition's
sh2e/ (without its FMV pack, which the port cannot play at the right speed yet) and the settings files.

Usage: make_sd.py <out dir> [--game <PC game folder>] [--keyconf <keyconf.dat>] [--lang en|fr|de|it|es|jp]
Files already present and the same size are not copied again.
"""
import argparse, os, shutil

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
# DX_CONFIG_LANGUAGE as the game reads it from language.ini: 0..5 are its j/e/f/g/i/s message files.
LANG = {'jp': 0, 'en': 1, 'fr': 2, 'de': 3, 'it': 4, 'es': 5}
SH2E_INI = open(os.path.join(ROOT, 'docs', 'sh2e.ini.example')).read()


def copy(src, dst):
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src):
        return 0
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return 1


def copy_tree(src, dst, skip=()):
    n = 0
    for dp, dns, fs in os.walk(src):
        dns[:] = [d for d in dns if os.path.relpath(os.path.join(dp, d), src) not in skip]
        for f in fs:
            n += copy(os.path.join(dp, f), os.path.join(dst, os.path.relpath(os.path.join(dp, f), src)))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--game', default=os.path.join(ROOT, 'game'))
    ap.add_argument('--keyconf')
    ap.add_argument('--lang', default='en', choices=LANG)
    a = ap.parse_args()
    dst = os.path.join(a.out, 'switch', 'sh2-nx')
    n = copy(os.path.join(ROOT, 'build', 'switch', 'sh2-nx.nro'), os.path.join(dst, 'sh2-nx.nro'))
    n += copy(os.path.join(a.game, 'sh2pc.exe'), os.path.join(dst, 'sh2pc.exe'))
    n += copy_tree(os.path.join(a.game, 'data'), os.path.join(dst, 'data'))
    if os.path.isdir(os.path.join(a.game, 'sh2e')):
        n += copy_tree(os.path.join(a.game, 'sh2e'), os.path.join(dst, 'sh2e'), skip=('movie', 'ps2'))
        if not os.path.exists(os.path.join(dst, 'sh2e.ini')):
            with open(os.path.join(dst, 'sh2e.ini'), 'w', newline='\n') as f:
                f.write(SH2E_INI)
    if a.keyconf:
        n += copy(a.keyconf, os.path.join(dst, 'keyconf.dat'))
    with open(os.path.join(dst, 'language.ini'), 'w', newline='\n') as f:
        f.write(f'SET DX_CONFIG_LANGUAGE {LANG[a.lang]}\n')
    print(f'{n} files copied to {dst} (language: {a.lang})')


if __name__ == '__main__':
    main()
