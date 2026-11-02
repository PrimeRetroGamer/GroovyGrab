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

// Locks MiSTer's raster to the capture clock. In interlaced modes every "frame"
// below is one field, as Groovy counts them.
//
// A session normally starts locked: capture and the MiSTer are timed behind the
// Calibrating image, and the first sample releases the raster parked after it, which
// sets its phase.
//
// Measure: capture is not timed yet or has changed, so free-run until the session
//   calibrates again.
// Slide: send samples as they arrive, with the pixel clock trimmed, and offset the
//   clock slightly until samples land where the raster has just enough time left to
//   scan them. Late or surplus samples along the way only cost a repeated or
//   replaced frame.
// Locked: the clock runs at the trimmed rate. If the phase drifts too far, correct
//   the rate from the longer measurement and slide back. On the GroovyGrab core, the
//   rate is instead trimmed all the time, which holds the phase with no visible step.
// Free: no plan fits this rate, or locking is turned off. The clock stays nominal
//   and the newest sample goes out once per frame.
//
// Clock changes never use INIT, which resets the core. SWITCHRES takes effect at a
// vblank and the next field releases the raster at once, so with a field always on
// its way the picture keeps running. Each one still nudges a CRT, whatever its size,
// because the core restarts its line counter: keep them rare.
struct RateLock {
    using clock = std::chrono::steady_clock;
    enum class State { Measure, Slide, Locked, Free };
    enum class Action { None, Retune, Restart, Trim };

    static constexpr double kMaxTrim = 0.01; // pixel clock +/- 1 %
    // Samples land this far ahead of the raster's line after the active area, which
    // is about 1.3 ms of blanking before line 0. The sum must stay above the
    // sender's repeat lead (1.8 ms).
    static constexpr double kLandMarginSeconds = 0.0015;
    // MiSTer's pixel clock comes from its crystal, so its real rate is within
    // this of the nominal one (~10 ppm in practice). A noisier measurement is
    // clamped; one above kNoisyFitMs is replaced by the nominal rate.
    static constexpr double kClockTolerance = 50e-6;
    static constexpr double kNoisyFitMs = 0.5;

    State state = State::Measure;
    double scale = 1.0; // pixel clock relative to the mode's nominal one
    bool use_trim = false; // hold the phase with the core's trim, instead of the clock
    double trim_ppm = 0.0; // what the raster is trimmed by; set before start_parked to resume
    unsigned repeats = 0; // late samples covered by a repeat
    std::string log; // messages for the main thread: printing here stalls the sender

    bool capture_clocked() const { return state == State::Slide || state == State::Locked; }

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

    // After sending a new sample.
    Action after_send(const Sender& sender, clock::time_point captured);

private:
    static constexpr auto kWarmup = std::chrono::seconds(1);
    static constexpr auto kRateBlock = std::chrono::seconds(4);
    static constexpr double kMaxRefine = 0.0003;    // re-lock trim step limit
    static constexpr double kIntervalChange = 0.05; // 60 -> 30/50 fps etc.
    static constexpr double kRateChange = 0.001;    // e.g. 60.00 -> 59.94
    static constexpr double kMinLockedSeconds = 10.0;
    // Drifting early only costs latency, which is worth it to change the clock less
    // often; drifting late eats the margin before the repeat deadline.
    static constexpr double kDriftLimit = 0.24; // frames early (~4 ms)
    static constexpr double kDriftLate = 0.05;  // frames late (~0.8 ms)
    // Moves the landing point by 0.002 frames per frame: half a frame takes 4 s.
    static constexpr double kSlideOffset = 0.002;
    static constexpr double kSlideStart = 0.06; // frames (~1 ms): not worth a clock change below this
    static constexpr double kSlideDone = 0.01;  // frames (~0.17 ms)
    static constexpr unsigned kSlideSettle = 8; // samples to average before deciding
    // A real rate error moves the phase far slower than this, so it has to be a step in
    // capture timing, which should not change the clock's rate.
    static constexpr auto kStepCheck = std::chrono::seconds(2);
    static constexpr double kStepFrames = 0.04; // ~0.7 ms in that time

    void note(const char* format, ...);

    static double seconds(clock::duration d) { return rate_fit_seconds(d); }
    static double wrap(double x);

    void reset_windows(clock::time_point now);
    void record_gap(double gap_ms);
    Action update_clock(const Sender& sender, clock::time_point captured, clock::time_point now);
    double median_gap_ms() const;
    bool rates(const Sender& sender, double& capture_fps, double& raster_fps);
    Action correct_rate(const Sender& sender, double capture_fps, double raster_fps, const char* why);
    Action check_capture_rate(double gap_ms, clock::time_point captured);
    Action update_slide(const Sender& sender);
    Action update_trim(const Sender& sender);

    // Trim control: a second's median phase error drives a slow proportional and
    // integral loop. The proportional gain settles in about 30 s, and the integral
    // gain gives it no overshoot.
    static constexpr auto kTrimInterval = std::chrono::seconds(1);
    static constexpr double kTrimGain = 500.0;     // ppm per frame of error
    static constexpr double kTrimIntegral = 3.75;  // ppm per frame of error, per second
    static constexpr double kTrimLimit = 190.0;    // about one clock tick a line
    static constexpr double kTrimResend = 0.25;    // ppm: smaller changes are not sent
    // Lands a little further from the deadline than the slide does: a loop that holds
    // the phase there has no reason to run close to it.
    static constexpr double kTrimLandMarginSeconds = 0.002;
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
    unsigned samples_seen = 0;
    unsigned slots_since_sample = 0;
    clock::time_point since = clock::now();
    int locks = 0;

    // Rate of the samples used, over the current state's window.
    RateFit cap_rate;
    clock::time_point cap_t1{};

    // Recent gaps between samples used (their median is the nominal interval), and
    // the rate the lock was made at.
    std::array<double, 64> gaps{};
    size_t gap_count = 0, gap_next = 0;
    bool have_arrival = false;
    clock::time_point last_arrival{};
    double ref_fps = 0.0;
    RateFit block_rate;

    // MiSTer raster rate.
    bool ras_valid = false;
    double ras_p0 = 0.0, ras_p1 = 0.0;
    clock::time_point ras_t0{}, ras_t1{}, ras_last{};
    RobustFit ras_fit;

    // Slide: the clock to settle on, and how far fields land from where they should.
    bool release_pending = false;
    double locked_scale = 1.0;
    int slide_direction = 0; // sign of the clock offset; 0 while still settling
    unsigned slide_samples = 0;
    double slide_error = 0.0;

    // Raster phase (fraction of a frame) at the moment samples arrive.
    unsigned target_n = 0;
    double target_x = 0.0, target_y = 0.0, target = 0.0, drift = 0.0;
    double step_drift = 0.0;
    bool step_ready = false;
    clock::time_point step_at{};
    bool have_target = false;
};

} // namespace groovy
