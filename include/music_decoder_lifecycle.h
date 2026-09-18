#pragma once

#include <stdint.h>

enum class MusicDecoderPhase : uint8_t
{
    EMPTY = 0,
    STARTING,
    PLAYING,
    STOPPING,
    RECOVERY_REQUIRED
};

// A null RTOS task handle is not an exit acknowledgement: the worker clears
// its handle immediately before publishing the ACK. This generation tracker
// closes that window and is shared by the patched decoder and native tests.
class MusicWorkerExitTracker
{
public:
    uint32_t begin()
    {
        uint32_t next = generation_ + 1;
        if (next == 0) ++next;
        generation_ = next;
        return next;
    }

    void acknowledge(uint32_t generation)
    {
        if (generation == generation_) acknowledged_generation_ = generation;
    }

    bool confirmed(uint32_t generation) const
    {
        return generation != 0 && generation == generation_ &&
               acknowledged_generation_ == generation;
    }

    uint32_t generation() const { return generation_; }

private:
    volatile uint32_t generation_ = 0;
    volatile uint32_t acknowledged_generation_ = 0;
};

// Platform-independent state machine used by the firmware and native tests.
// Resource destruction is legal only after a positive shutdown ACK.
class MusicDecoderLifecycle
{
public:
    uint32_t begin_start()
    {
        if (phase_ != MusicDecoderPhase::EMPTY) return 0;
        ++generation_;
        if (generation_ == 0) ++generation_;
        phase_ = MusicDecoderPhase::STARTING;
        return generation_;
    }

    bool finish_start(uint32_t generation, bool started)
    {
        if (generation == 0 || generation != generation_ ||
            phase_ != MusicDecoderPhase::STARTING) return false;
        phase_ = started ? MusicDecoderPhase::PLAYING : MusicDecoderPhase::EMPTY;
        return true;
    }

    bool begin_stop(uint32_t generation = 0)
    {
        if (generation != 0 && generation != generation_) return false;
        if (phase_ == MusicDecoderPhase::EMPTY) return true;
        if (phase_ != MusicDecoderPhase::PLAYING &&
            phase_ != MusicDecoderPhase::STARTING &&
            phase_ != MusicDecoderPhase::RECOVERY_REQUIRED &&
            phase_ != MusicDecoderPhase::STOPPING) return false;
        phase_ = MusicDecoderPhase::STOPPING;
        return true;
    }

    bool finish_stop(uint32_t generation, bool acknowledged)
    {
        if (generation != 0 && generation != generation_) return false;
        if (phase_ == MusicDecoderPhase::EMPTY) return acknowledged;
        if (phase_ != MusicDecoderPhase::STOPPING) return false;
        phase_ = acknowledged ? MusicDecoderPhase::EMPTY
                              : MusicDecoderPhase::RECOVERY_REQUIRED;
        return true;
    }

    bool accepts_event(uint32_t generation) const
    {
        return generation != 0 && generation == generation_ &&
               phase_ == MusicDecoderPhase::PLAYING;
    }

    bool can_destroy() const { return phase_ == MusicDecoderPhase::EMPTY; }
    uint32_t generation() const { return generation_; }
    MusicDecoderPhase phase() const { return phase_; }

private:
    MusicDecoderPhase phase_ = MusicDecoderPhase::EMPTY;
    uint32_t generation_ = 0;
};
