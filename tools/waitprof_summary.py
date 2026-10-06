#!/usr/bin/env python3
# waitprof_summary.py <log> [from_t] [to_t]: per 5 s window of a PPU_WAITPROF
# run, fps and each main thread's busy milliseconds per frame (time not blocked
# in a wait call), then the medians across the windows.
import re,sys,statistics
WAITS=('sys_cond_wait','sys_event_queue_receive','sys_timer_usleep','sys_lwcond_wait',
       'sys_lwmutex_lock','cellSpursEventFlagWait','cellSpursQueuePopBody','sys_event_flag_wait',
       'sys_semaphore_wait','sys_mutex_lock','sys_timer_sleep','sys_lwcond_queue_wait','sys_ppu_thread_join')
lo=int(sys.argv[2]) if len(sys.argv)>2 else 87; hi=int(sys.argv[3]) if len(sys.argv)>3 else 10**9
rows=[]; cur=None
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[frametime\] t=(\d+)s ([\d.]+) fps',l)
    if m:
        if cur: rows.append(cur)
        cur={'t':int(m.group(1)),'fps':float(m.group(2)),'wait':{},'jc':0.0}; continue
    m=re.search(r'\[waitprof\] tid\s+(\d+) (.{24}) (\S+)\s.*?([\d.]+) ms/s',l)
    if m and cur is not None:
        tid=int(m.group(1)); call=m.group(3); ms=float(m.group(4))
        if call in WAITS: cur['wait'][tid]=cur['wait'].get(tid,0)+ms
        if call=='cellSpursRunJobChain' and tid==2: cur['jc']+=ms
if cur: rows.append(cur)
rows=[r for r in rows if lo<=r['t']<=hi and r['fps']>5 and not (29.5<=r['fps']<=30.5) and r['fps']<100]   # gameplay only
def busy(r,tid): return max(0.0,1000.0-r['wait'].get(tid,0.0))/r['fps']
print(' t    fps  main-busy  render-busy  render-jobchain  (ms per frame)')
for r in rows: print('%3d %6.1f %9.1f %11.1f %14.1f'%(r['t'],r['fps'],busy(r,1),busy(r,2),r['jc']/r['fps']))
if rows:
    med=lambda f: statistics.median(f(r) for r in rows)
    print('median %5.1f %9.1f %11.1f %14.1f'%(med(lambda r:r['fps']),med(lambda r:busy(r,1)),med(lambda r:busy(r,2)),med(lambda r:r['jc']/r['fps'])))
