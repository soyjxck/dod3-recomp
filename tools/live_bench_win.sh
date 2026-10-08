#!/bin/bash
# live_bench_win.sh <tag> <seconds> [VAR=val ...] -- tools/live_bench.sh for
# Git Bash: a hands-off run of the chapter 1 battle, with the frame times for
# t >= 100 s summarised at the end. Autoplay (tools/autoplay_win.sh) starts
# a new game, skips the cutscenes until the HUD is up, then keeps attacking.
# Extra VAR=val arguments go into the game's environment (DOD3_PROF=2,
# AUDIO_GAPS=1, RSX_VSYNC=0, ...). Output: out/bench/<tag>.log,
# out/bench/<tag>.autoplay.log, and the summary on stdout.
#
# Run from the repo root. Refuses to start while another dod3.exe runs.
tag=$1; secs=$2; shift 2
[ -n "$tag" ] && [ -n "$secs" ] || { echo "usage: $0 <tag> <seconds> [VAR=val ...]"; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root" || exit 1
if tasklist 2>/dev/null | grep -qi dod3.exe; then echo "a dod3.exe is already running; not starting another"; exit 1; fi
mkdir -p out/bench
log=out/bench/$tag.log; pad=out/bench/$tag.pad; grab=out/bench/$tag.grab
rm -f "$pad" "$grab.ppm" "$grab.req" "$pad.stick"
# AUTOPLAY_FORWARD=1 (as a VAR=val argument): hold forward in play instead of
# attacking -- the game reads the hold from this file (cellPad PAD_STICK_FILE).
export PAD_STICK_FILE=$pad.stick
export PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc DOD3_FPS=60
export PAD_FILE=$pad PS3RECOMP_FRAME_GRAB=$grab
for kv in "$@"; do export "$kv"; done
./${DOD3_BUILD:-build}/dod3.exe ${DOD3_ELF:-elf/EBOOT.ELF} > "$log" 2>&1 &
game=$!
bash "$root/tools/autoplay_win.sh" "$log" "$pad" "$grab" > "out/bench/$tag.autoplay.log" 2>&1 &
auto=$!
t0=$(date +%s)
while kill -0 $game 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt "$secs" ]; do sleep 5; done
kill $auto 2>/dev/null
taskkill //IM dod3.exe //F >/dev/null 2>&1
sleep 1
echo "== $tag: $(grep -c '\[autoplay\] gameplay' "out/bench/$tag.autoplay.log") time(s) the HUD came up; $(grep -c 'audio-gap\]' "$log") audio gaps logged"
# Frame-time summary from the engine's 5 s lines, for t >= 100 s.
awk '/\[frametime\] t=/ { t=$2; sub("t=","",t); sub("s","",t); if (t+0 < 100) next;
       n++; sumfps+=$3; summ+=$6; if ($9+0 > worst) worst=$9; over+=$11; frames+=$13 }
     END { if (n) printf("   t>=100s: %.1f fps mean, %.1f ms mean frame, worst %.0f ms, %.2f%% of %d frames over 34 ms (%d windows)\n",
                         sumfps/n, summ/n, worst, 100.0*over/frames, frames, n);
           else print "   no frametime lines past t=100s" }' "$log"
grep "\[frametime\]" "$log" | tail -3
