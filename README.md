<div align=center>

<img src="docs/banner.png" alt="Silent Hill 2" width="70%">

</div>
<h1 align=center>Silent Hill 2 — Nintendo Switch port</h1>

<p align=center><a href="https://discord.gg/H9yb3hSkvf"><img src="https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white" alt="Discord"></a></p>

<p align=center><b>v1.03</b></p>

A native Nintendo Switch port of the PC version of **Silent Hill 2** (Director's Cut / Restless Dreams), with
support for the packs of **[Silent Hill 2: Enhanced Edition](https://enhanced.townofsilenthill.com/SH2/)**.

It's not an emulator. The game's x86 executable is statically recompiled to C ahead of time and the C is compiled
for the Switch's ARM64 CPU. The Windows layer the game expects (Direct3D 8, DirectSound, DirectInput, Bink,
kernel32...) is reimplemented on top of SDL2, OpenGL and FFmpeg.

> [!NOTE]
> No game files are included. You need your own copy of the PC game with the Enhanced Edition installed; the NRO
> from [Releases](https://github.com/cutarev/sh2-nx/releases), or one you build yourself
> ([Building the NRO](#building-the-nro)), runs it.

## Features

- **Native code**: every function of `sh2pc.exe` runs as ARM64 code; no emulator, no Wine.
- **Launcher**: Play and Options before the game; Options changes the main `sh2e.ini` settings on the console.
- **Widescreen**: 16:9, drawn at 720p handheld and 1080p docked, or the original 4:3 (ThirteenAG's
  WidescreenFix, ported).
- **Enhanced Edition packs**: HD textures and maps, the EE title and save screens, the restored music, voices and sound
  effects, EE menus, EE's high resolution font (`sh2e/font`, which also has the Japanese text), and the FMV pack's 1080p60
  movies with their subtitles (`movie=1`). Translations made for the Enhanced Edition load from a `lang` folder.
- **Enhanced Edition fixes**: search camera on the right stick, movement on the d-pad, pause menu fix, vibration
  fixes.
- **Controller**: SH2EE's button layout with Nintendo's A/B, HD rumble following the in-game Vibration option.
- **Saving and loading**, movies with synced audio, every menu.
- **Shader cache**: shaders are compiled once and kept on the SD card, so areas only stutter the first time.
- Also builds for Linux (x86-64), which is where most of the debugging happens.

## How to install

1. On your PC, install Silent Hill 2 and run the [Enhanced Edition setup](https://enhanced.townofsilenthill.com/SH2/)
   with at least the **Enhanced Executable**, **Essential Files**, **Image Enhancement Pack** and **Audio Enhancement
   Pack**. The port is built for the `sh2pc.exe` it installs: 5,685,248 bytes, SHA-1
   `3201a5e1029f3ae3b77770255dcbe10360b3cd09`.
2. Get `sh2-nx.nro` from [Releases](https://github.com/cutarev/sh2-nx/releases), or build it from that folder
   ([Building the NRO](#building-the-nro)).
3. Copy everything to the SD card. With the card (or its folder) mounted on the PC:

   ```sh
   .venv/bin/python tools/make_sd.py /path/to/sd --lang en      # en, fr, de, it, es or jp
   ```

   or by hand. Your `sh2-nx` folder should look like this:

   ```text
   /switch/sh2-nx/
     sh2-nx.nro
     sh2pc.exe        (from your PC install)
     data/            (from your PC install, about 2.4 GB)
     sh2e/            (from your PC install, about 6 GB with sh2e/movie)
     sh2e.ini         (which Enhanced Edition packs to use: docs/sh2e.ini.example)
     language.ini     (optional: SET DX_CONFIG_LANGUAGE 1 is English)
     lang/            (optional: a translation for the Enhanced Edition, e.g. Russian)
     keyconf.dat      (optional: your PC's button mapping)
   ```

   `sh2e/pic/etc` and `sh2e/menu/mc` are needed even with every pack switched off: the title and save screens are laid
   out for the Enhanced Edition's art.

4. Launch it from the homebrew menu in **title takeover** mode (hold **R** while starting an installed game) or from a
   forwarder set to a **39-bit address space**. Not from the album: applet mode does not give homebrew enough memory.

Settings, saves (`data/save`) and the shader cache are written next to the NRO.

> [!CAUTION]
> The first time you enter an area its shaders are compiled, so **it can stutter for a moment**. They are kept in
> `sh2-progs.bin` and later visits are smooth.

## Controls

<div align="center">

| Switch | Xbox (PC) | Action |
| --- | --- | --- |
| A | A | Action, confirm |
| B | B | Cancel, flashlight |
| Y | X | Run / guard |
| X | Y | Map |
| L / R | LB / RB | Cycle target |
| ZL | LT | Search mode (ready the weapon) |
| ZR | RT | Aim lock |
| + | Start | Inventory |
| − | Back | Skip / pause |
| Left stick, d-pad | Left stick, d-pad | Move |
| Right stick | Right stick | Search camera |

</div>

A confirms and B cancels, as in other Switch games, so those two sit swapped compared with an Xbox pad; every other
button is in the same place as on an Xbox pad with the Enhanced Edition. Remap them in Options > Control Options; a
`keyconf.dat` from a PC install maps the same buttons.

## Settings

`sh2e.ini`, next to the NRO ([example](docs/sh2e.ini.example)); the launcher's Options screen sets `resolution`,
`handheld`, `docked`, `movie`, `PreserveSoundsOnLoad` and `cores`:

| Setting | Effect |
| --- | --- |
| `pic=1`, `sound=1`, `bg=1`, ... | read that folder of `data/` from `sh2e/` (the Enhanced Edition pack) |
| `movie=1` | the FMV pack (`sh2e/movie`): its 1080p60 videos play at speed with their subtitles; the Switch drops frames decoding them. `movie=0` plays the original videos |
| `resolution=1920x1080` | the default; `1280x720` for a lighter game, `960x720` for the original 4:3 |
| `handheld=720`, `docked=0` | the defaults (720p handheld, 1080p docked): how many lines the screen-sized render targets are drawn with in each mode, then scaled to the screen (the game is still laid out for `resolution`); `0` draws them at the game's resolution. Docking or undocking switches at the next frame |
| `FlashlightToggleSFX=0` | no click when the flashlight is switched (on by default when `sh2e/sound/extra` has the sounds) |
| `PreserveSoundsOnLoad=0` | the game's own load and save sounds, which loading cuts off (by default the Enhanced Edition's play through) |
| `cores=1` | every game thread on one core, a fallback if the music ever loops again |
| `log=1` | write `sh2.log` next to the NRO, for bug reports (off by default: it costs an SD card write per line). Crashes always go to `sh2-crash.log` |

### Movies

The movie loop no longer waits for the 30 Hz limiter (EE `FixFMVSpeed`), subtitles follow the video's frame rate
(EE `FmvSubtitlesSyncFix`), and a video the Switch cannot decode in time drops frames to keep up with its sound.
With that the FMV pack's 60 fps videos play at speed (`movie=1`). The game itself runs at its original 30 fps.

## Building the NRO

You need the game (step 1 of [How to install](#how-to-install)): the code generator reads `sh2pc.exe`, applies the
Enhanced Edition patches to it and recompiles it.

Requirements: Python 3, CMake, Ninja, and for the Switch build podman (or a local devkitPro install with devkitA64,
libnx, switch-sdl2, switch-mesa, switch-ffmpeg and switch-pkg-config). For the Linux build, SDL2 and FFmpeg
development files (Fedora: `dnf install SDL2-devel ffmpeg-free-devel`).

```sh
git clone https://github.com/cutarev/sh2-nx && cd sh2-nx
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
ln -s "/path/to/Silent Hill 2" game     # the folder holding sh2pc.exe, data/ and sh2e/
tools/regen.sh                          # patches and recompiles game/sh2pc.exe into src/recomp/gen (~4 min)

tools/build-switch.sh                   # -> build/switch/sh2-nx.nro (devkitPro's container under podman)
tools/build-switch-local.sh             # the same with a local devkitPro install

cmake -S . -B build/linux -G Ninja && cmake --build build/linux    # Linux build
build/linux/sh2 game
```

## How it works

- **The game's code is translated, not emulated.** `tools/regen.sh` wraps the PE as an XBE for
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (patched for this port: flag joins, jump tables, x87/MMX
  corner cases, setjmp/longjmp), which turns every function into C.
- **Enhanced Edition patches.** EE patches the running game from its `d3d8.dll`; here its code byte patches are
  applied to the exe before recompiling (`tools/patch_exe.py`, each site checked against the original bytes) and its
  hooks become hand-written functions in `src/game/ee.c`. Details in [docs/ENHANCED.md](docs/ENHANCED.md).
- **The Windows layer.** Direct3D 8 runs on OpenGL 4.3 core with the shaders translated to GLSL (`src/d3d8`),
  DirectSound is a software mixer on SDL audio, DirectInput goes through SDL, Bink videos through FFmpeg, and
  kernel32, user32 and msvcr70 are in `src/win32`. `src/runtime` holds the guest memory, threads and the
  Linux/Horizon layer.

## Debugging aids (Linux)

| Variable | Effect |
| --- | --- |
| `SH2_KEYS="12:Return,30:W+5"` | scripted key presses (seconds, key, optional hold); the window is hidden and the real keyboard ignored |
| `SH2_SHOW=1` | keep the window visible during a scripted run |
| `SH2_SHOT=dir`, `SH2_SHOT_EVERY=n` | dump frames as PPM (`tools/ppm2png.py`) |
| `SH2_WAV=file` | record the audio mix (raw float32 stereo, 44.1 kHz) |
| `SH2_STATS=1` | frame rate and guest heap |
| `SH2_TRACE=1` | log every bridged Windows call (`trace=1` with `log=1` in `sh2e.ini` on the Switch) |
| `SH2_COVER=frame` | with a tracing build (`TRACE=$PWD/build/disasm/functions.json tools/regen.sh --recomp-only`), log each function the first time it runs after that frame |
| `SH2_ORACLE=va,n` | differential test of one function against Unicorn (`tools/oracle.py`) |

`tools/fuzz.py` checks the lifter's instruction semantics against Unicorn.

## Credits

- **Konami and Team Silent**: creators of Silent Hill 2. This is an unofficial, fan-made port with no affiliation.
- **[r4lix](https://github.com/r4lix/sh2-nx)** and Le Z: the Enhanced Edition support, widescreen, HD rumble, the
  shader cache and the lifter, save and front buffer fixes in v0.55.
- **[Ligux01](https://github.com/Ligux01)**: the Eddie room crash, handheld render target scaling and controller POV fixes
  in v1.03.
- **[Silent Hill 2 Enhancements](https://github.com/elishacloud/Silent-Hill-2-Enhancements)** by Elisha Riedlinger and
  contributors (zlib): the patches ported in `src/game/ee.c` and `tools/patch_exe.py`.
- **[WidescreenFixesPack](https://github.com/ThirteenAG/WidescreenFixesPack)** by ThirteenAG (MIT): the widescreen fix.
- **[xboxrecomp](https://github.com/sp00nznet/xboxrecomp)** by sp00nznet: the x86 to C lifter.
- **nfsmw-nx**, the Need for Speed: Most Wanted Switch port, for the Horizon guest-memory technique.
- **[devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx)**, SDL2, FFmpeg, Mesa.

## Legal

Silent Hill is a trademark of Konami. This project is not affiliated with Konami. This repository contains no assets
or program code from the original game; users must provide their own copy. Running homebrew requires custom firmware.
