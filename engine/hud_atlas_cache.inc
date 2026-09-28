/* CPU-side glyph cache; no graphics API dependencies. RGBA glyphs are reused
 * across tilemap cells and uploaded independently to each fenced GPU slot. */
#define HUD_CACHE_SLOTS 2048
#define HUD_CACHE_TILES 1271
#define HUD_ATLAS_W 1024
#define HUD_ATLAS_H 512
typedef struct {
    int key;
    unsigned checked, used, version;
    unsigned char source[128], palette[64], pixels[1024];
    int empty;
} hud_cached_glyph;
typedef struct { int slot,x0,y0,x1,y1; float u0,v0,u1,v1; } hud_cached_tile;
static hud_cached_glyph *hud_glyphs;
static int hud_lookup[16384];
static unsigned hud_cache_frame,hud_cache_version,hud_rebuilt;
static hud_cached_tile hud_tiles[HUD_CACHE_TILES];
static unsigned hud_tile_count;
static void hud_cache_init(void){
    hud_glyphs=calloc(HUD_CACHE_SLOTS,sizeof *hud_glyphs);
    if(!hud_glyphs)abort();
    for(int i=0;i<HUD_CACHE_SLOTS;i++)hud_glyphs[i].key=-1;
    for(int i=0;i<16384;i++)hud_lookup[i]=-1;
}
static int hud_cache_glyph(unsigned code,unsigned palette,const unsigned char *chars,
                           const unsigned char *map,const unsigned char *rgba){
    unsigned key=code*16+palette;
    int slot=hud_lookup[key];
    if(slot<0){
        unsigned oldest=~0u;
        for(int i=0;i<HUD_CACHE_SLOTS;i++)if(hud_glyphs[i].used<oldest){oldest=hud_glyphs[i].used;slot=i;}
        if(slot<0 || hud_glyphs[slot].used==hud_cache_frame)abort();
        hud_cached_glyph *g=hud_glyphs+slot;
        if(g->key>=0)hud_lookup[g->key]=-1;
        g->key=key;g->version=0;g->checked=0;hud_lookup[key]=slot;
    }
    hud_cached_glyph *g=hud_glyphs+slot;g->used=hud_cache_frame;
    if(g->checked==hud_cache_frame)return slot;
    g->checked=hud_cache_frame;
    const unsigned char *src=code<960?chars+code*128:map+(code-960)*128;
    const unsigned char *pal=rgba+palette*64;
    if(!g->version || memcmp(g->source,src,128) || memcmp(g->palette,pal,64)){
        memcpy(g->source,src,128);memcpy(g->palette,pal,64);g->empty=1;
        for(unsigned i=0;i<256;i++){
            unsigned pen=(src[i/2]>>((i&1)?0:4))&15;
            memcpy(g->pixels+i*4,pal+pen*4,3);g->pixels[i*4+3]=pen==15?0:255;
            if(pen!=15)g->empty=0;
        }
        g->version=++hud_cache_version;hud_rebuilt++;
    }
    return slot;
}
static void hud_cache_build(const unsigned char *chars,const unsigned char *map,
                            const unsigned char *rgba,unsigned sx,unsigned sy){
    if(!hud_glyphs)hud_cache_init();
    ++hud_cache_frame;hud_tile_count=hud_rebuilt=0;
    for(int y=-(int)(sy&15);y<480;y+=16)for(int x=-(int)(sx&15);x<640;x+=16){
        unsigned tx=((x+(int)sx)&1023)/16,ty=((y+(int)sy)&1023)/16;
        unsigned hi=map[(ty*64+tx)*2],lo=map[(ty*64+tx)*2+1];
        int slot=hud_cache_glyph(((hi&3)<<8)|lo,hi>>4,chars,map,rgba);
        if(hud_glyphs[slot].empty)continue;
        int x0=x<0?0:x,y0=y<0?0:y,x1=x+16>640?640:x+16,y1=y+16>480?480:y+16;
        float u0=x0-x,u1=x1-x,v0=y0-y,v1=y1-y;
        if(hi&4){u0=16-u0;u1=16-u1;}if(hi&8){v0=16-v0;v1=16-v1;}
        float ax=(slot&63)*16,ay=(slot>>6)*16;
        if(hud_tile_count==HUD_CACHE_TILES)abort();
        hud_tiles[hud_tile_count++]=(hud_cached_tile){slot,x0,y0,x1,y1,
            (ax+u0)/HUD_ATLAS_W,(ay+v0)/HUD_ATLAS_H,(ax+u1)/HUD_ATLAS_W,(ay+v1)/HUD_ATLAS_H};
    }
}
