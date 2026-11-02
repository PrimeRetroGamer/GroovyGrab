#include "rate_lock.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace groovy {

RatePlan plan_rate(double capture_fps, double mode_fps) {
    RatePlan plan;
    const auto try_plan = [&](unsigned repeat, unsigned skip) {
        const double trim = capture_fps * repeat / skip / mode_fps;
        if (std::abs(trim - 1.0) > RateLock::kMaxTrim) return false;
        plan.repeat = repeat;
        plan.skip = skip;
        plan.trim = trim;
        return true;
    };
    for (unsigned repeat = 1; repeat <= 4; ++repeat) {
        if (try_plan(repeat, 1)) return plan;
    }
    try_plan(1, 2);
    return plan;
}

void RateLock::note(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log += line;
}

double RateLock::wrap(double x) {
    return x - std::floor(x + 0.5);
}

void RateLock::start_parked(const RatePlan& rate_plan, double capture_fps) {
    plan = rate_plan;
    scale = locked_scale = plan.trim;
    ref_fps = capture_fps / plan.skip;
    state = use_trim ? State::Locked : State::Slide;
    trim_sum = trim_sent = trim_ppm;
    trim_since = clock::now();
    release_pending = true;
    note("Rate lock: capture %.4f fps, %u blit(s) per %u sample(s), pixel clock x%.6f\n",
         capture_fps, plan.repeat, plan.skip, plan.trim);
}

void RateLock::go_free(const char* why) {
    note("Rate lock off: %s\n", why);
    state = State::Free;
    plan = RatePlan{};
}

bool RateLock::take_release() {
    const bool pending = release_pending;
    release_pending = false;
    return pending;
}

bool RateLock::accept_sample() {
    samples_seen = (samples_seen + 1) % plan.skip;
    return samples_seen == 0;
}

double RateLock::median_gap_ms() const {
    std::array<double, 64> sorted{};
    std::copy_n(gaps.begin(), gap_count, sorted.begin());
    const auto mid = sorted.begin() + static_cast<ptrdiff_t>(gap_count / 2);
    std::nth_element(sorted.begin(), mid, sorted.begin() + static_cast<ptrdiff_t>(gap_count));
    return *mid;
}

void RateLock::reset_windows(clock::time_point now) {
    since = now;
    cap_rate.valid = ras_valid = have_target = false;
    target_n = 0;
    target_x = target_y = drift = 0.0;
    step_ready = false;
}

bool RateLock::rates(const Sender& sender, double& capture_fps, double& raster_fps) {
    if (!cap_rate.valid || !ras_valid) return false;
    const double cap_s = seconds(cap_t1 - cap_rate.t0);
    const double ras_s = seconds(ras_t1 - ras_t0);
    if (cap_s < 4.0 || ras_s < 4.0) return false;
    if (!cap_rate.fit.slope(capture_fps) || !ras_fit.slope(raster_fps)) {
        return false;
    }
    const double nominal = sender.nominal_rate();
    if (nominal > 0.0) {
        // A noisy window (ACKs read late) is worse than the crystal:
        // fall back to nominal. A clean one also captures this
        // MiSTer's own crystal error, within the tolerance.
        if (ras_fit.rms * sender.field_ms() > kNoisyFitMs) {
            raster_fps = nominal;
        } else {
            raster_fps = std::clamp(raster_fps, nominal * (1.0 - kClockTolerance),
                                    nominal * (1.0 + kClockTolerance));
        }
    }
    return capture_fps > 0.0 && raster_fps > 0.0;
}

// Corrects the residual rate error found while locked, then slides the phase back.
RateLock::Action RateLock::correct_rate(const Sender& sender, double capture_fps,
                                        double raster_fps, const char* why) {
    const double next = scale * capture_fps * plan.repeat / raster_fps;
    // Needing more than this means the capture rate itself changed.
    if (std::abs(next / scale - 1.0) > kMaxRefine) {
        note("Rate lock restart: %s needs a %.0f ppm correction\n", why,
             (next / scale - 1.0) * 1e6);
        return Action::Restart;
    }
    ref_fps = capture_fps;
    block_rate.valid = false;
    const double clock_mhz = sender.pixel_clock_mhz() / scale * next;
    const double field = sender.field_ms();
    const bool nominal = ras_fit.rms * field > kNoisyFitMs;
    note("Rate lock %d (%s): capture %.4f fps, MiSTer %.4f fps%s -> "
         "pixel clock %.6f MHz (x%.6f) [raster fit %zu samples, "
         "rms %.2f ms, worst %.2f ms, dropped %zu]\n",
         ++locks, why, capture_fps, raster_fps,
         nominal ? " (nominal: fit too noisy)" : "", clock_mhz, next,
         ras_fit.samples.size(), ras_fit.rms * field,
         ras_fit.worst * field, ras_fit.dropped);
    locked_scale = next;
    state = State::Slide;
    slide_samples = 0;
    // The drift already says which way to go, which saves a clock change.
    slide_direction = drift < 0.0 ? 1 : -1;
    scale = locked_scale * (1.0 + slide_direction * kSlideOffset);
    return Action::Retune;
}

