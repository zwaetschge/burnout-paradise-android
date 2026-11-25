#include "guest_slist.h"
int gsl_initialize(GuestMemory *memory,uint32_t header){
    if(header&7)return GM_UNSUPPORTED;
    const uint8_t empty[8]={0};return gm_write(memory,header,empty,sizeof(empty));
}
