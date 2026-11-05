#pragma once

#include "groovy_sender.h"
#include "rate_fit.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <string>

namespace groovy {

// How a capture rate maps onto a mode's blit rate. The mode's line count and line
// rate never change, because picky CRTs only sync close to standard timings.
struct RatePlan {
    unsigned repeat = 1;  // blits per sample (30 fps -> 2)
    unsigned skip = 1;    // samples per blit (120 fps -> 2)
    double trim = 0.0;    // pixel clock relative to nominal; 0 if the rate can't lock
};
RatePlan plan_rate(double capture_fps, double mode_fps);

// Locks the MiSTer's raster to the capture clock, with the GroovyGrab core's pixel
// rate trim. In interlaced modes every "frame" below is one field, as Groovy counts
// them.
//
// A session starts locked: capture is timed behind a few seconds of black, the
// pixel clock is set to match, and the first sample releases the raster parked
// after it, which sets its phase. The trim then holds that phase with no visible
// step.
//
// Measure: capture is not timed yet or has changed, so free-run until the session
//   times it again.
// Locked: samples go out as they arrive, and the trim holds the raster where they
//   land just ahead of scanout.
// Free: no plan fits this rate, or locking is turned off. The clock stays nominal
//   and the newest sample goes out once per frame.
struct RateLock {
    using clock = std::chrono::steady_clock;
    enum class State { Measure, Locked, Free };
    enum class Action { None, Restart, Trim };

    static constexpr double kMaxTrim = 0.01; // pixel clock +/- 1 %
    // The first sample is released this far ahead of the raster's line after the
    // active area, which is about 1.3 ms of blanking before line 0. The sum must
    // stay above the sender's repeat lead (1.8 ms).
    static constexpr double kLandMarginSeconds = 0.0015;
    // Where the trim holds pictures: a little further from the deadline than the
    // release, as a loop that holds the phase there has no reason to run close to it.
    static constexpr double kTrimLandMarginSeconds = 0.002;

    State state = State::Measure;
    double trim_ppm = 0.0; // what the raster is trimmed by; set before start_parked to resume
    unsigned repeats = 0; // late samples covered by a repeat
    std::string log; // messages for the main thread: printing here stalls the sender

    bool capture_clocked() const { return state == State::Locked; }

    // Starts on a parked raster, with the pixel clock already at the plan's trim.
    void start_parked(const RatePlan& plan, double capture_fps);

    // Stops locking and free-runs, before any clock change.
    void go_free(const char* why);

    // True once after start_parked(): delay this send by kLandMarginSeconds.
    bool take_release();

    // Whether to use this sample, for rates above the raster's.
    bool accept_sample();

    // The next frame is a duplicate by plan, so no sample is expected for it.
    bool repeat_planned() const { return slots_since_sample + 1 < plan.repeat; }

    void note_send(bool fresh) { slots_since_sample = fresh ? 0 : slots_since_sample + 1; }

    // After sending a new sample. `source` is the sample's own timestamp, which is
    // what its rate is measured from. `track_phase` is false for a frame from a
    // previous capture graph: its rate still counts, and it does not move the
    // landing point. `shift` moves the phase reading from this picture's arrival to
    // the source's usual worst delay: zero for a steady source.
    Action after_send(const Sender& sender, clock::time_point source, bool track_phase,
                      clock::duration shift = {});

private:
    static constexpr auto kRateBlock = std::chrono::seconds(4);
    static constexpr double kIntervalChange = 0.05; // 60 -> 30/50 fps etc.
    static constexpr double kRateChange = 0.001;    // e.g. 60.00 -> 59.94

    void note(const char* format, ...);

    static double seconds(clock::duration d) { return rate_fit_seconds(d); }
    static double wrap(double x);

    void record_gap(double gap_ms);
    double median_gap_ms() const;
    Action check_capture_rate(double gap_ms, clock::time_point captured);
    Action update_trim(const Sender& sender);

    // Trim control: a second's median phase error drives a slow proportional and
    // integral loop. The proportional gain settles in about 30 s, and the integral
    // gain gives it no overshoot.
    static constexpr auto kTrimInterval = std::chrono::seconds(1);
    static constexpr double kTrimGain = 500.0;     // ppm per frame of error
    static constexpr double kTrimIntegral = 3.75;  // ppm per frame of error, per second
    static constexpr double kTrimLimit = 190.0;    // about one clock tick a line
    static constexpr double kTrimResend = 0.25;    // ppm: smaller changes are not sent
    // Capture timing can step with no sample missing. That is a new position, not a new
    // rate: the integral stays out of it, and a higher gain moves the raster back in
    // seconds, as a late step would otherwise eat the margin for half a minute.
    static constexpr double kTrimStep = 0.04;       // frames (~0.7 ms) between one second and the next
    static constexpr double kTrimStepDone = 0.01;   // frames
    static constexpr double kTrimStepGain = 2500.0; // ppm per frame of error
    double trim_last_error = 0.0;
    bool trim_have_error = false, trim_stepping = false, trim_step_seen = false;
    std::array<double, 64> trim_errors{};
    size_t trim_error_count = 0;
    clock::time_point trim_since{};
    double trim_sum = 0.0, trim_sent = 0.0;

    RatePlan plan;
    clock::duration phase_shift{};
    bool release_pending = false;
    unsigned samples_seen = 0;
    unsigned slots_since_sample = 0;

    // Recent gaps between samples used (their median is the nominal interval), and
    // the rate the lock was made at.
    std::array<double, 64> gaps{};
    size_t gap_count = 0, gap_next = 0;
    bool have_arrival = false;
    clock::time_point last_arrival{};
    double ref_fps = 0.0;
    RateFit block_rate;
};

} // namespace groovy
