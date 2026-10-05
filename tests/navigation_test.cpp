#include "navigation_protocol.h"
#include "navigation_audio_policy.h"
#include "home_gesture.h"
#include "ui_performance.h"
#include <cassert>
#include <vector>
#include <cstdio>
using namespace navigation;
static std::vector<uint8_t> frame(uint8_t kind,uint32_t sid,uint32_t seq,uint16_t total,uint16_t at,const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p(14);p[0]=1;p[1]=kind;put32(p.data()+2,sid);put32(p.data()+6,seq);p[10]=total;p[11]=total>>8;p[12]=at;p[13]=at>>8;p.insert(p.end(),data.begin(),data.end());return p;
}
int main(){
    Packet p;auto bytes=frame(Start,1,1,0,0,{});assert(parse(bytes.data(),bytes.size(),p));
    bytes[0]=2;assert(!parse(bytes.data(),bytes.size(),p));bytes[0]=1;
    assert(!parse(bytes.data(),13,p));
    bytes=frame(Audio,1,1,32769,0,{1});assert(!parse(bytes.data(),bytes.size(),p));
    bytes=frame(Update,1,1,2,2,{1});assert(!parse(bytes.data(),bytes.size(),p));
    Assembly assembly;uint8_t buffer[10]={};
    bytes=frame(Update,1,2,4,0,{1,2});assert(parse(bytes.data(),bytes.size(),p));assert(assembly.add(p,buffer,sizeof(buffer)));assert(!assembly.complete());
    assert(!assembly.add(p,buffer,sizeof(buffer))); // duplicate first fragment
    bytes=frame(Update,1,2,4,3,{4});assert(parse(bytes.data(),bytes.size(),p));assert(!assembly.add(p,buffer,sizeof(buffer)));
    bytes=frame(Update,1,2,4,2,{3,4});assert(parse(bytes.data(),bytes.size(),p));assert(assembly.add(p,buffer,sizeof(buffer)));assert(assembly.complete());
    assert(buffer[3]==4);assert(!assembly.add(p,buffer,sizeof(buffer)));
    assembly.reset();assert(!assembly.add(p,buffer,sizeof(buffer)));
    Session s;assert(s.start(123,100));assert(!s.start(456,101));assert(s.update(2,200));assert(!s.update(2,201));assert(!s.update(1,201));
    assert(s.fresh(6099));assert(!s.fresh(6100));assert(s.awake(30099));assert(!s.awake(30100));s.stop();assert(!s.fresh(200));assert(s.start(456,1000));assert(s.sequence==0);
    assert(s.start(0,0)==false);
    Guidance g;std::vector<uint8_t> body(13);put32(body.data(),7);body[4]=2;put32(body.data()+5,200);
    const char *text="Rẽ trái";body[9]=strlen(text);body.insert(body.end(),text,text+strlen(text));assert(decode(body.data(),body.size(),g));assert(g.cue==7 && g.meters==200 && g.maneuver==2);
    assert(!decode(body.data(),body.size()-1,g));body.push_back(0);assert(!decode(body.data(),body.size(),g));
    const uint8_t broken[]={0xe0,0x80,0x80};assert(!utf8(broken,sizeof(broken)));const uint8_t surrogate[]={0xed,0xa0,0x80};assert(!utf8(surrogate,3));
    HomeGesture swipe;assert(!swipe.sample(true,100,280));assert(!swipe.sample(true,100,100));swipe.sample(false,0,0);assert(!swipe.fired);
    assert(swipe.sample(true,100,312));assert(swipe.sample(true,100,260));swipe.sample(false,0,0);assert(swipe.fired);
    swipe.sample(true,100,312);swipe.sample(true,200,260);swipe.sample(false,0,0);assert(!swipe.fired);
    swipe.sample(true,100,312);swipe.reset();swipe.sample(false,0,0);assert(!swipe.fired);
    ui_performance::Histogram h;for(unsigned i=0;i<95;++i)h.add(10000);for(unsigned i=0;i<5;++i)h.add(220000);assert(h.p95_ms()==10 && h.over_200ms==5 && h.max_us==220000);
    const uint8_t audio[]={1,0,42};auto samples=[](const uint8_t*,size_t){return 320;};auto mono=[](const uint8_t*){return 1;};
    assert(valid_audio(audio,3,samples,mono));assert(!valid_audio(audio,2,samples,mono));assert(!valid_audio(audio,0,samples,mono));
    assert(!valid_audio(audio,3,[](const uint8_t*,size_t){return -1;},mono));
    assert(!valid_audio(audio,3,samples,[](const uint8_t*){return 2;}));
    std::vector<uint8_t> long_audio;for(unsigned i=0;i<401;++i)long_audio.insert(long_audio.end(),audio,audio+3);
    assert(!valid_audio(long_audio.data(),long_audio.size(),samples,mono));
    assert(may_restore(true,true,4,4));assert(!may_restore(true,true,4,5));assert(!may_restore(true,false,4,4));assert(!may_restore(false,true,4,4));
    puts("Navigation implementation: framing, fragments, UTF-8, lifecycle, stale, gesture, metrics PASS");
}
