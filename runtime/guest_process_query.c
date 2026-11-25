#include "guest_process_query.h"
#include "guest_peb.h"
#include <string.h>
static void put32(uint8_t *b,uint32_t value){
    b[0]=(uint8_t)value;b[1]=(uint8_t)(value>>8);b[2]=(uint8_t)(value>>16);b[3]=(uint8_t)(value>>24);
}
static int last_error(GuestMemory *memory,uint32_t teb,uint32_t value){
    uint8_t bytes[4];put32(bytes,value);return gm_write(memory,teb+0x34,bytes,4);
}
/* The runtime-owned process id is the one serialized in the guest TEB. */
static int owner(GuestMemory *memory,uint32_t teb,uint32_t *process){
    return gp_current_identity(memory,teb,0,process);
}
int gpq_open_process(GuestMemory *memory,uint32_t teb,uint32_t access,uint32_t inherit,
                     uint32_t process,uint32_t *handle){
    if(!memory || !handle)return GM_UNSUPPORTED;
    if((access&0xffff0000u) || inherit>1)return GM_UNSUPPORTED;
    uint32_t current;
    int rc=owner(memory,teb,&current);if(rc)return rc;
    if(process!=current){
        rc=last_error(memory,teb,GPQ_ERROR_INVALID_PARAMETER);if(rc)return rc;
        *handle=0;return GM_OK;
    }
    *handle=GPQ_PROCESS_HANDLE;return GM_OK;
}
int gpq_module_base_name_w(GuestMemory *memory,const ShimLoader *loader,uint32_t process,
                           uint32_t module,uint32_t buffer,uint32_t capacity,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    if(process!=GPQ_PROCESS_HANDLE && process!=UINT32_MAX)return GM_UNSUPPORTED;
    if(!capacity)return GM_UNSUPPORTED;
    const char *name;
    if(!module)name=GPQ_MAIN_MODULE_NAME;
    else{
        name=NULL;
        if(loader)for(size_t i=0;i<loader->module_count;i++)
            if(loader->modules[i].handle==module){name=loader->modules[i].name;break;}
        if(!name){*result=0;return GM_OK;}
    }
    size_t length=strlen(name);
    if(length>capacity-1)length=capacity-1;
    if(!length)return GM_UNSUPPORTED; /* nothing but the terminator fits */
    uint8_t bytes[2*1024+2];
    if(length*2+2>sizeof(bytes))return GM_UNSUPPORTED;
    for(size_t i=0;i<length;i++){bytes[i*2]=(uint8_t)name[i];bytes[i*2+1]=0;}
    bytes[length*2]=0;bytes[length*2+1]=0;
    int rc=gm_write(memory,buffer,bytes,(uint32_t)(length*2+2));if(rc)return rc;
    *result=(uint32_t)length;return GM_OK;
}
int gpq_close_handle(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    if(handle==GPQ_PROCESS_HANDLE){*result=1;return GM_OK;}
    if(handle==UINT32_MAX || handle==UINT32_MAX-1){
        int rc=last_error(memory,teb,GPQ_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    return GM_UNSUPPORTED;
}
int gpq_enumerate_processes(GuestMemory *memory,uint32_t teb,uint32_t output,uint32_t capacity,
                            uint32_t needed,uint32_t *result){
    if(!memory || !result || !needed)return GM_UNSUPPORTED;
    int rc=gm_check(memory,needed,4,GM_WRITE);if(rc)return rc;
    if(capacity%4)return GM_UNSUPPORTED;
    uint32_t process;
    rc=owner(memory,teb,&process);if(rc)return rc;
    /* One guest process: a four-byte list, or the required size only. */
    if(output && capacity>=4){
        uint8_t bytes[4];put32(bytes,process);
        rc=gm_write(memory,output,bytes,4);if(rc)return rc;
    }else if(output)return GM_UNSUPPORTED;
    uint8_t bytes[4];put32(bytes,4);
    rc=gm_write(memory,needed,bytes,4);if(rc)return rc;
    *result=(output || !capacity) ? 1u : 0u;
    return GM_OK;
}
