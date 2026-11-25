#include "shim_loader.h"
#include <string.h>
static unsigned lower(unsigned c) { return c>='A' && c<='Z' ? c+32 : c; }
static int module_name(const char *name,const char *expected) {
    size_t n=strlen(name),e=strlen(expected);int extension=strchr(name,'.')==NULL;
    if (n+(extension?4:0)!=e) return 0;
    for (size_t i=0;i<n;i++) if(lower((unsigned char)name[i])!=lower((unsigned char)expected[i]))return 0;
    return !extension || strcmp(expected+n,".dll")==0;
}
int sl_load(ShimLoader *s,const char *name,uint32_t *handle) {
    if (!name || !*name || strchr(name,'/') || strchr(name,'\\') || strchr(name,':'))return 1;
    for(size_t i=0;i<s->module_count;i++) if(module_name(name,s->modules[i].name)) {
        if(s->references[i]==UINT32_MAX)return 2;
        s->references[i]++;*handle=s->modules[i].handle;return 0;
    }
    return 1;
}
int sl_symbol(ShimLoader *s,uint32_t handle,const char *name,uint32_t ordinal,uint32_t *target) {
    size_t module=s->module_count;
    for(size_t i=0;i<s->module_count;i++)if(s->modules[i].handle==handle && s->references[i]) {module=i;break;}
    if(module==s->module_count)return 1;
    for(size_t i=0;i<s->export_count;i++) {
        const ShimExport *e=&s->exports[i];
        if(e->module==module && ((name && e->name && strcmp(name,e->name)==0) ||
                                (!name && ordinal && ordinal==e->ordinal))) {
            *target=e->target;return 0;
        }
    }
    return 1;
}

int sl_get_loaded(const ShimLoader *s,const char *name,const char *main_name,uint32_t main_image,uint32_t *handle){
    if(!s || !handle || !main_image || !main_name)return 1;
    if(!name){*handle=main_image;return 0;}
    if(!*name || strchr(name,'/') || strchr(name,'\\') || strchr(name,':'))return 1;
    if(module_name(name,main_name)){*handle=main_image;return 0;}
    for(size_t i=0;i<s->module_count;i++)if(module_name(name,s->modules[i].name)){
        *handle=s->references[i] ? s->modules[i].handle : 0;return 0;
    }
    /* An API-set query needs its separate namespace rules, not a fake miss. */
    if(strlen(name)>=4 && name[3]=='-' &&
       ((lower((unsigned char)name[0])=='a' && lower((unsigned char)name[1])=='p' && lower((unsigned char)name[2])=='i') ||
        (lower((unsigned char)name[0])=='e' && lower((unsigned char)name[1])=='x' && lower((unsigned char)name[2])=='t')))return 1;
    *handle=0;return 0;
}
