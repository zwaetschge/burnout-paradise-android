#include "guest_file_api.h"
#include "guest_handles.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define GFA_MAX_GUEST_PATH 1024u
#define GFA_MAX_HOST_PATH 2048u
#define GFA_MAX_LEAF 256u
#define GFA_IO_CHUNK 65536u
#define GFA_UNIX_EPOCH_DELTA UINT64_C(11644473600)

/* Distinguishes "guest string too long" from GM_* model stops. */
enum { GFA_TOO_LONG=1000 };

/* Access bits accepted by CreateFile: GENERIC_READ, GENERIC_WRITE, SYNCHRONIZE
 * and the query-only 0. Everything else is a named stop. */
#define GFA_ACCESS_ALLOWED (0x80000000u|0x40000000u|0x00100000u)
/* Attribute bits plus the ignorable caching hints. FILE_FLAG_OVERLAPPED,
 * NO_BUFFERING, DELETE_ON_CLOSE, OPEN_REPARSE_POINT, OPEN_NO_RECALL and
 * ENCRYPTED are deliberately absent: they change semantics we do not model. */
#define GFA_FLAGS_ALLOWED (0x00000001u|0x00000002u|0x00000004u|0x00000010u|\
                           0x00000020u|0x00000080u|0x00000100u|0x00000200u|\
                           0x00000400u|0x00000800u|0x00001000u|0x00002000u|\
                           0x00004000u|0x02000000u|0x08000000u|0x10000000u|\
                           0x80000000u)

typedef struct {
    int fd;
    char host[GFA_MAX_HOST_PATH];
} GfaFile;

typedef struct {
    DIR *dir;
    char pattern[GFA_MAX_LEAF];
    char host[GFA_MAX_HOST_PATH];
} GfaFind;

struct GuestFileApi {
    GuestPathMap *map;
    const GuestLocale *locale;
    char cwd[GPM_MAX_PATH];
};

/* ---------------------------------------------------------------- helpers */

static void put32le(uint8_t *b,uint32_t v){
    b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);
}
static uint32_t get32le(const uint8_t *b){
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int put32(GuestMemory *m,uint32_t address,uint32_t value){
    uint8_t b[4];put32le(b,value);return gm_write(m,address,b,4);
}
static int put64(GuestMemory *m,uint32_t address,uint64_t value){
    uint8_t b[8];
    for(int i=0;i<8;i++)b[i]=(uint8_t)(value>>(8*i));
    return gm_write(m,address,b,8);
}
static int get32(GuestMemory *m,uint32_t address,uint32_t *value){
    uint8_t b[4];int rc=gm_read(m,address,b,4);
    if(rc)return rc;
    *value=get32le(b);return GM_OK;
}
static int last_error(GuestMemory *m,uint32_t teb,uint32_t value){
    return put32(m,teb+0x34,value);
}
static uint32_t map_error(int code){
    switch(code){
    case GPM_OK:return 0;
    case GPM_NOT_FOUND:return GFA_ERROR_FILE_NOT_FOUND;
    case GPM_PATH_NOT_FOUND:return GFA_ERROR_PATH_NOT_FOUND;
    case GPM_UNMAPPED:return GFA_ERROR_PATH_NOT_FOUND;
    case GPM_INVALID:return GFA_ERROR_INVALID_NAME;
    case GPM_TOO_LONG:return GFA_ERROR_FILENAME_EXCED_RANGE;
    case GPM_ACCESS:return GFA_ERROR_ACCESS_DENIED;
    default:return GFA_ERROR_READ_FAULT;
    }
}
/* A missing directory component is ERROR_PATH_NOT_FOUND, also when the API
 * asked for a directory explicitly. */
static uint32_t map_dir_error(int code){
    if(code==GPM_NOT_FOUND)return GFA_ERROR_PATH_NOT_FOUND;
    return map_error(code);
}
static uint32_t io_error(int error,int writing){
    switch(error){
    case EACCES:case EPERM:case EISDIR:case EBADF:return GFA_ERROR_ACCESS_DENIED;
    case ENOENT:return GFA_ERROR_FILE_NOT_FOUND;
    case ENOSPC:return GFA_ERROR_DISK_FULL;
    default:return writing?GFA_ERROR_WRITE_FAULT:GFA_ERROR_READ_FAULT;
    }
}
static void filetime(uint8_t *out,time_t seconds,long nanoseconds){
    uint64_t ticks=((uint64_t)seconds+GFA_UNIX_EPOCH_DELTA)*(uint64_t)10000000+
                   (uint64_t)(nanoseconds/100);
    for(int i=0;i<8;i++)out[i]=(uint8_t)(ticks>>(8*i));
}
static uint32_t attributes_of(const struct stat *st){
    uint32_t attributes;
    if(S_ISDIR(st->st_mode))attributes=0x10u;          /* FILE_ATTRIBUTE_DIRECTORY */
    else attributes=0x20u;                             /* FILE_ATTRIBUTE_ARCHIVE */
    if(!(st->st_mode&(S_IWUSR|S_IWGRP|S_IWOTH)))attributes|=0x1u;
    return attributes;
}

/* UTF-8 <-> UTF-16/CP1252 conversion, always through the installed locale. */
static int utf8_encode(uint32_t cp,char *out,uint32_t capacity,uint32_t *written){
    if(cp<0x80){
        if(capacity<1)return GFA_TOO_LONG;
        out[0]=(char)cp;*written=1;return 0;
    }
    if(cp<0x800){
        if(capacity<2)return GFA_TOO_LONG;
        out[0]=(char)(0xc0|(cp>>6));out[1]=(char)(0x80|(cp&0x3f));
        *written=2;return 0;
    }
    if(cp<0x10000){
        if(capacity<3)return GFA_TOO_LONG;
        out[0]=(char)(0xe0|(cp>>12));out[1]=(char)(0x80|((cp>>6)&0x3f));
        out[2]=(char)(0x80|(cp&0x3f));*written=3;return 0;
    }
    if(cp<0x110000){
        if(capacity<4)return GFA_TOO_LONG;
        out[0]=(char)(0xf0|(cp>>18));out[1]=(char)(0x80|((cp>>12)&0x3f));
        out[2]=(char)(0x80|((cp>>6)&0x3f));out[3]=(char)(0x80|(cp&0x3f));
        *written=4;return 0;
    }
    return GM_UNSUPPORTED;
}
static int utf8_decode(const char *text,uint32_t *cp,uint32_t *used){
    const unsigned char *u=(const unsigned char *)text;
    if(u[0]<0x80){ *cp=u[0]; *used=1; return GM_OK; }
    if((u[0]&0xe0)==0xc0 && (u[1]&0xc0)==0x80){
        *cp=((uint32_t)(u[0]&0x1f)<<6)|(u[1]&0x3f);*used=2;return GM_OK;
    }
    if((u[0]&0xf0)==0xe0 && (u[1]&0xc0)==0x80 && (u[2]&0xc0)==0x80){
        *cp=((uint32_t)(u[0]&0x0f)<<12)|((uint32_t)(u[1]&0x3f)<<6)|(u[2]&0x3f);
        *used=3;return GM_OK;
    }
    if((u[0]&0xf8)==0xf0 && (u[1]&0xc0)==0x80 && (u[2]&0xc0)==0x80 &&
       (u[3]&0xc0)==0x80){
        *cp=((uint32_t)(u[0]&0x07)<<18)|((uint32_t)(u[1]&0x3f)<<12)|
            ((uint32_t)(u[2]&0x3f)<<6)|(u[3]&0x3f);
        *used=4;return GM_OK;
    }
    return GM_UNSUPPORTED;
}

static int read_ansi(GuestMemory *m,GuestFileApi *api,uint32_t address,
                     char *out,uint32_t capacity){
    if(!api->locale || !api->locale->installed)return GM_UNSUPPORTED;
    uint32_t len=0;
    for(uint32_t i=0;i<GFA_MAX_GUEST_PATH;i++){
        uint8_t byte;
        int rc=gm_read(m,address+i,&byte,1);
        if(rc)return rc;
        if(!byte)break;
        uint32_t cp;
        rc=gl_decode_byte(api->locale,byte,&cp);
        if(rc)return rc;
        if(len+4>=capacity)return GFA_TOO_LONG;
        uint32_t written;
        rc=utf8_encode(cp,out+len,capacity-len,&written);
        if(rc==GFA_TOO_LONG)return GFA_TOO_LONG;
        if(rc)return rc;
        len+=written;
        if(i+1==GFA_MAX_GUEST_PATH)return GFA_TOO_LONG;
    }
    out[len]=0;
    return GM_OK;
}
static int read_wide(GuestMemory *m,GuestFileApi *api,uint32_t address,
                     char *out,uint32_t capacity){
    if(!api->locale || !api->locale->installed)return GM_UNSUPPORTED;
    uint32_t len=0;
    for(uint32_t i=0;i<GFA_MAX_GUEST_PATH;i++){
        uint8_t bytes[2];
        int rc=gm_read(m,address+i*2,bytes,2);
        if(rc)return rc;
        uint32_t unit=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8);
        if(!unit)break;
        if(unit>=0xd800 && unit<=0xdfff)return GM_UNSUPPORTED;
        if(len+4>=capacity)return GFA_TOO_LONG;
        uint32_t written;
        rc=utf8_encode(unit,out+len,capacity-len,&written);
        if(rc==GFA_TOO_LONG)return GFA_TOO_LONG;
        if(rc)return rc;
        len+=written;
        if(i+1==GFA_MAX_GUEST_PATH)return GFA_TOO_LONG;
    }
    out[len]=0;
    return GM_OK;
}
/* Encodes into a host buffer with a NUL terminator; capacity excludes the NUL
 * slot which the caller must provide. Unmappable units become '?'. */
