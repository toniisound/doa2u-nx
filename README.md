# DOA2U NX

Nintendo Switch port of *Dead or Alive 2 Ultimate* (Xbox, 2004), built on
[NoRain211/doa2u-re](https://github.com/NoRain211/doa2u-re): a native static
recompilation of the original Xbox executable with a hand-written runtime.
This repository keeps its full history and code; everything Switch-specific lives in
[`switch/`](switch), and the upstream README is kept as
[`README.doa2u-re.md`](README.doa2u-re.md).

Sister project: [DOA3 NX](https://github.com/toniisound/doa3-nx), whose Switch
host layer this port started from.

> [!IMPORTANT]
> This repository contains **no game files and no prebuilt `.nro`**. The
> `.nro` carries the game's own executable, translated to C and compiled, so
> each user builds it from their own legally obtained copy of *Dead or Alive
> Ultimate* (Xbox, **USA**), **Disc 2**. The build refuses any executable
> other than the one upstream validated.

## Status

**Stage 1 of 3: CPU bring-up.** The whole game runs as a libnx homebrew with
audio, input and saves, but draws nothing yet: the presenter is a probe that
counts frames and shows on screen how fast the lifted game runs on the
console's CPU (target: 60 fps). This build has not been run on hardware or in
an emulator yet.

| Stage | Goal | State |
|---|---|---|
| 1. CPU bring-up | Boot, audio, input, saves, frame-rate probe | **Code complete, untested** |
| 2. Renderer | OpenGL 4.3 backend on its own core; upstream's runtime-generated HLSL shaders compiled to GLSL with glslang + SPIRV-Cross, cached on the SD card | Not started |
| 3. Performance | Real hardware tuning | Not started |

| Area | Switch host layer | State |
|---|---|---|
| Lifted program + runtime | shared with doa2u-re (`recomp-runtime/`) | builds for AArch64 |
| Fibers | `fiber_switch_arm64.S` (shared) | — |
| Video | `switch/src/d3d_backend_probe.cpp` under upstream's `d3d_presenter_inline.cpp` | probe only (no picture) |
| Audio | `switch/src/audio_output_switch.cpp` (audout, mixer on core 2, dry/wet routing, I3DL2 reverb) | on |
| Input | `switch/src/input_host_switch.c` (libnx pads, rumble) | 4 ports |
| Files and saves | `win32_compat.c` + `switch/include` shims | SD card |

### What differs from DOA3 NX

- The presenter backend plugs into upstream's `d3d_backend_*` seam, so
  upstream's presenter driver is used unchanged.
- DOA2U generates its draw shaders as HLSL at run time (one per vertex format
  and vertex program, with D3D8 fixed-function lighting emulated inside), so
  stage 2 needs a shader compiler on the console rather than hand-written GLSL.
- Audio voices carry XAudio2-style dry and wet output matrices; the wet sends
  feed the same portable I3DL2 reverb and crosstalk canceller as upstream's
  SDL backend.
- Visibility tests (occlusion queries) exist; the probe answers them as
  visible.

## Building

Requirements: Linux or WSL with devkitPro (`switch-dev`), CMake 3.20+,
Ninja (optional), Python 3.12+ and everything
[`docs/building.md`](docs/building.md) lists for the Linux build.

Clone with submodules (the lifter is one):

```bash
git clone --recurse-submodules https://github.com/toniisound/doa2u-nx.git
cd doa2u-nx
```

1. Lift the game from your Disc 2 ISO. This extracts the disc to
   `private/imported-disc`, translates the executable to C and checks it
   against the program upstream validated; `--lift-only` skips the desktop
   runner, which the Switch build does not need:

   ```bash
   python3 -m venv private/venv
   private/venv/bin/pip install capstone==5.0.7
   ./build_game.sh --lift-only /path/to/disc2.iso
   ```

2. Build the `.nro` from that program:

   ```bash
   ./switch/build_switch.sh            # JOBS=3 for more parallel jobs
   ```

   Each generated chunk needs about 3 GB of RAM to compile. A 256x256 JPEG in
   `private/icon.jpg` becomes the homebrew menu icon (optional, not included).

## Installing

```
sdmc:/switch/doa2u-nx/doa2u-nx.nro
sdmc:/switch/doa2u-nx/disc/        <- the whole extracted Disc 2 (private/imported-disc/disc)
```

Start it from the homebrew menu, preferably through a game (title takeover)
for the full memory budget. The first launch copies the three disc archives
the game installs to its hard disk (about 700 MB) into
`disc/.recomp-storage/partition5`, as upstream's launcher does; later launches
skip it. Saves go to `disc/.recomp-storage`, every character and costume
starts unlocked, and each boot rewrites `sdmc:/switch/doa2u-nx/doa2u-nx.log`.

Buttons held at launch:

| Button | Effect |
|---|---|
| R | Run unpaced (as fast as the CPU allows) |
| L | Original 4:3 picture instead of the game's own 16:9 mode |
| Y | Skip the intro movies |

The CPU is not boosted by the app, since boost mode slows the GPU; use sys-clk
to raise the CPU clock.

Controls: Switch B/A/Y/X are Xbox A/B/X/Y by position; L/R are Black/White,
ZL/ZR the triggers, + is START and - is BACK. Quit with HOME or
ZL+ZR+(-)+(+).

## Credits

- [NoRain211](https://github.com/NoRain211): [doa2u-re](https://github.com/NoRain211/doa2u-re),
  its runtime and the [xboxrecomp](https://github.com/NoRain211/xboxrecomp) lifter fork.
- [devkitPro](https://devkitpro.org) and [switchbrew](https://github.com/switchbrew/libnx):
  devkitA64 and libnx.
- Switch port: Toniisound.

## License

GPL-3.0-or-later, as doa2u-re. Dead or Alive 2 Ultimate and its assets belong
to Koei Tecmo; nothing from the game is distributed here.

## Donations

This emulator is completely free and open-source. Optional donations to support the development are always welcome:

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20me-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/toniisound)
[![PayPal](https://img.shields.io/badge/PayPal-Donate-00457C?style=for-the-badge&logo=paypal&logoColor=white)](https://paypal.me/toniisound)
