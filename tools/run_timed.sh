#!/bin/sh
# Run the port for N seconds, then SIGKILL it. The runtime handles SIGALRM
# itself, so an alarm-style timeout does not stop it.
#
#   tools/run_timed.sh <seconds> <log> [VAR=value ...]
secs=$1; log=$2; shift 2
cd "$(dirname "$0")/.." || exit 1
RUN_SECS=$secs env PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc "$@" \
    perl -e 'my $p = fork;
             if (!$p) { exec @ARGV or die "exec: $!" }
             $SIG{ALRM} = sub { kill 9, $p };
             alarm $ENV{RUN_SECS};
             waitpid($p, 0);
             exit($? >> 8 || $? & 127)' \
    ./build/dod3 elf/EBOOT.ELF > "$log" 2>&1
