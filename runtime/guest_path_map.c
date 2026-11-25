#include "guest_path_map.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define GPM_MAX_MAPPINGS 8
#define GPM_CACHE_SLOTS 256
#define GPM_MAX_COMPONENTS 64
#define GPM_MAX_NAME 256

typedef struct {
    char guest[GPM_MAX_PATH];
    char host[GPM_MAX_HOST];
} GpmMapping;

typedef struct {
    uint32_t used;
    char key[GPM_MAX_HOST+GPM_MAX_NAME+2];
    char name[GPM_MAX_NAME];
} GpmCacheEntry;

struct GuestPathMap {
    GpmMapping mappings[GPM_MAX_MAPPINGS];
    uint32_t count;
    GpmCacheEntry cache[GPM_CACHE_SLOTS];
    char mod_root[GPM_MAX_HOST];   /* optional read overlay for C:\\BurnoutPR */
};

static int lower(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }

static int fold_equal(const char *a,const char *b){
    while(*a && *b){
        if(lower((unsigned char)*a)!=lower((unsigned char)*b))return 0;
        a++;b++;
    }
    return *a==*b;
}

static uint32_t key_hash(const char *s){
    uint32_t h=2166136261u;
    while(*s){ h^=(uint8_t)*s++; h*=16777619u; }
    return h;
}

static void mkdir_p(const char *path){
    char buf[GPM_MAX_HOST];
    size_t n=strlen(path);
    if(!n || n>=sizeof(buf))return;
    memcpy(buf,path,n+1);
    for(char *p=buf+1;*p;p++) if(*p=='/'){ *p=0; (void)mkdir(buf,0777); *p='/'; }
    (void)mkdir(buf,0777);
}

int gpm_normalize(const char *in,char *out,uint32_t capacity){
    if(!in || !out)return GPM_INVALID;
    size_t n=strlen(in);
    if(!n)return GPM_INVALID;
    if(n>=GPM_MAX_PATH)return GPM_TOO_LONG;
    if(in[0]=='\\' || in[0]=='/')return GPM_INVALID;   /* UNC or device path */
    if(!((in[0]>='A'&&in[0]<='Z')||(in[0]>='a'&&in[0]<='z')) || in[1]!=':')
        return GPM_INVALID;
    if(in[2] && in[2]!='\\' && in[2]!='/')return GPM_INVALID;
    char drive=(char)(in[0]>='a' ? in[0]-32 : in[0]);
    char buf[GPM_MAX_PATH];
    uint32_t len=0;
    buf[len++]=drive; buf[len++]=':';
    uint32_t stack[GPM_MAX_COMPONENTS];
    uint32_t depth=0;
    const char *p=in+2;
    while(*p){
        while(*p=='\\'||*p=='/')p++;
        if(!*p)break;
        const char *s=p;
        while(*p && *p!='\\' && *p!='/')p++;
        uint32_t l=(uint32_t)(p-s);
        if(l==1 && s[0]=='.')continue;
        if(l==2 && s[0]=='.' && s[1]=='.'){
            if(!depth)return GPM_INVALID;              /* above the drive root */
            len=stack[--depth];
            continue;
        }
        if(l>=GPM_MAX_NAME)return GPM_INVALID;
        if(depth>=GPM_MAX_COMPONENTS)return GPM_INVALID;
        for(uint32_t i=0;i<l;i++)if(s[i]==':')return GPM_INVALID;
        if(len+1+l+2>(uint32_t)sizeof(buf))return GPM_TOO_LONG;
        stack[depth++]=len;
        buf[len++]='\\';
        memcpy(buf+len,s,l);
        len+=l;
    }
    if(!depth)buf[len++]='\\';                         /* drive root "C:\" */
    buf[len]=0;
    if(len+1>capacity)return GPM_TOO_LONG;
    memcpy(out,buf,len+1);
    return GPM_OK;
}

static int add_mapping(GuestPathMap *m,const char *guest,const char *host){
    if(m->count>=GPM_MAX_MAPPINGS)return GPM_TOO_LONG;
    if(strlen(guest)>=GPM_MAX_PATH || strlen(host)>=GPM_MAX_HOST)return GPM_TOO_LONG;
    GpmMapping *e=&m->mappings[m->count];
    memcpy(e->guest,guest,strlen(guest)+1);
    memcpy(e->host,host,strlen(host)+1);
    size_t n=strlen(e->host);
    while(n>1 && e->host[n-1]=='/')e->host[--n]=0;
    m->count++;
    return GPM_OK;
}

