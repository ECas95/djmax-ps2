# DJMAX Fever — PS2 port

Experimental static recompilation of DJMAX Fever (PSP, ULUS-10403) for PlayStation 2, currently tested in PCSX2.

This repository contains source code and build tools only. It includes no game executable, generated game code, music, charts, graphics, BIOS, converted movie data, or song-import pipeline. You must provide your own game dump and decrypted EBOOT. The final ELF embeds segments from that EBOOT and is a local build artifact.

## Current state

The port reaches song selection and gameplay with audio, keysounds, BGA, controller input, results and memory-card save/load. A full song has been played in PCSX2. This is a development build, not a completed release: all modes/songs, long-session audio, score persistence, movie synchronization and equipment combinations still need systematic verification. Network is outside this project's scope. Hardware PS2 and disc/OPL loading are not validated; file access currently uses PCSX2's `host:`.

This public build uses only Fever's original catalog. Extra-song overlays are disabled at compile time (`DJMAX_BASE_ONLY`), and no import utilities or imported resources are included. Development builds currently unlock all content after load/before save.

## Build environment

- Windows + WSL Ubuntu; PowerShell, Python 3, CMake and a C compiler on Windows.
- `ps2dev` / ps2sdk in WSL, with audsrv, libmpeg and zlib. Set `PS2DEV` if it is not `$HOME/ps2dev`.
- FFmpeg on the Windows PATH for the local movie conversion.
- PCSX2 and your own PS2 BIOS, installed separately.

The workspace path should not contain spaces for the current shell build flags.

1. Put the decrypted Fever module at `.game/DUMPED EBOOT/ULUS10403_EBOOT.BIN`.
2. Put the extracted game's `PSP_GAME` directory at `.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME` (or use another path when launching).
3. Build the included recompiler, which is MIT-licensed and retains local emitter fixes:

```powershell
cmake -S .tools/psprecomp -B .tools/psprecomp/build
cmake --build .tools/psprecomp/build --config Release --target allegrexrecomp
```

4. In WSL, from the repository root:

```bash
BASE_ONLY=1 bash port/ps2/build.sh game
```

This regenerates the game-derived corpus locally and writes `build/ps2/out-base/djmax.elf`. For incremental builds use `BASE_ONLY=1 REGEN=0 bash port/ps2/build.sh game`. After changing headers, remove only the relevant object files or regenerate the object directory. Never recursively delete a `PSP_GAME` junction.

5. In PowerShell, convert the base game's movies locally:

```powershell
python tools-src/prepare_base.py '.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME' build/ps2/out-base
```

6. Launch with explicit emulator/game paths:

```powershell
./port/ps2/run_pcsx2.ps1 -Elf "$PWD/build/ps2/out-base/djmax.elf" -Pcsx2Dir 'C:/path/to/PCSX2' -GameDir "$PWD/.game/DJMAX_Fever_(Patched)_[A912C17D]/PSP_GAME" -Seconds 0
```

Alternatively, place PCSX2 under `.emu/.ps2/pcsx2-v2.8.2-windows-x64-Qt` and run `Jugar-DJMAX.cmd`.

Automated captures (`shot_pcsx2.ps1`) use a separate `DJMAX-Automated.ps2` card, initially copied from slot 1. The normal launcher keeps your selected card.

The runner enables host filesystem access and software rendering, restoring the emulator INI when it exits. It creates a local PSP_GAME junction next to the ELF. Use a test memory card: the runtime formats a previously unformatted PS2 card. The BIOS and emulator settings are not distributed.

## Source layout and attribution

- `port/ps2`: PS2 runtime, HLE, scheduler, graphics and audio backends.
- `port/tools` and `tools-src`: local regeneration, relocation, source fixes and base movie conversion.
- `.tools/psprecomp`: source snapshot from [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp), revision `5ca18fac1691a4a7b40ae683dabdf72a5703d2b8`, MIT. Local modifications are included; primarily the PSP container/emitter support needed for this module. Original license retained.
- `third_party/at3_standalone_src`: FFmpeg ATRAC decoders extracted by PPSSPP, LGPL 2.1 or later. Attribution, original file headers, license and local changes are retained in that directory.

Project-authored code is MIT; third-party files retain their own licenses. DJMAX and its game content belong to their respective owners. This is an independent port.
