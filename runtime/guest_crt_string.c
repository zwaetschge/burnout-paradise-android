/* UCRT string/ctype bridges (api-ms-win-crt-string-l1-1-0.dll and
 * api-ms-win-crt-multibyte-l1-1-0.dll).
 *
 * Everything is the "C" locale: ctype, tolower/toupper, _strlwr, _stricmp and
 * the wide counterparts fold ASCII only. That is exactly UCRT's behaviour
 * before setlocale() is called, which the recovered imports never do.
 *
 * The guest strings are read into bounded host buffers (1 MiB code units, a
 * named stop beyond that) and results are written back through gm_write. No
 * host pointer is ever handed to the guest.
 *
 * The *_s functions follow the documented UCRT contract: NULL arguments and a
 * zero destination size set errno=EINVAL, a destination that cannot hold the
 * result is emptied (dest[0]=0) and sets errno=ERANGE, and the return value is
 * the errno_t. No invalid parameter handler is installed, so nothing is
 * printed or aborted. */
#include "guest_crt.h"
#include <stdlib.h>
#include <string.h>

#define GCRT_MAX_STRING 0x100000u

static int ascii_lower(int c){return c>='A'&&c<='Z' ? c-'A'+'a' : c;}
static int ascii_upper(int c){return c>='a'&&c<='z' ? c-'a'+'A' : c;}

/* ---- loading guest strings ---------------------------------------- */

static int load_ansi(GuestCrt *crt,uint32_t address,char **out,uint32_t *length){
    if(!crt || !out)return GM_RANGE;
    *out=NULL;
    if(!address)return GM_RANGE;
    uint32_t n=0;
    int rc=gcrt_strlen_ansi(crt,address,GCRT_MAX_STRING,&n);
    if(rc==GM_RANGE)return gcrt_stop(crt,"crt string: unterminated ANSI string");
    if(rc)return rc;
    char *text=malloc(n+1);
    if(!text)return GM_ALLOCATION;
    rc=gm_read(gcrt_memory(crt),address,text,n+1);
    if(rc){free(text);return rc;}
    *out=text;if(length)*length=n;
    return GM_OK;
}
static int load_wide(GuestCrt *crt,uint32_t address,uint16_t **out,uint32_t *length){
    if(!crt || !out)return GM_RANGE;
    *out=NULL;
    if(!address)return GM_RANGE;
    uint32_t n=0;
    int rc=gcrt_strlen_wide(crt,address,GCRT_MAX_STRING,&n);
    if(rc==GM_RANGE)return gcrt_stop(crt,"crt string: unterminated UTF-16 string");
    if(rc)return rc;
    uint16_t *text=malloc((n+1)*sizeof(*text));
    if(!text)return GM_ALLOCATION;
    uint8_t *bytes=malloc((n+1)*2);
    if(!bytes){free(text);return GM_ALLOCATION;}
    rc=gm_read(gcrt_memory(crt),address,bytes,(n+1)*2);
    if(rc){free(bytes);free(text);return rc;}
    for(uint32_t i=0;i<=n;i++)text[i]=(uint16_t)(bytes[i*2]|(bytes[i*2+1]<<8));
    free(bytes);
    *out=text;if(length)*length=n;
    return GM_OK;
}
static int put_ansi(GuestCrt *crt,uint32_t address,const char *text,uint32_t count){
    if(!count)return GM_OK;
    return gm_write(gcrt_memory(crt),address,text,count);
}
static int put_wide(GuestCrt *crt,uint32_t address,const uint16_t *text,uint32_t count){
    if(!count)return GM_OK;
    uint8_t *bytes=malloc(count*2);
    if(!bytes)return GM_ALLOCATION;
    for(uint32_t i=0;i<count;i++){
        bytes[i*2]=(uint8_t)text[i];bytes[i*2+1]=(uint8_t)(text[i]>>8);
    }
    int rc=gm_write(gcrt_memory(crt),address,bytes,count*2);
    free(bytes);return rc;
}
static int clear_first_byte(GuestCrt *crt,uint32_t address){
    uint8_t zero=0;
    return gm_write(gcrt_memory(crt),address,&zero,1);
}

