import sys
f=sys.argv[1]; x0,y0,x1,y1=map(int,sys.argv[2:6])
d=open(f,'rb').read(); p=d.split(maxsplit=4); w=int(p[1]); px=p[4]
s=[0,0,0]; n=0
for y in range(y0,y1,2):
    for x in range(x0,x1,2):
        i=(y*w+x)*3; s[0]+=px[i]; s[1]+=px[i+1]; s[2]+=px[i+2]; n+=1
print("%s region mean=(%.0f %.0f %.0f)" % (f.split('/')[-1], s[0]/n, s[1]/n, s[2]/n))
