#include "navigation_service.h"
#include "navigation_audio_policy.h"
#include "../os/runtime_health.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../ai/ai_voice_service.h"
#include <opus.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <freertos/semphr.h>

namespace {
SemaphoreHandle_t mutex=nullptr;
TaskHandle_t task=nullptr;
navigation::Session session;
navigation::Assembly text_assembly,audio_assembly;
NavigationSnapshot view;
uint8_t text_buffer[navigation::MaxText];
uint8_t *incoming=nullptr,*pending=nullptr;
size_t pending_size=0;
uint32_t pending_session=0,pending_sequence=0,pending_time=0;
std::atomic<bool> speaking{false},invalidated{false};
std::atomic<bool> playback_running{false};
MusicVoiceHandoff inherited_music={};
uint32_t inherited_revision=0;
bool locked(uint32_t ms=0) { return mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(ms))==pdTRUE; }
void unlock() { xSemaphoreGive(mutex); }
void result(navigation::Result r,uint32_t sid,uint32_t seq) {
    if(locked(20)) { if(session.id==sid && view.guidance.cue==seq) {view.result=r;++view.revision;} unlock(); }
}
bool current(uint32_t sid,uint32_t seq) {
    if(invalidated.load() || !locked(10)) return false;
    bool ok=session.id==sid && view.guidance.cue==seq && session.fresh(millis());unlock();return ok;
}
bool valid_audio(const uint8_t *p,size_t n) {
    return navigation::valid_audio(p,n,[](const uint8_t *packet,size_t len){return opus_packet_get_nb_samples(packet,len,16000);},[](const uint8_t *packet){return opus_packet_get_nb_channels(packet);});
}
void play(uint8_t *data,size_t size,uint32_t sid,uint32_t seq,uint32_t received) {
    if(!valid_audio(data,size) || !current(sid,seq) || millis()-received>5000) {
        result(navigation::Invalid,sid,seq);return;
    }
    speaking=true;
    MusicVoiceHandoff handoff={};
    uint32_t music_revision=music_player_get_control_revision();
    if(locked(20)) {handoff=inherited_music;music_revision=handoff.valid?inherited_revision:music_revision;inherited_music={};unlock();}
    bool ok=ai_voice_get_active_generation()==0 && current(sid,seq);
    char error[96]={};
    MusicPlayerState state={};
    if(ok) {
        ok=music_player_copy_state(&state);
        if(ok && state.is_playing) {music_revision=music_player_get_control_revision();ok=music_player_suspend_for_voice(&handoff,2500,error,sizeof(error));}
    }
    uint32_t owner=0;
    if(ok && current(sid,seq) && audio_request_ownership(AUDIO_OWNER_NAVIGATION))
        owner=audio_get_owner_session(AUDIO_OWNER_NAVIGATION);
    ok=ok && owner!=0;
    int code=OPUS_OK;
    // Decoder state lives in PSRAM; no second microphone encoder is allocated.
    void *memory=ok?heap_caps_malloc(opus_decoder_get_size(1),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):nullptr;
    OpusDecoder *decoder=static_cast<OpusDecoder*>(memory);
    if(!decoder || (code=opus_decoder_init(decoder,16000,1))!=OPUS_OK) ok=false;
    if(ok) { audio_set_pa_for_session(AUDIO_OWNER_NAVIGATION,owner,true);result(navigation::Playing,sid,seq); }
    size_t at=0;int16_t pcm[320];
    while(ok && at<size && current(sid,seq)) {
        runtime_health_heartbeat(RUNTIME_TASK_XIAOZHI);
        size_t len=navigation::u16(data+at);at+=2;
        int count=opus_decode(decoder,data+at,len,pcm,320,0);at+=len;
        ok=count==320 && audio_write_pcm16_mono(pcm,count,100);
    }
    ok=ok && at==size && current(sid,seq);
    if(owner) {
        if(ok) ok=audio_drain_tx(250);
        audio_set_pa_for_session(AUDIO_OWNER_NAVIGATION,owner,false);
        if(!ok) audio_drain_tx(250);
        if(!audio_release_ownership_session(AUDIO_OWNER_NAVIGATION,owner))ok=false;
    }
    free(memory);
    // User Stop/Pause/source changes always invalidate automatic restoration.
    if(navigation::may_restore(handoff.valid,handoff.resume_after_voice,music_revision,music_player_get_control_revision()))
        if(!music_player_restore_after_voice(&handoff,2500,error,sizeof(error)))ok=false;
    speaking=false;
    result(ok?navigation::Finished:navigation::Failed,sid,seq);
}
void worker(void*) {
    for(;;) {
        bool cancel_ai=false;
        if(locked(20)) {
            if(invalidated.exchange(false)) {session.stop();++view.revision;}
            if(!session.fresh(millis())) {free(pending);pending=nullptr;pending_size=0;}
            if(pending && millis()-pending_time>5000) {free(pending);pending=nullptr;view.result=navigation::Failed;++view.revision;}
            cancel_ai=pending && !playback_running;
            if(!playback_running)speaking=cancel_ai;
            if(!session.active) {free(incoming);incoming=nullptr;audio_assembly.reset();}
            // Preallocate outside GATT callback, only during a navigation session.
            if(session.active && !incoming) incoming=static_cast<uint8_t*>(heap_caps_malloc(navigation::MaxAudio,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
            unlock();
        }
        if(cancel_ai && ai_voice_get_active_generation()!=0) {navigation_capture_music();ai_voice_cancel();}
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
}
bool navigation_init() {
    if(task) return true;
    if(!mutex) mutex=xSemaphoreCreateMutex();
    if(!mutex) return false;
    if(xTaskCreatePinnedToCore(worker,"Navigation",3072,nullptr,2,&task,0)!=pdPASS) {task=nullptr;return false;}
    return true;
}
bool navigation_audio_busy() {return speaking.load();}
void navigation_capture_music() {
    MusicVoiceHandoff captured={};
    const uint32_t revision=music_player_get_control_revision();
    if(ai_voice_copy_navigation_music(&captured) && locked(10)) {inherited_music=captured;inherited_revision=revision;unlock();}
}
void navigation_audio_owner_pump() {
    if(ai_voice_get_active_generation()!=0 || !locked())return;
    uint8_t *data=pending;const size_t n=pending_size;
    const uint32_t sid=pending_session,seq=pending_sequence,at=pending_time;
    if(data){pending=nullptr;pending_size=0;playback_running=true;speaking=true;}
    unlock();
    if(data){play(data,n,sid,seq,at);free(data);playback_running=false;speaking=false;}
}
void navigation_disconnect() { invalidated=true; }
bool navigation_receive(const uint8_t *bytes,size_t size) {
    navigation::Packet p;
    if(!task || invalidated || !navigation::parse(bytes,size,p) || !locked()) return false;
    bool ok=false;
    if(p.kind==navigation::Start) {
        ok=session.start(p.session,millis());
        if(ok) {view=NavigationSnapshot{};text_assembly.reset();audio_assembly.reset();}
    } else if(session.active && p.session==session.id) {
        if(p.kind==navigation::Heartbeat) {session.last_heartbeat=millis();ok=true;}
        else if(p.kind==navigation::End) {session.stop();ok=true;}
        else if(p.kind==navigation::Update && p.seq>session.sequence) {
            ok=text_assembly.add(p,text_buffer,sizeof(text_buffer));
            if(ok && text_assembly.complete()) {
                ok=navigation::decode(text_buffer,text_assembly.size(),view.guidance);
                if(ok) {session.update(p.seq,millis());view.result=navigation::Received;}
            }
        } else if(p.kind==navigation::Audio && p.seq==view.guidance.cue && session.fresh(millis())) {
            ok=audio_assembly.add(p,incoming,navigation::MaxAudio);
            if(ok && audio_assembly.complete()) {
                free(pending);pending=incoming;incoming=nullptr;pending_size=audio_assembly.size();
                pending_session=p.session;pending_sequence=p.seq;pending_time=millis();
                view.result=navigation::Received;
            }
        }
    }
    if(ok) ++view.revision;
    unlock();return ok;
}
bool navigation_snapshot(NavigationSnapshot &out) {
    if(!locked()) return false;
    out=view;out.session=session.id;out.sequence=session.sequence;
    out.active=session.active && !invalidated;out.fresh=out.active && session.fresh(millis());out.awake=out.active && session.awake(millis());
    unlock();return true;
}
void navigation_ack(uint8_t out[20]) {
    memset(out,0,20);out[0]=1;NavigationSnapshot s;
    if(!navigation_snapshot(s)) {out[1]=navigation::Busy;return;}
    out[1]=s.result;out[2]=s.active;out[3]=s.fresh;
    navigation::put32(out+4,s.session);navigation::put32(out+8,s.sequence);navigation::put32(out+12,s.revision);
}
