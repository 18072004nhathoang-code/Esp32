#include "xiaozhi_session_logic.h"

#include <cassert>
#include <cstring>

int main()
{
    using namespace xiaozhi;

    BackpressureWindow pressure;
    assert(pressure.observe(false, 100, 500) == BackpressureDecision::WAIT);
    assert(pressure.observe(false, 599, 500) == BackpressureDecision::WAIT);
    assert(pressure.observe(false, 600, 500) == BackpressureDecision::TIMED_OUT);
    assert(pressure.observe(true, 601, 500) == BackpressureDecision::READY);
    assert(pressure.sinceMs() == 0);

    // More source frames than queue slots must be processed in bounded batches.
    assert(bounded_frame_budget(960U * 20U, 960, 2) == 2);
    assert(bounded_frame_budget(1, 960, 2) == 1);
    assert(bounded_frame_budget(0, 960, 2) == 0);

    // Cancel remains authoritative even when a command queue cannot accept it.
    assert(cancellation_applies(7, 7));
    assert(cancellation_applies(7, 9));
    assert(!cancellation_applies(10, 9));

    // A stale cleanup/ACK must not mutate a newly opened session.
    assert(cleanup_may_mutate(11, 11));
    assert(!cleanup_may_mutate(10, 11));
    assert(recorder_control_targets(21, 21));
    assert(!recorder_control_targets(20, 21));

    // listen/stop is legal only after the exact audio queue and in-flight frame have drained.
    assert(!flush_ready_for_listen_stop(false, 0, false));
    assert(!flush_ready_for_listen_stop(true, 1, false));
    assert(!flush_ready_for_listen_stop(true, 0, true));
    assert(flush_ready_for_listen_stop(true, 0, false));
    // Overload with in_flight parameter
    assert(!flush_ready_for_listen_stop(true, 0, true, false));
    assert(!flush_ready_for_listen_stop(true, 1, false, false));
    assert(flush_ready_for_listen_stop(true, 0, false, false));
    assert(!flush_ready_for_listen_stop(true, 0, false, true));
    assert(!flush_ready_for_listen_stop(true, 1, true, true));

    // Snapshot generation isolation: lease matches expected generation strictly
    assert(snapshot_lease_matches(0, 5));
    assert(snapshot_lease_matches(5, 5));
    assert(!snapshot_lease_matches(5, 4));
    assert(!snapshot_lease_matches(5, 0));

    // Live capture matches expected recording command generation
    assert(live_capture_matches(0, 9));
    assert(live_capture_matches(9, 9));
    assert(!live_capture_matches(9, 8));
    assert(!live_capture_matches(9, 0));

    // Empty recording must not initiate audio flush
    assert(!snapshot_flush_needed(0, 0));
    assert(!snapshot_flush_needed(12, 0));
    assert(!snapshot_flush_needed(0, 1024));
    assert(snapshot_flush_needed(12, 1024));

    // 500-message chat queue simulation (capping bubbles strictly at 32)
    constexpr size_t MAX_BUBBLES = 32;
    int rendered_bubbles[MAX_BUBBLES] = {0};
    size_t bubble_count = 0;
    for (int msg_id = 1; msg_id <= 500; ++msg_id)
    {
        if (bubble_count >= MAX_BUBBLES)
        {
            // Prune oldest (index 0)
            for (size_t i = 1; i < MAX_BUBBLES; ++i)
            {
                rendered_bubbles[i - 1] = rendered_bubbles[i];
            }
            --bubble_count;
        }
        rendered_bubbles[bubble_count++] = msg_id;
    }
    assert(bubble_count == MAX_BUBBLES);
    assert(rendered_bubbles[0] == (500 - 32 + 1));
    assert(rendered_bubbles[MAX_BUBBLES - 1] == 500);

    // Phase Deadlines & Tracker
    PhaseDeadlines deadlines;
    SessionPhaseTracker tracker;
    assert(tracker.phase() == SessionPhase::IDLE);
    assert(tracker.check_timeout(1000, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);

    // CONNECTING
    tracker.start_connecting(1000);
    assert(tracker.phase() == SessionPhase::CONNECTING);
    assert(tracker.check_timeout(1000 + 14999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(1000 + 15000, deadlines) == SessionPhaseTracker::TimeoutReason::CONNECT_TIMEOUT);

    // LISTENING
    tracker.start_listening(2000);
    assert(tracker.check_timeout(2000 + 29999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(2000 + 30000, deadlines) == SessionPhaseTracker::TimeoutReason::LISTENING_TIMEOUT);

    // FLUSHING
    tracker.start_flushing(5000);
    assert(tracker.check_timeout(5000 + 4999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(5000 + 5000, deadlines) == SessionPhaseTracker::TimeoutReason::FLUSH_TIMEOUT);

    // WAITING_RESPONSE - progression stages:
    // Stage 1: WAITING_STT
    tracker.start_waiting_response(10000);
    assert(tracker.waiting_stage() == WaitingStage::WAITING_STT);
    assert(tracker.check_timeout(10000 + 14999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    // Unknown messages/ping must NOT advance stage or extend deadline
    tracker.record_progress(10000 + 14000);
    assert(tracker.check_timeout(10000 + 15000, deadlines) == SessionPhaseTracker::TimeoutReason::WAIT_STT_TIMEOUT);

    // Stage 2: WAITING_LLM_OR_MCP (STT arrived)
    tracker.start_waiting_response(10000);
    tracker.on_stt_received(20000); // STT arrives at 10s after listen/stop
    assert(tracker.waiting_stage() == WaitingStage::WAITING_LLM_OR_MCP);
    // At 25s after listen/stop (5s after STT), previous flat 20s timeout would fail; now it PASSES:
    assert(tracker.check_timeout(25000, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(20000 + 19999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    // MCP activity extends/refreshes LLM wait deadline:
    tracker.on_mcp_progress(25000);
    assert(tracker.check_timeout(25000 + 19999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(25000 + 20000, deadlines) == SessionPhaseTracker::TimeoutReason::WAIT_LLM_TIMEOUT);

    // Stage 3: WAITING_FIRST_AUDIO (TTS start received)
    tracker.start_waiting_response(10000);
    tracker.on_stt_received(15000);
    tracker.on_tts_start(20000);
    assert(tracker.waiting_stage() == WaitingStage::WAITING_FIRST_AUDIO);
    assert(tracker.check_timeout(20000 + 7999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(20000 + 8000, deadlines) == SessionPhaseTracker::TimeoutReason::WAIT_FIRST_AUDIO_TIMEOUT);

    // First PCM arrives within deadline
    tracker.on_first_pcm(25000);
    assert(tracker.check_timeout(25000, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);

    // Total wait budget cap (45s)
    tracker.start_waiting_response(10000);
    tracker.on_stt_received(20000);
    tracker.on_mcp_progress(35000);
    tracker.on_mcp_progress(50000);
    assert(tracker.check_timeout(10000 + 45000, deadlines) == SessionPhaseTracker::TimeoutReason::WAIT_RESPONSE_TIMEOUT);

    // Vietnamese timeout strings
    assert(strstr(SessionPhaseTracker::timeout_reason_string(SessionPhaseTracker::TimeoutReason::WAIT_STT_TIMEOUT), "(STT)") != nullptr);
    assert(strstr(SessionPhaseTracker::timeout_reason_string(SessionPhaseTracker::TimeoutReason::WAIT_LLM_TIMEOUT), "(LLM/MCP)") != nullptr);
    assert(strstr(SessionPhaseTracker::timeout_reason_string(SessionPhaseTracker::TimeoutReason::WAIT_FIRST_AUDIO_TIMEOUT), "luồng âm thanh") != nullptr);

    // SPEAKING - normal long response with active streaming progress must NOT time out at 60s
    tracker.start_speaking(30000);
    tracker.on_first_pcm(30500);
    // Simulate audio chunks arriving every 2 seconds for 70 seconds
    for (uint32_t t = 32000; t <= 30000 + 70000; t += 2000)
    {
        tracker.record_progress(t);
        assert(tracker.check_timeout(t, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    }
    // Now simulate stall: 8 seconds without audio chunk
    assert(tracker.check_timeout(30000 + 70000 + 7999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(30000 + 70000 + 8000, deadlines) == SessionPhaseTracker::TimeoutReason::SPEAKING_STALLED);

    // SPEAKING - max response duration cap (180s)
    tracker.start_speaking(10000);
    tracker.on_first_pcm(10100);
    tracker.record_progress(10000 + 179999);
    assert(tracker.check_timeout(10000 + 179999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    tracker.record_progress(10000 + 180000);
    assert(tracker.check_timeout(10000 + 180000, deadlines) == SessionPhaseTracker::TimeoutReason::SPEAKING_MAX_EXCEEDED);

    // CLEANUP
    tracker.start_cleanup(50000);
    assert(tracker.check_timeout(50000 + 4999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(50000 + 5000, deadlines) == SessionPhaseTracker::TimeoutReason::CLEANUP_TIMEOUT);

    // Absolute session timeout (240s)
    tracker.reset();
    tracker.start_connecting(1000);
    tracker.start_speaking(1000 + 70000);
    tracker.on_first_pcm(1000 + 70500);
    tracker.record_progress(1000 + 239999);
    assert(tracker.check_timeout(1000 + 239999, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(1000 + 240000, deadlines) == SessionPhaseTracker::TimeoutReason::ABSOLUTE_SESSION_TIMEOUT);

    // Wraparound safety
    tracker.reset();
    tracker.start_connecting(0xfffffff0U);
    assert(tracker.check_timeout(0xfffffff0U + 1000, deadlines) == SessionPhaseTracker::TimeoutReason::NONE);
    assert(tracker.check_timeout(0xfffffff0U + 15000, deadlines) == SessionPhaseTracker::TimeoutReason::CONNECT_TIMEOUT);

    // session_id_matches_contract
    assert(!session_id_matches_contract("sess-1", "sess-1", false));
    assert(session_id_matches_contract("sess-1", "sess-1", true));
    assert(session_id_matches_contract(nullptr, "sess-1", true)); // Optional session_id accepted
    assert(session_id_matches_contract("", "sess-1", true));      // Optional session_id accepted
    assert(!session_id_matches_contract("sess-2", "sess-1", true)); // Mismatch rejected

    // has_captured_audio
    assert(!has_captured_audio(0, 0, 0));
    assert(has_captured_audio(1, 0, 0));
    assert(has_captured_audio(0, 1, 0));
    assert(has_captured_audio(0, 0, 1));

    // can_reuse_connection
    assert(can_reuse_connection(true, true, 0, 0, 1000, 5000, 30000));
    assert(!can_reuse_connection(false, true, 0, 0, 1000, 5000, 30000)); // not connected
    assert(!can_reuse_connection(true, false, 0, 0, 1000, 5000, 30000)); // not clean turn
    assert(!can_reuse_connection(true, true, 1, 0, 1000, 5000, 30000));  // uplink pending
    assert(!can_reuse_connection(true, true, 0, 1, 1000, 5000, 30000));  // downlink pending
    assert(!can_reuse_connection(true, true, 0, 0, 1000, 32000, 30000)); // idle timed out (>30s)

    // SessionTiming reset
    SessionTiming timing;
    timing.t_ptt_ms = 123;
    timing.t_done_ms = 456;
    timing.reset();
    assert(timing.t_ptt_ms == 0 && timing.t_done_ms == 0);

    return 0;
}
