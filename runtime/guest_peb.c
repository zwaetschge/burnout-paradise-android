#include "guest_peb.h"
#include "guest_environment.h"
#include <string.h>
#include <stdlib.h>
/* PE32 offsets checked against public winternl PEB/LDR layouts. Never cast
 * host structs: Android pointer width/alignment differ from the guest. */
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put16(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
int gp_thread_identity(GuestMemory *m,uint32_t teb,uint32_t process_id,uint32_t thread_id){
    if(!process_id || !thread_id || (uint64_t)teb+0x28>UINT64_C(0x100000000))return GM_RANGE;
    uint8_t b[8];put32(b,process_id);put32(b+4,thread_id);return gm_write(m,teb+0x20,b,8);
}
int gp_current_pseudo(uint32_t thread,uint32_t *value){
    if(!value || thread>1)return GM_RANGE;
    *value=thread ? UINT32_C(0xfffffffe) : UINT32_C(0xffffffff);return GM_OK;
}
int gp_current_identity(GuestMemory *m,uint32_t teb,uint32_t thread,uint32_t *value){
    if(!value || thread>1 || (uint64_t)teb+0x28>UINT64_C(0x100000000))return GM_RANGE;
    uint8_t b[4];int rc=gm_read(m,teb+0x20+thread*4,b,4);if(rc)return rc;
    uint32_t id=(uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
    if(!id)return GM_UNSUPPORTED;
    *value=id;return GM_OK;
}
static int name(uint8_t *b,uint32_t slot,const char *full,const char *base){
    size_t n=strlen(full),k=strlen(base);
    if(n>200 || k>n)return GM_UNSUPPORTED;
    uint32_t address=0x6000+slot*0x200,record=0x1000+slot*0x80;
    for(size_t i=0;i<=n;i++)put16(b+address+2*i,(uint8_t)full[i]);
    put16(b+record+0x24,(uint32_t)n*2);put16(b+record+0x26,(uint32_t)(n+1)*2);
    put32(b+record+0x28,GP_BASE+address);
    put16(b+record+0x2c,(uint32_t)k*2);put16(b+record+0x2e,(uint32_t)(k+1)*2);
    put32(b+record+0x30,GP_BASE+address+(uint32_t)(n-k)*2);return 0;
}
static void links(uint8_t *b,const uint32_t *nodes,uint32_t count,uint32_t head,uint32_t offset){
    put32(b+head,count ? GP_BASE+nodes[0]+offset : GP_BASE+head);
    put32(b+head+4,count ? GP_BASE+nodes[count-1]+offset : GP_BASE+head);
    for(uint32_t i=0;i<count;i++){
        put32(b+nodes[i]+offset,i+1<count ? GP_BASE+nodes[i+1]+offset : GP_BASE+head);
        put32(b+nodes[i]+offset+4,i ? GP_BASE+nodes[i-1]+offset : GP_BASE+head);
    }
}
int gp_sync_modules(GuestMemory *m,uint32_t image,uint32_t size,uint32_t entry,const ShimLoader *s){
    if(!s || s->module_count>=80)return GM_UNSUPPORTED;
    uint8_t b[GP_SIZE];int rc=gm_read(m,GP_BASE,b,sizeof(b));if(rc)return rc;
    uint32_t nodes[80],sorted[80],count=1;nodes[0]=0x1000;
    put32(b+0x100,0x30);b[0x104]=1;
    put32(b+0x1018,image);put32(b+0x101c,entry);put32(b+0x1020,size);
    if((rc=name(b,0,"C:\\BurnoutPR\\BurnoutPR.exe","BurnoutPR.exe")))return rc;
    for(uint32_t i=0;i<s->module_count;i++)if(s->references[i]){
        uint32_t record=0x1000+(i+1)*0x80;char full[256];
        const char *prefix="C:\\Windows\\System32\\";
        if(strlen(s->modules[i].name)+strlen(prefix)>=sizeof(full))return GM_UNSUPPORTED;
        strcpy(full,prefix);strcat(full,s->modules[i].name);
        if((rc=name(b,i+1,full,s->modules[i].name)))return rc;
        put32(b+record+0x18,s->modules[i].handle);
        /* Host-shim descriptors do not yet supply mapped PE images. */
        put32(b+record+0x1c,0);put32(b+record+0x20,0);
        put16(b+record+0x38,s->references[i]>65535 ? 65535 : s->references[i]);
        nodes[count++]=record;
    }
    links(b,nodes,count,0x10c,0);
    memcpy(sorted,nodes,count*sizeof(*nodes));
    for(uint32_t i=1;i<count;i++)for(uint32_t j=i;j>0;j--){
        uint32_t a=sorted[j],z=sorted[j-1];
        uint32_t av=a==0x1000 ? image : s->modules[(a-0x1000)/0x80-1].handle;
        uint32_t zv=z==0x1000 ? image : s->modules[(z-0x1000)/0x80-1].handle;
        if(av>=zv)break;sorted[j]=z;sorted[j-1]=a;
    }
    links(b,sorted,count,0x114,8);links(b,nodes+1,count-1,0x11c,16);
    return gm_write(m,GP_BASE+0x100,b+0x100,GP_SIZE-0x100);
}
int gp_initialize(GuestMemory *m,uint32_t image,uint32_t size,uint32_t entry,
                  uint32_t command,uint32_t command_bytes,const ShimLoader *s){
    if(command_bytes>65532 || (command_bytes&1))return GM_UNSUPPORTED;
    uint8_t b[0x300]={0};put32(b+8,image);put32(b+0xc,GP_BASE+0x100);put32(b+0x10,GP_BASE+0x200);
    put32(b+0x200,0x90);put32(b+0x204,0x90);put32(b+0x208,1);
    put32(b+0x248,GE_BLOCK_BASE);
    for(unsigned p=0x238;p<=0x240;p+=8){
        put16(b+p,command_bytes);put16(b+p+2,command_bytes+2);put32(b+p+4,command);
    }
    int rc=gm_write(m,GP_BASE,b,sizeof(b));
    return rc ? rc : gp_sync_modules(m,image,size,entry,s);
}
int gp_sync_image_views(GuestMemory *m,const ShimLoader *s,const GuestModuleImage *views,uint32_t count){
    if(!s || (!views && count) || s->module_count>=80)return GM_UNSUPPORTED;
    for(uint32_t i=0;i<s->module_count;i++)if(s->references[i]){
        for(uint32_t j=0;j<count;j++)if(s->modules[i].handle==views[j].base){
            uint8_t data[8];put32(data,views[j].entry);put32(data+4,views[j].size);
            int rc=gm_write(m,GP_BASE+0x1000+(i+1)*0x80+0x1c,data,sizeof(data));if(rc)return rc;
            break;
        }
    }
    return 0;
}
int gp_system_fields(GuestMemory *m,uint32_t major,uint32_t minor,uint32_t build,
                     uint32_t processors,uint32_t subsystem,uint32_t subsystem_major,uint32_t subsystem_minor){
    if(!processors)return GM_RANGE;
    uint8_t data[32];put32(data,major);put32(data+4,minor);put32(data+8,build);put32(data+12,2);
    put32(data+16,subsystem);put32(data+20,subsystem_major);put32(data+24,subsystem_minor);
    int rc=gm_write(m,GP_BASE+0xa4,data,28);if(rc)return rc;
    put32(data,processors);return gm_write(m,GP_BASE+0x64,data,4);
}

static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int read_field(GuestMemory *memory,uint32_t base,uint32_t offset,void *out,uint32_t size){
    if((uint64_t)base+offset+size>UINT64_C(0x100000000))return GM_RANGE;
    return gm_read(memory,base+offset,out,size);
}
int gp_main_file_name_a(GuestMemory *memory,uint32_t peb,uint32_t module,uint32_t output,uint32_t capacity,uint32_t *result,uint32_t *error){
    if(!result || !error)return GM_RANGE;
    uint8_t b[8];int rc=read_field(memory,peb,8,b,8);if(rc)return rc;
    uint32_t image=get32(b),ldr=get32(b+4);
    if(!image || !ldr || (module && module!=image))return GM_UNSUPPORTED;
    if(!capacity){*result=0;*error=122;return GM_OK;}
    rc=read_field(memory,ldr,0xc,b,4);if(rc)return rc;
    uint32_t record=get32(b);if(!record || record==(uint64_t)ldr+0xc)return GM_UNSUPPORTED;
    rc=read_field(memory,record,0x18,b,4);if(rc)return rc;
    if(get32(b)!=image)return GM_UNSUPPORTED;
    rc=read_field(memory,record,0x24,b,8);if(rc)return rc;
    uint32_t length=(uint32_t)b[0]|((uint32_t)b[1]<<8),maximum=(uint32_t)b[2]|((uint32_t)b[3]<<8),text=get32(b+4);
    if(!length || (length&1) || length>maximum)return GM_UNSUPPORTED;
    uint32_t count=length/2;uint8_t *wide=malloc(length),*ansi=malloc(count+1);
    if(!wide || !ansi){free(wide);free(ansi);return GM_ALLOCATION;}
    rc=gm_read(memory,text,wide,length);
    if(!rc){
        for(uint32_t i=0;i<count;i++){
            if(wide[i*2+1] || !wide[i*2] || wide[i*2]>=128){rc=GM_UNSUPPORTED;break;}
            ansi[i]=wide[i*2];
        }
    }
    uint32_t written=count<capacity ? count+1 : capacity;
    if(!rc){ansi[written-1]=0;rc=gm_write(memory,output,ansi,written);}
    free(wide);free(ansi);if(rc)return rc;
    *result=count<capacity ? count : capacity;*error=count<capacity ? 0 : 122;return GM_OK;
}
