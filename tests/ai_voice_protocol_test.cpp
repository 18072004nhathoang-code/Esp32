#include "ai_voice_protocol.h"
#include <cassert>
#include <cstring>

int main()
{
    AiMusicAction action = {};
    action.type = ai_music_action_type("music.volume");
    action.volume = 80;
    assert(ai_music_action_valid(action));
    action.volume = 101;
    assert(!ai_music_action_valid(action));
    assert(ai_music_action_type("device.restart") == AI_MUSIC_ACTION_NONE);
    action = {};
    action.type = AI_MUSIC_ACTION_PLAY;
    std::strcpy(action.source_id, "radio1");
    assert(ai_music_action_valid(action));
    assert(ai_music_action_suppresses_auto_resume(action.type));
    action.type = AI_MUSIC_ACTION_STOP;
    assert(!ai_music_action_valid(action));
    action.source_id[0] = '\0';
    assert(ai_music_action_valid(action));
    assert(!ai_music_action_suppresses_auto_resume(AI_MUSIC_ACTION_VOLUME));
    return 0;
}
