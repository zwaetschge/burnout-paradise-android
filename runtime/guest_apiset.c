#include "guest_apiset.h"
#include <string.h>
static unsigned lower(unsigned c){return c>='A' && c<='Z' ? c+32 : c;}
int gas_resolve(const GuestApiSet *entries,size_t count,const char *name,const char **host){
    if(!entries || !name || !host)return 1;
    for(size_t i=0;name[i];i++)if(name[i]=='/' || name[i]=='\\' || name[i]==':')return 1;
    size_t end=0,prefix=0;
    for(;name[end] && name[end]!='.';end++){
        if(name[end]=='/' || name[end]=='\\' || name[end]==':')return 1;
        if(name[end]=='-')prefix=end;
    }
    if(!prefix || end<=4 || !((lower(name[0])=='a' && lower(name[1])=='p' && lower(name[2])=='i' && name[3]=='-') ||
                            (lower(name[0])=='e' && lower(name[1])=='x' && lower(name[2])=='t' && name[3]=='-')))return 1;
    for(size_t i=0;i<count;i++){
        const char *p=entries[i].prefix;
        if(strlen(p)!=prefix)continue;
        size_t j=0;while(j<prefix && lower((unsigned char)name[j])==lower((unsigned char)p[j]))j++;
        if(j==prefix && entries[i].host && *entries[i].host){*host=entries[i].host;return 0;}
    }
    return 1;
}
