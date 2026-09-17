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
