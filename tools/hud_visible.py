#!/usr/bin/env python3
# hud_visible.py <frame.ppm>...: is the battle HUD on screen? Counts bright
# green pixels where the health gauge's left end sits (x 210-300, y 84-97 of a
# 1280x720 frame); prints "<count> HUD|-" per frame, exit 0 if the last has it.
import sys
def load(f):
    d=open(f,'rb').read(); i=0; tok=[]
    while len(tok)<4:
        while d[i:i+1].isspace(): i+=1
        j=i
        while not d[j:j+1].isspace(): j+=1
        tok.append(d[i:j]); i=j
    i+=1; w,h=int(tok[1]),int(tok[2]); return w,h,d[i:i+w*h*3]
ok=False
for f in sys.argv[1:]:
    w,h,px=load(f); n=0
    for y in range(84*h//720, 97*h//720+1):
        for x in range(210*w//1280, 300*w//1280):
            o=(y*w+x)*3; r,g,b=px[o],px[o+1],px[o+2]
            if g>170 and g>r+50 and b>100: n+=1
    ok = n >= 40
    print(n, 'HUD' if ok else '-', f)
sys.exit(0 if ok else 1)
