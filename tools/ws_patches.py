"""WidescreenFix (ThirteenAG's SilentHill2.WidescreenFix, bundled with the Enhanced Edition), Fix2D part,
for the v1.0 exe. Imported by patch_exe.py.

The game lays its 2D out in a 4:3 area of the screen height (the width variable at 0xA33480 holds the
4:3 width) and the wide screen adds offsets. WidescreenFix finds its sites by byte patterns; here they
are resolved against the exe and their operands moved to guest variables that ee.c fills in
(ee_widescreen_init). At 4:3 every variable equals the original value, so 960x720 plays as before.
"""
import re, struct

BASE = 0x400000
SCR = 0x0EC01100
(V_W, V_H, V_FW, V_FH, V_HUD, V_W43, V_TEXT, V_F_05, V_F05, V_N640, V_N480, V_BORDER0, V_BORDER1) = (
    SCR + 4 * i for i in range(13))
WIDTH_VAR = 0xA33480
FLT_HALF = 0x62FE1C
CAVE_WS = 0x4011C0  # eight bytes apart: ee.c ws_text_* (the text position hooks)

F05_INDICES = {
    29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 75, 77, 79, 81, 83, 84, 85,
    86, 87, 88, 89, 91, 93, 95, 97, 99, 101, 103, 105, 107, 109, 111, 113, 120, 122, 124, 126, 128, 130, 132,
    134, 136, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164, 166, 168, 170, 172, 174,
    176, 178, 180, 182, 184, 186, 188, 190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216,
    218, 220, 222, 224, 226, 228, 230, 232, 234, 236, 238, 240, 242, 244, 246, 248, 250, 252, 254, 256, 258,
    260, 262, 264, 266, 268, 270, 272, 274, 276, 278, 280, 282, 284, 286, 288, 290, 292, 294, 296, 298, 300,
    302, 304, 306, 308, 310, 312, 314, 316, 318, 320, 322, 324, 326, 328, 330, 332, 334, 336, 338, 340, 342,
    344, 346, 348, 350, 352, 354, 356, 370, 372, 386, 388, 390, 392, 394, 396, 398, 400, 402, 404, 406, 408,
    410, 412, 414, 416, 418, 420, 422, 424, 426, 428, 430, 432, 435, 437, 439, 441, 443, 445, 447, 449, 451,
    453, 455, 457, 459, 461, 463, 469, 471, 473, 475, 477, 479, 481, 483, 485, 487, 489, 491, 493, 495, 497,
    499, 501, 503, 505, 507, 509, 511, 513, 515, 517, 519, 521, 523, 525, 527, 529, 531, 533, 535, 537, 539,
    541, 543, 545, 547, 549, 551, 553, 555, 557, 559, 561, 563, 565, 567, 569, 571, 574, 576, 578, 580, 582,
    584, 586, 588, 590, 592, 594, 596, 598, 600, 602, 604, 606, 608, 628, 630, 632, 634, 682, 610, 612, 614,
    616, 618, 620, 622, 624, 626}
PIC_INDICES = {318, 315, 314, 313, 310, 312, 311, 320, 178, 177, 176, 175, 174, 327, 2, 309, 173, 317, 316,
               332, 330, 333}

# (pattern, bytes of instructions the cave replaces) for ee.c's ws_text_0 ... ws_text_8
TEXT_HOOKS = (
    ('89 54 24 04 0F B7 50 ? D8 0D ? ? ? ? 42 D8 C2', 8),
    ('D9 1D ? ? ? ? 8B 08 FF 91', 6),
    ('89 94 24 A8 00 00 00 DB 44 24 18 89 94 24 D0', 7),
    ('D9 94 24 90 00 00 00 D9 C1 D9 9C 24 94 00 00 00', 7),
    ('DB 05 ? ? ? ? 7D 06 D8 05 ? ? ? ? 0F BE 0D ? ? ? ? 8B', 6),
    ('D9 54 24 58 D9 C9 D9 5C 24 5C D9 5C 24 6C DB 44 24 24', 6),
    ('DB 05 ? ? ? ? 7D ? D8 05 ? ? ? ? D9 5C 24 20 0F BE 15', 6),
    ('D9 54 24 40 D9 C1 D9 5C 24 44 D9 CA', 6),
    ('A1 ? ? ? ? 8B 10 6A 14 8D 4C 24 30 51 6A 03 6A 02 50', 5),
)

