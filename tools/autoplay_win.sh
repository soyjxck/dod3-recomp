#!/bin/bash
# autoplay_win.sh <log> <padfile> [grab-prefix] -- tools/autoplay.sh for Git
# Bash on Windows: drive a new game into the chapter 1 battle and keep it
# there. Same logic, same PAD_FILE presses (libs/input/cellPad.c); the grab
# prefix is the game's PS3RECOMP_FRAME_GRAB (the engine answers <prefix>.req
# with <prefix>.ppm) and tools/hud_visible.py decides whether the battle HUD
# is up. Masks: START 0x0008, CROSS 0x4000, CIRCLE 0x2000.
log=$1; pad=$2; grab=$3
here=$(cd "$(dirname "$0")" && pwd)
START=0x0008; CROSS=0x4000; CIRCLE=0x2000
press() { echo "$1" > "$pad"; echo "$(date +%H:%M:%S) [autoplay] press $1 ($2)"; }
wait_for() {   # wait_for <pattern> <timeout_s>
  for i in $(seq 1 "$2"); do grep -q -- "$1" "$log" 2>/dev/null && return 0; sleep 1; done
  return 1
}
hud() {        # 0 when the battle HUD is on screen
  [ -n "$grab" ] || return 1
  rm -f "$grab.ppm"; : > "$grab.req"
  for i in $(seq 1 50); do [ -f "$grab.ppm" ] && break; sleep 0.1; done
  [ -f "$grab.ppm" ] && python -I "$here/hud_visible.py" "$grab.ppm" >/dev/null
}
SQUARE=0x8000; TRIANGLE=0x1000
# AUTOPLAY_COMBAT=1 (with AUTOPLAY_FORWARD): in play, a ~4 s cycle of running,
# light and heavy attacks, camera swings and strafing instead of only holding
# forward -- effects, enemies and new views, where a player meets hitches the
# straight run never does. PAD_STICK_FILE takes "lx ly rx ry".
stick() { [ -n "$PAD_STICK_FILE" ] && echo "$1" > "$PAD_STICK_FILE"; }
tap() { echo "$1 6" > "$pad"; }
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
state=""
while :; do
  # AUTOPLAY_GRAB_UNTIL=<s>: from that many seconds after this script started,
  # stop checking the HUD (each check grabs a frame -- a GPU readback that
  # costs the game a ~9 ms hitch every ~3 s) and only hold the stick, so
  # the frame times after it are the game's own.
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
      # Hold the left stick forward (cellPad's PAD_STICK_FILE) instead of
      # attacking: running ahead reaches the scene where scenery breaks up.
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
