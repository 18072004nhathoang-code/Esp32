#include "service_state_logic.h"

bool service_generation_current(uint32_t request_id, uint32_t current_id, bool cancelled)
{
    return request_id != 0 && request_id == current_id && !cancelled;
}

int music_eof_next_index(bool playing, int current_index, int track_count)
{
    if (!playing || current_index < 0 || track_count <= 0 || current_index >= track_count)
        return -1;
    return (current_index + 1) % track_count;
}

bool audio_session_cleanup_current(uint8_t current_owner, uint8_t requester,
                                   uint32_t current_session, uint32_t expected_session)
{
    return requester != 0 && current_owner == requester && expected_session != 0 &&
           current_session == expected_session;
}

ServiceAttemptResult camera_control_attempt(bool target_active, bool backend_reached_target,
                                            uint8_t attempts)
{
    if (backend_reached_target) return ServiceAttemptResult::APPLIED;
    if (!target_active && attempts >= 5) return ServiceAttemptResult::WAIT_LATE_EXIT;
    if (attempts >= 5) return ServiceAttemptResult::REJECTED;
    return ServiceAttemptResult::RETRY;
}

bool ai_cleanup_must_clear(uint32_t completed_request, uint32_t active_request)
{
    return completed_request != 0 && completed_request == active_request;
}

bool transactional_replace_can_commit(size_t expected_bytes, size_t written_bytes,
                                      bool temp_valid, bool backup_ready)
{
    return expected_bytes > 0 && written_bytes == expected_bytes && temp_valid && backup_ready;
}
