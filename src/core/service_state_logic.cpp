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

bool camera_late_exit_should_apply_current(bool waiting_for_exit, bool backend_exited,
                                           uint32_t requested_revision,
                                           uint32_t acknowledged_revision)
{
    return waiting_for_exit && backend_exited && requested_revision != acknowledged_revision;
}

bool ai_cleanup_must_clear(uint32_t completed_request, uint32_t active_request)
{
    return completed_request != 0 && completed_request == active_request;
}

bool settings_revision_needs_reconcile(uint32_t completed_revision,
                                       uint32_t applied_revision)
{
    return completed_revision != applied_revision;
}

bool transactional_replace_can_commit(size_t expected_bytes, size_t written_bytes,
                                      bool temp_valid, bool backup_ready)
{
    return expected_bytes > 0 && written_bytes == expected_bytes && temp_valid && backup_ready;
}

bool audio_control_applies_to_generation(uint32_t active_generation,
                                         uint32_t cancel_through_generation)
{
    return cancel_through_generation == 0 || active_generation == 0 ||
           static_cast<int32_t>(active_generation - cancel_through_generation) <= 0;
}

bool audio_pause_ack_is_current(uint32_t worker_request_id, uint32_t current_request_id,
                                bool pause_still_requested)
{
    return worker_request_id != 0 && worker_request_id == current_request_id &&
           pause_still_requested;
}

bool transactional_remove_new_final(bool new_file_installed, bool commit_verified)
{
    return new_file_installed && !commit_verified;
}

bool transactional_keep_recovery_file(bool file_valid, bool rename_succeeded)
{
    return file_valid && !rename_succeeded;
}

bool wifi_connect_may_save(bool save_requested, bool forget_pending, bool forgetting)
{
    return save_requested && !forget_pending && !forgetting;
}

bool camera_config_transaction_complete(bool configure_ok, bool save_ok,
                                        bool run_service, bool start_ok)
{
    return configure_ok && save_ok && (!run_service || start_ok);
}

bool audio_music_handoff_can_grant(bool pause_acked, bool uninstall_ok)
{
    return pause_acked && uninstall_ok;
}

bool audio_duplex_restore_ready(bool driver_installed, bool codec_configured)
{
    return driver_installed && codec_configured;
}

bool ai_cancel_retry_due(bool pending, uint32_t control_request_id,
                         uint32_t now_ms, uint32_t retry_after_ms,
                         uint32_t deadline_ms)
{
    return pending && control_request_id == 0 && deadline_ms != 0 &&
           static_cast<int32_t>(now_ms - retry_after_ms) >= 0;
}

bool camera_config_revision_current(uint32_t request_id, uint32_t latest_request_id,
                                    uint32_t session_id, uint32_t latest_session_id,
                                    uint32_t expected_revision, uint32_t actual_revision,
                                    bool active)
{
    return request_id != 0 && request_id == latest_request_id && session_id != 0 &&
           session_id == latest_session_id && expected_revision != 0 &&
           expected_revision == actual_revision && active;
}

bool wifi_startup_may_open(bool deadline_reached, bool connected,
                           bool home_active, bool wifi_app_ever_opened)
{
    return deadline_reached && !connected && home_active && !wifi_app_ever_opened;
}
