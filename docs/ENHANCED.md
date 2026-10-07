# Silent Hill 2: Enhanced Edition on Switch

The Enhanced Edition support was written by r4lix and Le Z in [r4lix/sh2-nx](https://github.com/r4lix/sh2-nx) and
brought into this repository in v0.55; these are their notes, kept as they wrote them except where noted.

The patches were made against the unprotected v1.0 `sh2pc.exe` of the Enhanced Edition package (5,459,968 bytes,
SHA-1 `3d7e1161772e0a39cb9d57d92cfaf8b5e4871b04`). Its code is byte for byte that of the 5,685,248-byte exe this
repository builds from (SHA-1 `3201a5e1029f3ae3b77770255dcbe10360b3cd09`, the one SH2EE's setup installs): all 396
patches of `tools/patch_exe.py` verify against it.

## Building on Windows (no podman)

```sh
py -m venv .venv && .venv/Scripts/python -m pip install -r requirements.txt
# .venv/bin/python3 must exist for tools/regen.sh: a two-line sh shim calling ../Scripts/python.exe
sh tools/regen.sh                 # patches the exe (tools/patch_exe.py), then recompiles (~5 min)
sh tools/build-switch-local.sh    # devkitA64 + switch-sdl2, switch-mesa, switch-ffmpeg, switch-pkg-config
python tools/make_sd.py <SD root> --game <game folder> --keyconf <EE>/keyconf.dat --lang fr
```

## What changed

### Language
`language.ini` next to the NRO: `SET DX_CONFIG_LANGUAGE n`. The game maps n onto its message files
(0 j, 1 e, 2 f, 3 g, 4 i, 5 s): **2 = French**, 1 = English. `tools/make_sd.py --lang fr` writes it.
(v0.55: corrected from "1 g ... 5 e", since 1 shows the game in English.)

### Controller (`src/win32/dinput.c`, `src/game/ee.c`)
- DirectInput force feedback implemented (constant/ramp/periodic effects) and sent to the pad with
  `SDL_GameControllerRumble` (Switch HD rumble), scaled by the in-game Vibration option.
- EE ControllerTweaks: d-pad movement, right stick drives the search camera
  (`RestoreSearchCamMovement = 2`), walking while looking around.
- EE vibration fixes: no rumble outside gameplay / during fades, main-menu infinite rumble removed.
- `keyconf.dat` from the Enhanced Edition is used as the default button layout.

### Enhanced Edition patches
EE patches the running game from its d3d8.dll. Here:
- code byte patches are applied to the exe before recompilation (`tools/patch_exe.py`, each site
  verified against the original bytes);
- hooks become hand-written functions in `src/game/ee.c`: a `sub_X` there replaces the generated one,
  and `extern void sub_X_gen(void)` keeps the original body callable. `regen.sh` passes
  `--exclude-manual src/game/ee.c`; calls to wrapped functions route through `recomp_lookup_manual`
  (xboxrecomp change: `translate_batch_split(wrapped=...)`).
- new code needs an address: int3 padding between functions is turned into `ret` "caves" that the
  patched code calls and `ee.c` defines.
- `tools/eepattern.py` / `tools/eeaddr.py` resolve EE's search patterns to v1.0 addresses.

Ported so far: ControllerTweaks (search camera, d-pad, separate analogs), RestoreVibration,
PauseScreenFix.

### Upstream issues
- **Missing branch in MSVC switch chains** (xboxrecomp lifter): a jcc reached by both `cmp` and
  `sub`/`add`/`inc`/`dec` edges had no single flag snapshot and lifted as the never-true `_flags`
  fallback. Every ZF producer now also writes `_zr` (zero iff ZF), and mixed joins read it
  (`test_flag_join_switch_chain.py`). `prefetch*`/`lfence`/`mfence` no longer drop flag tracking.
  `tools/flagchains.py` lists what is left: 3DNow! paths (never taken: the runtime reports no
  3DNow!) and data decoded as code.
- **Stutter when areas or shaders load**: programs were compiled at first draw. Every program key is
  now appended to `sh2-progs.bin` (shaders identified by a hash of their GLSL) and rebuilt at the
  next start: fixed-function ones before the first draw, shader ones once the game created them.
- **Pause background not frozen in some rooms**: needs hardware testing.

## Status 2026-10-05 and next steps (EE assets)

- Director's Cut boots on hardware (title menu reached). Main-menu images are English/Japanese only in
  DC data (pic/etc/start00/01); French text is in-game. Mouse cursor (knife) is drawn: report no mouse.
- EE file redirection (EE Common/FileSystemHooks.cpp): `data/X` is loaded from `sh2e/X` when it exists
  (end.bik/ending.bik and start01 need start00 special cases). To do in `rt_path` with per-folder switches.
- sh2e sizes: movie 4.9 GB, sound 4.0 GB, pic 1.9 GB (281 files, 56 over 16 MB = 1.1 GB, up to 21 MB
  e.g. pic/map/*.tex), bg 117 MB, menu 11 MB, font 16 MB.
- HD textures need EE PatchTexAddr (TexPatch.cpp): texture load buffers at 0x401CC1 / 0x44B99D /
  0x496F87 / 0x49B40A (+ UFO 0x57E84E-0x28, 0x58C31E-0x28) moved to larger buffers. Port plan: reserve
  fixed guest VAs for the buffers, patch the immediates in tools/patch_exe.py, clear hook as an ee.c cave.
- Audio pack needs EE PatchCriware/SfxPatch (BGM size tables) before sh2e/sound can be used.
- sys-ftpd moved to port 5002 (config.ini.bak kept); sphaira FTP on 5000 is faster. Use curl only
  (tools/ftp_sync.py): sys-ftpd crashes on MLSD and on MKD of an existing directory.

## Status 2026-10-05 (evening): audio pack, widescreen

- **SFX bank** (EE SfxPatch): the sound bank buffer moved to guest memory (`SFX_BUF`, `tools/patch_exe.py`), and
  `ee_sfx_init` re-indexes the effect table at 0x8A67DC to the RIFF positions of `sh2e/sound/sddata.bin`.
  The game's own CRI streaming plays the pack's `.adx/.aix/.afs` unchanged (EE's CriWare reimplementation is a
  stability fix, not needed). `sound=1` in `sh2e.ini`.
- **Widescreen** (ThirteenAG's WidescreenFix, Fix2D part): `tools/ws_patches.py` moves ~330 operands to guest
  variables and calls nine text caves (`ee.c`, `ws_text_*` at 0x4011C0). `resolution=WxH` in `sh2e.ini`
  (default 1280x720; 1920x1080 docked; 960x720 is the original 4:3). Cutscene letterbox is hidden when wide.
  Not ported: mouse hitbox/cursor patches, EE's fullscreen image stretching (images stay 4:3, centred).
- **Heap quarantine** (`rt_core.c`): the 1280x720 crash was a use-after-free in the game that the immediate
  LIFO reuse of freed blocks exposed (the screen-sized image surface and the Konami logo texture shared a
  size class). Freed blocks now wait in a 64 MB quarantine with their contents intact.
- Debugging aids in `sh2e.ini`: `wsN=value` overrides widescreen variable N (see `ee_widescreen_init`),
  `trace=1` logs every bridged call.
- **"Des dossiers sont endommagés"**: a `Folder 01` save written earlier the same day (by a different exe build) was
  rejected by the v1.0 exe's content check after `sh2pc.sys` is read; removing it gives a normal new-game flow.
  A fresh save round trip (save, quit, reload) with the current build is still to be tested. `FindFirstFile` now
  returns real file times (were zeros); with `trace=1` file searches are logged.
- **Save "damaged folders" root cause (fixed in 0.2.2)**: the game reads `sh2pc.sys` with ReadFile, then reads again at EOF
  and expects 0 bytes with its buffer untouched. Calling `fread` for more bytes than remain zeroed the first 2048
  bytes of the destination buffer on Horizon, so the checksum failed. `file_io` (kernel32.c) now never reads more than
  the file holds. `FileTimeToLocalFileTime` now applies the UTC offset (the game stamps saves with local time).
  Save activity is logged to sh2.log (`save:` lines); `save: fn ...` lines log the save module's return values.
- Guest threads run on one core by default (`cores=3` in sh2e.ini to spread them) against the CRI audio looping bug.
  (v0.55: the bug's cause, lost `PulseEvent` pulses in kernel32.c, is fixed, so they spread over three cores by
  default and `cores=1` is the fallback.)
- **Grey screens (0.2.3)**: `GetFrontBuffer` read the *back buffer*, which the game had already cleared to grey (70,70,70)
  for the next frame, so the transition cross-fade and the save thumbnail were uniform grey. Present now keeps a copy
  of each presented frame (`res_snapshot_front`, d3d8_res.c) and GetFrontBuffer returns that. The capture function is
  FUN_00477020 (called from 0x479390); `front buffer:` lines in sh2.log show the average colour of each capture.
- **Map screen (0.2.4)**: the EE HD map pages (`sh2e/pic/map`, `pic/add/map*`) need EE's FullscreenImages map code (MapWidthASM,
  MapXPosASM, map icon scaling), which is not ported: with them the map drew 160 px too far right and cropped. By default
  those pages now come from the original `data/pic/map` (the layout WidescreenFix was written for); `hdmaps=1` in sh2e.ini
  opts back in to the HD pages. Everything else in `pic` stays HD.
