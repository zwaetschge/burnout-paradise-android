#include "guest_registry.h"
/* Read-only registry projection: the Windows language/codepage keys a
 * Windows 10 x86 prefix reports (en-US, codepage 1252). Values the game
 * asks for that are not listed read as absent. */
static const uint8_t rv_000[]={0x30,0x00,0x30,0x00,0x30,0x00,0x30,0x00,0x30,0x00,0x34,0x00,0x30,0x00,0x39,0x00};
static const uint8_t rv_001[]={0x65,0x00,0x6e,0x00,0x2d,0x00,0x55,0x00,0x53,0x00};
static const uint8_t rv_002[]={0x31,0x00,0x39,0x00,0x30,0x00,0x34,0x00,0x35,0x00};
static const uint8_t rv_003[]={0x31,0x00,0x39,0x00,0x30,0x00,0x34,0x00,0x35,0x00};
static const uint8_t rv_004[]={0x0a,0x00,0x00,0x00};
static const uint8_t rv_005[]={0x00,0x00,0x00,0x00};
static const uint8_t rv_006[]={0x36,0x00,0x2e,0x00,0x33,0x00};
static const uint8_t rv_007[]={0x57,0x00,0x69,0x00,0x6e,0x00,0x64,0x00,0x6f,0x00,0x77,0x00,0x73,0x00,0x20,0x00,0x31,0x00,0x30,0x00,0x20,0x00,0x50,0x00,0x72,0x00,0x6f,0x00};
static const uint8_t rv_008[]={0x31,0x00,0x32,0x00,0x35,0x00,0x32,0x00};
static const uint8_t rv_009[]={0x34,0x00,0x33,0x00,0x37,0x00};
static const uint8_t rv_010[]={0x30,0x00,0x34,0x00,0x30,0x00,0x39,0x00};
static const uint8_t rv_011[]={0x30,0x00,0x34,0x00,0x30,0x00,0x39,0x00};
static const uint8_t rv_012[]={0x31,0x00,0x32,0x00,0x35,0x00,0x32,0x00};
static const uint8_t rv_013[]={0x34,0x00,0x33,0x00,0x37,0x00};
static const uint8_t rv_014[]={0x30,0x00,0x34,0x00,0x30,0x00,0x39,0x00};
static const uint8_t rv_015[]={0x30,0x00,0x34,0x00,0x30,0x00,0x39,0x00};
static const GuestRegistryValue guest_registry_values[]={
{"Locale",1u,rv_000,16u},
{"LocaleName",1u,rv_001,10u},
{"CurrentBuild",1u,rv_002,10u},
{"CurrentBuildNumber",1u,rv_003,10u},
{"CurrentMajorVersionNumber",4u,rv_004,4u},
{"CurrentMinorVersionNumber",4u,rv_005,4u},
{"CurrentVersion",1u,rv_006,6u},
{"ProductName",1u,rv_007,28u},
{"ACP",1u,rv_008,8u},
{"OEMCP",1u,rv_009,6u},
{"Default",1u,rv_010,8u},
{"InstallLanguage",1u,rv_011,8u},
{"ACP",1u,rv_012,8u},
{"OEMCP",1u,rv_013,6u},
{"Default",1u,rv_014,8u},
{"InstallLanguage",1u,rv_015,8u},
};
static const GuestRegistryKey guest_registry_keys[]={
{0x80000001u,"Control Panel\\International",0u,2u},
{0x80000002u,"Software\\Microsoft\\Windows NT\\CurrentVersion",2u,6u},
{0x80000002u,"System\\ControlSet001\\Control\\Nls\\Codepage",8u,2u},
{0x80000002u,"System\\ControlSet001\\Control\\Nls\\Language",10u,2u},
{0x80000002u,"System\\CurrentControlSet\\Control\\Nls\\Codepage",12u,2u},
{0x80000002u,"System\\CurrentControlSet\\Control\\Nls\\Language",14u,2u},
};
const GuestRegistryLayout guest_registry_layout={6u,16u,guest_registry_keys,guest_registry_values};

/* Body of runtime/guest_registry.c; concatenated after the generated table
 * (port/build/agents/a3-registry-table.inc) by
 * port/build/agents/a3-generate-registry-table.py. */
#include <stdlib.h>
#include <string.h>

