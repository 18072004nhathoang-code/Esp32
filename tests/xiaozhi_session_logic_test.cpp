#include "xiaozhi_session_logic.h"

#include <cassert>

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

    // listen/stop is legal only after the exact audio queue has drained.
    assert(!flush_ready_for_listen_stop(false, 0, false));
    assert(!flush_ready_for_listen_stop(true, 1, false));
    assert(!flush_ready_for_listen_stop(true, 0, true));
    assert(flush_ready_for_listen_stop(true, 0, false));

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

    return 0;
}
