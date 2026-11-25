/* msvcp140.dll bridges: the C++ standard library entry points the recovered
 * modules import.
 *
 * The port has no C++ object model and no SEH/unwind model. Every imported
 * msvcp140 entry point is therefore a *named stop* that reports the mangled
 * name - never a silent success, never a fabricated stream/locale object.
 *
 * The four exception throwers (std::_Xbad_alloc, std::_Xbad_function_call,
 * std::_Xlength_error, std::_Xout_of_range) additionally read the guest
 * message pointer where the import passes one, so a stop names the exact
 * standard-library error text the original would have thrown.
 *
 * The data imports (the six vtables, ?_BADOFF@std@@3_JB and the codecvt
 * locale::id) are not functions at all; a bridge cannot return a data address,
 * so they are named stops as well. */
#include "guest_crt.h"
#include <stdio.h>
#include <string.h>

static int stop_named(GuestCrt *crt,const char *mangled){
    char reason[256];
    snprintf(reason,sizeof(reason),"msvcp140: %s",mangled);
    return gcrt_stop(crt,reason);
}
static int stop_throw(GuestCrt *crt,uint32_t sp,const char *mangled,const char *type){
    uint32_t message=0;
    char text[128];
    text[0]=0;
    if(!gcrt_arg32(crt,sp,0,&message) && message){
        uint32_t length=0;
        if(!gcrt_strlen_ansi(crt,message,sizeof(text)-1,&length) &&
           !gm_read(gcrt_memory(crt),message,text,length+1))
            text[length]=0;
    }
    char reason[256];
    if(text[0])snprintf(reason,sizeof(reason),"msvcp140: %s(\"%s\") [%s]",type,text,
                        mangled);
    else snprintf(reason,sizeof(reason),"msvcp140: %s [%s]",type,mangled);
    return gcrt_stop(crt,reason);
}

/* One bridge per imported entry point. The comment names the C++ entity. */
#define M(ident,mangled) \
    static int msvcp_##ident(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){ \
        (void)sp;(void)ret;return stop_named(crt,mangled); }

/* std::basic_ostream<char>::basic_ostream(basic_streambuf<char>*,bool) */
M(_basic_ostream_ctor,"??0?$basic_ostream@DU?$char_traits@D@std@@@std@@QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z")
/* std::basic_streambuf<char>::basic_streambuf() */
M(_basic_streambuf_ctor,"??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAE@XZ")
/* std::codecvt<wchar_t,char,mbstate_t>::codecvt(unsigned) */
M(_codecvt_ctor,"??0?$codecvt@_WDU_Mbstatet@@@std@@QAE@I@Z")
/* std::basic_ostream<char>::~basic_ostream() */
M(_basic_ostream_dtor,"??1?$basic_ostream@DU?$char_traits@D@std@@@std@@UAE@XZ")
/* std::basic_streambuf<char>::~basic_streambuf() */
M(_basic_streambuf_dtor,"??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UAE@XZ")
/* std::_Yarn<char>::operator=(const char*) */
M(_Yarn_assign,"??4?$_Yarn@D@std@@QAEAAV01@PBD@Z")
/* std::locale::id::operator unsigned int() */
M(_locale_id_convert,"??Bid@locale@std@@QAEIXZ")
/* data: std::basic_ios<char> vtable */
M(_vtable_basic_ios,"??_7?$basic_ios@DU?$char_traits@D@std@@@std@@6B@")
/* data: std::codecvt<wchar_t,char,mbstate_t> vtable */
M(_vtable_codecvt,"??_7?$codecvt@_WDU_Mbstatet@@@std@@6B@")
/* data: std::_Facet_base vtable */
M(_vtable_Facet_base,"??_7_Facet_base@std@@6B@")
/* data: std::codecvt_base vtable */
M(_vtable_codecvt_base,"??_7codecvt_base@std@@6B@")
/* data: std::locale::facet vtable */
M(_vtable_facet,"??_7facet@locale@std@@6B@")
/* data: std::ios_base vtable */
M(_vtable_ios_base,"??_7ios_base@std@@6B@")
/* std::locale::_Locimp::_Addfac(std::locale::facet*,unsigned) */
M(_Addfac,"?_Addfac@_Locimp@locale@std@@AAEXPAVfacet@23@I@Z")
/* data: std::_BADOFF (const __int64) */
M(_BADOFF,"?_BADOFF@std@@3_JB")
/* std::locale::_Init(bool) */
M(_Init_locale,"?_Init@locale@std@@CAPAV_Locimp@12@_N@Z")
/* std::ios_base::_Ios_base_dtor(std::ios_base*) */
M(_Ios_base_dtor,"?_Ios_base_dtor@ios_base@std@@CAXPAV12@@Z")
/* std::locale::_Locimp::_New_Locimp(const std::locale::_Locimp&) */
M(_New_Locimp,"?_New_Locimp@_Locimp@locale@std@@CAPAV123@ABV123@@Z")
/* std::basic_ostream<char>::_Osfx() */
M(_Osfx,"?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEXXZ")
/* std::basic_streambuf<char>::_Pninc() */
M(_Pninc,"?_Pninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAEPADXZ")
/* std::basic_ostream<char>::flush() */
M(_flush,"?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ")
/* data: std::codecvt<wchar_t,char,mbstate_t>::id */
M(_codecvt_id,"?id@?$codecvt@_WDU_Mbstatet@@@std@@2V0locale@2@A")
/* std::basic_ios<char>::setstate(int,bool) */
M(_setstate,"?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QAEXH_N@Z")
/* std::basic_streambuf<char>::sputc(char) */
M(_sputc,"?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z")

