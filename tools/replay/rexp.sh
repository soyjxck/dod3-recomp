#!/bin/zsh
# rexp.sh <tag> [VAR=val ...]: replay the village capture with the given
# switches, write present 270, print its mean brightness.
tag=$1; shift
cd ~/Workspace/dod3-recomp; mkdir -p out/rexp
env "$@" build/ps3recomp_sdk/rsx_replay out/cap/village.rsxcap --out out/rexp/$tag --from 270 --every 100000 >/dev/null 2>&1
python3 - out/rexp/$tag.000270.ppm <<'PY'
import sys
d=open(sys.argv[1],'rb').read(); p=d.split(maxsplit=4); px=p[4]
print("%s mean=%.1f" % (sys.argv[1].split('/')[-1], sum(px[::97])/len(px[::97])))
PY
