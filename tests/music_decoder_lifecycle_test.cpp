#include "music_decoder_lifecycle.h"

#include <assert.h>

struct MockDecoder
{
    bool task_alive = false;
    unsigned creates = 0;
    unsigned destroys = 0;

    bool create()
    {
        assert(!task_alive);
        task_alive = true;
        ++creates;
        return true;
    }

    bool shutdown(bool acknowledge)
    {
        if (acknowledge) task_alive = false;
        return acknowledge;
    }

    void destroy()
    {
        assert(!task_alive);
        ++destroys;
    }
};

int main()
{
    MusicWorkerExitTracker worker_exit;
    const uint32_t worker_generation = worker_exit.begin();
    assert(!worker_exit.confirmed(worker_generation)); // handle=null alone is not completion
    worker_exit.acknowledge(worker_generation);
    assert(worker_exit.confirmed(worker_generation));

    MusicDecoderLifecycle lifecycle;
    MockDecoder decoder;

    for (unsigned i = 0; i < 100; ++i)
    {
        const uint32_t generation = lifecycle.begin_start();
        assert(generation != 0 && decoder.create());
        assert(lifecycle.finish_start(generation, true));
        assert(lifecycle.accepts_event(generation));
        assert(lifecycle.begin_stop(generation));
        assert(decoder.shutdown(true));
        assert(lifecycle.finish_stop(generation, true));
        assert(lifecycle.can_destroy());
        decoder.destroy();
    }
    assert(decoder.creates == 100 && decoder.destroys == 100);

    const uint32_t timed_out = lifecycle.begin_start();
    assert(decoder.create());
    assert(lifecycle.finish_start(timed_out, true));
    assert(lifecycle.begin_stop(timed_out));
    assert(!decoder.shutdown(false));
    assert(lifecycle.finish_stop(timed_out, false));
    assert(lifecycle.phase() == MusicDecoderPhase::RECOVERY_REQUIRED);
    assert(!lifecycle.can_destroy() && decoder.destroys == 100);
    assert(lifecycle.begin_start() == 0);

    assert(lifecycle.begin_stop(timed_out));
    assert(decoder.shutdown(true));
    assert(lifecycle.finish_stop(timed_out, true));
    decoder.destroy();

    // A decoder whose connect/open fails is still in STARTING. Its worker must
    // receive the same positive shutdown acknowledgement before destruction.
    const uint32_t failed_start = lifecycle.begin_start();
    assert(failed_start != 0 && decoder.create());
    assert(lifecycle.begin_stop(failed_start));
    assert(!decoder.shutdown(false));
    assert(lifecycle.finish_stop(failed_start, false));
    assert(lifecycle.phase() == MusicDecoderPhase::RECOVERY_REQUIRED);
    assert(!lifecycle.can_destroy());
    assert(lifecycle.begin_stop(failed_start));
    assert(decoder.shutdown(true));
    assert(lifecycle.finish_stop(failed_start, true));
    decoder.destroy();

    const uint32_t current = lifecycle.begin_start();
    assert(decoder.create());
    assert(lifecycle.finish_start(current, true));
    assert(!lifecycle.accepts_event(timed_out));
    assert(lifecycle.accepts_event(current));
    assert(!lifecycle.begin_stop(timed_out));
    assert(lifecycle.begin_stop(current));
    assert(decoder.shutdown(true));
    assert(lifecycle.finish_stop(current, true));
    decoder.destroy();
    return 0;
}
