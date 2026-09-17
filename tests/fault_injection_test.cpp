#include "service_state_logic.h"

#include <assert.h>

int main()
{
    // WAV/cache transaction: failed old->bak rename never authorizes deletion
    // of the old final; partial writes never reach commit.
    assert(!transactional_replace_can_commit(1044, 1000, false, true));
    assert(!transactional_replace_can_commit(1044, 1044, true, false));
    assert(!transactional_remove_new_final(false, false));
    assert(transactional_remove_new_final(true, false));
    assert(transactional_keep_recovery_file(true, false));

    // Cancel A may clean A while Start B, issued later, remains untouched.
    assert(audio_control_applies_to_generation(100, 101));
    assert(!audio_control_applies_to_generation(102, 101));

    // Pause timeout followed by a late ACK is rejected. MUSIC is granted only
    // after both ACK and uninstall; restore requires driver plus codec.
    assert(!audio_pause_ack_is_current(7, 7, false));
    assert(!audio_music_handoff_can_grant(false, true));
    assert(!audio_music_handoff_can_grant(true, false));
    assert(audio_music_handoff_can_grant(true, true));
    assert(!audio_duplex_restore_ready(true, false));
    assert(audio_duplex_restore_ready(true, true));

    // Forget accepted before Connect creates a save barrier independent of
    // ordinary queue capacity.
    assert(!wifi_connect_may_save(true, true, false));
    assert(!wifi_connect_may_save(true, false, true));

    // Stop -> Save & Connect must not report complete before Start succeeds.
    assert(!camera_config_transaction_complete(true, true, true, false));
    assert(camera_config_transaction_complete(true, true, true, true));
    return 0;
}