static int encode_ansi(GuestFileApi *api,const char *text,uint8_t *out,
                       uint32_t capacity,uint32_t *written){
    if(!api->locale || !api->locale->installed)return GM_UNSUPPORTED;
    uint32_t len=0,pos=0;
    while(text[pos]){
        if(len>=capacity)return GFA_TOO_LONG;
        uint32_t cp,used;
        int rc=utf8_decode(text+pos,&cp,&used);
        if(rc)return rc;
        uint8_t byte=(cp<0x10000)?api->locale->encode[cp]:0;
        if(!byte)byte='?';
        out[len++]=byte;
        pos+=used;
    }
    out[len]=0;
    *written=len;
    return GM_OK;
}
static int encode_wide(GuestFileApi *api,const char *text,uint8_t *out,
                       uint32_t capacity,uint32_t *written){
    (void)api;
    uint32_t len=0,pos=0;
    while(text[pos]){
        if(len>=capacity)return GFA_TOO_LONG;
        uint32_t cp,used;
        int rc=utf8_decode(text+pos,&cp,&used);
        if(rc)return rc;
        uint32_t unit=(cp<0x10000)?cp:'?';
        out[len*2]=(uint8_t)unit;
        out[len*2+1]=(uint8_t)(unit>>8);
        len++;
        pos+=used;
    }
    out[len*2]=0;
    out[len*2+1]=0;
    *written=len;
    return GM_OK;
}

/* Relative guest names are resolved against the runtime-owned current
 * directory; drive-relative and UNC forms are rejected. */
static int make_full_path(GuestFileApi *api,const char *guest,char *out,
                          uint32_t capacity){
    if(guest[0] && guest[1]==':'){
        size_t n=strlen(guest);
        if(n+1>capacity)return GFA_TOO_LONG;
        memcpy(out,guest,n+1);
        return GM_OK;
    }
    if(guest[0]=='\\'||guest[0]=='/')return GPM_INVALID;
    int n=snprintf(out,capacity,"%s\\%s",api->cwd,guest);
    if(n<0 || (size_t)n>=capacity)return GFA_TOO_LONG;
    return GM_OK;
}

