#!/bin/zsh
# capture_freeze.sh <secs> <tag> [VAR=val ...] -- tools/run_newgame.sh, plus a
# 3 s `sample` of the process once the frame counter has stopped for 8 s.
#   out/<tag>.log         the run log (needs DOD3_GCM_WATCH=1, set here)
#   out/<tag>.sample.txt  stacks of every thread at the freeze
#   out/<tag>.status      "frozen at flips=N" or "no freeze"
secs=$1; tag=$2; shift 2
cd "$(dirname "$0")/.."
log=out/$tag.log; smp=out/$tag.sample.txt; st=out/$tag.status
rm -f "$log" "$smp" "$st"
echo "no freeze" > "$st"
(
  for i in $(seq 1 $secs); do
    perl -e 'sleep 1'
    # Eight identical readings: a level load pauses the flip counter for a
    # few seconds and a 3-reading test sampled that instead of the freeze.
    set -- $(grep -o "flips=[0-9]*" "$log" 2>/dev/null | tail -8 | tr '\n' ' ')
    if [ -n "$8" ] && [ "$1" = "$8" ] && [ "$1" != "flips=0" ]; then
      pid=$(pgrep -f "dod3 elf/EBOOT.ELF" | head -1)
      [ -n "$pid" ] && sample "$pid" 3 -mayDie -file "$smp" >/dev/null 2>&1
      echo "frozen at $1" > "$st"
      break
    fi
  done
) >/dev/null 2>&1 &
watcher=$!
tools/run_newgame.sh "$secs" "$log" DOD3_GCM_WATCH=1 "$@" >/dev/null 2>&1
kill $watcher 2>/dev/null
cat "$st"