/* ---- comparisons --------------------------------------------------- */

static int crt_stricmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(rc)return rc;
    char *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_ansi(crt,a,&left,&ln);
    if(!rc)rc=load_ansi(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;;i++){
        int x=i<=ln ? ascii_lower((unsigned char)left[i]) : 0;
        int y=i<=rn ? ascii_lower((unsigned char)right[i]) : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_strnicmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b,count;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!count){ret->eax=0;return GM_OK;}
    char *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_ansi(crt,a,&left,&ln);
    if(!rc)rc=load_ansi(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;i<count;i++){
        int x=i<=ln ? ascii_lower((unsigned char)left[i]) : 0;
        int y=i<=rn ? ascii_lower((unsigned char)right[i]) : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_strncmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b,count;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!count){ret->eax=0;return GM_OK;}
    char *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_ansi(crt,a,&left,&ln);
    if(!rc)rc=load_ansi(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;i<count;i++){
        int x=i<=ln ? (unsigned char)left[i] : 0;
        int y=i<=rn ? (unsigned char)right[i] : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_strcoll(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(rc)return rc;
    char *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_ansi(crt,a,&left,&ln);
    if(!rc)rc=load_ansi(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;;i++){
        int x=i<=ln ? (unsigned char)left[i] : 0;
        int y=i<=rn ? (unsigned char)right[i] : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_mbsicmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return crt_stricmp(crt,sp,ret);   /* C locale: one byte per character */
}
static int crt_wcsicmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(rc)return rc;
    uint16_t *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_wide(crt,a,&left,&ln);
    if(!rc)rc=load_wide(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;;i++){
        int x=i<=ln ? ascii_lower(left[i]) : 0;
        int y=i<=rn ? ascii_lower(right[i]) : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_wcsnicmp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t a,b,count;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!count){ret->eax=0;return GM_OK;}
    uint16_t *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_wide(crt,a,&left,&ln);
    if(!rc)rc=load_wide(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;i<count;i++){
        int x=i<=ln ? ascii_lower(left[i]) : 0;
        int y=i<=rn ? ascii_lower(right[i]) : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}
static int crt_wcscoll_l(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    /* A locale object is not modelled; the C locale collation is ordinal. */
    uint32_t a,b,locale;
    int rc=gcrt_arg32(crt,sp,0,&a);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&b);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&locale);
    if(rc)return rc;
    if(locale)return gcrt_stop(crt,"_wcscoll_l: explicit locale object");
    uint16_t *left=NULL,*right=NULL;
    uint32_t ln=0,rn=0;
    rc=load_wide(crt,a,&left,&ln);
    if(!rc)rc=load_wide(crt,b,&right,&rn);
    if(rc){free(left);free(right);return rc;}
    int result=0;
    for(uint32_t i=0;;i++){
        int x=i<=ln ? left[i] : 0;
        int y=i<=rn ? right[i] : 0;
        if(x!=y){result=x<y?-1:1;break;}
        if(!x)break;
    }
    free(left);free(right);
    ret->eax=(uint32_t)result;return GM_OK;
}

/* ---- ctype --------------------------------------------------------- */

