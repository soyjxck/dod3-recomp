#!/bin/zsh
# run_newgame.sh <secs> <log> [VAR=val ...] -- run_timed.sh plus tools/autoplay.sh:
# boots, skips the opening, presses through the title into a new game.
secs=$1; log=$2; shift 2
cd "$(dirname "$0")/.."
pad=out/pad_$$.txt
: > "$pad"
tools/autoplay.sh "$log" "$pad" &
ap=$!
tools/run_timed.sh "$secs" "$log" PAD_FILE="$pad" "$@"
kill $ap 2>/dev/null; rm -f "$pad"
