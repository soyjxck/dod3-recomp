#!/bin/zsh
# bench.sh [captures...]: replay each capture 3 times, print the best
# user+sys CPU seconds and wall seconds (default: shadows village outside).
cd ~/Workspace/dod3-recomp
caps=("$@"); (( $#caps )) || caps=(shadows village outside)
for c in $caps; do
  best_cpu=999; best_real=999
  for i in 1 2 3; do
    t=$(/usr/bin/time -p sh -c "RSX_FP_SAT_ALPHA=1a9b74dc1afc2a84 build/ps3recomp_sdk/rsx_replay out/cap/$c.rsxcap >/dev/null 2>&1" 2>&1)
    real=$(echo "$t" | awk '/^real/{print $2}'); cpu=$(echo "$t" | awk '/^user/{u=$2} /^sys/{s=$2} END{print u+s}')
    (( cpu < best_cpu )) && best_cpu=$cpu
    (( real < best_real )) && best_real=$real
  done
  printf "%-10s cpu %.2fs  real %.2fs\n" $c $best_cpu $best_real
done
