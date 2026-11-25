#include "guest_commandline.h"
#include <stdlib.h>
static int read_text(GuestMemory *m,uint32_t address,uint16_t *text,uint32_t *length) {
    for(uint32_t i=0;i<32768;i++) {
        uint8_t b[2];
        if((uint64_t)address+2*i+2>0x100000000ull)return GM_RANGE;
        int rc=gm_read(m,address+2*i,b,2);if(rc)return rc;
        text[i]=(uint16_t)b[0]|((uint16_t)b[1]<<8);
        if(!text[i]){*length=i;return GM_OK;}
    }
    return GM_RANGE;
}
static int space(uint16_t c){return c==' ' || c=='\t';}
static void u32le(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
int gc_argv(GuestMemory *m,GuestLocalHeap *h,uint32_t command,uint32_t count_out,
            uint32_t fallback,uint32_t *argv) {
    *argv=0;
    int rc=gm_check(m,count_out,4,GM_WRITE);if(rc)return rc;
    uint16_t *text=calloc(32768,sizeof(*text)),*strings=calloc(32770,sizeof(*strings));
    uint32_t *offsets=calloc(32769,sizeof(*offsets));
    if(!text || !strings || !offsets){rc=GM_RANGE;goto done;}
    uint32_t length=0;
    rc=read_text(m,command,text,&length);if(rc)goto done;
    if(!length){rc=read_text(m,fallback,text,&length);if(rc)goto done;}
    uint32_t count=0,used=0,pos=0;
    if(space(text[0])){offsets[count++]=used;strings[used++]=0;while(space(text[pos]))pos++;}
    while(pos<length) {
        offsets[count++]=used;int quoted=0;
        while(pos<length && (quoted || !space(text[pos]))) {
            uint32_t slashes=0;while(text[pos]=='\\'){slashes++;pos++;}
            if(text[pos]=='"') {
                for(uint32_t i=0;i<slashes/2;i++)strings[used++]='\\';
                if(slashes&1){strings[used++]='"';pos++;}
                else {
                    if(quoted && text[pos+1]=='"'){rc=GM_UNSUPPORTED;goto done;}
                    quoted=!quoted;pos++;
                }
            } else {
                for(uint32_t i=0;i<slashes;i++)strings[used++]='\\';
                if(text[pos] && (quoted || !space(text[pos])))strings[used++]=text[pos++];
            }
        }
        strings[used++]=0;while(space(text[pos]))pos++;
    }
    if(!count){offsets[count++]=used;strings[used++]=0;}
    uint32_t vector_bytes=(count+1)*4,total=vector_bytes+used*2,address=0;
    rc=lh_alloc(h,total,&address);if(rc)goto done;
    uint8_t *payload=calloc(1,total);
    if(!payload){lh_free(h,address);rc=GM_RANGE;goto done;}
    for(uint32_t i=0;i<count;i++)u32le(payload+4*i,address+vector_bytes+offsets[i]*2);
    for(uint32_t i=0;i<used;i++){payload[vector_bytes+2*i]=(uint8_t)strings[i];payload[vector_bytes+2*i+1]=(uint8_t)(strings[i]>>8);}
    rc=gm_write(m,address,payload,total);free(payload);
    if(!rc){uint8_t b[4];u32le(b,count);rc=gm_write(m,count_out,b,4);}
    if(rc)lh_free(h,address);else *argv=address;
done:
    free(text);free(strings);free(offsets);return rc;
}