static void sort_mappings(GuestPathMap *m){
    for(uint32_t i=1;i<m->count;i++)
        for(uint32_t j=i;j>0 && strlen(m->mappings[j].guest)>strlen(m->mappings[j-1].guest);j--){
            GpmMapping t=m->mappings[j];
            m->mappings[j]=m->mappings[j-1];
            m->mappings[j-1]=t;
        }
}

static int join(char *out,size_t capacity,const char *root,const char *suffix){
    int n=snprintf(out,capacity,"%s%s",root,suffix);
    if(n<0 || (size_t)n>=capacity)return GPM_TOO_LONG;
    return GPM_OK;
}

int gpm_create(const char *game_root,const char *user_root,GuestPathMap **out){
    if(!game_root || !user_root || !out)return GPM_INVALID;
    if(!*game_root || !*user_root)return GPM_INVALID;
    GuestPathMap *m=calloc(1,sizeof(*m));
    if(!m)return GPM_IO;
    char host[GPM_MAX_HOST];
    int rc=add_mapping(m,"C:\\BurnoutPR",game_root);
    if(!rc)rc=join(host,sizeof(host),user_root,"/Documents");
    if(!rc)rc=add_mapping(m,"C:\\Users\\Player\\Documents",host);
    if(!rc)rc=join(host,sizeof(host),user_root,"/AppData/Roaming");
    if(!rc)rc=add_mapping(m,"C:\\Users\\Player\\AppData\\Roaming",host);
    if(!rc)rc=join(host,sizeof(host),user_root,"/AppData/Local");
    if(!rc)rc=add_mapping(m,"C:\\Users\\Player\\AppData\\Local",host);
    if(!rc)rc=join(host,sizeof(host),user_root,"/AppData/LocalLow");
    if(!rc)rc=add_mapping(m,"C:\\Users\\Player\\AppData\\LocalLow",host);
    if(!rc)rc=join(host,sizeof(host),user_root,"/ProgramData");
    if(!rc)rc=add_mapping(m,"C:\\ProgramData",host);
    if(!rc)rc=add_mapping(m,"C:\\Users\\Player",user_root);
    if(rc){ free(m); return rc; }
    sort_mappings(m);
    /* mkdir -p is idempotent: the port owns these directories. */
    for(uint32_t i=0;i<m->count;i++)mkdir_p(m->mappings[i].host);
    *out=m;
    return GPM_OK;
}

void gpm_destroy(GuestPathMap *m){ free(m); }

static int map_prefix(GuestPathMap *m,const char *canon,
                      const GpmMapping **out,const char **rest){
    for(uint32_t i=0;i<m->count;i++){
        const char *g=m->mappings[i].guest;
        size_t n=strlen(g),j=0;
        for(;j<n;j++){
            if(!canon[j] || lower((unsigned char)canon[j])!=lower((unsigned char)g[j]))break;
        }
        if(j!=n)continue;
        if(canon[n]!='\\' && canon[n]!=0)continue;
        *out=&m->mappings[i];
        *rest=canon+n;
        return GPM_OK;
    }
    return GPM_UNMAPPED;
}

int gpm_map(GuestPathMap *m,const char *guest_utf8,char *out,uint32_t capacity){
    if(!m || !guest_utf8 || !out)return GPM_INVALID;
    char canon[GPM_MAX_PATH];
    int rc=gpm_normalize(guest_utf8,canon,sizeof(canon));
    if(rc)return rc;
    const GpmMapping *map=NULL;
    const char *rest=NULL;
    rc=map_prefix(m,canon,&map,&rest);
    if(rc)return rc;
    size_t n=strlen(map->host);
    if(n+1>capacity)return GPM_TOO_LONG;
    memcpy(out,map->host,n);
    uint32_t len=(uint32_t)n;
    while(*rest){
        rest++;                                        /* skip the separator */
        const char *s=rest;
        while(*rest && *rest!='\\')rest++;
        size_t l=(size_t)(rest-s);
        if(len+1+l+1>capacity)return GPM_TOO_LONG;
        out[len++]='/';
        memcpy(out+len,s,l);
        len+=(uint32_t)l;
    }
    out[len]=0;
    return GPM_OK;
}

