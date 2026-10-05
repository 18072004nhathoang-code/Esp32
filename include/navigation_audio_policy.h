#pragma once
#include "navigation_protocol.h"
namespace navigation {
template<class Samples,class Channels>
bool valid_audio(const uint8_t *p,size_t n,Samples sample_count,Channels channels) {
    if(!p || n>MaxAudio)return false;
    size_t at=0,samples=0;
    while(at+2<=n) {
        size_t len=u16(p+at);at+=2;
        if(!len || len>1275 || len>n-at)return false;
        const int count=sample_count(p+at,len);
        if(count!=320 || channels(p+at)!=1)return false;
        samples+=count;if(samples>128000)return false;at+=len;
    }
    return at==n && samples>0;
}
inline bool may_restore(bool valid,bool was_playing,uint32_t captured,uint32_t current) {
    return valid && was_playing && captured==current;
}
}
