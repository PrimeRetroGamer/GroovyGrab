#include "async_sender.h"

#include <algorithm>
#include <cstdio>

// How a session gets from connected to streaming: timing capture behind a few seconds
// of black, then planning the rate.

namespace groovy {

namespace {

using clock = std::chrono::steady_clock;

// The trim corrects what a short measurement leaves, with no visible step.
constexpr auto kTrimCaptureLimit = std::chrono::seconds(4);

constexpr int kBlackFrames = 6; // shown before the final clock is set, to hide it

} // namespace

// Works out how to run `mode` at the capture rate, as timed so far. Returns `mode`,
// or its NTSC or PAL counterpart if only that fits: cards can deliver another rate
// than the one they advertise.
VideoMode AsyncSender::plan_start(VideoMode mode) {
    start_plan_ = RatePlan{};
    start_fps_ = 0.0;
    if (!rate_lock_.load()) return mode;
    double fps = 0.0;
    if (!capture_rate_.rate(kTrimCaptureSeconds, fps)) return mode;
    start_fps_ = fps;
    start_plan_ = plan_rate(fps, Sender::nominal_mode_rate(mode));
    if (start_plan_.trim > 0.0) return mode;
    const VideoMode other = video_mode_for(!is_pal_mode(mode), !is_interlaced(mode));
    const RatePlan other_plan = plan_rate(fps, Sender::nominal_mode_rate(other));
    if (other_plan.trim <= 0.0) return mode;
    start_plan_ = other_plan;
    return other;
}

bool AsyncSender::prepare_black(VideoMode mode, PreparedField& black) {
    const std::array<uint8_t, 6> rgb{};
    Sample sample;
    sample.data = rgb.data();
    sample.pitch = 6;
    sample.width = 2;
    sample.height = 1;
    sample.pixels = SamplePixels::Rgb;
    return field_prep_.prepare(sample, mode, ViewSettings{}, black);
}

// Black while capture is timed, so the CRT has a picture until the final clock is set.
// False if cancelled or the send failed.
bool AsyncSender::wait_for_capture(VideoMode mode) {
    PreparedField black;
    if (!prepare_black(mode, black)) return false;
    const auto started = clock::now();
    double fps = 0.0;
    int frames = 0;
    while (frames < kBlackFrames ||
           (!capture_rate_.rate(kTrimCaptureSeconds, fps) && clock::now() - started < kTrimCaptureLimit)) {
        if (cancel_open_.load() || !send_at_deadline(black)) return false;
        ++frames;
    }
    return true;
}

bool AsyncSender::calibrate_and_start(VideoMode mode) {
    trim_ppm_ = 0.0;
    if (rate_lock_.load()) {
        calibrating_.store(true);
        recalibrate_.store(false);
        const bool ready = wait_for_capture(mode);
        calibrating_.store(false);
        if (!ready && !cancel_open_.load() && !sender_.error().empty()) {
            report_sender_error();
            sender_.close();
            return false;
        }
    }
    if (cancel_open_.load()) {
        sender_.close();
        return false;
    }
    mode = plan_start(mode);
    // Starts the picture the way connecting does: the raster parks, and the first
    // sample releases it just ahead of scanout.
    if (!sender_.set_mode(mode, start_plan_.trim > 0.0 ? start_plan_.trim : 1.0)) {
        report_sender_error();
        sender_.close();
        return false;
    }
    start_workers();
    connected_.store(true);
    set_error("");
    return true;
}

} // namespace groovy
