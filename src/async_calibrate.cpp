#include "async_sender.h"

#include <algorithm>
#include <cstdio>

// How a session gets from connected to streaming: timing capture and the MiSTer behind
// a black or Calibrating image, then planning the rate.

namespace groovy {

namespace {

using clock = std::chrono::steady_clock;

// The trim corrects what a short measurement leaves, with no visible step.
constexpr auto kTrimCaptureLimit = std::chrono::seconds(4);

// How long the Calibrating image stays up, and the part of it left out of the raster
// timing while the raster settles after connecting.
constexpr auto kCalibrateTime = std::chrono::seconds(10);
constexpr auto kCalibrateWarmup = std::chrono::seconds(1);
constexpr int kBlackFrames = 6; // shown before the final clock is set, to hide it

// Runs the raster this much slower than measured. What is left of the measurement's
// error then adds a little latency instead of eating the margin before the deadline.
constexpr double kEarlyBias = 2e-6;

constexpr int kCalibratingImage = 2; // resource id in groovygrab.rc
constexpr unsigned kCalibratingWidth = 720, kCalibratingHeight = 480;

} // namespace

double AsyncSender::start_clock_error() const {
    return sender_.has_trim() ? 1.0 : mister_clock_ * (1.0 + kEarlyBias);
}

// Works out how to run `mode` at the capture rate, as timed so far. Returns `mode`,
// or its NTSC or PAL counterpart if only that fits: cards can deliver another rate
// than the one they advertise.
VideoMode AsyncSender::plan_start(VideoMode mode) {
    start_plan_ = RatePlan{};
    start_fps_ = 0.0;
    if (!rate_lock_.load()) return mode;
    double fps = 0.0;
    const double measured = sender_.has_trim() ? kTrimCaptureSeconds : kCaptureMeasureSeconds;
    if (!capture_rate_.rate(measured, fps)) return mode;
    start_fps_ = fps;
    const double clock_error = start_clock_error();
    start_plan_ = plan_rate(fps, Sender::nominal_mode_rate(mode) * clock_error);
    if (start_plan_.trim > 0.0) return mode;
    const VideoMode other = video_mode_for(!is_pal_mode(mode), !is_interlaced(mode));
    const RatePlan other_plan = plan_rate(fps, Sender::nominal_mode_rate(other) * clock_error);
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

// Prepares the embedded Calibrating image for `mode`, ignoring the user's view
// settings, and black. Only runs while the prep thread is stopped.
bool AsyncSender::prepare_calibration_fields(VideoMode mode, PreparedField& image, PreparedField& black) {
    if (!prepare_black(mode, black)) return false;

    std::vector<uint8_t> rgb;
    Sample sample;
    sample.pixels = SamplePixels::Rgb;
    std::string error;
    const HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(kCalibratingImage), RT_RCDATA);
    const HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
    const auto* jpeg = loaded ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
    if (!jpeg || !jpeg_decoder_.decompress(jpeg, SizeofResource(nullptr, found), kCalibratingWidth,
                                           kCalibratingHeight, rgb, &error)) {
        return false;
    }
    sample.data = rgb.data();
    sample.pitch = kCalibratingWidth * 3;
    sample.width = kCalibratingWidth;
    sample.height = kCalibratingHeight;
    return field_prep_.prepare(sample, mode, ViewSettings{}, image);
}

// Black while capture is timed. On stock Groovy a parked raster has no hsync, which a
// CRT cannot hold through for long, so frames keep going until the final clock is set.
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

// Shows the Calibrating image while timing the MiSTer's raster, which can only be done
// once connected. Capture is timed all along. False if cancelled or the send failed.
bool AsyncSender::calibrate(VideoMode mode) {
    PreparedField image, black;
    if (!prepare_calibration_fields(mode, image, black)) return false;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    RobustFit fit;
    clock::time_point fit_start{}, last_at{};
    double first_position = 0.0, last_position = 0.0;
    const auto started = clock::now();
    while (clock::now() - started < kCalibrateTime) {
        if (cancel_open_.load() || !send_at_deadline(image)) return false;
        double position = 0.0;
        clock::time_point at{};
        if (clock::now() - started < kCalibrateWarmup || !sender_.raster_sample(position, at) || at == last_at) {
            continue;
        }
        last_at = at;
        // Sender re-INITed: a new frame count and raster phase.
        if (fit.samples.empty() || position < last_position) {
            fit.clear();
            fit_start = at;
            first_position = position;
        }
        last_position = position;
        fit.add(rate_fit_seconds(at - fit_start), position - first_position);
    }

    double raster_fps = 0.0;
    if (fit.slope(raster_fps) && fit.rms * sender_.field_ms() <= RateLock::kNoisyFitMs) {
        mister_clock_ = std::clamp(raster_fps / sender_.nominal_rate(), 1.0 - RateLock::kClockTolerance,
                                   1.0 + RateLock::kClockTolerance);
    }
    char line[128];
    std::snprintf(line, sizeof(line), "Calibrated: MiSTer clock %+.1f ppm [raster fit %zu samples, rms %.2f ms]\n",
                  (mister_clock_ - 1.0) * 1e6, fit.samples.size(), fit.rms * sender_.field_ms());
    add_log(line);
    for (int i = 0; i < kBlackFrames; ++i) {
        if (cancel_open_.load() || !send_at_deadline(black)) return false;
    }
    return true;
}

bool AsyncSender::calibrate_and_start(VideoMode mode) {
    mister_clock_ = 1.0;
    trim_ppm_ = 0.0;
    if (rate_lock_.load() && sender_.has_trim()) {
        calibrating_.store(true);
        recalibrate_.store(false);
        const bool ready = wait_for_capture(mode);
        calibrating_.store(false);
        if (!ready && !cancel_open_.load() && !sender_.error().empty()) {
            report_sender_error();
            sender_.close();
            return false;
        }
    } else if (rate_lock_.load()) {
        calibrating_.store(true);
        // Again if capture is reopened meanwhile, which restarts its timing.
        bool calibrated = false;
        do {
            recalibrate_.store(false);
            calibrated = calibrate(mode);
        } while (calibrated && recalibrate_.load());
        calibrating_.store(false);
        if (!calibrated && !cancel_open_.load() && !sender_.error().empty()) {
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
