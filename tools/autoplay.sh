#!/bin/zsh
# autoplay.sh <log> <padfile> -- press through the intro into a new game.
#
# Drives the title through PAD_FILE (libs/input/cellPad.c): each line written
# to <padfile> is one press ("<mask> [polls]"), consumed by the next pad poll.
# Decisions are made from the log, because a fixed schedule misses whenever a
# load takes longer:
#   opening movie opened        -> START   (skip it)
#   title music loaded          -> START   (press start), then CROSS x3
#                                  (New Game is the default item, confirms)
#   first chapter map loading   -> START x2 after a pause (skip the first cutscene)
# Masks: START 0x0008, CROSS 0x4000, CIRCLE 0x2000, UP 0x0010, DOWN 0x0040.
log=$1; pad=$2
press() { echo "$1" > "$pad"; echo "[autoplay] press $1 ($2)"; }
wait_for() {   # wait_for <pattern> <timeout_s>
  for i in $(seq 1 $2); do grep -q -- "$1" "$log" 2>/dev/null && return 0; perl -e 'sleep 1'; done
  return 1
}
: > "$pad"
wait_for "D3_OPN_TEST.BIK" 400 || { echo "[autoplay] no opening movie seen"; exit 1; }
perl -e 'sleep 3'; press 0x0008 "skip opening"
wait_for "MUSIC_OTHER_TITLE_SCD.XXX" 120 || { echo "[autoplay] no title screen seen"; exit 1; }
perl -e 'sleep 4'; press 0x0008 "press start"
for i in 1 2 3 4; do
  perl -e 'sleep 4'
  grep -q "BG00_CHC_10_MAP.XXX" "$log" && break
  press 0x4000 "confirm #$i"
done
wait_for "BG00_CHC_10_MAP.XXX" 60 && echo "[autoplay] new game loading" || { echo "[autoplay] new game NOT reached"; exit 1; }
# The chapter opens on a cutscene; START skips it once the level is in.
perl -e 'sleep 12'; press 0x0008 "skip first cutscene"
perl -e 'sleep 4';  press 0x0008 "skip first cutscene (again)"
