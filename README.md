<p align="center"><img src="src/rt/switch/icon/cover-1024.png" width="320" alt="Fan-made cover art: the title over the Earth under an energy shield"></p>

# Ben 10: Protector of Earth for Nintendo Switch

An unofficial, non-commercial fan port of the PSP game *Ben 10: Protector of Earth* (ULUS10307) to
Nintendo Switch homebrew on Atmosphère. The game's MIPS code is translated to C ahead of time and runs
natively on the console, with a renderer written for its GPU.

**This repository contains no game code or assets.** You need your own copy of the game. The C translated
from it and the `.nro` built from it contain code from the game, so they are not part of this source tree.

The game, its name and its characters belong to their owners. This project is not affiliated with them
or with Nintendo.

## Status

- Handheld: about 30 fps with internal rendering at twice the PSP's resolution.
  Heavy scenes can dip for a moment.
- Saves, closing and reopening the game, the HOME button, and powering the console off and on all work.
- Docked 1080p output is written but not yet verified on a console.
- Only part of the game has been played on this build.
- The build steps below have not yet been followed from scratch on another machine.

## What you need

- A Switch running Atmosphère. Launch in title mode: hold R while starting an installed game. The
  album's applet mode has too little memory and no CPU boost.
- Your own disc image of the game (ULUS10307).
- On a PC: [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev` and `deko3d`,
  and the Windows setup in [`docs/SETUP.md`](docs/SETUP.md) to build the desktop player.

## Building

1. Build the desktop player, add your disc image to its library, and let it build the game's package.
   The package holds `experimental-ulus10307_image.bin` and the headers the port uses.

2. Translate the game again with register promotion, into a new private folder. The package's
   `codegen_profile.json` lists the `--extra-elf` arguments in `EXTRA_ELF_ARGS`.

   ```bash
   python tools/codegen.py <selected.elf> <out>/experimental-ulus10307_recomp.c --base=0x08804000 \
     --funcs-per-chunk=2000 --target-chunk-bytes=4000000 --promote-registers <EXTRA_ELF_ARGS>
   ```

   Then copy the package's `*.h` files that are not already in `<out>`.

3. Build the port, from WSL or Linux:

   ```bash
   export DEVKITPRO=/opt/devkitpro
   make -f Makefile.switch -j6 PACKAGE_DIR=<out>
   ```

   `Makefile.switch` has no header dependencies: after changing a header or a flag, remove
   `build_switch/src` and `build_switch/gen` first.

## Installing

Put these files in `sdmc:/switch/ben10-psp/`:

| File | Comes from |
| --- | --- |
| `ben10-psp.nro` | the build above |
| `game.iso` | your own disc image, renamed |
| `experimental-ulus10307_image.bin` | the package |
| `assets/`, `font/` | the package |

Saves go to `memstick/` in that folder, and the log to `log.txt`.

## Controls

The face buttons match the PSP by position. L and ZL are the PSP's L, R and ZR its R, + is START and
− is SELECT. Press + and − together to quit.

## Switches

An empty file with one of these names in the game's folder changes how it runs:

| File | Effect |
| --- | --- |
| `scale1.txt`, `scale3.txt`, `scale4.txt` | Internal render scale instead of 2. The scale also lowers itself if the GPU falls behind. |
| `debug.txt` | Frame, GPU and save measurements in `log.txt` |
| `profile.txt` | Samples where the CPU goes into `profile.bin` |
| `noboost.txt` | Normal clocks instead of the CPU boost |
| `nogethread.txt` | The GE on the game's core |
| `notnl.txt` | Vertex processing on the CPU |
| `software.txt` | The CPU renderer |

## What this adds

This work is based on [Nakagawa Recomp](https://github.com/Jstar269/nakagawa-recomp) at commit
`70b1d3f8`, under GPL-3.0-or-later, and stays under that licence. Its notices and credits are kept:
see [`NOTICE.md`](NOTICE.md) and [`CREDITS.md`](CREDITS.md). As the GPL requires, these are the changes,
made in November 2026.

**New:** `Makefile.switch`, this file, and `src/rt/switch/`: entry point, SDL3 subset over libnx, pad and
presenter, audio over audout, POSIX calls newlib lacks, deko3d renderer and shaders, a worker thread for
the GE, a sampling profiler, and the cover art in `src/rt/switch/icon/`.

**Modified:**

| File | Change |
| --- | --- |
| `README.md` | This page; the original is now [`README.nakagawa.md`](README.nakagawa.md) |
| `src/rt/hle_kernel_sync.c` | First-come semaphore hand-off, which fixes a starved audio thread at a loading screen |
| `src/rt/fp_convert.h` | On the Switch, guest float operations in round-to-nearest skip the FP environment switch |
| `src/rt/recomp.h` | On the Switch, a cheaper time-slice count and no store-watch check |
| `src/rt/sched.c` | A longer time slice on the Switch |
| `src/rt/sr_coro.c` | Main stack bounds on the Switch |
| `src/rt/ge.c`, `src/rt/hle.c`, `src/rt/hle_ge.c`, `src/rt/hle_internal.h` | Display lists run on the GE worker thread |

The build links [libnx](https://github.com/switchbrew/libnx) and
[deko3d](https://github.com/devkitPro/deko3d), which come with devkitPro and are not in this repository.

The cover art is original and uses no art from the game. The name and the symbol it reinterprets belong
to their owners.
