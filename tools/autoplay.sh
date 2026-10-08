#!/bin/bash
# autoplay.sh <log> <padfile> [grab-prefix] -- drive a new game into the
# chapter 1 battle and keep it there, hands off. Bash on macOS, Git Bash on
# Windows.
#
# Presses go through PAD_FILE (libs/input/cellPad.c): each line written to
# <padfile> is one press ("<mask> [polls]"), consumed by the next pad poll.
# Masks: START 0x0008, CROSS 0x4000, CIRCLE 0x2000, SQUARE 0x8000,
# TRIANGLE 0x1000.
#
#   opening movie opened   -> START (skip it; none with DOD3_SKIP_INTRO)
#   title music loaded     -> START, then CROSS every 3 s until the chapter
#                             loads: with no save that is two notices, the
#                             save slot list and New Game
#   chapter 1 loading      -> with a grab prefix (the game run with
#                             PS3RECOMP_FRAME_GRAB=<prefix>): look at the
#                             screen every few seconds. No battle HUD
#                             (tools/hud_visible.py) means a cutscene, a
#                             loading screen or a menu: START (a cutscene's
#                             skip prompt), CROSS (take it), CIRCLE x2 (back
#                             out of a pause menu START may have opened).
#                             HUD up means play: CROSS, which dismisses a
#                             tutorial pop-up and is otherwise an attack.
#                             Without a prefix: 30 s of blind skips.
#
# In play, from the environment:
#   AUTOPLAY_FORWARD=1      hold the left stick forward (cellPad's
#                           PAD_STICK_FILE) instead of attacking: running
#                           ahead reaches the scene where scenery breaks up,
#                           the heaviest in chapter 1
#   AUTOPLAY_COMBAT=1       with it: a ~4 s cycle of running, light and heavy
#                           attacks, camera swings and strafing
#   AUTOPLAY_GRAB_UNTIL=<s> from that many seconds after the start, stop
#                           checking the HUD (each check grabs a frame, a GPU
#                           readback that costs the game a hitch), so the
#                           frame times after it are the game's own
log=$1; pad=$2; grab=$3
[ -n "$log" ] && [ -n "$pad" ] || { echo "usage: $0 <log> <padfile> [grab-prefix]"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
python=${PYTHON:-python3}
"$python" -c "import sys" >/dev/null 2>&1 || python=python   # a python3 that is only the Microsoft Store alias
START=0x0008; CROSS=0x4000; CIRCLE=0x2000; SQUARE=0x8000; TRIANGLE=0x1000
press() { echo "$1" > "$pad"; echo "$(date +%H:%M:%S) [autoplay] press $1 ($2)"; }
tap() { echo "$1 6" > "$pad"; }
stick() { [ -n "$PAD_STICK_FILE" ] && echo "$1" > "$PAD_STICK_FILE"; }
wait_for() {   # wait_for <pattern> <timeout_s>
  for i in $(seq 1 "$2"); do grep -q -- "$1" "$log" 2>/dev/null && return 0; sleep 1; done
  return 1
}
hud() {        # 0 when the battle HUD is on screen
  [ -n "$grab" ] || return 1
  rm -f "$grab.ppm"; : > "$grab.req"
  for i in $(seq 1 50); do [ -f "$grab.ppm" ] && break; sleep 0.1; done
  [ -f "$grab.ppm" ] && "$python" -I "$here/hud_visible.py" "$grab.ppm" >/dev/null
}
combat_cycle() {
  stick "128 0 128 128"; sleep 0.8
  tap $SQUARE; sleep 0.35; tap $SQUARE; sleep 0.35; tap $SQUARE; sleep 0.35
  tap $TRIANGLE; sleep 0.6
  stick "128 0 235 128"; sleep 0.7
  tap $SQUARE; sleep 0.35; tap $TRIANGLE; sleep 0.5
  stick "40 40 128 128"; sleep 0.5
  stick "128 0 20 128"; sleep 0.7
}

: > "$pad"
# The opening movie, unless the title skips it (DOD3_SKIP_INTRO, on by
# default): then the title screen comes first.
for i in $(seq 1 400); do
  grep -q "MUSIC_OTHER_TITLE_SCD.XXX" "$log" 2>/dev/null && break
  if grep -q "D3_OPN_TEST.BIK" "$log" 2>/dev/null; then sleep 3; press $START "skip opening"; break; fi
  sleep 1
done
wait_for "MUSIC_OTHER_TITLE_SCD.XXX" 120 || { echo "[autoplay] no title screen seen"; exit 1; }
sleep 4; press $START "press start"
for i in $(seq 1 20); do
  sleep 3
  grep -q "BG00_CHC_10_MAP.XXX" "$log" && break
  press $CROSS "title confirm #$i"
done
wait_for "BG00_CHC_10_MAP.XXX" 30 || { echo "[autoplay] new game NOT reached"; exit 1; }
echo "$(date +%H:%M:%S) [autoplay] chapter 1 loading"

if [ -z "$grab" ]; then
  for i in $(seq 1 15); do
    sleep 1.4; press $START "skip cutscene #$i"
    sleep 0.6; press $CROSS "confirm skip #$i"
  done
  exit 0
fi

state=""
while :; do
  if [ -n "$AUTOPLAY_GRAB_UNTIL" ] && [ "$SECONDS" -ge "$AUTOPLAY_GRAB_UNTIL" ]; then
    [ "$state" != hold ] && { state=hold; echo "$(date +%H:%M:%S) [autoplay] holding forward, no more grabs"; }
    if [ -n "$AUTOPLAY_COMBAT" ]; then combat_cycle; continue; fi
    [ -n "$PAD_STICK_FILE" ] && : > "$PAD_STICK_FILE"
    sleep 3
    continue
  fi
  if hud; then
    [ "$state" != play ] && { state=play; echo "$(date +%H:%M:%S) [autoplay] gameplay (HUD up)"; }
    if [ -n "$AUTOPLAY_FORWARD" ] && [ -n "$PAD_STICK_FILE" ]; then
      if [ -n "$AUTOPLAY_COMBAT" ]; then combat_cycle; else : > "$PAD_STICK_FILE"; sleep 3; fi
    else
      press $CROSS "in play"
      sleep 3
    fi
  else
    [ -n "$PAD_STICK_FILE" ] && rm -f "$PAD_STICK_FILE"
    [ "$state" != skip ] && { state=skip; echo "$(date +%H:%M:%S) [autoplay] no HUD -- skipping"; }
    press $START "skip"; sleep 1
    press $CROSS "take skip"; sleep 1
    press $CIRCLE "back"; sleep 0.5
    press $CIRCLE "back"; sleep 1
  fi
done
