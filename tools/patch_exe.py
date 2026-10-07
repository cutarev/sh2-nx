#!/usr/bin/env python3
"""Applies the Silent Hill 2 Enhancements code patches to sh2pc.exe (v1.0) before recompilation.

Recompiled code is static C, so EE's run-time code patches have to happen to the bytes the
recompiler reads. Patches that need new code call a "cave" (int3 padding between functions, given a
`ret` here) that src/game/ee.c defines by hand. Every site is verified against the original bytes.

Usage: patch_exe.py game/sh2pc.exe build/sh2pc_ee.exe
"""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ws_patches import widescreen_patches

BASE = 0x400000
CAVE_SEARCH_CAMERA = 0x401460   # ee.c: StartSearchCamera_Hook
CAVE_PAUSE_OPTIONS = 0x4015D0   # ee.c: PauseScreenASM
CAVE_TEX_CLEAR = 0x401970       # ee.c: TexBufferASM
CAVE_TITLE_LANG = 0x401D60      # ee.c: MainMenuTitleASM
# EE TexPatch buffers, in guest memory between the image and the heap (0x2500000-0x10000000):
# 2x and 4x the largest texture file (22,380,640 bytes, sh2e/pic/map), as EE sizes them.
TEX_BUF1, TEX_BUF2, TEX_BUF3 = 0x04000000, 0x06B00000, 0x09600000
# SfxPatch: the sddata.bin load buffer (ee.c commits it for the size of the file in use).
SFX_BUF = 0xD0000000
# Guest variables owned by ee.c (0x0EC00000 page, committed in ee_init).
EE_TEX_SCALE_X, EE_TEX_SCALE_Y = 0x0EC01000, 0x0EC01004


def call(at, target):
    return b'\xE8' + struct.pack('<i', target - (at + 5))


def p32(v):
    return struct.pack('<I', v)


def p16(v):
    return struct.pack('<H', v & 0xFFFF)


def f32(v):
    return struct.pack('<f', v)


# The Enhanced Edition title and save screen art (sh2e/pic/etc/start00, sh2e/menu/mc/savebg) is 2732x2048.
EE_ART_RATIO = struct.unpack('<f', struct.pack('<f', 2732 / 2048))[0]
EE_ART_POSX = int(4096 * EE_ART_RATIO)


