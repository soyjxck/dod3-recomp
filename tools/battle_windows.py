#!/usr/bin/env python3
# battle_windows.py <log> [from_t] [to_t]: the [frametime] windows that are
# gameplay -- not a cutscene (held at exactly 30 fps) or a loading screen
# (~120 fps) -- one "t fps" line each.
import re,sys
lo=int(sys.argv[2]) if len(sys.argv)>2 else 87; hi=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[frametime\] t=(\d+)s ([\d.]+) fps',l)
    if not m: continue
    t,f=int(m.group(1)),float(m.group(2))
    if lo<=t<=hi and not (29.5<=f<=30.5) and f<100: print(t,f)