static int msvcp_Xbad_alloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return stop_named(crt,"?_Xbad_alloc@std@@YAXXZ");
}
static int msvcp_Xbad_function_call(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return stop_named(crt,"?_Xbad_function_call@std@@YAXXZ");
}
static int msvcp_Xlength_error(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)ret;
    return stop_throw(crt,sp,"?_Xlength_error@std@@YAXPBD@Z","std::_Xlength_error");
}
static int msvcp_Xout_of_range(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)ret;
    return stop_throw(crt,sp,"?_Xout_of_range@std@@YAXPBD@Z","std::_Xout_of_range");
}

#define MSVCP_DLL "msvcp140.dll"
const GcrtExport msvcp_exports[]={
    {MSVCP_DLL,"??0?$basic_ostream@DU?$char_traits@D@std@@@std@@QAE@PAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z",0,msvcp__basic_ostream_ctor},
    {MSVCP_DLL,"??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAE@XZ",0,msvcp__basic_streambuf_ctor},
    {MSVCP_DLL,"??0?$codecvt@_WDU_Mbstatet@@@std@@QAE@I@Z",0,msvcp__codecvt_ctor},
    {MSVCP_DLL,"??1?$basic_ostream@DU?$char_traits@D@std@@@std@@UAE@XZ",0,msvcp__basic_ostream_dtor},
    {MSVCP_DLL,"??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UAE@XZ",0,msvcp__basic_streambuf_dtor},
    {MSVCP_DLL,"??4?$_Yarn@D@std@@QAEAAV01@PBD@Z",0,msvcp__Yarn_assign},
    {MSVCP_DLL,"??Bid@locale@std@@QAEIXZ",0,msvcp__locale_id_convert},
    {MSVCP_DLL,"??_7?$basic_ios@DU?$char_traits@D@std@@@std@@6B@",0,msvcp__vtable_basic_ios},
    {MSVCP_DLL,"??_7?$codecvt@_WDU_Mbstatet@@@std@@6B@",0,msvcp__vtable_codecvt},
    {MSVCP_DLL,"??_7_Facet_base@std@@6B@",0,msvcp__vtable_Facet_base},
    {MSVCP_DLL,"??_7codecvt_base@std@@6B@",0,msvcp__vtable_codecvt_base},
    {MSVCP_DLL,"??_7facet@locale@std@@6B@",0,msvcp__vtable_facet},
    {MSVCP_DLL,"??_7ios_base@std@@6B@",0,msvcp__vtable_ios_base},
    {MSVCP_DLL,"?_Addfac@_Locimp@locale@std@@AAEXPAVfacet@23@I@Z",0,msvcp__Addfac},
    {MSVCP_DLL,"?_BADOFF@std@@3_JB",0,msvcp__BADOFF},
    {MSVCP_DLL,"?_Init@locale@std@@CAPAV_Locimp@12@_N@Z",0,msvcp__Init_locale},
    {MSVCP_DLL,"?_Ios_base_dtor@ios_base@std@@CAXPAV12@@Z",0,msvcp__Ios_base_dtor},
    {MSVCP_DLL,"?_New_Locimp@_Locimp@locale@std@@CAPAV123@ABV123@@Z",0,msvcp__New_Locimp},
    {MSVCP_DLL,"?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEXXZ",0,msvcp__Osfx},
    {MSVCP_DLL,"?_Pninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IAEPADXZ",0,msvcp__Pninc},
    {MSVCP_DLL,"?_Xbad_alloc@std@@YAXXZ",0,msvcp_Xbad_alloc},
    {MSVCP_DLL,"?_Xbad_function_call@std@@YAXXZ",0,msvcp_Xbad_function_call},
    {MSVCP_DLL,"?_Xlength_error@std@@YAXPBD@Z",0,msvcp_Xlength_error},
    {MSVCP_DLL,"?_Xout_of_range@std@@YAXPBD@Z",0,msvcp_Xout_of_range},
    {MSVCP_DLL,"?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV12@XZ",0,msvcp__flush},
    {MSVCP_DLL,"?id@?$codecvt@_WDU_Mbstatet@@@std@@2V0locale@2@A",0,msvcp__codecvt_id},
    {MSVCP_DLL,"?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QAEXH_N@Z",0,msvcp__setstate},
    {MSVCP_DLL,"?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z",0,msvcp__sputc}
};
const uint32_t msvcp_export_count=sizeof(msvcp_exports)/sizeof(msvcp_exports[0]);
