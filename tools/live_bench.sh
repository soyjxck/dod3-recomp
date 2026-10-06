#!/bin/zsh
# live_bench.sh <tag> <secs> [VAR=val ...]: a hands-off run of the chapter 1
# battle and a summary of its [frametime] lines from gameplay.
#
# No save data, so the title goes to New Game; tools/autoplay.sh skips every
# cutscene until the battle HUD is on screen (it asks the game for screenshots
# through PS3RECOMP_METAL_FRAME_GRAB) and then keeps Zero fighting. Gameplay
# starts about 90 s after the first frame; the summary covers t >= FROM
# (default 100) to the end.
#   PROFILE=1: also a 10 s `sample` of the process 25 s before the end (it
#   stalls the game, so the summary stops 40 s before the end).
#   out/<tag>.log, out/<tag>.autoplay.txt, out/<tag>.prof.txt
tag=$1; secs=$2; shift 2
cd ~/Workspace/dod3-recomp
rm -rf out/save_bench; rm -f out/$tag.grab.*(N); mkdir -p out/save_bench
pad=out/$tag.pad; : > $pad
tools/autoplay.sh out/$tag.log $pad out/$tag.grab > out/$tag.autoplay.txt 2>&1 &
ap=$!
upto=$secs
if [ -n "$PROFILE" ]; then
  ( perl -e "sleep $((secs - 25))"; pid=$(pgrep -f "^./build/dod3" | head -1)
    [ -n "$pid" ] && sample $pid 10 -file out/$tag.prof.txt >/dev/null 2>&1 ) &
  upto=$((secs - 40))
fi
caffeinate -i tools/run_timed.sh $secs out/$tag.log PAD_FILE=$pad PS3_SAVEDATA_ROOT=out/save_bench \
    PS3RECOMP_METAL_FRAME_GRAB=out/$tag.grab "$@"
kill $ap 2>/dev/null; wait 2>/dev/null
grep -q "gameplay (HUD up)" out/$tag.autoplay.txt || echo "$tag: WARNING -- autoplay never saw the battle HUD"
python3 - out/$tag.log ${FROM:-100} $upto $tag <<'PY'
import re,sys
rows=[]
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[frametime\] t=(\d+)s ([\d.]+) fps, mean ([\d.]+) ms, worst ([\d.]+) ms, (\d+) of (\d+)',l)
    if m and int(sys.argv[2]) <= int(m.group(1)) <= int(sys.argv[3]): rows.append([float(x) for x in m.groups()])
if not rows: print(sys.argv[4]+': no gameplay frametime lines'); sys.exit()
frames=sum(r[5] for r in rows); span=sum(r[5]*r[2] for r in rows)/1000
w=sorted(r[3] for r in rows)
print('%s: %d windows, %.1f fps avg, %.1f%% of frames over 34 ms, worst frame median %.0f ms / max %.0f ms'%(
    sys.argv[4], len(rows), frames/span, 100*sum(r[4] for r in rows)/frames, w[len(w)//2], w[-1]))
print('  fps per window:', ' '.join('%.0f'%r[1] for r in rows))
PY
