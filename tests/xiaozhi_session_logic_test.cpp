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
    return 0;
}