// Moves the raster until samples land just ahead of the blanking they must beat.
RateLock::Action RateLock::update_slide(const Sender& sender) {
    double position = 0.0;
    clock::time_point at{};
    if (!sender.raster_sample(position, at) || at == ras_last) return Action::None;
    ras_last = at;
    // The last ACK can be for a duplicate, sent on the raster's schedule rather than
    // the sample's: carry it forward to this send.
    position += seconds(clock::now() - at) * 1000.0 / sender.field_ms();

    const double want = sender.release_fraction() - kLandMarginSeconds * 1000.0 / sender.field_ms();
    const double error = wrap(position - std::floor(position) - want);
    slide_error = slide_samples == 0 ? error : slide_error + 0.25 * wrap(error - slide_error);
    ++slide_samples;
    if (slide_samples < kSlideSettle) return Action::None;

    if (slide_direction == 0) {
        if (std::abs(slide_error) > kSlideStart) {
            // A faster raster is further along when a sample arrives.
            slide_direction = slide_error < 0.0 ? 1 : -1;
            scale = locked_scale * (1.0 + slide_direction * kSlideOffset);
            return Action::Retune;
        }
    } else if (slide_error * slide_direction < -kSlideDone) {
        return Action::None;
    } else {
        scale = locked_scale;
    }
    const Action action = slide_direction == 0 ? Action::None : Action::Retune;
    state = State::Locked;
    reset_windows(clock::now());
    return action;
}

RateLock::Action RateLock::update_trim(const Sender& sender) {
    double position = 0.0;
    clock::time_point at{};
    if (!sender.raster_sample(position, at)) return Action::None;
    const auto now = clock::now();
    position += seconds(now - at) * 1000.0 / sender.field_ms();
    const double want = sender.release_fraction() - kTrimLandMarginSeconds * 1000.0 / sender.field_ms();
    if (trim_error_count < trim_errors.size()) {
        trim_errors[trim_error_count++] = wrap(position - std::floor(position) - want);
    }
    if (now - trim_since < kTrimInterval) return Action::None;
    const double elapsed = seconds(now - trim_since);
    trim_since = now;
    const auto mid = trim_errors.begin() + static_cast<ptrdiff_t>(trim_error_count / 2);
    std::nth_element(trim_errors.begin(), mid, trim_errors.begin() + static_cast<ptrdiff_t>(trim_error_count));
    const double error = *mid; // positive: the raster is further along than wanted
    trim_error_count = 0;

    // A step has to last into a second reading: a burst of late ACKs can move just one.
    const bool jumped = trim_have_error && !trim_stepping && std::abs(wrap(error - trim_last_error)) > kTrimStep;
    if (jumped && trim_step_seen) {
        trim_stepping = true;
        note("Rate lock: capture timing stepped %.2f ms, moving the raster to match\n",
             wrap(error - trim_last_error) * sender.field_ms());
    }
    trim_step_seen = jumped && !trim_stepping;
    if (!trim_step_seen) trim_last_error = error;
    trim_have_error = true;
    if (std::abs(error) < kTrimStepDone) trim_stepping = false;

    const double next_sum = trim_stepping ? trim_sum : trim_sum - kTrimIntegral * error * elapsed;
    const double next = next_sum - (trim_stepping ? kTrimStepGain : kTrimGain) * error;
    // Hold the integral at the limit, or it would overshoot when the error clears.
    if (std::abs(next) <= kTrimLimit) trim_sum = next_sum;
    trim_ppm = std::clamp(next, -kTrimLimit, kTrimLimit);
    if (std::abs(trim_ppm - trim_sent) < kTrimResend) return Action::None;
    trim_sent = trim_ppm;
    return Action::Trim;
}

// Locked: is capture still running at the rate it was locked at?
RateLock::Action RateLock::check_capture_rate(double gap_ms, clock::time_point captured) {
    if (ref_fps <= 0.0 || gap_ms <= 0.0 || gap_count < 32) return Action::None;
    const double ref_ms = 1000.0 / ref_fps;
    const double median = median_gap_ms();
    if (std::abs(median / ref_ms - 1.0) > kIntervalChange) {
        note("Rate lock restart: capture interval %.2f ms, was %.2f ms\n", median, ref_ms);
        return Action::Restart;
    }
    if (!block_rate.valid) {
        block_rate.start(captured, ref_fps);
        return Action::None;
    }
    block_rate.add(captured);
    if (captured - block_rate.t0 < kRateBlock) return Action::None;
    double fps = 0.0;
    block_rate.valid = false;
    if (!block_rate.fit.slope(fps)) return Action::None;
    if (std::abs(fps / ref_fps - 1.0) > kRateChange) {
        note("Rate lock restart: capture %.4f fps, was %.4f fps\n", fps, ref_fps);
        return Action::Restart;
    }
    return Action::None;
}

