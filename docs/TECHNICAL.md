# How the port works

Drakengard 3 is an Unreal Engine 3 game. Its PPU executable (30,134
functions) is translated to C++ by ps3recomp's lifter and compiled into
`dod3`; its 40 SPU programs and two raw SPURS jobs are lifted the same way.
The PS3's system libraries are ps3recomp's HLE implementations, and the RSX
command stream goes through the toolkit's draw engine to Direct3D 12, Metal
or Vulkan. What this repository adds is the port: the runner, the pieces of
the game's own code done natively where the lifted version was too slow, the
settings menu, the installer, and the toolkit changes in the `ps3recomp/`
fork.

## The runner (`main.cpp`)

- **Boot.** The guest address space is one flat 4 GB mapping (reserved on
  Windows and committed on first touch by a vectored exception handler, so
  any 32-bit guest pointer is valid). The EBOOT's ELF is loaded, the lifted
  function table and the HLE handlers registered, the title's defaults set,
  and the entry OPD dispatched. On macOS the guest runs on its own thread
  and the frame clock takes the main thread, which AppKit needs.
- **The frame clock.** The RSX has no vblank interrupt here, so a host
  thread synthesises it: it ticks vblanks and flips at the vblank rate,
  drains the GCM FIFO on every tick (the FIFO walker; the sync labels the
  game waits on are written by that drain), and presents a frame when the
  guest flips. Between ticks the walker sleeps until a guest `usleep`
  kicks it, so a render thread polling a label is answered in microseconds
  rather than a sleep later. The render thread's own fence poll
  (`func_008B0C70`) sleeps until the next label write or drain instead of
  its 200 µs.
- **Frame rate.** `DOD3_FPS=n` replaces `UEngine::GetMaxTickRate` in the
  function registry so it returns n (the game's own smoothing, a 300-frame
  running average clamped to 22-30 fps, is bypassed) and runs the vblank at
  8n Hz so a late frame is late by a millisecond, not a vblank.
