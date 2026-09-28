"""Validate baseline/probe runs and separate GPU waiting from display queue work."""
import argparse
import csv
import re
from pathlib import Path
from statistics import median
from compare_render_runs import read


def trace(path):
    text,checks,quads,rows=read(path)
    for tag in ('PRESENT','PRESENT_CALLBACK'):
        for frame,body in re.findall(r'\['+tag+r'\] frame=(\d+) ([^\n]+)',text):
            rows.setdefault(int(frame),{}).update({k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',body)})
    return text,checks,quads,rows


def compare(baseline,probe,out):
    a,ac,aq,ar=trace(baseline);b,bc,bq,br=trace(probe)
    for tag in ('BUILD','CPU_BENCH'):
        assert re.findall(r'\['+tag+r'\] ([^\n]+)',a)==re.findall(r'\['+tag+r'\] ([^\n]+)',b),tag
    assert '[PRESENT_TRACE] wrapped=1 probe=0 ' in a
    assert '[PRESENT_TRACE] wrapped=1 probe=1 ' in b
    assert ac==bc and aq==bq
    pattern=r'\[DRAW_CHECK\] frame=(\d+) vertices=(\d+) crc=([0-9a-f]+)'
    da=re.findall(pattern,a);db=re.findall(pattern,b)
    assert da==db and {int(x[0]) for x in da}==set(ac)
    pattern=r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)'
    sa=re.findall(pattern,a);sb=re.findall(pattern,b)
    assert sa==sb
    assert all(int(m)==3 and int(s)>0 for f,m,s in sa if int(f)>=1200)
    assert '[CAPTURE] saved' not in a+b
    assert 'probe wait failed' not in a+b and 'queue failed' not in a+b
    # The process can exit after frame 1800's GPU completion but before its
    # display callback runs. Use the fixed complete callback windows, while
    # still requiring all 30 state/triangle/PRESENT checkpoints.
    assert all('queue' in ar[f] and br[f]['done_probe']==60 for f in ac)
    frames=list(range(1200,1800,60))
    assert all(ar[f]['incomplete']==br[f]['incomplete']==0 for f in frames)
    assert all(br[f]['done_probe']==60 and br[f]['probe']>0 for f in frames)
    print(f'PASS: {len(ac)} complete state, geometry, speed and final triangle checkpoints match')
    metrics=('frame','probe','heartbeat','queue','scene_to_callback','queue_to_callback','work','done_before','done_probe','done_queue','max_pending')
    for k in metrics:
        print(f'{k}: {median(ar[f][k] for f in frames):.3f} -> {median(br[f][k] for f in frames):.3f}')
    if out:
        with out.open('w',newline='') as fp:
            w=csv.writer(fp,lineterminator='\n')
            w.writerow(['frame',*[p+k for k in metrics for p in ('baseline_','probe_')],'matching_triangle_crc'])
            crc={int(f):c for f,_,c in da}
            for f in frames:w.writerow([f,*[r[f][k] for k in metrics for r in (ar,br)],crc[f]])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('baseline',type=Path);p.add_argument('probe',type=Path);p.add_argument('--csv',type=Path)
    args=p.parse_args();compare(args.baseline,args.probe,args.csv)
