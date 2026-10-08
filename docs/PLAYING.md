# Drakengard 3 Recompiled — playing it

A native PC port of **Drakengard 3** (PS3), made by static recompilation of
the game's code. It does not include the game: you need your own copy of the
**US disc (BLUS31197)**, and the setup takes the game's files from it.

This project is not affiliated with or endorsed by Square Enix, Access Games
or Sony.

## What you need

- **Windows 10 or 11, 64-bit**, a GPU with Direct3D 12, and a CPU with AVX2
  (Intel Haswell / AMD Zen or newer). About 16 GB of free disk space.
- **Your copy of Drakengard 3, US release (BLUS31197), version 1.00**, as one
  of:
  - a **decrypted** disc image (`.iso`), or
  - the disc dumped to a folder (the one that contains `PS3_GAME`).

  PS3 Disc Dumper and a compatible Blu-ray drive dump your disc to a
  decrypted folder. An image ripped directly from the drive is still
  encrypted and will be refused. Other regions (EU, JP) and copies with the game update installed are
  not supported.
- **[RPCS3](https://rpcs3.net)** (the PS3 emulator, free), for one thing only:
  **EBOOT.ELF**. In RPCS3, choose *Utilities › Decrypt PS3 Binaries* and pick
  `PS3_GAME\USRDIR\EBOOT.BIN` from your disc. RPCS3 writes `EBOOT.elf` next to it.
  No PS3 firmware is needed.

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

Edit **dod3.ini** next to dod3.exe (it explains each setting):

| Setting | What it does |
|---|---|
| `RSX_SCALE` | Internal resolution as a multiple of 1280×720: 1.5 = 1080p, 2 = 1440p, 3 = 4K |
| `RSX_DISPLAY` | `windowed`, `borderless` (recommended for full screen) or `fullscreen` |
| `RSX_WINDOW` | Window size in windowed mode, e.g. `1920x1080` |
| `RSX_VSYNC` | `1` to sync to the display |
| `DOD3_FPS` | Frame-rate cap: `60` is tested and recommended; unset keeps the original 30 |

## Controls

An Xbox-compatible (XInput) controller is recommended.

## If something goes wrong

- Each run writes **dod3.log** next to dod3.exe (the run before is kept as
  `dod3.prev.log`). Please include it when you report a problem.
- `dod3.exe --check-disc <your .iso or folder> [EBOOT.ELF]` checks your
  files without installing anything.

## Known issues

- Pre-rendered movies play with uneven frame pacing.
- The game has only been tested on one PC (Intel i9-13900K, RTX 4090). Reports
  from other hardware are very welcome.
