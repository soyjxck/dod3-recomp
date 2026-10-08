# Drakengard 3 Recompiled — playing it

A native PC port of **Drakengard 3** (PS3), made by static recompilation of
the game's code. It does not include the game: you need your own copy of the
**US disc (BLUS31197)**, and the setup takes the game's files from it.

This project is not affiliated with or endorsed by Square Enix, Access Games
or Sony.

## What you need

- **Windows 10 or 11, 64-bit**, a GPU with Direct3D 12, and a CPU with AVX2
  (Intel Haswell / AMD Zen or newer). About 16 GB of free disk space.
- **Your copy of Drakengard 3, US release (BLUS31197), version 1.00**, as
  one of:
  - the disc dumped to a folder (the one that contains `PS3_GAME`), or
  - a **decrypted** disc image (`.iso`).

  The easiest way to get it is
  **[PS3 Disc Dumper](https://github.com/13xforever/ps3-disc-dumper)** (free,
  for Windows, macOS and Linux) with a compatible Blu-ray drive: it reads your
  disc and writes a decrypted folder, which the setup takes as it is. A
  modded PS3 (multiMAN, webMAN MOD) can also copy the disc to a decrypted
  folder or image. An image ripped with an ordinary disc tool is still
  encrypted and will be refused. Other regions (EU, JP) are not supported.
- **EBOOT.ELF**, the game's executable decrypted with
  **[RPCS3](https://rpcs3.net)** (the PS3 emulator, free, for Windows and
  macOS). You only need RPCS3 for this; no PS3 firmware is needed.
  1. Find `PS3_GAME/USRDIR/EBOOT.BIN` on your disc dump. If you have an
     `.iso`, open (mount) it first — on Windows right-click it and choose
     *Mount*, on a Mac double-click it — and **copy EBOOT.BIN to a normal
     folder** such as your Desktop: RPCS3 saves its result next to the file,
     and a mounted image is read-only.
  2. In RPCS3, choose *Utilities › Decrypt PS3 Binaries* and pick that
     EBOOT.BIN.
  3. RPCS3 writes **EBOOT.elf** in the same folder. That is the file the
     setup asks for.

  Use the EBOOT.BIN **from the disc**. If you have played the game in RPCS3
  with its update installed, RPCS3's own copy (under
  `dev_hdd0/game/BLUS31197`) is the updated one, and the setup will refuse
  it: this release is built for version 1.00.

## Installing

1. Unzip the release anywhere you like (not inside `Program Files`).
2. Run **dod3.exe**. The first time, the setup asks for the disc, then
   EBOOT.ELF. It checks both and copies the game's files next to dod3.exe
   (this takes a few minutes).
3. The game starts. The first minutes of the first launch stutter while
   graphics shaders are compiled. They are cached in the `cache` folder, so
   later launches are smooth.

To run the setup again: `dod3.exe --setup`.

## Settings

In the game: **Settings → Graphics Settings** (on the title menu, next to
Audio Settings) sets the resolution, display mode, frame rate, v-sync,
anti-aliasing (FXAA, or MSAA 2x/4x/8x), texture filtering and
renderer. **Settings → System Settings** sets whether the logos and opening
movie are skipped, and what the game does while its window is in the
background (keep running, mute, or pause). Confirm with ✕ to save them to
dod3.ini; everything but the renderer and Skip Intro changes at once (a
second of black while the resolution or display mode switches), those two
the next time the game starts.
The first start adds the pages to the game's menu, which takes a second or
two (the patched menu goes to `game/patch`, the game's own files are not
changed).

Or edit **dod3.ini** next to dod3.exe (it explains each setting):

| Setting | What it does |
|---|---|
| `RSX_SCALE` | Internal resolution as a multiple of 1280×720: 1.5 = 1080p, 2 = 1440p, 3 = 4K. Above the window's size it is averaged down: supersampling |
| `RSX_DISPLAY` | `windowed`, `borderless` (recommended for full screen) or `fullscreen` |
| `RSX_WINDOW` | Window size in windowed mode, e.g. `1920x1080` |
| `RSX_VSYNC` | `1` to sync to the display |
| `DOD3_FPS` | Frame-rate cap: `60` is tested and recommended; unset keeps the original 30 |
| `RSX_ANISO` | Texture filtering: `1` (original) to `16` (16× anisotropic, the default) |
| `RSX_AA` | `fxaa`, or `msaa2` / `msaa4` / `msaa8` (MSAA costs a lot at 4K); unset is off |
| `DOD3_SKIP_INTRO` | `1` (default) skips the logos and the opening movie at start-up; `0` keeps them |
| `DOD3_UNFOCUSED` | While the window is in the background: unset keeps running, `mute` silences the audio, `pause` stops the game until you come back (Windows) |
| `RSX_BACKEND` | `vulkan` for the Vulkan renderer (experimental); unset is Direct3D 12 |

## Controls

An Xbox-compatible (XInput) controller is recommended.

## If something goes wrong

- Each run writes **dod3.log** next to dod3.exe (the run before is kept as
  `dod3.prev.log`). Please include it when you report a problem.
- `dod3.exe --check-disc <your .iso or folder> [EBOOT.ELF]` checks your
  files without installing anything.

## macOS

The same game on a Mac, as **Drakengard 3 Recompiled.app**.

- **What you need:** a Mac with Apple silicon (M1 or newer) on **macOS 13
  Ventura or newer**, about 16 GB of free disk space, and the same two things
  from your copy of the game as above (the disc and EBOOT.ELF). RPCS3 has a
  macOS version; its *Utilities › Decrypt PS3 Binaries* works the same way.
- **Installing:**
  1. Unzip the release and move **Drakengard 3 Recompiled.app** to
     Applications (or anywhere you like).
  2. The first time, macOS refuses to open it, because the app is not
     notarized by Apple. On macOS 15 and later: try to open it once, then
     go to *System Settings › Privacy & Security* and choose **Open Anyway**.
     On macOS 13 and 14: Control-click the app, choose **Open**, then
     **Open** again. After that it opens normally.
  3. The setup asks for the disc, then EBOOT.ELF, checks both and copies the
     game's files into `~/Library/Application Support/Drakengard 3 Recompiled`
     (this takes a few minutes). The game then starts; the first minutes
     stutter while shaders are compiled, as on Windows.

  To run the setup again, hold **Option** while opening the app.
- **Settings and logs:** dod3.ini (the same settings as above) and dod3.log
  are in `~/Library/Application Support/Drakengard 3 Recompiled`. In Finder,
  choose *Go › Go to Folder…* and paste that path.
- **Controls:** Xbox, PlayStation (DualShock 4, DualSense) and Switch Pro
  controllers work, wired or over Bluetooth.
- **Checking your files:** in Terminal,
  `"/Applications/Drakengard 3 Recompiled.app/Contents/MacOS/dod3" --check-disc <your .iso or folder> [EBOOT.ELF]`.
- Tested on an M1 Pro: 60 fps with the cap at 60, dipping to the 50s in the
  heaviest battles.

## Known issues

- Pre-rendered movies play with uneven frame pacing.
- The game has only been tested on one PC (Intel i9-13900K, RTX 4090). Reports
  from other hardware are very welcome.
