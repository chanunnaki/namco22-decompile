"""Compare decoded HUD/scene textures on identical scripted driving frames."""
import argparse,csv,re
from pathlib import Path
from statistics import median
from compare_render_runs import read

def compare(old,new,out=None,hud_only=False):
    a,ac,aq,ar=read(old);b,bc,bq,br=read(new)
    for tag in ('BUILD','CPU_BENCH'):
        assert re.findall(r'\['+tag+r'\] ([^\n]+)',a)==re.findall(r'\['+tag+r'\] ([^\n]+)',b),tag
    assert '[HUD_GPU] tilemap renderer enabled' in a and '[HUD_ATLAS] cached glyph renderer enabled' in b
    if not hud_only:
        assert '[NATIVE_BANK] enabled=0' in a and '[NATIVE_BANK] enabled=1' in b
        assert '[NATIVE_BANK] resident=' in b
    for text in (a,b):
        assert '[RUN_END] frame=1800 traps=0 host_closed=1' in text
        assert '[PRESENT_TRACE] wrapped=1 probe=0 ' in text
        assert '[CAPTURE] saved' not in text
    assert ac==bc and aq==bq
    pattern=r'\[DRAW_CHECK\] frame=(\d+) vertices=(\d+) crc=([0-9a-f]+)'
    da=re.findall(pattern,a);db=re.findall(pattern,b)
    assert da==db and {int(x[0]) for x in da}==set(ac)
    pattern=r'\[BENCH_RACE\] frame=(\d+) mode=(\d+) menu=\d+ speed=(\d+)'
    sa=re.findall(pattern,a);sb=re.findall(pattern,b);assert sa==sb
    assert all(int(m)==3 and int(s)>0 for f,m,s in sa if int(f)>=1200)
    for text,rows in ((a,ar),(b,br)):
        for frame,body in re.findall(r'\[PRESENT\] frame=(\d+) ([^\n]+)',text):
            rows[int(frame)]['queue']=float(re.search(r'queue=([\d.]+)',body)[1])
    frames=list(range(1200,1801,60));metrics=('frame','render','layers_upload','hud_draw','queue')
    print(f'PASS: {len(ac)} complete state, geometry, speed and final scene-triangle checkpoints match; both runs exit cleanly')
    for k in metrics:
        old_ms=median(ar[f][k] for f in frames);new_ms=median(br[f][k] for f in frames)
        print(f'{k}: {old_ms:.3f} -> {new_ms:.3f} ms ({100*(1-new_ms/old_ms):.1f}% reduction)')
    if out:
        with out.open('w',newline='') as fp:
            w=csv.writer(fp,lineterminator='\n')
            w.writerow(['frame',*[p+k+'_ms' for k in metrics for p in ('old_','new_')],'matching_scene_triangle_crc'])
            crc={int(f):c for f,_,c in da}
            for f in frames:w.writerow([f,*[r[f][k] for k in metrics for r in (ar,br)],crc[f]])

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('old',type=Path);p.add_argument('new',type=Path)
    p.add_argument('--csv',type=Path);p.add_argument('--hud-only',action='store_true')
    a=p.parse_args();compare(a.old,a.new,a.csv,a.hud_only)
