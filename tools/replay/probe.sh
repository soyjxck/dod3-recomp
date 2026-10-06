#!/bin/zsh
# probe.sh <fp-hlsl hash> <msl expression> [x y frame]: replace the shader's
# colour output with <expr> (float4) and print the value the pixel pick reads
# right after that shader's draws touch the pixel.
h=$1; ex=$2; x=${3:-220}; y=${4:-430}; fr=${5:-270}
cd ~/Workspace/dod3-recomp; rm -rf out/ovr2; mkdir -p out/ovr2
python3 - "$h" "$ex" <<'PY'
import sys,re
h,ex=sys.argv[1],sys.argv[2]
s=open('out/repsh2/fp_%s.msl'%h).read()
s2=re.sub(r'(    out\._entryPointOutput_t0 = )select\(float4\(0\.0\), [^;]*\);', lambda m: m.group(1)+'float4('+ex+');', s, count=1)
assert s2!=s
open('out/ovr2/fp_%s.msl'%h,'w').write(s2)
PY
RSX_MSL_OVERRIDE=out/ovr2 RSX_PICK=$x,$y,$fr build/ps3recomp_sdk/rsx_replay out/cap/village.rsxcap 2>&1 | grep "\[pick\].*fp $h" | tail -1 | sed 's/.*-> /  /; s/ blend.*//'
