/* Expand one immutable arcade bank to a native, wrapped 4096x4096 pen texture.
 * Palette and color mode remain dynamic; tile attributes are resolved once. */
static void decode_texture_bank(uint8_t *dst,unsigned bank,const uint8_t *rom,const uint8_t *map){
    uint8_t row[16*4096];
    for(unsigned ty=0;ty<256;ty++){
        for(unsigned tx=0;tx<256;tx++){
            unsigned index=bank*65536+ty*256+tx;
            unsigned tile=map[index*2]|(map[index*2+1]<<8);
            unsigned packed=map[0x200000+index/2],attr=(index&1)?packed&15:packed>>4;
            for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++){
                unsigned u=(attr&4)?15-x:x,v=(attr&2)?15-y:y;
                if(attr&8){unsigned swap=u;u=v;v=swap;}
                row[y*4096+tx*16+x]=(attr&1)?0:rom[tile*256+v*16+u];
            }
        }
        memcpy(dst+ty*sizeof row,row,sizeof row);
    }
}
