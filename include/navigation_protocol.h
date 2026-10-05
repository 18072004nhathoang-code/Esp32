#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace navigation {
constexpr size_t Header = 14, MaxText = 480, MaxAudio = 32768;
enum Kind : uint8_t { Start=1, Update=2, Heartbeat=3, End=4, Audio=5 };
enum Result : uint8_t { Received=1, Playing=2, Finished=3, Invalid=4, Busy=5, Stale=6, Failed=7 };
inline uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t(p[1])<<8); }
inline uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t(u16(p+2))<<16); }
inline void put32(uint8_t *p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(8*i)); }
inline bool utf8(const uint8_t *p,size_t n) {
    for(size_t i=0;i<n;) {
        uint32_t c=p[i++]; unsigned count=0; uint32_t min=0;
        if(c<128) { if(c<32) return false; continue; }
        if(c>=0xc2 && c<=0xdf) { count=1; c&=31; min=128; }
        else if(c>=0xe0 && c<=0xef) { count=2;c&=15;min=2048; }
        else if(c>=0xf0 && c<=0xf4) { count=3;c&=7;min=65536; }
        else return false;
        if(i+count>n) return false;
        while(count--) { if((p[i]&0xc0)!=0x80) return false; c=(c<<6)|(p[i++]&63); }
        if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return false;
    } return true;
}
struct Packet { uint8_t kind; uint32_t session,seq; uint16_t total,offset; const uint8_t *data; size_t size; };
inline bool parse(const uint8_t *p,size_t n,Packet &o) {
    if(!p || n<Header || n>244 || p[0]!=1 || p[1]<Start || p[1]>Audio) return false;
    o={p[1],u32(p+2),u32(p+6),u16(p+10),u16(p+12),p+Header,n-Header};
    const size_t limit=o.kind==Audio?MaxAudio:MaxText;
    return o.session && o.seq && o.total<=limit && o.offset<=o.total && o.size<=o.total-o.offset &&
        ((o.kind==Update || o.kind==Audio)?o.total>0:o.total==0);
}
struct Guidance {
    uint32_t cue=0;
    uint8_t maneuver=0; // 0 unknown, 1 straight, 2 left, 3 right, 4 U-turn, 5 arrive, 6 roundabout
    uint32_t meters=UINT32_MAX;
    char instruction[321]={},road[97]={},eta[49]={};
};
inline bool decode(const uint8_t *p,size_t n,Guidance &out) {
    if(n<13 || p[4]>6) return false;
    const size_t a=u16(p+9),b=p[11],c=p[12];
    if(!a || a>320 || b>96 || c>48 || 13+a+b+c!=n || !utf8(p+13,a+b+c)) return false;
    Guidance next;next.cue=u32(p); next.maneuver=p[4];next.meters=u32(p+5);
    memcpy(next.instruction,p+13,a);memcpy(next.road,p+13+a,b);memcpy(next.eta,p+13+a+b,c);
    out=next; return true;
}
class Assembly {
    uint32_t seq_=0; size_t total_=0,used_=0;
public:
    void reset() { seq_=0;total_=used_=0; }
    bool add(const Packet &p,uint8_t *dest,size_t capacity) {
        if(!dest || p.total>capacity || !p.size) return false;
        if(p.offset==0) {
            if(seq_ && p.seq<=seq_) return false;
            seq_=p.seq;total_=p.total;used_=0;
        }
        if(p.seq!=seq_ || p.total!=total_ || p.offset!=used_ || p.size>total_-used_) return false;
        memcpy(dest+used_,p.data,p.size);used_+=p.size;return true;
    }
    bool complete() const { return total_ && used_==total_; }
    size_t size() const { return used_; }
};
struct Session {
    uint32_t id=0,sequence=0,last_heartbeat=0,updated=0;
    bool active=false;
    bool fresh(uint32_t now) const { return active && uint32_t(now-last_heartbeat)<6000; }
    bool awake(uint32_t now) const { return active && uint32_t(now-last_heartbeat)<30000; }
    bool start(uint32_t session,uint32_t now) {
        if(!session || active) return false;
        id=session;sequence=0;last_heartbeat=updated=now;active=true;return true;
    }
    bool update(uint32_t seq,uint32_t now) {
        if(!active || seq<=sequence) return false;
        sequence=seq;updated=now;return true;
    }
    void stop() { active=false; }
};
} // namespace navigation
