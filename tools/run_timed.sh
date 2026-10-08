#!/bin/sh
# Run the port for N seconds, then SIGKILL it. The runtime handles SIGALRM
# itself, so an alarm-style timeout does not stop it -- a separate watchdog
# process does the kill.
#
#   tools/run_timed.sh <seconds> <log> [VAR=value ...]
#   DOD3_BIN=<path> picks the binary (default build/dod3), DOD3_ELF=<path>
#   the EBOOT (default elf/EBOOT.ELF; elf/EBOOT_101.ELF for a 1.01 build).
secs=$1; log=$2; shift 2
cd "$(dirname "$0")/.." || exit 1
env PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc "$@" \
    "${DOD3_BIN:-./build/dod3}" "${DOD3_ELF:-elf/EBOOT.ELF}" > "$log" 2>&1 &
pid=$!
( perl -e "sleep $secs"; kill -9 "$pid" 2>/dev/null ) &
watchdog=$!
wait "$pid"
status=$?
kill "$watchdog" 2>/dev/null
exit $status
