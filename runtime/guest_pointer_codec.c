#include "guest_pointer_codec.h"
#include "guest_memory.h"
#include <errno.h>
#include <sys/random.h>
int gpc_transform(uint32_t cookie,uint32_t which,uint32_t pointer,uint32_t *result){
    if(!cookie || !result || (which!=1 && which!=2))return GM_UNSUPPORTED;
    uint32_t count=cookie&31u,value;
    if(which==1){value=pointer^cookie;if(count)value=(value>>count)|(value<<(32u-count));}
    else{value=pointer;if(count)value=(value<<count)|(value>>(32u-count));value^=cookie;}
    *result=value;return GM_OK;
}
int gpc_random_cookie(uint32_t *cookie){
    if(!cookie)return GM_RANGE;
    for(;;){
        uint32_t value=0;uint32_t done=0;
        while(done<sizeof(value)){
            ssize_t n=getrandom((uint8_t*)&value+done,sizeof(value)-done,0);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return GM_ALLOCATION;
            done+=(uint32_t)n;
        }
        if(value){*cookie=value;return GM_OK;}
    }
}