static int ctype_arg(GuestCrt *crt,uint32_t sp,int *value){
    uint32_t c;
    int rc=gcrt_arg32(crt,sp,0,&c);
    if(rc)return rc;
    *value=(int)c;
    return GM_OK;
}
static int crt_isalnum(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=(c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z');return GM_OK;
}
static int crt_iscntrl(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=(c>=0&&c<0x20)||c==0x7f;return GM_OK;
}
static int crt_isdigit(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=c>='0'&&c<='9';return GM_OK;
}
static int crt_isprint(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=c>=0x20&&c<0x7f;return GM_OK;
}
static int crt_isspace(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=c==' '||(c>=0x09&&c<=0x0d);return GM_OK;
}
static int crt_isxdigit(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=(c>='0'&&c<='9')||(c>='A'&&c<='F')||(c>='a'&&c<='f');return GM_OK;
}
static int crt_tolower(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=(uint32_t)ascii_lower(c);return GM_OK;
}
static int crt_toupper(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    int c;int rc=ctype_arg(crt,sp,&c);if(rc)return rc;
    ret->eax=(uint32_t)ascii_upper(c);return GM_OK;
}

/* ---- copies -------------------------------------------------------- */

static int crt_strlwr(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    char *text=NULL;
    uint32_t length=0;
    rc=load_ansi(crt,address,&text,&length);
    if(rc)return rc;
    for(uint32_t i=0;i<length;i++)text[i]=(char)ascii_lower((unsigned char)text[i]);
    rc=put_ansi(crt,address,text,length);
    free(text);
    if(rc)return rc;
    ret->eax=address;return GM_OK;
}
static int crt_strcpy_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,destsz,src;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&destsz);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&src);
    if(rc)return rc;
    if(!dest || !destsz || !src){
        if(dest && destsz)clear_first_byte(crt,dest);
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    char *text=NULL;
    uint32_t length=0;
    rc=load_ansi(crt,src,&text,&length);
    if(rc)return rc;
    if(length+1>destsz){
        clear_first_byte(crt,dest);
        free(text);
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=GCRT_ERANGE;return GM_OK;
    }
    rc=put_ansi(crt,dest,text,length+1);
    free(text);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int crt_strncpy_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,destsz,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&destsz);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&count);
    if(rc)return rc;
    if(!dest || !destsz || !src){
        if(dest && destsz)clear_first_byte(crt,dest);
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    char *text=NULL;
    uint32_t length=0;
    rc=load_ansi(crt,src,&text,&length);
    if(rc)return rc;
    uint32_t copy;
    if(count==UINT32_MAX)copy=length<destsz-1 ? length : destsz-1;   /* _TRUNCATE */
    else copy=length<count ? length : count;
    if(copy+1>destsz){
        clear_first_byte(crt,dest);
        free(text);
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=GCRT_ERANGE;return GM_OK;
    }
    rc=put_ansi(crt,dest,text,copy);
    if(!rc)rc=clear_first_byte(crt,dest+copy);
    free(text);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int crt_strcat_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,destsz,src;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&destsz);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&src);
    if(rc)return rc;
    if(!dest || !destsz || !src){
        if(dest && destsz)clear_first_byte(crt,dest);
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    char *right=NULL;
    uint32_t ln=0,rn=0;
    rc=gcrt_strlen_ansi(crt,dest,destsz,&ln);
    if(rc==GM_RANGE){
        clear_first_byte(crt,dest);
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=GCRT_ERANGE;return GM_OK;
    }
    if(rc)return rc;
    rc=load_ansi(crt,src,&right,&rn);
    if(rc)return rc;
    if(ln+rn+1>destsz){
        clear_first_byte(crt,dest);
        free(right);
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=GCRT_ERANGE;return GM_OK;
    }
    rc=put_ansi(crt,dest+ln,right,rn+1);
    free(right);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int crt_strncat(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!dest)return gcrt_stop(crt,"strncat: NULL destination");
    uint32_t ln=0;
    rc=gcrt_strlen_ansi(crt,dest,GCRT_MAX_STRING,&ln);
    if(rc==GM_RANGE)return gcrt_stop(crt,"strncat: unterminated destination");
    if(rc)return rc;
    char *text=NULL;
    uint32_t rn=0;
    rc=load_ansi(crt,src,&text,&rn);
    if(rc)return rc;
    uint32_t copy=rn<count ? rn : count;
    rc=put_ansi(crt,dest+ln,text,copy);
    if(!rc)rc=clear_first_byte(crt,dest+ln+copy);
    free(text);
    if(rc)return rc;
    ret->eax=dest;return GM_OK;
}
static int crt_strncpy(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!dest)return gcrt_stop(crt,"strncpy: NULL destination");
    if(!count){ret->eax=dest;return GM_OK;}
    char *text=NULL;
    uint32_t rn=0;
    rc=load_ansi(crt,src,&text,&rn);
    if(rc)return rc;
    uint32_t copy=rn<count ? rn : count;
    rc=put_ansi(crt,dest,text,copy);
    if(!rc && copy<count){
        uint8_t *zeros=calloc(1,count-copy);
        if(!zeros)rc=GM_ALLOCATION;
        else{
            rc=gm_write(gcrt_memory(crt),dest+copy,zeros,count-copy);
            free(zeros);
        }
    }
    free(text);
    if(rc)return rc;
    ret->eax=dest;return GM_OK;
}
static int crt_wcsncat(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!dest)return gcrt_stop(crt,"wcsncat: NULL destination");
    uint32_t ln=0;
    rc=gcrt_strlen_wide(crt,dest,GCRT_MAX_STRING,&ln);
    if(rc==GM_RANGE)return gcrt_stop(crt,"wcsncat: unterminated destination");
    if(rc)return rc;
    uint16_t *text=NULL;
    uint32_t rn=0;
    rc=load_wide(crt,src,&text,&rn);
    if(rc)return rc;
    uint32_t copy=rn<count ? rn : count;
    rc=put_wide(crt,dest+ln*2,text,copy);
    if(!rc){
        uint8_t bytes[2]={0,0};
        rc=gm_write(gcrt_memory(crt),dest+(ln+copy)*2,bytes,2);
    }
    free(text);
    if(rc)return rc;
    ret->eax=dest;return GM_OK;
}
static int crt_wcsncpy(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(!dest)return gcrt_stop(crt,"wcsncpy: NULL destination");
    if(!count){ret->eax=dest;return GM_OK;}
    uint16_t *text=NULL;
    uint32_t rn=0;
    rc=load_wide(crt,src,&text,&rn);
    if(rc)return rc;
    uint32_t copy=rn<count ? rn : count;
    rc=put_wide(crt,dest,text,copy);
    if(!rc && copy<count){
        uint8_t *zeros=calloc(1,(count-copy)*2);
        if(!zeros)rc=GM_ALLOCATION;
        else{
            rc=gm_write(gcrt_memory(crt),dest+copy*2,zeros,(count-copy)*2);
            free(zeros);
        }
    }
    free(text);
    if(rc)return rc;
    ret->eax=dest;return GM_OK;
}

