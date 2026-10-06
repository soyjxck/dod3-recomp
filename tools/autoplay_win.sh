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
: > "$pad"
wait_for "D3_OPN_TEST.BIK" 400 || { echo "[autoplay] no opening movie seen"; exit 1; }
sleep 3; press $START "skip opening"
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
  if hud; then
    [ "$state" != play ] && { state=play; echo "$(date +%H:%M:%S) [autoplay] gameplay (HUD up)"; }
    press $CROSS "in play"
    sleep 3
  else
    [ "$state" != skip ] && { state=skip; echo "$(date +%H:%M:%S) [autoplay] no HUD -- skipping"; }
    press $START "skip"; sleep 1
    press $CROSS "take skip"; sleep 1
    press $CIRCLE "back"; sleep 0.5
    press $CIRCLE "back"; sleep 1
  fi
done
