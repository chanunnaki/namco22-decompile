"""Compare original and buffered rendering on identical scripted driving frames."""
import argparse
import csv
from pathlib import Path
import re
from statistics import median
from compare_cpu_runs import read_run


def read(path):
    text=path.read_text();text=text[text.rfind('[BUILD]'):]
    _,checks,_,quads=read_run(path)
    rows={};render_frame=None;timing={};parallel=0;prep={}
    for line in text.splitlines():
        match=re.search(r'\[GXM_PREP\] frame=(\d+)',line)
        if match:
            render_frame=int(match[1]);rows.setdefault(render_frame,{}).update(prep)
        if '[PREP] ms/frame' in line:
            prep={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)}
        if '[BATCH_TIME]' in line:
            rows.setdefault(render_frame,{}).update({'batch_'+k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)})
        if '[SYNC]' in line:
            rows.setdefault(render_frame,{}).update({k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)})
        match=re.search(r'\[PIPELINE\] render work=([\d.]+)',line)
        if match:rows.setdefault(render_frame,{})['render']=float(match[1])
        match=re.search(r'\[DEVICES\] ms/frame parallel=([\d.]+)',line)
        if match:parallel=float(match[1])
        if '[TIMING]' in line:
            timing={k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)}
        match=re.search(r'\[CPU_CHECK\] frame=(\d+)',line)
        if match:
            rows.setdefault(int(match[1]),{})['frame']=timing['cpu']+parallel+timing['prepare']+timing['draw']
    return text,checks,quads,rows


def compare(old,new,out=None,geometry=False):
    a,ac,aq,ar=read(old);b,bc,bq,br=read(new)
    for tag in ('BUILD','CPU_BENCH'):
        assert re.findall(r'\['+tag+r'\] ([^\n]+)',a)==re.findall(r'\['+tag+r'\] ([^\n]+)',b), tag+' differs'
    if geometry:
        assert '[GEOMETRY] worker packets=0 indexed meshes=0' in a
        assert '[GEOMETRY] worker packets=1 indexed meshes=1' in b
        pattern=r'\[DRAW_CHECK\] frame=(\d+) vertices=(\d+) crc=([0-9a-f]+)'
        da={int(f):(int(n),crc) for f,n,crc in re.findall(pattern,a)}
        db={int(f):(int(n),crc) for f,n,crc in re.findall(pattern,b)}
        assert da==db and set(da)==set(ac), 'Final GPU triangle stream differs or checks are incomplete'
        print(f'PASS: {len(da)} final triangle counts and byte-stream checksums match')
    else:
        assert '[HUD_GPU] tilemap renderer enabled' not in a
    assert '[HUD_GPU] tilemap renderer enabled' in b and '[GPU_FENCE] buffered=1 notifications=1' in b
    assert '[CAPTURE] saved' not in a+b, 'Capture stalls invalidate timing comparisons'
    assert ac==bc and aq==bq, 'State, budget or geometry differs'
    speeds=[]
    for text in (a,b):
        speeds.append({int(f):(int(m),int(s)) for f,m,s in re.findall(r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)',text)})
    assert speeds[0]==speeds[1]
    frames=list(range(1200,1801,60))
    assert all(speeds[0][f][0]==3 and speeds[0][f][1]>0 for f in frames)
    print(f'PASS: {len(ac)} complete state/budget/geometry checkpoints and driving speeds match')
    # BATCH_TIME counts nonempty batches, so its windows do not align with
    # global frame checkpoints. Do not present those as paired measurements.
    metrics=('frame','render','geometry','sort') if geometry else ('frame','render','gpu_wait','layers_upload','hud_draw','scene_end')
    for k in metrics:
        before=median(ar[f][k] for f in frames);after=median(br[f][k] for f in frames)
        print(f'{k}: {before:.3f} -> {after:.3f} ms, {100*(1-after/before):.1f}% reduction')
    if out:
        with out.open('w',newline='') as fp:
            w=csv.writer(fp,lineterminator='\n')
            w.writerow(['frame','quads','speed_raw',*[p+k+'_ms' for k in metrics for p in ('old_','new_')],'matching_state_and_budget'])
            for f in frames:w.writerow([f,aq[f],speeds[0][f][1],*[r[f][k] for k in metrics for r in (ar,br)],ac[f]])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('legacy',type=Path);p.add_argument('native',type=Path);p.add_argument('--csv',type=Path)
    p.add_argument('--geometry',action='store_true')
    a=p.parse_args();compare(a.legacy,a.native,a.csv,a.geometry)
