# DJMAX Fever Recomp — PS2 & 3DS

Experimental static recompilation of DJMAX Fever (PSP, ULUS-10403) to other consoles. One shared recompiler and game-code pipeline, two platform runtimes:

- **PS2** (`port/ps2`): tested in PCSX2. Reaches song selection and gameplay with audio, keysounds, BGA video, results and memory-card save/load.
- **3DS** (`port/3ds`): tested in Azahar (Citra successor). Boots, registers a DJ profile (saved to SD), navigates every menu and reaches 4B gameplay with GPU rendering, keysound/SFX audio and input. New 3DS gets 804 MHz + L2 cache automatically.

This repository contains source code and build tools only. It includes no game executable, generated game code, music, charts, graphics, BIOS, converted movie data, or song-import pipeline. You must provide your own game dump and decrypted EBOOT. Build artifacts embed segments from that EBOOT and are local only — never redistribute them.

## Current state

**PS2** — A full song has been played in PCSX2. Development build: all modes/songs, long-session audio, score persistence, movie sync and equipment combinations still need systematic verification. Network is out of scope. Real PS2 hardware and disc/OPL loading are not validated (file access uses PCSX2 `host:`).

**3DS** — Verified in Azahar 2126.2: boot, intro-movie clean skip, profile registration with real savedata on `sdmc:/djmax/SAVE/`, title → mode select → song select → 4B gameplay at ~30 fps (emulator CPU clock 400%, or New 3DS hardware untested). Audio: PSP SAS core is a real VAG/ADPCM mixer (keysounds and menu SFX audible through ndsp), sceAudio channels are real FIFOs paced to 44.1 kHz. Atrac BGM runs on a silent real-time clock so charts scroll correctly; the AT3/AT3+ PCM decode is pending (`third_party/at3_standalone_src` is the intended route). BGA/intro movies are skipped coherently, not decoded. Old 3DS boots but will not hold 30 fps; the recomp is CPU-bound.

> Old Citra nightlies (e.g. 2104, March 2024) do not execute GPU command lists from homebrew built with current libctru/citro3d — not even devkitPro's `simple_tri` example. Use [Azahar](https://github.com/azahar-emu/azahar) ≥ 2126.

This public build uses only Fever's original catalog. Extra-song overlays are disabled at compile time (`DJMAX_BASE_ONLY`); no import utilities or imported resources are included.

## Build environment