static int scan_dir(const char *dir,const char *want,char *name,uint32_t capacity){
    DIR *d=opendir(dir);
    if(!d)return errno==EACCES ? GPM_ACCESS : GPM_PATH_NOT_FOUND;
    int found=GPM_NOT_FOUND;
    struct dirent *e;
    while((e=readdir(d))!=NULL){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        if(!fold_equal(e->d_name,want))continue;
        size_t n=strlen(e->d_name);
        if(n>=capacity){ found=GPM_TOO_LONG; break; }
        memcpy(name,e->d_name,n+1);
        found=GPM_OK;
        break;
    }
    (void)closedir(d);
    return found;
}

static GpmCacheEntry *cache_slot(GuestPathMap *m,const char *key){
    uint32_t h=key_hash(key)%GPM_CACHE_SLOTS;
    for(uint32_t i=0;i<GPM_CACHE_SLOTS;i++){
        GpmCacheEntry *e=&m->cache[(h+i)%GPM_CACHE_SLOTS];
        if(!e->used || !strcmp(e->key,key))return e;
    }
    return &m->cache[h];
}

static int resolve_host(GuestPathMap *m,const char *host,uint32_t mode,
                        char *out,uint32_t capacity){
    char copy[GPM_MAX_HOST];
    size_t n=strlen(host);
    if(n>=sizeof(copy))return GPM_TOO_LONG;
    memcpy(copy,host,n+1);
    if(copy[0]!='/')return GPM_INVALID;
    const char *comp[GPM_MAX_COMPONENTS];
    uint32_t count=0;
    char *p=copy;
    while(*p){
        while(*p=='/')p++;
        if(!*p)break;
        if(count>=GPM_MAX_COMPONENTS)return GPM_TOO_LONG;
        comp[count++]=p;
        while(*p && *p!='/')p++;
        if(*p)*p++=0;
    }
    if(capacity<2)return GPM_TOO_LONG;
    uint32_t len=0;
    out[0]=0;
    for(uint32_t k=0;k<count;k++){
        int last=(k+1==count);
        char name[GPM_MAX_NAME];
        size_t nl=strlen(comp[k]);
        if(nl>=sizeof(name))return GPM_TOO_LONG;
        memcpy(name,comp[k],nl+1);
        char dir[GPM_MAX_HOST];
        if(!len){ dir[0]='/'; dir[1]=0; }
        else{
            if(len+1>sizeof(dir))return GPM_TOO_LONG;
            memcpy(dir,out,len);
            dir[len]=0;
        }
        char candidate[GPM_MAX_HOST];
        struct stat st;
        memset(&st,0,sizeof(st));
        int have=0;
        int w=snprintf(candidate,sizeof(candidate),"%s/%s",dir,name);
        if(w>0 && (size_t)w<sizeof(candidate) && stat(candidate,&st)==0)have=1;
        if(!have){
            /* The cache key folds the component so every case spelling of the
             * same directory entry shares one slot. */
            char key[sizeof(((GpmCacheEntry *)0)->key)];
            char folded[GPM_MAX_NAME];
            size_t fl=strlen(name);
            if(fl>=sizeof(folded))return GPM_TOO_LONG;
            for(size_t i=0;i<fl;i++)folded[i]=(char)lower((unsigned char)name[i]);
            folded[fl]=0;
            int kw=snprintf(key,sizeof(key),"%s\n%s",dir,folded);
            if(kw>0 && (size_t)kw<sizeof(key)){
                GpmCacheEntry *e=cache_slot(m,key);
                if(e->used && !strcmp(e->key,key)){
                    char cached[GPM_MAX_HOST];
                    int cw=snprintf(cached,sizeof(cached),"%s/%s",dir,e->name);
                    if(cw>0 && (size_t)cw<sizeof(cached) && stat(cached,&st)==0){
                        memcpy(name,e->name,strlen(e->name)+1);
                        nl=strlen(name);
                        have=1;
                    }else e->used=0;
                }
                if(!have){
                    char actual[GPM_MAX_NAME];
                    int rc=scan_dir(dir,name,actual,sizeof(actual));
                    if(rc==GPM_OK){
                        memcpy(name,actual,strlen(actual)+1);
                        nl=strlen(name);
                        int cw2=snprintf(candidate,sizeof(candidate),"%s/%s",dir,name);
                        if(cw2>0 && (size_t)cw2<sizeof(candidate) && stat(candidate,&st)==0)have=1;
                        if(have){
                            e->used=1;
                            memcpy(e->key,key,strlen(key)+1);
                            memcpy(e->name,name,strlen(name)+1);
                        }
                    }else if(rc!=GPM_NOT_FOUND)return rc;
                }
            }
        }
        if(!have && !(last && mode==GPM_MODE_WRITE))
            return last ? GPM_NOT_FOUND : GPM_PATH_NOT_FOUND;
        if(last && mode==GPM_MODE_DIR && have && !S_ISDIR(st.st_mode))
            return GPM_PATH_NOT_FOUND;
        if(len+1+nl+1>capacity)return GPM_TOO_LONG;
        out[len++]='/';
        memcpy(out+len,name,nl+1);
        len+=(uint32_t)nl;
    }
    if(!count){
        if(capacity<2)return GPM_TOO_LONG;
        out[0]='/'; out[1]=0;
    }
    return GPM_OK;
}

