#!/bin/sh
# Run the port for N seconds, then SIGKILL it. The runtime handles SIGALRM
# itself, so an alarm-style timeout does not stop it -- a separate watchdog
# process does the kill.
#
#   tools/run_timed.sh <seconds> <log> [VAR=value ...]
secs=$1; log=$2; shift 2
cd "$(dirname "$0")/.." || exit 1
env PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc "$@" \
    ./build/dod3 elf/EBOOT.ELF > "$log" 2>&1 &
pid=$!
( perl -e "sleep $secs"; kill -9 "$pid" 2>/dev/null ) &
watchdog=$!
wait "$pid"
status=$?
kill "$watchdog" 2>/dev/null
exit $status
