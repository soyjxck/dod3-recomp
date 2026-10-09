# Debugging and measuring

Everything here is an environment variable (or a `dod3.ini` key, same
names) and a line in the log. The game writes `dod3.log` beside the
executable when it has no console; the previous run is `dod3.prev.log`.

## Hands-off runs

- `tools/live_bench.sh <tag> <seconds> [VAR=val ...]`: a run of the chapter 1
  battle with nobody at the controls (`tools/autoplay.sh` starts a new game,
  skips the cutscenes until the battle HUD is on screen, then fights), and
  a summary of its frame times (`tools/bench_summary.py`). Frame rates from
  a run a person played in are not comparable. Run-to-run variance is 5-15%,
  so compare pairs, or use `DOD3_AB`.
- `DOD3_AB=<switch>[,<seconds>]`: an A/B test inside one run. It flips one
  run-time switch every few seconds, counts the frames in each window and
  reports the paired difference with its standard error, and (Windows) each
  thread's CPU time per frame in either state. Switches: `stores`, `icall`,
  `labelwake`, `querysync`, `jcpar`, `bufpool`, `kickbusy`, `msdsp`,
  `texwatch`, `patchhook`, `lzfmemo`, `vcwatch`, `snapincr`, `aniso`,
  `none` (the control, which must report no difference). `DOD3_AB_FROM=<s>`
  skips the boot and menus (default 70).
- `tools/replay_regress.sh`: the rendering regression. Replays RSX captures
  through the engine headlessly and compares the frames with a reference set
  (`tools/compare_frames.py`). The captures and references are made from the
  game and are not in the repository: record captures with `RSX_CAPTURE`
  (`tools/capture_run.bat` + `tools/capture_now.bat` capture 30 frames at a
  problem the player reaches), and keep a known-good replay as the
  reference. Each renderer wants its own reference: Metal and Direct3D 12
  frames differ by a few levels of rounding.

## Frames

