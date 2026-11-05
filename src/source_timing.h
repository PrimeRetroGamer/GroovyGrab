#pragma once

#include <array>
#include <cstddef>

namespace groovy {

// When a source's pictures belong, from how long after its own timestamp each one
// arrives. A capture card stamps on arrival, so that delay barely varies. A virtual
// camera stamps a perfect cadence and hands pictures over in clumps, so it can vary
// by 15 ms.
//
// Every picture is placed as if it arrived at the source's usual worst delay: the
// raster is set for it, the trim reads the phase at it, and a picture that came
// earlier waits until the refresh it would have had then. A steady source is never
// early, so it pays nothing. A clumpy one pays only for its own clumping.
//
// Plain logic with no clock of its own, so it can be tested on made-up streams.
class SourceTiming {
public:
    // A new capture graph: its delays are learned afresh.
    void reset();

    // Every picture from the capture, as it is handed over, so the allowance is
    // known before the stream starts. `delay_ms` is arrival minus timestamp.
    void note_arrival(double delay_ms);

    // Enough pictures seen to trust the allowance.
    bool ready() const { return count_ >= kMinPictures; }

    // The usual worst delay, smoothed.
    double allowance_ms() const { return allowance_ms_; }

    // How much earlier than the usual worst delay a picture with this delay arrived.
    // Negative for a late one. Zero until ready().
    double early_ms(double delay_ms) const { return ready() ? allowance_ms_ - delay_ms : 0.0; }

private:
    // About two seconds of pictures.
    static constexpr size_t kWindow = 128;
    static constexpr size_t kMinPictures = 16;
    // Per picture, at about 60 a second. The worst delay of the window jumps as slow
    // pictures come and go, and each jump would move the raster, so it is followed
    // smoothly: up within about half a second, so a slower source is soon covered,
    // and down over about five, so one quiet second doesn't pull it back.
    static constexpr double kRise = 1.0 / 30.0;
    static constexpr double kFall = 1.0 / 300.0;

    double worst_in_window() const;

    std::array<double, kWindow> delays_{};
    size_t count_ = 0, next_ = 0;
    double allowance_ms_ = 0.0;
};

} // namespace groovy