- Windows + WSL Ubuntu; PowerShell, Python 3, CMake and a C compiler on Windows.
- PS2 target: `ps2dev` / ps2sdk in WSL, with audsrv, libmpeg and zlib. Set `PS2DEV` if it is not `$HOME/ps2dev`. FFmpeg on the Windows PATH for local movie conversion. PCSX2 and your own PS2 BIOS.
- 3DS target: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `3ds-dev` group (devkitARM, libctru, citro3d) on Windows. For `.cia`/`.3ds` packaging: [makerom](https://github.com/3DSGuy/Project_CTR/releases) and [bannertool](https://github.com/carstene1ns/3ds-bannertool/releases). Azahar for emulator runs.

The workspace path should not contain spaces for the current shell build flags.

## Shared first steps (both targets)

1. Put the decrypted Fever module at `.game/DUMPED EBOOT/ULUS10403_EBOOT.BIN`.
2. Put the extracted game's `PSP_GAME` directory at `.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME` (or use another path when launching).
3. Build the included recompiler (MIT, with local emitter fixes):

```powershell
cmake -S .tools/psprecomp -B .tools/psprecomp/build
cmake --build .tools/psprecomp/build --config Release --target allegrexrecomp
```

## PS2 build

In WSL, from the repository root:

```bash
BASE_ONLY=1 bash port/ps2/build.sh game
```

This regenerates the game-derived corpus locally (`build/emit`, `build/eboot_reloc.bin`) and writes `build/ps2/out-base/djmax.elf`. For incremental builds use `BASE_ONLY=1 REGEN=0 ...`. Never recursively delete a `PSP_GAME` junction.

Convert the base game's movies locally:

```powershell
python tools-src/prepare_base.py '.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME' build/ps2/out-base
```

Launch with explicit emulator/game paths:

```powershell
./port/ps2/run_pcsx2.ps1 -Elf "$PWD/build/ps2/out-base/djmax.elf" -Pcsx2Dir 'C:/path/to/PCSX2' -GameDir "$PWD/.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME" -Seconds 0
```

Alternatively, place PCSX2 under `.emu/.ps2/pcsx2-v2.8.2-windows-x64-Qt` and run `Jugar-DJMAX.cmd`. Automated captures (`shot_pcsx2.ps1`) use a separate `DJMAX-Automated.ps2` card. The BGA clock regression runs with a host compiler:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iport/ps2 tools-src/test_bga_clock.c -o /tmp/djmax-bga-clock && /tmp/djmax-bga-clock
```

The PCSX2 runner enables host filesystem access and software rendering, restoring the emulator INI when it exits. Use a test memory card. BIOS and emulator settings are not distributed.

## 3DS build

Requires the corpus from the shared steps (`build/emit` + `build/eboot_reloc.bin`; run the PS2 corpus regeneration once, or `port/ps2/make_ps2_corpus.ps1`). Then:

```powershell
powershell -ExecutionPolicy Bypass -File port/3ds/build.ps1 -DevkitPro C:\devkitPro -Jobs 8
```

Outputs `port/3ds/build/djmax3ds.3dsx` (Homebrew Launcher / Azahar). For an installable `.cia` (console with CFW) and a `.3ds` CCI (emulator cart image):

```powershell
powershell -ExecutionPolicy Bypass -File port/3ds/make_cia.ps1 -ToolsDir C:\path\to\ctr-tools
```

`make_cia.ps1` looks for `makerom.exe`/`bannertool.exe` in `-ToolsDir`, next to the script, or on the PATH. The RSF requests New 3DS 804 MHz, L2 cache and the 124 MB app region; the same binary still boots on Old 3DS (slow).

### SD card layout (emulator `sdmc/` folder or the console's SD)

```
sdmc:/djmax/PSP_GAME/...     <- your own dump's PSP_GAME tree
sdmc:/djmax/SAVE/            <- created automatically (profile save)
sdmc:/3ds/dspfirm.cdc        <- console: dump with the DSP1 homebrew for audio.
                                Emulator: a dummy file is created automatically.
sdmc:/autopad.txt            <- optional scripted input ("<ms> <hex PSP buttons>" lines)
```

### Running

- **Azahar**: `azahar.exe path\to\djmax3ds.3dsx` (or the `.3ds`/installed `.cia`). Set *CPU clock* to 400% in emulation settings for full speed; keep New 3DS mode enabled (default).
- **Console (CFW/Luma3DS)**: install `djmax3ds.cia` with FBI, or run the `.3dsx` from the Homebrew Launcher. Dump your DSP firmware once with DSP1 for sound. Real-hardware performance is only acceptable on New 3DS and is still unverified.

Controls: D-pad = PSP d-pad, `A/B/X/Y` = `○/✕/△/□`, `L/R` = `L/R`, `START/SELECT` = `START/SELECT`, circle pad = analog nub.

## Source layout and attribution

- `port/ps2`: PS2 runtime, HLE, scheduler, graphics and audio backends.
- `port/3ds`: 3DS runtime: cooperative scheduler, PSP HLE (threads, IO, SAS/VAG mixer, sceAudio→ndsp, savedata, sceMpeg skip), GE→citro3d renderer with texture cache, CIA packaging.
- `port/tools` and `tools-src`: local regeneration, relocation, source fixes and base movie conversion.
- `.tools/psprecomp`: source snapshot from [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp), revision `5ca18fac1691a4a7b40ae683dabdf72a5703d2b8`, MIT. Local modifications included; original license retained.
- `third_party/at3_standalone_src`: FFmpeg ATRAC decoders extracted by PPSSPP, LGPL 2.1 or later. Attribution, headers, license and local changes retained in that directory.

Project-authored code is MIT; third-party files retain their own licenses. DJMAX and its game content belong to their respective owners. This is an independent port.
