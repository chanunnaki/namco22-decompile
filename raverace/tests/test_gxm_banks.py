"""Validate predecoded pen banks and the actual native-bank fragment shader."""
from pathlib import Path
import runpy,ast,re,subprocess,tempfile,os
root=Path(__file__).resolve().parents[2]
base=runpy.run_path(str(root/'raverace/tests/test_gxm_batch.py'))
s=(root/'engine/quad_gxm_banks.inc').read_text();a=s.index('static const char bank_f_src');b=s.index('static bool bank_shader_init',a)
shader=''.join(ast.literal_eval(x) for x in re.findall(r'"(?:[^"\\]|\\.)*"',s[a:b]))
shader=re.sub(r':TEXCOORD\d','',shader).replace('uniform ','').replace('float4 main(', 'float4 shader_sample(')
for t in ['float2','float3','float4']:shader=shader.replace(t+'(', 'make_'+t+'(')
prefix=base['prefix'].replace('static uint8_t palette','static uint8_t *native_banks[3];\nstatic uint8_t palette')
prefix=prefix.replace('static float4 tex2D(int sampler,float2 uv){','''static float4 tex2D(int sampler,float2 uv){
 if(sampler>=3){int x=(int)floorf(uv.x*4096)&4095,y=(int)floorf(uv.y*4096)&4095;return {native_banks[sampler-3][y*4096+x]/255.0f,0,0,1};}''')
test=base['test'].replace(' for(int i=0;i<200000;i++){','''
 const unsigned banks[3]={0,7,15};
 for(unsigned k=0;k<3;k++){
  native_banks[k]=(uint8_t*)malloc(0x1000000);decode_texture_bank(native_banks[k],banks[k],g_texture_data,g_texture_tilemap);
  for(unsigned y=0;y<4096;y++)for(unsigned x=0;x<4096;x++)assert(native_banks[k][y*4096+x]==texture_pen_lookup(x,y,banks[k]));
 }
 for(int i=0;i<600000;i++){''')
test=test.replace('shader_sample(uv,shade,decode,fog,1,0,1,2)','shader_sample(uv,shade,decode,fog,1,bank==0?3:bank==7?4:5,2)')
test=test.replace('bank=next()%16','bank=banks[next()%3]')
test=test.replace(' free(rom_atlas);',' for(int i=0;i<3;i++)free(native_banks[i]);\n free(rom_atlas);')
test=test.replace('PASS: 200000 shader texture samples match CPU oracle across banks, flips, transpose, cmodes and wrapped coordinates','PASS: 50,331,648 predecoded texels and 600000 native shader samples match the original texture oracle')
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'test.cpp').write_text(prefix+base['packer']+base['oracle']+(root/'engine/texture_bank_decode.inc').read_text()+shader+test)
 subprocess.run(['clang++','-O2','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