static int resolve(GuestMemory *m,GuestFileApi *api,uint32_t address,uint32_t wide,
                   uint32_t mode,char *host,uint32_t *error){
    char guest[GFA_MAX_GUEST_PATH];
    int rc=wide ? read_wide(m,api,address,guest,sizeof(guest))
                : read_ansi(m,api,address,guest,sizeof(guest));
    if(rc==GFA_TOO_LONG){ *error=GFA_ERROR_FILENAME_EXCED_RANGE; return GM_OK; }
    if(rc)return rc;
    if(!guest[0]){ *error=GFA_ERROR_PATH_NOT_FOUND; return GM_OK; }
    char full[GFA_MAX_GUEST_PATH];
    rc=make_full_path(api,guest,full,sizeof(full));
    if(rc==GFA_TOO_LONG){ *error=GFA_ERROR_FILENAME_EXCED_RANGE; return GM_OK; }
    if(rc==GPM_INVALID){ *error=GFA_ERROR_INVALID_NAME; return GM_OK; }
    if(rc)return rc;
    rc=gpm_resolve(api->map,full,mode,host,GFA_MAX_HOST_PATH);
    *error=map_error(rc);
    return GM_OK;
}

static void file_destroy(void *object){
    GfaFile *file=(GfaFile *)object;
    if(!file)return;
    if(file->fd>=0)(void)close(file->fd);
    free(file);
}
static void find_destroy(void *object){
    GfaFind *find=(GfaFind *)object;
    if(!find)return;
    if(find->dir)(void)closedir(find->dir);
    free(find);
}
static GfaFile *file_of(uint32_t handle){
    return (GfaFile *)gh_lookup(handle,GH_KIND_FILE,NULL);
}

/* --------------------------------------------------------------- creation */

int gfa_create(GuestPathMap *map,const GuestLocale *locale,GuestFileApi **out){
    if(!map || !out)return GM_RANGE;
    GuestFileApi *api=calloc(1,sizeof(*api));
    if(!api)return GM_ALLOCATION;
    api->map=map;
    api->locale=locale;
    memcpy(api->cwd,GFA_DEFAULT_CURRENT_DIRECTORY,sizeof(GFA_DEFAULT_CURRENT_DIRECTORY));
    *out=api;
    return GM_OK;
}
void gfa_destroy(GuestFileApi *api){ free(api); }
const char *gfa_current_directory(const GuestFileApi *api){
    return api ? api->cwd : "";
}

/* ------------------------------------------------------------------ files */

static int create_file(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                       uint32_t access,uint32_t share,uint32_t security,
                       uint32_t disposition,uint32_t flags,uint32_t templ,
                       uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    (void)share;   /* one guest process: no other opener can observe sharing */
    if(templ)return GM_UNSUPPORTED;
    if(security){
        /* SECURITY_ATTRIBUTES with the default descriptor (the CRT's fopen passes
         * one to carry bInheritHandle; no child process exists to inherit). An
         * explicit descriptor is not modelled. */
        uint8_t sa[12];int rc=gm_read(m,security,sa,sizeof(sa));if(rc)return rc;
        if(sa[4]|sa[5]|sa[6]|sa[7])return GM_UNSUPPORTED;
    }
    if(access&~GFA_ACCESS_ALLOWED)return GM_UNSUPPORTED;
    if(flags&~GFA_FLAGS_ALLOWED)return GM_UNSUPPORTED;
    if(disposition<1 || disposition>5)return GM_UNSUPPORTED;
    if(disposition!=3 && !(access&0x40000000u)){
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,GFA_ERROR_ACCESS_DENIED);
    }
    char host[GFA_MAX_HOST_PATH];
    uint32_t error=0;
    uint32_t mode=(disposition==3||disposition==5)?GPM_MODE_READ:GPM_MODE_WRITE;
    int rc=resolve(m,api,name,wide,mode,host,&error);
    if(rc)return rc;
    if(error){ *result=GFA_INVALID_HANDLE_VALUE; return last_error(m,teb,error); }
    struct stat st;
    int exists=stat(host,&st)==0;
    if(exists && S_ISDIR(st.st_mode)){
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,GFA_ERROR_ACCESS_DENIED);
    }
    int oflags;
    uint32_t rw=access&(0x80000000u|0x40000000u);
    if(rw==(0x80000000u|0x40000000u))oflags=O_RDWR;
    else if(rw==0x40000000u)oflags=O_WRONLY;
    else oflags=O_RDONLY;
    switch(disposition){
    case 1: if(exists){error=GFA_ERROR_FILE_EXISTS;break;} oflags|=O_CREAT|O_EXCL;break;
    case 2: oflags|=O_CREAT|O_TRUNC;break;
    case 3: if(!exists)error=GFA_ERROR_FILE_NOT_FOUND;break;
    case 4: oflags|=O_CREAT;break;
    default: if(!exists)error=GFA_ERROR_FILE_NOT_FOUND;else oflags|=O_TRUNC;break;
    }
    if(error){ *result=GFA_INVALID_HANDLE_VALUE; return last_error(m,teb,error); }
    int fd=open(host,oflags,0666);
    if(fd<0){
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,io_error(errno,1));
    }
    GfaFile *file=calloc(1,sizeof(*file));
    if(!file){
        (void)close(fd);
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,GFA_ERROR_NOT_ENOUGH_MEMORY);
    }
    file->fd=fd;
    memcpy(file->host,host,strlen(host)+1);
    uint32_t handle=gh_insert(GH_KIND_FILE,file,file_destroy);
    if(!handle){
        file_destroy(file);
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,GFA_ERROR_NOT_ENOUGH_MEMORY);
    }
    *result=handle;
    return GM_OK;
}

int gfa_create_file_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                      uint32_t access,uint32_t share,uint32_t security,
                      uint32_t disposition,uint32_t flags,uint32_t templ,
                      uint32_t *result){
    return create_file(m,teb,api,name,access,share,security,disposition,flags,templ,0,result);
}
int gfa_create_file_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                      uint32_t access,uint32_t share,uint32_t security,
                      uint32_t disposition,uint32_t flags,uint32_t templ,
                      uint32_t *result){
    return create_file(m,teb,api,name,access,share,security,disposition,flags,templ,1,result);
}

