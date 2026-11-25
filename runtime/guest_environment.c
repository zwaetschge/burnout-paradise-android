#include "guest_environment.h"
#include <string.h>
static unsigned lower(unsigned c){return c>='A' && c<='Z' ? c+32 : c;}
static int equal(const char *a,const char *b) {
    while(*a && *b){if(lower((unsigned char)*a++)!=lower((unsigned char)*b++))return 0;}return *a==*b;
}
static int compare_keys(const char *a,const char *b){
    while(*a && *b){unsigned x=lower((unsigned char)*a++),y=lower((unsigned char)*b++);if(x!=y)return x<y ? -1 : 1;}
    return *a ? 1 : *b ? -1 : 0;
}
int ge_serialize_block(const GuestEnvironmentEntry *entries,uint32_t count,uint8_t *out,uint32_t capacity,uint32_t *written){
    if((!entries && count) || !out || !written || count>128)return GM_RANGE;
    uint32_t order[128],units=count ? 1 : 2;
    for(uint32_t i=0;i<count;i++){
        if(!entries[i].name || !entries[i].value || !*entries[i].name)return GM_UNSUPPORTED;
        const char *parts[]={entries[i].name,entries[i].value};
        for(unsigned part=0;part<2;part++){
            size_t n=0;
            for(;parts[part][n];n++){
                if((unsigned char)parts[part][n]>=128u || (!part && parts[part][n]=='='))return GM_UNSUPPORTED;
                if(n>=32767)return GM_RANGE;
            }
            if(n>UINT32_MAX-units)return GM_RANGE;units+=(uint32_t)n;
        }
        if(units>UINT32_MAX-2)return GM_RANGE;units+=2;order[i]=i;
        for(uint32_t j=i;j>0 && compare_keys(entries[order[j]].name,entries[order[j-1]].name)<0;j--){
            uint32_t temp=order[j];order[j]=order[j-1];order[j-1]=temp;
        }
    }
    for(uint32_t i=1;i<count;i++)if(!compare_keys(entries[order[i-1]].name,entries[order[i]].name))return GM_UNSUPPORTED;
    if(units>capacity/2)return GM_RANGE;
    memset(out,0,units*2);uint32_t at=0;
    for(uint32_t i=0;i<count;i++){
        const GuestEnvironmentEntry *entry=&entries[order[i]];
        for(const char *p=entry->name;*p;p++,at++)out[at*2]=(uint8_t)*p;
        out[at++*2]='=';
        for(const char *p=entry->value;*p;p++,at++)out[at*2]=(uint8_t)*p;
        at++;
    }
    *written=units*2;return 0;
}
int ge_get_a(GuestMemory *m,const GuestEnvironmentEntry *entries,uint32_t count,
             uint32_t name,uint32_t buffer,uint32_t capacity,uint32_t last_error,uint32_t *result) {
    char key[32768];uint32_t length=0;
    for(;length<sizeof(key);length++) {
        if((uint64_t)name+length>UINT32_MAX)return GM_RANGE;
        uint8_t c;int rc=gm_read(m,name+length,&c,1);if(rc)return rc;
        if(c>=128)return GM_UNSUPPORTED;
        key[length]=(char)c;if(!c)break;
    }
    if(length==sizeof(key))return GM_RANGE;
    const char *value=NULL;
    for(uint32_t i=0;i<count;i++)if(equal(key,entries[i].name)){value=entries[i].value;break;}
    if(!value) {
        uint8_t code[4]={203,0,0,0};int rc=gm_write(m,last_error,code,4);
        *result=0;return rc;
    }
    size_t n=strlen(value);if(n>=32767)return GM_RANGE;
    *result=(uint32_t)n;
    if(capacity<=n){*result=(uint32_t)n+1;return GM_OK;}
    return gm_write(m,buffer,value,(uint32_t)n+1);
}
