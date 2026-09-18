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
bool camera_late_exit_should_apply_current(bool waiting_for_exit, bool backend_exited,
                                           uint32_t requested_revision,
                                           uint32_t acknowledged_revision);
bool ai_cleanup_must_clear(uint32_t completed_request, uint32_t active_request);
bool settings_revision_needs_reconcile(uint32_t completed_revision,
                                       uint32_t applied_revision);
bool transactional_replace_can_commit(size_t expected_bytes, size_t written_bytes,
                                      bool temp_valid, bool backup_ready);
bool audio_control_applies_to_generation(uint32_t active_generation,
                                         uint32_t cancel_through_generation);
bool audio_pause_ack_is_current(uint32_t worker_request_id, uint32_t current_request_id,
                                bool pause_still_requested);
bool transactional_remove_new_final(bool new_file_installed, bool commit_verified);
bool transactional_keep_recovery_file(bool file_valid, bool rename_succeeded);
bool wifi_connect_may_save(bool save_requested, bool forget_pending, bool forgetting);
bool camera_config_transaction_complete(bool configure_ok, bool save_ok,
                                        bool run_service, bool start_ok);
bool audio_music_handoff_can_grant(bool pause_acked, bool uninstall_ok);
bool audio_duplex_restore_ready(bool driver_installed, bool codec_configured);
bool ai_cancel_retry_due(bool pending, uint32_t control_request_id,
                         uint32_t now_ms, uint32_t retry_after_ms,
                         uint32_t deadline_ms);
bool camera_config_revision_current(uint32_t request_id, uint32_t latest_request_id,
                                    uint32_t session_id, uint32_t latest_session_id,
                                    uint32_t expected_revision, uint32_t actual_revision,
                                    bool active);
bool wifi_startup_may_open(bool deadline_reached, bool connected,
                           bool home_active, bool wifi_app_ever_opened);