int gfa_read_file(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t buffer,
                  uint32_t to_read,uint32_t read_out,uint32_t overlapped,
                  uint32_t *result){
    if(!m || !result)return GM_RANGE;
    if(overlapped)return GM_UNSUPPORTED;   /* asynchronous I/O is not modelled */
    GfaFile *file=file_of(handle);
    if(!file){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(!read_out){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    if(!to_read){ *result=1; return put32(m,read_out,0); }
    if(!buffer){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    int rc=gm_check(m,buffer,to_read,GM_WRITE);
    if(rc)return rc;
    uint8_t *chunk=malloc(GFA_IO_CHUNK);
    if(!chunk)return GM_ALLOCATION;
    uint32_t total=0;
    while(total<to_read){
        uint32_t want=to_read-total;
        if(want>GFA_IO_CHUNK)want=GFA_IO_CHUNK;
        ssize_t n=read(file->fd,chunk,want);
        if(n<0){
            if(errno==EINTR)continue;
            uint32_t error=io_error(errno,0);
            free(chunk);
            *result=0;
            return last_error(m,teb,error);
        }
        if(!n)break;
        rc=gm_write(m,buffer+total,chunk,(uint32_t)n);
        if(rc){ free(chunk); return rc; }
        total+=(uint32_t)n;
    }
    free(chunk);
    rc=put32(m,read_out,total);
    if(rc)return rc;
    *result=1;
    return GM_OK;
}

int gfa_write_file(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t buffer,
                   uint32_t to_write,uint32_t written_out,uint32_t overlapped,
                   uint32_t *result){
    if(!m || !result)return GM_RANGE;
    if(overlapped)return GM_UNSUPPORTED;
    GfaFile *file=file_of(handle);
    if(!file){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(!written_out){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    if(!to_write){ *result=1; return put32(m,written_out,0); }
    if(!buffer){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    int rc=gm_check(m,buffer,to_write,GM_READ);
    if(rc)return rc;
    uint8_t *chunk=malloc(GFA_IO_CHUNK);
    if(!chunk)return GM_ALLOCATION;
    uint32_t total=0;
    while(total<to_write){
        uint32_t want=to_write-total;
        if(want>GFA_IO_CHUNK)want=GFA_IO_CHUNK;
        rc=gm_read(m,buffer+total,chunk,want);
        if(rc){ free(chunk); return rc; }
        uint32_t done=0;
        while(done<want){
            ssize_t n=write(file->fd,chunk+done,want-done);
            if(n<0){
                if(errno==EINTR)continue;
                uint32_t error=io_error(errno,1);
                free(chunk);
                *result=0;
                return last_error(m,teb,error);
            }
            if(!n){ free(chunk); *result=0; return last_error(m,teb,GFA_ERROR_WRITE_FAULT); }
            done+=(uint32_t)n;
        }
        total+=want;
    }
    free(chunk);
    rc=put32(m,written_out,total);
    if(rc)return rc;
    *result=1;
    return GM_OK;
}

int gfa_set_file_pointer(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t low,
                         uint32_t high,uint32_t method,uint32_t *result){
    if(!m || !result)return GM_RANGE;
    GfaFile *file=file_of(handle);
    if(!file){ *result=GFA_INVALID_SET_FILE_POINTER;
               return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(method>2){ *result=GFA_INVALID_SET_FILE_POINTER;
                  return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    uint32_t high_value=0;
    if(high){
        int rc=get32(m,high,&high_value);
        if(rc)return rc;
    }
    int64_t distance=(int64_t)(((uint64_t)high_value<<32)|(uint64_t)low);
    int whence=method==0?SEEK_SET:(method==1?SEEK_CUR:SEEK_END);
    off_t position=lseek(file->fd,(off_t)distance,whence);
    if(position<0){
        *result=GFA_INVALID_SET_FILE_POINTER;
        return last_error(m,teb,errno==EINVAL?GFA_ERROR_NEGATIVE_SEEK:
                                               GFA_ERROR_INVALID_PARAMETER);
    }
    if(high){
        int rc=put32(m,high,(uint32_t)((uint64_t)position>>32));
        if(rc)return rc;
    }
    *result=(uint32_t)((uint64_t)position&0xffffffffu);
    return GM_OK;
}

int gfa_set_file_pointer_ex(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t low,
                            uint32_t high,uint32_t new_pointer,uint32_t method,
                            uint32_t *result){
    if(!m || !result)return GM_RANGE;
    GfaFile *file=file_of(handle);
    if(!file){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(method>2){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    int64_t distance=(int64_t)(((uint64_t)high<<32)|(uint64_t)low);
    int whence=method==0?SEEK_SET:(method==1?SEEK_CUR:SEEK_END);
    off_t position=lseek(file->fd,(off_t)distance,whence);
    if(position<0){
        *result=0;
        return last_error(m,teb,errno==EINVAL?GFA_ERROR_NEGATIVE_SEEK:
                                               GFA_ERROR_INVALID_PARAMETER);
    }
    if(new_pointer){
        int rc=put64(m,new_pointer,(uint64_t)position);
        if(rc)return rc;
    }
    *result=1;
    return GM_OK;
}

int gfa_get_file_size(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t high_out,
                      uint32_t *result){
    if(!m || !result)return GM_RANGE;
    GfaFile *file=file_of(handle);
    if(!file){ *result=GFA_INVALID_FILE_ATTRIBUTES;
               return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    struct stat st;
    if(fstat(file->fd,&st)){ *result=GFA_INVALID_FILE_ATTRIBUTES;
                             return last_error(m,teb,io_error(errno,0)); }
    uint64_t size=(uint64_t)st.st_size;
    if(high_out){
        int rc=put32(m,high_out,(uint32_t)(size>>32));
        if(rc)return rc;
    }
    *result=(uint32_t)(size&0xffffffffu);
    return GM_OK;
}

int gfa_get_file_size_ex(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t out,
                         uint32_t *result){
    if(!m || !result)return GM_RANGE;
    if(!out){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    GfaFile *file=file_of(handle);
    if(!file){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    struct stat st;
    if(fstat(file->fd,&st)){ *result=0; return last_error(m,teb,io_error(errno,0)); }
    int rc=put64(m,out,(uint64_t)st.st_size);
    if(rc)return rc;
    *result=1;
    return GM_OK;
}

/* ------------------------------------------------------------- attributes */

static int get_attributes(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                          uint32_t name,uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    char host[GFA_MAX_HOST_PATH];
    uint32_t error=0;
    int rc=resolve(m,api,name,wide,GPM_MODE_READ,host,&error);
    if(rc)return rc;
    if(error){ *result=GFA_INVALID_FILE_ATTRIBUTES; return last_error(m,teb,error); }
    struct stat st;
    if(stat(host,&st)){ *result=GFA_INVALID_FILE_ATTRIBUTES;
                        return last_error(m,teb,io_error(errno,0)); }
    *result=attributes_of(&st);
    return GM_OK;
}
int gfa_get_file_attributes_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                              uint32_t name,uint32_t *result){
    return get_attributes(m,teb,api,name,0,result);
}
int gfa_get_file_attributes_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                              uint32_t name,uint32_t *result){
    return get_attributes(m,teb,api,name,1,result);
}

static int get_attributes_ex(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                             uint32_t name,uint32_t level,uint32_t data,
                             uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    /* GetFileExInfoStandard (0) is the only level Win32 defines; any other
     * value is the documented ERROR_INVALID_PARAMETER, not a silent stub. */
    if(level){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    if(!data){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    char host[GFA_MAX_HOST_PATH];
    uint32_t error=0;
    int rc=resolve(m,api,name,wide,GPM_MODE_READ,host,&error);
    if(rc)return rc;
    if(error){ *result=0; return last_error(m,teb,error); }
    struct stat st;
    if(stat(host,&st)){ *result=0; return last_error(m,teb,io_error(errno,0)); }
    uint8_t buffer[GFA_FILE_ATTRIBUTE_DATA_SIZE];
    put32le(buffer,attributes_of(&st));
    filetime(buffer+4,st.st_ctim.tv_sec,st.st_ctim.tv_nsec);
    filetime(buffer+12,st.st_atim.tv_sec,st.st_atim.tv_nsec);
    filetime(buffer+20,st.st_mtim.tv_sec,st.st_mtim.tv_nsec);
    put32le(buffer+28,(uint32_t)((uint64_t)st.st_size>>32));
    put32le(buffer+32,(uint32_t)((uint64_t)st.st_size&0xffffffffu));
    rc=gm_write(m,data,buffer,sizeof(buffer));
    if(rc)return rc;
    *result=1;
    return GM_OK;
}
int gfa_get_file_attributes_ex_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                 uint32_t name,uint32_t level,uint32_t data,
                                 uint32_t *result){
    return get_attributes_ex(m,teb,api,name,level,data,0,result);
}
int gfa_get_file_attributes_ex_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                 uint32_t name,uint32_t level,uint32_t data,
                                 uint32_t *result){
    return get_attributes_ex(m,teb,api,name,level,data,1,result);
}

/* ------------------------------------------------------------------- find */

static int lower(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }
static int wild_match(const char *pattern,const char *name){
    if(!strcmp(pattern,"*")||!strcmp(pattern,"*.*"))return 1;
    const char *p=pattern,*n=name,*star=NULL,*resume=NULL;
    while(*n){
        if(*p=='?' || (*p && lower((unsigned char)*p)==lower((unsigned char)*n))){
            p++;n++;
        }else if(*p=='*'){
            star=p++;resume=n;
        }else if(star){
            p=star+1;n=++resume;
        }else return 0;
    }
    while(*p=='*')p++;
    return *p==0;
}

static int fill_find_data(GuestMemory *m,GuestFileApi *api,uint32_t data,uint32_t wide,
                          const char *name,const struct stat *st){
    uint32_t size=wide?GFA_FIND_DATA_W_SIZE:GFA_FIND_DATA_A_SIZE;
    uint8_t buffer[GFA_FIND_DATA_W_SIZE];
    memset(buffer,0,size);
    put32le(buffer,attributes_of(st));
    filetime(buffer+4,st->st_ctim.tv_sec,st->st_ctim.tv_nsec);
    filetime(buffer+12,st->st_atim.tv_sec,st->st_atim.tv_nsec);
    filetime(buffer+20,st->st_mtim.tv_sec,st->st_mtim.tv_nsec);
    put32le(buffer+28,(uint32_t)((uint64_t)st->st_size>>32));
    put32le(buffer+32,(uint32_t)((uint64_t)st->st_size&0xffffffffu));
    uint32_t written=0;
    int rc;
    if(wide){
        rc=encode_wide(api,name,buffer+44,259,&written);
        if(rc)return rc;
    }else{
        rc=encode_ansi(api,name,buffer+44,259,&written);
        if(rc)return rc;
    }
    return gm_write(m,data,buffer,size);
}

/* Splits "dir\leaf" into a guest directory path (relative parts prefixed with
 * the current directory) and the leaf wildcard pattern. */
static int split_pattern(GuestFileApi *api,const char *guest,char *directory,
                         uint32_t directory_capacity,char *leaf,uint32_t leaf_capacity){
    const char *last=NULL;
    for(const char *p=guest;*p;p++)if(*p=='\\'||*p=='/')last=p;
    const char *name=last?last+1:guest;
    size_t name_length=strlen(name);
    if(!name_length)return GPM_INVALID;
    if(name_length>=leaf_capacity)return GPM_TOO_LONG;
    memcpy(leaf,name,name_length+1);
    if(!last){
        size_t cwd_length=strlen(api->cwd);
        if(cwd_length+1>directory_capacity)return GPM_TOO_LONG;
        memcpy(directory,api->cwd,cwd_length+1);
        return GPM_OK;
    }
    size_t directory_length=(size_t)(last-guest);
    if(!directory_length){
        size_t cwd_length=strlen(api->cwd);
        if(cwd_length+1>directory_capacity)return GPM_TOO_LONG;
        memcpy(directory,api->cwd,cwd_length+1);
        return GPM_OK;
    }
    if(directory_length>=directory_capacity)return GPM_TOO_LONG;
    memcpy(directory,guest,directory_length);
    directory[directory_length]=0;
    return GPM_OK;
}

static int find_next(GuestMemory *m,uint32_t teb,GuestFileApi *api,GfaFind *find,
                     uint32_t data,uint32_t wide,uint32_t *result){
    if(!find->dir)return GM_UNSUPPORTED;
    struct dirent *entry;
    while((entry=readdir(find->dir))!=NULL){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        if(!wild_match(find->pattern,entry->d_name))continue;
        char path[GFA_MAX_HOST_PATH];
        int n=snprintf(path,sizeof(path),"%s/%s",find->host,entry->d_name);
        if(n<0 || (size_t)n>=sizeof(path))return GM_UNSUPPORTED;
        struct stat st;
        if(stat(path,&st))continue;      /* vanished between readdir and stat */
        int rc=fill_find_data(m,api,data,wide,entry->d_name,&st);
        if(rc)return rc;
        *result=1;
        return GM_OK;
    }
    *result=0;
    return last_error(m,teb,GFA_ERROR_NO_MORE_FILES);
}

static int find_first(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t pattern,
                      uint32_t data,uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    if(!data){ *result=GFA_INVALID_HANDLE_VALUE;
               return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    char guest[GFA_MAX_GUEST_PATH];
    int rc=wide ? read_wide(m,api,pattern,guest,sizeof(guest))
                : read_ansi(m,api,pattern,guest,sizeof(guest));
    if(rc==GFA_TOO_LONG){ *result=GFA_INVALID_HANDLE_VALUE;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc)return rc;
    if(!guest[0]){ *result=GFA_INVALID_HANDLE_VALUE;
                   return last_error(m,teb,GFA_ERROR_PATH_NOT_FOUND); }
    char directory[GFA_MAX_GUEST_PATH],leaf[GFA_MAX_LEAF];
    rc=split_pattern(api,guest,directory,sizeof(directory),leaf,sizeof(leaf));
    if(rc==GPM_INVALID||rc==GPM_TOO_LONG){
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,rc==GPM_INVALID?GFA_ERROR_PATH_NOT_FOUND:
                                                GFA_ERROR_FILENAME_EXCED_RANGE);
    }
    if(rc)return rc;
    char full[GFA_MAX_GUEST_PATH];
    rc=make_full_path(api,directory,full,sizeof(full));
    if(rc==GFA_TOO_LONG){ *result=GFA_INVALID_HANDLE_VALUE;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc==GPM_INVALID){ *result=GFA_INVALID_HANDLE_VALUE;
                         return last_error(m,teb,GFA_ERROR_INVALID_NAME); }
    if(rc)return rc;
    char host[GFA_MAX_HOST_PATH];
    rc=gpm_resolve(api->map,full,GPM_MODE_DIR,host,sizeof(host));
    if(rc){ *result=GFA_INVALID_HANDLE_VALUE; return last_error(m,teb,map_dir_error(rc)); }
    DIR *dir=opendir(host);
    if(!dir){ *result=GFA_INVALID_HANDLE_VALUE;
              return last_error(m,teb,io_error(errno,0)); }
    GfaFind *find=calloc(1,sizeof(*find));
    if(!find){ (void)closedir(dir); *result=GFA_INVALID_HANDLE_VALUE;
               return last_error(m,teb,GFA_ERROR_NOT_ENOUGH_MEMORY); }
    find->dir=dir;
    memcpy(find->pattern,leaf,strlen(leaf)+1);
    memcpy(find->host,host,strlen(host)+1);
    uint32_t found=0;
    rc=find_next(m,teb,api,find,data,wide,&found);
    if(rc){ find_destroy(find); return rc; }
    if(!found){
        find_destroy(find);
        *result=GFA_INVALID_HANDLE_VALUE;
        return last_error(m,teb,GFA_ERROR_FILE_NOT_FOUND);
    }
    uint32_t handle=gh_insert(GH_KIND_FIND,find,find_destroy);
    if(!handle){ find_destroy(find); *result=GFA_INVALID_HANDLE_VALUE;
                 return last_error(m,teb,GFA_ERROR_NOT_ENOUGH_MEMORY); }
    *result=handle;
    return GM_OK;
}

int gfa_find_first_file_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                          uint32_t pattern,uint32_t data,uint32_t *result){
    return find_first(m,teb,api,pattern,data,0,result);
}
int gfa_find_first_file_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                          uint32_t pattern,uint32_t data,uint32_t *result){
    return find_first(m,teb,api,pattern,data,1,result);
}
static int find_next_call(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t handle,
                          uint32_t data,uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    GfaFind *find=(GfaFind *)gh_lookup(handle,GH_KIND_FIND,NULL);
    if(!find){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(!data){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    return find_next(m,teb,api,find,data,wide,result);
}
int gfa_find_next_file_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t handle,
                         uint32_t data,uint32_t *result){
    return find_next_call(m,teb,api,handle,data,0,result);
}
int gfa_find_next_file_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t handle,
                         uint32_t data,uint32_t *result){
    return find_next_call(m,teb,api,handle,data,1,result);
}
int gfa_find_close(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!m || !result)return GM_RANGE;
    if(!gh_lookup(handle,GH_KIND_FIND,NULL)){
        *result=0;
        return last_error(m,teb,GFA_ERROR_INVALID_HANDLE);
    }
    if(!gh_close(handle))return GM_UNSUPPORTED;
    *result=1;
    return GM_OK;
}

/* ------------------------------------------------------------- directories */

static int create_directory(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                            uint32_t name,uint32_t security,uint32_t wide,
                            uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    if(security)return GM_UNSUPPORTED;
    char host[GFA_MAX_HOST_PATH];
    uint32_t error=0;
    int rc=resolve(m,api,name,wide,GPM_MODE_WRITE,host,&error);
    if(rc)return rc;
    if(error){ *result=0; return last_error(m,teb,error); }
    struct stat st;
    if(stat(host,&st)==0){
        *result=0;
        return last_error(m,teb,GFA_ERROR_ALREADY_EXISTS);
    }
    if(mkdir(host,0777)){
        *result=0;
        return last_error(m,teb,io_error(errno,1));
    }
    *result=1;
    return GM_OK;
}
int gfa_create_directory_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                           uint32_t name,uint32_t security,uint32_t *result){
    return create_directory(m,teb,api,name,security,0,result);
}
int gfa_create_directory_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                           uint32_t name,uint32_t security,uint32_t *result){
    return create_directory(m,teb,api,name,security,1,result);
}

static int delete_file(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                       uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    char host[GFA_MAX_HOST_PATH];
    uint32_t error=0;
    int rc=resolve(m,api,name,wide,GPM_MODE_READ,host,&error);
    if(rc)return rc;
    if(error){ *result=0; return last_error(m,teb,error); }
    struct stat st;
    if(stat(host,&st)){ *result=0; return last_error(m,teb,io_error(errno,0)); }
    if(S_ISDIR(st.st_mode)){ *result=0;
                             return last_error(m,teb,GFA_ERROR_ACCESS_DENIED); }
    if(unlink(host)){ *result=0; return last_error(m,teb,io_error(errno,1)); }
    *result=1;
    return GM_OK;
}
int gfa_delete_file_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                      uint32_t *result){
    return delete_file(m,teb,api,name,0,result);
}
int gfa_delete_file_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,uint32_t name,
                      uint32_t *result){
    return delete_file(m,teb,api,name,1,result);
}

/* ------------------------------------------------------------- handle misc */

int gfa_get_file_type(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!m || !result)return GM_RANGE;
    if(!file_of(handle)){ *result=GFA_FILE_TYPE_UNKNOWN;
                          return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    *result=GFA_FILE_TYPE_DISK;
    return GM_OK;
}
int gfa_flush_file_buffers(GuestMemory *m,uint32_t teb,uint32_t handle,
                           uint32_t *result){
    if(!m || !result)return GM_RANGE;
    GfaFile *file=file_of(handle);
    if(!file){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_HANDLE); }
    if(fsync(file->fd)){ *result=0; return last_error(m,teb,io_error(errno,1)); }
    *result=1;
    return GM_OK;
}
int gfa_close_handle(GuestMemory *m,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!m || !result)return GM_RANGE;
    uint32_t kind=GH_KIND_NONE;
    if(!gh_lookup(handle,GH_KIND_NONE,&kind)){
        *result=0;
        return last_error(m,teb,GFA_ERROR_INVALID_HANDLE);
    }
    if(kind!=GH_KIND_FILE && kind!=GH_KIND_FIND)return GM_UNSUPPORTED;
    if(!gh_close(handle))return GM_UNSUPPORTED;
    *result=1;
    return GM_OK;
}

/* ------------------------------------------------------------ current path */

static int get_current_directory(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                 uint32_t capacity,uint32_t buffer,uint32_t wide,
                                 uint32_t *result){
    (void)teb;   /* GetCurrentDirectory never sets LastError */
    if(!m || !api || !result)return GM_RANGE;
    uint32_t required;
    uint8_t encoded[GFA_MAX_GUEST_PATH*2];
    uint32_t units=0;
    int rc;
    if(wide){
        rc=encode_wide(api,api->cwd,encoded,sizeof(encoded)/2,&units);
        if(rc)return rc;
        required=units+1;
    }else{
        rc=encode_ansi(api,api->cwd,encoded,sizeof(encoded)-1,&units);
        if(rc)return rc;
        required=units+1;
    }
    if(capacity<required || !buffer){
        *result=required;
        return GM_OK;
    }
    uint32_t bytes=wide?required*2:required;
    rc=gm_write(m,buffer,encoded,bytes);
    if(rc)return rc;
    *result=required-1;
    return GM_OK;
}
int gfa_get_current_directory_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                uint32_t capacity,uint32_t buffer,uint32_t *result){
    return get_current_directory(m,teb,api,capacity,buffer,0,result);
}
int gfa_get_current_directory_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                uint32_t capacity,uint32_t buffer,uint32_t *result){
    return get_current_directory(m,teb,api,capacity,buffer,1,result);
}