/* ---- scans --------------------------------------------------------- */

static int crt_strlen(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,length=0;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    rc=gcrt_strlen_ansi(crt,address,GCRT_MAX_STRING,&length);
    if(rc==GM_RANGE)return gcrt_stop(crt,"strlen: unterminated string");
    if(rc)return rc;
    ret->eax=length;return GM_OK;
}
static int crt_strnlen(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,limit,length=0;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&limit);
    if(rc)return rc;
    if(!limit){ret->eax=0;return GM_OK;}
    rc=gcrt_strlen_ansi(crt,address,limit,&length);
    if(rc==GM_RANGE)length=limit;   /* no terminator inside the limit */
    else if(rc)return rc;
    ret->eax=length;return GM_OK;
}
static int crt_strcspn(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,reject;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&reject);
    if(rc)return rc;
    char *text=NULL,*set=NULL;
    uint32_t tn=0,sn=0;
    rc=load_ansi(crt,address,&text,&tn);
    if(!rc)rc=load_ansi(crt,reject,&set,&sn);
    if(rc){free(text);free(set);return rc;}
    int found=0;uint32_t i;
    for(i=0;i<tn;i++){
        for(uint32_t k=0;k<sn;k++)if(text[i]==set[k]){found=1;break;}
        if(found)break;
    }
    free(text);free(set);
    ret->eax=i;return GM_OK;
}
static int crt_strpbrk(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,accept;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&accept);
    if(rc)return rc;
    char *text=NULL,*set=NULL;
    uint32_t tn=0,sn=0;
    rc=load_ansi(crt,address,&text,&tn);
    if(!rc)rc=load_ansi(crt,accept,&set,&sn);
    if(rc){free(text);free(set);return rc;}
    uint32_t hit=0;
    for(uint32_t i=0;i<tn && !hit;i++)
        for(uint32_t k=0;k<sn;k++)if(text[i]==set[k]){hit=address+i;break;}
    free(text);free(set);
    ret->eax=hit;return GM_OK;
}
static int unit_in_set(uint16_t unit,const uint16_t *set,uint32_t count){
    for(uint32_t k=0;k<count;k++)if(set[k]==unit)return 1;
    return 0;
}
static int crt_wcstok(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,delim;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&delim);
    if(rc)return rc;
    uint32_t *cursor=gcrt_wcstok_cursor(crt);
    if(!cursor)return GM_RANGE;
    uint32_t start=address ? address : *cursor;
    if(!start){ret->eax=0;return GM_OK;}
    uint16_t *set=NULL;
    uint32_t sn=0;
    rc=load_wide(crt,delim,&set,&sn);
    if(rc)return rc;
    uint32_t position=start;
    for(;;){
        uint8_t bytes[2];
        rc=gm_read(gcrt_memory(crt),position,bytes,2);
        if(rc){free(set);return rc;}
        uint16_t unit=(uint16_t)(bytes[0]|(bytes[1]<<8));
        if(!unit){free(set);*cursor=0;ret->eax=0;return GM_OK;}
        if(!unit_in_set(unit,set,sn))break;
        position+=2;
    }
    uint32_t token=position;
    for(;;){
        uint8_t bytes[2];
        rc=gm_read(gcrt_memory(crt),position,bytes,2);
        if(rc){free(set);return rc;}
        uint16_t unit=(uint16_t)(bytes[0]|(bytes[1]<<8));
        if(!unit){free(set);*cursor=0;ret->eax=token;return GM_OK;}
        if(unit_in_set(unit,set,sn)){
            uint8_t zero[2]={0,0};
            rc=gm_write(gcrt_memory(crt),position,zero,2);
            free(set);
            if(rc)return rc;
            *cursor=position+2;ret->eax=token;return GM_OK;
        }
        position+=2;
    }
}