- **Out of focus.** `DOD3_UNFOCUSED=pause` stops the frame clock, holds the
  audio mixer and freezes the guest clocks (`mftb`, the system time, the SPU
  decrementer, RSX timestamps: ps3recomp's `sys_timer.c`), so the game never
  sees the pause as a hitch; `mute` plays silence.
- **Title defaults** the game needs from the runtime, set in `main()`: the
  GCM window moved to 0x8F000000 (the game maps its own memory over the
  default), FIFO resync off, NV0039 on subchannel 1, SPURS job chains run
  synchronously with their completion events off, the PhysX taskset at its
  hardware contention of 3, a wider memory window, the point-light shaft
  mask clamped (`RSX_FP_SAT_ALPHA`), the sound threads at high priority.

## Guest memory

The lifted code reads and writes guest memory through one lookup in a
64 KiB-page table and one branch (`ppu_vm_fast.h`); pages that need the full
path (watched, unmapped) clear their bit. Textures the engine has uploaded
are write-watched: the 4 KiB pages under them are made read-only, a write
faults and marks them, and a texture is re-hashed only when a page under it
was written (RPCS3's method). The vertex cache is validated the same way.

## Native fast paths

Where the lifted code was the bottleneck, the function runs natively, with a
check mode that runs both and compares:

| What | Where | Check |
|---|---|---|
| The garbage collector's reachability pass (`func_00EE6538`): a statement-for-statement translation; phase 2 on 8 worker threads; the periodic purge moved into the frame limiter's sleep | `src/dod3_gc.cpp`, `src/dod3_gc_native.cpp` | `DOD3_GC_NATIVE=check` |
| ShaderPatching's LZF decoder (an SPU job run ~200 times a frame on the render thread): the copy loops, whole tokens, and a memo of decoded shaders | `src/dod3_spu_hooks.c` | `DOD3_SPU_NATIVE_CHECK=1` |
| MultiStream's DSP biquads (half of the audio SPU task) | `src/dod3_msdsp_hooks.c` | `DOD3_SPU_NATIVE_CHECK=1` |
| The MP3 decoder MultiStream loads from the PS3's `flashMP3.pic`: a native decoder (`src/dod3_mp3dec.c`) behind a stand-in image the runtime loads instead | `src/dod3_mp3_native.c`, `tools/make_spu_overlays.py` | `DOD3_MP3_CHECK=1` |
| The package inflate the game sends to an SPU zlib task through a SPURS LFQueue | `src/dod3_edgezlib.cpp` | each chunk's Adler-32 |
| The SPURS queue and LFQueue paths, lifted from libsre at build time (the HLE's own were not enough for this game) | `src/spurs_queue.cpp`, `src/spurs_lfqueue.cpp`, `tools/gen_libsre.py` | |

Hooks into lifted SPU code are declared in `tools/lift_spu.sh`
(`--native-hook`): a hook is called first thing in a lifted function and
either does its work or falls through.

## Rendering

The draw engine (`ps3recomp/libs/video/rsx_draw_engine.c`) decodes the RSX
register file into draws and hands them to a backend: `rsx_d3d12_engine.c`,
`rsx_metal_backend.m`, `rsx_vulkan_engine.c`. Shaders are decompiled to HLSL
and compiled by each backend (DXBC with a disk cache; MSL through SPIR-V;
SPIR-V). What this game needed from the engine:

- **Snapshots** of a target that is read while it is drawn into (the heat
  haze samples the scene ~95 times a frame): kept incrementally, by
  redrawing only the draws since the last snapshot with a copy shader.
- **Occlusion queries** with their GPU round trip before the fence the game
  waits on (without it, objects flicker and are culled).
- **Internal resolution** (`RSX_SCALE`): every target is scaled except the
  small ones (the colour-grading lookup table is built texel by texel from
  WPOS), with viewports, scissors, clears, occlusion counts and WPOS scaled
  per pass by the target it draws into.
- **MSAA**: multisampled twins of the full-size scene targets, resolved
  before anything reads them; **FXAA** and the supersampling blit are
  present passes (`rsx_present_passes.h`).
- **Live changes**: the settings page asks the engine to re-read the display
  settings; a new scale remakes every target under the same handle.

The replay tool (`rsx_replay`) replays captured command streams
(`RSX_CAPTURE`) headlessly through the engine: the rendering regression.

## The settings menu

The game's own Settings menu gains Graphics Settings, System Settings and
Advanced Graphics. The menu is UnrealScript in `SQEX03GAME.XXX`;
`tools/menu_patch.py` rewrites 18 script functions (the root list, an unused
page the title shipped with its layout, the title's page chain for skip
intro, the camera for the field of view) and records them as edits of the
original functions in `src/dod3_menu_patch_data*.h`: ranges copied from the
original body plus the new bytes. Nothing of the game's bytecode is in the
repository.

At boot, `src/dod3_menu_patch.cpp` checks the player's package against the
hash the EBOOT carries for it, applies the edits, writes the patched package
to an overlay folder the file system lays over the disc (`game/patch101`),
and writes the patched hash into the EBOOT's own table (the engine checks
loaded packages against it). The pages reach the port through two natives
the script already had (`GetString`, `UpdateDisplayParam`), whose exec
thunks `src/dod3_settings_menu.cpp` replaces; it owns the rows, the values
and the `dod3.ini` writer.

## The 1.01 update

The port runs the game's 1.01 update, as a PS3 with the update would. The
update is the official package: a new EBOOT and a `PATCH` folder of
replaced packages. Its native code is the same compiler's build of the same
source with 452 of 30,134 functions changed (the Japanese voice DLC, French
text, fixes) and the rest moved; `tools/eboot_diff.py`, `tools/addr_map.py`
and `tools/data_map.py` found the 1.01 column of every address the port's
code knows (`src/dod3_eboot.h`). The SPU programs are byte-identical.

The update installs whole under `game/disc/game/BLES00000`, and a 1.01 build
boots the title as its update (`PS3_GAME_PATCH`): cellGame reports the
update's folder, and the engine finds every patched file there. A 1.00 build
never reads it, so one game tree serves both.

## DLC

The 20 PSN packs install as a PS3 installs them, under
`game/disc/game/NPUB31251/USRDIR`, where the disc title looks for them at
boot. Their content is plain UE3 packages; the few licence-bound files are
not read, except the Japanese voice pack's two file lists, which the port
writes itself from the installed files (`src/dod3_dlc.cpp`).

## The installer

`src/setup_*.cpp`, one Dear ImGui wizard for both platforms over a small
platform layer (`setup_ui.h`: Win32 + Direct3D 12, or Cocoa + Metal). Each
file the player adds is recognised by itself: the disc by its PARAM.SFO and
the hash of its EBOOT.BIN (`setup_iso.cpp`, an ISO 9660 reader), the update
and the DLC by their package content IDs and footer checksums
(`setup_pkg.cpp`). Each part is unpacked into a staging folder and moved
into place whole, so a failure leaves nothing half-installed. The game's
executable is made from the update's EBOOT.BIN (`setup_self.cpp`) and
checked by its hash. The title's own first-boot copy of 5 GB of game data
is laid out as links onto the disc instead (`gamedata_layout`), at install
and at every boot.

## Audio

MultiStream runs on the SPUs as on the console; the mixer feeds WASAPI on
Windows and CoreAudio through SDL on macOS, paced one block per 5.33 ms by
the clock (pacing on buffer room mixed in bursts and lost blocks). The
sound-driver threads and the mixer task run at high priority.

## What made the difference

Measured in the chapter 1 battle on an i9-13900K / RTX 4090, from the first
Windows run at 49 fps to the game's own 120 fps ceiling at 720p:

- guest loads and stores through the page table instead of four tests per
  access (45 → 81 fps); `sys_lwmutex` as a user-mode lock word instead of a
  kernel semaphore (40 → 50); the indirect-call cache; the lifted code at
  x86-64-v3 with the registers as locals;
- SPU task threads pooled, GPU vertex buffers pooled, textures write-watched
  instead of hashed every frame (+13 fps, walker 6.5 → 4.4 ms);
- the SPU sources built with SSE4.1 and FMA (the mixer task 50% → 16% of a
  core), the mixer paced by the clock;
- the garbage collector native, parallel and deferred into the limiter's
  sleep (a 20 ms frame every 15 s → none over 20 ms); the ShaderPatching
  memo and patch loop (+22 fps uncapped); the PhysX taskset at its hardware
  contention (the destruction scene 38 → 57 fps);
- `sys_lwcond` signals that wake their waiters (loads ~10 s faster), MFC
  slots released per job, snapshots kept by copy draws at 4K (GPU busy 500
  → 220 ms/s).

Measured as no gain, so nobody repeats them: pinning to the P-cores, EcoQoS
opt-out, skipping the occlusion-query sync (`RSX_QUERY_NOSYNC`, also
flickers), submitting partial batches early, parallel job-chain workers
(`SPURS_JC_PAR`: the jobs are too small), vsync off at a cap, the five-test
inline store path.
