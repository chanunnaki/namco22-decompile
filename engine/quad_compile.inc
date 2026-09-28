/* Pure packet compiler. No GXM calls or shared writes; callable by geometry lanes. */
unsigned quad_gxm_compile(const geo_quad *q, const eng_draw_cfg *cfg, eng_batch_vertex *out, unsigned capacity) {
    if (q->nrv < 3) return 0;
    int full = q->clip[0] <= 0 && q->clip[1] >= ENG_SCREEN_W-1;
    float left = full ? g_scene_x0 : fmaxf(q->clip[0],0);
    float right = full ? g_scene_x1 : fminf(q->clip[1]+1,ENG_SCREEN_W);
    float top = fmaxf(q->clip[2],0), bottom = fminf(q->clip[3]+1,ENG_SCREEN_H);
    if (left >= right || top >= bottom) return 0;
    geo_sv sv[10], cv[32];
    for (int i=0; i<q->nrv && i<10; i++) {
        float iw=q->rv[i].z>0 ? 1.0f/q->rv[i].z : 1.0f;
        float center=cfg->texel_centre ? 0.5f : 0.0f;
        sv[i]=(geo_sv){q->rv[i].sx16/16.0f,q->rv[i].sy16/16.0f,
            (q->rv[i].u+center)*iw,(q->rv[i].v+center)*iw,iw,
            cfg->shade ? fmaxf(q->rv[i].bri/64.0f,0.0f) : 1.0f};
    }
    int n=clip_to_screen(sv,q->nrv<10?q->nrv:10,cv,left,right,top,bottom);
    if (n<3) return 0;
    unsigned needed=(n-2)*3;
    if (needed>capacity) return 0;
    unsigned count=0;
    int solid=q->objectflags!=0, noshade=0;
    float rgb[3]={1,1,1};
    if (solid) {
        int col=(q->color>>8)&255,pen;
        if(q->objectflags&6){pen=q->cz_adjust&0x7fff;noshade=1;}
        else pen=((col&127)<<8)+(((q->cz_adjust>>16)&127)&(col|31));
        pen&=0x7fff;
        for(int c=0;c<3;c++)rgb[c]=direct_palette[pen>>8][pen&255][c]/255.0f;
    }
    int cm=q->cmode&15,offset=0,shift=0,mask=255;
    if(cm&4){offset=0xec+((cm&8)<<1);mask=3;shift=2*(~cm&3);}
    else if(cm&2){offset=0xe0+((cm&8)<<1);mask=15;shift=4*(~cm&1);}
    eng_fog fog={0}; fog.alpha_const=255;
    if(cfg->fog && cfg->fog_quad && !(q->cz_adjust&0x800000)) cfg->fog_quad(q,&fog);
    BatchVertex polygon[32];
    for(int i=0;i<n;i++) {
        float k=solid&&noshade ? 1.0f : cv[i].bri;
        float fk=cfg->fog_before_shade ? cv[i].bri : 1.0f;
        int32_t z=cv[i].w>0 ? (int32_t)(1.0f/cv[i].w) : 1;
        polygon[i]=(BatchVertex){cv[i].x,cv[i].y,cv[i].s,cv[i].t,cv[i].w,
            rgb[0]*k,rgb[1]*k,rgb[2]*k,
            cfg->write_prio_alpha ? ((q->cmode&7)==1?1.0f:0.0f) : 1.0f,
            q->texbank,((q->color>>8)&127)*256+offset,1<<shift,mask+1,
            fog.rgb[0]/255.0f*fk,fog.rgb[1]/255.0f*fk,fog.rgb[2]/255.0f*fk,
            eng_fog_alpha(&fog,z)/255.0f,solid?0.0f:1.0f};
    }
    for(int i=1;i<n-1;i++) {
        out[count++]=polygon[0];
        out[count++]=polygon[i];
        out[count++]=polygon[i+1];
    }
    return count;
}
