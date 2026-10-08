#!/bin/zsh
# autoplay.sh <log> <padfile> [grab-prefix] -- drive a new game into the
# chapter 1 battle and keep it there.
#
# Presses go through PAD_FILE (libs/input/cellPad.c): each line written to
# <padfile> is one press ("<mask> [polls]"), consumed by the next pad poll.
# Masks: START 0x0008, CROSS 0x4000, CIRCLE 0x2000, UP 0x0010, DOWN 0x0040.
#
#   opening movie opened   -> START (skip it)
#   title music loaded     -> START, then CROSS every 3 s until the chapter
#                             loads: with no save that is two notices, the
#                             save slot list and New Game
#   chapter 1 loading      -> with a grab prefix (the game run with
#                             PS3RECOMP_METAL_FRAME_GRAB=<prefix>): look at
#                             the screen every few seconds. No battle HUD
#                             (tools/hud_visible.py) means a cutscene, a
#                             loading screen or a menu: START (a cutscene's
#                             skip prompt), CROSS (take it), CIRCLE x2 (back
#                             out of a pause menu START may have opened).
#                             HUD up means play: CROSS, which dismisses a
#                             tutorial pop-up and is otherwise an attack.
#                             Without a prefix: the old blind 30 s of skips.
# Fixed schedules missed whenever a load or a cutscene ran long; deciding from
# the log and the screen does not.
log=$1; pad=$2; grab=$3
here=${0:A:h}      # the script's directory ($0 inside a function is the function)
START=0x0008; CROSS=0x4000; CIRCLE=0x2000
press() { echo "$1" > "$pad"; echo "$(date +%H:%M:%S) [autoplay] press $1 ($2)"; }
nap() { perl -e "select(undef,undef,undef,$1)"; }
wait_for() {   # wait_for <pattern> <timeout_s>
  for i in $(seq 1 $2); do grep -q -- "$1" "$log" 2>/dev/null && return 0; nap 1; done
  return 1
}
hud() {        # 0 when the battle HUD is on screen
  rm -f "$grab.ppm"; : > "$grab.req"
  for i in $(seq 1 50); do [ -f "$grab.ppm" ] && break; nap 0.1; done
  [ -f "$grab.ppm" ] && python3 "$here/hud_visible.py" "$grab.ppm" >/dev/null
}

: > "$pad"
wait_for "D3_OPN_TEST.BIK" 400 || { echo "[autoplay] no opening movie seen"; exit 1; }
nap 3; press $START "skip opening"
wait_for "MUSIC_OTHER_TITLE_SCD.XXX" 120 || { echo "[autoplay] no title screen seen"; exit 1; }
nap 4; press $START "press start"
for i in $(seq 1 20); do
  nap 3
  grep -q "BG00_CHC_10_MAP.XXX" "$log" && break
  press $CROSS "title confirm #$i"
done
wait_for "BG00_CHC_10_MAP.XXX" 30 || { echo "[autoplay] new game NOT reached"; exit 1; }
echo "$(date +%H:%M:%S) [autoplay] chapter 1 loading"

if [ -z "$grab" ]; then
  for i in $(seq 1 15); do
    nap 1.4; press $START "skip cutscene #$i"
    nap 0.6; press $CROSS "confirm skip #$i"
  done
  exit 0
fi

# AUTOPLAY_FORWARD=1: in play, hold the left stick forward (cellPad's
# PAD_STICK_FILE, which tools/live_bench.sh points at <pad>.stick) instead of
# attacking -- running ahead reaches the scene where scenery breaks up, the
# heaviest in chapter 1. AUTOPLAY_COMBAT=1 (with it): a ~4 s cycle of
# running, light and heavy attacks, camera swings and strafing instead.
# AUTOPLAY_GRAB_UNTIL=<s>: from that many seconds after this script started,
# stop checking the HUD -- each check grabs a frame, a GPU readback that costs
# the game a hitch every ~3 s -- so the frame times after it are the game's
# own. As tools/autoplay_win.sh.
SQUARE=0x8000; TRIANGLE=0x1000
stick() { [ -n "$PAD_STICK_FILE" ] && echo "$1" > "$PAD_STICK_FILE"; }
tap() { echo "$1 6" > "$pad"; }
combat_cycle() {
  stick "128 0 128 128"; nap 0.8
  tap $SQUARE; nap 0.35; tap $SQUARE; nap 0.35; tap $SQUARE; nap 0.35
  tap $TRIANGLE; nap 0.6
  stick "128 0 235 128"; nap 0.7
  tap $SQUARE; nap 0.35; tap $TRIANGLE; nap 0.5
  stick "40 40 128 128"; nap 0.5
  stick "128 0 20 128"; nap 0.7
}
state=""
while :; do
  if [ -n "$AUTOPLAY_GRAB_UNTIL" ] && [ "$SECONDS" -ge "$AUTOPLAY_GRAB_UNTIL" ]; then
    [ "$state" != hold ] && { state=hold; echo "$(date +%H:%M:%S) [autoplay] holding forward, no more grabs"; }
    if [ -n "$AUTOPLAY_COMBAT" ]; then combat_cycle; continue; fi
    [ -n "$PAD_STICK_FILE" ] && : > "$PAD_STICK_FILE"
    nap 3
    continue
  fi
  if hud; then
    [ "$state" != play ] && { state=play; echo "$(date +%H:%M:%S) [autoplay] gameplay (HUD up)"; }
    if [ -n "$AUTOPLAY_FORWARD" ] && [ -n "$PAD_STICK_FILE" ]; then
      if [ -n "$AUTOPLAY_COMBAT" ]; then combat_cycle; else : > "$PAD_STICK_FILE"; nap 3; fi
    else
      press $CROSS "in play"
      nap 3
    fi
  else
    [ -n "$PAD_STICK_FILE" ] && rm -f "$PAD_STICK_FILE"
    [ "$state" != skip ] && { state=skip; echo "$(date +%H:%M:%S) [autoplay] no HUD -- skipping"; }
    press $START "skip"; nap 1
    press $CROSS "take skip"; nap 1
    press $CIRCLE "back"; nap 0.5
    press $CIRCLE "back"; nap 1
  fi
done
