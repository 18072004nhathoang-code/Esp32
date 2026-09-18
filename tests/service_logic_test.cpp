#include "service_state_logic.h"

#include <assert.h>

int main()
{
    // WiFi Connect(generation 7) is invalidated by Forget(generation 8)
    // immediately before the worker would call WiFi.begin().
    assert(service_generation_current(7, 7, false));
    assert(!service_generation_current(7, 8, false));
    assert(!service_generation_current(7, 7, true));

    // AI timeout/cancel invalidates its queued START_RECORDING generation;
    // a new start ordered after that stop remains current.
    assert(!service_generation_current(21, 22, false));
    assert(service_generation_current(23, 23, false));

    // The EOF callback only contributes one event. After worker cleanup marks
    // the old track stopped, observing the same event cannot enqueue NEXT again.
    assert(music_eof_next_index(true, 0, 2) == 1);
    assert(music_eof_next_index(true, 1, 2) == 0);
    assert(music_eof_next_index(false, 0, 2) == -1);

    // Playback A cleanup must not change PA/ownership after Playback B owns a
    // newer session.
    assert(audio_session_cleanup_current(3, 3, 11, 11));
    assert(!audio_session_cleanup_current(3, 3, 12, 11));
    assert(!audio_session_cleanup_current(4, 3, 11, 11));

    // Stop remains pending after five timeouts and is acknowledged only after
    // the old backend actually exits. Start does not ACK on failure.
    assert(camera_control_attempt(false, false, 5) == ServiceAttemptResult::WAIT_LATE_EXIT);
    assert(camera_control_attempt(false, true, 5) == ServiceAttemptResult::APPLIED);
    assert(camera_control_attempt(true, false, 5) == ServiceAttemptResult::REJECTED);
    assert(camera_control_attempt(true, false, 2) == ServiceAttemptResult::RETRY);
    assert(!camera_late_exit_should_apply_current(true, false, 5, 3));
    assert(camera_late_exit_should_apply_current(true, true, 5, 3));
    assert(!camera_late_exit_should_apply_current(true, true, 5, 5));

    // A cancelled/old AI worker can clear only its own active request; a new
    // request remains active.
    assert(ai_cleanup_must_clear(9, 9));
    assert(!ai_cleanup_must_clear(8, 9));

    // Settings A commits, B fails, and UI polls only after both completions:
    // revision 2 still forces reconciliation from the committed snapshot A.
    assert(settings_revision_needs_reconcile(2, 0));
    assert(!settings_revision_needs_reconcile(2, 2));

    // WAV/cache replacement requires an exact complete temporary file and a
    // recoverable old-file backup. Partial writes preserve the old file.
    assert(transactional_replace_can_commit(100, 100, true, true));
    assert(!transactional_replace_can_commit(100, 99, true, true));
    assert(!transactional_replace_can_commit(100, 100, false, true));
    assert(!transactional_replace_can_commit(100, 100, true, false));

    // Map A arriving after request B is stale for both publish and cache.
    assert(!service_generation_current(41, 42, false));
    assert(service_generation_current(42, 42, false));

    // Cancel A applies to A but cannot cancel the newer Start B.
    assert(audio_control_applies_to_generation(10, 10));
    assert(audio_control_applies_to_generation(10, 11));
    assert(!audio_control_applies_to_generation(12, 11));

    // A timed-out pause revokes its request; a late ACK is rejected.
    assert(audio_pause_ack_is_current(7, 7, true));
    assert(!audio_pause_ack_is_current(7, 8, true));
    assert(!audio_pause_ack_is_current(7, 7, false));

    // Fault-injected rename/verify failures never delete an untouched final,
    // and retain valid backup/temp files whose restore rename failed.
    assert(!transactional_remove_new_final(false, false));
    assert(transactional_remove_new_final(true, false));
    assert(!transactional_remove_new_final(true, true));
    assert(transactional_keep_recovery_file(true, false));
    assert(!transactional_keep_recovery_file(false, false));

    // Queue saturation cannot turn an accepted Forget into a credential save.
    assert(wifi_connect_may_save(true, false, false));
    assert(!wifi_connect_may_save(true, true, false));
    assert(!wifi_connect_may_save(true, false, true));

    // Dừng -> Lưu & Kết nối is complete only after configure, save and start.
    assert(camera_config_transaction_complete(true, true, true, true));
    assert(!camera_config_transaction_complete(true, true, true, false));
    assert(camera_config_transaction_complete(true, true, false, false));

    // AI cancel queue-full path retries until an ACK request is installed.
    assert(ai_cancel_retry_due(true, 0, 100, 100, 5000));
    assert(!ai_cancel_retry_due(true, 9, 100, 100, 5000));
    assert(!ai_cancel_retry_due(false, 0, 100, 100, 5000));

    // Configure is valid only for the exact desired-state revision/session.
    assert(camera_config_revision_current(3, 3, 7, 7, 11, 11, true));
    assert(!camera_config_revision_current(3, 3, 7, 7, 11, 12, true));
    assert(!camera_config_revision_current(3, 3, 7, 8, 11, 11, true));
    assert(!camera_config_revision_current(3, 3, 7, 7, 11, 11, false));

    // Startup never replaces another app or a password form the user opened.
    assert(wifi_startup_may_open(true, false, true, false));
    assert(!wifi_startup_may_open(true, false, false, false));
    assert(!wifi_startup_may_open(true, false, true, true));
    assert(!wifi_startup_may_open(true, true, true, false));
    return 0;
}