static int set_current_directory(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                 uint32_t name,uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    char guest[GFA_MAX_GUEST_PATH];
    int rc=wide ? read_wide(m,api,name,guest,sizeof(guest))
                : read_ansi(m,api,name,guest,sizeof(guest));
    if(rc==GFA_TOO_LONG){ *result=0;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc)return rc;
    if(!guest[0]){ *result=0; return last_error(m,teb,GFA_ERROR_PATH_NOT_FOUND); }
    char full[GFA_MAX_GUEST_PATH],canonical[GPM_MAX_PATH];
    rc=make_full_path(api,guest,full,sizeof(full));
    if(rc==GFA_TOO_LONG){ *result=0;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc==GPM_INVALID){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_NAME); }
    if(rc)return rc;
    rc=gpm_normalize(full,canonical,sizeof(canonical));
    if(rc){ *result=0; return last_error(m,teb,map_error(rc)); }
    char host[GFA_MAX_HOST_PATH];
    rc=gpm_resolve(api->map,canonical,GPM_MODE_DIR,host,sizeof(host));
    if(rc){ *result=0; return last_error(m,teb,map_dir_error(rc)); }
    memcpy(api->cwd,canonical,strlen(canonical)+1);
    *result=1;
    return GM_OK;
}
int gfa_set_current_directory_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                uint32_t name,uint32_t *result){
    return set_current_directory(m,teb,api,name,0,result);
}
int gfa_set_current_directory_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                                uint32_t name,uint32_t *result){
    return set_current_directory(m,teb,api,name,1,result);
}