- `PS3RECOMP_FRAME_GRAB=<prefix>`: creating `<prefix>.req` makes the game
  write the next presented frame to `<prefix>.ppm` (what autoplay's HUD check
  and the installer's test hook use). `PS3RECOMP_FRAME_DUMP=<path>` with
  `_EVERY` and `_SEQ` dumps frames on a schedule; `RSX_DUMP_FULL=1` at the
  internal resolution. `tools/ppm2png.py` converts a frame.
- `RSX_FRAMETIME=0` turns off the `[frametime]` line every 5 s (fps, mean and
  worst frame, frames over 34 ms).
- `RSX_HITCH_LOG=<ms>` lists frames over that, with the walker's GPU-blocked
  time; with `DOD3_PROF` also the sampled stacks of the frame;
  `DOD3_TRACE_HITCH=1` adds the events of the frame before it (guest sleeps,
  drains, label writes, flips, ring recycles) as `[trace]` lines.
- `DOD3_FRAME_CPU=<ms>` (Windows): for each frame longer than that, the
  threads that used the CPU during it, from thread cycle counters.
- `DOD3_STUTTER_MS=<n>`: per slow frame, `[stutter-rsx]` (textures, vertex
  conversions, pipelines built), `[stutter]` (guest threads) and, on macOS,
  `[stutter-cpu]`.
- `RSX_GPU_TIME=1` (D3D12): GPU time per submit and the draw/barrier/PSO
  counts; `=2` the costliest passes. `RSX_DRAW_STATS=1`, `RSX_SNAP_STATS=1`
  (snapshots), `RSX_SYNC_STATS=1` (the walker's GPU waits), `RSX_PIPE_LOG=1`
  (pipelines built), `RSX_SURF_DUMP_DIR=<dir>` (every surface).
- `D3D12_DBG=1` the Direct3D debug layer; `RSX_VK_VALIDATION=1` the Vulkan
  validation layer.

## Profiling

- `DOD3_PROF=<ms>` (Windows, `src/os/win32/prof.cpp`): an in-process sampling
  profiler. Every `DOD3_PROF_REPORT` seconds (5), each busy thread's CPU
  share, its hottest functions and where it waits; lifted PPU code is named
  through the function table (`ppu:<address>`), the rest through the PDB.
  `DOD3_PROF_TREE=<part of a thread name>` adds inclusive time over a deep
  stack and the call chains behind the waits for the threads it matches
  (`tid` matches the unnamed ones: the game's main thread and the RSX
  walker); `DOD3_PROF_CALLERS=<function>` who calls a function.
- `DOD3_STALL_MS=<n>`: when no frame has been presented for n ms, every
  thread's stack, at most `DOD3_STALL_MAX` times (12), into
  `DOD3_STALL_SAMPLE=<dir>` or the log. On macOS the same switch runs
  `sample`; `tools/stack_summary.py` condenses its output.
- `PPU_WAITPROF=1`: time in syscalls by thread and guest call site, every
  5 s, with the fence poll's statistics.
- `DOD3_HOT_LOG=1`: calls per second and cycles per call of the hooked hot
  functions (the collision test).
- `DOD3_GC_LOG=1` times every garbage collection over 1 ms (`=2` adds the
  sampled stacks); `DOD3_GC_STATS=1` the cycles in the two per-object virtual
  calls.
- `SPU_TASK_STATS=1`: per SPU task image, tasks a second and their pickup,
  load, turnaround and run times. `FS_READ_TIME=1`: every file read's time.
- A crash prints the exception and a symbolised stack (`[crash]` lines);
  `DOD3_NO_CRASH_HANDLER=1` leaves it to the system.

## Audio

`AUDIO_GAPS=1` counts audio blocks the game wrote late; `AUDIO_RATE=1` the
gaps per 5 s window; `AUDIO_WRITEPOS=1` histograms where the game writes
relative to the read index.

## The native paths and their checks

| Switch | |
|---|---|
| `DOD3_GC_NATIVE=0 / 1 / check` | the lifted reachability pass, the native one (default), or both compared |
| `DOD3_GC_PAR=<n>` | worker threads for the collector's mark phase (8; 0 serial); `DOD3_GC_DEFER=0` collects where the game asks instead of in the limiter's sleep; `DOD3_GC_PREFETCH=0` |
| `DOD3_SPU_NATIVE=0` | the SPU hooks off; `DOD3_SPU_NATIVE_CHECK=1` runs each hooked stretch twice and compares every register and the local store |
| `DOD3_SPU_PATCH_HOOK=0`, `DOD3_LZF_MEMO=0` (`DOD3_LZF_MEMO_LOG=1`), `DOD3_MSDSP_NATIVE=0` | one path at a time |
| `DOD3_MP3_NATIVE=0` | the console's MP3 decoder instead of ours (needs its `flashMP3.pic` in `fw/dev_flash/sys/external`); `DOD3_MP3_CHECK=1` runs both |
| `DOD3_ZLIB_NATIVE=0` | the SPU zlib task instead of the native inflate |
| `DOD3_MENU_PATCH=0` | the game's own Settings menu, unpatched |

## Runtime fallbacks

Each turns a change back off, to bisect a regression: `PPU_SLOW_STORES=1`
(the old store path), `PPU_ICALL_SLOW=1` (no indirect-call cache),
`LWM_KERNEL=1` (kernel lwmutex), `PS3_LWCOND_SLEEP=1`, `GCM_POS_FLIP=0`,
`RSX_TEX_WATCH=0` (hash textures every frame), `RSX_VCACHE=0` /
`RSX_VCACHE_CHECK=1` / `RSX_VC_WATCH_CHECK=1`, `RSX_BUF_POOL=0`,
`RSX_SNAP_INCR=0` (full snapshot copies), `SPU_TASK_POOL=0` (a thread per
task), `SPURS_TASKSET_SERIAL=01AA7700` (PhysX one task at a time),
`RSX_ASYNC_SHADERS=0` (build shaders on the walker), `RSX_QUERY_NOSYNC=1`,
`DOD3_PACE=vblank`, `DOD3_VBLANK_MULT=<n>` / `DOD3_VBLANK_HZ=<hz>`,
`DOD3_FIFO_SLEEP_MS=<n>`, `DOD3_FAST_POLL_LR=<hex>` (0 for none),
`PS3RECOMP_RSX_ENGINE=vtable` (the toolkit's old RSX path).

`PS3_VERBOSE=1` turns the runtime's per-event log back on (it is off here:
two lines per SPURS job at 2,700 jobs a second).

## The installer and the menu patch

- `DOD3_INSTALL_BASE=<dir>`: install into and run from another folder.
- `DOD3_SETUP_ADD=<file>|<file>...`, `DOD3_SETUP_AUTO=<n>` (press the
  primary button of the next n pages), `DOD3_SETUP_GRAB=<prefix>` (save every
  page as `<prefix>_<n>.ppm`): the wizard driven without a hand on it.
- `PS3_VFS_OVERLAY=<dir>` with `DOD3_SHA_OVERRIDE=<package>=<sha1>`: run with
  a menu patch made by `tools/menu_patch.py <dir>` without rebuilding its
  header. `python tools/ue3 dis <package> <Class> [Function]` disassembles
  any script; `python tools/ue3 diff <dir a> <dir b>` compares two builds'.
- `DOD3_FOCUS_FILE=<path>`: "out of focus" is the existence of that file, for
  testing `DOD3_UNFOCUSED`.
- `DOD3_LOG_FILE=1` writes `dod3.log` even with a console.