# (pattern, operand offset, new operand, what)
OPERANDS = (
    ('A1 ? ? ? ? 8B 0D ? ? ? ? 48 89 44 24 0C 89', 1, V_W, 'cutscene border width'),
    ('8B 15 ? ? ? ? 50 52 6A 00 6A 00 33 C0', 2, V_W, 'cutscene border width'),
    ('8B 0D ? ? ? ? 50 51 56 6A 00 33 C0', 2, V_W, 'cutscene border width'),
    ('A1 ? ? ? ? 8B 0D ? ? ? ? 48 51 50 53 50 33 C0', 1, V_W, 'width'),
    ('8B 15 ? ? ? ? 52 C7 44 24 14 00 00 00 00', 2, V_W, 'width'),
    ('8B 15 ? ? ? ? D1 EA 85 D2 89 54 24 04', 2, V_W, 'flashlight'),
    ('D9 05 ? ? ? ? D8 0D ? ? ? ? D9 1D ? ? ? ? D9 05 ? ? ? ? D8 0D ? ? ? ? D9 1D ? ? ? ? D9 05 ? ? ? ? D8 35',
     2, V_FW, 'flashlight float width'),
    ('DB 05 ? ? ? ? 85 C0 7D 06 D8 05 ? ? ? ? 8B 0D ? ? ? ? D9 1D ? ? ? ? DB 05 ? ? ? ?', 2, V_W43,
     'flashlight 4:3 width'),
    ('8B 0D ? ? ? ? D1 E9 85 C9 89 4C 24 10 DB 44 24 10', 2, V_W, 'Toluca lake light'),
    ('8B 0D ? ? ? ? 51 6A 00 03 D1 52 33 C0', 2, V_W, 'screen position overlay'),
    ('8B 0D ? ? ? ? 50 51 33 C0 6A 00 50', 2, V_W, 'screen position overlay'),
    ('8B 15 ? ? ? ? 51 52 03 C1 50 33 C0 50', 2, V_W, 'screen position overlay'),
    ('A1 ? ? ? ? 8B 0D ? ? ? ? BD 01 00 00 00', 1, V_W, 'width'),
    ('DB 05 ? ? ? ? A1 ? ? ? ? 85 C0 7D 06', 2, V_W, 'stretching'),
    ('DB 05 ? ? ? ? C7 44 24 0C 0A D7 23 3C C7', 2, V_W43, 'menu hitboxes'),
    ('D9 05 ? ? ? ? 0F BF 46 0C D8 C9 89 54 24', 2, V_HUD, 'image hud offset'),
    ('D9 05 ? ? ? ? 0F BE 15 ? ? ? ? D8 C9 A1 ? ? ? ? 85 C0', 2, V_F05, 'image position'),
    ('A1 ? ? ? ? D9 15 ? ? ? ? D9 C2 89 15 ? ? ? ? D9 1D', 1, V_TEXT, 'FMV offset'),
    ('D8 25 ? ? ? ? 8B 0D ? ? ? ? 85 C9 8B 15', 2, V_TEXT, 'FMV offset'),
    ('8B 15 ? ? ? ? A1 ? ? ? ? 89 15 ? ? ? ? A3', 2, V_TEXT, 'FMV offset'),
    ('DB 05 ? ? ? ? 50 A1 ? ? ? ? 85 C0 7D 06 D8 05', 2, V_N640, 'loading text X'),
    ('DB 05 ? ? ? ? 8B 15 ? ? ? ? 85 D2 7D 06 D8 05', 2, V_N480, 'loading text Y'),
)


def p32(v):
    return struct.pack('<I', v)


def widescreen_patches(data, call):
    """(va, old, new, why) tuples; `call(at, target)` encodes an E8 call."""
    raw = bytes(data)

    def find(pat):
        rx = b''.join(b'.' if t == '?' else re.escape(bytes([int(t, 16)])) for t in pat.split())
        return [m.start() + BASE for m in re.finditer(rx, raw, re.S)]

    out = []

    def at(va, n):
        return raw[va - BASE:va - BASE + n]

    def operand(va, new, why):
        out.append((va, at(va, 4), p32(new), why))

    def code(va, n, new, why):
        out.append((va, at(va, n), new.ljust(n, b'\x90'), why))

    width_use = find('DB 05 ' + ' '.join(f'{b:02X}' for b in p32(WIDTH_VAR)))
    for k, i in enumerate(sorted(PIC_INDICES)):  # one variable each, SCR + 0x100 + 4 k (all the width)
        operand(width_use[i] + 2, SCR + 0x100 + 4 * k, f'2D width {i}')
    for pat, off, var, why in OPERANDS:
        h = find(pat)
        assert h, pat
        if var == V_W:  # one variable each (SCR + 0x200 + 4 j, all the width) so ee.c can bisect them
            var = SCR + 0x200 + 4 * sum(1 for o in out if o[3].startswith('w:'))
            why = 'w: ' + why
        operand(h[0] + off, var, why)
    h = find('D8 0D ? ? ? ? D8 44 24 0C D8 44 24 18 D8 25 ? ? ? ? D9 54 24 1C')
    operand(h[1] + 2, V_F_05, 'image position')
    # cutscene letterbox: 0.5/0.5 is the game's, 0/1 hides the bars (ee.c picks by aspect ratio)
    h = find('D8 0D ? ? ? ? DE C1 DA 44 24 ? E8')
    assert len(h) == 2
    operand(h[0] + 2, V_BORDER0, 'cutscene border')
    operand(h[1] + 2, V_BORDER1, 'cutscene border')
    # the width variable is set to the 4:3 width by ee.c instead of the game's stores
    code(find('A3 ? ? ? ? 89 44 24 14 A1 ? ? ? ? 8D 4C 24 0C 51')[0], 5, b'', 'width store (startup)')
    code(find('89 15 ? ? ? ? C6 05 ? ? ? ? 01 33 C0 C3')[0], 6, b'', 'width store (mode change)')
    # the 0.5 constants of the HUD, shifted by the wide screen's half-offset
    half = find('D8 0D ' + ' '.join(f'{b:02X}' for b in p32(FLT_HALF)))
    for i in sorted(F05_INDICES):
        operand(half[i] + 2, V_HUD, f'hud offset {i}')
    # camera pan/tilt scale: the game asked the window size; use the 4:3 constants
    h = find('51 E8 ? ? ? ? 89 44 24 00 DB 44 24 00 D9 1D')[0]
    for off, v in ((1, 512), (39, 512), (20, 448), (64, 448)):
        assert at(h + off, 1) == b'\xE8'
        code(h + off, 5, b'\xB8' + p32(v), f'camera constant {v}')
    for i, (pat, n) in enumerate(TEXT_HOOKS):
        h = find(pat)
        assert len(h) == 1, pat
        va, cave = h[0], CAVE_WS + 8 * i
        out.append((va, at(va, n), call(va, cave).ljust(n, b'\x90'), f'text position hook {i}'))
        out.append((cave, b'\xCC' * 4, b'\xC3\xCC\xCC\xCC', f'cave: text position {i}'))
    return out