#define GR_PATH_MAX 128
#define GR_WIDE_MAX 1024
#define GR_OPEN_MAX 64
#define GR_UNSERVED_MAX 32
#define GR_UNSERVED_PATH 128

typedef struct {
    uint32_t handle;
    uint32_t root;
    char path[GR_PATH_MAX];
    uint32_t first,count;
} OpenKey;

struct GuestRegistry {
    OpenKey open[GR_OPEN_MAX];
    uint32_t open_count,queries;
    char unserved_keys[GR_UNSERVED_MAX][GR_UNSERVED_PATH];
    char unserved_values[GR_UNSERVED_MAX][GR_UNSERVED_PATH];
    uint32_t unserved_key_count,unserved_value_count;
};

static uint32_t read_dword(const uint8_t *b){
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int write_word(GuestMemory *memory,uint32_t address,uint32_t value){
    uint8_t bytes[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
    return gm_write(memory,address,bytes,sizeof(bytes));
}
static int write_error(GuestMemory *memory,uint32_t teb,uint32_t error){
    return write_word(memory,teb+0x34,error);
}
static char lower_ascii(char c){return (c>='A' && c<='Z')?(char)(c-'A'+'a'):c;}
static int equal_ci(const char *a,const char *b){
    while(*a && *b){if(lower_ascii(*a)!=lower_ascii(*b))return 0;a++;b++;}
    return *a==*b;
}
static int starts_with_ci(const char *text,const char *prefix){
    while(*prefix){if(!*text || lower_ascii(*text)!=lower_ascii(*prefix))return 0;text++;prefix++;}
    return 1;
}
static int contains_ci(const char *text,const char *needle){
    size_t length=strlen(needle);
    for(;*text;text++){
        size_t i=0;
        while(i<length && text[i] && lower_ascii(text[i])==lower_ascii(needle[i]))i++;
        if(i==length)return 1;
    }
    return 0;
}
/* Captured labels escape separators twice; collapse runs, map '/' and strip
 * the edges. A name of only separators normalizes to the empty string. */
static int normalize(char *out,size_t capacity,const char *in){
    size_t used=0;
    for(size_t i=0;in[i];i++){
        char c=in[i]=='/'?'\\':in[i];
        if(c=='\\' && used && out[used-1]=='\\')continue;
        if(used+1>=capacity)return GM_UNSUPPORTED;
        out[used++]=c;
    }
    out[used]=0;
    size_t start=0;
    while(out[start]=='\\')start++;
    if(start)memmove(out,out+start,strlen(out+start)+1);
    size_t end=strlen(out);
    while(end && out[end-1]=='\\')out[--end]=0;
    return GM_OK;
}
/* Bounded guest UTF-16 name read. Only ASCII names are modelled; every other
 * unit becomes 0xFF so it can never collide with a captured ASCII key or value
 * name (an unknown name is reported as not found, never guessed). */
static int read_wide(GuestMemory *memory,uint32_t pointer,char *out,size_t capacity){
    size_t used=0;
    for(uint32_t index=0;index<GR_WIDE_MAX;index++){
        uint8_t bytes[2];
        int rc=gm_check(memory,pointer+index*2u,2,GM_READ);
        if(rc)return rc;
        if((rc=gm_read(memory,pointer+index*2u,bytes,sizeof(bytes))))return rc;
        uint32_t character=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8);
        if(!character){out[used]=0;return GM_OK;}
        if(used+1>=capacity)return GM_UNSUPPORTED;
        out[used++]=character>0x7fu?(char)0xff:(char)character;
    }
    return GM_UNSUPPORTED;
}
static OpenKey *find_open(GuestRegistry *registry,uint32_t handle){
    for(uint32_t i=0;i<registry->open_count;i++)
        if(registry->open[i].handle==handle)return &registry->open[i];
    return NULL;
}
static const GuestRegistryKey *find_key(uint32_t root,const char *path){
    for(uint32_t i=0;i<guest_registry_layout.key_count;i++){
        const GuestRegistryKey *key=&guest_registry_layout.keys[i];
        if(key->root==root && equal_ci(key->path,path))return key;
    }
    return NULL;
}
/* The guest is 32-bit, so HKLM\Software resolves through Wow6432Node. */
static const GuestRegistryKey *lookup(uint32_t root,const char *path){
    const GuestRegistryKey *key=find_key(root,path);
    if(key || root!=GWR_HKEY_LOCAL_MACHINE)return key;
    if(!starts_with_ci(path,"Software\\") || contains_ci(path,"wow6432node"))return NULL;
    char redirected[GR_PATH_MAX];
    static const char prefix[]="Software\\Wow6432Node\\";
    size_t prefix_length=sizeof(prefix)-1;
    size_t rest=strlen(path+9);
    if(prefix_length+rest+1>sizeof(redirected))return NULL;
    memcpy(redirected,prefix,prefix_length);
    memcpy(redirected+prefix_length,path+9,rest+1);
    return find_key(root,redirected);
}
static void record_unserved(char (*slots)[GR_UNSERVED_PATH],uint32_t *count,const char *name){
    if(*count>=GR_UNSERVED_MAX)return;
    char *slot=slots[*count];
    size_t length=strlen(name);
    if(length>=GR_UNSERVED_PATH)length=GR_UNSERVED_PATH-1;
    memcpy(slot,name,length);
    slot[length]=0;
    (*count)++;
}

int gwr_create(GuestRegistry **out){
    if(!out)return GM_UNSUPPORTED;
    GuestRegistry *registry=calloc(1,sizeof(*registry));
    if(!registry)return GM_ALLOCATION;
    *out=registry;
    return GM_OK;
}
void gwr_destroy(GuestRegistry *registry){free(registry);}
uint32_t gwr_queries(const GuestRegistry *registry){return registry?registry->queries:0;}
uint32_t gwr_unserved_keys(const GuestRegistry *registry){return registry?registry->unserved_key_count:0;}
uint32_t gwr_unserved_values(const GuestRegistry *registry){return registry?registry->unserved_value_count:0;}
const char *gwr_unserved_key(const GuestRegistry *registry,uint32_t index){
    if(!registry || index>=registry->unserved_key_count)return NULL;
    return registry->unserved_keys[index];
}
const char *gwr_unserved_value(const GuestRegistry *registry,uint32_t index){
    if(!registry || index>=registry->unserved_value_count)return NULL;
    return registry->unserved_values[index];
}

int gwr_open(GuestRegistry *registry,GuestMemory *memory,uint32_t teb,uint32_t key,
             uint32_t subkey,uint32_t options,uint32_t access,uint32_t result,
             uint32_t *status){
    (void)options;
    if(!registry || !memory || !status)return GM_UNSUPPORTED;
    char name[GR_PATH_MAX];
    int rc;
    if(!subkey)name[0]=0;
    else if((rc=read_wide(memory,subkey,name,sizeof(name))))return rc;
    char normalized[GR_PATH_MAX];
    if((rc=normalize(normalized,sizeof(normalized),name)))return rc;
    const GuestRegistryKey *found=normalized[0]?lookup(key,normalized):NULL;
    if((rc=gm_check(memory,result,4,GM_WRITE)))return rc;
    if(access & GWR_WRITE_ACCESS)return GM_UNSUPPORTED;
    if(normalized[0] && !found){
        record_unserved(registry->unserved_keys,&registry->unserved_key_count,name);
        if((rc=write_word(memory,result,0)))return rc;
        if((rc=write_error(memory,teb,GWR_ERROR_FILE_NOT_FOUND)))return rc;
        *status=GWR_ERROR_FILE_NOT_FOUND;
        return GM_OK;
    }
    /* Handles are positional exactly like the Python model's open-key dict:
     * the next handle is HANDLE_BASE + number of live keys, and a handle that
     * is still present (possible after a close freed a lower slot) is
     * overwritten in place instead of growing the table. */
    uint32_t handle=GWR_HANDLE_BASE+registry->open_count;
    OpenKey *slot=find_open(registry,handle);
    if(!slot){
        if(registry->open_count>=GR_OPEN_MAX)return GM_UNSUPPORTED;
        slot=&registry->open[registry->open_count++];
    }
    slot->handle=handle;
    slot->root=key;
    memcpy(slot->path,normalized,strlen(normalized)+1);
    slot->first=found?found->first_value:0;
    slot->count=found?found->value_count:0;
    if((rc=write_word(memory,result,handle)))return rc;
    if((rc=write_error(memory,teb,GWR_ERROR_SUCCESS)))return rc;
    *status=GWR_ERROR_SUCCESS;
    return GM_OK;
}

int gwr_query(GuestRegistry *registry,GuestMemory *memory,uint32_t teb,uint32_t handle,
              uint32_t name,uint32_t reserved,uint32_t type,uint32_t data,uint32_t size,
              uint32_t *status){
    (void)reserved;
    if(!registry || !memory || !status)return GM_UNSUPPORTED;
    OpenKey *key=find_open(registry,handle);
    if(!key)return GM_UNSUPPORTED;
    char requested[GR_PATH_MAX];
    int rc;
    if(!name)requested[0]=0;
    else if((rc=read_wide(memory,name,requested,sizeof(requested))))return rc;
    const GuestRegistryValue *entry=NULL;
    for(uint32_t i=0;i<key->count;i++){
        const GuestRegistryValue *candidate=&guest_registry_layout.values[key->first+i];
        if(equal_ci(candidate->name,requested)){entry=candidate;break;}
    }
    int has_size=size!=0;
    uint32_t capacity=0;
    if(has_size){
        uint8_t bytes[4];
        if((rc=gm_check(memory,size,4,GM_WRITE)))return rc;
        if((rc=gm_read(memory,size,bytes,sizeof(bytes))))return rc;
        capacity=read_dword(bytes);
    }
    if(!entry){
        if(has_size && (rc=write_word(memory,size,0)))return rc;
        record_unserved(registry->unserved_values,&registry->unserved_value_count,requested);
        if((rc=write_error(memory,teb,GWR_ERROR_FILE_NOT_FOUND)))return rc;
        *status=GWR_ERROR_FILE_NOT_FOUND;
        return GM_OK;
    }
    registry->queries++;
    if(type){
        if((rc=gm_check(memory,type,4,GM_WRITE)))return rc;
        if((rc=write_word(memory,type,entry->type)))return rc;
    }
    uint32_t length=entry->size;
    if(!data){
        if(has_size && (rc=write_word(memory,size,length)))return rc;
        if((rc=write_error(memory,teb,GWR_ERROR_SUCCESS)))return rc;
        *status=GWR_ERROR_SUCCESS;
        return GM_OK;
    }
    if(!has_size)return GM_UNSUPPORTED;
    if(capacity<length){
        if((rc=write_word(memory,size,length)))return rc;
        if((rc=write_error(memory,teb,GWR_ERROR_MORE_DATA)))return rc;
        *status=GWR_ERROR_MORE_DATA;
        return GM_OK;
    }
    /* The checked-write contract of the model rejects a zero-length copy
     * (memory.check(address,0,..) is false), so an empty value can only be
     * read as a size query. Mirrored here instead of inventing a success. */
    if((rc=gm_check(memory,data,length,GM_WRITE)))return rc;
    if((rc=gm_write(memory,data,entry->data,length)))return rc;
    if((rc=write_word(memory,size,length)))return rc;
    if((rc=write_error(memory,teb,GWR_ERROR_SUCCESS)))return rc;
    *status=GWR_ERROR_SUCCESS;
    return GM_OK;
}

int gwr_close(GuestRegistry *registry,GuestMemory *memory,uint32_t teb,uint32_t handle,
              uint32_t *status){
    if(!registry || !memory || !status)return GM_UNSUPPORTED;
    int rc;
    if(handle==GWR_HKEY_CLASSES_ROOT || handle==GWR_HKEY_CURRENT_USER ||
       handle==GWR_HKEY_LOCAL_MACHINE || handle==GWR_HKEY_USERS){
        if((rc=write_error(memory,teb,GWR_ERROR_SUCCESS)))return rc;
        *status=GWR_ERROR_SUCCESS;
        return GM_OK;
    }
    OpenKey *key=find_open(registry,handle);
    if(!key)return GM_UNSUPPORTED;
    uint32_t index=(uint32_t)(key-registry->open);
    memmove(&registry->open[index],&registry->open[index+1],
            (size_t)(registry->open_count-index-1)*sizeof(OpenKey));
    registry->open_count--;
    if((rc=write_error(memory,teb,GWR_ERROR_SUCCESS)))return rc;
    *status=GWR_ERROR_SUCCESS;
    return GM_OK;
}
