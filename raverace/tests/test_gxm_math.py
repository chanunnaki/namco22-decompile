"""Host checks for Vita viewport clipping and projection division."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
quad = (root / "engine/quad_gxm.c").read_text()
geo = (root / "engine/geo_hw.c").read_text()
clip = quad[quad.index("typedef struct { float x, y, s, t, w, bri; }"):quad.index("/* ---- GXM Vertex")]
start = geo.index("static inline uint64_t geo_udiv128_64")
div = geo[start:geo.index("static inline int64_t mulshr128", start)]
headers = "#include <stdint.h>\n#include <assert.h>\n#include <math.h>\n#include <stdio.h>\n"
clip_test = 'int main(void) {\n geo_sv in[4]={{0,0,0,0,1,1},{640,0,1,0,1,1},{640,480,1,1,1,1},{0,480,0,1,1,1}},out[32];\n int n=clip_to_screen(in,4,out,200,440,20,100);assert(n==4);\n for(int i=0;i<n;i++){assert(out[i].x>=200 && out[i].x<=440 && out[i].y>=20 && out[i].y<=100);assert(fabsf(out[i].s-out[i].x/640)<1e-6f);assert(fabsf(out[i].t-out[i].y/480)<1e-6f);}\n assert(clip_to_screen(in,4,out,700,800,20,100)==0);\n puts("PASS: mirror viewport contains all output vertices, preserves interpolants, rejects invisible polygons");\n}\n'
div_test = 'static uint64_t rng=22;\nstatic uint64_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}\nint main(void){\n for(int i=0;i<200000;i++) {uint64_t v=next()|1,u1=i%2 ? 0 : next()%v,u0=next();\n uint64_t expected=(uint64_t)((((unsigned __int128)u1<<64)|u0)/v);\n assert(geo_udiv128_64(u1,u0,v)==expected);}\n assert(geo_udiv128_64(10,1,10)==UINT64_MAX);\n puts("PASS: 200000 projection division cases match native 128-bit arithmetic");\n}\n'
a = geo.index("/* 32-bit fallback")
projection = geo[a:geo.index("\n#endif", a)]
projection_test = 'static uint64_t rng=22;\nstatic uint64_t next(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}\nint main(void){for(int i=0;i<200000;i++){\n int64_t v=(int32_t)next(),base=(int32_t)next(),z=(int32_t)next();int32_t m=(int16_t)next();int sh=next()%64,neg=next()%2;\n __int128 num=((__int128)v*m*16)>>sh;__int128 q=z?num/z:0;__int128 r=(__int128)base+(neg?-q:q);\n int32_t ref=r>INT32_MAX?INT32_MAX:r<INT32_MIN?INT32_MIN:(int32_t)r;\n assert(proj_sat(base,v,m,sh,z,neg)==ref);\n}\n for(int i=0;i<200000;i++) {\n  int64_t v=(int32_t)next(),base=(int32_t)next(),z=(next()%2147483647)+1;\n  int32_t m=(int16_t)next(); int sh=next()%12,neg=next()%2;\n  __int128 num=((__int128)v*m*16)>>sh,q=num/z,r=(__int128)base+(neg?-q:q);\n  int32_t ref=r>INT32_MAX?INT32_MAX:r<INT32_MIN?INT32_MIN:(int32_t)r;\n  assert(proj_sat(base,v,m,sh,z,neg)==ref);\n }\n puts("PASS: 400000 projection coordinates match native 128-bit formula");}\n'
sort_code = quad[quad.index("static int g_sort_tie_emit"):quad.index("/* Screen space clipping")]
sort_test = 'int main(void){for(int n=0;n<5000;n+=37)for(int tie=0;tie<2;tie++){\n geo_quad *a=calloc(n+1,sizeof *a),*b=calloc(n+1,sizeof *b);\n for(int i=0;i<n;i++){memset(&a[i],i&255,sizeof a[i]);a[i].zsort=rand()%30;a[i].order=i;}\n memcpy(b,a,n*sizeof *a);g_sort_tie_emit=tie;qsort(a,n,sizeof *a,zcmp);uint32_t *idx=quad_gxm_sort_indices(b,n,tie);if(n)for(int j=0;j<n;j++)assert(memcmp(&a[j],&b[idx[j]],sizeof *a)==0);eng_quad_sort(b,n,tie);\n assert(memcmp(a,b,n*sizeof *a)==0);free(a);free(b);\n}printf("PASS: index sort matches full-polygon sort for both tie modes (polygon size %zu bytes)\\n",sizeof(geo_quad));}\n'
# Compare the trivial-accept/reject shortcut against all four clipping passes.
reference_clip = clip[clip.index("static int clip_to_screen("):].replace("clip_to_screen(", "reference_clip(", 1)
a = reference_clip.index("    unsigned all = 15")
b = reference_clip.index("    for (int i = 0; i < n; i++) a[i] = in[i];", a)
reference_clip = reference_clip[:a] + reference_clip[b:]
clip_test = reference_clip + clip_test.replace(' puts("PASS:', ' for(int trial=0;trial<10000;trial++){geo_sv p[4],a[32],b[32];for(int i=0;i<4;i++)p[i]=(geo_sv){(rand()%14000-7000)/16.0f,(rand()%12000-3000)/16.0f,0.2f,0.7f,0.1f,1.0f};int na=reference_clip(p,4,a,-100,540,0,480),nb=clip_to_screen(p,4,b,-100,540,0,480);assert(na==nb);assert(memcmp(a,b,na*sizeof *a)==0);}\n puts("PASS: 10000 fast clipping cases match four-pass reference; ')
with tempfile.TemporaryDirectory(prefix="rr-math-") as d:
    for name, code in [("clip", clip + clip_test), ("divide", div + div_test), ("projection", projection + projection_test), ("sort", sort_code + sort_test)]:
        source = Path(d) / (name + ".c")
        binary = Path(d) / name
        source.write_text(headers + "#include <stdlib.h>\n#include <string.h>\n#include \"geo_hw.h\"\n" + code)
        subprocess.run(["cc", "-I" + str(root / "engine"), "-O1", "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