# (va, expected original bytes, replacement, why)
PATCHES = [
    # -- caves: int3 padding becomes `ret` so the disassembler sees a function there
    (CAVE_SEARCH_CAMERA, b'\xCC' * 4, b'\xC3\xCC\xCC\xCC', 'cave: search camera hook'),
    (CAVE_PAUSE_OPTIONS, b'\xCC' * 4, b'\xC3\xCC\xCC\xCC', 'cave: pause options exit'),

    # -- ControllerTweaks (RestoreSearchCamMovement): the search camera reads the right stick
    (0x535CD1, p32(0x1FB8054), p32(0x1FB804C), 'search camera X <- right stick X'),
    (0x535D53, p32(0x1FB8054), p32(0x1FB804C), 'search camera X <- right stick X'),
    (0x535CE4, p32(0x1FB8058), p32(0x1FB8050), 'search camera Y <- right stick Y'),
    (0x535D6B, p32(0x1FB8058), p32(0x1FB8050), 'search camera Y <- right stick Y'),
    # StartSearchCamera_Hook: `mov eax, [moveDirection]` -> call; keep `test eax, eax`; drop `je; cmp`
    (0x535CBD, bytes.fromhex('a1e87cfb01'), call(0x535CBD, CAVE_SEARCH_CAMERA), 'start search camera hook'),
    (0x535CC4, bytes.fromhex('740983f803'), b'\x90' * 5, 'start search camera hook'),

    # -- PauseScreen fix: leaving Options from the pause menu returns to gameplay cleanly
    (0x4697B5, bytes.fromhex('b810000000'), call(0x4697B5, CAVE_PAUSE_OPTIONS), 'pause screen options exit'),

    # -- XInput vibration: the main menu's rumble call never stops (infinite rumble)
    (0x497D71, bytes.fromhex('e8ba05fcff'), b'\x90' * 5, 'menu infinite rumble'),

    # -- TexPatch (EnableTexAddrHack): texture load buffers moved to guest memory sized for the EE
    # HD textures (ee.c commits it at start). The original buffer is at 0x1DBC040.
    (CAVE_TEX_CLEAR, b'\xCC' * 4, b'\xC3\xCC\xCC\xCC', 'cave: texture buffer clear'),
    (0x401CC1, p32(0x1DBC040), p32(TEX_BUF1), 'texture buffer 1 (static)'),
    (0x44B99E, p32(0x1DBC040), p32(TEX_BUF1), 'texture buffer 1'),
    (0x496F87, bytes.fromhex('0500000800'), b'\xB8' + p32(TEX_BUF2), 'texture buffer 2 (add -> mov)'),
    (0x49B40A, bytes.fromhex('0500481000'), b'\xB8' + p32(TEX_BUF2), 'texture buffer 2 (add -> mov)'),
    (0x57E826, p32(0x1DBC040), p32(TEX_BUF3), 'lake UFO buffer'),
    (0x57E835, p32(0x1DBC040), p32(TEX_BUF3), 'lake UFO buffer'),
    (0x58C2F6, p32(0x1DBC040), p32(TEX_BUF3), 'hospital UFO buffer'),
    (0x58C305, p32(0x1DBC040), p32(TEX_BUF3), 'hospital UFO buffer'),
    (0x44A00D, call(0x44A00D, 0x448810), call(0x44A00D, CAVE_TEX_CLEAR), 'texture load: clear buffer after'),

    # -- FullscreenImages / Start00Scaling: the title (pic/etc/start00) and save screen background
    # (menu/mc/savebg) drawn at the aspect of the Enhanced Edition art, 2732x2048. EE computes these
    # from the texture size at run time: ScaleX = 8192 * ratio, PosX = 4096 * ratio,
    # Width = 61440 - (PosX - 4096). The data half (save screen PosX/Width, logo Y) is in ee.c.
    (0x4969C8, f32(8192.0), f32(8192.0 * EE_ART_RATIO), 'title scale X'),
    (0x4969A3, p16(4096), p16(EE_ART_POSX), 'title pos X'),
    (0x496991, p16(61440), p16(61440 - (EE_ART_POSX - 4096)), 'title width'),
    (0x49699A, p16(0xF200), p16(61698), 'title height top'),
    (0x4969AC, p16(3584), p16(3838), 'title height bottom'),
    (0x496A46, p16(-103 & 0xFFFF), p16(-107 & 0xFFFF), 'title logo highlight height'),
    (0x44B4F4, f32(8192.0), f32(8192.0 * EE_ART_RATIO), 'save screen scale X'),
    (0x44B495, bytes.fromhex('66a33cc99300'), b'\x90' * 6, 'save screen width store (ee.c sets it)'),
    (0x44B4A6, bytes.fromhex('66a340c99300'), b'\x90' * 6, 'save screen pos X store (ee.c sets it)'),

    # -- MainMenu (PatchMainMenuTitlePerLang): the title menu image per language (start01f.tex...)
    # and the menu text placed for the Enhanced Edition art.
    (CAVE_TITLE_LANG, b'\xCC' * 4, b'\xC3\xCC\xCC\xCC', 'cave: title per language'),
    (0x496F30, bytes.fromhex('33f683c8ff'), call(0x496F30, CAVE_TITLE_LANG), 'title per language hook'),
    (0x4979E3, p32(-198 & 0xFFFFFFFF), p32(-244 & 0xFFFFFFFF), 'title menu layout'),
    (0x4979EE, p16(408), p16(508), 'title menu layout'),
    (0x497A1E, p32(-148 & 0xFFFFFFFF), p32(-180 & 0xFFFFFFFF), 'title menu layout'),

    # -- FullscreenImages (core): sprite texture coordinates are divided by TextureScaleX/Y (was the
    # constant 1.0 at 0x62EDF4), which ee.c sets per loaded texture to new size / original size.
    (0x49F4BA, p32(0x62EDF4), p32(EE_TEX_SCALE_X), 'texture scale X'),
    (0x49F4CB, p32(0x62EDF4), p32(EE_TEX_SCALE_Y), 'texture scale Y'),

    # -- SfxPatch (EnableSFXAddrHack): the sound bank buffer passed to the DirectSound setup.
    (0x515164, p32(0xBDDD40), p32(SFX_BUF), 'sound bank buffer'),

    # -- SaveBGImage: the save screen shows the background of the chapter being saved (ee.c resets
    # the chapter to the main scenario at the main menu).
    (0x44B419, bytes.fromhex('7d1d'), b'\x90\x90', 'save screen background per chapter'),
]


def main(src, dst):
    data = bytearray(open(src, 'rb').read())
    patches = PATCHES + widescreen_patches(data, call)
    for va, old, new, why in patches:
        o = va - BASE
        if data[o:o + len(old)] != old:
            sys.exit(f'{va:#x} ({why}): expected {old.hex()} found {data[o:o + len(old)].hex()}')
        data[o:o + len(new)] = new
    open(dst, 'wb').write(data)
    print(f'{len(patches)} patches -> {dst}')


if __name__ == '__main__':
    main(*sys.argv[1:3])