static int get_full_path_name(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                              uint32_t name,uint32_t capacity,uint32_t buffer,
                              uint32_t file_part,uint32_t wide,uint32_t *result){
    if(!m || !api || !result)return GM_RANGE;
    char guest[GFA_MAX_GUEST_PATH];
    int rc=wide ? read_wide(m,api,name,guest,sizeof(guest))
                : read_ansi(m,api,name,guest,sizeof(guest));
    if(rc==GFA_TOO_LONG){ *result=0;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc)return rc;
    if(!guest[0]){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_PARAMETER); }
    char full[GFA_MAX_GUEST_PATH],canonical[GPM_MAX_PATH];
    rc=make_full_path(api,guest,full,sizeof(full));
    if(rc==GFA_TOO_LONG){ *result=0;
                          return last_error(m,teb,GFA_ERROR_FILENAME_EXCED_RANGE); }
    if(rc==GPM_INVALID){ *result=0; return last_error(m,teb,GFA_ERROR_INVALID_NAME); }
    if(rc)return rc;
    rc=gpm_normalize(full,canonical,sizeof(canonical));
    if(rc){ *result=0; return last_error(m,teb,map_error(rc)); }
    uint32_t units=0,required;
    uint8_t encoded[GFA_MAX_GUEST_PATH*2];
    if(wide){
        rc=encode_wide(api,canonical,encoded,sizeof(encoded)/2,&units);
        required=units+1;
    }else{
        rc=encode_ansi(api,canonical,encoded,sizeof(encoded)-1,&units);
        required=units+1;
    }
    if(rc)return rc;
    if(capacity<required || !buffer){
        *result=required;
        return GM_OK;
    }
    uint32_t bytes=wide?required*2:required;
    rc=gm_write(m,buffer,encoded,bytes);
    if(rc)return rc;
    if(file_part){
        const char *leaf=canonical;
        for(const char *p=canonical;*p;p++)if(*p=='\\')leaf=p+1;
        uint32_t offset=wide?(uint32_t)((size_t)(leaf-canonical)*2)
                            :(uint32_t)(size_t)(leaf-canonical);
        rc=put32(m,file_part,buffer+offset);
        if(rc)return rc;
    }
    *result=units;
    return GM_OK;
}
int gfa_get_full_path_name_a(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                             uint32_t name,uint32_t capacity,uint32_t buffer,
                             uint32_t file_part,uint32_t *result){
    return get_full_path_name(m,teb,api,name,capacity,buffer,file_part,0,result);
}
int gfa_get_full_path_name_w(GuestMemory *m,uint32_t teb,GuestFileApi *api,
                             uint32_t name,uint32_t capacity,uint32_t buffer,
                             uint32_t file_part,uint32_t *result){
    return get_full_path_name(m,teb,api,name,capacity,buffer,file_part,1,result);
}