RateLock::Action RateLock::after_send(const Sender& sender, clock::time_point captured) {
    if (state == State::Slide) return update_slide(sender);
    if (state != State::Locked) return Action::None;
    const auto now = clock::now();
    if (sender.field_ms() <= 0.0) return Action::None;

    double gap_ms = 0.0;
    if (have_arrival) {
        gap_ms = std::chrono::duration<double, std::milli>(captured - last_arrival).count();
        record_gap(gap_ms);
    }
    last_arrival = captured;
    have_arrival = true;
    if (const Action change = check_capture_rate(gap_ms, captured); change != Action::None) {
        return change;
    }
    return use_trim ? update_trim(sender) : update_clock(sender, captured, now);
}

void RateLock::record_gap(double gap_ms) {
    if (gap_ms <= 0.0) return;
    gaps[gap_next] = gap_ms;
    gap_next = (gap_next + 1) % gaps.size();
    gap_count = std::min(gap_count + 1, gaps.size());
}

// Stock core: holds the phase by correcting the clock when it has drifted too far.
RateLock::Action RateLock::update_clock(const Sender& sender, clock::time_point captured, clock::time_point now) {
    const bool warm = now - since >= kWarmup;
    const double frame_ms = sender.field_ms(); // blit period
    if (warm) {
        if (!cap_rate.valid) {
            const double seed_ms = gap_count >= 16 ? median_gap_ms() : frame_ms * plan.repeat;
            cap_rate.start(captured, 1000.0 / seed_ms);
        } else {
            cap_rate.add(captured);
        }
        cap_t1 = captured;
    }
    double position = 0.0;
    clock::time_point at{};
    const bool new_raster = sender.raster_sample(position, at) && at != ras_last;
    if (new_raster && ras_valid && position < ras_p1) {
        // Sender re-INITed: new frame epoch
        // and a new raster phase. Start this state's timing again.
        reset_windows(now);
        return Action::None;
    }
    if (new_raster) {
        ras_last = at;
        if (warm) {
            if (!ras_valid) {
                ras_p0 = position;
                ras_t0 = at;
                ras_valid = true;
                ras_fit.clear();
            }
            ras_p1 = position;
            ras_t1 = at;
            ras_fit.add(seconds(at - ras_t0), position - ras_p0);
        }
    }

    // Phase of the raster when this sample arrived.
    if (!warm || !new_raster) return Action::None;
    const double ack_after_ms = std::chrono::duration<double, std::milli>(at - captured).count();
    const double phase = wrap(position - ack_after_ms / frame_ms);
    if (!have_target) {
        constexpr double kTau = 6.283185307179586;
        target_x += std::cos(phase * kTau);
        target_y += std::sin(phase * kTau);
        if (++target_n >= 60) {
            target = std::atan2(target_y, target_x) / kTau;
            have_target = true;
        }
        return Action::None;
    }
    // Positive drift = fields arrive later in the raster frame. That
    // eats the ~1 ms between normal arrival and the repeat deadline,
    // so it re-locks much sooner than drift the other way, which
    // only adds latency.
    drift = 0.95 * drift + 0.05 * wrap(phase - target);
    if (now - step_at >= kStepCheck) {
        const double moved = drift - step_drift;
        const bool stepped = step_ready && std::abs(moved) > kStepFrames;
        step_drift = drift;
        step_at = now;
        step_ready = true;
        if (stepped) {
            note("Rate lock: capture timing stepped %.2f ms, sliding back\n", moved * frame_ms);
            state = State::Slide;
            slide_samples = 0;
            slide_direction = 0;
            return Action::None;
        }
    }
    if (drift < kDriftLate && drift > -kDriftLimit) return Action::None;
    const double held_s = seconds(now - since);
    if (held_s < kMinLockedSeconds) return Action::None;
    double capture_fps = 0.0, raster_fps = 0.0;
    if (rates(sender, capture_fps, raster_fps)) {
        char why[64];
        std::snprintf(why, sizeof(why), "drift %+.3f frame after %.0f s", drift, held_s);
        return correct_rate(sender, capture_fps, raster_fps, why);
    }
    return Action::None;
}

} // namespace groovy