#define STRING_DLL "api-ms-win-crt-string-l1-1-0.dll"
#define MULTIBYTE_DLL "api-ms-win-crt-multibyte-l1-1-0.dll"
const GcrtExport crt_string_exports[]={
    {STRING_DLL,"_stricmp",0,crt_stricmp},
    {STRING_DLL,"_strlwr",0,crt_strlwr},
    {STRING_DLL,"_strnicmp",0,crt_strnicmp},
    {STRING_DLL,"_wcscoll_l",0,crt_wcscoll_l},
    {STRING_DLL,"_wcsicmp",0,crt_wcsicmp},
    {STRING_DLL,"_wcsnicmp",0,crt_wcsnicmp},
    {STRING_DLL,"isalnum",0,crt_isalnum},
    {STRING_DLL,"iscntrl",0,crt_iscntrl},
    {STRING_DLL,"isdigit",0,crt_isdigit},
    {STRING_DLL,"isprint",0,crt_isprint},
    {STRING_DLL,"isspace",0,crt_isspace},
    {STRING_DLL,"isxdigit",0,crt_isxdigit},
    {STRING_DLL,"strcat_s",0,crt_strcat_s},
    {STRING_DLL,"strcoll",0,crt_strcoll},
    {STRING_DLL,"strcpy_s",0,crt_strcpy_s},
    {STRING_DLL,"strcspn",0,crt_strcspn},
    {STRING_DLL,"strlen",0,crt_strlen},
    {STRING_DLL,"strncat",0,crt_strncat},
    {STRING_DLL,"strncmp",0,crt_strncmp},
    {STRING_DLL,"strncpy",0,crt_strncpy},
    {STRING_DLL,"strncpy_s",0,crt_strncpy_s},
    {STRING_DLL,"strnlen",0,crt_strnlen},
    {STRING_DLL,"strpbrk",0,crt_strpbrk},
    {STRING_DLL,"tolower",0,crt_tolower},
    {STRING_DLL,"toupper",0,crt_toupper},
    {STRING_DLL,"wcsncat",0,crt_wcsncat},
    {STRING_DLL,"wcsncpy",0,crt_wcsncpy},
    {STRING_DLL,"wcstok",0,crt_wcstok},
    {MULTIBYTE_DLL,"_mbsicmp",0,crt_mbsicmp}
};
const uint32_t crt_string_export_count=sizeof(crt_string_exports)/sizeof(crt_string_exports[0]);
