#include "shim_loader_wide.h"
static int read_name(GuestMemory *memory,uint32_t pointer,char *name,uint32_t capacity){
    uint32_t i;
    for(i=0;i<capacity;i++){
        uint8_t word[2];
        if((uint64_t)pointer+i*2+2>UINT64_C(0x100000000))return GM_RANGE;
        int rc=gm_read(memory,pointer+i*2,word,2);if(rc)return rc;
        if(word[1] || word[0]>=128)return GM_UNSUPPORTED;
        name[i]=(char)word[0];if(!word[0])break;
    }
    if(i==capacity)return GM_UNSUPPORTED;
    return GM_OK;
}
int sl_load_exw(ShimLoader *loader,GuestMemory *memory,const GuestApiSet *sets,size_t count,
                uint32_t pointer,uint32_t file,uint32_t flags,uint32_t *handle){
    if(file || flags!=0x800u || !pointer || !handle)return GM_UNSUPPORTED;
    char name[1024];int rc=read_name(memory,pointer,name,sizeof(name));if(rc)return rc;
    const char *host=name;
    if(gas_resolve(sets,count,name,&host))host=name;
    return sl_load(loader,host,handle) ? GM_UNSUPPORTED : GM_OK;
}

int sl_get_module_w(const ShimLoader *loader,GuestMemory *memory,uint32_t pointer,const char *main_name,uint32_t main_image,uint32_t *handle){
    if(!pointer)return sl_get_loaded(loader,NULL,main_name,main_image,handle) ? GM_UNSUPPORTED : GM_OK;
    char name[1024];int rc=read_name(memory,pointer,name,sizeof(name));if(rc)return rc;
    return sl_get_loaded(loader,name,main_name,main_image,handle) ? GM_UNSUPPORTED : GM_OK;
}
