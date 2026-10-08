# Drakengard 3 Recompiled

A native PC port of **Drakengard 3** (Drag-On Dragoon 3, PlayStation 3,
BLUS31197) for Windows and macOS, made by static recompilation of the game's
code on the [ps3recomp](https://github.com/sp00nznet/ps3recomp) toolkit.

The game's PowerPC and SPU code is translated to C++ ahead of time and built
into a normal executable, with the PS3's system libraries replaced by native
implementations and the RSX graphics by Direct3D 12, Metal or Vulkan. There
is no emulation at run time.

**Nothing of the game is in this repository.** You need your own copy of
Drakengard 3: the US disc and the game's 1.01 update, plus whatever DLC you
own. The installer takes them from there.

- **Players:** [docs/PLAYING.md](docs/PLAYING.md) -- what you need, installing,
  settings, troubleshooting.
- **Developers:** [docs/BUILDING.md](docs/BUILDING.md) (toolchain, lifting
  and building), [docs/TECHNICAL.md](docs/TECHNICAL.md) (how the port works),
  [docs/DEBUGGING.md](docs/DEBUGGING.md) (switches, profiling, the regression
  tests), [CONTRIBUTING.md](CONTRIBUTING.md).

## Status

| | |
|---|---|
| Windows 10/11, Direct3D 12 | Playable. 60 fps at 4K on an RTX 4090 / i9-13900K (chapter 1 through the heavy destruction scene, open areas, the village); 120 fps holds at lower resolutions |
| Windows, Vulkan | Experimental (`RSX_BACKEND = vulkan`) |
| macOS 13+, Apple silicon, Metal | Playable. 60 fps at 1080p in battle on an M1 Pro |
| Game version | The 1.01 update (required) |
| DLC | All 20 packs, including the Japanese voice pack |

What the port adds over the original: any internal resolution (supersampling
included), windowed, borderless or full-screen display, a frame-rate cap of
your choice (60 and 120 tested), FXAA or MSAA, anisotropic filtering, a wider
field of view, skipping the intro logos and movie, and pausing or muting when
the window is in the background -- all from a Graphics Settings and a System
Settings page added to the game's own Settings menu.

Known issues: pre-rendered movies play with uneven frame pacing, and the game
has been tested on few machines.

## How it is built

```
elf/EBOOT.ELF  --ppu_loader/ppu_lifter-->  recompiled/*.cpp   (the game's PPU code as C++)
SPU programs   --tools/lift_spu.sh------>  spu/               (SPURS tasks, jobs, the audio DSP)
main.cpp, src/ ------------------------->  the port: frame clock, settings, installer, native fast paths
ps3recomp/     ------------------------->  runtime, HLE libraries, the RSX draw engine and its backends
```

The executable `dod3` installs the game from your files (a Dear ImGui
installer, `src/setup_*.cpp`) and runs it. See [docs/BUILDING.md](docs/BUILDING.md).

## Legal

This project is not affiliated with or endorsed by Square Enix, Access Games
or Sony Interactive Entertainment. Drakengard 3 and all of its assets belong
to their owners; none are included, and the port only runs from a copy of the
game you own.

The code in this repository is under the [MIT License](LICENSE). It builds on
ps3recomp (MIT) and includes Dear ImGui (MIT); see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Credits

- [ps3recomp](https://github.com/sp00nznet/ps3recomp) by sp00nznet: the
  recompiler, the runtime and the PS3 library implementations. This port's
  changes to it are in the [soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp)
  fork, the `ps3recomp/` submodule.
- [RPCS3](https://rpcs3.net): the reference for much of the PS3's behaviour,
  and the source of two techniques used here (write-watched texture memory,
  a guest clock that stops while the game is paused).
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and
  [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp): the
  model for a recompiled port, and for its installer.
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut: the
  installer's user interface.
