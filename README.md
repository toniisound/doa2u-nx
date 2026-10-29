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

**Stage 2 of 3: renderer.** The game runs as a libnx homebrew with an
OpenGL 4.3 renderer on its own core, audio, input and saves. Stage 1 (the
CPU probe) ran the intro, menus and the attract-mode fights in Eden at
60 fps in menus and 42–58 fps in fights, without a picture. The renderer has
been verified on Mesa's llvmpipe on a PC through the same presenter API the
game uses, but has not yet run on the console or in Eden.

| Stage | Goal | State |
|---|---|---|
| 1. CPU bring-up | Boot, audio, input, saves, frame-rate probe | Done (Eden) |
| 2. Renderer | OpenGL 4.3 backend on core 1; upstream's run-time HLSL shaders compiled to GLSL with glslang + SPIRV-Cross, cached on the SD card | **Code complete, host-tested** |
| 3. Performance | Real hardware tuning (3D audio off the guest core, Sofdec movies) | Not started |

| Area | Switch host layer | State |
|---|---|---|
| Lifted program + runtime | shared with doa2u-re (`recomp-runtime/`) | builds for AArch64 |
| Fibers | `fiber_switch_arm64.S` (shared) | — |
| Video | `switch/src/d3d_backend_gl.cpp` behind `switch/src/d3d_presenter_thread_nx.cpp` (render thread on core 1) | OpenGL 4.3 |
| Shaders | `recomp-runtime/d3d_shader_compiler.cpp` (GLSL target), glslang 16.2.0 + SPIRV-Cross 1.4.341 built from source | cached in `shader-cache/` |
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
- Visibility tests (occlusion queries) exist; the renderer answers them with
  `GL_SAMPLES_PASSED` queries.
- The renderer follows upstream's SDL_GPU backend decision for decision. Two
  GL details were measured on llvmpipe rather than assumed: with
  `glClipControl(UPPER_LEFT)` GL's winding matches D3D's screen winding, and
  GL breaks pixel-centre ties on rows the other way from D3D, which a
  1/256-pixel viewport lift corrects.

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

   The first configure downloads glslang and SPIRV-Cross (pinned commits)
   from GitHub. `PRESENTER=probe ./switch/build_switch.sh` builds the stage 1
   CPU probe instead of the renderer.

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
Compiled shaders are kept in `sdmc:/switch/doa2u-nx/shader-cache`, so the
first boot (and the first sight of each new effect) compiles a little; later
boots only link.

Options: for 2 seconds after launch the screen lists them; press a button to
turn its option on (A starts at once).

| Button | Effect |
|---|---|
| R | Run unpaced (as fast as the CPU allows) |
| L | Original 4:3 picture instead of the game's own 16:9 mode |
| Y | Skip the intro movies |
| X | Render at 480p instead of 720p (lighter GPU load) |

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
