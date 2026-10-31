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

## The runner (`src/main.cpp`)

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
- **Movies.** With no render commands to run, Unreal's rendering thread
  ticks the Bink player itself, at most `GRenderingThreadMaxIdleTickFrequency`
  times a second: 40 in this title. A 29.97 fps movie's frames, each shown
  at the first tick after it is due, came out 25, 25 and 50 ms apart. While
  a movie plays (the renderer counts the uploads of its planes) the port
  sets that rate to 1000 and the frames come 33.4 ms apart; the rest of the
  time it is the frame-rate cap. `DOD3_RENDER_TICK_HZ=<hz>` holds it at one
  value (40 is the title's).
- **Loading screens.** A chapter's packages come in through Unreal's async
  loader, which runs once a frame, for 5 ms at most, and gives the frame up
  whenever the read it needs next has not come back; the rest of the frame
  the game thread sleeps in the limiter. So a loading screen lasted a number
  of frames, not an amount of work: 265 for the first chapter's, which is
  4.5 s at 60 fps and 9 s at the console's 30. While no scene is drawn and
  packages are pending, the port spends the limiter's sleep in the loader
  (`src/patches/load_idle.cpp`): that screen takes 2.7 s, and the frame rate
  is what it was. `DOD3_LOAD_IDLE=0` turns it off.
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

## The lifted code

As the lifter first writes it, every guest register is a slot of the
register file in memory and every call goes through that memory, which
leaves a compiler little to do. What is done about it is in the lifter
(`ps3recomp/docs/PPU_RECOMP.md` has the detail and the switches):

- a function's registers are C locals, written back to the register file
  before a call only where they changed and something reads them: 10.1
  million stores at the game's 450,000 calls and returns became 3.7
  million. The CR fields and the carry are locals of their own, so a
  compare and the branch after it are a compare and a branch;
- the saves of a prologue, and any run of loads and stores off one
  register, share one test of the page table (687,000 accesses in 88,000
  runs);
- the AltiVec operations a compiler made scalar loops of are one SSE helper
  each. The unaligned load, `lvlx`, was 190 instructions and a dozen
  branches as a loop over its bytes and is a table-driven shuffle; the
  collision tree walk (`func_012B32C8`), which is made of them, went from
  10,000 instructions to 3,700;
- on Windows the lifted units are built without thread-local guards
  (`/Zc:tlsGuards-`): the tail-call trampoline is a thread-local read after
  a call.

A lift can be made to check all of this while the game runs
(`PPU_NV_VERIFY=1`, see [DEBUGGING.md](DEBUGGING.md)).

## Native fast paths

Where the lifted code was the bottleneck, the function runs natively, with a
check mode that runs both and compares:

| What | Where | Check |
|---|---|---|
| The garbage collector's reachability pass (`func_00EE6538`): a statement-for-statement translation; phase 2 on 8 worker threads; the periodic purge moved into the frame limiter's sleep | `src/patches/gc.cpp`, `src/patches/gc_native.cpp` | `DOD3_GC_NATIVE=check` |
| ShaderPatching's LZF decoder (an SPU job run ~200 times a frame on the render thread): the copy loops, whole tokens, and a memo of decoded shaders | `src/patches/spu_hooks.c` | `DOD3_SPU_NATIVE_CHECK=1` |
| MultiStream's DSP biquads (half of the audio SPU task) | `src/apu/msdsp_hooks.c` | `DOD3_SPU_NATIVE_CHECK=1` |
| The MP3 decoder MultiStream loads from the console's firmware: a native decoder (`src/apu/mp3dec.c`) behind a stand-in image of our own that the runtime serves in its place | `src/apu/mp3_native.c`, `tools/make_spu_overlays.py` | `DOD3_MP3_CHECK=1` |
| The package inflate the game sends to an SPU zlib task through a SPURS LFQueue | `src/patches/edgezlib.cpp` | each chunk's Adler-32 |
| The SPURS queue and LFQueue paths, lifted from libsre at build time (the HLE's own were not enough for this game) | `src/kernel/spurs_queue.cpp`, `src/kernel/spurs_lfqueue.cpp`, `tools/gen_libsre.py` | |

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
  waits on (without it, objects flicker and are culled). The frame goes to
  the GPU in pieces while it is recorded (`RSX_SUBMIT_EVERY`, 256 records),
  so the round trip waits for the end of the frame's work, not all of it.
- **Internal resolution** (`RSX_SCALE`): every target is scaled except the
  small ones (the colour-grading lookup table is built texel by texel from
  WPOS), with viewports, scissors, clears, occlusion counts and WPOS scaled
  per pass by the target it draws into.
- **MSAA**: multisampled twins of the full-size scene targets, resolved
  before anything reads them; **FXAA** and the supersampling blit are
  present passes (`rsx_present_passes.h`).
- **Live changes**: the settings page asks the engine to re-read the display
  settings; a new scale remakes every target under the same handle.
- **Memory by the block**, for a level's first frame, which brings some 500
  textures and 25 MB of vertices at once. A texture is placed in a 64 MB
  heap and a vertex-cache buffer is a range of a 32 MB mapped page (D3D12;
  the Vulkan engine sub-allocates its device memory), where each used to be
  an allocation in the driver; and what a submit stages is a chain of 16 MB
  chunks, where it used to be one arena grown by copying it out of
  write-combined memory. That frame went from 0.84 s to 0.2 s, the
  renderer's share of it from 0.68 s to 0.07 s.
- **Pipeline states built ahead**: a draw whose pipeline state the driver is
  still compiling is skipped, so the first sight of an effect came in late.
  The D3D12 engine records every state it builds in
  `cache/dxbc/pipelines.bin` (the programs by checksum, the state's values)
  and builds the listed ones on low-priority threads at start-up. A Windows
  release ships `shaders/dxbc` as its `cache/dxbc`: the bytecode and the
  list that a sweep of the whole game recorded (every part the book opens,
  chapters, branches, requests and DLC, played and with its cutscenes run:
  1,720 programs, 2,037 states). A player's machine then compiles no shader
  the sweep met, and builds the states before they are needed; a player's
  sessions add what the sweep missed. `tools/shader_cache.py` builds the
  bytecode cache from shader dumps (`PS3RECOMP_SHADER_DUMP`) and checks and
  merges lists.
- **The cache made again without playing**: both lists above hold what the
  decompilers made, so a decompiler change leaves them stale. The draw
  engine also records what the game gave -- each pipeline's programs and
  every input to the decompilers, in `cache/dxbc/guest_pipelines.txt` and
  `guest_programs.bin` -- and a start with `RSX_GUEST_REBUILD=1` translates
  that list again with the current code, compiles it and exits (the sweep's
  1,897 pipelines: 22 s). `tools/shader_cache.py remap` then carries over
  the states only play adds (multisampled builds, snapshot copies). The
  guest list holds the game's own programs, so it stays out of the
  repository and the release; the maintainers keep the sweep's, and refresh
  `shaders/dxbc` from it when the decompilers change.

The replay tool (`rsx_replay`) replays captured command streams
(`RSX_CAPTURE`) headlessly through the engine: the rendering regression.

## The settings menu

The game's own Settings menu gains Graphics Settings, System Settings and
Advanced Graphics. The menu is UnrealScript in `SQEX03GAME.XXX`;
`tools/menu_patch.py` rewrites 18 script functions (the root list, an unused
page the title shipped with its layout, the title's page chain for skip
intro, the camera for the field of view) and records them as edits of the
original functions in `src/patches/menu_patch_data*.h`: ranges copied from the
original body plus the new bytes. Nothing of the game's bytecode is in the
repository.

At boot, `src/patches/menu_patch.cpp` checks the player's package against the
hash the EBOOT carries for it, applies the edits, writes the patched package
to an overlay folder the file system lays over the disc (`game/patch101`),
and writes the patched hash into the EBOOT's own table (the engine checks
loaded packages against it). The pages reach the port through two natives
the script already had (`GetString`, `UpdateDisplayParam`), whose exec
thunks `src/ui/settings_menu.cpp` replaces; it owns the rows, the values
and the `dod3.ini` writer. With the Spanish translation installed and
switched on (`dod3_menus_spanish`, `src/kernel/mods.cpp`), the pages'
names, rows and values are given in Spanish, in the translation's own
words for the game's menus.

A script package that is not the update's -- a mod copied over the update
by hand -- is not patched: its own hash goes into the table instead, so it
plays as it is, without the pages.

## Button prompts

Every button the game draws, in its text, its menus' help bar and its HUD,
is a sprite of one layout (`HUD_COMMON.menu.icon_button`, 28 rectangles)
over one 256x128 atlas of DualShock 3 buttons
(`HUD_COMMON.Texture.icon_button`); the text reaches it through a control
code in the message tables. The cooker put the same atlas in seven
packages, so its content hash names it wherever it was loaded from.

The draw engine asks the port about each texture it uploads
(`rsx_draw_engine_set_texture_standin`), and for that atlas
`src/patches/prompts.cpp` answers with another: the same rectangles drawn
for a DualShock 4, a DualSense, an Xbox pad, a Switch pad or the keyboard
(`tools/prompt_atlas.py`, at four times the size). The rectangles reach the
GPU as normalised coordinates, so the larger atlas is only sharper; it is
handed over at the internal resolution's scale, because the game samples it
without mip levels. Which set is shown follows player 1's pad as SDL names
it, or `DOD3_PROMPTS`; a change asks the engine to put its question again,
and only the atlas is uploaded anew.

Not covered, and still the DualShock 3's: the words "Press the START
Button" and "Press START to Skip" (textures of their own), the Circle
drawn into the dragon's call icon, the pad pictured on the controller
settings page, and the handful of messages that name a control in words.

## Keyboard, mouse and the camera

The pad library takes port 0's state from the port as well as from a real
pad (`cellPad_set_host_input`): `src/input/kbm.cpp` turns the keys and
mouse buttons held into buttons and stick directions by a table of
bindings read from `dod3.ini`. The two are merged, and whichever was
touched last decides the button prompts; the keyboard's prompt atlas is
made at start-up from a sheet of key caps, so it shows the keys bound.

The mouse is not a stick. The script hands the pawn's native code the
camera stick as two floats (`GetCameraMoveX`, `GetCameraMoveY`); the
settings patch routes them, and the frame's time, through the port
(`src/input/camera.cpp`). Measured frame by frame with constant values in
their place, the native code turns the view by 3440 x value x dt squared
degrees, with no dead zone, limit or easing. So the mouse's motion since
the last frame, a turn in degrees, divided by that is the value that makes
exactly the turn: the view follows the hand to within a percent. The
pitch's constant falls away towards the ends of its range and is read off
the view's own pitch, which the script reports each frame. The window's
side is raw input and a hidden, confined cursor while the game has the
focus (`ps3recomp/libs/video/rsx_host_mouse.c`). On macOS the Metal
backend does it for its own window (`rsx_metal_backend.m`): the cursor is
hidden and detached from the mouse, whose events still carry the motion,
and the keys and mouse buttons the bindings read are that window's record
of the events it was sent, so nothing is held unless the game is in front.

The same measurement showed the stick's turn speed to be proportional to
the frame time: right at the 30 frames a second the game was made for, a
quarter of that at 120. The stick's value is scaled by (1/30)/dt above 30
frames a second, so the camera turns as on the console at any rate
(`DOD3_CAMERA_SPEED=original` for the game's own). The dragon's camera has
not been measured and its stick is left alone.

## The 1.01 update

The port runs the game's 1.01 update, as a PS3 with the update would. The
update is the official package: a new EBOOT and a `PATCH` folder of
replaced packages. Its native code is the same compiler's build of the same
source with 452 of 30,134 functions changed (the Japanese voice DLC, French
text, fixes) and the rest moved; `tools/eboot_diff.py`, `tools/addr_map.py`
and `tools/data_map.py` found the 1.01 column of every address the port's
code knows (`src/cpu/eboot.h`). The SPU programs are byte-identical.

The update installs whole under `game/disc/game/BLES00000`, and a 1.01 build
boots the title as its update (`PS3_GAME_PATCH`): cellGame reports the
update's folder, and the engine finds every patched file there. A 1.00 build
never reads it, so one game tree serves both.

## DLC

The 20 PSN packs install as a PS3 installs them, under
`game/disc/game/NPUB31251/USRDIR`, where the disc title looks for them at
boot. Their content is plain UE3 packages; the few licence-bound files are
not read, except the Japanese voice pack's two file lists, which the port
writes itself from the installed files (`src/kernel/dlc.cpp`).

## The setup, the launcher and mods

`src/ui/wizard.cpp` is the first-run setup and `src/ui/launcher.cpp` the
launcher every later start opens (Play, Mods, DLC, Settings): Dear ImGui
over a small platform layer (`ui/ui.h`: Win32 + Direct3D 12, or Cocoa +
Metal). Both are drawn with the same pieces (`ui/look.*`: the Inter fonts,
the colours, the game's key art from the player's own disc or package,
buttons, cards) and share the file checks and the install job
(`ui/setup_shared.*`). The launcher's Settings read and write `dod3.ini`
with `ui/ini.*`, which reads it as the game does (the first of a key set
twice counts).

On Windows the launcher also looks for a newer release as it opens
(`install/update.*`): it reads the repository's release list (not GitHub's
"latest release", which leaves pre-releases out) on a thread nothing waits
for, and offers the newest one above its own version that has a Windows zip
with a SHA-256 digest. Install Update downloads the zip into `.update/`
beside the program, checks its size and digest, unpacks it, and swaps each
file in: the old one renamed to `<name>.old` (Windows lets a running
program and its loaded DLLs be renamed), the new one moved into its place;
a failed swap puts back every file swapped so far. `game/`, `gamedata/` and
an existing `dod3.ini` are never written. The launcher then starts the new
`dod3.exe` and closes; the new one's first start removes the `.old` files.
The approach follows King Kong Recompiled's updater.

Each file the player adds is recognised by itself (`install/install.cpp`):
the disc by its PARAM.SFO and the hash of its EBOOT.BIN (`install/iso.cpp`,
an ISO 9660 reader), the update, the DLC and the PSN release's game by their
package content IDs and footer checksums (`install/pkg.cpp`), a mod by its
packages' names or its folder's layout (a `.zip` through `install/zip.cpp`).
Each part is unpacked into a staging folder and moved into place whole, so
a failure leaves nothing half-installed. The game's executable is made from
the update's EBOOT.BIN (`install/self.cpp`) and checked by its hash. The
title's own first-boot copy of 5 GB of game data is laid out as links onto
the disc instead (`gamedata_layout`), at install and at every boot.

A mod leaves the player's game files as they are. Its files go to
`game/mods`, at the path each stands in for under `game/disc`, and
`src/kernel/mods.cpp` serves them from there through the file system's path
hook. Only the files for the update's own folder, which the title lists, go
into that folder, with the update's files they replace kept in
`game/modfiles/<id>/orig/`. `game/modfiles/<id>.txt` lists every file a mod
put in place, so Remove takes that mod alone out and puts the originals
back without the update package. The launcher's switch moves the listed
files to `game/modfiles/<id>/off/`, puts the originals back meanwhile and
marks the mod off (`<id>.off`); on moves them back. The DLC Undub's voices
go beside the voice pack's (`DLC_JPV/UNDUB`), and the game's own Voice
option switches them.

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

The lifted code itself, later (i9-13900K, uncapped at 720p, each scene
run in turn with the build before): the hub 187 → 205 fps, its main thread
2.05 → 1.40 ms of CPU a frame and the render thread 2.67 → 2.04; the
chapter 1 battle 179 → 208 fps, main thread 4.6 → 2.9 ms, render thread
3.6 → 2.3; from the start to the title 9.3 → 8.2 s; the executable 430 →
293 MB. What did it: registers written back only where changed and read,
the CR fields as locals, no thread-local guards, one page test for a run of
accesses, SSE forms of the AltiVec loops.

A sweep of every part of the game (the 100% save, each part loaded from
the book and played for two minutes) found the slowest ones -- 77 to 105 fps
against the 120 cap -- held up in one place: the walker waiting 2-5 ms a
frame at the fence after the occlusion queries, for a frame of GPU work it
had handed over only at that fence. Handing it over every 256 records
instead, paired in one run: one of them 95 → 109 fps (91 → 107 at 4K), the
heaviest 77 → 89, a part at the cap unchanged. The present's pacing had to count presents
rather than submits for it (a fence two submits back was this frame's own).
Skipping the sync altogether reaches the cap there but flickers; holding the
fence back while the walker goes on gains nothing, because the game waits
for those counts before its next queries.

Measured as no gain, so nobody repeats them: pinning to the P-cores, EcoQoS
opt-out, parallel job-chain workers
(`SPURS_JC_PAR`: the jobs are too small), vsync off at a cap, the five-test
inline store path, `-O3` on the lifted units (byte-identical objects),
profile-guided optimisation of the lifted units (`DOD3_PGO`, an IR profile
of a battle run and an idle run: the idle scene 8% slower, the collision
test 10% more cycles per call -- the layout the profile asks for does the
huge units no good).
