#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum AiMusicActionType : uint8_t
{
    AI_MUSIC_ACTION_NONE = 0,
    AI_MUSIC_ACTION_PLAY,
    AI_MUSIC_ACTION_PAUSE,
    AI_MUSIC_ACTION_RESUME,
    AI_MUSIC_ACTION_STOP,
    AI_MUSIC_ACTION_VOLUME
};

struct AiMusicAction
{
    AiMusicActionType type;
    char source_id[32];
    char query[64];
    uint8_t volume;
};

inline AiMusicActionType ai_music_action_type(const char *value)
{
    if (!value) return AI_MUSIC_ACTION_NONE;
    if (strcmp(value, "music.play") == 0) return AI_MUSIC_ACTION_PLAY;
    if (strcmp(value, "music.pause") == 0) return AI_MUSIC_ACTION_PAUSE;
    if (strcmp(value, "music.resume") == 0) return AI_MUSIC_ACTION_RESUME;
    if (strcmp(value, "music.stop") == 0) return AI_MUSIC_ACTION_STOP;
    if (strcmp(value, "music.volume") == 0) return AI_MUSIC_ACTION_VOLUME;
    return AI_MUSIC_ACTION_NONE;
}

inline bool ai_music_action_valid(const AiMusicAction &action)
{
    if (action.type == AI_MUSIC_ACTION_NONE) return false;
    if (action.type == AI_MUSIC_ACTION_VOLUME)
        return action.volume <= 100 && action.source_id[0] == '\0' && action.query[0] == '\0';
    if (action.type != AI_MUSIC_ACTION_PLAY && (action.source_id[0] != '\0' || action.query[0] != '\0'))
        return false;
    return strnlen(action.source_id, sizeof(action.source_id)) < sizeof(action.source_id) &&
           strnlen(action.query, sizeof(action.query)) < sizeof(action.query);
}

inline bool ai_music_action_suppresses_auto_resume(AiMusicActionType type)
{
    return type == AI_MUSIC_ACTION_PLAY || type == AI_MUSIC_ACTION_PAUSE ||
           type == AI_MUSIC_ACTION_RESUME || type == AI_MUSIC_ACTION_STOP;
}