int gpm_set_mod_root(GuestPathMap *m,const char *mod_root){
    if(!m)return GPM_INVALID;
    if(!mod_root || !*mod_root){ m->mod_root[0]=0; return GPM_OK; }
    size_t n=strlen(mod_root);
    if(n>=sizeof(m->mod_root))return GPM_TOO_LONG;
    memcpy(m->mod_root,mod_root,n+1);
    while(n>1 && m->mod_root[n-1]=='/')m->mod_root[--n]=0;
    return GPM_OK;
}

/* Read-only overlay: an existing file below the mod root replaces the game
 * file of the same relative path (added content such as converted vehicles;
 * the game files themselves stay untouched). Only plain reads use it. */
static int resolve_mod(GuestPathMap *m,const char *guest_utf8,char *out,uint32_t capacity){
    static const char root[]="C:\\BurnoutPR";
    char canon[GPM_MAX_PATH];
    if(gpm_normalize(guest_utf8,canon,sizeof(canon)))return GPM_NOT_FOUND;
    size_t n=strlen(root);
    for(size_t j=0;j<n;j++)
        if(!canon[j] || lower((unsigned char)canon[j])!=lower((unsigned char)root[j]))return GPM_NOT_FOUND;
    if(canon[n]!='\\')return GPM_NOT_FOUND;
    char host[GPM_MAX_HOST];
    int len=snprintf(host,sizeof(host),"%s",m->mod_root);
    if(len<0 || (size_t)len>=sizeof(host))return GPM_TOO_LONG;
    for(const char *p=canon+n;*p;p++){
        if((size_t)len+2>=sizeof(host))return GPM_TOO_LONG;
        host[len++]=(*p=='\\') ? '/' : *p;
    }
    host[len]=0;
    return resolve_host(m,host,GPM_MODE_READ,out,capacity);
}

int gpm_resolve(GuestPathMap *m,const char *guest_utf8,uint32_t mode,
                char *out,uint32_t capacity){
    if(!m || !guest_utf8 || !out)return GPM_INVALID;
    if(mode!=GPM_MODE_READ && mode!=GPM_MODE_WRITE && mode!=GPM_MODE_DIR)
        return GPM_INVALID;
    static int trace=-1;
    if(trace<0)trace=getenv("PORT_TRACE_FILES") ? 1 : 0;
    if(mode==GPM_MODE_READ && m->mod_root[0] && resolve_mod(m,guest_utf8,out,capacity)==GPM_OK){
        if(trace)fprintf(stderr,"PORT_FILE mod %s -> %s\n",guest_utf8,out);
        return GPM_OK;
    }
    char host[GPM_MAX_HOST];
    int rc=gpm_map(m,guest_utf8,host,sizeof(host));
    if(rc)return rc;
    rc=resolve_host(m,host,mode,out,capacity);
    if(trace)fprintf(stderr,"PORT_FILE %s %s -> %s (%d)\n",mode==GPM_MODE_READ?"read":mode==GPM_MODE_WRITE?"write":"dir",
                     guest_utf8,rc==GPM_OK?out:host,rc);
    return rc;
}
