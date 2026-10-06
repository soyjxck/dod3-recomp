#!/bin/zsh
# live_bench.sh <tag> <secs> [VAR=val ...]: the scripted new-game run into the
# chapter 1 battle (out/run_snap.sh), a 10 s `sample` of the process 25 s
# before the end, and a summary of the [frametime] lines from the battle:
# t >= FROM (default 87) and before the sample, which stalls the process.
tag=$1; secs=$2; shift 2
cd ~/Workspace/dod3-recomp
# The title menu opens on New Game or Continue depending on how far its save
# scan has got, so a run sometimes loads another chapter instead of the
# chapter 1 battle (BG00_CHC_10). Such a run is retried, up to three times.
for attempt in 1 2 3; do
  ( perl -e "sleep $((secs - 25))"; pid=$(pgrep -f "^./build/dod3" | head -1)
    [ -n "$pid" ] && sample $pid 10 -file out/$tag.prof.txt >/dev/null 2>&1 ) &
  out/run_snap.sh $secs $tag "$@" >/dev/null 2>&1
  # Gameplay windows: cutscenes hold exactly 30 fps and loading screens ~120,
  # so a window outside both is play. Ten of them make a usable run.
  n=$(python3 tools/battle_windows.py out/$tag.log ${FROM:-87} $((secs - 40)) | wc -l)
  grep -q "BG00_CHC_10_MAP" out/$tag.log && [ "$n" -ge 10 ] && break
  echo "$tag: attempt $attempt has $n gameplay windows -- retrying"
done
wait
echo "$tag: $(cat out/$tag.status)"
python3 - out/$tag.log ${FROM:-87} $((secs - 40)) <<'PY'
import re,sys
rows=[]
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[frametime\] t=(\d+)s ([\d.]+) fps, mean ([\d.]+) ms, worst ([\d.]+) ms, (\d+) of (\d+)',l)
    if m and int(sys.argv[2]) <= int(m.group(1)) <= int(sys.argv[3]) and \
       not (29.5 <= float(m.group(2)) <= 30.5) and float(m.group(2)) < 100:
        rows.append([float(x) for x in m.groups()])
if not rows: print('  no battle frametime lines'); sys.exit()
frames=sum(r[5] for r in rows); span=sum(r[5]*r[2] for r in rows)/1000
print('  %d windows, %.1f fps avg, worst-frame median %.0f ms, max %.0f ms, %.1f%% of frames over 34 ms'%(
    len(rows), frames/span, sorted(r[3] for r in rows)[len(rows)//2], max(r[3] for r in rows), 100*sum(r[4] for r in rows)/frames))
print('  fps per window:', ' '.join('%.0f'%r[1] for r in rows))
PY
