"""Measure full sound bypass; shared RAM can legitimately differ without its MCU."""
import sys,re,csv
from pathlib import Path
from statistics import median
from compare_render_runs import read
if len(sys.argv) != 4: raise SystemExit('usage: compare_sound_runs.py SOUND_ON.log SOUND_OFF.log OUT.csv')
paths=[Path(x) for x in sys.argv[1:3]]
runs=[read(p) for p in paths]
for n,(s,c,q,r) in enumerate(runs):
 assert '[SOUND_BYPASS] enabled='+str(n) in s
 assert '[RUN_END] frame=1800 traps=0 host_closed=1' in s
 # Initial boot capture is outside the measured windows; reject later stalls.
 assert all(int(f)<1140 for f in re.findall(r'\[CAPTURE\] saved frame (\d+)',s))
 assert '[PRESENT_TRACE] wrapped=1 probe=0 ' in s
 if n:assert '[SOUND_WORKER]' not in s and '[MAIN] Calling rr_sound_init' not in s and '[MAIN] Calling rr_audio_init' not in s
 timings={};device={}
 for l in s.splitlines():
  if '[TIMING]' in l:timings={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',l)}
  if '[DEVICES]' in l:device={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',l)}
  m=re.search(r'\[CPU_CHECK\] frame=(\d+)',l)
  if m:r[int(m[1])].update(timings,**device)
 if n:assert all(x['sound']==x['mix']==x['wake']==0 for f,x in r.items() if isinstance(f,int) and 'sound' in x)
a,b=[x[0] for x in runs]
for tag in ('BUILD','CPU_BENCH'):assert re.findall(r'\['+tag+r'\] ([^\n]+)',a)==re.findall(r'\['+tag+r'\] ([^\n]+)',b)
for name,pat in [('scene',r'\[DRAW_CHECK\] frame=(\d+) vertices=(\d+) crc=([0-9a-f]+)'),('speed',r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)')]:
 x,y=[re.findall(pat,s) for s in (a,b)]
 print(name,'matching',sum(i==j for i,j in zip(x,y)),'of',len(x),len(y))
 assert x==y and len(x)==30, name+' checkpoints differ'
 if name=='speed':assert all(int(m)==3 and int(v)>0 for f,m,v in x if int(f)>=1200)
print('state equality:',runs[0][1]==runs[1][1])
frames=list(range(1200,1801,60));metrics=('frame','render','parallel','cpu','dsp','sound','mix','wake','join','prepare')
for k in metrics:
 vals=[median(x[3][f][k] for f in frames) for x in runs];print(k,vals)
with open(sys.argv[3],'w',newline='') as fp:
 w=csv.writer(fp);w.writerow(['frame',*[p+k+'_ms' for k in metrics for p in ('on_','off_')]])
 for f in frames:w.writerow([f,*[x[3][f][k] for k in metrics for x in runs]])
