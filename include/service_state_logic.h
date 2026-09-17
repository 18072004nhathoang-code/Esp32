#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ServiceAttemptResult : uint8_t
{
    APPLIED = 0,
    RETRY,
    WAIT_LATE_EXIT,
    REJECTED
};

bool service_generation_current(uint32_t request_id, uint32_t current_id, bool cancelled);
int music_eof_next_index(bool playing, int current_index, int track_count);
bool audio_session_cleanup_current(uint8_t current_owner, uint8_t requester,
                                   uint32_t current_session, uint32_t expected_session);
ServiceAttemptResult camera_control_attempt(bool target_active, bool backend_reached_target,
                                            uint8_t attempts);
bool ai_cleanup_must_clear(uint32_t completed_request, uint32_t active_request);
bool transactional_replace_can_commit(size_t expected_bytes, size_t written_bytes,
                                      bool temp_valid, bool backup_ready);
