"""Validate simulation-only and frozen-scene Vita isolation logs."""
import argparse
import csv
from pathlib import Path
import re
from statistics import median
from compare_cpu_runs import read_run

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('baseline', type=Path)
p.add_argument('simulation', type=Path)
p.add_argument('replay', type=Path)
p.add_argument('--csv', type=Path)
a = p.parse_args()
texts = [x.read_text() for x in (a.baseline, a.simulation, a.replay)]
texts = [s[s.rfind('[BUILD]'):] for s in texts]
checks = [read_run(x)[1] for x in (a.baseline, a.simulation, a.replay)]
assert checks[0] == checks[1] == checks[2], 'Guest state/budget mismatch'
for s in texts:
    assert '[RUN_END] frame=1800 traps=0 host_closed=1' in s
    assert '[SOUND_BYPASS] enabled=0' in s
    assert '[CAPTURE] saved' not in s
    assert '[PRESENT_TRACE] wrapped=1 probe=0 ' in s
spec = r'\[CPU_BENCH\] ([^\n]+)'
assert len({tuple(re.findall(spec, s)) for s in texts}) == 1
race = r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)'
races = [re.findall(race, s) for s in texts]
assert races[0] == races[1] == races[2]
assert all(int(mode)==3 and int(speed)>0 for f,mode,speed in races[1] if int(f)>=1200)
assert '[ISOLATION] mode=1 sound_enabled=1' in texts[1]
assert int(re.search(r'simulation main=USER_0 affinity_result=(-?\d+)',texts[1])[1]) >= 0
assert '[DRAW_CHECK]' not in texts[1] and '[PIPELINE] render work=' not in texts[1]
assert '[SOUND_WORKER]' in texts[1]
assert '[ISOLATION] mode=2 sound_enabled=1' in texts[2]
wall = [{int(f): float(ms) for f,ms in re.findall(r'\[ISOLATION_TICKS\] frame=(\d+) mode=\d+ ms_per_tick=([\d.]+)',s)} for s in texts[:2]]
frames = list(range(1200,1801,60))
print('PASS: all 30 complete guest state/budget and driving checkpoints match in all three modes')
for name, rows in zip(('normal','simulation-only'),wall):
    vals=[rows[f] for f in frames]
    print(f'{name}: median {median(vals):.3f} ms/tick, {1000/median(vals):.1f} ticks/s; window range {min(vals):.3f}..{max(vals):.3f} ms')
results=[]
for body in re.findall(r'\[REPLAY_RESULT\] ([^\n]+)',texts[2]):
    row=dict(re.findall(r'(\w+)=([\w.]+)',body))
    assert row['frames']=='180' and row['unchanged']=='1'
    results.append(row)
assert {(r['source'],r['cached_geometry']) for r in results} == {(str(f),str(c)) for f in (600,1200,1740) for c in (0,1)}
assert len(results)==6
for r in results: print('replay:',r)
# Repeated draws must have the source scene's identical final vertex stream.
base_draw={int(f):(n,crc) for f,n,crc in re.findall(r'\[DRAW_CHECK\] frame=(\d+) vertices=(\d+) crc=(\w+)',texts[0])}
for source,chunk in re.findall(r'\[REPLAY_BEGIN\] source=(\d+)\n(.*?)(?=\[REPLAY_BEGIN\]|\Z)',texts[2],re.S):
    chunk=chunk[:chunk.rfind('[REPLAY_RESULT]')]
    draws=re.findall(r'\[DRAW_CHECK\] frame=\d+ vertices=(\d+) crc=(\w+)',chunk)
    assert draws and all(x==base_draw[int(source)] for x in draws), 'Frozen scene triangles changed'
print('PASS: frozen replay triangle streams match their normal-run source scenes')
if a.csv:
    with a.csv.open('w',newline='') as fp:
        w=csv.writer(fp);w.writerow(['test','frame','ms','prepare_ms','draw_ms','present_ms'])
        for f in frames:
            for name,rows in zip(('normal','simulation'),wall):w.writerow([name,f,rows[f],'','',''])
        for r in results:w.writerow(['replay_cached' if r['cached_geometry']=='1' else 'replay_full',r['source'],r['total_ms'],r['prepare_ms'],r['draw_ms'],r['present_ms']])
