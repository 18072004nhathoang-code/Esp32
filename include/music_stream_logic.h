#pragma once
#include <stddef.h>
#include <string.h>
inline bool music_url_encode_query(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return false;
    static const char hex[]="0123456789ABCDEF";
    size_t d=0;
    for(size_t s=0;src[s];++s){
        const unsigned char c=static_cast<unsigned char>(src[s]);
        const bool safe=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~';
        const size_t need=(safe||c==' ')?1U:3U;
        if(d+need>=dst_size){dst[0]='\0';return false;}
        if(safe)dst[d++]=static_cast<char>(c); else if(c==' ')dst[d++]='+';
        else{dst[d++]='%';dst[d++]=hex[(c>>4)&0x0F];dst[d++]=hex[c&0x0F];}
    }
    dst[d]='\0'; return true;
}
inline bool music_stream_url_supported(const char *url,bool https_ca_configured)
{
    if(!url||!*url)return false;
    if(strncmp(url,"http://",7)==0)return true;
    return https_ca_configured&&strncmp(url,"https://",8)==0;
}
