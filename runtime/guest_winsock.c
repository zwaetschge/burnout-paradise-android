#include "guest_winsock.h"
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int gws_backend_ready(void){
    /* No bind, connect, DNS, packets or remote endpoint. */
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
    if(fd<0)return 0;
    int rc=close(fd);return rc==0;
}
int gws_startup_value(GuestMemory *memory,GuestWinsock *winsock,uint32_t version,uint32_t output,uint32_t ready,uint32_t *result){
    /* Version negotiation of a Winsock 2.2 provider (WSAStartup, "Remarks"):
     * a 2.x request gets min(requested, 2.2) in wVersion and 2.2 in
     * wHighVersion. Requests above 2.2 and the 1.x compatibility profile
     * (iMaxSockets/iMaxUdpDg semantics) have not been needed and stay stops. */
    uint32_t major=version&0xffu,minor=(version>>8)&0xffu;
    if(!winsock || !result || ready>1 || major!=2u || minor>2u)return GM_UNSUPPORTED;
    if(!ready){*result=10091;return GM_OK;}
    if(gm_check(memory,output,400,GM_WRITE)){*result=10014;return GM_OK;}
    if(winsock->references==UINT32_MAX)return GM_UNSUPPORTED;
    uint8_t bytes[400]={0};bytes[0]=2;bytes[1]=(uint8_t)minor;bytes[2]=2;bytes[3]=2;
    static const char description[]="Native BSD socket compatibility runtime";
    static const char status[]="Socket backend available";
    memcpy(bytes+4,description,sizeof(description));memcpy(bytes+261,status,sizeof(status));
    /* PE32 WSADATA: 2-byte padding at394, pointer at396. Never host-cast. */
    int rc=gm_write(memory,output,bytes,394);if(rc)return rc;
    rc=gm_write(memory,output+396,bytes+396,4);if(rc)return rc;
    winsock->references++;*result=0;return GM_OK;
}
int gws_startup(GuestMemory *memory,GuestWinsock *winsock,uint32_t version,uint32_t output,uint32_t *result){
    return gws_startup_value(memory,winsock,version,output,(uint32_t)gws_backend_ready(),result);
}
int gws_cleanup(GuestWinsock *winsock,uint32_t *result){
    /* Only a matching initialization count is modelled; no socket teardown,
     * no packet or remote endpoint state exists in this provider. */
    if(!winsock || !result || !winsock->references)return GM_UNSUPPORTED;
    winsock->references--;*result=0;return GM_OK;
}
