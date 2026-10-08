# Contributing

Start with [docs/BUILDING.md](docs/BUILDING.md) to get a build, then
[docs/TECHNICAL.md](docs/TECHNICAL.md) for how the port works and
[docs/DEBUGGING.md](docs/DEBUGGING.md) for the switches and the regression
tests.

## The two repositories

- This repository holds the port: `main.cpp`, `src/`, `tools/`, the docs.
- `ps3recomp/` is a submodule: the [soyjxck/ps3recomp](https://github.com/soyjxck/ps3recomp)
  fork of the toolkit (branch `master`), where the runtime, the HLE
  libraries and the RSX engine live. Changes there go to the fork first;
  fixes of general use are offered upstream as single-change pull requests
  from `pr/*` branches of the fork.
- A change that spans both: commit and push the fork first, then commit
  the new submodule pointer here together with the port's change.

## One branch for both platforms

Windows and macOS are built from the same `main`. Keep platform code behind
`#ifdef _WIN32` / `__APPLE__` or in per-backend files, and keep `main.cpp`
and `src/` building on both: a reference to something that exists only in a
Windows-only file (`rsx_d3d12_engine.c`, `src/win_prof.cpp`, ...) must sit
inside `#ifdef _WIN32`. One unguarded `extern` is enough to break the other
platform's link.

Say in the commit message when a change needs a **re-lift** on other
machines: lifter changes, new lift flags or `--hook`s, new `--native-hook`s
in `tools/lift_spu.sh`. `recompiled/` and `spu/` are generated and not in
git.

## Before committing

Verify. For rendering and engine changes, the replay regression
(`tools/replay_regress.sh`); for everything else at least one run of the
game that exercises the change, hands-off where possible
(`tools/live_bench.sh`, `tools/autoplay.sh`). Record what you measured in
the commit message: a performance change without numbers is a guess, and a
change that was measured as no gain is worth a line too, so nobody repeats
it.

Commit messages: a short imperative subject, then a body that says why and
gives the measured effect.

## Style

C and C++ follow `.clang-format` (4 spaces, braces on their own line for
functions only, `char* p`, `/* */` comments). Run `clang-format -i` on the
files you touch in `main.cpp` and `src/`; `third_party/` and the generated
tables (`src/dod3_menu_patch_data*.h`, `src/dod3_mp3dec_tables.h`,
`src/dod3_mp3_standin.h`) are not formatted.

Comments describe the code as it is and why it is that way. The history of
how a bug was found belongs in the commit message, not the source.

Python tools take a shebang, a docstring that gives the usage, and
`argparse` for anything beyond a positional argument or two. Shell scripts
are bash, and run under Git Bash on Windows as well as on macOS.

## What not to commit

Nothing from the game or the PlayStation 3: no executables, packages,
captures of the game's frames or memory, or files derived from them that
contain its data. `.gitignore` covers the usual ones (`game/`, `elf/`,
`out/`, `*.pkg`, `*.iso`, ...); when in doubt, leave it out.