/* ------------------------------------------------------------ registration */

const GuestFileExport kernel32_file_exports[]={
    {"kernel32.dll","CreateFileA",28,(int (*)())gfa_create_file_a},
    {"kernel32.dll","CreateFileW",28,(int (*)())gfa_create_file_w},
    {"kernel32.dll","ReadFile",20,(int (*)())gfa_read_file},
    {"kernel32.dll","WriteFile",20,(int (*)())gfa_write_file},
    {"kernel32.dll","SetFilePointer",16,(int (*)())gfa_set_file_pointer},
    {"kernel32.dll","SetFilePointerEx",20,(int (*)())gfa_set_file_pointer_ex},
    {"kernel32.dll","GetFileSize",8,(int (*)())gfa_get_file_size},
    {"kernel32.dll","GetFileSizeEx",8,(int (*)())gfa_get_file_size_ex},
    {"kernel32.dll","GetFileAttributesA",4,(int (*)())gfa_get_file_attributes_a},
    {"kernel32.dll","GetFileAttributesW",4,(int (*)())gfa_get_file_attributes_w},
    {"kernel32.dll","GetFileAttributesExA",12,(int (*)())gfa_get_file_attributes_ex_a},
    {"kernel32.dll","GetFileAttributesExW",12,(int (*)())gfa_get_file_attributes_ex_w},
    {"kernel32.dll","FindFirstFileA",8,(int (*)())gfa_find_first_file_a},
    {"kernel32.dll","FindFirstFileW",8,(int (*)())gfa_find_first_file_w},
    {"kernel32.dll","FindNextFileA",8,(int (*)())gfa_find_next_file_a},
    {"kernel32.dll","FindNextFileW",8,(int (*)())gfa_find_next_file_w},
    {"kernel32.dll","FindClose",4,(int (*)())gfa_find_close},
    {"kernel32.dll","CreateDirectoryA",8,(int (*)())gfa_create_directory_a},
    {"kernel32.dll","CreateDirectoryW",8,(int (*)())gfa_create_directory_w},
    {"kernel32.dll","DeleteFileA",4,(int (*)())gfa_delete_file_a},
    {"kernel32.dll","DeleteFileW",4,(int (*)())gfa_delete_file_w},
    {"kernel32.dll","GetFileType",4,(int (*)())gfa_get_file_type},
    {"kernel32.dll","FlushFileBuffers",4,(int (*)())gfa_flush_file_buffers},
    {"kernel32.dll","GetCurrentDirectoryA",8,(int (*)())gfa_get_current_directory_a},
    {"kernel32.dll","GetCurrentDirectoryW",8,(int (*)())gfa_get_current_directory_w},
    {"kernel32.dll","SetCurrentDirectoryA",4,(int (*)())gfa_set_current_directory_a},
    {"kernel32.dll","SetCurrentDirectoryW",4,(int (*)())gfa_set_current_directory_w},
    {"kernel32.dll","GetFullPathNameA",16,(int (*)())gfa_get_full_path_name_a},
    {"kernel32.dll","GetFullPathNameW",16,(int (*)())gfa_get_full_path_name_w},
    {"kernel32.dll","CloseHandle",4,(int (*)())gfa_close_handle}
};
const uint32_t kernel32_file_export_count=
    (uint32_t)(sizeof(kernel32_file_exports)/sizeof(kernel32_file_exports[0]));
