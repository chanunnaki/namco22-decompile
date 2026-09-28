/* Compile immutable point-ROM objects to indexed vertices and packet commands.
 * Each execution lane owns its cache and scratch state. Point RAM and unusual
 * packets retain the original walker. Only transformed position/projection is
 * shared across faces; UVs, brightness, lighting cursors and clipping stay per face. */
#define MESH_CACHE_LIMIT (8u*1024*1024)
typedef struct { int32_t x,y,z,vx,vy,vz,sx,sy; unsigned stamp; int projected; } mesh_vertex;
typedef struct {
    int color,shift,flags,format,kind;
    uint32_t address,uv[8];
    uint16_t indices[4];
} mesh_command;
typedef struct mesh_object {
    struct mesh_object *next;
    int32_t code;
    mesh_command *commands;
    mesh_vertex *vertices;
    unsigned count,nvertices,command_capacity,vertex_capacity,generation;
} mesh_object;
static mesh_object *mesh_cache[256],*mesh_active;
static const mesh_command *mesh_current;
static const int32_t *mesh_rom;
static uint32_t mesh_rom_n;
static size_t mesh_bytes;
static int mesh_enabled=1;
static void mesh_free(mesh_object *m){free(m->commands);free(m->vertices);free(m);}
void geo_hw_clear_meshes(void){
    for(int i=0;i<256;i++){
        mesh_object *m=mesh_cache[i];while(m){mesh_object *next=m->next;mesh_free(m);m=next;}
        mesh_cache[i]=NULL;
    }
    mesh_bytes=0;mesh_active=NULL;mesh_current=NULL;
}
void geo_hw_native_meshes(int enabled){mesh_enabled=enabled;}
static int mesh_add_vertex(mesh_object *m,int *hash,int32_t x,int32_t y,int32_t z){
    unsigned h=((uint32_t)x*73856093u^(uint32_t)y*19349663u^(uint32_t)z*83492791u)&32767;
    while(hash[h]>=0){
        mesh_vertex *v=&m->vertices[hash[h]];
        if(v->x==x&&v->y==y&&v->z==z)return hash[h];
        h=(h+1)&32767;
    }
    if(m->nvertices==16384)return -1;
    if(m->nvertices==m->vertex_capacity){
        unsigned cap=m->vertex_capacity?m->vertex_capacity*2:64;
        void *p=realloc(m->vertices,cap*sizeof *m->vertices);if(!p)return -1;
        m->vertices=p;m->vertex_capacity=cap;
    }
    unsigned i=m->nvertices++;m->vertices[i]=(mesh_vertex){.x=x,.y=y,.z=z};hash[h]=i;return i;
}
static mesh_object *mesh_compile(int32_t code){
    if(code<0 || (uint32_t)code>=g_eng_pointrom_n || mesh_bytes>=MESH_CACHE_LIMIT)return NULL;
    mesh_object *m=calloc(1,sizeof *m);int *hash=malloc(32768*sizeof *hash);
    if(!m||!hash){free(m);free(hash);return NULL;}
    for(int i=0;i<32768;i++)hash[i]=-1;
    m->code=code;
    uint32_t list=(uint32_t)g_eng_pointrom[code];
    for(int guard=0;guard<4096;guard++){
        if(list>=g_eng_pointrom_n)goto fail;
        int32_t chunk=g_eng_pointrom[list++];if(chunk<0)goto done;
        if((uint32_t)chunk>=g_eng_pointrom_n)goto fail;
        uint32_t len=(uint32_t)g_eng_pointrom[chunk];
        if(len>256 || (uint64_t)(uint32_t)chunk+1+len>g_eng_pointrom_n)goto fail;
        uint32_t at=(uint32_t)chunk+1,end=at+len;
        while(at<end){
            int plen=g_eng_pointrom[at++];
            if(plen!=0x17&&plen!=0x18&&plen!=0x10&&plen!=0x0d)goto fail;
            if((uint64_t)at+(unsigned)plen>end || m->count==4096)goto fail;
            if(m->count==m->command_capacity){
                unsigned cap=m->command_capacity?m->command_capacity*2:32;
                void *p=realloc(m->commands,cap*sizeof *m->commands);if(!p)goto fail;
                m->commands=p;m->command_capacity=cap;
            }
            mesh_command *c=&m->commands[m->count++];memset(c,0,sizeof *c);
            c->format=g_eng_pointrom[at];c->kind=plen;
            if(plen==0x17||plen==0x18){
                c->color=g_eng_pointrom[at+2];c->flags=g_eng_pointrom[at+1];
                c->shift=plen==0x18?g_eng_pointrom[at+3]:0;c->address=at+(plen==0x18?4:3);
                for(int i=0;i<8;i++)c->uv[i]=(uint32_t)g_eng_pointrom[c->address+i]&0xffffff;
                for(int i=0;i<4;i++){
                    uint32_t a=c->address+8+i*3;
                    int index=mesh_add_vertex(m,hash,g_eng_pointrom[a],g_eng_pointrom[a+1],g_eng_pointrom[a+2]);
                    if(index<0)goto fail;
                    c->indices[i]=(uint16_t)index;
                }
            } else c->address=at+(plen==0x10?4:1);
            at+=(unsigned)plen;
            if(c->format&0x800000)break;
        }
    }
done:
    free(hash);
    size_t size=sizeof *m+m->command_capacity*sizeof *m->commands+m->vertex_capacity*sizeof *m->vertices;
    if(size>MESH_CACHE_LIMIT-mesh_bytes){mesh_free(m);return NULL;}
    mesh_bytes+=size;return m;
fail:
    free(hash);mesh_free(m);return NULL;
}
static int mesh_run(int32_t code,geo_quad_cb cb,void *user){
    if(!mesh_enabled||code==5)return 0;
    if(mesh_rom!=g_eng_pointrom||mesh_rom_n!=g_eng_pointrom_n){
        geo_hw_clear_meshes();mesh_rom=g_eng_pointrom;mesh_rom_n=g_eng_pointrom_n;
    }
    unsigned h=(uint32_t)code&255;
    mesh_object *m=mesh_cache[h];while(m&&m->code!=code)m=m->next;
    if(!m){m=mesh_compile(code);if(!m)return 0;m->next=mesh_cache[h];mesh_cache[h]=m;}
    if(++m->generation==0){for(unsigned i=0;i<m->nvertices;i++)m->vertices[i].stamp=0;m->generation=1;}
    mesh_active=m;
    for(unsigned i=0;i<m->count;i++){
        const mesh_command *c=&m->commands[i];
        if(c->kind==0x10){g_lit_n=0;g_lit_idx=0;register_normals(c->address);}
        else if(c->kind==0x0d)register_normals(c->address);
        else {mesh_current=c;quad_fixed(c->color,c->address,c->shift,c->flags,c->format,cb,user);mesh_current=NULL;}
    }
    mesh_active=NULL;return 1;
}
