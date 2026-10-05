# sh2-nx

Silent Hill 2 (PC, Director's Cut / Restless Dreams) running natively on the Nintendo Switch.

This is not an emulator. The game's x86 executable is statically recompiled to C ahead of time, and
the C is compiled for the Switch's ARM64 CPU. The Windows layer the game expects is reimplemented on
top of SDL2, OpenGL and FFmpeg:

| Windows | here |
|---|---|
| Direct3D 8 (fixed function, vs/ps 1.x) | OpenGL 4.3 core, shaders translated to GLSL (`src/d3d8`) |
| DirectSound | software mixer on SDL audio (`src/win32/dsound.c`) |
| DirectInput 8 | keyboard, mouse and gamepad through SDL (`src/win32/dinput.c`) |
| Bink | FFmpeg's Bink decoders (`src/win32/bink.c`) |
| kernel32, user32, msvcr70, ... | `src/win32`, `src/runtime` |

It also builds for Linux (x86-64), which is where most debugging happens.

**No game content is included.** You need your own copy of the game.

## Status

Playable: menus, gameplay, movies with synced audio, saving and loading. Known issues:

- some MSVC `switch` chains still lift with a missing branch (`grep -c '_flags /\*' src/recomp/gen/*.c`);
- the pause screen background is not frozen in some rooms;
- occasional stutter when areas or shaders load.

## Game files

The recompiled code is tied to one exact executable:

- `sh2pc.exe`, 5,685,248 bytes, SHA-1 `3201a5e1029f3ae3b77770255dcbe10360b3cd09`

plus the game's `data/` folder (about 2.4 GB).

## Building

Requirements: Python 3 with the packages in `requirements.txt`, CMake, Ninja, SDL2 and FFmpeg
development files (Fedora: `dnf install SDL2-devel ffmpeg-free-devel`), and podman for the Switch build.

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
ln -s "/path/to/Silent Hill 2" game     # the folder holding sh2pc.exe and data/
tools/regen.sh                          # recompiles game/sh2pc.exe into src/recomp/gen (~3 min)

# Linux
cmake -S . -B build/linux -G Ninja && cmake --build build/linux
build/linux/sh2 game

# Switch (devkitPro's devkita64 image under podman)
tools/build-switch.sh                   # -> build/switch/sh2-nx.nro
```

## Installing on the Switch

Copy to the SD card:

```
sdmc:/switch/sh2-nx/sh2-nx.nro
sdmc:/switch/sh2-nx/sh2pc.exe
sdmc:/switch/sh2-nx/data/
```

Launch it from the homebrew menu in title takeover mode (hold R while starting a game), not from the
album: applet mode does not give homebrew enough memory. Settings and saves are written next to it.

## Layout

- `tools/regen.sh`: the whole recompilation pipeline (PE to XBE wrapper, disassembly with extra
  function seeds from `tools/seeds.py`, ABI analysis, C generation).
- `third_party/xboxrecomp`: [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) by sp00nznet (MIT, with
  LGPL-2.1 parts, see its `LICENSE`, `NOTICE` and `LICENSES/`), the x86 to C lifter, with fixes made for
  this port (flag joins, jump tables, x87/MMX corner cases, setjmp/longjmp).
- `src/runtime`: guest memory, threads and the host platform layer (Linux and Horizon).
- `src/win32`, `src/d3d8`: the Windows APIs the game imports.

## Debugging aids

| variable | effect |
|---|---|
| `SH2_KEYS="12:Return,30:W+5"` | scripted key presses (seconds, key, optional hold); the window is hidden and the real keyboard ignored |
| `SH2_SHOT=dir`, `SH2_SHOT_EVERY=n` | dump frames as PPM (`tools/ppm2png.py`) |
| `SH2_WAV=file` | record the audio mix (raw float32 stereo, 44.1 kHz) |
| `SH2_STATS=1` | frame rate and guest heap |
| `SH2_TRACE=1` | log every bridged Windows call |
| `SH2_COVER=frame` | with a tracing build (`TRACE=$PWD/build/disasm/functions.json tools/regen.sh --recomp-only`), log each function the first time it runs after that frame |
| `SH2_ORACLE=va,n` | differential test of one function against Unicorn (`tools/oracle.py`) |

`tools/fuzz.py` checks the lifter's instruction semantics against Unicorn.

## Credits

[xboxrecomp](https://github.com/sp00nznet/xboxrecomp); nfsmw-nx, the Need for Speed: Most Wanted Switch
port, for the Horizon guest-memory technique; devkitPro and libnx, SDL2, FFmpeg, Mesa.

Silent Hill is a trademark of Konami. This project is not affiliated with Konami.
