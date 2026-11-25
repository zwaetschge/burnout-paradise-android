#include "guest_condition.h"
int gcv_initialize(GuestMemory *memory,uint32_t condition){
    const uint8_t empty[4]={0};return gm_write(memory,condition,empty,sizeof(empty));
}
