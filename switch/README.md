# DOA2U NX — Switch host layer

The Nintendo Switch port's own sources: entry point, presenter backend, audio,
input and the POSIX shims newlib lacks. The runtime itself is shared with
upstream in [`recomp-runtime/`](../recomp-runtime). Building, installing and
status are in the [repository README](../README.md).

| File | Role |
|---|---|
| `src/nx_main.cpp` | Entry point: console, log file, picks the game XBE by hash, seeds the hard-disk cache, starts the runner |
| `src/d3d_backend_gl.cpp` | OpenGL 4.3 presenter backend (a port of upstream's SDL_GPU backend); also builds on desktop Linux for host tests |
| `src/d3d_presenter_thread_nx.cpp` | Upstream's threaded presenter driver without Win32; renders on core 1 |
| `src/d3d_backend_probe.cpp` | Stage 1 presenter backend (`PRESENTER=probe`): draws nothing, measures CPU speed |
| `src/audio_output_switch.cpp` | audout mixer on core 2, per-voice dry/wet routing, I3DL2 reverb |
| `src/input_host_switch.c` | libnx pads and rumble, 4 ports |
| `src/boot_trace.cpp`, `src/profile.cpp`, `src/crash_report.cpp` | Boot log, per-second profile, crash screen |
| `src/switch_shims.c`, `include/sys/*` | POSIX calls and headers devkitA64's newlib lacks |
