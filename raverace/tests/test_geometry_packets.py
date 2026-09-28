"""Validate indexed meshes and actual parallel packet emission against serial geometry."""
from pathlib import Path
import ast,subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
def assignment(path,name):
 tree=ast.parse(path.read_text())
 return next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id==name for t in n.targets))
shim=assignment(root/'raverace/tests/test_vita_worker.py','shim')
old=root/'raverace/tests/test_geometry_jobs.py'
prefix=assignment(old,'prefix');test=assignment(old,'test')
s=(root/'raverace/src/rr_gxm.c').read_text();code=s[s.index('/* Geometry lanes use separate'):s.index('/* ------------------------------------------------ the frame */')]
q=(root/'engine/quad_gxm.c').read_text();clip=q[q.index('typedef struct { float x, y, s, t, w, bri; }'):q.index('/* ---- GXM Vertex')]
h=(root/'engine/quad_gxm.h').read_text();types=h[h.index('typedef struct {'):h.index('/* Sort far to near */')]
fog=q[q.index('int eng_fog_alpha('):q.index('static int g_sort_tie_emit')]
extra=r'''
#include <math.h>
#include "quad_packet.h"
#define ENG_SCREEN_W 640
#define ENG_SCREEN_H 480
typedef eng_batch_vertex BatchVertex;
static float g_scene_x0=-103.529411f,g_scene_x1=743.529411f;
static bool packet_mode;
static int qn,qorder;
void geo_lane_clear_meshes(void);
'''
fog_cb=r'''
static int s22_fog_quad(const geo_quad*q,eng_fog*f){f->rgb[0]=q->color;f->alpha_const=q->cz_value&255;return 1;}
'''
test=test.replace('  memset(poly,0,sizeof poly);','  geo_hw_clear_meshes();geo_lane_clear_meshes();\n  memset(poly,0,sizeof poly);')
test=test.replace('for(int k=0;k<12;k++)points[at++]=(int16_t)next();\n   }','for(int k=0;k<12;k++)points[at++]=(trial%2)?(int16_t)next():((k%3)==2?1000:((k/3)&1)*2000-1000);\n   }')
test=test.replace('  count=0;eng_walk_list', '  geo_hw_native_meshes(0);count=0;memset(&g_geo_stats,0,sizeof g_geo_stats);eng_walk_list')
test=test.replace('  count=0;build_geometry(&cfg);', '''  geo_stats stats=g_geo_stats;
  geo_hw_native_meshes(1);geo_lane_native_meshes(1);count=0;memset(&g_geo_stats,0,sizeof g_geo_stats);
  eng_walk_list(poly_word,&cfg,push_quad,NULL);
  assert(count==n);assert(!memcmp(expected,output,n*sizeof *output));
  assert(!memcmp(&stats,&g_geo_stats,sizeof stats));
  packet_mode=false;count=0;build_geometry(&cfg);''')
test=test.replace('  assert(count==n);assert(memcmp(expected,output,n*sizeof *output)==0);', '''  assert(count==n);assert(memcmp(expected,output,n*sizeof *output)==0);
  packet_mode=true;qn=qorder=0;build_geometry(&cfg);assert(qn==n);
  for(int i=0;i<n;i++){
   eng_batch_vertex v[90];unsigned nv=quad_gxm_compile(&expected[i],&packet_cfg,v,90);
   assert(draw_packets[i].order==i && draw_packets[i].zsort==expected[i].zsort);
   assert(draw_packets[i].count==nv);
   if(nv)assert(!memcmp(v,draw_packets[i].vertices,nv*sizeof *v));
  }
  qsort(draw_packets,qn,sizeof *draw_packets,packet_compare);
  for(int i=1;i<qn;i++)assert(draw_packets[i-1].zsort>draw_packets[i].zsort ||
    (draw_packets[i-1].zsort==draw_packets[i].zsort && draw_packets[i-1].order>draw_packets[i].order));
  packet_mode=false;''')
test=test.replace('for(int i=0;i<4096;i++)free(geometry_jobs[i].quads);','for(int i=0;i<4096;i++){free(geometry_jobs[i].quads);free(geometry_jobs[i].spans);free(geometry_jobs[i].vertices);}\n free(draw_packets);geo_hw_clear_meshes();geo_lane_clear_meshes();')
test=test.replace('PASS: 200 parallel scenes match serial polygon bytes and emission order, including lighting and object flags','PASS: 200 indexed-mesh scenes match full serial polygon bytes/statistics; parallel packets match every triangle byte and painter order')
with tempfile.TemporaryDirectory(prefix='rr-mesh-packets-') as td:
 p=Path(td);inc=p/'psp2/kernel';inc.mkdir(parents=True);(inc/'threadmgr.h').write_text(shim)
 (p/'test.c').write_text(prefix+extra+types+fog+clip+fog_cb+(root/'engine/quad_compile.inc').read_text()+code+test)
 subprocess.run(['cc','-O2','-fwrapv','-pthread','-DGEO_NATIVE_MESH=1','-U__SIZEOF_INT128__','-fsanitize=address,undefined',
 '-I'+str(p),'-I'+str(root/'engine'),'-I'+str(root/'raverace/include'),str(p/'test.c'),
 *[str(root/'engine'/f) for f in ('eng.c','geo_hw.c','geo_hw_lane.c','slave_list.c')],'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
