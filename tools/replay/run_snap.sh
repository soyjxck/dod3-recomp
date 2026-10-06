#!/bin/zsh
# run_snap.sh <secs> <tag> [VAR=val ...]: a capture_freeze run from a fresh copy
# of the snapshot save in out/save_snap, never the player's own save data.
secs=$1; tag=$2; shift 2
cd ~/Workspace/dod3-recomp
rm -rf out/save_test; mkdir -p out/save_test; cp -R out/save_snap/* out/save_test/
caffeinate -i tools/capture_freeze.sh $secs $tag PS3_SAVEDATA_ROOT=out/save_test "$@"
