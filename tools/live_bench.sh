#!/bin/bash
# live_bench.sh <tag> <seconds> [VAR=val ...] -- a hands-off run of the
# chapter 1 battle and a summary of its frame times. Bash on macOS, Git Bash
# on Windows; from any directory.
#
# No save data, so the title goes to New Game; tools/autoplay.sh skips every
# cutscene until the battle HUD is on screen (it asks the game for
# screenshots through PS3RECOMP_FRAME_GRAB) and then keeps Zero fighting.
# Gameplay starts about 90 s after the first frame; the summary
# (tools/bench_summary.py) covers t >= FROM (default 100 s).
#
#   out/bench/<tag>.log, out/bench/<tag>.autoplay.log, and the summary.
#   VAR=val arguments go into the game's environment: DOD3_PROF=2,
#   AUDIO_GAPS=1, RSX_VSYNC=0, and AUTOPLAY_FORWARD=1, AUTOPLAY_COMBAT=1,
#   AUTOPLAY_GRAB_UNTIL=<s> (see tools/autoplay.sh).
#   DOD3_BIN=<path>  the game (default build/dod3, .exe on Windows)
#   DOD3_ELF=<path>  the executable it runs (default elf/EBOOT_101.ELF, or
#                    elf/EBOOT.ELF when the game's path says 100)
#   FROM=<s>         the summary's start
#   PROFILE=1        macOS: a 10 s `sample` of the process 25 s before the
#                    end (it stalls the game, so the summary stops 40 s
#                    before the end), in out/bench/<tag>.prof.txt
#
# Refuses to start while another dod3 runs: the frame times would not be
# comparable. Run-to-run variance is about 5-15% in fps, so compare pairs.
tag=$1; secs=$2; shift 2
[ -n "$tag" ] && [ -n "$secs" ] || { echo "usage: $0 <tag> <seconds> [VAR=val ...]"; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
python=${PYTHON:-python3}
"$python" -c "import sys" >/dev/null 2>&1 || python=python   # a python3 that is only the Microsoft Store alias
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) win=1; bin=${DOD3_BIN:-build/dod3.exe};;
  *) win=; bin=${DOD3_BIN:-build/dod3};;
esac
if [ -n "$win" ]; then
  tasklist 2>/dev/null | grep -qi "^dod3.exe" && { echo "a dod3.exe is already running; not starting another"; exit 1; }
else
  pgrep -f "/dod3( |$)" >/dev/null 2>&1 && { echo "a dod3 is already running; not starting another"; exit 1; }
fi
[ -x "$bin" ] || { echo "no game at $bin (DOD3_BIN=<path>)"; exit 1; }
case "$bin" in *100*) elf=${DOD3_ELF:-elf/EBOOT.ELF};; *) elf=${DOD3_ELF:-elf/EBOOT_101.ELF};; esac

mkdir -p out/bench
log=out/bench/$tag.log; pad=out/bench/$tag.pad; grab=out/bench/$tag.grab; save=out/bench/save_$tag
rm -f "$pad" "$pad.stick" "$grab.ppm" "$grab.req"
rm -rf "$save"; mkdir -p "$save"
export PAD_STICK_FILE=$PWD/$pad.stick
export PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc DOD3_FPS=60
export PAD_FILE=$pad PS3RECOMP_FRAME_GRAB=$grab PS3RECOMP_METAL_FRAME_GRAB=$grab PS3_SAVEDATA_ROOT=$save
for kv in "$@"; do export "$kv"; done

upto=$secs
if [ -n "$PROFILE" ] && command -v sample >/dev/null 2>&1; then
  ( sleep $((secs - 25)); pid=$(pgrep -f "$bin" | head -1)
    [ -n "$pid" ] && sample "$pid" 10 -file "out/bench/$tag.prof.txt" >/dev/null 2>&1 ) &
  upto=$((secs - 40))
fi
keepawake=
command -v caffeinate >/dev/null 2>&1 && keepawake="caffeinate -i"
$keepawake "$bin" "$elf" > "$log" 2>&1 &
game=$!
bash "$root/tools/autoplay.sh" "$log" "$pad" "$grab" > "out/bench/$tag.autoplay.log" 2>&1 &
auto=$!
t0=$(date +%s)
while kill -0 $game 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt "$secs" ]; do sleep 5; done
kill $auto 2>/dev/null
if [ -n "$win" ]; then taskkill //IM "$(basename "$bin")" //F >/dev/null 2>&1; else kill $game 2>/dev/null; fi
wait 2>/dev/null
sleep 1
grep -q "gameplay (HUD up)" "out/bench/$tag.autoplay.log" || echo "$tag: WARNING -- autoplay never saw the battle HUD"
echo "== $tag: $(grep -c 'audio-gap\]' "$log") audio gaps logged"
"$python" -I "$root/tools/bench_summary.py" --from "${FROM:-100}" --to "$upto" "$log"
grep "\[frametime\]" "$log" | tail -3
