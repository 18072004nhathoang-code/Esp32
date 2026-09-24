#include "health_app.h"
#include "../ui/ui_theme.h"
#include "../audio/audio_manager.h"
#include "../audio/music_player.h"
#include "../ai/ai_voice_service.h"
#include "../os/wifi_manager.h"
#include <esp_heap_caps.h>

namespace {
lv_obj_t *s_root=nullptr,*s_internal=nullptr,*s_psram=nullptr,*s_services=nullptr;

const char *owner_name(AudioOwner owner)
{
    switch(owner){
        case AUDIO_OWNER_SYSTEM:return "System";
        case AUDIO_OWNER_PLAYBACK:return "Playback";
        case AUDIO_OWNER_RECORDER:return "Recorder";
        case AUDIO_OWNER_MUSIC:return "Music";
        case AUDIO_OWNER_AI_VOICE:return "AI";
        case AUDIO_OWNER_DIAGNOSTIC:return "Diag";
        default:return "Free";
    }
}
lv_obj_t *make_card(lv_obj_t *parent,lv_coord_t y,lv_coord_t h,uint32_t border,const char *title)
{
    lv_obj_t *c=lv_obj_create(parent);
    lv_obj_set_size(c,SCREEN_WIDTH-16,h);
    lv_obj_align(c,LV_ALIGN_TOP_MID,0,y);
    lv_obj_set_style_radius(c,10,0);
    lv_obj_set_style_bg_color(c,lv_color_hex(COLOR_CARD_BG),0);
    lv_obj_set_style_border_color(c,lv_color_hex(border),0);
    lv_obj_set_style_border_width(c,1,0);
    lv_obj_set_style_pad_all(c,7,0);
    lv_obj_clear_flag(c,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t=lv_label_create(c);
    lv_label_set_text(t,title);
    lv_obj_set_style_text_color(t,lv_color_hex(border),0);
    lv_obj_set_style_text_font(t,UI_FONT_12,0);
    lv_obj_align(t,LV_ALIGN_TOP_LEFT,0,0);
    return c;
}
lv_obj_t *make_value(lv_obj_t *c)
{
    lv_obj_t *l=lv_label_create(c);
    lv_obj_set_width(l,SCREEN_WIDTH-38);
    lv_label_set_long_mode(l,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l,lv_color_hex(COLOR_TEXT_SECONDARY),0);
    lv_obj_set_style_text_font(l,UI_FONT_12,0);
    lv_obj_align(l,LV_ALIGN_BOTTOM_LEFT,0,0);
    return l;
}
}

void health_app_open(lv_obj_t *parent)
{
    if(!parent)return;
    s_root=parent;
    lv_obj_set_style_pad_all(parent,8,0);
    lv_obj_clear_flag(parent,LV_OBJ_FLAG_SCROLLABLE);
    s_internal=make_value(make_card(parent,4,66,COLOR_ACCENT_CYAN,"Internal RAM"));
    s_psram=make_value(make_card(parent,76,66,COLOR_ACCENT_PURPLE,"Octal PSRAM"));
    s_services=make_value(make_card(parent,148,104,COLOR_ACCENT_GREEN,"Runtime Health"));
    health_app_update(system_get_stats());
}
void health_app_close(void)
{
    s_root=nullptr;s_internal=nullptr;s_psram=nullptr;s_services=nullptr;
}
void health_app_update(const SystemStats &stats)
{
    if(!s_root)return;
    (void)stats;
    const RuntimeHealthSnapshot health=system_get_runtime_health();
    if(s_internal)lv_label_set_text_fmt(s_internal,"Free %u KB • Largest %u KB\nLow-water %u KB",
        (unsigned)(health.internal_free_bytes/1024U),
        (unsigned)(health.internal_largest_free_bytes/1024U),
        (unsigned)(health.internal_min_free_bytes/1024U));
    if(s_psram)lv_label_set_text_fmt(s_psram,"Free %.1f MB • Largest %.1f MB\nLow-water %.1f MB",
        (double)health.psram_free_bytes/1048576.0,
        (double)health.psram_largest_free_bytes/1048576.0,
        (double)health.psram_min_free_bytes/1048576.0);
    char voice_state[96] = {};
    if (ai_voice_is_connected()) strlcpy(voice_state, "WARM", sizeof(voice_state));
    else if (!ai_voice_copy_state_text(voice_state, sizeof(voice_state)))
        strlcpy(voice_state, "BUSY", sizeof(voice_state));
    uint32_t min_stack=UINT32_MAX;
    for(uint8_t i=0;i<RUNTIME_TASK_COUNT;++i)
        if(health.tasks[i].seen&&health.tasks[i].stack_free_bytes<min_stack)
            min_stack=health.tasks[i].stack_free_bytes;
    if(min_stack==UINT32_MAX)min_stack=0;
    if(s_services)lv_label_set_text_fmt(s_services,
        "WiFi %s • Audio %s\nMusic %s • Xiaozhi %s\nQ %lu/%lu • drops %lu/%lu\nStack %uK • LVGL %uK/%u%%",
        wifi_manager_is_connected()?"OK":"OFF",owner_name(audio_get_current_owner()),
        music_player_is_playing()?"PLAY":(music_player_is_paused()?"PAUSE":"IDLE"),
        voice_state,
        (unsigned long)health.voice_uplink_queue_depth,
        (unsigned long)health.voice_inbound_queue_depth,
        (unsigned long)health.voice_uplink_drops,
        (unsigned long)health.voice_inbound_drops,
        (unsigned)(min_stack/1024U),
        (unsigned)(health.lvgl_free_bytes/1024U),
        (unsigned)health.lvgl_fragmentation_percent);
}
